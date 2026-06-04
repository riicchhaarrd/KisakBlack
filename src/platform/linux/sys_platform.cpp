// sys_platform.cpp — Linux implementations of the Sys_*/NET_*/IN_* platform layer
// that the engine calls (declared in src/win32/*.h, whose .cpp impls are Windows-only
// and excluded from the Linux build). Load-critical paths (critical sections,
// filesystem, the event queue, timing, error/exit) are implemented for real; the
// non-essential Windows extras (debug sockets, splash/console, hotkeys) are no-ops.
#include <win32/win_main.h>
#include <win32/win_common.h>
#include <universal/dvar.h>   // _Dvar_RegisterBool (sys_SSE registration)
#include <win32/win_shared.h>
#include <win32/win_net.h>
#include <win32/win_input.h>
#include <win32/win_wndproc.h>

#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstring>

#include "../sdl/sdl_events.h"   // Sys_PumpSDLEvents
#include <SDL2/SDL.h>            // SDL_GetMouseState / relative-mouse mode (IN_Frame)
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/em_asm.h>   // MAIN_THREAD_ASYNC_EM_ASM (pointer lock + OS cursor)
// Mouse sampled from the shared input block filled by the DOM-thread JS listeners
// (src/platform/sdl/sdl_events.cpp): position in engine px + accumulated raw movement.
extern "C" void WebInput_GetMouse(int *x, int *y, int *dx, int *dy);
#endif

// Client mouse entry point (src/client_mp/cl_input_mp.cpp). Declared directly to
// avoid pulling the whole client header into the platform layer; __cdecl is the
// default on x86-32 so this resolves to the same symbol.
int CL_MouseEvent(int x, int y, int dx, int dy);

// ---- The window-vars global (normally in win_wndproc.cpp) ------------------
WinVars_t g_wv;

// ---- Critical sections: 75 recursive pthread mutexes -----------------------
namespace {
pthread_mutex_t g_critSects[CRITSECT_COUNT];
bool g_critInit = false;
void EnsureCritInit() {
    if (g_critInit) return; g_critInit = true;
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    for (int i = 0; i < CRITSECT_COUNT; ++i) pthread_mutex_init(&g_critSects[i], &a);
    pthread_mutexattr_destroy(&a);
}
}
void Sys_InitializeCriticalSections() { EnsureCritInit(); }
void Sys_EnterCriticalSection(CriticalSection s) { EnsureCritInit(); if ((int)s < CRITSECT_COUNT) pthread_mutex_lock(&g_critSects[s]); }
void Sys_LeaveCriticalSection(CriticalSection s) { if ((int)s < CRITSECT_COUNT) pthread_mutex_unlock(&g_critSects[s]); }
bool Sys_TryEnterCriticalSection(CriticalSection s) { EnsureCritInit(); return (int)s < CRITSECT_COUNT && pthread_mutex_trylock(&g_critSects[s]) == 0; }

// ---- Timing ----------------------------------------------------------------
unsigned int Sys_MillisecondsRaw() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}
void Sys_SnapVector(float *v) { if (v) { v[0] = floorf(v[0] + 0.5f); v[1] = floorf(v[1] + 0.5f); v[2] = floorf(v[2] + 0.5f); } }

// ---- Filesystem ------------------------------------------------------------
static char g_cwd[4096];
char *Sys_Cwd() { if (!getcwd(g_cwd, sizeof(g_cwd))) g_cwd[0] = '\0'; return g_cwd; }
void  Sys_Mkdir(const char *path) { if (path) mkdir(path, 0755); }
const char *Sys_DefaultCDPath() { return ""; }
char *Sys_DefaultInstallPath() { return Sys_Cwd(); }
int Sys_DirectoryHasContents(const char *dir) {
    if (!dir) return 0;
    char path[4096]; int i = 0; for (; dir[i] && i < 4095; ++i) path[i] = dir[i] == '\\' ? '/' : dir[i]; path[i] = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    int has = 0; struct dirent *e;
    while ((e = readdir(d))) { if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { has = 1; break; } }
    closedir(d); return has;
}

