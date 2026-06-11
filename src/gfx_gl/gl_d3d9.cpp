// gl_d3d9.cpp — OpenGL implementations of IDirect3D9 / IDirect3DDevice9.
#include "gl_d3d9.h"
#include "glcontext.h"
#include "gl_resources.h"
#include "gl_optrace.h"

#include <GL/glew.h>
extern "C" void KB_FlushBatchedDraws();
extern "C" void KB_FlushTagged(int cause); // +flush-cause telemetry  // batched-draw flush (gl_d3d9_draw.cpp)

#include <SDL2/SDL.h>   // adapter display-mode queries (EnumAdapterModes etc.)
#include <cstdio>
#if defined(__EMSCRIPTEN__)
#include <emscripten/html5_webgl.h>  // emscripten_webgl_get_current_context()
extern "C" void glClearDepthf(float);             // GLES/WebGL2 depth-clear
extern "C" void glDepthRangef(float, float);      // GLES/WebGL2 depth-range
#endif

// glClearDepth/glDepthRange take doubles and are desktop-GL only. Under WebGL2 the
// render backend runs on a worker whose GL context is PROXIED to the main thread;
// only the GLES3 core entry points carry proxy wrappers. The desktop double variants
// are stray compat aliases with NO proxy wrapper — they dereference the integer
// context handle and throw "GLctx.<fn> is not a function". Route through the GLES
// *f names (which ARE proxied) on Emscripten; use the native doubles on desktop.
static inline void KB_glClearDepth(double z) {
#if defined(__EMSCRIPTEN__)
    glClearDepthf((float)z);
#else
    glClearDepth(z);
#endif
}
static inline void KB_glDepthRange(double n, double f) {
#if defined(__EMSCRIPTEN__)
    glDepthRangef((float)n, (float)f);
#else
    glDepthRange(n, f);
#endif
}

// Default device caps, shared by GLDevice::GetDeviceCaps and GLD3D9::GetDeviceCaps.
// These advertise an SM3.0-class GPU, which is what the Black Ops renderer expects.
static void FillDefaultCaps(D3DCAPS9 *c) {
    *c = D3DCAPS9{};
    c->DeviceType              = D3DDEVTYPE_HAL;
    c->MaxTextureWidth         = 8192;
    c->MaxTextureHeight        = 8192;
    c->MaxAnisotropy           = 16;
    c->MaxSimultaneousTextures = 8;
    c->MaxTextureBlendStages   = 8;
    c->NumSimultaneousRTs      = 4;
    c->VertexShaderVersion     = 0xFFFE0300;  // vs_3_0
    c->PixelShaderVersion      = 0xFFFF0300;  // ps_3_0
    c->MaxVertexShaderConst    = 256;
    c->MaxPrimitiveCount       = 0x00FFFFFF;
    c->MaxVertexIndex          = 0x00FFFFFF;
    c->MaxStreams              = 16;
}

// ---------------------------------------------------------------------------
// GLDevice
// ---------------------------------------------------------------------------
GLDevice::GLDevice(GLContext *ctx, int width, int height)
    : ctx_(ctx), fbWidth_(width), fbHeight_(height), bbWidth_(width), bbHeight_(height) {}

GLDevice::~GLDevice() {
    if (swapChain_)   swapChain_->Release();   // swap chain references the back buffer; drop it first
    if (backBuffer_)  backBuffer_->Release();
    if (builtinProg_) glDeleteProgram(builtinProg_);
    if (vao_)         glDeleteVertexArrays(1, &vao_);
    if (fbo_)         glDeleteFramebuffers(1, &fbo_);
    if (fboDepth_)    glDeleteRenderbuffers(1, &fboDepth_);
    delete ctx_;
}

