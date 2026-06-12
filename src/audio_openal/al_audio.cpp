// al_audio.cpp — OpenAL implementation of the XAudio2 interfaces (see al_audio.h).
#include "al_audio.h"

#include <deque>
#include <vector>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "libwma/wma_decode.h"   // standalone Rockbox-derived WMAv2 decoder (xWMA -> PCM16)

namespace kisak_al {

// ---- OpenAL source/buffer pools --------------------------------------------------------------
// Sounds play at a high rate; gen/delete-ing an alSource+alBuffer per sound floods the
// sync-proxied main thread and churns GC (each is an Emscripten AL.* object). Reuse them via
// free-lists instead. (Audio lifecycle runs on the sound-mixer thread; the mutex is cheap
// insurance in case a call ever originates elsewhere.)
static std::vector<ALuint> g_srcPool, g_bufPool;
static std::mutex          g_poolMtx;

static ALuint PoolTakeSource() {
    { std::lock_guard<std::mutex> lk(g_poolMtx);
      if (!g_srcPool.empty()) { ALuint s = g_srcPool.back(); g_srcPool.pop_back(); return s; } }
    ALuint s = 0; alGenSources(1, &s); return s;
}
static void PoolReturnSource(ALuint s) {       // caller has already Stop'd + unqueued its buffers
    if (!s) return;
    alSourcei(s, AL_LOOPING, AL_FALSE);         // reset reusable state so the next voice is clean
    alSourcef(s, AL_GAIN, 1.0f);
    alSourcef(s, AL_PITCH, 1.0f);
    bool keep; { std::lock_guard<std::mutex> lk(g_poolMtx); keep = g_srcPool.size() < 64; if (keep) g_srcPool.push_back(s); }
    if (!keep) alDeleteSources(1, &s);
}
static ALuint PoolTakeBuffer() {
    { std::lock_guard<std::mutex> lk(g_poolMtx);
      if (!g_bufPool.empty()) { ALuint b = g_bufPool.back(); g_bufPool.pop_back(); return b; } }
    ALuint b = 0; alGenBuffers(1, &b); return b;
}
static void PoolReturnBuffer(ALuint b) {       // buffer must already be unqueued (detached)
    if (!b) return;
    bool keep; { std::lock_guard<std::mutex> lk(g_poolMtx); keep = g_bufPool.size() < 256; if (keep) g_bufPool.push_back(b); }
    if (!keep) alDeleteBuffers(1, &b);
}

#define KB_WAVE_FORMAT_ADPCM    0x0002   /* WAVE_FORMAT_ADPCM (Microsoft ADPCM) — looping ambients */
#define KB_WAVE_FORMAT_WMAUDIO2 0x0161   /* WAVE_FORMAT_WMAUDIO2 (xWMA) — most SFX/music/dialogue */

ALenum AlFormat(const WAVEFORMATEX *fmt) {
    if (!fmt || fmt->wFormatTag != WAVE_FORMAT_PCM) return 0;  // only raw PCM here
    if (fmt->nChannels == 1) return fmt->wBitsPerSample == 8 ? AL_FORMAT_MONO8   : AL_FORMAT_MONO16;
    if (fmt->nChannels == 2) return fmt->wBitsPerSample == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16;
    return 0;  // >2ch (e.g. multichannel output formats) are not source formats here
}

// ---- MS-ADPCM (WAVE_FORMAT_ADPCM) -> PCM16 decode -------------------------------------------
// CoD ships all sounds as 4-bit MS-ADPCM; native XAudio2 decodes it, our OpenAL shim must too.
// Standard Microsoft ADPCM: per nBlockAlign-byte block, a header (predictor index + adaptive
// delta + two seed samples per channel) then 4-bit nibbles, decoded with the 7 standard
// coefficient pairs + the adaptation table. Handles mono and (interleaved) stereo.
static const int kMsAdapt[16] = {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};
static const int kMsCoef1[7]  = {256,512,0,192,240,460,392};
static const int kMsCoef2[7]  = {0,-256,0,64,0,-208,-232};
static inline short kbClamp16(int v){ return v < -32768 ? -32768 : (v > 32767 ? 32767 : (short)v); }
static inline int   kbRd16(const unsigned char *p){ return (short)(p[0] | (p[1] << 8)); }

// Decode the whole ADPCM buffer (channels 1 or 2) into out (PCM16, interleaved). Returns true.
static bool MsAdpcmDecode(const unsigned char *in, int bytes, int blockAlign, int ch,
                          std::vector<short> &out) {
    if (blockAlign < (ch == 2 ? 14 : 7) || ch < 1 || ch > 2) return false;
    out.clear();
    for (int off = 0; off + blockAlign <= bytes; off += blockAlign) {
        const unsigned char *b = in + off;
        int pred[2], delta[2], s1[2], s2[2]; const unsigned char *p = b;
        for (int c = 0; c < ch; ++c) { pred[c] = *p++ % 7; }
        for (int c = 0; c < ch; ++c) { delta[c] = kbRd16(p); p += 2; }
        for (int c = 0; c < ch; ++c) { s1[c] = kbRd16(p); p += 2; }
        for (int c = 0; c < ch; ++c) { s2[c] = kbRd16(p); p += 2; }
        for (int c = 0; c < ch; ++c) out.push_back((short)s2[c]);   // two seed samples come first
        for (int c = 0; c < ch; ++c) out.push_back((short)s1[c]);
        const unsigned char *nib = b + (ch == 2 ? 14 : 7);
        int nibCount = (blockAlign - (ch == 2 ? 14 : 7)) * 2;       // samples-per-channel for stereo
        for (int i = 0; i < nibCount; ++i) {
            int c = (ch == 2) ? (i & 1) : 0;                        // stereo: nibbles alternate L,R
            unsigned char byte = nib[i >> 1];
            int nb = (i & 1) ? (byte & 0xF) : (byte >> 4);
            int sn = nb >= 8 ? nb - 16 : nb;
            int predict = (s1[c] * kMsCoef1[pred[c]] + s2[c] * kMsCoef2[pred[c]]) >> 8;
            int samp = kbClamp16(predict + sn * delta[c]);
            out.push_back((short)samp);
            s2[c] = s1[c]; s1[c] = samp;
            delta[c] = (kMsAdapt[nb] * delta[c]) >> 8; if (delta[c] < 16) delta[c] = 16;
        }
    }
    return !out.empty();
}

// ---- ALSourceVoice ---------------------------------------------------------
// Per-voice queue of the pContext values for buffers still in flight (kept here,
// out of the header, so <deque> doesn't leak into it).
struct VoiceCtx { std::deque<void *> contexts; };

ALSourceVoice::ALSourceVoice(const WAVEFORMATEX *fmt, IXAudio2VoiceCallback *cb) : cb_(cb) {
    if (fmt) { format_ = AlFormat(fmt); rate_ = (ALsizei)fmt->nSamplesPerSec; channels_ = fmt->nChannels; origTag_ = fmt->wFormatTag; }
    // CoD ships sounds as 4-bit MS-ADPCM (tag 0x2); AlFormat rejects it. Decode to PCM16 on submit.
    if (fmt && !format_ && fmt->wFormatTag == KB_WAVE_FORMAT_ADPCM &&
        (fmt->nChannels == 1 || fmt->nChannels == 2) && fmt->nBlockAlign > 0) {
        isAdpcm_   = true;
        blockAlign_ = fmt->nBlockAlign;
        format_    = fmt->nChannels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;  // decoded output
    }
    // xWMA (WMAv2): most SFX/music/dialogue. AlFormat rejects it; decode to PCM16 on submit.
    if (fmt && !format_ && fmt->wFormatTag == KB_WAVE_FORMAT_WMAUDIO2 &&
        (fmt->nChannels == 1 || fmt->nChannels == 2) && fmt->nBlockAlign > 0) {
        isWma_       = true;
        blockAlign_  = fmt->nBlockAlign;
        avgBytesPerSec_ = (int)fmt->nAvgBytesPerSec;
        format_      = fmt->nChannels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    }
    source_ = PoolTakeSource();         // reuse a pooled alSource instead of alGenSources per voice
    ctxQueue_ = new VoiceCtx();
    static unsigned g_voicePhase = 0;   // stagger Service() refresh across voices (spread sync load)
    svcTick_ = g_voicePhase++;
}

void ALSourceVoice::Service() {
    if (!source_) return;
    ALint processed = 0;
    alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint buf = 0;
        alSourceUnqueueBuffers(source_, 1, &buf);
        PoolReturnBuffer(buf);          // recycle instead of alDeleteBuffers (+ skip the 3 alGetBufferi
        samplesPlayed_ += 1;            // size queries — the engine only checks SamplesPlayed != 0)
        VoiceCtx *vc = (VoiceCtx *)ctxQueue_;
        void *context = nullptr;
        if (vc && !vc->contexts.empty()) { context = vc->contexts.front(); vc->contexts.pop_front(); }
        if (cb_) cb_->OnBufferEnd(context);
        if (cachedQueued_ > 0) --cachedQueued_;   // mirror the unqueue (keeps cachedQueued_ == AL_BUFFERS_QUEUED)
    }
}

