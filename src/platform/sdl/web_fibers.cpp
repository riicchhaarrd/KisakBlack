// web_fibers.cpp — cooperative fiber (green-thread) scheduler over Emscripten's
// Asyncify fibers. See web_fibers.h for the rationale. Emscripten only.
//
// HOW THE ASYNCIFY FIBER SWAP WORKS (and why this is correct)
// ----------------------------------------------------------
// emscripten_fiber_swap(old, new) is itself an Asyncify async op: it UNWINDS the
// current fiber's wasm stack into old->asyncify_data, records new as the next
// fiber, and the unwind propagates out to the JS boundary (callMain or the
// emscripten_set_main_loop Com_Frame tick). When the wasm export returns, the
// Asyncify "maybeStopUnwind" hook runs Fibers.trampoline(), which REWINDS into
// `new` (restoring its C stack + asyncify stack). So a swap is a yield to JS and
// back into another fiber — fully compatible with emscripten_set_main_loop, which
// re-enters wasm each rAF tick.
//
// A swap is only legal when Asyncify.state == Normal (no other async op mid-flight).
// We only ever swap from engine code running "normally" — never from inside an FS
// read (those EM_ASYNC_JS reads complete, suspend+resume, before control returns to
// engine code that would call a wait/yield). Each fiber owns its own Asyncify stack,
// so an FS read inside a worker fiber unwinds/rewinds against THAT fiber's stack.
//
// SINGLE OS THREAD: there is no real concurrency. A fiber holds the CPU until it
// yields. That makes the scheduler data-race-free by construction; the only failure
// mode is a logical deadlock (everyone blocked), which we detect and log loudly
// instead of hanging silently.
#ifdef __EMSCRIPTEN__

#include "web_fibers.h"

#include <emscripten/fiber.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <deque>

// Defined at global scope below; forward-declared here so the anonymous-namespace
// fiber entry trampoline can call it without picking up a (wrong) internal-linkage
// symbol. (One-way swap when a fiber's entry returns.)
static void WebFiber_ReturnToScheduler(struct WebFiber *finished);

namespace {

// Per-fiber C stack and Asyncify stack. Sized generously: the FS reads (and the
// whole engine call tree, e.g. DB_TryLoadXFile -> fastfile inflate) run on the
// worker fiber's C stack, and Asyncify needs room for the unwound frames.
constexpr size_t kCStackSize       = 4 * 1024 * 1024;   // 4 MB C stack per fiber
constexpr size_t kAsyncifyStackSize = 512 * 1024;       // 512 KB asyncify stack per fiber

enum FiberState {
    FS_NEW,        // created, never scheduled (entry not yet run)
    FS_READY,      // runnable, in the ready queue
    FS_RUNNING,    // currently executing
    FS_BLOCKED,    // waiting on a token (not runnable until woken)
    FS_FINISHED,   // entry returned (engine thread entries normally never do)
};

} // namespace

struct WebFiber {
    emscripten_fiber_t ctx;
    void              *cStack       = nullptr;
    void              *asyncStack   = nullptr;
    FiberState         state        = FS_NEW;
    int                threadContext = -1;       // engine context, -1 == main fiber
    const void        *waitToken    = nullptr;   // token this fiber is blocked on
    void             (*entry)(unsigned int) = nullptr;
    unsigned int       entryArg     = 0;
};

