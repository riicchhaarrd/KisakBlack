// gl_query.cpp — GL occlusion / event queries + GLDevice::CreateQuery.
#include "gl_query.h"

#include "gl_optrace.h"

// ---- kbprof: runtime per-zone SELF-TIME profiler (?kbprof) — declared in universal/profile.h.
// Reuses the engine's existing PROF_SCOPED zones; each thread accumulates self-time per zone and
// dumps its top zones periodically. Lets us pinpoint the backend "other" hotspot with no hand-timers.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <universal/profile.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <pthread.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
namespace kbprof {
int g_on = -1;
int Init() {
    // Resolve ?kbprof. The PROF_SCOPED zones only ever run on the engine's WORKER threads
    // (backend/frontend/physics), never on the browser main thread. A worker's location.search
    // is its worker-script URL, NOT the page URL — so reading location.search here returns ""
    // and the profiler would never turn on. Instead the page URL is parsed on the MAIN thread in
    // index.html (?kbprof -> ENV KB_KBPROF), forwarded to every pthread, and read with getenv()
    // here — exactly like ?perfms (KB_PERFMS). g_on is a SHARED wasm global, so the first worker
    // to resolve it sets it for all.
    const char *v = getenv("KB_KBPROF");
    g_on = (v && *v == '1') ? 1 : 0;
    return g_on;
}
double Now() { return emscripten_get_now(); }
namespace {
struct Acc { double self = 0.0, incl = 0.0; unsigned calls = 0; };
thread_local std::unordered_map<const char *, Acc> t_acc;   // keyed by the literal name's pointer
thread_local std::vector<double> t_child;                   // per-active-zone child-time accumulator
thread_local unsigned long long t_n = 0;
thread_local double t_dumpT = 0.0;
void Dump() {
    if (t_acc.empty()) return;
    double now = Now();
    double span = t_dumpT > 0.0 ? now - t_dumpT : 0.0;
    t_dumpT = now;
    std::vector<std::pair<const char *, Acc>> v(t_acc.begin(), t_acc.end());
    std::sort(v.begin(), v.end(),
              [](const std::pair<const char *, Acc> &a, const std::pair<const char *, Acc> &b) {
                  return a.second.self > b.second.self;
              });
    fprintf(stderr, "[kbprof tid=%u over %.0fms] top self-time zones:\n",
            (unsigned)(uintptr_t)pthread_self(), span);
    int k = 0;
    for (const auto &p : v) {
        if (k++ >= 15) break;
        fprintf(stderr, "  %-36s self=%.1fms incl=%.1fms n=%u\n",
                p.first, p.second.self, p.second.incl, p.second.calls);
    }
    t_acc.clear();
}
}  // namespace
void Enter() { t_child.push_back(0.0); }
void Exit(const char *name, double t0) {
    double incl = Now() - t0;
    double ch = t_child.empty() ? 0.0 : t_child.back();
    if (!t_child.empty()) t_child.pop_back();
    double self = incl - ch;
    if (self < 0.0) self = 0.0;
    if (!t_child.empty()) t_child.back() += incl;   // bill our inclusive time up to our parent
    Acc &a = t_acc[name];
    a.self += self; a.incl += incl; ++a.calls;
    if ((++t_n & 0x3FFF) == 0) Dump();              // ~every 16384 zone-exits on this thread
}
}  // namespace kbprof
#endif

// ---- GL-op ring trace (see gl_optrace.h) ------------------------------------
extern unsigned long g_kbDraws;
namespace {
struct KbOp { const char *tag; unsigned a, b, c; unsigned long draws; };
KbOp     s_ops[96];
unsigned s_opIdx = 0;        // monotonically increasing; slot = idx % 96
}
extern "C" void KB_OpTag(const char *tag, unsigned a, unsigned b, unsigned c) {
    KbOp &o = s_ops[s_opIdx % 96];
    o.tag = tag; o.a = a; o.b = b; o.c = c; o.draws = g_kbDraws;
    ++s_opIdx;
}
extern "C" void KB_DumpOpRing(void) {
    static int dumped = 0;
    if (dumped) return;
    dumped = 1;
    unsigned n = s_opIdx < 96 ? s_opIdx : 96;
    unsigned start = s_opIdx - n;
    fprintf(stderr, "[gl] ===== OP RING (oldest->newest, %u ops, draws=%lu) =====\n", n, g_kbDraws);
    for (unsigned i = 0; i < n; ++i) {
        const KbOp &o = s_ops[(start + i) % 96];
        fprintf(stderr, "[gl] op[%u] %s a=%u b=%u c=%u draws=%lu\n",
                start + i, o.tag ? o.tag : "?", o.a, o.b, o.c, o.draws);
    }
    fprintf(stderr, "[gl] ===== OP RING END =====\n");
}
#include "gl_d3d9.h"

