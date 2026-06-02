// windows.h — minimal portable <windows.h> for non-Windows builds.
//
// Placed on the include path only on non-Windows, so the game's `#include
// <windows.h>` resolves here. Provides the base Win32 types the engine uses; grown
// as real translation units surface more of the surface. On Windows the real SDK
// header is used and this file is never seen.
#ifndef KISAK_WINDOWS_H
#define KISAK_WINDOWS_H

#include "_kisak_wintypes.h"

// Message-pump / windowing scalar types (the actual pump lives in the platform
// layer; these let renderer/headers that mention them compile).
typedef UINT_PTR  WPARAM;
typedef LONG_PTR  LPARAM;
typedef LONG_PTR  LRESULT;
typedef DWORD     COLORREF;
typedef WORD      ATOM;

KISAK_DECLARE_HANDLE(HMENU);
KISAK_DECLARE_HANDLE(HICON);
KISAK_DECLARE_HANDLE(HCURSOR);
KISAK_DECLARE_HANDLE(HBRUSH);
KISAK_DECLARE_HANDLE(HBITMAP);
KISAK_DECLARE_HANDLE(HGLRC);

#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | \
     ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#endif

// ---- Win32 thread / timing / atomic APIs the engine calls ------------------
#include "../compat/msvc_intrin.h"  // _Interlocked*/MemoryBarrier
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <ctime>

static inline DWORD GetCurrentThreadId()  { return (DWORD)(uintptr_t)pthread_self(); }
static inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
static inline void  Sleep(DWORD ms)       { if (ms) usleep((useconds_t)ms * 1000u); }
static inline BOOL  SwitchToThread()      { return sched_yield() == 0; }

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;
static inline void GetLocalTime(SYSTEMTIME *st) {
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    st->wYear = (WORD)(tmv.tm_year + 1900); st->wMonth = (WORD)(tmv.tm_mon + 1);
    st->wDayOfWeek = (WORD)tmv.tm_wday;      st->wDay = (WORD)tmv.tm_mday;
    st->wHour = (WORD)tmv.tm_hour; st->wMinute = (WORD)tmv.tm_min;
    st->wSecond = (WORD)tmv.tm_sec; st->wMilliseconds = 0;
}
static inline void GetSystemTime(SYSTEMTIME *st) { GetLocalTime(st); }

// MSVC CRT _itoa(value, buffer, radix) — minimal integer-to-string.
static inline char *_itoa(int value, char *str, int radix) {
    char tmp[35]; int i = 0;
    bool neg = (radix == 10 && value < 0);
    unsigned int v = neg ? (unsigned int)(-value) : (unsigned int)value;
    do { unsigned d = v % (unsigned)radix; tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)radix; } while (v);
    char *p = str;
    if (neg) *p++ = '-';
    while (i > 0) *p++ = tmp[--i];
    *p = '\0';
    return str;
}

// The non-underscore Interlocked* Win32 functions; macros (type-generic, like the
// _Interlocked* intrinsics) so they accept the engine's various pointer types.
#define InterlockedExchange(p, v)          __sync_lock_test_and_set((p), (v))
#define InterlockedExchangeAdd(p, v)       __sync_fetch_and_add((p), (v))
#define InterlockedIncrement(p)            __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p)            __sync_sub_and_fetch((p), 1)
#define InterlockedCompareExchange(p, e, c) __sync_val_compare_and_swap((p), (c), (e))

#endif // KISAK_WINDOWS_H