namespace {

WebFiber                 g_mainFiber;            // the OS entry context (main / Com_Frame)
WebFiber                *g_current = nullptr;    // the running fiber
std::vector<WebFiber *>  g_all;                  // every worker fiber (for liveness checks)
std::deque<WebFiber *>   g_ready;                // runnable worker fibers, round-robin
WebFiber_OnSwitch        g_onSwitch = nullptr;
bool                     g_inited   = false;

// Push a fiber onto the ready queue if it isn't already there/running.
void MakeReady(WebFiber *f) {
    if (f->state == FS_READY || f->state == FS_RUNNING || f->state == FS_FINISHED)
        return;
    f->state = FS_READY;
    f->waitToken = nullptr;
    g_ready.push_back(f);
}

// Tell the engine which context is now current (so Sys_GetCurrentThreadId works).
void NotifySwitch(WebFiber *f) {
    if (g_onSwitch) g_onSwitch(f->threadContext);
}

// Pop the next runnable fiber, or null if none. (The main fiber is never in
// g_ready; it is scheduled by being swapped back to directly when no worker is
// runnable, or when a worker yields and the main fiber is the only thing waiting.)
WebFiber *PopReady() {
    while (!g_ready.empty()) {
        WebFiber *f = g_ready.front();
        g_ready.pop_front();
        if (f->state == FS_READY) return f;
    }
    return nullptr;
}

// True if any worker fiber could still run again (ready, blocked-but-wakeable, or
// not-yet-started). A FINISHED fiber can never run again.
bool AnyWorkerLive() {
    for (WebFiber *f : g_all)
        if (f != g_current && f->state != FS_FINISHED) return true;
    return false;
}

// Trampoline that runs a worker fiber's entry exactly once, then parks it FINISHED.
// emscripten_fiber expects a void(*)(void*) entry; we stash the WebFiber* as the arg.
void FiberEntryTrampoline(void *arg) {
    WebFiber *f = static_cast<WebFiber *>(arg);
    // We are now running on f. (NotifySwitch was already called by the swap site.)
    if (f->entry) f->entry(f->entryArg);
    // Engine thread entries loop forever; reaching here means the fiber returned.
    f->state = FS_FINISHED;
    f->waitToken = nullptr;
    // Hand control back to whoever should run next. We must never "return" from a
    // fiber entry (there is nowhere to return to), so swap away and never come back.
    WebFiber_ReturnToScheduler(f);
}

} // namespace

// Swap from `from` into `to`, updating bookkeeping + identity. `to` must be a fiber
// whose ctx is initialized (NEW fibers are initialized at create time).
static void SwapTo(WebFiber *from, WebFiber *to) {
    g_current = to;
    to->state = FS_RUNNING;
    NotifySwitch(to);
    emscripten_fiber_swap(&from->ctx, &to->ctx);
    // --- When we get back here, `from` is running again. Restore its identity. ---
    g_current = from;
    from->state = FS_RUNNING;
    NotifySwitch(from);
}

// Called from FiberEntryTrampoline when a worker entry returns: swap to the next
// runnable fiber (or main) and never come back.
static void WebFiber_ReturnToScheduler(WebFiber *finished) {
    WebFiber *next = PopReady();
    if (!next) next = &g_mainFiber;
    g_current = next;
    next->state = FS_RUNNING;
    NotifySwitch(next);
    // One-way swap: `finished` will never be scheduled again.
    emscripten_fiber_swap(&finished->ctx, &next->ctx);
    // unreachable
}

void WebFiber_Init(WebFiber_OnSwitch onSwitch) {
    if (g_inited) { g_onSwitch = onSwitch; return; }
    g_onSwitch = onSwitch;

    // The main fiber must own an Asyncify stack so it too can suspend (its FS reads
    // and its fiber swaps both unwind against this stack). Heap-allocate it; it
    // lives for the whole program.
    void *mainAsync = malloc(kAsyncifyStackSize);
    if (!mainAsync) { fprintf(stderr, "[web_fibers] OOM allocating main asyncify stack\n"); abort(); }
    memset(&g_mainFiber, 0, sizeof(g_mainFiber));
    g_mainFiber.threadContext = -1;
    g_mainFiber.state = FS_RUNNING;
    g_mainFiber.asyncStack = mainAsync;
    emscripten_fiber_init_from_current_context(&g_mainFiber.ctx, mainAsync, kAsyncifyStackSize);

    g_current = &g_mainFiber;
    g_inited = true;
    NotifySwitch(&g_mainFiber);
}