void WINAPI ALSourceVoice::GetVoiceDetails(XAUDIO2_VOICE_DETAILS *d) {
    if (!d) return; d->CreationFlags = 0; d->ActiveFlags = 0;
    d->InputChannels = channels_; d->InputSampleRate = (UINT32)rate_;
}

HRESULT WINAPI ALSourceVoice::SetOutputMatrix(IXAudio2Voice *, UINT32 srcCh, UINT32 dstCh, const float *matrix, UINT32) {
    if (source_ && matrix) {                       // approximate the level matrix as an overall gain
        float g = 0.0f; UINT32 n = srcCh * dstCh; if (!n) n = 1;
        for (UINT32 i = 0; i < n; ++i) { float a = matrix[i] < 0 ? -matrix[i] : matrix[i]; if (a > g) g = a; }
        // The engine re-sends this EVERY frame per voice; alSourcef is sync-proxied to the busy main
        // thread. Skip the round-trip when the gain hasn't actually changed (static ambients/music
        // never change; 3D sounds only change when moving). This is the bulk of the audio FPS tax.
        if (g != lastGain_) { lastGain_ = g; alSourcef(source_, AL_GAIN, g); }
    }
    return S_OK;
}

void WINAPI ALSourceVoice::GetState(XAUDIO2_VOICE_STATE *s) {
    // OpenAL is sync-proxied to the (render-busy) main thread, so per-voice per-frame alGetSourcei
    // round-trips were the dominant audio cost (~halves FPS vs ?nosnd). We keep a LOCAL mirror of
    // the queued count (cachedQueued_: ++ on submit, -- on recycle), so GetState needs no query at
    // all; the only remaining sync call is Service()'s AL_BUFFERS_PROCESSED, throttled to ~every 4th
    // call and staggered across voices (svcTick_). Worst-case staleness is a few frames of delayed
    // finish-detection -- safe (a buffer just reads "still queued" slightly longer, never early).
    if ((svcTick_++ & 3) == 0) Service();
    if (!s) return;
    s->pCurrentBufferContext = lastContext_;
    s->BuffersQueued = (UINT32)(cachedQueued_ > 0 ? cachedQueued_ : 0);
    // SamplesPlayed must be NONZERO while the voice is live or the engine re-Start()s it every frame
    // (rewind blips). cachedQueued_>0 implies live (we auto-play on submit). Do NOT query
    // AL_SAMPLE_OFFSET — Emscripten sourceTell() throws on a queued buffer lacking .audioBuf.
    s->SamplesPlayed = samplesPlayed_ + (cachedQueued_ > 0 ? 1 : 0);
}

