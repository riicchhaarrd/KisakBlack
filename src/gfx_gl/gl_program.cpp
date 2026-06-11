// gl_program.cpp — programmable shader path: create/bind shaders, upload the c#
// constant registers, and link+cache the bound (vs, ps) pair into a GL program.
#include "gl_d3d9.h"
#include "gl_shader.h"
#include "gl_resources.h"

#include <GL/glew.h>
extern "C" void KB_FlushBatchedDraws();  // batched-draw flush (gl_d3d9_draw.cpp)
extern "C" void KB_FlushTagged(int cause); // same, +flush-cause telemetry

#include <cstdio>
#include <cstring>

HRESULT WINAPI GLDevice::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLVertexShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLPixelShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexShader(IDirect3DVertexShader9 *pShader) {
    GLVertexShader *vs = static_cast<GLVertexShader *>(pShader);
    if (vs_ == vs) return D3D_OK;   // no-change fast path (engine re-sets per drawSurf)
    KB_FlushTagged(2);
    vs_ = vs;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShader(IDirect3DPixelShader9 *pShader) {
    GLPixelShader *ps = static_cast<GLPixelShader *>(pShader);
    if (ps_ == ps) return D3D_OK;   // no-change fast path
    KB_FlushTagged(3);
    ps_ = ps;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    // No-change fast path: the engine re-sets identical constants around most draws.
    // Flushing+bumping only on REAL changes keeps draw batches alive (flushes/f used
    // to equal draws/f) and lets useDrawProgram skip the per-draw uniform uploads.
    if (pData && StartRegister + Vec4Count <= 256) {
        float *dst = vsConst_ + StartRegister * 4;
        size_t bytes = Vec4Count * 4 * sizeof(float);
        if (std::memcmp(dst, pData, bytes) != 0) {
            KB_FlushTagged(0);   // pending draws read the OLD values
            std::memcpy(dst, pData, bytes);
            if (vsDirtyMin_ > vsDirtyMax_) vsDirtyBaseVer_ = vsVer_;  // span was empty
            if (StartRegister < vsDirtyMin_) vsDirtyMin_ = StartRegister;
            if (StartRegister + Vec4Count - 1 > vsDirtyMax_) vsDirtyMax_ = StartRegister + Vec4Count - 1;
            ++vsVer_;
        }
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    if (pData && StartRegister + Vec4Count <= 256) {
        float *dst = psConst_ + StartRegister * 4;
        size_t bytes = Vec4Count * 4 * sizeof(float);
        if (std::memcmp(dst, pData, bytes) != 0) {
            KB_FlushTagged(1);
            std::memcpy(dst, pData, bytes);
            if (psDirtyMin_ > psDirtyMax_) psDirtyBaseVer_ = psVer_;  // span was empty
            if (StartRegister < psDirtyMin_) psDirtyMin_ = StartRegister;
            if (StartRegister + Vec4Count - 1 > psDirtyMax_) psDirtyMax_ = StartRegister + Vec4Count - 1;
            ++psVer_;
        }
    }
    return D3D_OK;
}

// Poll the async link for completion (NON-blocking on web via KHR_parallel_shader_compile)
// and, once linked, cache all uniform locations. Returns false while still compiling.
// Blocking on glGetProgramiv(GL_LINK_STATUS) here would serialize ~120 synchronous shader
// compiles on the DOM thread on the first 3D frame -> multi-second "page unresponsive".
#if defined(__EMSCRIPTEN__)
// Direct (non-GLEW, non-funcptr) getters — see the note in gl_shader.cpp compileGL.
// The COMPLETION_STATUS read below gates EVERY draw: if a mangled getter leaves `done`
// at 0 forever, every program reads "still compiling" and the world draws nothing.
extern "C" void emscripten_glGetProgramiv(unsigned program, unsigned pname, int *params);
extern "C" void emscripten_glGetProgramInfoLog(unsigned program, int maxLength, int *length, char *infoLog);
extern "C" int  emscripten_glGetUniformLocation(unsigned program, const char *name);
extern "C" void emscripten_glGetActiveUniform(unsigned program, unsigned index, int bufSize,
                                              int *length, int *size, unsigned *type, char *name);
extern int g_kbCtxIsLocal;   // 1 = direct calls legal; 0 = proxied, use GLEW dispatch
static inline void KB_glGetProgramiv(unsigned p, unsigned pn, int *o) {
    if (g_kbCtxIsLocal) emscripten_glGetProgramiv(p, pn, o); else glGetProgramiv(p, pn, o);
}
static inline void KB_glGetProgramInfoLog(unsigned p, int n, int *len, char *log) {
    if (g_kbCtxIsLocal) emscripten_glGetProgramInfoLog(p, n, len, log); else glGetProgramInfoLog(p, n, len, log);
}
static inline int KB_glGetUniformLocation(unsigned p, const char *nm) {
    return g_kbCtxIsLocal ? emscripten_glGetUniformLocation(p, nm) : glGetUniformLocation(p, nm);
}
static inline void KB_glGetActiveUniform(unsigned p, unsigned i, int bs, int *len, int *sz, unsigned *ty, char *nm) {
    if (g_kbCtxIsLocal) emscripten_glGetActiveUniform(p, i, bs, len, sz, ty, nm);
    else                glGetActiveUniform(p, i, bs, len, sz, (GLenum *)ty, nm);
}
#else
#define KB_glGetProgramiv        glGetProgramiv
#define KB_glGetProgramInfoLog   glGetProgramInfoLog
#define KB_glGetUniformLocation  glGetUniformLocation
#define KB_glGetActiveUniform    glGetActiveUniform
#endif

bool GLDevice::finalizeProgram(LinkedProgram &lp) {
#if defined(__EMSCRIPTEN__)
    static int parallel = -1;               // -1 unknown, 0 unsupported, 1 supported
    if (parallel < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        parallel = (ext && strstr(ext, "parallel_shader_compile")) ? 1 : 0;
    }
    if (parallel) {
        GLint done = 0;
        KB_glGetProgramiv(lp.prog, 0x91B1 /*GL_COMPLETION_STATUS_KHR*/, &done);
        if (!done) {
            // The async completion signal may require the worker's event loop, which this
            // render thread never pumps — COMPLETION_STATUS can then read false FOREVER
            // and every program stays "pending" = nothing draws (the in-game black).
            // Give the async path a bounded window (cheap polls), then FORCE the link to
            // finish: any non-status query is REQUIRED to block until completion via the
            // synchronous GPU-process path, no event loop needed.
            if (++lp.pendPolls < 60)
                return false;               // still compiling -> caller skips the draw
            // Spread forced finishes across frames: 160 blocking links in one frame
            // slammed the GPU process into the empty-log compile-failure state.
            extern unsigned long g_kbPresentEnter;
            static unsigned long s_blockPres = ~0ul; static int s_blocksThisPres = 0;
            if (s_blockPres != g_kbPresentEnter) { s_blockPres = g_kbPresentEnter; s_blocksThisPres = 0; }
            if (s_blocksThisPres >= 4) return false;   // finish the rest next frame
            ++s_blocksThisPres;
            GLint attached = 0;
            KB_glGetProgramiv(lp.prog, GL_ATTACHED_SHADERS, &attached);  // BLOCKS until linked
        }
    }
#endif
    GLint ok = 0;
    KB_glGetProgramiv(lp.prog, GL_LINK_STATUS, &ok);   // ready now (or native): does not stall
    if (!ok) {
        char log[1024];
        log[0] = 0;
        KB_glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        static int linkFailPrints = 0;
        if (++linkFailPrints <= 16)
            fprintf(stderr, "[gl] program link failed (try %d): %s\n", lp.linkTries + 1, log[0] ? log : "(empty log)");
        // Self-heal: marking this "ready" made its materials permanently black
        // (useProgram on an invalid program — the 256x 'program not valid' spam).
        // Delete and re-link on a later frame instead; the caller skips the draw.
        extern unsigned long g_kbPresentEnter;
        glDeleteProgram(lp.prog);
        lp.prog = 0;
        lp.pendPolls = 0;
        ++lp.linkTries;
        lp.lastFailPres = g_kbPresentEnter;
        return false;
    }
    lp.vscLoc = KB_glGetUniformLocation(lp.prog, "vsc");
    lp.pscLoc = KB_glGetUniformLocation(lp.prog, "psc");
    // How many vec4s the constant arrays actually declare (the translator now sizes them
    // to the shader's highest referenced register, not a blanket 256). Upload only these
    // per draw — the per-draw 256+256 vec4 upload was the dominant web draw-call cost.
    auto arraySize = [&](int loc, const char *want) -> int {
        if (loc < 0) return 0;
        GLint nu = 0; KB_glGetProgramiv(lp.prog, GL_ACTIVE_UNIFORMS, &nu);
        size_t wl = strlen(want);
        for (int i = 0; i < nu; ++i) {
            char nm[80]; GLint sz = 0; GLenum ty = 0; GLsizei len = 0;
            nm[0] = 0;
            KB_glGetActiveUniform(lp.prog, i, sizeof(nm), &len, &sz, &ty, nm);
            if (!strncmp(nm, want, wl) && (nm[wl] == '[' || nm[wl] == '\0')) return sz;
        }
        return 0;
    };
    lp.vscCount = arraySize(lp.vscLoc, "vsc");
    lp.pscCount = arraySize(lp.pscLoc, "psc");
#ifdef __EMSCRIPTEN__
    lp.alphaFuncLoc = KB_glGetUniformLocation(lp.prog, "uAlphaTestFunc");
    lp.alphaRefLoc  = KB_glGetUniformLocation(lp.prog, "uAlphaRef");
#endif
    for (int i = 0; i < kMaxStages; ++i) {
        char name[4]; snprintf(name, sizeof(name), "s%d", i);
        lp.samplerLoc[i] = KB_glGetUniformLocation(lp.prog, name);
    }
    // The sampler->unit mapping (s0->0, s1->1, ...) is constant per program, so bind it ONCE
    // here instead of every draw — removes up to kMaxStages marshaled glUniform1i calls/draw.
    glUseProgram(lp.prog); curProgram_ = lp.prog;
    for (int i = 0; i < kMaxStages; ++i)
        if (lp.samplerLoc[i] >= 0) glUniform1i(lp.samplerLoc[i], i);
    lp.ready = true;
    return true;
}

// Pick the draw program: the linked (vs,ps) pair if both are bound and valid,
// otherwise the built-in pre-transformed program. Sets the program's uniforms.
// Returns false if the (vs,ps) program is still linking — caller must skip the draw.
bool GLDevice::useDrawProgram() {
    if (!(vs_ && ps_ && vs_->ok() && ps_->ok())) {
        bindBuiltinForDraw();
        return true;
    }

    // glShader() compiles lazily; a GLSL compile REJECTION leaves it 0. Never attach a
    // 0 shader — on a local (de-proxied) context the TypeError is synchronous and kills
    // the render worker mid-frame (the proxied build only "tolerated" this because the
    // exception landed on the DOM thread). Draw with the builtin instead, like !ok().
    // Shadow-sampler mask: stages with a DEPTH texture bound need the pixel shader's
    // matching samplers typed sampler2DShadow (depth-compare). glShader(mask) returns a
    // cached per-mask variant; the program cache keys on shader ids so variants link
    // independently.
    // NO sampler retyping: the GLSL dump of the real shaders proved Black Ops does
    // MANUAL PCF — plain texture(s2, xy) taps reading raw depth + in-shader compares.
    // sampler2DShadow variants broke every tap (no vec2 overload) and the draws fell
    // to the builtin = black ground. Depth textures sample as regular sampler2D with
    // COMPARE_MODE NONE, returning depth in .x — exactly what the shaders expect.
    unsigned shadowMask = 0;
    (void)0;
    {
        static bool once = false;
        if (shadowMask && !once) {
            once = true;
            fprintf(stderr, "[gl] shadow sampler variant active (stage mask=0x%x)\n", shadowMask);
        }
    }
    unsigned vsN = vs_->glShader(), psN = ps_->glShader(shadowMask);
    if (!vsN || !psN) {
        // Engine-supplied shaders degraded to the builtin passthrough: world geometry
        // drawn this way lands at garbage clip coords = INVISIBLE for the frame.
        extern unsigned long g_kbBuiltinFall; ++g_kbBuiltinFall;
        bindBuiltinForDraw();
        return true;
    }

    uint64_t key = (uint64_t(vsN) << 32) | psN;
    auto it = progCache_.find(key);
    if (it == progCache_.end()) {
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vsN);
        glAttachShader(prog, psN);
        // Match the device's canonical attribute locations (see gl_shader.cpp).
        GLBindAttribLocations(prog);
        glLinkProgram(prog);                // kicks off the (async on web) link
        LinkedProgram lp; lp.prog = prog;   // ready=false; finalized once the link completes
        extern unsigned long g_kbProgLinks;
        ++g_kbProgLinks;
        it = progCache_.emplace(key, lp).first;
    }

    LinkedProgram &lp = it->second;
    if (!lp.prog) {
        // A failed link was deleted for retry. Cooldown + bounded attempts: a real
        // (deterministic) link error stops after 8 tries; a transient GPU-process
        // failure heals on a later frame instead of leaving materials black forever.
        extern unsigned long g_kbPresentEnter;
        if (lp.linkTries >= 8 || g_kbPresentEnter - lp.lastFailPres < 30) {
            extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
            return false;
        }
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vsN);
        glAttachShader(prog, psN);
        GLBindAttribLocations(prog);
        glLinkProgram(prog);
        lp.prog = prog; lp.ready = false; lp.pendPolls = 0;
        extern unsigned long g_kbProgLinks; ++g_kbProgLinks;
    }
    if (!lp.ready && !finalizeProgram(lp)) {
        extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
        return false;                        // still compiling this frame -> skip the draw
    }

    if (curProgram_ != lp.prog) { glUseProgram(lp.prog); curProgram_ = lp.prog; }
    if (lp.vscLoc >= 0 && lp.vscCount > 0 && lp.upVsVer != vsVer_) {
#if defined(__EMSCRIPTEN__)
        // Dirty-range upload: same program redrawing after a small constant change
        // (per-model matrices — thousands of times per frame) re-sends only the touched
        // span, not all 256 vec4s. Emscripten assigns array elements sequential uniform
        // location ids, so vscLoc+reg addresses vsc[reg] directly. Programs that missed
        // older changes (upVer predates the span) still get the full array.
        if (lp.upVsVer >= vsDirtyBaseVer_ && vsDirtyMin_ <= vsDirtyMax_) {
            unsigned lo = vsDirtyMin_;
            unsigned hi = vsDirtyMax_ < (unsigned)lp.vscCount - 1 ? vsDirtyMax_ : (unsigned)lp.vscCount - 1;
            if (lo <= hi) glUniform4fv(lp.vscLoc + lo, hi - lo + 1, vsConst_ + lo * 4);
        } else
#endif
            glUniform4fv(lp.vscLoc, lp.vscCount, vsConst_);
        lp.upVsVer = vsVer_;
    }
    if (lp.pscLoc >= 0 && lp.pscCount > 0 && lp.upPsVer != psVer_) {
#if defined(__EMSCRIPTEN__)
        if (lp.upPsVer >= psDirtyBaseVer_ && psDirtyMin_ <= psDirtyMax_) {
            unsigned lo = psDirtyMin_;
            unsigned hi = psDirtyMax_ < (unsigned)lp.pscCount - 1 ? psDirtyMax_ : (unsigned)lp.pscCount - 1;
            if (lo <= hi) glUniform4fv(lp.pscLoc + lo, hi - lo + 1, psConst_ + lo * 4);
        } else
#endif
            glUniform4fv(lp.pscLoc, lp.pscCount, psConst_);
        lp.upPsVer = psVer_;
    }
#if defined(__EMSCRIPTEN__)
    // The bound program is current now: restart the dirty span so it tracks only the
    // NEXT draw's delta (otherwise it widens monotonically all frame). Programs whose
    // upVer predates the new base simply take one full upload when next bound.
    if (lp.upVsVer == vsVer_) { vsDirtyMin_ = 256; vsDirtyMax_ = 0; vsDirtyBaseVer_ = vsVer_; }
    if (lp.upPsVer == psVer_) { psDirtyMin_ = 256; psDirtyMax_ = 0; psDirtyBaseVer_ = psVer_; }
#endif

#ifdef __EMSCRIPTEN__
    // Feed the in-shader alpha test. uAlphaTestFunc carries the D3DCMP_* value
    // (1..8) when enabled, 0 when disabled; uAlphaRef is the normalized [0,1] ref.
    // Gated on change: these vary per material batch, not per draw — unconditional
    // re-upload was 2 GL calls on every one of ~10k draws.
    if (lp.alphaFuncLoc >= 0) {
        int af = alphaTestOn_ ? (int)alphaFunc_ : 0;
        if (lp.upAlphaFunc != af) { glUniform1i(lp.alphaFuncLoc, af); lp.upAlphaFunc = af; }
    }
    if (lp.alphaRefLoc >= 0) {
        float ar = (float)alphaRef_ / 255.0f;
        if (lp.upAlphaRef != ar) { glUniform1f(lp.alphaRefLoc, ar); lp.upAlphaRef = ar; }
    }
#endif

    // Bind each referenced sampler s# to texture unit # and the matching texture.
    // Uses the cached location (queried once at link) — no per-draw glGetUniformLocation.
    for (int i = 0; i < kMaxStages; ++i) {
        int loc = lp.samplerLoc[i];
        if (loc < 0) continue;
        // (sampler->unit binding now done once at link, see finalizeProgram)
        if (boundTexName_[i]) {
            // Per-unit cache: rebinding the same texture (the common case inside a
            // material batch) costs two proxied calls per stage per draw for nothing.
            if (unitTex_[i] != boundTexName_[i]) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(boundTexTarget_[i], boundTexName_[i]);
                unitTex_[i] = boundTexName_[i];
                applyStageSampler(i, boundTexTarget_[i]);
            }
        }
    }
    return true;
}
