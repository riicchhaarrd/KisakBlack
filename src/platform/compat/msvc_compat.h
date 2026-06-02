// msvc_compat.h — MSVC/Hex-Rays keyword compatibility for GCC/Clang builds.
//
// The decompiled sources use a handful of MSVC builtins that GCC/Clang don't
// provide as keywords. This header supplies them on non-MSVC. It is force-included
// (compiler `-include`) ahead of every translation unit on non-Windows so it
// applies before q_shared.h (which already maps __int64/_DWORD/etc. for __GNUC__).
//
// Calling-convention keywords: GCC/Clang do NOT accept the bare __cdecl /
// __stdcall / __fastcall / __thiscall on a non-Windows target (they parse as
// identifiers -> "expected initializer"). The build is 32-bit x86 and internally
// self-consistent (no external symbol relies on a specific convention), so map
// them all to nothing (see below).
#ifndef KISAK_MSVC_COMPAT_H
#define KISAK_MSVC_COMPAT_H

#include "msvc_intrin.h"  // _Interlocked*/MemoryBarrier -> GCC builtins

#if !defined(_MSC_VER)

// MSVC makes the C string/float-limits functions broadly visible; the decompiled
// code uses memset/memcpy/FLT_MAX/etc. without always including the header. Pull
// them in build-wide (this header is force-included) to match. (Not <cstdlib> —
// its POSIX random()/etc. clash with the engine's own declarations.)
#include <cstring>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <csetjmp>
#include <strings.h>

#ifndef __debugbreak
#define __debugbreak() __builtin_trap()
#endif

// More MSVC-isms: another calling convention, and the _-prefixed CRT aliases.
#ifndef __pascal
#define __pascal
#endif
#ifndef _snprintf
#define _snprintf  snprintf
#define _vsnprintf vsnprintf
#define _stricmp   strcasecmp
#define _strnicmp  strncasecmp
#define _strdup    strdup
#endif
typedef FILE _iobuf;  // MSVC's FILE struct tag, used bare in the decompiled code

// MSVC's 64-bit time CRT (rb_logfile.cpp) and the SEH-era setjmp spelling
// (rb_backend.cpp). __time64_t is just a 64-bit time_t; map onto the POSIX CRT.
typedef long long __time64_t;
static inline __time64_t _time64(__time64_t *t) { time_t r = time(nullptr); if (t) *t = (__time64_t)r; return (__time64_t)r; }
static inline struct tm *_localtime64(const __time64_t *t) { time_t v = (time_t)*t; return localtime(&v); }
#ifndef _setjmp
#define _setjmp setjmp   // <csetjmp> provides jmp_buf + setjmp; MSVC spells it _setjmp
#endif

// MSVC's qsort/bsearch comparator typedef (used in casts, e.g. rb_imagetouch.cpp).
typedef int (*_CoreCrtNonSecureSearchSortCompareFunction)(const void *, const void *);

#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

// Calling conventions -> nothing (see header note above).
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef __thiscall
#define __thiscall
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
#define _declspec(x)           __KISAK_DS_##x   // deprecated single-underscore spelling (r_stream.h)
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
