// glcontext_sdl.cpp — SDL2 implementation of GLContext.
//
// SDL2 gives us one window/GL-context/input path across Linux, macOS and
// Windows-GL, so this single file is the entire window-system dependency of the
// GL backend. GLEW is initialised here so the rest of src/gfx_gl can call modern
// GL entry points directly.
#include "glcontext.h"
#include "gl_optrace.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <cstdio>

#if defined(__EMSCRIPTEN_PTHREADS__)
// ---------------------------------------------------------------------------
// WebGL2 context on the RENDER WORKER (Emscripten pthreads build).
//
// Under -pthread + -sPROXY_TO_PTHREAD the engine's render backend (RB_RenderThread)
// runs on its own Web Worker and is the thread that creates the D3D9 "device" — and
// therefore the thread that must OWN the GL context. SDL2's Emscripten port creates
// its WebGL context via Browser.createContext on the page <canvas>, which only works
// on the browser's main (DOM) thread, so it cannot give a worker an owned context.
//
// Instead we bypass SDL here and create the context directly with the Emscripten
// HTML5 WebGL API, targeting the page canvas selector ("#canvas", matching the
// harness <canvas id="canvas">). We set proxyContextToMainThread =
// EMSCRIPTEN_WEBGL_CONTEXT_PROXY_FALLBACK: if the canvas has NOT been transferred to
// this worker as an OffscreenCanvas, the runtime transparently PROXIES every GL call
// to the DOM thread (where the canvas lives) while the GL state/handle is still owned
// by this worker — so RB_RenderThread keeps issuing draws and swapping while the main
// thread does the actual GPU submission. (With -sOFFSCREENCANVAS_SUPPORT=1 and a
// transferred canvas it would instead render directly on the worker; the FALLBACK
// covers browsers/paths where the transfer didn't happen.)
//
// explicitSwapControl=true + emscripten_webgl_commit_frame() gives us a real swap
// (matching SwapBuffers) rather than the implicit "swap when the rAF callback exits",
// which does not apply when the loop runs on a worker.
#include <emscripten.h>            // emscripten_get_now (proxy self-test)
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#include <pthread.h>               // g_kbGLThread (GL-thread detection, gl_resources.cpp)

// Defined in gl_resources.cpp; set below when the WebGL2 context is created so the
// resource classes can detect off-GL-thread calls and defer them. Declared at file
// scope (the class below sits in an anonymous namespace, which would otherwise give
// a block-scope extern internal linkage).
extern pthread_t g_kbGLThread;

// Defined in src/platform/sdl/sdl_events.cpp — tells the HTML5 input layer the engine
// backbuffer size so it can scale mouse coordinates from CSS pixels.
extern "C" void WebInput_SetResolution(int w, int h);

// Perf counters defined in gl_query.cpp (global namespace).
extern unsigned long g_kbOcclGetData, g_kbEventWaits, g_kbProgLinks;
extern unsigned long g_kbTexUploads, g_kbTexBytes, g_kbBufBytes;
extern unsigned long g_kbDraws, g_kbReadbacks, g_kbPresentEnter;
extern unsigned long g_kbBatchedDraws, g_kbBatchFlushes;  // draw batcher (gl_d3d9_draw.cpp)
extern unsigned long g_kbInstRuns, g_kbInstSaved;        // ?inst instancing (gl_d3d9_draw.cpp)
extern unsigned long g_kbBrk[3], g_kbBrkCause[12]; extern int g_kbBrkMaxRange;  // run-break diag
extern unsigned long g_kbMdrawSameBuf, g_kbMdrawDiffBuf;   // multiDraw-target classification
extern unsigned long g_kbFlushCause[12], g_kbMergeSubmits; // flush-cause telemetry + merge path
extern int g_kbTimeDraws; extern double g_kbMsDraw;        // ?perfms=1 frame split (gl_d3d9_draw.cpp)
extern double g_kbMsBuffers;                               // ?perfms=1 buffer-upload split (file scope: SwapBuffers' class is in an anon namespace)
extern unsigned long g_kbSkipPending, g_kbBuiltinFall;    // dropped/degraded draws (gl_program.cpp)
extern unsigned long g_kbBlits;                           // StretchRect blits (gl_surface_ops.cpp)
extern int g_kbRaceParity;                                // SMP parity of the frame being rendered
extern int g_kbHasMultiDraw;                              // multi-draw ext availability
extern int g_kbHasBaseVertexExt;                          // single-draw base-vertex ext (kbDrawElementsBV / ?vbarena gate)
// [perf/tr] adjacent-batch transition counters (rb_backend.cpp R_RenderDrawSurfListMaterial)
extern unsigned long g_kbTrBatches, g_kbTrSameMatDiffLight, g_kbTrSameMatDiffTech, g_kbTrDiffMat;
extern int g_kbCtxIsLocal;                                // 1 = worker-local context
extern int g_kbBatchEnable, g_kbCoalesceEnable;           // opt-in perf toggles (ENV)
extern unsigned long g_kbGLCtxHandle;                     // context handle for thread-attach
unsigned long g_kbPresPosted = 0, g_kbPresDropped = 0;   // de-proxy present delivery
unsigned long g_kbYields = 0;                            // render-thread event-loop yields

