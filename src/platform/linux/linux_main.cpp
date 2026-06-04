// linux_main.cpp — the Linux entry point + the remaining platform stubs that pull
// the whole engine together into a binary. main() mirrors win32/win_main.cpp's WinMain
// init sequence (critical sections -> main thread/TLS -> Com_Init -> Com_Frame loop).
// The gamepad, worker-thread/task-manager, streaming and crypto symbols are stubbed so
// the engine links and boots; these are progressively replaced as bring-up continues.
#include <qcommon/common.h>
#include <qcommon/threads.h>
#include <qcommon/tl_support.h>
#include <win32/win_main.h>
#include <win32/win_common.h>
#include <win32/win_gamepad.h>
#include <win32/win_workercmds.h>
#include <win32/win_tasks.h>
#include <win32/win_stream.h>
#include <demo/demo_common.h>
#include <physics/phys_broad_phase.h>

#include <cstring>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#if defined(__EMSCRIPTEN__)
// musl/wasm has no <execinfo.h> (glibc backtrace) and cannot block in main(): the
// browser event loop must be driven via emscripten_set_main_loop instead.
#include <emscripten.h>
#else
#include <execinfo.h>
#endif

// Crash backtrace — the engine's asserts trap via __debugbreak (SIGILL); print where.
#if defined(__EMSCRIPTEN__)
static void CrashHandler(int sig) {
    // No backtrace() on wasm; the browser console / source map give the trace.
    fprintf(stderr, "\n*** caught signal %d ***\n", sig);
    fflush(stderr); _exit(128 + sig);
}
#else
static void CrashHandler(int sig) {
    void *bt[40]; int n = backtrace(bt, 40);
    fprintf(stderr, "\n*** caught signal %d — backtrace (%d frames) ***\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
    fflush(stderr); _exit(128 + sig);
}
#endif

// ---- Gamepad: no controller (SDL game-controller support is future work) ----
const dvar_t *gpad_enabled = nullptr;
bool   GPad_IsActive(int) { return false; }
double GPad_GetButton(int, GamePadButton) { return 0.0; }
double GPad_GetStick(int, GamePadStick) { return 0.0; }
bool   GPad_IsButtonPressed(int, GamePadButton) { return false; }
bool   GPad_IsStickPressed(int, GamePadStick, GamePadStickDir) { return false; }
bool   GPad_IsStickReleased(int, GamePadStick, GamePadStickDir) { return false; }


// ---- Streaming / task manager (worker threads now in linux_workercmds.cpp) ---
// R_InitWorkerThreads, IW_task_manager_*, and the nuge_physics job module are the
// real job-queue bring-up, ported to src/platform/linux/linux_workercmds.cpp.
char Stream_Init() { return 1; }
bool PC_StartWithNoSounds() { return false; }
char TaskManager_AnyTaskInProgress(overlappedTask *) { return 0; }
void TaskManager_ClearOverlappedTasks(overlappedTask *) {}
void TaskManager_ClearTask(overlappedTask *) {}
void *TaskManager_GetTaskData(overlappedTask *) { return nullptr; }
char TaskManager_TaskIsInProgress(overlappedTask *, int) { return 0; }

// ---- vtable/typeinfo anchors for two decompiled polymorphic structs ---------
// Defining each class's key virtual out-of-line makes the compiler emit its vtable
// and typeinfo here (otherwise nothing in the build emits them).
unsigned int bdDownloadInterceptor::handleDownload(void *, unsigned int) { return 0; }
void broad_phase_terrain_query_callback::query(const broad_phase_environment_query_input *, broad_phase_environement_query_results *) {}

// ---- libtomcrypt: CBC + cipher-registry stubs -------------------------------
// TODO(bring-up): compile the real libtomcrypt for fast-file decryption. Stubbed to
// link (signatures match tomcrypt.h, which the engine headers pull in).
extern "C" {
int find_cipher(const char *) { return -1; }
int cbc_start(int, const unsigned char *, const unsigned char *, int, int, symmetric_CBC *) { return 0; }
int cbc_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_CBC *) { if (pt && ct && pt != ct) memcpy(ct, pt, len); return 0; }
int cbc_decrypt(const unsigned char *ct, unsigned char *pt, unsigned long len, symmetric_CBC *) { if (ct && pt && ct != pt) memcpy(pt, ct, len); return 0; }
int cbc_setiv(const unsigned char *, unsigned long, symmetric_CBC *) { return 0; }
const char *error_to_string(int) { return "ok"; }
}

