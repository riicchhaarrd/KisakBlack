// gl_program.cpp — programmable shader path: create/bind shaders, upload the c#
// constant registers, and link+cache the bound (vs, ps) pair into a GL program.
#include "gl_d3d9.h"
#include "gl_shader.h"
#include "gl_optrace.h"
#include "gl_resources.h"

#include <GL/glew.h>
extern "C" void KB_FlushBatchedDraws();  // batched-draw flush (gl_d3d9_draw.cpp)
extern "C" void KB_FlushTagged(int cause); // same, +flush-cause telemetry
extern "C" int  KB_MatArrayLevelC();     // ?matarray level (r_draw_bsp.cpp): 3 = instanced layer

#include <cstdio>
#include <cstring>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>   // EM_ASM (RAWPROG link-failure probe)
#endif

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

// Instancing (gl_d3d9_draw.cpp): the per-object matrix candidate = the vs-const range changed
// since the last draw, tracked here.
extern int g_kbInstEnable, g_kbVscCalls; extern unsigned g_kbVscChangedMin, g_kbVscChangedMax;
extern int g_kbInstActive, g_kbInstMatCount, g_kbInstLocs[8]; extern unsigned g_kbInstMatBase;

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
            if (g_kbInstEnable > 0) {            // record the changed range for instance detection
                if (StartRegister < g_kbVscChangedMin) g_kbVscChangedMin = StartRegister;
                if (StartRegister + Vec4Count - 1 > g_kbVscChangedMax) g_kbVscChangedMax = StartRegister + Vec4Count - 1;
                ++g_kbVscCalls;
            }
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
    // B100 RECIPE (restored B123): with KHR_parallel_shader_compile enabled, force the
    // link to finish with a BLOCKING query IMMEDIATELY after glLinkProgram — while the
    // job is still IN FLIGHT, which is the only window where the result gets delivered
    // on this no-event-loop worker. The caller kicks <=4 links/frame, so this is
    // bounded. This is the config that reliably rendered the whole world; without it
    // delivery is nondeterministic (all-draws-skip blackouts).
    static int parallel = -1;
    if (parallel < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        parallel = (ext && strstr(ext, "parallel_shader_compile")) ? 1 : 0;
    }
    if (parallel) {
        GLint done = 0;
        KB_glGetProgramiv(lp.prog, 0x91B1 /*GL_COMPLETION_STATUS_KHR*/, &done);
        if (!done) {
            GLint attached = 0;
            KB_glGetProgramiv(lp.prog, GL_ATTACHED_SHADERS, &attached);  // BLOCKS while in-flight
        }
    }
