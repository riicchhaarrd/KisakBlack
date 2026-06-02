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
#include <cerrno>

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

// ---- Misc Win32 ------------------------------------------------------------
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
static inline DWORD GetLastError() { return (DWORD)errno; }
static inline int   ShowCursor(BOOL) { return 0; }
static inline BOOL  SystemTimeToFileTime(const SYSTEMTIME *, FILETIME *ft) {
    if (ft) { ft->dwLowDateTime = 0; ft->dwHighDateTime = 0; } return TRUE;
}

// ---- Window message pump (stubbed; real input runs through SDL) -------------
typedef struct tagMSG {
    HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt;
} MSG, *LPMSG;
static inline BOOL    PeekMessageA(MSG *, HWND, UINT, UINT, UINT) { return FALSE; }
static inline BOOL    GetMessageA(MSG *, HWND, UINT, UINT) { return FALSE; }
static inline BOOL    TranslateMessage(const MSG *) { return FALSE; }
static inline LRESULT DispatchMessageA(const MSG *) { return 0; }

// ---- File enumeration / deletion (FindFirstFile* -> glob, DeleteFile -> unlink)
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

#include <glob.h>
#include <cstring>
struct KisakFindState { glob_t g; size_t i; };
static inline void KisakFindFill(KisakFindState *h, WIN32_FIND_DATAA *d) {
    const char *p = h->g.gl_pathv[h->i];
    const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
    int n = 0; while (base[n] && n < MAX_PATH - 1) { d->cFileName[n] = base[n]; ++n; }
    d->cFileName[n] = '\0'; d->dwFileAttributes = 0;
}
static inline HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *d) {
    KisakFindState *h = new KisakFindState(); h->i = 0;
    if (glob(pattern, 0, nullptr, &h->g) != 0 || h->g.gl_pathc == 0) { globfree(&h->g); delete h; return INVALID_HANDLE_VALUE; }
    KisakFindFill(h, d); return (HANDLE)h;
}
static inline BOOL FindNextFileA(HANDLE handle, WIN32_FIND_DATAA *d) {
    KisakFindState *h = (KisakFindState *)handle;
    if (++h->i >= h->g.gl_pathc) return FALSE;
    KisakFindFill(h, d); return TRUE;
}
static inline BOOL FindClose(HANDLE handle) {
    KisakFindState *h = (KisakFindState *)handle;
    if (h && h != INVALID_HANDLE_VALUE) { globfree(&h->g); delete h; } return TRUE;
}
static inline BOOL DeleteFileA(const char *path) { return unlink(path) == 0; }

#endif // KISAK_WINDOWS_H
