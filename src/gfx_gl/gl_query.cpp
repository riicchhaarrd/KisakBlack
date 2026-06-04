// gl_query.cpp — GL occlusion / event queries + GLDevice::CreateQuery.
#include "gl_query.h"
#include "gl_d3d9.h"

#include <GL/glew.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cstdio>
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
unsigned long g_kbBlits       = 0;   // StretchRect / glBlitFramebuffer calls
unsigned long g_kbPresentEnter= 0;   // SwapBuffers entries (before commit_frame)
unsigned long g_kbDevSpin     = 0;   // frontend spin iterations waiting for the DX device lock

#if defined(__EMSCRIPTEN__)
// Called every 500ms from the DOM-thread heartbeat (linux_main.cpp). Reads the render
// thread's GL-call counters from shared memory: during a freeze, whether these keep
// climbing pinpoints WHICH loop the render thread is spinning in (occlusion / fence /
// draws) or, if all frozen, that it is stuck OUTSIDE the GL layer (physics/SMP/condvar).
// Returns a string for JS to console.log. Uses snprintf (NO stderr FILE lock): calling
// fprintf here on the DOM thread can deadlock against a worker that holds the line-buffered
// stderr lock while blocked on its own proxied write — which itself was freezing the page.
extern "C" EMSCRIPTEN_KEEPALIVE const char *kb_heartbeat_dump() {
    static char buf[192];
    extern int g_AcquisitionCount; extern unsigned long long g_DXDeviceThread;
    snprintf(buf, sizeof(buf),
             "[hb] com=%lu sv=%lu draws=%lu pres=%lu | devSpin=%lu devOwner=%u devAcq=%d",
             g_kbComFrames, g_kbSvFrames, g_kbDraws, g_kbPresentEnter,
             g_kbDevSpin, (unsigned)(g_DXDeviceThread & 0xffffu), g_AcquisitionCount);
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
    if (type_ == D3DQUERYTYPE_OCCLUSION) {
        if (dwIssueFlags & D3DISSUE_BEGIN) glBeginQuery(KB_OCCLUSION_TARGET, glQuery_);
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