// ---- Error / exit / print --------------------------------------------------
void Sys_Print(char *msg) {
    if (!msg) return;
#if defined(__EMSCRIPTEN__)
    // The decompiled engine emits long runs of identical non-fatal asserts/warnings
    // in-game (e.g. Vec4IsNormalized, gjk_ray_cast). Each console line is proxied to the
    // DOM thread, so the spam alone can dominate the frame. Collapse consecutive exact
    // duplicates per thread, emitting a "(x N)" summary when the run ends.
    static thread_local char last[512] = {0};
    static thread_local int  dupCount  = 0;
    if (std::strncmp(msg, last, sizeof(last) - 1) == 0 && last[0]) {
        ++dupCount;
        return;
    }
    if (dupCount > 0) {
        char note[48];
        int n = snprintf(note, sizeof(note), "   ^ (repeated x%d)\n", dupCount + 1);
        fwrite(note, 1, (size_t)n, stdout);
        dupCount = 0;
    }
    std::strncpy(last, msg, sizeof(last) - 1); last[sizeof(last) - 1] = 0;
#endif
    fputs(msg, stdout);
    fflush(stdout);
}
void Sys_Error(char *error, ...) {
    char buf[4096]; va_list ap; va_start(ap, error); vsnprintf(buf, sizeof(buf), error, ap); va_end(ap);
    fprintf(stderr, "\n********** Sys_Error **********\n%s\n", buf); fflush(stderr);
    _exit(1);
}
void Sys_Quit()        { _exit(0); }
void Sys_NormalExit()  {}
void Sys_QuitAndStartProcess(const char *) { _exit(0); }
void Sys_OutOfMemErrorInternal(const char *file, int line) { fprintf(stderr, "out of memory (%s:%d)\n", file ? file : "?", line); _exit(1); }
void Sys_DirectXFatalError() { fprintf(stderr, "graphics init failed\n"); _exit(1); }

// ---- System info -----------------------------------------------------------
const dvar_t *sys_SSE = nullptr;   // CPU-SSE dvar (engine builds with -msse)
void Sys_Init() {
    EnsureCritInit();
    // Windows registers the system-info dvars in Sys_RegisterInfoDvars (win_main.cpp),
    // which is not compiled on Linux. The engine dereferences sys_SSE without a null
    // check (e.g. R_SkinXModelCmd's SSE-skinning gate), so register it here. We always
    // build with SSE/SSE2, so report it available. Com_InitDvars() has already run.
    sys_SSE = _Dvar_RegisterBool("sys_SSE", true, 0x40u,
                                 "Operating system allows Streaming SIMD Extensions");
}
void Sys_GetInfo(SysInfo *info) { if (info) memset(info, 0, sizeof(*info)); }
bool Sys_HasConfigureChecksumChanged(int) { return false; }
bool Sys_HasInfoChanged() { return false; }
void Sys_ArchiveInfo(int) {}
bool Sys_IsMiniDumpStarted() { return false; }

// ---- Event queue: SDL-fed ring buffer --------------------------------------
namespace {
sysEvent_t g_eventQue[256];
int g_eventHead = 0, g_eventTail = 0;
}
void Sys_QueEvent(unsigned int time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr) {
    sysEvent_t *ev = &g_eventQue[g_eventHead & 255];
    if (g_eventHead - g_eventTail >= 256) { if (ev->evPtr) free(ev->evPtr); ++g_eventTail; }
    ++g_eventHead;
    ev->evTime = time ? time : Sys_Milliseconds();
    ev->evType = type; ev->evValue = value; ev->evValue2 = value2;
    ev->evPtrLength = ptrLength; ev->evPtr = ptr;
}
sysEvent_t *Sys_GetEvent(sysEvent_t *result) {
    if (g_eventHead == g_eventTail) Sys_PumpSDLEvents(Sys_Milliseconds());   // pull from SDL
    if (g_eventHead > g_eventTail) { *result = g_eventQue[g_eventTail & 255]; ++g_eventTail; return result; }
    memset(result, 0, sizeof(*result)); result->evTime = Sys_Milliseconds(); return result;
}
void Sys_LoadingKeepAlive() { Sys_PumpSDLEvents(Sys_Milliseconds()); }