void WINAPI ALSourceVoice::DestroyVoice() {
    if (source_) { alSourceStop(source_);
        // Reclaim queued buffers to the pool. Query the REAL count here (not cachedQueued_, which
        // may lag Service's throttle) so the source is fully drained before it goes back in the pool.
        ALint q = 0; alGetSourcei(source_, AL_BUFFERS_QUEUED, &q);
        while (q-- > 0) { ALuint b = 0; alSourceUnqueueBuffers(source_, 1, &b); PoolReturnBuffer(b); }
        PoolReturnSource(source_); source_ = 0;   // recycle the source instead of alDeleteSources
    }
    if (wma_) { wma_close((WmaDecoder *)wma_); wma_ = nullptr; }
    delete (VoiceCtx *)ctxQueue_;
    delete this;
}

HRESULT WINAPI ALSourceVoice::Start(UINT32, UINT32) {
    // Resume only if not already playing — alSourcePlay on an AL_PLAYING source rewinds it to
    // the start, and the engine calls Start() repeatedly while a voice is live.
    if (source_) { ALint st = 0; alGetSourcei(source_, AL_SOURCE_STATE, &st);
                   if (st != AL_PLAYING) alSourcePlay(source_); }
    return S_OK;
}
HRESULT WINAPI ALSourceVoice::Stop(UINT32, UINT32)  { if (source_) alSourceStop(source_); return S_OK; }

