// glcontext_sdl.cpp — SDL2 implementation of GLContext.
//
// SDL2 gives us one window/GL-context/input path across Linux, macOS and
// Windows-GL, so this single file is the entire window-system dependency of the
// GL backend. GLEW is initialised here so the rest of src/gfx_gl can call modern
// GL entry points directly.
#include "glcontext.h"

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

// Defined in src/platform/sdl/sdl_events.cpp — tells the HTML5 input layer the engine
// backbuffer size so it can scale mouse coordinates from CSS pixels.
extern "C" void WebInput_SetResolution(int w, int h);

namespace {
class EmWebGLContext final : public GLContext {
public:
    bool init(const GLContextDesc &desc) {
        // The page <canvas> has no width/height attributes, so it defaults to 300x150;
        // creating the (offscreen-backed) context on it would render at that size and
        // the CSS stretch to the window makes it badly pixelated. Size the backbuffer
        // to the engine's resolution first, and tell the input layer so mouse coords
        // scale from CSS pixels into this space.
        emscripten_set_canvas_element_size("#canvas", desc.width, desc.height);
        WebInput_SetResolution(desc.width, desc.height);

        EmscriptenWebGLContextAttributes attrs;
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.majorVersion = 2;           // WebGL2 == GLES3
        attrs.minorVersion = 0;
        attrs.alpha       = false;
        attrs.depth       = desc.depthStencil;
        attrs.stencil     = desc.depthStencil;
        attrs.antialias   = false;
        attrs.enableExtensionsByDefault = true;
        // Worker-owned context: explicit swap + proxy-to-main fallback (see header note).
        attrs.explicitSwapControl       = true;
        attrs.renderViaOffscreenBackBuffer = true;
        attrs.proxyContextToMainThread  = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_FALLBACK;

        ctx_ = emscripten_webgl_create_context("#canvas", &attrs);
        if (ctx_ <= 0) {
            fprintf(stderr, "[gl] emscripten_webgl_create_context(#canvas) failed: %d\n", (int)ctx_);
            return false;
        }
        if (emscripten_webgl_make_context_current(ctx_) != EMSCRIPTEN_RESULT_SUCCESS) {
            fprintf(stderr, "[gl] emscripten_webgl_make_context_current failed\n");
            return false;
        }
        // GLEW emulation still maps the entry points the engine calls onto the active
        // WebGL2 context; init it so glew* tables are populated on this worker.
        glewExperimental = GL_TRUE;
        GLenum ge = glewInit();
        if (ge != GLEW_OK)
            fprintf(stderr, "[gl] glewInit (webgl worker): %s\n", glewGetErrorString(ge));
        glGetError();

        // Proxy self-test: glGetError returns a value, so on a PROXIED context every
        // call must synchronously round-trip to the DOM thread (slow); on a worker-
        // local (transferred OffscreenCanvas) context it is microseconds. This tells
        // us up-front whether the canvas transfer to this render worker succeeded.
        double t0 = emscripten_get_now();
        for (int i = 0; i < 200; ++i) glGetError();
        double per = (emscripten_get_now() - t0) / 200.0;
        fprintf(stderr, "[gl] WebGL2 ctx=%d on render worker; glGetError %.4f ms/call -> %s\n",
                (int)ctx_, per, per > 0.02 ? "PROXIED to DOM thread (slow)" : "worker-LOCAL (fast)");
        return true;
    }
    ~EmWebGLContext() override { if (ctx_ > 0) emscripten_webgl_destroy_context(ctx_); }
    void  MakeCurrent() override        { if (ctx_ > 0) emscripten_webgl_make_context_current(ctx_); }
    void  SwapBuffers() override        { emscripten_webgl_commit_frame(); }
    void  Resize(int w, int h) override { emscripten_set_canvas_element_size("#canvas", w, h); }
    void *GetProcAddress(const char *n) override { return emscripten_webgl_get_proc_address(n); }
private:
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx_ = 0;
};
} // namespace

GLContext *GLContext::Create(const GLContextDesc &desc) {
    auto *c = new EmWebGLContext();
    if (!c->init(desc)) { delete c; return nullptr; }
    return c;
}

#else // !__EMSCRIPTEN_PTHREADS__  — SDL2 path (desktop + single-thread/fiber web)

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
