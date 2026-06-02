// XAudio2.h — minimal portable surface for non-Windows builds.
//
// The decompiled sound system (snd_driver_xaudio2.h) is built directly on XAudio2:
// its SoundState embeds XAudio2 interfaces and detail structs. This header declares
// just enough — the by-value structs, the WAVEFORMATEX it nests, and the voice
// callback interface used as a base class — for the sound headers to PARSE so the
// many files that include them transitively compile. The XAudio2 *implementation*
// (snd*.cpp calling these interfaces) is the deferred audio port to SDL/OpenAL; the
// interfaces below are therefore forward-declared (incomplete), not fully defined.
#ifndef KISAK_XAUDIO2_H
#define KISAK_XAUDIO2_H

#include "windows.h"

// ---- Waveform format (also the base of WAVEFORMATEXTENSIBLE) ----------------
#ifndef _WAVEFORMATEX_
#define _WAVEFORMATEX_
typedef struct tWAVEFORMATEX {
    WORD  wFormatTag, nChannels;
    DWORD nSamplesPerSec, nAvgBytesPerSec;
    WORD  nBlockAlign, wBitsPerSample, cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;
typedef struct {
    WAVEFORMATEX Format;
    union { WORD wValidBitsPerSample, wSamplesPerBlock, wReserved; } Samples;
    DWORD dwChannelMask;
    GUID  SubFormat;
} WAVEFORMATEXTENSIBLE;
#endif

// ---- ADPCM wave format (the sound header redeclares its own tag) ------------
typedef struct { short iCoef1, iCoef2; } ADPCMCOEFSET;
typedef struct {
    WAVEFORMATEX wfx;
    WORD         wSamplesPerBlock, wNumCoef;
    ADPCMCOEFSET aCoef[1];
} ADPCMWAVEFORMAT;

// ---- XAudio2 interfaces (incomplete: pointers only on the Linux build) ------
struct IXAudio2;
struct IXAudio2Voice;
struct IXAudio2SourceVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2MasteringVoice;

typedef UINT32 XAUDIO2_WINDOWS_PROCESSOR_SPECIFIER;

// ---- Voice callback (used as a base class -> needs a full definition) -------
struct IXAudio2VoiceCallback {
    virtual void WINAPI OnVoiceProcessingPassStart(UINT32 BytesRequired) = 0;
    virtual void WINAPI OnVoiceProcessingPassEnd() = 0;
    virtual void WINAPI OnStreamEnd() = 0;
    virtual void WINAPI OnBufferStart(void *pBufferContext) = 0;
    virtual void WINAPI OnBufferEnd(void *pBufferContext) = 0;
    virtual void WINAPI OnLoopEnd(void *pBufferContext) = 0;
    virtual void WINAPI OnVoiceError(void *pBufferContext, HRESULT Error) = 0;
};

// ---- By-value structs the sound state embeds --------------------------------
typedef struct XAUDIO2_BUFFER {
    UINT32      Flags, AudioBytes;
    const BYTE *pAudioData;
    UINT32      PlayBegin, PlayLength, LoopBegin, LoopLength, LoopCount;
    void       *pContext;
} XAUDIO2_BUFFER;

typedef struct XAUDIO2_DEVICE_DETAILS {
    WCHAR  DeviceID[256];
    WCHAR  DisplayName[256];
    int    Role;
    WAVEFORMATEXTENSIBLE OutputFormat;
} XAUDIO2_DEVICE_DETAILS;

typedef struct XAUDIO2_VOICE_DETAILS {
    UINT32 CreationFlags, ActiveFlags, InputChannels, InputSampleRate;
} XAUDIO2_VOICE_DETAILS;

typedef struct XAUDIO2_PERFORMANCE_DATA {
    UINT64 AudioCyclesSinceLastQuery, TotalCyclesSinceLastQuery;
    UINT32 MinimumCyclesPerQuantum, MaximumCyclesPerQuantum;
    UINT32 MemoryUsageInBytes, CurrentLatencyInSamples, GlitchesSinceEngineStarted;
    UINT32 ActiveSourceVoiceCount, TotalSourceVoiceCount;
    UINT32 ActiveSubmixVoiceCount, ActiveResamplerCount, ActiveMatrixMixCount;
    UINT32 ActiveXmaSourceVoices, ActiveXmaStreams;
} XAUDIO2_PERFORMANCE_DATA;

#endif // KISAK_XAUDIO2_H
