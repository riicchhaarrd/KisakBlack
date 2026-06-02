// XAPOBase.h — minimal portable stand-in. XAPO (the XAudio2 DSP framework) is used
// only by the Windows-only DSP backend (snd_driver_xaudio2_dsp.h, behind _WIN32).
// snd_driver_xaudio2.h includes this ungated but references no XAPO types itself, so
// an empty surface is enough on Linux. Real XAPO support arrives with the audio port.
#ifndef KISAK_XAPOBASE_H
#define KISAK_XAPOBASE_H
#include "windows.h"
#endif