#include <GL/glew.h>
extern "C" void KB_FlushBatchedDraws();
extern "C" void KB_FlushTagged(int cause); // +flush-cause telemetry  // batched-draw flush (gl_d3d9_draw.cpp)

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cstdio>
#include <cstring>
#endif

// WebGL2/GLES3 has no GL_SAMPLES_PASSED (exact sample count) occlusion target — only
// the boolean GL_ANY_SAMPLES_PASSED. Using the wrong target throws GL_INVALID_ENUM on
// every glBeginQuery/glEndQuery (the sun-sprite calibration flooded the console with
// these). Use the boolean target on Emscripten and synthesize a large/zero "count" in
// GetData so the engine's sample-count thresholds still resolve to visible / occluded.
#if defined(__EMSCRIPTEN__)
static constexpr GLenum KB_OCCLUSION_TARGET = GL_ANY_SAMPLES_PASSED;
#else
static constexpr GLenum KB_OCCLUSION_TARGET = GL_SAMPLES_PASSED;
#endif

// Perf instrumentation: every occlusion/event GetData on Emscripten issues a RETURNING
// GL call that synchronously round-trips to the canvas-owning thread (~0.1-0.8 ms each
// while proxied) — the suspected source of the in-game 1 FPS. Count them; SwapBuffers
// dumps the per-frame totals (see glcontext_sdl.cpp).
unsigned long g_kbOcclGetData = 0;   // glGetQueryObjectiv/uiv pairs (occlusion poll)
unsigned long g_kbEventWaits  = 0;   // glClientWaitSync (event-fence poll/spin)
unsigned long g_kbProgLinks   = 0;   // (vs,ps) program links (lazy, at first draw use)
unsigned long g_kbDraws       = 0;   // GLDevice::Draw(Indexed)Primitive calls (render-thread liveness)
unsigned long g_kbTexUploads  = 0;   // glTex(Compressed)Image2D calls
unsigned long g_kbTexBytes    = 0;   // bytes of texture data uploaded
unsigned long g_kbBufBytes    = 0;   // bytes of vertex/index buffer data uploaded
unsigned long g_kbComFrames   = 0;   // Com_Frame() entries (game/main-thread liveness)
unsigned long g_kbSvFrames    = 0;   // SV_Frame() entries (server/physics liveness)
unsigned long g_kbReadbacks   = 0;   // glReadPixels calls (per-frame GPU-sync readbacks)
unsigned long g_kbSkipPending = 0;   // draws DROPPED because the program is still linking (flicker source)
unsigned long g_kbBuiltinFall = 0;   // draws degraded to the builtin program (translate/compile gave 0)
unsigned long g_kbShadowFbo   = 0;   // SetRenderTarget passes with the honored DS attached COMPLETE
unsigned long g_kbBiasSets    = 0;   // nonzero depth-bias SetRenderState applications
unsigned long g_kbSetDS       = 0;   // SetDepthStencilSurface calls
unsigned long g_kbSetDSTex    = 0;   // ... with a texture-backed DS (shadowmap candidates)
unsigned long g_kbPrimDrop    = 0;   // surfaces dropped by primDrawSurf buffer overflow (mesh pops!)
unsigned long g_kbBlits       = 0;   // StretchRect / glBlitFramebuffer calls
unsigned long g_kbPresentEnter= 0;   // SwapBuffers entries (before commit_frame)
unsigned long g_kbDevSpin     = 0;   // frontend spin iterations waiting for the DX device lock
int           g_kbBackStage   = 0;   // RB_RenderThread loop stage (where the backend is now)
int           g_kbFrontStage  = 0;   // R_ToggleSmpFrameCmd stage (where the frontend is now)
const char   *g_kbWorkerCmdName = "-"; // name of the job-queue batch whose Code is running ("-"=none)
const char   *g_kbWaitName       = "-"; // name of the worker-cmd group currently being waited/flushed
int           g_kbDbcf           = -1;  // fx_draw gate: *frontEndDataOut->dynamicBufferCurrentFrame
int           g_kbFc             = -1;  // fx_draw gate: frontEndDataOut->frameCount
int           g_kbFxStage        = 0;   // FX_GenerateVerts sub-step (where it hangs)
int           g_kbGlassLock      = -1;  // glass rendererLock.lock value at a spinlock acquire
unsigned long g_kbVisCells       = 0;   // popcount(cellVisibleBits) at end of portal walk (flicker probe)
int           g_kbCameraCell      = -2;  // cameraCellIndex (which cell the camera is in) for web-vs-native compare
unsigned long g_kbVisSurfs        = 0;   // popcount(surfaceVisData) after the static-cull worker wait (surface-race probe)

