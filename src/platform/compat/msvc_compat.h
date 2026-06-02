// msvc_compat.h — MSVC/Hex-Rays keyword compatibility for GCC/Clang builds.
//
// The decompiled sources use a handful of MSVC builtins that GCC/Clang don't
// provide as keywords. This header supplies them on non-MSVC. It is force-included
// (compiler `-include`) ahead of every translation unit on non-Windows so it
// applies before q_shared.h (which already maps __int64/_DWORD/etc. for __GNUC__).
//
// Note: __cdecl/__stdcall/__thiscall ARE recognised by GCC/Clang as calling-
// convention attributes on 32-bit x86 (the target here), so they need no shim.
#ifndef KISAK_MSVC_COMPAT_H
#define KISAK_MSVC_COMPAT_H

#include "msvc_intrin.h"  // _Interlocked*/MemoryBarrier -> GCC builtins

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

// MSVC __declspec(...) -> GCC/Clang. GCC does not parse `__declspec(align(N))`
// natively (the decompiled code uses it on hundreds of structs). Map it via token
// paste: __declspec(align(8)) -> __KISAK_DS_align(8) -> __attribute__((aligned(8))).
// Coexists with -fms-extensions (verified).
#define __declspec(x)          __KISAK_DS_##x
#define __KISAK_DS_align(n)    __attribute__((aligned(n)))
#define __KISAK_DS_noinline    __attribute__((noinline))
#define __KISAK_DS_noreturn    __attribute__((noreturn))
#define __KISAK_DS_nothrow     __attribute__((nothrow))
#define __KISAK_DS_deprecated  __attribute__((deprecated))
#define __KISAK_DS_selectany   __attribute__((weak))
#define __KISAK_DS_naked
#define __KISAK_DS_dllimport
#define __KISAK_DS_dllexport
#define __KISAK_DS_import
#define __KISAK_DS_export

#endif // !_MSC_VER
#endif // KISAK_MSVC_COMPAT_H