// On the PROXIED build this is a no-op stub — the DOM thread has its own event loop, so WebGL
// shader/link completions are delivered without the render thread yielding (and emscripten_sleep
// / ASYNCIFY stay out of the proxied wasm entirely). On the DE-PROXY build (-DKB_DEPROXY_BUILD,
// linked with -sASYNCIFY) it restores the B127/B129 fix: the render thread blocks forever
// otherwise, so Chrome never delivers WebGL completions on the worker's never-pumped event loop
// -> useProgram rejected -> nondeterministic in-game black. emscripten_sleep(0) unwinds to the
// event loop once/frame during active link windows; quiet once the map's programs are compiled.
extern "C" void KB_RenderThreadYield() {
#ifdef KB_DEPROXY_BUILD
    // Yield to the worker event loop EVERY frame. Two jobs: (1) deliver Chrome's WebGL
    // shader/link completions (B127); (2) trigger the IMPLICIT OffscreenCanvas present — the
    // browser shows #kbgl's committed frame when the worker returns to its event loop. (2)
    // replaces the old transferToImageBitmap manual present, whose per-frame GPU->CPU readback
    // leaked renderer RAM to ~5GB and OOM-crashed the tab. Must fire every frame (not just during
    // link windows like B129) so the present keeps flowing after the map's shaders compile.
    if (g_kbCtxIsLocal) { ++g_kbYields; emscripten_sleep(0); }
#endif
}
extern "C" void KB_DrawCompFrame();   // ?drawcomp histogram, defined in gl_d3d9_draw.cpp

// ?manualpresent: fall back to the legacy transferToImageBitmap readback present (which leaked
// RAM). Default = implicit OffscreenCanvas present via the per-frame yield (no readback).
static bool kbManualPresent() {
    static int v = -1;
    if (v < 0) { const char *e = getenv("KB_MANUALPRESENT"); v = (e && *e == '1') ? 1 : 0; }  // ENV from index.html (worker can't read location.search)
    return v != 0;
}