// ---- Entry point -----------------------------------------------------------
int main(int argc, char **argv) {
#if defined(__EMSCRIPTEN__)
    // Console writes are proxied to the DOM thread; with unbuffered stdio that is a
    // per-CHARACTER round-trip, so the engine's warning spam alone can dominate frame
    // time. Line-buffer both streams: each message flushes as ONE proxied write at its
    // newline (output still appears promptly, but the per-character tax is gone).
    static char kbOutBuf[1 << 16], kbErrBuf[1 << 16];
    setvbuf(stdout, kbOutBuf, _IOLBF, sizeof(kbOutBuf));
    setvbuf(stderr, kbErrBuf, _IOLBF, sizeof(kbErrBuf));
    // Diagnostic heartbeat on the BROWSER (DOM) thread, independent of the engine. If it
    // keeps logging during a "freeze" the DOM thread is alive (the render thread is stuck);
    // if it stops, a proxied call is blocking the DOM thread itself. Decides where to look.
    MAIN_THREAD_EM_ASM({
        if (!globalThis.__kbHb) {
            var n = 0;
            globalThis.__kbHb = setInterval(function() {
                // Lock-free: get a string pointer and log it from JS (no stderr FILE lock).
                var s = '';
                try { if (Module['_kb_heartbeat_dump']) s = UTF8ToString(Module['_kb_heartbeat_dump']()); } catch (e) {}
                console.log('[heartbeat] #' + (++n) + ' ' + s);
            }, 2000);
        }
    });
#endif
    char cmdline[2048] = {0};
    for (int i = 1; i < argc; ++i) {
        size_t n = strlen(cmdline);
        snprintf(cmdline + n, sizeof(cmdline) - n, "%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    fprintf(stderr, "[KisakBlack] boot: cmdline=\"%s\"\n", cmdline);
    signal(SIGILL, CrashHandler); signal(SIGSEGV, CrashHandler); signal(SIGABRT, CrashHandler);

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    Sys_SetupTLCallbacks(0x900000);
    Com_Init(cmdline);

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    // Single-OS-thread fiber build: browsers can't block in main(), so hand one
    // Com_Frame() per rAF tick to the event loop. fps=0 => use requestAnimationFrame;
    // simulate_infinite_loop=1 keeps the runtime/stack alive after main() returns.
    emscripten_set_main_loop([]{ Com_Frame(); }, 0, 1);
#elif defined(__EMSCRIPTEN_PTHREADS__)
    // Pthreads build with -sPROXY_TO_PTHREAD: main() runs on a dedicated Web Worker
    // (NOT the browser DOM thread), so it CAN block in a normal loop without freezing
    // the page — exactly like the desktop build, and with NO Asyncify. The render
    // BACKEND runs on its own worker (RB_RenderThread, spawned via Sys_SpawnRenderThread
    // under sys_smp_allowed==1), owns the WebGL2 context and swaps buffers; this worker
    // just drives the game frame and trades the DX-device-ownership lock with it — the
    // deadlock the cooperative single-thread build could not resolve. When this worker
    // blocks on a Win32 event (a real pthread condvar) the Emscripten runtime services
    // its proxying queue, so DOM/canvas events still flow. No rAF driver is needed.
    for (;;) Com_Frame();
#else
    for (;;) Com_Frame();
#endif
    return 0;
}
