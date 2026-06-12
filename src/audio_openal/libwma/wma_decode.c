/*
 * wma_decode.c - implementation of the minimal standalone WMA decode API.
 *
 * Wraps the Rockbox libwma integer decoder (wmadeci.c). The decoder uses
 * file-static scratch/output buffers and static VLC tables, so only ONE
 * WmaDecoder instance may be active at a time (this matches the Rockbox
 * single-stream codec model). The game wiring decodes one WMA stream at a
 * time, which is fine.
 *
 * Output scaling: the decoder leaves PCM in s->frame_out[ch][] in Rockbox'
 * 29-bit sample format (wma.c declares DSP_SET_SAMPLE_DEPTH, 29). To convert
 * to signed 16-bit (15-bit full scale) the shift is therefore 29 - 15 = 14,
 * with a clamp to [-32768, 32767]. (An earlier >>9 left samples ~32x too hot
 * and clipped hard -> "overblown".)
 */
#include "rbglue.h"
#include "asf.h"
#include "wmadec.h"
#include "wma_decode.h"

/* frame_out is 29-bit (DSP_SET_SAMPLE_DEPTH 29 in Rockbox wma.c) -> S16 = >>14. */
#define WMA_OUTPUT_SHIFT 14

struct WmaDecoder {
    WMADecodeContext ctx;
    int channels;
    int initialised;
    /* Per-instance MDCT overlap buffer. The decoder otherwise points ctx.frame_out at a
     * shared file-static (frame_out_buf in wmadeci.c), so a second stream (or an SFX one-shot
     * decoding mid-music) would clobber this one's overlap. Giving each decoder its own makes
     * concurrent/persistent streams safe (decode calls are still serial, so the scratch arrays
     * coefsarray/stat* that ARE shared are fine). */
    fixed32 frame_out[MAX_CHANNELS][BLOCK_MAX_SIZE * 2] MEM_ALIGN_ATTR;
};

static inline short clip16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (short)v;
}

WmaDecoder *wma_open(int channels, int sampleRate, int blockAlign,
                     int avgBytesPerSec, const unsigned char *extraData,
                     int extraLen)
{
    WmaDecoder *d;
    asf_waveformatex_t wfx;

    if (channels < 1 || channels > MAX_CHANNELS || sampleRate <= 0 ||
        blockAlign <= 0)
        return NULL;

    d = (WmaDecoder *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;

    memset(&wfx, 0, sizeof(wfx));
    wfx.packet_size   = (uint32_t)blockAlign;
    wfx.audiostream   = 0;
    wfx.codec_id      = 0x161;                 /* WMAv2 */
    wfx.channels      = (uint16_t)channels;
    wfx.rate          = (uint32_t)sampleRate;
    wfx.bitrate       = (uint32_t)avgBytesPerSec * 8u;
    wfx.blockalign    = (uint16_t)blockAlign;
    wfx.bitspersample = 16;
    wfx.numpackets    = 0;

    /* Copy format-extension bytes (cbSize/extradata) if present. The
     * decoder reads use_exp_vlc / use_bit_reservoir / use_variable_block_len
     * flags out of these; xWMA streams legitimately have none. */
    if (extraData && extraLen > 0) {
        int n = extraLen;
        if (n > (int)sizeof(wfx.data))
            n = (int)sizeof(wfx.data);
        memcpy(wfx.data, extraData, (size_t)n);
        wfx.datalen = (uint16_t)n;
    } else {
        wfx.datalen = 0;
    }

    if (wma_decode_init(&d->ctx, &wfx) < 0) {
        free(d);
        return NULL;
    }

    /* Repoint overlap state at our per-instance buffer (see struct comment). */
    memset(d->frame_out, 0, sizeof(d->frame_out));
    d->ctx.frame_out = &d->frame_out;

    d->channels    = channels;
    d->initialised = 1;
    return d;
}

int wma_decode_packet(WmaDecoder *d, const unsigned char *packet,
                      int packetLen, short *out, int outCapSamples)
{
    int res, written = 0;
    int ch, i, nch, frame_len;
    WMADecodeContext *s;

    if (!d || !d->initialised || !packet || packetLen <= 0 || !out)
        return -1;

    s   = &d->ctx;
    nch = d->channels;

    /* Prime the superframe. Returns 0 if this packet carries no frames
     * (e.g. the very first packet that only fills the bit reservoir). */
    res = wma_decode_superframe_init(s, packet, packetLen);
    if (res == 0)
        return 0;

    /* Pull every frame in the superframe. wma_decode_superframe_frame()
     * returns the per-channel sample count (s->frame_len) on success, or
     * -1 on a decode error. nb_frames is set by the init call above. */
    {
        int nb = s->nb_frames;
        int f;
        for (f = 0; f < nb; ++f) {
            frame_len = wma_decode_superframe_frame(s, packet, packetLen);
            if (frame_len < 0)
                return -1;

            /* Interleave + scale frame_out[ch][0..frame_len) to S16. */
            if (written + frame_len * nch > outCapSamples)
                frame_len = (outCapSamples - written) / nch;
            if (frame_len <= 0)
                break;

            for (i = 0; i < frame_len; ++i) {
                for (ch = 0; ch < nch; ++ch) {
                    int32_t v = (*s->frame_out)[ch][i] >> WMA_OUTPUT_SHIFT;
                    out[written++] = clip16(v);
                }
            }
        }
    }

    return written;
}

void wma_close(WmaDecoder *d)
{
    if (d)
        free(d);
}