#endif
    GLint ok = 0;
    KB_glGetProgramiv(lp.prog, GL_LINK_STATUS, &ok);   // ready now (or native): does not stall
    if (!ok) {
        char log[1024];
        log[0] = 0;
        KB_glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        // STATUS IS UNKNOWABLE on this thread: Chrome's client answers every status
        // query from a cache only the (never-pumped) event loop updates — false +
        // empty log means "result not delivered", NOT "failed". The programs work
        // service-side; B100 rendered the entire world on exactly these stale-false
        // statuses. Treat empty-log as success and USE the program. Only a real
        // error text means a real failure (delete + bounded retry below).
        if (!log[0]) {
            // Empty-log "failure" = link result not yet delivered to Chrome's client
            // (NOT a real failure). DON'T verify with useProgram here — right after
            // glLinkProgram is the worst moment (most in-flight); B116 did that, every
            // program "failed", got deleted+relinked into the same state, and ~all
            // draws skipped (black screen). Trust it and finish setup; the DRAW path
            // does a verified bind later (naturally past delivery) and skips only the
            // draws whose program still hasn't arrived.
            static int staleN = 0;
            if (++staleN <= 3)
                fprintf(stderr, "[gl] link status false+empty (#%d) — trusting; draw-path verifies the bind\n", staleN);
            ok = 1;
        }
    }
    if (!ok) {
        char log[1024];
        log[0] = 0;
        KB_glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        static int linkFailPrints = 0;
        if (++linkFailPrints <= 16)
            fprintf(stderr, "[gl] program link failed (try %d): %s\n", lp.linkTries + 1, log[0] ? log : "(empty log)");
#if defined(__EMSCRIPTEN__)
        // RAW-JS deep probe (one per run, first 3 failures): bypass every wasm layer and
        // ask the browser directly — and link a TRIVIAL control program in the same
        // breath. trivialLS=false -> the context/thread is broken globally;
        // trivialLS=true + real attached-shader logs -> OUR GLSL fails on this driver;
        // empty logs everywhere -> compiles still in flight even after force-finish;
        // ext=false -> COMPLETION_STATUS was an invalid query all along.
        static int progProbes = 0;
        if (progProbes < 3) {
            ++progProbes;
            EM_ASM({
                try {
                    var p = GL.programs[$0];
                    var lost = GLctx.isContextLost();
                    var ext = GLctx.getExtension('KHR_parallel_shader_compile');
                    var ls = GLctx.getProgramParameter(p, 0x8B82);
                    var err = GLctx.getError();
                    var lg = GLctx.getProgramInfoLog(p) || '';
                    var att = GLctx.getAttachedShaders(p);
                    var attS = att ? att.map(function (s) {
                        return GLctx.getShaderParameter(s, 0x8B81) + ':"' + (GLctx.getShaderInfoLog(s) || '') + '"';
                    }).join(' | ') : '?';
                    var vs2 = GLctx.createShader(0x8B31);
                    GLctx.shaderSource(vs2, '#version 300 es\nvoid main(){gl_Position=vec4(0.);}');
                    GLctx.compileShader(vs2);
                    var fs2 = GLctx.createShader(0x8B30);
                    GLctx.shaderSource(fs2, '#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){o=vec4(1.);}');
                    GLctx.compileShader(fs2);
                    var p2 = GLctx.createProgram();
                    GLctx.attachShader(p2, vs2); GLctx.attachShader(p2, fs2);
                    GLctx.linkProgram(p2);
                    var ls2 = GLctx.getProgramParameter(p2, 0x8B82);
                    // finish() forces the whole command stream + compile jobs to complete:
                    // on a LIVE context the trivial program MUST read true after this; on a
                    // LOST one getParameter(VERSION) returns null (the isContextLost() flag
                    // can lie here — its update needs this worker's never-pumped event loop).
                    GLctx.finish();
                    var ls2b = GLctx.getProgramParameter(p2, 0x8B82);
                    var ver = GLctx.getParameter(0x1F02 /*VERSION*/);
                    var err2 = GLctx.getError();
                    GLctx.deleteProgram(p2); GLctx.deleteShader(vs2); GLctx.deleteShader(fs2);
                    console.error('[gl] RAWPROG prog=' + $0 + ' lost=' + lost + ' obj=' + (p ? p.constructor.name : 'null') +
                                  ' ext=' + !!ext + ' LS=' + ls + ' err=0x' + err.toString(16) +
                                  ' log="' + lg + '" att=[' + attS + '] trivialLS=' + ls2 +
                                  ' afterFinish=' + ls2b + ' version=' + (ver === null ? 'NULL(LOST)' : 'ok') +
                                  ' err2=0x' + err2.toString(16));
                } catch (e) { console.error('[gl] RAWPROG threw: ' + e.message); }
            }, (int)lp.prog);
        }
#endif
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
    lp.lmLayerLoc   = KB_glGetUniformLocation(lp.prog, "uLmLayer");   // ?lmarray (-1 if shader has no lightmap array)
    lp.matLayerLoc  = KB_glGetUniformLocation(lp.prog, "uMatLayer");  // ?matarray (-1 if not a mat-array variant)
#endif
    for (int i = 0; i < kMaxStages; ++i) {
        char name[4]; snprintf(name, sizeof(name), "s%d", i);
        lp.samplerLoc[i] = KB_glGetUniformLocation(lp.prog, name);
    }
    // Sampler->unit mapping (s0->0, ...) is set on the FIRST verified bind in the draw
    // path (see below) — NOT here. Binding right after link can be rejected on this
    // driver (result undelivered), which would leave the uniforms unset; doing it at
    // the first clean bind guarantees the program is actually current.
    lp.ready = true;
    return true;
}

