// msvc_compat.h — MSVC/Hex-Rays keyword compatibility for GCC/Clang builds.
//
// The decompiled sources use a handful of MSVC builtins that GCC/Clang don't
// provide as keywords. This header supplies them on non-MSVC. It is force-included
// (compiler `-include`) ahead of every translation unit on non-Windows so it
// applies before q_shared.h (which already maps __int64/_DWORD/etc. for __GNUC__).
//
// Note: __cdecl/__stdcall/__thiscall ARE recognised by GCC/Clang as calling-
// convention attributes on 32-bit x86 (the target here), so they need no shim.
// __declspec is enabled via the -fdeclspec compiler flag, not a macro (macroing it
// would clash with the keyword).
#ifndef KISAK_MSVC_COMPAT_H
#define KISAK_MSVC_COMPAT_H

#if !defined(_MSC_VER)

#ifndef __debugbreak
#define __debugbreak() __builtin_trap()
#endif

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

// Sized integer keywords (also defined identically in q_shared.h under __GNUC__;
// benign redefinition). Lets standalone headers like ui/keycodes.h — which use
// `enum X : __int32` — compile without pulling in q_shared.h.
#ifndef __int8
#define __int8  char
#endif
#ifndef __int16
#define __int16 short
#endif
#ifndef __int32
#define __int32 int
#endif
#ifndef __int64
#define __int64 long long
#endif

#endif // !_MSC_VER
#endif // KISAK_MSVC_COMPAT_H
