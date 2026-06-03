// web_fibers.h — cooperative green-thread (fiber) scheduler for the single-OS-thread
// Emscripten / WebGL2 build. Emscripten only; the whole module compiles out elsewhere.
//
// WHY THIS EXISTS
// ---------------
// The wasm build has no Web Workers / pthreads (the page is served over plain LAN
// HTTP, so SharedArrayBuffer / COOP-COEP are unavailable). But the engine spawns
// several worker "threads" (DATABASE, BACKEND, SERVER, OCCLUSION, STREAM) and then
// *blocks the main thread* on Win32 events those workers are supposed to signal
// (e.g. Com_Init -> DB_SyncXAssets -> WaitForSingleObject(databaseCompletedEvent,
// INFINITE)). With the old no-op thread stub the worker never ran, so the boot
// froze on that wait.
//
// This module runs every engine "thread" as a cooperative FIBER on the one OS
// thread, using Emscripten's Asyncify fibers (emscripten_fiber_*). A blocking wait
// no longer hangs: it yields to the scheduler, which runs the worker fiber that
// will signal the event, then resumes the waiter. Because fiber swaps are built on
// Asyncify (emscripten_fiber_swap unwinds the current fiber to the JS boundary and
// the Asyncify trampoline rewinds the target), the engine's *asynchronous file
// reads* (web_fs.cpp's EM_ASYNC_JS Asyncify bridge) keep working INSIDE a worker
// fiber: each fiber carries its own Asyncify stack, so an FS read suspends/resumes
// against that fiber's stack.
//
// COOPERATIVE MODEL (single OS thread => no real preemption / no data races)
//   * One fiber per engine thread context, plus the "main" fiber (the OS entry
//     context: main()/callMain and every emscripten_set_main_loop Com_Frame tick).
//   * A fiber runs until it voluntarily yields (a blocking wait, a lock it cannot
//     take, or Sleep). The scheduler then round-robins to the next runnable fiber.
//   * Events / mutexes / joins are modeled as wait conditions a fiber blocks on;
//     SetEvent / ReleaseMutex / a fiber returning wakes the blocked fibers.
//
// All entry points are safe to call from any fiber (including the main fiber).
#pragma once
#ifdef __EMSCRIPTEN__

#include <stddef.h>

// Opaque per-fiber handle (a WebFiber* internally). Returned by WebFiber_Create and
// used as the Win32 K_THREAD object's identity for joins.
struct WebFiber;

// Per-swap identity hook: invoked by the scheduler right after it makes a fiber the
// running one, with that fiber's engine thread context (or -1 for the main fiber).
// threads.cpp registers this so Sys_GetCurrentThreadId() reports the running fiber's
// context (keeping Sys_Is{Render,Database,...}Thread() correct).
typedef void (*WebFiber_OnSwitch)(int threadContext);

// Initialize the scheduler. Captures the CURRENT execution context as the main
// fiber (via emscripten_fiber_init_from_current_context). Call once, early, from
// the main thread (before any worker fiber is created). Idempotent.
void WebFiber_Init(WebFiber_OnSwitch onSwitch);

// Create a worker fiber bound to engine thread context `threadContext`. The fiber is
// created NOT runnable; its first scheduling will call `entry(threadContext)`. The
// returned handle is used by WebFiber_Resume and as the join target. `entry` is the
// engine thread entry (Sys_ThreadMain-style: it never normally returns).
WebFiber *WebFiber_Create(void (*entry)(unsigned int), unsigned int threadContext);

// Mark a created fiber runnable (the Win32 ResumeThread / Sys_ResumeThread seam).
// Safe to call before the scheduler ever runs; safe to call more than once.
void WebFiber_Resume(WebFiber *f);

// Yield the running fiber and let the scheduler pick the next runnable fiber. Used
// by Sleep / SwitchToThread. Returns once this fiber is scheduled again.
void WebFiber_Yield(void);

// ---- Wait primitives (used by the Win32 emulation in win_kernel.cpp) ----------
// These operate on opaque "wait token" pointers — the scheduler does not interpret
// them; it just matches WebFiber_Wake(token) against waiters blocked on that token.
// win_kernel.cpp passes each KObject* (its event/mutex object) as the token.

// Block the running fiber until WebFiber_Wake(token) is called. Yields to the
// scheduler. Returns true if woken, false if it returned because no other fiber was
// runnable (caller-defined meaning, e.g. WAIT_TIMEOUT for a finite/zero wait).
// `allowDeadlockReturn`: if true and there is NOTHING else runnable, return false
// immediately instead of yielding into a guaranteed hang (used for finite-timeout
// waits so a 1ms spin-wait loop makes progress rather than deadlocking).
bool WebFiber_WaitOn(const void *token, bool allowDeadlockReturn);

// Wake every fiber blocked on `token` (move them to the ready queue). No-op if none.
void WebFiber_Wake(const void *token);

// True if at least one fiber (other than the running one) is currently runnable or
// blocked-but-wakeable — used by win_kernel to decide if a finite wait can make
// progress. Conservative: returns true whenever any non-running worker fiber exists
// that is not permanently finished.
bool WebFiber_HasOtherLiveFiber(void);

// The running fiber's engine thread context (-1 for the main fiber). Lets the Win32
// layer label diagnostics.
int WebFiber_CurrentContext(void);

#endif // __EMSCRIPTEN__