HRESULT WINAPI GLDevice::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
    KB_FlushTagged(11);
    KB_OpTag("setRT", (unsigned)(uintptr_t)pRenderTarget, 0, 0);
    if (RenderTargetIndex != 0) return D3D_OK;  // single render target for now (MRT: TODO)
    GLSurface *s = static_cast<GLSurface *>(pRenderTarget);
    // A null target, or the back-buffer surface itself, means the default framebuffer.
    if (!s || s->isBackbuffer()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fboActive_ = false;
        dsLive_ = false;
        fbWidth_ = bbWidth_; fbHeight_ = bbHeight_;
        return D3D_OK;
    }
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    fboActive_ = true;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->texName(), s->level());

    int w = (int)s->width(), h = (int)s->height();
    // Honor an engine-set depth-stencil surface backed by a DEPTH TEXTURE (the shadow
    // map): attach it so the shadow pass renders depth into sampleable storage. The
    // attach point depends on whether the format carries stencil.
    if (curDS_ && curDS_->texName() &&
        curDS_->width() == (UINT)w && curDS_->height() == (UINT)h) {
        // The DS texture's storage decides the attach point. Depth-tag surfaces always
        // carry DEPTH24_STENCIL8 (the engine's metrics format arrives as garbage, see
        // gl_resources.cpp); texture-backed depth surfaces keep the format-derived choice.
        unsigned attach = curDS_->texIsDepthStencil() ? GL_DEPTH_STENCIL_ATTACHMENT
                          : (curDS_->format() == (D3DFORMAT)80 /*D16*/ ||
                             curDS_->format() == (D3DFORMAT)70 /*D16_LOCKABLE*/ ||
                             curDS_->format() == (D3DFORMAT)71 /*D32*/)
                                ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
        // Clear the OTHER attach point first (a stale renderbuffer on DEPTH_STENCIL
        // while we attach DEPTH leaves the FBO incomplete).
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, curDS_->texName(), 0);
        // An incomplete FBO silently no-ops EVERY draw of the pass (the B59 black-region
        // regression: a color texture attached as depth no-op'd the MAIN scene). Never
        // leave a broken FBO live — detach and fall back to the auto renderbuffer.
        // glCheckFramebufferStatus is a SYNC round-trip on the proxied context; with
        // ~9 DS binds/frame that is real frame time. Skip it for pairs already known
        // good (attachments are immutable storage; completeness cannot regress).
        unsigned long long pairKey = ((unsigned long long)s->texName() << 32) | curDS_->texName();
        bool known = false;
        for (int pi = 0; pi < fboOkN_; ++pi) if (fboOkPairs_[pi] == pairKey) { known = true; break; }
        unsigned st = known ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER);
        bool dsAttached = (st == GL_FRAMEBUFFER_COMPLETE);
        if (dsAttached && !known && fboOkN_ < 8) fboOkPairs_[fboOkN_++] = pairKey;
        if (!dsAttached)
            glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, 0, 0);
        {
            static unsigned lastStatus = 0xFFFFFFFFu;
            if (st != lastStatus) {
                lastStatus = st;
                fprintf(stderr, "[gl] DS-attach FBO status=0x%x (%s) ds=%ux%u rt=%dx%d\n",
                        st, dsAttached ? "COMPLETE" : "INCOMPLETE->auto-renderbuffer for this pass",
                        curDS_->width(), curDS_->height(), w, h);
            }
        }
        if (dsAttached) {
            extern unsigned long g_kbShadowFbo; ++g_kbShadowFbo;
            dsLive_ = true; fbWidth_ = w; fbHeight_ = h; return D3D_OK;
        }
    }
    dsLive_ = false;
    {
        // Auto depth-stencil renderbuffer sized to the colour target (the default path).
        if (fboDepthW_ != w || fboDepthH_ != h) {
            if (!fboDepth_) glGenRenderbuffers(1, &fboDepth_);
            glBindRenderbuffer(GL_RENDERBUFFER, fboDepth_);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            fboDepthW_ = w; fboDepthH_ = h;
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
    }

    fbWidth_ = w; fbHeight_ = h;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
    KB_FlushTagged(11);
    KB_OpTag("setDS", (unsigned)(uintptr_t)pNewZStencil, 0, 0);
    GLSurface *ds = static_cast<GLSurface *>(pNewZStencil);
    { extern unsigned long g_kbSetDS, g_kbSetDSTex;
      ++g_kbSetDS; if (ds && ds->texName()) ++g_kbSetDSTex; }
    // Only depth-TEXTURE-backed surfaces are honored (texName != 0, i.e. a view onto a
    // depth-format GLTexture). Plain metadata depth-stencil handles keep the auto
    // renderbuffer behavior. NULL restores the auto path.
    curDS_ = (KB_ShadowsEnabled() && ds && ds->texName()) ? ds : nullptr;
    // The shadowmap image's CreateTexture flags don't reliably mark it depth (engine
    // flag soup + garbage formats from the decompiled metrics). The point of truth is
    // HERE: anything used as a depth-stencil surface gets real depth storage.
    if (curDS_ && curDS_->ownerTex() && !curDS_->ownerTex()->isDepth())
        curDS_->ownerTex()->ensureDepthStorage();
    {
        static bool once = false;
        if (curDS_ && !once) {
            once = true;
            fprintf(stderr, "[gl] SetDepthStencilSurface: depth texture honored (%ux%u fmt=%u)\n",
                    curDS_->width(), curDS_->height(), (unsigned)curDS_->format());
        }
    }
    if (fboActive_) {
        // Re-apply on the live FBO immediately (engine may set DS after the RT) — but
        // only when the DS size matches the live RT (WebGL2 requires equal dimensions;
        // D3D9 allowed DS >= RT, e.g. the 1080p main DS during a 256x256 UI3D pass).
        if (curDS_ && curDS_->width() == (UINT)fbWidth_ && curDS_->height() == (UINT)fbHeight_) {
            unsigned attach = curDS_->texIsDepthStencil() ? GL_DEPTH_STENCIL_ATTACHMENT
                              : (curDS_->format() == (D3DFORMAT)80 || curDS_->format() == (D3DFORMAT)70 ||
                                 curDS_->format() == (D3DFORMAT)71)
                                    ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, curDS_->texName(), 0);
            dsLive_ = true;
            { extern unsigned long g_kbShadowFbo; ++g_kbShadowFbo; }
            // Never leave a broken FBO live: incomplete -> auto renderbuffer for THIS
            // pass (keep curDS_; it may match a later render target).
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                dsLive_ = false;
                glFramebufferTexture2D(GL_FRAMEBUFFER, attach, GL_TEXTURE_2D, 0, 0);
                if (fboDepth_)
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
                static bool once = false;
                if (!once) { once = true; fprintf(stderr, "[gl] DS re-apply left FBO incomplete -> auto renderbuffer this pass\n"); }
            }
        } else if (fboDepth_) {
            dsLive_ = false;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fboDepth_);
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::Reset(D3DPRESENT_PARAMETERS *pp) {
    if (pp && pp->BackBufferWidth && pp->BackBufferHeight) {
        fbWidth_  = (int)pp->BackBufferWidth;
        fbHeight_ = (int)pp->BackBufferHeight;
        if (ctx_) ctx_->Resize(fbWidth_, fbHeight_);
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::Present(const RECT *, const RECT *, HWND, const RGNDATA *) {
    KB_FlushTagged(11);
    if (ctx_) ctx_->SwapBuffers();
    return D3D_OK;
}

HRESULT WINAPI GLDevice::Clear(DWORD /*Count*/, const D3DRECT * /*pRects*/, DWORD Flags,
                               D3DCOLOR Color, float Z, DWORD Stencil) {
    KB_FlushTagged(11);
    GLbitfield mask = 0;
    if (Flags & D3DCLEAR_TARGET) {
        const float inv = 1.0f / 255.0f;
        glClearColor(((Color >> 16) & 0xff) * inv,   // R
                     ((Color >>  8) & 0xff) * inv,   // G
                     ((Color      ) & 0xff) * inv,   // B
                     ((Color >> 24) & 0xff) * inv);  // A
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (Flags & D3DCLEAR_ZBUFFER)  { KB_glClearDepth(Z);       mask |= GL_DEPTH_BUFFER_BIT; }
    if (Flags & D3DCLEAR_STENCIL)  { glClearStencil((GLint)Stencil); mask |= GL_STENCIL_BUFFER_BIT; }

    // D3D's Clear ignores scissor (when no rects) and the write masks; GL's does
    // not. Force the affected state for the clear, then restore.
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (mask & GL_COLOR_BUFFER_BIT) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (mask & GL_DEPTH_BUFFER_BIT) glDepthMask(GL_TRUE);

    glClear(mask);

    if (scissor) glEnable(GL_SCISSOR_TEST);
    // Clear forced colorMask/depthMask open and did NOT restore D3D's values, so the GL
    // state now diverges from the SetRenderState cache — invalidate those entries so the
    // next SetRenderState re-applies them (otherwise the masks stay stuck full-open).
    if (mask & GL_COLOR_BUFFER_BIT) rsSet_[D3DRS_COLORWRITEENABLE] = 0;
    if (mask & GL_DEPTH_BUFFER_BIT) rsSet_[D3DRS_ZWRITEENABLE]     = 0;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetViewport(const D3DVIEWPORT9 *vp) {
    KB_FlushTagged(11);
    if (!vp) return E_INVALIDARG;
    // WINDOW target: D3D viewport origin is top-left, GL is bottom-left — flip Y.
    // FBO target: keep D3D placement. The vertex path already flips clip-space Y, so
    // CONTENT orientation matches D3D either way; the viewport flip only RELOCATES
    // sub-rects, which broke every render target sampled with D3D-convention coords
    // through a non-screen-space projection — the tiled sun-shadow atlas (its two
    // vertical cascade tiles landed in swapped halves -> angle/distance-dependent
    // black viewmodel + flapping world sun factors). Full-target viewports are
    // unaffected (the flip is identity at vp.Y=0, Height=target height).
    // D3D placement ONLY for the shadow-atlas build (live honored DS on a target that
    // is not backbuffer-sized): its tiles are sampled later with D3D-convention coords.
    // Everything else (scene incl. scissored sub-passes, postfx chains) keeps the
    // legacy flip those paths were built against.
    bool shadowBuild = fboActive_ && dsLive_ && (fbWidth_ != bbWidth_ || fbHeight_ != bbHeight_);
    GLint y = shadowBuild ? (GLint)vp->Y : fbHeight_ - (GLint)(vp->Y + vp->Height);
    glViewport((GLint)vp->X, y, (GLsizei)vp->Width, (GLsizei)vp->Height);
    KB_glDepthRange(vp->MinZ, vp->MaxZ);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::GetDeviceCaps(D3DCAPS9 *pCaps) {
    if (!pCaps) return E_INVALIDARG;
    FillDefaultCaps(pCaps);
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// GLD3D9 (factory)
// ---------------------------------------------------------------------------
HRESULT WINAPI GLD3D9::GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
    if (!pIdentifier) return E_INVALIDARG;
    *pIdentifier = D3DADAPTER_IDENTIFIER9{};
    // GL_VENDOR/GL_RENDERER need a current context. R_ChooseAdapter() queries this
    // BEFORE R_CreateGameWindow() creates the context, so there may be none yet.
    // On desktop glGetString() returns null with no context; under Emscripten the
    // JS shim instead THROWS (GLctx is undefined), so only query when one is current.
    const GLubyte *renderer = nullptr;
    const GLubyte *vendor   = nullptr;
#if defined(__EMSCRIPTEN__)
    if (emscripten_webgl_get_current_context() > 0)
#endif
    {
        renderer = glGetString(GL_RENDERER);
        vendor   = glGetString(GL_VENDOR);
    }
    snprintf(pIdentifier->Description, sizeof(pIdentifier->Description), "%s",
             renderer ? (const char *)renderer : "OpenGL Renderer");
    snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver), "%s",
             vendor ? (const char *)vendor : "OpenGL");
    return D3D_OK;
}

HRESULT WINAPI GLD3D9::GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *pMode) {
    if (!pMode) return E_INVALIDARG;
    SDL_DisplayMode dm;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        pMode->Width = (UINT)dm.w; pMode->Height = (UINT)dm.h;
        pMode->RefreshRate = (UINT)dm.refresh_rate;
    } else {
        pMode->Width = 1920; pMode->Height = 1080; pMode->RefreshRate = 60;
    }
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

UINT WINAPI GLD3D9::GetAdapterModeCount(UINT, D3DFORMAT) {
    int n = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetNumDisplayModes(0) : 0;
    return n > 0 ? (UINT)n : 1;  // always offer at least the fallback desktop mode
}

HRESULT WINAPI GLD3D9::EnumAdapterModes(UINT, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
    if (!pMode) return E_INVALIDARG;
    SDL_DisplayMode dm;
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDisplayMode(0, (int)Mode, &dm) == 0) {
        pMode->Width = (UINT)dm.w; pMode->Height = (UINT)dm.h;
        pMode->RefreshRate = (UINT)dm.refresh_rate;
    } else {
        if (Mode != 0) return D3DERR_INVALIDCALL;
        pMode->Width = 1920; pMode->Height = 1080; pMode->RefreshRate = 60;
    }
    pMode->Format = Format;
    return D3D_OK;
}

HRESULT WINAPI GLD3D9::GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS9 *pCaps) {
    if (!pCaps) return E_INVALIDARG;
    FillDefaultCaps(pCaps);  // same caps the device reports
    return D3D_OK;
}

