// dsound.h — minimal portable DirectSound surface for non-Windows builds.
//
// The cinematic player creates a DirectSound device and hands it to Bink for audio
// (r_cinematic.cpp). Only the slice it touches is declared: IDirectSound8 with
// SetCooperativeLevel, plus DirectSoundCreate8. On Linux DirectSoundCreate8 fails
// (returns a non-zero HRESULT and a null device) — the cinematic audio path will be
// rehosted on the portable audio backend alongside the XAudio2 work.
#ifndef KISAK_DSOUND_H
#define KISAK_DSOUND_H

#include "windows.h"

struct IDirectSound8 : public IUnknown {
    virtual HRESULT WINAPI SetCooperativeLevel(HWND hwnd, DWORD dwLevel) = 0;
};
typedef IDirectSound8 *LPDIRECTSOUND8;

static inline HRESULT DirectSoundCreate8(const GUID *, IDirectSound8 **ppDS8, void *) {
    if (ppDS8) *ppDS8 = nullptr;
    return (HRESULT)-1;  // DSERR_NODRIVER analogue — no DirectSound on this platform
}

#endif // KISAK_DSOUND_H