WebFiber *WebFiber_Create(void (*entry)(unsigned int), unsigned int threadContext) {
    WebFiber *f = new WebFiber();
    f->entry = entry;
    f->entryArg = threadContext;
    f->threadContext = (int)threadContext;
    f->state = FS_NEW;
    f->cStack = malloc(kCStackSize);
    f->asyncStack = malloc(kAsyncifyStackSize);
    if (!f->cStack || !f->asyncStack) {
        fprintf(stderr, "[web_fibers] OOM allocating fiber stacks (ctx=%u)\n", threadContext);
        abort();
    }
    // emscripten_fiber_init takes the entry as em_arg_callback_func (void(*)(void*));
    // we pass the WebFiber* so the trampoline can find itself and run f->entry.
    emscripten_fiber_init(&f->ctx, FiberEntryTrampoline, f,
                          f->cStack, kCStackSize,
                          f->asyncStack, kAsyncifyStackSize);
    g_all.push_back(f);
    return f;
}

void WebFiber_Resume(WebFiber *f) {
    if (!f) return;
    MakeReady(f);
}

void WebFiber_Yield(void) {
    if (!g_inited || !g_current) return;
    WebFiber *self = g_current;
    // A plain yield: put ourselves back on the ready queue (if we're a worker) so we
    // get another turn, then schedule someone else. The main fiber is not queued; it
    // resumes whenever workers run dry.
    WebFiber *next = PopReady();
    if (!next) return;                 // nobody else to run — stay on self
    if (self != &g_mainFiber) {        // re-queue self behind the others
        self->state = FS_READY;
        g_ready.push_back(self);
    }
    SwapTo(self, next);
}

bool WebFiber_WaitOn(const void *token, bool allowDeadlockReturn) {
    if (!g_inited || !g_current) return false;
    WebFiber *self = g_current;

    // If returning-on-deadlock is allowed (finite/zero timeout) and there is no
    // other live fiber that could ever signal us, don't block — report timeout.
    if (allowDeadlockReturn && !AnyWorkerLive() && self == &g_mainFiber)
        return false;

    self->state = FS_BLOCKED;
    self->waitToken = token;

    WebFiber *next = PopReady();
    if (!next) {
        // Nothing runnable right now. If we're a worker, go back to the main fiber
        // (its waiter may make progress / pump the loop). If we're the main fiber
        // and allowed to time out, unblock and return false. Otherwise it's a hang.
        if (self != &g_mainFiber) {
            SwapTo(self, &g_mainFiber);
            // resumed: we were woken (token matched) — fall through to success
        } else if (allowDeadlockReturn) {
            self->state = FS_RUNNING;
            self->waitToken = nullptr;
            return false;
        } else {
            // Main fiber, INFINITE wait, nothing else runnable => real deadlock.
            fprintf(stderr,
                "[web_fibers] DEADLOCK: main fiber blocked (token=%p) with no runnable "
                "worker fiber. No one can signal it. (Did a worker thread fail to start "
                "or already finish?)\n", token);
            // Best effort: unblock ourselves and return false so the caller's spin
            // loop at least keeps the app responsive rather than freezing the tab.
            self->state = FS_RUNNING;
            self->waitToken = nullptr;
            return false;
        }
    } else {
        SwapTo(self, next);
    }

    // We were swapped back in. WebFiber_Wake sets us READY and re-queues us; by the
    // time SwapTo returns to us we are RUNNING again. Success.
    self->state = FS_RUNNING;
    self->waitToken = nullptr;
    return true;
}

void WebFiber_Wake(const void *token) {
    if (!g_inited) return;
    // Wake the main fiber if it is blocked on this token (it isn't in g_all).
    if (g_mainFiber.state == FS_BLOCKED && g_mainFiber.waitToken == token) {
        g_mainFiber.state = FS_READY;   // logically; the main fiber resumes by swap-back
        g_mainFiber.waitToken = nullptr;
    }
    // Wake every worker fiber blocked on this token.
    for (WebFiber *f : g_all) {
        if (f->state == FS_BLOCKED && f->waitToken == token)
            MakeReady(f);
    }
}

bool WebFiber_HasOtherLiveFiber(void) {
    return AnyWorkerLive();
}

int WebFiber_CurrentContext(void) {
    return g_current ? g_current->threadContext : -1;
}

#endif // __EMSCRIPTEN__