// Pick the draw program: the linked (vs,ps) pair if both are bound and valid,
// otherwise the built-in pre-transformed program. Sets the program's uniforms.
// Returns false if the (vs,ps) program is still linking — caller must skip the draw.
bool GLDevice::useDrawProgram() {
    // Resolve any staged blend render-states once here — the single choke point every draw
    // path funnels through (builtin + programmable + instanced + batched). See commitBlendState.
    commitBlendState();
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
    // Instanced draw (flushInstanceRun): use the variant whose per-object matrix reads from
    // instanced attributes. Distinct GL shader id => the program cache links it as its own entry.
    unsigned vsN = g_kbInstActive ? vs_->glShaderInstanced(g_kbInstMatBase, g_kbInstMatCount, g_kbInstLocs)
                                  : vs_->glShader();
    // ?matarray: when a bucketed draw is active, use the variant whose masked stages sample their
    // bucket arrays. Level 2 = uMatLayer uniform (parity, default). Level 3 = per-instance layer:
    // PS reads vMatLayer (flat varying), VS carries it from an instanced attr at a free decl slot
    // (KB_DrawWorldMulti binds the buffer + sets baseInstance). Distinct shader ids -> the program
    // cache links each combo independently.
    static int s_matLevel = -2;
    if (s_matLevel == -2) s_matLevel = KB_MatArrayLevelC();
    bool matInstanced = false;
    kbMatLayerLoc_ = -1;
    if (kbMatArrayMask_ && s_matLevel >= 3 && !g_kbInstActive) {   // (?inst owns vsN when active)
        bool used[16] = {};
        if (decl_) for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int l = GLAttribLocation(e.Usage, e.UsageIndex); if (l >= 0 && l < 16) used[l] = true;
        }
        int loc = -1;
        for (int l = 15; l >= 0; --l) if (!used[l]) { loc = l; break; }
        if (loc >= 0) {
            unsigned vsL = vs_->glShaderMatLayer(loc);
            if (vsL) { vsN = vsL; kbMatLayerLoc_ = loc; matInstanced = true; }
        }
        // No free slot or VS variant failed -> fall through to the level-2 uniform path below.
    }
    unsigned psN = kbMatArrayMask_ ? ps_->glShaderMat(kbMatArrayMask_, shadowMask, matInstanced)
                                   : ps_->glShader(shadowMask);
    if (!vsN || !psN) {
        // Engine-supplied shaders degraded to the builtin passthrough: world geometry
        // drawn this way lands at garbage clip coords = INVISIBLE for the frame.
        extern unsigned long g_kbBuiltinFall; ++g_kbBuiltinFall;
        bindBuiltinForDraw();
        return true;
    }

    // Per-frame NEW-LINK budget: each kicked link is force-finished immediately
    // (see finalizeProgram — stale results are unreadable on this driver), so this
    // bounds the per-frame stall. Programs over budget skip their draws and get
    // kicked on a later frame.
    extern unsigned long g_kbPresentEnter;
    static unsigned long s_linkPres = ~0ul; static int s_linksThisPres = 0;
    auto linkBudgetOk = [&]() -> bool {
        if (s_linkPres != g_kbPresentEnter) { s_linkPres = g_kbPresentEnter; s_linksThisPres = 0; }
        return s_linksThisPres < 4;
    };

    uint64_t key = (uint64_t(vsN) << 32) | psN;
    auto it = progCache_.find(key);
    if (it == progCache_.end()) {
        if (!linkBudgetOk()) {
            extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
            return false;                   // kick the link on a later frame
        }
        ++s_linksThisPres;
        KB_OpTag("link", vsN, psN, 0);
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
        if (lp.linkTries >= 16 || g_kbPresentEnter - lp.lastFailPres < 10 || !linkBudgetOk()) {
            extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
            return false;
        }
        ++s_linksThisPres;
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vsN);
        glAttachShader(prog, psN);
        GLBindAttribLocations(prog);
        glLinkProgram(prog);
        lp.prog = prog; lp.ready = false; lp.pendPolls = 0; lp.bindOk = false;
        extern unsigned long g_kbProgLinks; ++g_kbProgLinks;
    }
    if (!lp.ready && !finalizeProgram(lp)) {
        extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
        return false;                        // still compiling this frame -> skip the draw
    }

    if (curProgram_ != lp.prog) {
#if defined(__EMSCRIPTEN__)
        if (!lp.bindOk) {
            // Verified bind: until Chrome's client has the link result, glUseProgram is
            // rejected (0x502). Skip the draw (invisible) rather than render with the
            // previously-bound program (garbage); keep the program and retry next frame.
            // The getError round-trip runs only until the first clean bind per program.
            while (glGetError() != GL_NO_ERROR) {}
            glUseProgram(lp.prog);
            if (glGetError() != GL_NO_ERROR) {
                curProgram_ = 0;            // not actually bound
                extern unsigned long g_kbSkipPending; ++g_kbSkipPending;
                return false;
            }
            lp.bindOk = true;
            curProgram_ = lp.prog;
            // First clean bind: set the sampler->unit mapping (constant per program).
            for (int i = 0; i < kMaxStages; ++i)
                if (lp.samplerLoc[i] >= 0) glUniform1i(lp.samplerLoc[i], i);
        } else {
            glUseProgram(lp.prog); curProgram_ = lp.prog;
        }
#else
        glUseProgram(lp.prog); curProgram_ = lp.prog;
#endif
    }
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

    // ?lmarray: the lightmap is a sampler2DArray on unit 12 (s12), bound once below; the per-surface
    // 2D lightmap bind on that unit must be skipped (sampler-type mismatch would invalidate the draw).