HRESULT WINAPI ALSourceVoice::SubmitSourceBuffer(const XAUDIO2_BUFFER *buf, const XAUDIO2_BUFFER_WMA *) {
    Service();
    if (!buf || !source_) return S_OK;
    lastContext_ = buf->pContext;
    bool queued = false;
    if (format_ && buf->pAudioData && buf->AudioBytes) {
        ALuint b = PoolTakeBuffer();   // reuse a pooled alBuffer instead of alGenBuffers per submit
        if (isAdpcm_) {
            // Decode MS-ADPCM -> PCM16 (native XAudio2 does this in hardware; the shim must do it).
            std::vector<short> pcm;
            if (MsAdpcmDecode((const unsigned char *)buf->pAudioData, (int)buf->AudioBytes,
                              blockAlign_, (int)channels_, pcm) && !pcm.empty()) {
                alBufferData(b, format_, pcm.data(), (ALsizei)(pcm.size() * sizeof(short)), rate_);
                queued = true;
            }
        } else if (isWma_ && blockAlign_ > 0) {
            // xWMA: the whole sound is one buffer of packetCount*blockAlign bytes. Decode every
            // packet in sequence through ONE decoder (preserves inter-packet MDCT overlap), then
            // hand the PCM16 to OpenAL. (wma_* is single-instance; this open/decode/close is serial.)
            std::vector<short> pcm;
            // xWMA carries NO extradata (cbSize=0), so synthesize WMA2 flags exactly as FFmpeg's
            // xWMA demuxer does: 6 bytes with [4]=31 -> flags2=0x1F (exp_vlc|bit_reservoir|
            // variable_block_len + block-size bits). Without this the decoder misframes -> noise+OOB.
            static const unsigned char kXwmaExtradata[6] = { 0, 0, 0, 0, 31, 0 };
            // Persistent per-voice decoder: streaming voices (music/ambience) feed windows across
            // many submits, and the decoder's reservoir + MDCT overlap must carry between them.
            // One-shots just open it on their single submit; it's freed in DestroyVoice either way.
            if (!wma_)
                wma_ = wma_open((int)channels_, (int)rate_, blockAlign_, avgBytesPerSec_, kXwmaExtradata, 6);
            WmaDecoder *dec = (WmaDecoder *)wma_;
            if (dec) {
                const unsigned char *data = (const unsigned char *)buf->pAudioData;
                const int kPerPkt = 2048 * 2 * 8;   // frame_len(2048) * maxch(2) * superframe headroom
                for (int off = 0; off + blockAlign_ <= (int)buf->AudioBytes; off += blockAlign_) {
                    size_t base = pcm.size();
                    pcm.resize(base + kPerPkt);
                    int n = wma_decode_packet(dec, data + off, blockAlign_, pcm.data() + base, kPerPkt);
                    pcm.resize(base + (n > 0 ? (size_t)n : 0));
                }
            }
            if (!pcm.empty()) {
                alBufferData(b, format_, pcm.data(), (ALsizei)(pcm.size() * sizeof(short)), rate_);
                queued = true;
            }
        } else {
            alBufferData(b, format_, buf->pAudioData, (ALsizei)buf->AudioBytes, rate_);
            queued = true;
        }
        if (queued) {
            alSourceQueueBuffers(source_, 1, &b);
            ++cachedQueued_;   // mirror AL_BUFFERS_QUEUED locally so GetState needs no per-frame query
            ((VoiceCtx *)ctxQueue_)->contexts.push_back(buf->pContext);
            // Looping ambients (sprinkler, menu loops) submit ONE buffer with LoopCount>0 (255 =
            // infinite). Without AL_LOOPING the buffer plays once, the engine resubmits, and you
            // hear the head fragment retriggering -> a chiptune buzz. Loop it seamlessly instead.
            // Only set it for actual loops — a fresh source defaults to AL_FALSE, so one-shots (the
            // vast majority, played at high rate) skip this sync-proxied call entirely.
            if (buf->LoopCount > 0) alSourcei(source_, AL_LOOPING, AL_TRUE);
            // Keep playing if the source drained while we were between buffers.
            ALint state = 0; alGetSourcei(source_, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) alSourcePlay(source_);
        } else {
            PoolReturnBuffer(b);   // decode failed / empty -> recycle the unused buffer
        }
    }
    if (!queued) {
        // Couldn't queue (no format / empty / decode failed) — report completion so streaming progresses.
        if (cb_) cb_->OnBufferEnd(buf->pContext);
    }
    if (buf->Flags & XAUDIO2_END_OF_STREAM) { endOfStream_ = true; if (cb_) cb_->OnStreamEnd(); }
    return S_OK;
}