HRESULT WINAPI GLD3D9::CreateDevice(UINT /*Adapter*/, D3DDEVTYPE /*DeviceType*/,
                                    HWND /*hFocusWindow*/, DWORD /*BehaviorFlags*/,
                                    D3DPRESENT_PARAMETERS *pp,
                                    IDirect3DDevice9 **ppReturnedDeviceInterface) {
    if (!pp || !ppReturnedDeviceInterface) return E_INVALIDARG;
    *ppReturnedDeviceInterface = nullptr;

    GLContextDesc desc;
    desc.width        = pp->BackBufferWidth  ? (int)pp->BackBufferWidth  : 640;
    desc.height       = pp->BackBufferHeight ? (int)pp->BackBufferHeight : 480;
    desc.doubleBuffer = true;
    desc.depthStencil = pp->EnableAutoDepthStencil ? true : true;
    desc.visible      = pp->Windowed ? true : true;

    GLContext *ctx = GLContext::Create(desc);
    if (!ctx) { fprintf(stderr, "[gl] CreateDevice: GL context creation failed\n"); return E_FAIL; }

    *ppReturnedDeviceInterface = new GLDevice(ctx, desc.width, desc.height);
    return D3D_OK;
}

// ---------------------------------------------------------------------------
// Library entry point (replaces d3d9.dll's Direct3DCreate9 on non-Windows).
// ---------------------------------------------------------------------------
extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT /*SDKVersion*/) {
    return new GLD3D9();
}