// ---- Input / window (driven by the SDL layer / GL backend) -----------------
// Poll the mouse each frame and feed it to the client, mirroring win_input.cpp's
// IN_Frame -> IN_MouseMove -> CL_MouseEvent. SDL events are pumped every frame by
// Com_EventLoop (Sys_GetEvent -> Sys_PumpSDLEvents), so SDL_GetMouseState is
// current. CL_MouseEvent routes the window-relative position to the menu cursor
// (UI_MouseEvent) when a menu is open, or accumulates the delta for game look
// otherwise; its return value (recenterMouse) requests relative/captured mode,
// which maps cleanly to SDL's relative mouse mode.
void IN_Frame() {
#if defined(__EMSCRIPTEN_PTHREADS__)
    // Web has no SDL window; mouse comes from the shared input block. Menu uses the
    // absolute (scaled) cursor; in-game look uses Pointer Lock movement deltas.
    static bool relative = false, primed = false;
    static int oldX = 0, oldY = 0;
    int x = 0, y = 0, mdx = 0, mdy = 0;
    WebInput_GetMouse(&x, &y, &mdx, &mdy);   // position (engine px) + accumulated motion
    int dx, dy;
    if (relative) {
        dx = mdx; dy = mdy;
    } else {
        if (!primed) { oldX = x; oldY = y; primed = true; }
        dx = x - oldX; dy = y - oldY;
        oldX = x; oldY = y;
    }
    int recenterWeb = CL_MouseEvent(x, y, dx, dy);
    if ((bool)recenterWeb != relative) {
        relative = recenterWeb != 0;
        primed = false;
        // Pointer lock via an async DOM call so this worker never blocks. The browser
        // honours the request on the next user click (gesture requirement) for look.
        if (relative)
            MAIN_THREAD_ASYNC_EM_ASM({ var c = document.getElementById('canvas'); if (c && c.requestPointerLock) c.requestPointerLock(); });
        else
            MAIN_THREAD_ASYNC_EM_ASM({ if (document.exitPointerLock) document.exitPointerLock(); });
    }
    return;
#else
    // In-game look uses relative-mouse mode: SDL captures and recenters the cursor,
    // so absolute SDL_GetMouseState positions jump around and differencing them gives
    // garbage deltas (the view "teleports" while looking). Use SDL_GetRelativeMouseState
    // for motion when captured; only the menu path needs the absolute cursor position.
    static bool relative = false;
    int x = 0, y = 0, dx = 0, dy = 0;
    if (relative) {
        SDL_GetRelativeMouseState(&dx, &dy);   // accumulated look deltas since last call
        SDL_GetMouseState(&x, &y);             // position (unused in-game, but harmless)
    } else {
        SDL_GetMouseState(&x, &y);             // absolute window-relative cursor (menu)
        static int oldX = 0, oldY = 0;
        static bool primed = false;
        if (!primed) { oldX = x; oldY = y; primed = true; }
        dx = x - oldX; dy = y - oldY;
        oldX = x; oldY = y;
    }
    int recenter = CL_MouseEvent(x, y, dx, dy);
    if ((bool)recenter != relative) {
        relative = recenter != 0;
        SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE);
        if (relative) SDL_GetRelativeMouseState(nullptr, nullptr);  // drop the entry-frame jump
    }