namespace {
class EmWebGLContext final : public GLContext {
public:
    bool init(const GLContextDesc &desc) {
        // Loud build marker: lets us confirm the browser is running THIS build (not a
        // cached older one) on every test. Bump the tag each rebuild.
        fprintf(stderr, "\n==== KB BUILD MARKER: H11 (H10 + [perf/tr] batch-transition classifier for the cross-light merge design)  ====\n\n");
        // The page <canvas> has no width/height attributes, so it defaults to 300x150;
        // creating the (offscreen-backed) context on it would render at that size and
        // the CSS stretch to the window makes it badly pixelated. Size the backbuffer
        // to the engine's resolution first, and tell the input layer so mouse coords
        // scale from CSS pixels into this space.
        emscripten_set_canvas_element_size("#canvas", desc.width, desc.height);
        WebInput_SetResolution(desc.width, desc.height);
        cw_ = desc.width; ch_ = desc.height;

        // De-proxy target selection: if the hidden #kbgl canvas was transferred to this
        // worker at thread spawn (KB_DEPROXY builds — win_kernel.cpp), create the context
        // on IT for a LOCAL context. The visible #canvas can't be used: extensions /
        // recorders captureStream() it, which forbids offscreen transfer. Size the
        // OffscreenCanvas directly — we own it on this thread.
        const char *kbSel = "#canvas";
        if (EM_ASM_INT({
                if (typeof GL === 'object' && GL.offscreenCanvases && GL.offscreenCanvases['kbgl']) {
                    var oc = GL.offscreenCanvases['kbgl'].offscreenCanvas || GL.offscreenCanvases['kbgl'].canvas;
                    if (oc) { oc.width = $0; oc.height = $1; return 1; }
                }
                return 0;
            }, desc.width, desc.height))
            kbSel = "#kbgl";

        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;           // WebGL2 == GLES3
        attrs.minorVersion = 0;
        attrs.alpha       = false;
        attrs.depth       = desc.depthStencil;
        attrs.stencil     = desc.depthStencil;
        attrs.antialias   = false;
        // CURATED extensions (enabled below) instead of enable-all, so we control
        // exactly what's on. KHR_parallel_shader_compile IS enabled (B123) — see the
        // kbExts note: its COMPLETION_STATUS query + an immediate in-flight blocking
        // query is the only thing that reliably delivers link results on this worker.
        attrs.enableExtensionsByDefault = false;
        // Worker-owned context: explicit swap + proxy-to-main fallback (see header note).
        attrs.explicitSwapControl       = true;
        attrs.renderViaOffscreenBackBuffer = true;
        attrs.proxyContextToMainThread  = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_FALLBACK;

        // Which mode will we get? If the transferred OffscreenCanvas reached this worker
        // it's in GL.offscreenCanvases and the context below is LOCAL; otherwise the
        // create falls back to PROXY. The transfer has proven flaky run-to-run — this
        // line tells us which happened and what the worker actually received.
        EM_ASM({
            try {
                var keys = (typeof GL === 'object' && GL.offscreenCanvases)
                    ? Object.keys(GL.offscreenCanvases).filter(function (k) { return GL.offscreenCanvases[k]; }) : [];
                var mc = Module['canvas'];
                console.error('[gl] worker canvas probe: offscreenCanvases=[' + keys.join(',') + '] ' +
                              'Module.canvas=' + (mc ? (typeof OffscreenCanvas !== 'undefined' && mc instanceof OffscreenCanvas ? 'OffscreenCanvas' : 'other') : 'null') +
                              ' -> ' + (keys.length ? 'LOCAL context expected' : 'PROXY fallback expected'));
            } catch (e) {}
        });
        ctx_ = emscripten_webgl_create_context(kbSel, &attrs);
        if (ctx_ <= 0) {
            fprintf(stderr, "[gl] emscripten_webgl_create_context(%s) failed: %d\n", kbSel, (int)ctx_);
            // failed:0 with the canvas present = getContext('webgl2') returned null. After
            // a prior WebGL crash Chrome BLOCKLISTS new contexts until a full browser
            // restart — tell the user instead of silently retrying into the same wall.
            fprintf(stderr, "[gl] If a previous run crashed/lost the GL context, Chrome blocks new WebGL "
                            "contexts until the browser is FULLY restarted (check chrome://gpu).\n");
            return false;
        }
        // Loud context-loss sentinel: a lost context makes every subsequent GL call a
        // silent no-op (glCreateShader returns 0 with an empty info log -> "shader
        // compile failed" + black world). Knowing WHEN it dies pins the trigger.
        EM_ASM({
            var c = Module['canvas'];
            if (c && c.addEventListener) {
                c.addEventListener('webglcontextlost', function (e) {
                    console.error('[gl] *** WEBGL CONTEXT LOST *** (render worker canvas)', e && e.statusMessage || '');
                });
            }
        });
        if (emscripten_webgl_make_context_current(ctx_) != EMSCRIPTEN_RESULT_SUCCESS) {
            fprintf(stderr, "[gl] emscripten_webgl_make_context_current failed\n");
            return false;
        }
        // Record the GL-owning thread: gl_resources.cpp defers resource creation/uploads
        // issued from any OTHER thread (loader threads) to bind time on this one.
        g_kbGLThread = pthread_self();
        // Publish the handle so OTHER threads that issue GL (frontend-inline render
        // work / r_smp_backend 0) can attach it to their TLS — see KB_EnsureCtxOnThread.
        g_kbGLCtxHandle = (unsigned long)ctx_;
        // Multi-draw batching support (gl_d3d9_draw.cpp): enable the extension that lets
        // one call submit N draws with varying index ranges AND base vertices.
        {
            // Is this context LOCAL to the worker, or PROXIED to another thread? Direct
            // emscripten_gl* calls (the trustworthy getters + the multi-draw entry point)
            // execute against the worker's GLctx and are ONLY valid when local; on the
            // proxied path everything must ride the proxy-aware dispatch instead.
            // Everything we actually rely on, EXCEPT parallel compile (see attrs note):
            // compressed textures, baseVertex draws, multi-draw, float RTs, aniso.
            {
                static const char *kbExts[] = {
                    "WEBGL_compressed_texture_s3tc",
                    "WEBGL_draw_instanced_base_vertex_base_instance",
                    "WEBGL_multi_draw",
                    "WEBGL_multi_draw_instanced_base_vertex_base_instance",
                    "EXT_color_buffer_float",
                    "OES_texture_float_linear",
                    "EXT_texture_filter_anisotropic",
                    "EXT_float_blend",
                    // RE-ENABLED (B123): parallel compile gives the COMPLETION_STATUS query,
                    // and the B100 recipe — kick a few links/frame + an IMMEDIATE blocking
                    // query while the job is still in-flight — is the ONLY config that
                    // reliably delivered link results on this no-event-loop worker (B100
                    // rendered the whole world). Without it ANGLE still compiles async but
                    // gives no in-flight window to force delivery -> nondeterministic
                    // 'program not valid' / all-draws-skip blackouts.
                    "KHR_parallel_shader_compile",
                };
                for (const char *e : kbExts) emscripten_webgl_enable_extension(ctx_, e);
            }
            g_kbCtxIsLocal = EM_ASM_INT({
                return (typeof GL !== 'undefined' && GL.currentContextIsProxied) ? 0 : 1;
            });
            // Implicit-present mode (default): tell the page to promote the hidden 2x2 #kbgl into
            // the visible full-size canvas. The worker's per-frame event-loop yield then commits
            // its rendered frame directly (no transferToImageBitmap readback, no #display overlay).
            if (g_kbCtxIsLocal && !kbManualPresent()) {
                EM_ASM({ try { postMessage({ kbImplicitPresent: true }); } catch (e) {} });
            }
            g_kbHasMultiDraw = (g_kbCtxIsLocal && emscripten_webgl_enable_extension(
                ctx_, "WEBGL_multi_draw_instanced_base_vertex_base_instance")) ? 1 : 0;
            // Single-draw base-vertex ext: WebGL2 core has NO baseVertex — the plain
            // glDrawElementsBaseVertex is a stub that DROPS basevertex (stubs_web.cpp).
            // kbDrawElementsBV (gl_d3d9_draw.cpp) needs this ext for any non-zero base;
            // ?vbarena is gated on it (its folds make every static draw base-vertex).
            g_kbHasBaseVertexExt = (g_kbCtxIsLocal && emscripten_webgl_enable_extension(
                ctx_, "WEBGL_draw_instanced_base_vertex_base_instance")) ? 1 : 0;
            // Default ON: the post-flicker perf push. The crash that originally
            // benched these was the thread-context-attach bug (fixed), not batching.
            { const char *v = getenv("KB_BATCH");    g_kbBatchEnable    = (v && *v == '0') ? 0 : 1; }
            { const char *v = getenv("KB_COALESCE"); g_kbCoalesceEnable = (v && *v == '0') ? 0 : 1; }
            { const char *v = getenv("KB_PERFMS");   g_kbTimeDraws      = (v && *v == '1') ? 1 : 0; }
            fprintf(stderr, "[gl] ctxLocal=%d multi_draw=%d basevtx=%d batch=%d coalesce=%d\n",
                    g_kbCtxIsLocal, g_kbHasMultiDraw, g_kbHasBaseVertexExt, g_kbBatchEnable, g_kbCoalesceEnable);
        }
        // Canary program for GPU-channel death detection: client-side state (VERSION,
        // getError, isContextLost) stays "alive" when Chrome drops the GPU-process side
        // of this context, while every object query round-trip returns null/false/0 —
        // an UNANNOUNCED loss (the lost event needs this worker's never-pumped event
        // loop). SwapBuffers re-reads this program's LINK_STATUS periodically; a
        // true->false flip is the moment of death.
        EM_ASM({
            try {
                var gl = GLctx;
                var vs = gl.createShader(0x8B31);
                gl.shaderSource(vs, '#version 300 es\nvoid main(){gl_Position=vec4(0.);}');
                gl.compileShader(vs);
                var fs = gl.createShader(0x8B30);
                gl.shaderSource(fs, '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){o=vec4(1.);}');
                gl.compileShader(fs);
                var p = gl.createProgram();
                gl.attachShader(p, vs); gl.attachShader(p, fs); gl.linkProgram(p);
                Module.__kbCanary = p;
            } catch (e) {}
        });

        // GLEW emulation still maps the entry points the engine calls onto the active
        // WebGL2 context; init it so glew* tables are populated on this worker.
        glewExperimental = GL_TRUE;
        GLenum ge = glewInit();
        if (ge != GLEW_OK)
            fprintf(stderr, "[gl] glewInit (webgl worker): %s\n", glewGetErrorString(ge));
        glGetError();

        // One-shot proxy cost report: returning vs void call cost (see SwapBuffers dump).
        double t0 = emscripten_get_now();
        for (int i = 0; i < 100; ++i) glGetError();
        double perGet = (emscripten_get_now() - t0) / 100.0;
        fprintf(stderr, "[gl] WebGL2 ctx=%d; returning GL call = %.4f ms (~0.1=proxied, ~0.001=local)\n",
                (int)ctx_, perGet);
        // Which device/driver actually backs THIS context: on de-proxy builds Chrome can
        // hand the worker's OffscreenCanvas a different backend than the page canvas
        // (e.g. SwiftShader fallback while the page gets the real GPU) with a stricter
        // GLSL compiler — the prime suspect when shaders compile proxied but fail local.
        fprintf(stderr, "[gl] VENDOR='%s' RENDERER='%s' VERSION='%s'\n",
                (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));
        // THE REAL BACKEND: GL_RENDERER above is masked ('WebKit WebGL'). Chrome can hand
        // a worker OffscreenCanvas a SOFTWARE (SwiftShader) context while the page gets
        // the GPU — fingerprints: S3TC=NO + ~0.1ms returning calls + sub-10fps "local"
        // rendering. This line settles hardware-vs-software per run.
        EM_ASM({
            try {
                var gl = GLctx;
                var ext = gl.getExtension('WEBGL_debug_renderer_info');
                console.error('[gl] UNMASKED: ' +
                    (ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) + ' | ' + gl.getParameter(ext.UNMASKED_VENDOR_WEBGL)
                         : (gl.getParameter(gl.RENDERER) + ' (debug_renderer_info unavailable)')));
            } catch (e) { console.error('[gl] UNMASKED probe failed: ' + e); }
        });
        // Uniform-vector limits of THIS context. vs_3_0 shaders use up to 256 const regs
        // (vsc[256]) + our injected uniforms; the GLES3 guaranteed minimum is exactly 256
        // vertex / 224 fragment vectors, so a min-spec compiler rejects the big world
        // shaders ("too many uniforms") while small menu shaders pass — the prime suspect
        // for world-only compile failures on the de-proxied worker context.
        {
            GLint vu = 0, fu = 0;
            glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vu);
            glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &fu);
            fprintf(stderr, "[gl] MAX_VERTEX_UNIFORM_VECTORS=%d MAX_FRAGMENT_UNIFORM_VECTORS=%d\n",
                    (int)vu, (int)fu);
        }
        return true;
    }
    ~EmWebGLContext() override { if (ctx_ > 0) emscripten_webgl_destroy_context(ctx_); }
    void  MakeCurrent() override        { if (ctx_ > 0) emscripten_webgl_make_context_current(ctx_); }
    void  SwapBuffers() override {
        double kbT0 = g_kbTimeDraws ? emscripten_get_now() : 0.0;  // ?perfms=1
        ++g_kbPresentEnter;            // reached present (before commit_frame) this frame
        KB_DrawCompFrame();           // ?drawcomp=1 histogram (no-op unless enabled)
        // Default draw-call readout: every ~120 rendered frames print draws/frame + ms/frame
        // (ms/frame is the honest per-frame cost — call-bound, so draws/frame ~= the bottleneck).
        // Stand in a dense area and read these off. KB_NODRAWLOG=1 silences it.
        {
            static int kbDrawLogOff = -1;
            if (kbDrawLogOff < 0) { const char *e = getenv("KB_NODRAWLOG"); kbDrawLogOff = (e && *e == '1') ? 1 : 0; }
            if (!kbDrawLogOff) {
                static unsigned long s_lastDraws = 0; static int s_fc = 0; static double s_lastT = 0.0;
                if (++s_fc >= 120) {
                    double now = emscripten_get_now();
                    double secs = s_lastT > 0.0 ? (now - s_lastT) / 1000.0 : 0.0;
                    unsigned long d = g_kbDraws - s_lastDraws;
                    fprintf(stderr, "[perf] %.2f ms/frame, %lu draws/frame  (%lu draws / %d frames)\n",
                            secs > 0.0 ? 1000.0 * secs / s_fc : 0.0,
                            d / (unsigned long)s_fc, d, s_fc);
                    s_lastDraws = g_kbDraws; s_lastT = now; s_fc = 0;
                }
            }
        }
        // Diagnostic center-pixel canary (ctrPx in [perf/rb]). A 1px glReadPixels still forces a
        // full GPU pipeline flush + sync round-trip on the proxied context (it waits for the whole
        // deep command queue) — a periodic stall every 30 frames (readPixels = 3.6% in a trace).
        // OFF by default; KB_CANARY=1 re-enables it for render-vs-present-black diagnosis.
        {
            static int kbCanary = -1;
            if (kbCanary < 0) { const char *e = getenv("KB_CANARY"); kbCanary = (e && *e == '1') ? 1 : 0; }
            static int kbPxCtr = 0;
            if (kbCanary && ++kbPxCtr >= 30) {
                kbPxCtr = 0;
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                unsigned char px[4] = {0,0,0,0};
                glReadPixels((GLint)cw_/2, (GLint)ch_/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                kbCtrPx_ = (px[0]<<16)|(px[1]<<8)|px[2];
            }
        }
        // Synchronous lost-context poll: the webglcontextlost EVENT can never fire on this
        // worker (event delivery needs the event loop, which this thread never pumps), but
        // isContextLost() is a plain synchronous query. Prints ONCE at the moment of death
        // -- a lost context otherwise just silently no-ops every GL call (black screen,
        // null getters, empty logs) while the engine keeps running.
        EM_ASM({
            if (typeof GLctx !== 'undefined' && GLctx && GLctx.isContextLost && GLctx.isContextLost()
                && !Module.__kbLostPrinted) {
                Module.__kbLostPrinted = 1;
                console.error('[gl] *** CONTEXT IS LOST (synchronous poll, pres=' + $0 + ') ***');
            }
        }, (int)g_kbPresentEnter);
        emscripten_webgl_commit_frame();
        // DE-PROXY present (KB_DEPROXY builds): when this worker owns the canvas as an
        // OffscreenCanvas, browsers only display its frames implicitly when the worker
        // yields its event loop -- which this render thread (a blocking loop) never does,
        // so the page stays black despite correct rendering (commit_frame is a no-op for
        // OffscreenCanvas; the spec removed commit()). Manually grab the committed frame
        // and ship it zero-copy to the page, which draws it on the #display overlay (see
        // index.html's KB worker patch). No-op on the stock proxied build (Module.canvas
        // is not an OffscreenCanvas on this worker there).
        //
        // BACKPRESSURE (the empty-log shader failures + GPU crashes): each present
        // allocates a full-res ImageBitmap (~8MB); unthrottled at 50fps that is ~400MB/s
        // of GPU-memory churn, and when the page consumes slower than we produce (always,
        // during map load) the pile-up OOMs the GPU process -- ANGLE then fails shader
        // compiles with EMPTY info logs and sometimes kills the tab. Allow exactly ONE
        // frame in flight: the page clears the ack flag (shared heap) after consuming;
        // until then, DROP frames instead of allocating more bitmaps.
        //
        // DEFAULT NOW = IMPLICIT present (the per-frame KB_RenderThreadYield commits #kbgl
        // directly), so this manual readback path is OFF unless ?manualpresent is set. The
        // readback leaked renderer RAM to ~5GB (each transferToImageBitmap copies the frame
        // GPU->CPU and the staging RAM wasn't reclaimed) — the implicit path has no readback.
        if (kbManualPresent()) {
            static int s_frameInFlight = 0;  // 1 = an ImageBitmap is in flight to the page
            // Ack watchdog: if the page stops consuming (a dropped message, a present
            // error before the ack store), the flag wedges at 1 and every later frame
            // is dropped = permanent black while the engine runs. Force-clear after a
            // stuck second and try again.
            static int s_stuckPresents = 0;
            if (s_frameInFlight) { if (++s_stuckPresents > 30) { s_frameInFlight = 0; s_stuckPresents = 0; } }
            else s_stuckPresents = 0;
            int kbPosted = EM_ASM_INT({
                var c = Module['canvas'];
                // Official pthread canvas transfer registers the OffscreenCanvas in
                // GL.offscreenCanvases but leaves Module.canvas unset (the wrapper's
                // moduleCanvasId is '' because an OffscreenCanvas has no .id).
                if (!c && typeof GL === 'object' && GL.offscreenCanvases) {
                    var k = Object.keys(GL.offscreenCanvases).filter(function (n) { return GL.offscreenCanvases[n]; })[0];
                    if (k) c = GL.offscreenCanvases[k].offscreenCanvas || GL.offscreenCanvases[k].canvas;
                }
                if (c && typeof OffscreenCanvas !== 'undefined' && c instanceof OffscreenCanvas) {
                    if (Atomics.load(HEAP32, $0 >> 2)) return 0;   // previous frame unconsumed -> drop
                    Atomics.store(HEAP32, $0 >> 2, 1);
                    try { var b = c.transferToImageBitmap(); postMessage({ kbFrame: b, kbAck: $0 }, [b]); return 1; }
                    catch (e) { Atomics.store(HEAP32, $0 >> 2, 0);
                                if (!Module.__kbPostErr) { Module.__kbPostErr = 1; console.error('[gl] present post failed: ' + e); }
                                return -1; }
                }
                return 2;   // not an OffscreenCanvas (stock proxied build)
            }, &s_frameInFlight);
            if (kbPosted == 1) ++g_kbPresPosted; else if (kbPosted == 0 || kbPosted == -1) ++g_kbPresDropped;
        }
        // GPU-channel canary (every 30 presents): once the canary program has read
        // LINK_STATUS=true, a later false means Chrome dropped the GPU-process side of
        // this context without any detectable loss event — name the exact moment.
        {
            // Every present: the moment the canary flips, dump the GL-op ring — the
            // killing call is in the last few entries.
            int kbDead = EM_ASM_INT({
                try {
                    if (Module.__kbCanary && !Module.__kbCanaryDead) {
                        var ok = GLctx.getProgramParameter(Module.__kbCanary, 0x8B82);
                        if (ok) Module.__kbCanaryOk = 1;
                        else if (Module.__kbCanaryOk) {
                            Module.__kbCanaryDead = 1;
                            console.error('[gl] *** GPU CHANNEL DEAD (canary flipped true->false) pres=' + $0 + ' ***');
                            return 1;
                        }
                    }
                } catch (e) {}
                return 0;
            }, (int)g_kbPresentEnter);
            if (kbDead) KB_DumpOpRing();
        }
        static double kbMsPresent = 0.0;   // ?perfms=1: time in this function per second
        if (kbT0 != 0.0) kbMsPresent += emscripten_get_now() - kbT0;
        // Per-second dump of frame time + the per-frame count of RETURNING GL calls
        // (occlusion polls + event-fence waits), the suspected proxy sync-stall source.
        // One compact [perf] line every 120 frames only — console writes are proxied to
        // the DOM thread per character, so frequent logging itself costs framerate.
        static double t0 = 0; static int frames = 0;
        static unsigned long occl0 = 0, dr0 = 0, rb0 = 0, buf0 = 0;
        double now = emscripten_get_now();
        if (t0 == 0) { t0 = now; occl0 = g_kbOcclGetData; dr0 = g_kbDraws; rb0 = g_kbReadbacks; buf0 = g_kbBufBytes; }
        ++frames;
        double dt = now - t0;
        if (dt >= 1000.0) {   // time-based: also a render-thread heartbeat (stops if RB stalls)
            // Per-FRAME draws/occlusion/readbacks: draws ~50k => culling off; readbk>0 =>
            // a per-frame GPU-sync readback (GetRenderTargetData) stalling every frame.
            static unsigned long bd0 = 0, bf0 = 0, sk0 = 0, bi0 = 0, pl0 = 0, ir0 = 0, is0 = 0;
            static unsigned long fc0[12] = {0}, mg0 = 0;
            // Memory telemetry (tab-crash hunt): worker JS heap + wasm heap size.
            int kbMemMB = EM_ASM_INT({
                try { return (performance && performance.memory) ? (performance.memory.usedJSHeapSize / 1048576) | 0 : -1; }
                catch (e) { return -1; }
            });
            int kbWasmMB = EM_ASM_INT({ try { return (HEAP8.length / 1048576) | 0; } catch (e) { return -1; } });
            // RENDER-BLACK vs PRESENT-BLACK split: kbCtrPx_ is sampled at SwapBuffers
            // entry (before the transfer-clear). Colored while the page is black ->
            // present/display path; black -> the engine rendered black.
            unsigned kbPx = kbCtrPx_;
            static unsigned long pp0 = 0, pd0 = 0, yl0 = 0;
            fprintf(stderr, "[perf/rb] %.1f fps loc=%d jsMB=%d wasmMB=%d ctrPx=%06x yld/f=%lu post/f=%lu drop/f=%lu | draws/f=%lu batched/f=%lu flushes/f=%lu mrg/f=%lu occl/f=%lu bufKB/f=%lu skip/f=%lu bfall/f=%lu links=%lu inst/f=%lu sv/f=%lu\n",
                    1000.0 * frames / dt, g_kbCtxIsLocal, kbMemMB, kbWasmMB, kbPx,
                    (g_kbYields - yl0) / frames,
                    (g_kbPresPosted - pp0) / frames, (g_kbPresDropped - pd0) / frames,
                    (g_kbDraws - dr0) / frames,
                    (g_kbBatchedDraws - bd0) / frames, (g_kbBatchFlushes - bf0) / frames,
                    (g_kbMergeSubmits - mg0) / frames,
                    (g_kbOcclGetData - occl0) / frames,
                    (g_kbBufBytes - buf0) / 1024 / frames,
                    (g_kbSkipPending - sk0) / frames,
                    (g_kbBuiltinFall - bi0) / frames,
                    g_kbProgLinks - pl0,
                    (g_kbInstRuns - ir0) / frames, (g_kbInstSaved - is0) / frames);
            // What interrupted batches, per frame (vc/pc=vs/ps constants, vs/ps=shader
            // binds, tex/smp/rs, vtx=stream+indices+decl, geo=mode-change/full, oth).
            fprintf(stderr, "[perf/fc] vc=%lu pc=%lu vs=%lu ps=%lu tex=%lu smp=%lu rs=%lu vtx=%lu geo=%lu oth=%lu\n",
                    (g_kbFlushCause[0] - fc0[0]) / frames, (g_kbFlushCause[1] - fc0[1]) / frames,
                    (g_kbFlushCause[2] - fc0[2]) / frames, (g_kbFlushCause[3] - fc0[3]) / frames,
                    (g_kbFlushCause[4] - fc0[4]) / frames, (g_kbFlushCause[5] - fc0[5]) / frames,
                    (g_kbFlushCause[6] - fc0[6]) / frames,
                    (g_kbFlushCause[7] - fc0[7] + g_kbFlushCause[8] - fc0[8] + g_kbFlushCause[9] - fc0[9]) / frames,
                    (g_kbFlushCause[10] - fc0[10]) / frames,
                    (g_kbFlushCause[11] - fc0[11]) / frames);
            for (int i = 0; i < 12; ++i) fc0[i] = g_kbFlushCause[i];
            mg0 = g_kbMergeSubmits;
            // Instancing run-break reasons (cumulative): nonMat=a state mutator between copies
            // (broken down by which: tex/rs/stream/decl/other), multiCall=>1 vs-const set,
            // range=single set but >4 regs (maxRange=biggest seen -> how many ?instregs to capture).
            fprintf(stderr, "[perf/brk] nonMat=%lu(tex=%lu rs=%lu strm=%lu decl=%lu oth=%lu) multiCall=%lu range=%lu maxRange=%d | mdrawSameBuf=%lu mdrawDiffBuf=%lu\n",
                    g_kbBrk[0], g_kbBrkCause[4], g_kbBrkCause[5]+g_kbBrkCause[6], g_kbBrkCause[7],
                    g_kbBrkCause[9], g_kbBrkCause[11], g_kbBrk[1], g_kbBrk[2], g_kbBrkMaxRange,
                    g_kbMdrawSameBuf, g_kbMdrawDiffBuf);
            // [perf/tr] per-frame batch-transition classes: how many adjacent material
            // batches could MERGE across a light boundary (same material+technique,
            // only light constants differ) vs need a shader change vs are real
            // material boundaries — sizes the cross-light batch-merge lever.
            {
                static unsigned long tb0 = 0, tl0 = 0, tt0 = 0, tm0 = 0;
                fprintf(stderr, "[perf/tr] batches=%lu mergeable(sameMat+light)=%lu techChange=%lu matChange=%lu\n",
                        (g_kbTrBatches - tb0) / frames, (g_kbTrSameMatDiffLight - tl0) / frames,
                        (g_kbTrSameMatDiffTech - tt0) / frames, (g_kbTrDiffMat - tm0) / frames);
                tb0 = g_kbTrBatches; tl0 = g_kbTrSameMatDiffLight;
                tt0 = g_kbTrSameMatDiffTech; tm0 = g_kbTrDiffMat;
            }
            // ?perfms=1: wall-time split. draw = inside DrawIndexedPrimitive (program
            // setup + GL submission), pres = inside SwapBuffers (commit + bitmap ship),
            // other = engine CPU (drawsurf generation, state-setter work, waits).
            if (g_kbTimeDraws) {
                double fAvg = dt / frames;
                double dAvg = g_kbMsDraw / frames, pAvg = kbMsPresent / frames;
                double bAvg = g_kbMsBuffers / frames;
                // other = frame - draw - pres - buf -> the per-draw setup CPU (state/constants/
                // texture binds + RB command dispatch). buf = dynamic VB/IB uploads.
                fprintf(stderr, "[perf/ms] frame=%.1f draw=%.1f pres=%.1f buf=%.1f other=%.1f\n",
                        fAvg, dAvg, pAvg, bAvg, fAvg - dAvg - pAvg - bAvg);
                g_kbMsDraw = 0.0; kbMsPresent = 0.0; g_kbMsBuffers = 0.0;
            }

            pp0 = g_kbPresPosted; pd0 = g_kbPresDropped; yl0 = g_kbYields;
            bd0 = g_kbBatchedDraws; bf0 = g_kbBatchFlushes;
            sk0 = g_kbSkipPending; bi0 = g_kbBuiltinFall; pl0 = g_kbProgLinks;
            ir0 = g_kbInstRuns; is0 = g_kbInstSaved;
            t0 = now; frames = 0; occl0 = g_kbOcclGetData; dr0 = g_kbDraws; rb0 = g_kbReadbacks; buf0 = g_kbBufBytes;
        }
    }
    void  Resize(int w, int h) override { emscripten_set_canvas_element_size("#canvas", w, h); }
    void *GetProcAddress(const char *n) override { return emscripten_webgl_get_proc_address(n); }
private:
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx_ = 0;
    int cw_ = 0, ch_ = 0;   // backbuffer size (pixel probe sampling point)
    unsigned kbCtrPx_ = 0;  // last sampled center pixel (render-black vs present-black)
};
} // namespace

