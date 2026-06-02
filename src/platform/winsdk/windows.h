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

// NOTE: deliberately NOT defining max/min macros here. MSVC's <windows.h> does
// (unless NOMINMAX), but those 2-arg macros poison the C++ standard library
// (std::max, numeric_limits::max, iterator traits). The handful of bare max()/min()
// call sites in the decompiled renderer are spelled std::max/std::min instead.

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

// ---- Dynamic library loading -----------------------------------------------
// The engine LoadLibraryA's a few Windows-only DLLs (ddraw.dll, ...) and degrades
// gracefully when they are missing. Returning null here takes that fallback path —
// the matching POSIX route would be dlopen, but none of these DLLs exist on Linux.
static inline HMODULE LoadLibraryA(const char *) { return nullptr; }
static inline void   *GetProcAddress(HMODULE, const char *) { return nullptr; }
static inline BOOL    FreeLibrary(HMODULE) { return TRUE; }

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

// ---- High-resolution timer (QPC -> CLOCK_MONOTONIC) ------------------------
// (LARGE_INTEGER lives in _kisak_wintypes.h — also needed by d3d9 headers.)
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *c) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (c) c->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec; return TRUE;
}
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) {
    if (f) f->QuadPart = 1000000000LL; return TRUE;   // QPC counts nanoseconds
}

// ---- Async-overlap / SEH placeholders --------------------------------------
// OVERLAPPED is an opaque member of the async file-load state (db_file_load.h);
// the real async I/O runs through the platform layer. _EXCEPTION_POINTERS only
// needs to name a type for the unhandled-filter prototype (win_main.h); the body
// lives in the Windows-only win32/ source.
typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    DWORD Offset, OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
struct _EXCEPTION_POINTERS;

// ---- Mutex / wait (pthread-backed; HANDLE carries a pthread_mutex_t) --------
#define WAIT_OBJECT_0 0x0
#define INFINITE      0xFFFFFFFF
static inline HANDLE CreateMutexA(void *, BOOL initialOwner, const char *) {
    pthread_mutex_t *m = new pthread_mutex_t; pthread_mutex_init(m, nullptr);
    if (initialOwner) pthread_mutex_lock(m); return (HANDLE)m;
}
static inline DWORD WaitForSingleObject(HANDLE h, DWORD /*ms*/) {
    if (h && h != INVALID_HANDLE_VALUE) pthread_mutex_lock((pthread_mutex_t *)h); return WAIT_OBJECT_0;
}
static inline BOOL ReleaseMutex(HANDLE h) {
    if (h && h != INVALID_HANDLE_VALUE) pthread_mutex_unlock((pthread_mutex_t *)h); return TRUE;
}

// ---- Path / directory / file-time helpers ----------------------------------
static inline BOOL RemoveDirectoryA(const char *path) { return rmdir(path) == 0; }
// GetFullPathNameA normalizes lexically (the target need not exist) -> getcwd join.
static inline DWORD GetFullPathNameA(const char *name, DWORD len, char *buf, char **filePart) {
    char abs[MAX_PATH * 4];
    if (name[0] == '/') { abs[0] = '\0'; strncat(abs, name, sizeof(abs) - 1); }
    else { if (!getcwd(abs, sizeof(abs))) return 0; size_t n = strlen(abs);
           if (n && abs[n - 1] != '/') { abs[n++] = '/'; abs[n] = '\0'; } strncat(abs, name, sizeof(abs) - n - 1); }
    DWORD wrote = (DWORD)strlen(abs);
    if (buf && len) { strncpy(buf, abs, len - 1); buf[len - 1] = '\0';
        if (filePart) { char *s = strrchr(buf, '/'); *filePart = s ? s + 1 : buf; } }
    return wrote;
}
static inline LONG CompareFileTime(const FILETIME *a, const FILETIME *b) {
    unsigned long long x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
    unsigned long long y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : (x > y ? 1 : 0);
}

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