HRESULT WINAPI ALSourceVoice::SetFrequencyRatio(float ratio, UINT32) {
    // Re-sent every frame per voice (SD_UpdateVoice); alSourcef is sync-proxied to the main thread.
    // Skip the round-trip when the pitch is unchanged (only doppler/varispeed sounds change it).
    if (source_ && ratio > 0 && ratio != lastPitch_) { lastPitch_ = ratio; alSourcef(source_, AL_PITCH, ratio); }
    return S_OK;
}

// ---- Bus / submix / mastering voices ---------------------------------------
static void FillDetails(XAUDIO2_VOICE_DETAILS *d, UINT32 ch, UINT32 rate) {
    if (!d) return; d->CreationFlags = 0; d->ActiveFlags = 0; d->InputChannels = ch; d->InputSampleRate = rate;
}
void WINAPI ALBusVoice::GetVoiceDetails(XAUDIO2_VOICE_DETAILS *d)        { FillDetails(d, channels_, rate_); }
void WINAPI ALBusVoice::GetState(XAUDIO2_VOICE_STATE *s)                 { if (s) { s->pCurrentBufferContext = nullptr; s->BuffersQueued = 0; s->SamplesPlayed = 0; } }
void WINAPI ALSubmixVoice::GetVoiceDetails(XAUDIO2_VOICE_DETAILS *d)     { FillDetails(d, channels_, rate_); }
void WINAPI ALSubmixVoice::GetState(XAUDIO2_VOICE_STATE *s)             { if (s) { s->pCurrentBufferContext = nullptr; s->BuffersQueued = 0; s->SamplesPlayed = 0; } }
void WINAPI ALMasteringVoice::GetVoiceDetails(XAUDIO2_VOICE_DETAILS *d)  { FillDetails(d, channels_, rate_); }
void WINAPI ALMasteringVoice::GetState(XAUDIO2_VOICE_STATE *s)          { if (s) { s->pCurrentBufferContext = nullptr; s->BuffersQueued = 0; s->SamplesPlayed = 0; } }