#if defined(__EMSCRIPTEN__)
    extern unsigned KB_LmArrayMask();
    static int s_lmA12 = -1;
    if (s_lmA12 < 0) s_lmA12 = (KB_LmArrayMask() >> 12) & 1;
    const bool lmArray12 = s_lmA12 && kbLmArrayTex_;
#else
    const bool lmArray12 = false;
#endif

    // Bind each referenced sampler s# to texture unit # and the matching texture.
    // Uses the cached location (queried once at link) — no per-draw glGetUniformLocation.
    for (int i = 0; i < kMaxStages; ++i) {
        int loc = lp.samplerLoc[i];
        if (loc < 0) continue;
        if (lmArray12 && i == 12) continue;   // ?lmarray: unit 12 is the lightmap array (bound below)
        if ((kbMatArrayMask_ >> i) & 1) continue;   // ?matarray: this stage is a bucket array (bound below)
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
#if defined(__EMSCRIPTEN__)
    if (lmArray12 && lp.samplerLoc[12] >= 0) {
        glActiveTexture(GL_TEXTURE0 + 12);
        glBindTexture(GL_TEXTURE_2D_ARRAY, kbLmArrayTex_);
        unitTex_[12] = 0;                       // array bind: invalidate the 2D per-unit cache here
        if (lp.lmLayerLoc >= 0 && lp.upLmLayer != kbLmLayer_) {
            glUniform1f(lp.lmLayerLoc, kbLmLayer_);
            lp.upLmLayer = kbLmLayer_;
        }
    }
    // ?matarray stage 2b: bind each masked stage's bucket array + set the per-draw layer.
    if (kbMatArrayMask_) {
        for (int i = 0; i < kMaxStages; ++i) {
            if (!((kbMatArrayMask_ >> i) & 1) || lp.samplerLoc[i] < 0 || !kbMatStageTex_[i]) continue;
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D_ARRAY, kbMatStageTex_[i]);
            unitTex_[i] = 0;                     // array bind: invalidate the 2D per-unit cache
        }
        if (lp.matLayerLoc >= 0 && lp.upMatLayer != kbMatLayer_) {
            glUniform1f(lp.matLayerLoc, kbMatLayer_);
            lp.upMatLayer = kbMatLayer_;
        }
    }
#endif
    return true;
}