#endif // __EMSCRIPTEN_PTHREADS__
}
#if defined(__EMSCRIPTEN_PTHREADS__)
// No window-relative warp on the web (only Pointer Lock can move the cursor); the
// menu cursor is driven by absolute position, so warp is a no-op. Hide/show the OS
// cursor over the canvas so the engine's own menu cursor isn't doubled.
void IN_SetCursorPos(unsigned int, unsigned int) {}
void IN_ShowSystemCursor(bool show) {
    // Async so the game worker never blocks on the DOM thread.
    MAIN_THREAD_ASYNC_EM_ASM({
        var c = document.getElementById('canvas');
        if (c) c.style.cursor = $0 ? 'auto' : 'none';
    }, show ? 1 : 0);
}
#else
void IN_SetCursorPos(unsigned int x, unsigned int y) { SDL_WarpMouseInWindow(nullptr, (int)x, (int)y); }
void IN_ShowSystemCursor(bool show) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
#endif

// ---- Misc Windows extras: no-ops on Linux ----------------------------------
char *Sys_GetClipboardData() { return nullptr; }
void  Sys_OpenURL(const char *, int) {}
void Sys_ShowConsole() {}
void  Sys_DestroySplashWindow() {}
void  Sys_HideSplashWindow() {}
void  Sys_UpdateHotkeyBlock() {}

// ---- Networking: minimal (offline) -----------------------------------------
void NET_Init() {}
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
// Cooperative single-OS-thread build: yield to the fiber scheduler instead of blocking
// the one OS thread (a real usleep would freeze the page).
extern void WebFiber_Yield(void);
void NET_Sleep(unsigned int /*msec*/) { WebFiber_Yield(); }
#else
// Desktop, or Emscripten-with-pthreads (this is its own Web Worker): a real usleep
// parks just this worker and lets the others run.
void NET_Sleep(unsigned int msec) { if (msec) usleep(msec * 1000u); }
#endif
void NET_RestartDebug() {}
void NET_ShutdownDebug() {}
char Sys_SendPacket(unsigned int, unsigned char *, netadr_t) { return 1; }
int  Sys_GetPacket(netadr_t *, msg_t *) { return 0; }
int  Sys_SocketPool_GetPacket(netadr_t *, msg_t *) { return 0; }
int  Sys_StringToAdr(const char *, netadr_t *a) { if (a) memset(a, 0, sizeof(*a)); return 0; }
int  Sys_IsLANAddress(netadr_t) { return 0; }
void Sys_CheckForNATOverflow() {}

// ---- Remote script-debug sockets: not supported (no-ops) -------------------
char Sys_StartRemoteDebugServer() { return 0; }
int  Sys_IsRemoteDebugServer() { return 0; }
void Sys_AckDebugSocket() {}
bool Sys_DebugSocketReady(int) { return false; }
int  Sys_UpdateDebugSocket() { return 0; }
void Sys_FlushDebugSocketData() {}
void Sys_EndWriteDebugSocket() {}
int  Sys_ReadDebugSocketInt() { return 0; }
char *Sys_ReadDebugSocketString() { return nullptr; }
void Sys_WriteDebugSocketData(unsigned char *, int) {}
void Sys_WriteDebugSocketInt(int) {}
void Sys_WriteDebugSocketMessageType(unsigned char) {}
void Sys_WriteDebugSocketString(char *) {}

// ---- Localization (English) ------------------------------------------------
char *Win_GetLanguage() { return (char *)"english"; }  // language NAME, not index (read from registry on Windows)
const char *Win_LocalizeRef(const char *ref) { return ref ? ref : ""; }

// ---- Aligned allocation ----------------------------------------------------
extern "C" void *_aligned_malloc(size_t size, size_t align) {
    void *p = nullptr; if (align < sizeof(void *)) align = sizeof(void *);
    if (posix_memalign(&p, align, size) != 0) return nullptr; return p;
}
extern "C" void _aligned_free(void *p) { free(p); }

// MSVC spells unlink _unlink; one decompiled TU calls it by that name.
extern "C" int _unlink(const char *path) { return unlink(path); }