#if defined(__EMSCRIPTEN__)
// Called every 500ms from the DOM-thread heartbeat (linux_main.cpp). Reads the render
// thread's GL-call counters from shared memory: during a freeze, whether these keep
// climbing pinpoints WHICH loop the render thread is spinning in (occlusion / fence /
// draws) or, if all frozen, that it is stuck OUTSIDE the GL layer (physics/SMP/condvar).
// Returns a string for JS to console.log. Uses snprintf (NO stderr FILE lock): calling
// fprintf here on the DOM thread can deadlock against a worker that holds the line-buffered
// stderr lock while blocked on its own proxied write — which itself was freezing the page.
extern "C" EMSCRIPTEN_KEEPALIVE const char *kb_heartbeat_dump() {
    static char buf[384];
    extern int g_AcquisitionCount; extern unsigned long long g_DXDeviceThread;
    snprintf(buf, sizeof(buf),
             "[hb] com=%lu sv=%lu draws=%lu pres=%lu event=%lu blits=%lu rb=%lu | spin=%lu own=%u acq=%d bstage=%d fstage=%d wc=%s ww=%s dbcf=%d fc=%d fxs=%d gl=%d viscells=%lu",
             g_kbComFrames, g_kbSvFrames, g_kbDraws, g_kbPresentEnter,
             g_kbEventWaits, g_kbBlits, g_kbReadbacks,
             g_kbDevSpin, (unsigned)(g_DXDeviceThread & 0xffffu), g_AcquisitionCount, g_kbBackStage, g_kbFrontStage,
             g_kbWorkerCmdName ? g_kbWorkerCmdName : "?", g_kbWaitName ? g_kbWaitName : "?", g_kbDbcf, g_kbFc, g_kbFxStage, g_kbGlassLock, g_kbVisCells);
    { size_t L = strlen(buf); snprintf(buf+L, sizeof(buf)-L, " camcell=%d vissurf=%lu", g_kbCameraCell, g_kbVisSurfs); }
    return buf;
}
#endif

GLQuery::GLQuery(IDirect3DDevice9 *device, D3DQUERYTYPE type) : device_(device), type_(type) {
    if (type_ == D3DQUERYTYPE_OCCLUSION) glGenQueries(1, &glQuery_);
}

GLQuery::~GLQuery() {
    if (glQuery_) glDeleteQueries(1, &glQuery_);
    if (sync_)    glDeleteSync(static_cast<GLsync>(sync_));
}

