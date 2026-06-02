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

#endif // KISAK_WINDOWS_H