// ---- ALXAudio2 -------------------------------------------------------------
ALXAudio2::ALXAudio2() {
    // KB_NOSND=1: skip device creation (headless harness -- Web Audio time never
    // advances without an output device, and the engine's sound update spins on it).
    const char *nosnd = getenv("KB_NOSND");
    if (nosnd && *nosnd == '1') { fprintf(stderr, "[al] KB_NOSND=1: audio disabled\n"); return; }
    dev_ = alcOpenDevice(nullptr);
    if (dev_) { ctx_ = alcCreateContext(dev_, nullptr); if (ctx_) alcMakeContextCurrent(ctx_); }
    if (!ctx_) fprintf(stderr, "[al] OpenAL device init FAILED (dev=%p ctx=%p)\n", (void*)dev_, (void*)ctx_);
}
ALXAudio2::~ALXAudio2() {
    if (ctx_) { alcMakeContextCurrent(nullptr); alcDestroyContext(ctx_); }
    if (dev_) alcCloseDevice(dev_);
}

HRESULT WINAPI ALXAudio2::CreateSourceVoice(IXAudio2SourceVoice **ppv, const WAVEFORMATEX *fmt, UINT32, float, IXAudio2VoiceCallback *cb, const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN *) {
    if (!ppv) return E_POINTER;
    *ppv = new ALSourceVoice(fmt, cb);   // effect chain/sends ignored (OpenAL mixes to output)
    return S_OK;
}
HRESULT WINAPI ALXAudio2::CreateSubmixVoice(IXAudio2SubmixVoice **ppv, UINT32 ch, UINT32 rate, UINT32, UINT32, const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN *) {
    if (!ppv) return E_POINTER;
    *ppv = new ALSubmixVoice(ch ? ch : 2, rate ? rate : 48000);
    return S_OK;
}
HRESULT WINAPI ALXAudio2::CreateMasteringVoice(IXAudio2MasteringVoice **ppv, UINT32 ch, UINT32 rate, UINT32, UINT32, const XAUDIO2_EFFECT_CHAIN *) {
    if (!ppv) return E_POINTER;
    *ppv = new ALMasteringVoice(ch ? ch : 2, rate ? rate : 48000);
    return S_OK;
}
HRESULT WINAPI ALXAudio2::GetDeviceDetails(UINT32 index, XAUDIO2_DEVICE_DETAILS *d) {
    if (!d || index != 0) return E_INVALIDARG;
    const char name[] = "OpenAL Default Device";
    for (int i = 0; name[i]; ++i) { d->DeviceID[i] = name[i]; d->DisplayName[i] = name[i]; d->DeviceID[i + 1] = 0; d->DisplayName[i + 1] = 0; }
    d->Role = GlobalDefaultDevice;
    WAVEFORMATEX &f = d->OutputFormat.Format;
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = 48000;
    f.wBitsPerSample = 16; f.nBlockAlign = 4; f.nAvgBytesPerSec = 48000 * 4; f.cbSize = 0;
    return S_OK;
}

} // namespace kisak_al

// ---- Entry point (matches the XAudio2Create declaration in XAudio2.h) -------
extern "C" HRESULT WINAPI XAudio2Create(IXAudio2 **ppXAudio2, unsigned int, XAUDIO2_WINDOWS_PROCESSOR_SPECIFIER) {
    if (!ppXAudio2) return E_POINTER;
    kisak_al::ALXAudio2 *o = new kisak_al::ALXAudio2();
    if (!o->Ok()) { o->Release(); *ppXAudio2 = nullptr; return E_FAIL; }
    *ppXAudio2 = o;
    return S_OK;
}
