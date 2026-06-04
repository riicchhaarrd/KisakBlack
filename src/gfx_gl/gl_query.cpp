// gl_query.cpp — GL occlusion / event queries + GLDevice::CreateQuery.
#include "gl_query.h"
#include "gl_d3d9.h"

#include <GL/glew.h>

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
unsigned long g_kbTexUploads  = 0;   // glTex(Compressed)Image2D calls
unsigned long g_kbTexBytes    = 0;   // bytes of texture data uploaded
unsigned long g_kbBufBytes    = 0;   // bytes of vertex/index buffer data uploaded

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
#if defined(__EMSCRIPTEN__)
        // Proxied web context: glGetQueryObjectuiv(GL_QUERY_RESULT) when the result is
        // not yet ready forces a full command-queue flush + GPU wait (~500 ms) — the
        // in-game occlusion stall. But occlusion culling is load-bearing: defaulting
        // "visible" while waiting floods the renderer and freezes on spawn. So read the
        // result (which BLOCKS) ONLY on the first-ever read of this query, where there is
        // no cached value — that keeps spawn culling accurate without flooding. In steady
        // state `available` is already true (we read last frame's finished query) so the
        // read does not block; when it is not yet available we reuse the last real result
        // (1-frame latency) instead of stalling. Never default-open, never force a flush.
        if (available || !haveResult_) {
            GLuint samples = 0;
            glGetQueryObjectuiv(glQuery_, GL_QUERY_RESULT, &samples);
            // GL_ANY_SAMPLES_PASSED yields 0/1; expand to a large "visible" count.
            lastResult_ = samples ? 0xFFFFu : 0u;
            haveResult_ = true;
        }
        if (pData) *static_cast<DWORD *>(pData) = lastResult_;
        return S_OK;
#else
        if (!available && !flush) return S_FALSE;       // not ready yet
        GLuint samples = 0;
        glGetQueryObjectuiv(glQuery_, GL_QUERY_RESULT, &samples);  // blocks if flush
        if (pData) *static_cast<DWORD *>(pData) = samples;
        return S_OK;
#endif
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
        GLenum r = glClientWaitSync(static_cast<GLsync>(sync_),
                                    flush ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, 0);
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