GLContext *GLContext::Create(const GLContextDesc &desc) {
    auto *c = new EmWebGLContext();
    if (!c->init(desc)) { delete c; return nullptr; }
    return c;
}

#else // !__EMSCRIPTEN_PTHREADS__  — SDL2 path (desktop + single-thread/fiber web)

// No worker event loop to yield to off the pthreads build — render thread runs normally.
extern "C" void KB_RenderThreadYield() {}

namespace {

class SDLGLContext final : public GLContext {
public:
    bool init(const GLContextDesc &desc) {
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[gl] SDL_InitSubSystem(VIDEO): %s\n", SDL_GetError());
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE,     8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,   8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,    8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,   8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,   desc.depthStencil ? 24 : 0);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, desc.depthStencil ? 8  : 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, desc.doubleBuffer ? 1  : 0);

        Uint32 flags = SDL_WINDOW_OPENGL | (desc.visible ? 0u : Uint32(SDL_WINDOW_HIDDEN));
        win_ = SDL_CreateWindow("KisakBlack", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                desc.width, desc.height, flags);
        if (!win_) { fprintf(stderr, "[gl] SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
        // Request raise + keyboard focus so menu/game input (which needs X input
        // focus, unlike the polled mouse position) reaches the window.
        if (desc.visible) SDL_RaiseWindow(win_);

        ctx_ = SDL_GL_CreateContext(win_);
        if (!ctx_) { fprintf(stderr, "[gl] SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }

        glewExperimental = GL_TRUE;
        GLenum ge = glewInit();
        if (ge != GLEW_OK) {
            fprintf(stderr, "[gl] glewInit: %s\n", glewGetErrorString(ge));
            return false;
        }
        glGetError();  // GLEW can leave a benign GL_INVALID_ENUM behind on core profiles.
        return true;
    }

    ~SDLGLContext() override {
        if (ctx_) SDL_GL_DeleteContext(ctx_);
        if (win_) SDL_DestroyWindow(win_);
    }

    void  MakeCurrent() override        { SDL_GL_MakeCurrent(win_, ctx_); }
    void  SwapBuffers() override        { SDL_GL_SwapWindow(win_); }
    void  Resize(int w, int h) override { SDL_SetWindowSize(win_, w, h); }
    void *GetProcAddress(const char *n) override { return SDL_GL_GetProcAddress(n); }

private:
    SDL_Window   *win_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
};

} // namespace

GLContext *GLContext::Create(const GLContextDesc &desc) {
    auto *c = new SDLGLContext();
    if (!c->init(desc)) { delete c; return nullptr; }
    return c;
}

#endif // __EMSCRIPTEN_PTHREADS__