HRESULT WINAPI GLQuery::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLQuery::Issue(DWORD dwIssueFlags) {
    KB_FlushTagged(11);
    if (type_ == D3DQUERYTYPE_OCCLUSION) {
        if (dwIssueFlags & D3DISSUE_BEGIN) { haveResult_ = false; glBeginQuery(KB_OCCLUSION_TARGET, glQuery_); }
        if (dwIssueFlags & D3DISSUE_END)   glEndQuery(KB_OCCLUSION_TARGET);
    } else if (type_ == D3DQUERYTYPE_EVENT) {
        if (dwIssueFlags & D3DISSUE_END) {
            if (sync_) glDeleteSync(static_cast<GLsync>(sync_));
            sync_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLQuery::GetData(void *pData, DWORD /*dwSize*/, DWORD dwGetDataFlags) {
    bool flush = (dwGetDataFlags & D3DGETDATA_FLUSH) != 0;

    if (type_ == D3DQUERYTYPE_OCCLUSION) {
#if defined(__EMSCRIPTEN__)
        // Result already read since the last Issue(BEGIN): answer from cache — zero GL
        // calls (repeat polls were a returning round-trip each, ~0.12ms proxied).
        if (haveResult_) {
            if (pData) *static_cast<DWORD *>(pData) = lastResult_;
            return S_OK;
        }
#endif
        ++g_kbOcclGetData;
        GLint available = 0;
        glGetQueryObjectiv(glQuery_, GL_QUERY_RESULT_AVAILABLE, &available);
        // Defer when the engine is just polling (no FLUSH): never block here, or a wave of
        // newly-visible objects (look around / spawn) each forces a proxied GPU flush and
        // the scene freezes. Only the explicit-FLUSH path reads the result (may block).
        if (!available && !flush) {
#if defined(__EMSCRIPTEN__)
            // CRITICAL on the deferred/proxied web context: SUBMIT the queued glEndQuery so
            // the GPU can resolve this query. The engine polls in `while (GetData==S_FALSE)`
            // and never reaches commit_frame to flush the command stream; without this the
            // query never becomes available and the render thread spins forever (the freeze
            // the DOM-heartbeat diagnostic pinned to a stuck render thread).
            glFlush();
#endif
            return S_FALSE;
        }
        GLuint samples = 0;
        glGetQueryObjectuiv(glQuery_, GL_QUERY_RESULT, &samples);  // blocks if flush
#if defined(__EMSCRIPTEN__)
        // GL_ANY_SAMPLES_PASSED yields 0/1; expand to a large "visible" count.
        samples = samples ? 0xFFFFu : 0u;
        lastResult_ = samples;
        haveResult_ = true;
#endif
        if (pData) *static_cast<DWORD *>(pData) = samples;
        return S_OK;
    }

    if (type_ == D3DQUERYTYPE_EVENT) {
        // No outstanding fence (never Issue'd, or already consumed): real D3D9
        // reports such an event query as signaled. Returning S_FALSE here makes
        // R_FinishGpuFence's `while (GetData == S_FALSE)` spin forever, since the
        // engine waits on dx.flushGpuQuery without ever issuing it.
        if (!sync_) { if (pData) *static_cast<DWORD *>(pData) = TRUE; return S_OK; }
        // NOTE: this fence is the engine's GPU throttle (R_FinishGpuFence) — it keeps the
        // render backend from outrunning the GPU. Reporting it always-signaled on web
        // removed the throttle and let the proxied command queue flood -> freeze on spawn.
        // Keep the real poll: glClientWaitSync(timeout=0) is non-blocking, returns S_FALSE
        // until the GPU passes the fence; the engine's wait loop throttles correctly.
        ++g_kbEventWaits;
        // ALWAYS pass GL_SYNC_FLUSH_COMMANDS_BIT (not just when the engine asks for FLUSH):
        // the engine polls this fence in `while (GetData==S_FALSE)` and never reaches
        // commit_frame to submit the queued glFenceSync. Without the flush bit the fence is
        // never sent to the GPU, never signals, and the render thread spins forever. The
        // flush bit is the GL-standard guard against exactly this poll deadlock; the GPU
        // throttle still holds because we still return S_FALSE until the fence signals.
        GLbitfield fbit = GL_SYNC_FLUSH_COMMANDS_BIT;
#if !defined(__EMSCRIPTEN__)
        fbit = flush ? GL_SYNC_FLUSH_COMMANDS_BIT : 0;
#endif
        GLenum r = glClientWaitSync(static_cast<GLsync>(sync_), fbit, 0);
        bool done = (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
        if (!done) return S_FALSE;
        if (pData) *static_cast<DWORD *>(pData) = TRUE;
        return S_OK;
    }
    return S_OK;
}

HRESULT WINAPI GLDevice::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) {
    if (!ppQuery) return E_INVALIDARG;
    *ppQuery = new GLQuery(this, Type);
    return D3D_OK;
}
