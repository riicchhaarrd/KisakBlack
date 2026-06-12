// gl_d3d9_draw.cpp — geometry resource creation + the draw path for GLDevice.
//
// The built-in GLSL program here only covers the *pre-transformed* (D3DFVF_XYZRHW /
// D3DDECLUSAGE_POSITIONT) vertex path — i.e. the 2D / fixed-function geometry the
// engine submits in screen space. Programmable 3D shaders go through the DX9
// bytecode→GLSL translator instead (task #5); when a vertex shader is bound this
// built-in is bypassed.
#include "gl_d3d9.h"
#include "gl_resources.h"
#include "gl_format.h"   // D3DToGLFormat (?lmarray lightmap-array build)
#include "gl_shader.h"   // GLAttribLocation / GLAttribName / GLBindAttribLocations

#include <GL/glew.h>
#include <cstdio>
#include <vector>
#include <mutex>
#include <cstring>
#include <unordered_map>
#include <cstdint>

// ---- Batched indexed draws (web only) ---------------------------------------------
// On the proxied web context every GL call is marshaled to the canvas-owning thread, so
// the per-frame cost is dominated by CALL COUNT (~2500-8000 draws/frame). Consecutive
// DrawIndexedPrimitive calls between which NO device state changed are accumulated here
// and submitted as ONE glMultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL call.
// Correctness invariant: EVERY state-mutating entry point (Set*, Clear, Present, blits,
// query Issue, resource Unlock...) calls KB_FlushBatchedDraws() FIRST, so a batch only
// ever contains draws with bitwise-identical device state, and the flush executes them
// BEFORE the mutator's GL change applies. Program setup, uniform upload and vertex-attrib
// setup run once per batch instead of once per draw — a second large saving.
#if defined(__EMSCRIPTEN__)
#include <webgl/webgl2_ext.h>   // glMultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL
#include <emscripten/html5_webgl.h>
#include <emscripten.h>         // emscripten_get_now (?perfms=1 draw timing)

// THE normalize_thread ABORT FIX: emscripten's proxied GL dispatches each call to
// GetOwningThread(emscripten_webgl_get_current_context()) — read from the CALLING
// thread's current-context TLS. The engine sometimes executes render commands on a
// thread that never made the context current (frontend-inline backend work during sync
// points; ALWAYS with r_smp_backend 0): TLS is 0 there, the owner is read from a null
// handle, and the dispatch aborts ("Assertion failed: target_thread"). Attach the
// context to any GL-issuing thread on first use; for a PROXIED context this is legal
// from any thread (it is just TLS + a dispatch token).
unsigned long g_kbGLCtxHandle = 0;   // set by glcontext_sdl.cpp at context creation
extern "C" void KB_EnsureCtxOnThread() {
    static thread_local bool attached = false;
    if (!attached) {
        attached = true;
        if (!emscripten_webgl_get_current_context() && g_kbGLCtxHandle)
            emscripten_webgl_make_context_current((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)g_kbGLCtxHandle);
    }
}
#else
extern "C" void KB_EnsureCtxOnThread() {}
#endif

#if defined(__EMSCRIPTEN__)
int g_kbHasMultiDraw = -1;        // set at context init: 1 = extension available
// WEBGL_draw_instanced_base_vertex_base_instance (set at context init). WebGL2 core has
// NO base-vertex draw: the link-level glDrawElementsBaseVertex is a STUB that silently
// DROPS basevertex (stubs_web.cpp — fine while every base was 0). Any non-zero base
// must ride this extension's instanced entry with instanceCount=1.
int g_kbHasBaseVertexExt = -1;
static inline void kbDrawElementsBV(GLenum mode, GLsizei count, GLenum type,
                                    const void *offset, GLint bv) {
    if (bv != 0 && g_kbHasBaseVertexExt == 1) {
        glDrawElementsInstancedBaseVertexBaseInstanceWEBGL(mode, count, type,
            const_cast<void *>(offset), 1, bv, 0);
        return;
    }
    glDrawElementsBaseVertex(mode, count, type, const_cast<void *>(offset), bv);
}
// 1 = worker-LOCAL WebGL context (direct emscripten_gl* calls legal), 0 = PROXIED
// (calls must go through the proxy-aware GLEW pointers or they hit a stub GLctx).
int g_kbCtxIsLocal = 0;
// Opt-in switches (?batch=1 / ?coalesce=1 on the page URL -> ENV -> here). Default OFF:
// a post-B45 in-game crash (a zeroed runtime global -> null proxy-dispatch target) needs
// isolating, and these two changes shift heap layout/draw timing the most.
int g_kbBatchEnable = 0;
int g_kbCoalesceEnable = 0;
unsigned long g_kbBatchedDraws = 0;   // draws that rode in a batch (appended, no GL calls)
unsigned long g_kbBatchFlushes = 0;   // multi-draw submissions
unsigned long g_kbMergeSubmits = 0;   // flushes that went through the CPU index-merge path

// ?perfms=1: wall-time split of the frame. Costs two performance.now() JS calls per
// draw when enabled (a few ms/frame at 10k draws) — diagnostic runs only.
int    g_kbTimeDraws = 0;
double g_kbMsDraw    = 0.0;   // ms spent inside DrawIndexedPrimitive/DrawPrimitive
double g_kbMsBuffers = 0.0;   // ?perfms=1: ms in RB_UpdateDynamicBuffers (dynamic VB/IB uploads)
namespace {
struct KbDrawTimer {
    double t0;
    KbDrawTimer() : t0(g_kbTimeDraws ? emscripten_get_now() : 0.0) {}
    ~KbDrawTimer() { if (t0 != 0.0) g_kbMsDraw += emscripten_get_now() - t0; }
};
}

namespace {
const int kMaxBatch = 256;
GLenum        s_bMode = 0;
GLenum        s_bType = 0;
GLsizei       s_bCounts[kMaxBatch];
const GLvoid *s_bOffsets[kMaxBatch];
GLsizei       s_bInstCounts[kMaxBatch];
GLint         s_bBaseVerts[kMaxBatch];
GLuint        s_bBaseInst[kMaxBatch];
int           s_bN = 0;
GLIndexBuffer *s_bIb = nullptr;   // the batch's index buffer (its CPU shadow feeds merge-flush)
unsigned     *s_bElemSlot = nullptr;   // current VAO's tracked element binding (merge updates it)
// Merged-index snapshot, built AT APPEND TIME: a DISCARD lock may rewrite the CPU
// shadow while a batch is still pending (the GPU buffer keeps its old contents until
// Unlock, which flushes first — the CPU shadow has no such protection), so indices
// must be captured while the just-issued draw's data is still current. Entry 0 is
// copied lazily when the batch reaches size 2: singleton batches (the common case)
// never pay for the copy.
std::vector<unsigned> s_bMerged;
int           s_bMergedN = 0;     // batch entries snapshotted into s_bMerged so far

void kbMergeAppend(int i) {
    const unsigned char *src = s_bIb->shadowData();
    // ?vbarena: per-draw offsets are GL offsets into the shared chunk; the CPU shadow
    // starts at this buffer's placement, so strip the arena bias before reading.
    size_t off = (size_t)s_bOffsets[i] - (s_bIb->inArena() ? s_bIb->arenaOff() : 0u);
    GLint base = s_bBaseVerts[i];
    GLsizei n = s_bCounts[i];
    if (s_bType == GL_UNSIGNED_SHORT) {
        const unsigned short *p = (const unsigned short *)(src + off);
        for (GLsizei k = 0; k < n; ++k) s_bMerged.push_back((unsigned)(p[k] + base));
    } else {
        const unsigned *p = (const unsigned *)(src + off);
        for (GLsizei k = 0; k < n; ++k) s_bMerged.push_back(p[k] + (unsigned)base);
    }
    ++s_bMergedN;
}
}

// ---- GPU instancing of repeated geometry (?inst) — state declared here (used by KB_FlushTagged
// below) ; flushInstanceRun() defined later. See the ?inst notes near the draw path.
int g_kbInstEnable = -1;          // 0=off 1=detect+per-instance-normal(validate) 2=instanced draw
unsigned g_kbVscChangedMin = 256, g_kbVscChangedMax = 0;   // vs-const range changed since last draw
int      g_kbVscCalls = 0, g_kbNonMatrixDirty = 0;
int      g_kbInstActive = 0, g_kbInstMatCount = 0, g_kbInstLocs[8] = {0};  // up to 8 instanced vsc regs
unsigned g_kbInstMatBase = 0;
GLDevice *g_kbInstDev = nullptr;
unsigned long g_kbInstRuns = 0, g_kbInstSaved = 0;
// Why same-geometry sequences FAIL to extend a run (capture < theoretical). brk[0]=a non-matrix
// mutator fired between copies, brk[1]=>1 vs-const call (matrix + other constants), brk[2]=single
// call but range>4. brkCause = which mutator caused the non-matrix break (4=tex 5/6=rs 7=stream
// 8=idx 9=decl 11=other). brkMaxRange = largest single-call vs-const range seen (would ?instregs
// need to be that big to capture matrix+lighting?).
int g_kbLastDirtyCause = 0;
unsigned long g_kbBrk[3] = {0}, g_kbBrkCause[12] = {0}; int g_kbBrkMaxRange = 0;
// Diagnostic: among instanceable (matOnly) draws that DON'T continue an identical-geometry run,
// how many share the previous draw's vertex/index BUFFER (different index range only = a
// multiDraw+baseInstance target) vs a different buffer (irreducible separate draw)? Decides
// whether the multiDraw per-object-matrix path is worth building.
unsigned long g_kbMdrawSameBuf = 0, g_kbMdrawDiffBuf = 0;
namespace {
struct InstGeom {
    uintptr_t decl, vb, ib; unsigned start, prim, baseVert, off, stride;
    bool operator==(const InstGeom &o) const {
        return decl==o.decl && vb==o.vb && ib==o.ib && start==o.start && prim==o.prim
            && baseVert==o.baseVert && off==o.off && stride==o.stride;
    }
};
int      s_iN = 0;
InstGeom s_iGeom;
unsigned s_iMatBase = 0; int s_iMatCount = 0;
GLenum   s_iMode = GL_TRIANGLES; GLsizei s_iVerts = 0; GLenum s_iIdxType = GL_UNSIGNED_SHORT;
const void *s_iOffset = nullptr;
std::vector<float> s_iMatrices;
// Loose-geometry multiDraw: draws that share the run head's BUFFER but use a different index
// range extend the run anyway; per-draw geometry is recorded and the run flushes as ONE
// glMultiDrawElementsInstancedBaseVertexBaseInstance with instanceCount=1 + baseInstance=i, so
// each sub-draw reads matrix[i] from the same instanced-attribute buffer (no UBO, no extra
// shader variant — reuses glShaderInstanced). Lets the per-object matrix stop breaking batches
// for objects packed in a shared vertex buffer.
std::vector<GLsizei>       s_iVertsArr;     // per-draw index count
std::vector<const GLvoid*> s_iOffsArr;      // per-draw index offset
std::vector<GLint>         s_iBaseVertArr;  // per-draw base vertex
bool s_iVarying = false;                    // geometry differs across the run -> multiDraw path
bool s_haveLast = false; InstGeom s_lastGeom; unsigned s_lastMatBase = 0; int s_lastMatCount = 0;
unsigned s_instVbo = 0;
} // namespace
int g_kbMdraw = -1;   // loose-geometry multiDraw matrix path: 1=on (default), 0=?nomdraw
// ?vbarena (gl_resources.cpp): arena-resident static buffers normalize their bind to
// (chunk identity, folded offset) so switching between co-resident models changes no GL
// vertex state — no batch flush, shared VAO, per-draw baseVertex carries the placement.
// s_kbStream0Fold is the vertex-count fold for the CURRENT stream-0 bind, applied to
// every draw's BaseVertexIndex. Valid only for single-stream draws (baseVertex shifts
// EVERY stream's fetch); multi-stream draws un-fold at draw time (DrawIndexedPrimitive).
static size_t   s_kbStreamIdent[4] = {0};
static size_t   s_kbIbIdent = 0;
static unsigned s_kbStream0Fold = 0;
extern "C" int KB_VbArenaEnabled();   // gl_resources.cpp
// Same vertex/index BUFFER + format as the run head (different index range allowed).
static inline bool kbLooseGeom(const InstGeom &a, const InstGeom &b) {
    return a.decl == b.decl && a.vb == b.vb && a.ib == b.ib && a.off == b.off && a.stride == b.stride;
}
static inline void kbInstResetTrack() {
    g_kbVscChangedMin = 256; g_kbVscChangedMax = 0; g_kbVscCalls = 0; g_kbNonMatrixDirty = 0;
}

extern "C" void KB_FlushBatchedDraws() {
    KB_EnsureCtxOnThread();
    if (!s_bN) return;
    ++g_kbBatchFlushes;
    if (s_bN == 1) {
        kbDrawElementsBV(s_bMode, s_bCounts[0], s_bType, s_bOffsets[0], s_bBaseVerts[0]);
    } else if (g_kbHasMultiDraw == 1) {
        glMultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL(
            s_bMode, s_bCounts, s_bType, s_bOffsets, s_bInstCounts, s_bBaseVerts, s_bBaseInst, s_bN);
    } else if (s_bMode == GL_TRIANGLES && s_bMergedN == s_bN) {
        // CPU INDEX MERGE: on the proxied context every GL call is marshaled to the
        // DOM thread, so at ~16k draws/frame the calls themselves ARE the frame time.
        // The multi-draw extension only exists on local contexts, but the per-draw
        // baseVertex was baked into s_bMerged at append time: submit the whole batch
        // as ONE glDrawElements from a scratch 32-bit IB. Millions of CPU index adds
        // cost milliseconds; thousands of proxied calls cost hundreds.
        // TRIANGLES only — merging strips would weld them together.
        ++g_kbMergeSubmits;
        static GLuint s_mergeIbo = 0;
        if (!s_mergeIbo) glGenBuffers(1, &s_mergeIbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_mergeIbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, s_bMerged.size() * sizeof(unsigned),
                     s_bMerged.data(), GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, (GLsizei)s_bMerged.size(), GL_UNSIGNED_INT, nullptr);
        // the scratch bind landed in the current VAO — record it so the next batch
        // start's element-bind gate doesn't skip a needed rebind
        if (s_bElemSlot) *s_bElemSlot = s_mergeIbo;
    } else {
        for (int i = 0; i < s_bN; ++i)
            kbDrawElementsBV(s_bMode, s_bCounts[i], s_bType, s_bOffsets[i], s_bBaseVerts[i]);
    }
    s_bN = 0;
    s_bMergedN = 0;
    s_bMerged.clear();
}

// Flush-cause telemetry: which mutator keeps interrupting batches? Tags only count
// when a batch was actually pending, so the per-cause sum tracks g_kbBatchFlushes.
// 0=vsconst 1=psconst 2=vshader 3=pshader 4=texture 5=sampler 6=renderstate
// 7=stream 8=indices 9=decl 10=mode-change/full 11=other
unsigned long g_kbFlushCause[12] = {0};
extern "C" void KB_FlushTagged(int cause) {
    if (s_bN > 0 && (unsigned)cause < 12u) ++g_kbFlushCause[cause];
    KB_FlushBatchedDraws();
    // Instancing: cause 0 is a vs-constant change (the per-object matrix candidate) and must NOT
    // break an open run; ANY other mutator (texture/stream/state/decl/RT/clear) is a real state
    // change -> mark non-matrix-dirty and emit the run now.
    if (g_kbInstEnable > 0 && cause != 0) {
        g_kbNonMatrixDirty = 1; g_kbLastDirtyCause = cause;
        if (s_iN > 0 && g_kbInstDev) g_kbInstDev->flushInstanceRun();
    }
}
#else
extern "C" void KB_FlushBatchedDraws() {}   // native: draws are immediate, nothing to flush
extern "C" void KB_FlushTagged(int) {}
#endif

// ?worldmerge2: one multi-draw for N single-stream world surfaces from the bound (static) IB, each
// with its own baseVertex (= firstVertex). The engine has bound stream0 = worldVb at offset 0 and the
// static world IB; all N surfaces share that state, so one call replaces N R_DrawIndexedPrimitive
// calls (the kbprof "draw submit" cost). Reuses the same WEBGL multi-draw extension the batch flush
// uses; per-draw baseVertex avoids any uint16 index rebasing.
void GLDevice::KB_DrawWorldMulti(const int *counts, const void *const *offsets, const int *baseVerts, int n) {
    if (!ib_ || n <= 0) return;
#if defined(__EMSCRIPTEN__)
    KB_FlushBatchedDraws();          // close any open batch before our own multi-draw
    KB_EnsureCtxOnThread();
    extern unsigned long g_kbDraws; g_kbDraws += (unsigned long)n;
    if (!useDrawProgram()) return;   // shader still linking -> skip (re-drawn next frame)
    applyVertexState();
    unsigned elem = ib_->glName();
    if (!curVaoEnt_ || curVaoEnt_->elem != elem) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem);
        if (curVaoEnt_) curVaoEnt_->elem = elem;
    }
    GLenum idxType = (ib_->format() == D3DFMT_INDEX16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    // ?vbarena: the engine's inputs are buffer-relative — bias index byte offsets by the
    // IB placement and baseVertex by the stream-0 fold (these world surfs are
    // single-stream by construction, so the fold is valid).
    static std::vector<const void *> kbOffs; static std::vector<int> kbBases;
    unsigned kbIbBias = ib_->inArena() ? ib_->arenaOff() : 0u;
    int kbVBias = (int)s_kbStream0Fold;
    if (kbIbBias || kbVBias) {
        kbOffs.resize(n); kbBases.resize(n);
        for (int i = 0; i < n; ++i) {
            kbOffs[i]  = (const void *)((size_t)offsets[i] + kbIbBias);
            kbBases[i] = baseVerts[i] + kbVBias;
        }
        offsets = kbOffs.data(); baseVerts = kbBases.data();
    }
    if (g_kbHasMultiDraw == 1) {
        static std::vector<GLsizei> inst; static std::vector<GLuint> binst;
        if ((int)inst.size() < n) { inst.assign(n, 1); binst.assign(n, 0); }
        glMultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL(
            GL_TRIANGLES, counts, idxType, offsets, inst.data(), baseVerts, binst.data(), n);
    } else {
        for (int i = 0; i < n; ++i)
            kbDrawElementsBV(GL_TRIANGLES, counts[i], idxType, offsets[i], baseVerts[i]);
    }
#else
    (void)counts; (void)offsets; (void)baseVerts;   // native path unused (engine gates ?worldmerge2 to web)
#endif
}

#if defined(__EMSCRIPTEN__)
// Engine bridge: the renderer holds an IDirect3DDevice9* (always a GLDevice on this backend).
extern "C" void KB_DrawWorldMultiC(void *dev, const int *counts, const void *const *offsets,
                                   const int *baseVerts, int n) {
    static_cast<GLDevice *>(reinterpret_cast<IDirect3DDevice9 *>(dev))
        ->KB_DrawWorldMulti(counts, offsets, baseVerts, n);
}

// ?lmarray: build the lit-world lightmap texture array from the world's per-page lightmap textures.
void GLDevice::KB_BuildLightmapArray(void *const *basemaps, int count) {
    if (kbLmArrayTex_ || count <= 0 || !basemaps) return;   // built once per world
    KB_EnsureCtxOnThread();
    GLTexture *t0 = static_cast<GLTexture *>(reinterpret_cast<IDirect3DBaseTexture9 *>(basemaps[0]));
    if (!t0) return;
    int w = (int)t0->width(), h = (int)t0->height();
    unsigned internal, format, type; int bpp;
    if (!D3DToGLFormat(t0->format(), &internal, &format, &type, &bpp)) return;
    glGenTextures(1, &kbLmArrayTex_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, kbLmArrayTex_);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal, w, h, count, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    for (int i = 0; i < count; ++i)
        if (basemaps[i])
            static_cast<GLTexture *>(reinterpret_cast<IDirect3DBaseTexture9 *>(basemaps[i]))
                ->KB_UploadIntoArrayLayer(kbLmArrayTex_, i);
    unitTex_[12] = 0;   // the per-draw path binds the array to unit 12; clear the 2D cache there
    fprintf(stderr, "[lmarray] built lightmap array %dx%d x %d layers (internal=0x%x)\n",
            w, h, count, internal);
}

void GLDevice::KB_SetLightmapLayer(int layer) { kbLmLayer_ = (float)layer; }

extern "C" void KB_BuildLightmapArrayC(void *dev, void *const *basemaps, int count) {
    static_cast<GLDevice *>(reinterpret_cast<IDirect3DDevice9 *>(dev))->KB_BuildLightmapArray(basemaps, count);
}
extern "C" void KB_SetLightmapLayerC(void *dev, int layer) {
    static_cast<GLDevice *>(reinterpret_cast<IDirect3DDevice9 *>(dev))->KB_SetLightmapLayer(layer);
}
#endif

namespace {

// The built-in 2D program consumes the canonical POSITION / COLOR0 / TEXCOORD0
// attribute names (see GLAttribName in gl_shader.cpp), so the same vertex setup
// drives it and the translated programs.
#if defined(__EMSCRIPTEN__)
const char *kBuiltinVS =
    "#version 300 es\n"
    "in vec4 aPos;\n"               // POSITIONT: x,y in screen pixels, z depth, w=rhw
    "in vec4 aColor0;\n"
    "in vec2 aTexCoord0;\n"
    "uniform vec2 uViewport;\n"
    "out vec4 vColor;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  float x = (aPos.x / uViewport.x) * 2.0 - 1.0;\n"
    "  float y = 1.0 - (aPos.y / uViewport.y) * 2.0;\n"  // D3D top-left -> GL bottom-left
    "  gl_Position = vec4(x, y, aPos.z, 1.0);\n"
    "  vColor = aColor0;\n"
    "  vTexCoord = aTexCoord0;\n"
    "}\n";

const char *kBuiltinFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTexture;\n"
    "in vec4 vColor;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "  vec4 c = vColor;\n"
    "  if (uUseTexture != 0) c *= texture(uTex, vTexCoord);\n"
    "  oColor = c;\n"
    "}\n";
#else
const char *kBuiltinVS =
    "#version 120\n"
    "attribute vec4 aPos;\n"        // POSITIONT: x,y in screen pixels, z depth, w=rhw
    "attribute vec4 aColor0;\n"
    "attribute vec2 aTexCoord0;\n"
    "uniform vec2 uViewport;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "  float x = (aPos.x / uViewport.x) * 2.0 - 1.0;\n"
    "  float y = 1.0 - (aPos.y / uViewport.y) * 2.0;\n"  // D3D top-left -> GL bottom-left
    "  gl_Position = vec4(x, y, aPos.z, 1.0);\n"
    "  vColor = aColor0;\n"
    "  vTexCoord = aTexCoord0;\n"
    "}\n";

const char *kBuiltinFS =
    "#version 120\n"
    "uniform sampler2D uTex;\n"
    "uniform int uUseTexture;\n"
    "uniform int uColorOp;\n"    // 0 = SELECTARG1 (texture only), 1 = MODULATE (tex*diffuse)
    "uniform int uAlphaFunc;\n"  // GL compare func (GL_NEVER..GL_ALWAYS), 0 = alpha test off
    "uniform float uAlphaRef;\n" // reference in [0,1]
    "varying vec4 vColor;\n"
    "varying vec2 vTexCoord;\n"
    "bool alphaPass(float a) {\n"
    "  if (uAlphaFunc == 0) return true;\n"            // disabled
    "  if (uAlphaFunc == 0x0200) return false;\n"      // GL_NEVER
    "  if (uAlphaFunc == 0x0201) return a <  uAlphaRef;\n"  // GL_LESS
    "  if (uAlphaFunc == 0x0202) return a == uAlphaRef;\n"  // GL_EQUAL
    "  if (uAlphaFunc == 0x0203) return a <= uAlphaRef;\n"  // GL_LEQUAL
    "  if (uAlphaFunc == 0x0204) return a >  uAlphaRef;\n"  // GL_GREATER
    "  if (uAlphaFunc == 0x0205) return a != uAlphaRef;\n"  // GL_NOTEQUAL
    "  if (uAlphaFunc == 0x0206) return a >= uAlphaRef;\n"  // GL_GEQUAL
    "  return true;\n"                                  // GL_ALWAYS (0x0207) / default
    "}\n"
    "void main() {\n"
    "  vec4 c = vColor;\n"
    "  if (uUseTexture != 0) {\n"
    "    vec4 t = texture2D(uTex, vTexCoord);\n"
    "    c = (uColorOp == 0) ? t : (t * vColor);\n"  // SELECTARG1(tex) vs MODULATE(tex*diffuse)
    "  }\n"
    "  if (!alphaPass(c.a)) discard;\n"
    "  gl_FragColor = c;\n"
    "}\n";
#endif

unsigned compile(GLenum stage, const char *src) {
    unsigned s = glCreateShader(stage);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = 0;
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        // Empty log + status 0 = the proxied status readback came back unfilled (a
        // phantom; see gl_shader.cpp compileGL) — not a real failure; stay quiet.
        if (log[0])
            fprintf(stderr, "[gl] built-in shader compile failed: %s\n", log);
    }
    return s;
}

// D3DDECLTYPE -> (component count, GL type, normalized). CoD's vertex formats
// pack normals/tangents (UBYTE4N/DEC3N), colours (UBYTE4N) and texcoords
// (FLOAT16_2/4) — decoding any of these as plain floats yields garbage normals
// (dark/flat lighting, half-black triangles) and garbage UVs (skybox moire).
void declType(BYTE t, GLint *size, GLenum *type, GLboolean *norm) {
    *norm = GL_FALSE;
    switch (t) {
        case D3DDECLTYPE_FLOAT1:    *size = 1; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT2:    *size = 2; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT3:    *size = 3; *type = GL_FLOAT;          break;
        case D3DDECLTYPE_FLOAT4:    *size = 4; *type = GL_FLOAT;          break;
#if defined(__EMSCRIPTEN__)
        // WebGL2 forbids GL_BGRA as the attribute size (must be 1..4); the call
        // throws GL_INVALID_VALUE, the attribute is never set up, and the draw then
        // fails "enabled array has no buffer" -> nothing renders. Decode as a plain
        // 4xUBYTE normalized attribute. Caveat: D3DCOLOR is BGRA in memory, so the
        // shader receives R/B swapped (cosmetic; menu colours are mostly white).
        case D3DDECLTYPE_D3DCOLOR:  *size = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;
#else
        case D3DDECLTYPE_D3DCOLOR:  *size = GL_BGRA; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; break;
#endif
        case D3DDECLTYPE_UBYTE4:    *size = 4; *type = GL_UNSIGNED_BYTE;  break;
        case D3DDECLTYPE_UBYTE4N:   *size = 4; *type = GL_UNSIGNED_BYTE;  *norm = GL_TRUE; break;
        case D3DDECLTYPE_SHORT2:    *size = 2; *type = GL_SHORT;          break;
        case D3DDECLTYPE_SHORT4:    *size = 4; *type = GL_SHORT;          break;
        case D3DDECLTYPE_SHORT2N:   *size = 2; *type = GL_SHORT;          *norm = GL_TRUE; break;
        case D3DDECLTYPE_SHORT4N:   *size = 4; *type = GL_SHORT;          *norm = GL_TRUE; break;
        case D3DDECLTYPE_USHORT2N:  *size = 2; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; break;
        case D3DDECLTYPE_USHORT4N:  *size = 4; *type = GL_UNSIGNED_SHORT; *norm = GL_TRUE; break;
        // 3 components packed 10:10:10:2 (low bits = x). REV matches D3D's order.
        case D3DDECLTYPE_UDEC3:     *size = 4; *type = GL_UNSIGNED_INT_2_10_10_10_REV; break;
        case D3DDECLTYPE_DEC3N:     *size = 4; *type = GL_INT_2_10_10_10_REV; *norm = GL_TRUE; break;
        case D3DDECLTYPE_FLOAT16_2: *size = 2; *type = GL_HALF_FLOAT;     break;
        case D3DDECLTYPE_FLOAT16_4: *size = 4; *type = GL_HALF_FLOAT;     break;
        default:                    *size = 4; *type = GL_FLOAT;          break;
    }
}


void primInfo(D3DPRIMITIVETYPE pt, UINT primCount, GLenum *mode, GLsizei *verts) {
    switch (pt) {
        case D3DPT_POINTLIST:     *mode = GL_POINTS;         *verts = primCount;     break;
        case D3DPT_LINELIST:      *mode = GL_LINES;          *verts = primCount * 2; break;
        case D3DPT_LINESTRIP:     *mode = GL_LINE_STRIP;     *verts = primCount + 1; break;
        case D3DPT_TRIANGLELIST:  *mode = GL_TRIANGLES;      *verts = primCount * 3; break;
        case D3DPT_TRIANGLESTRIP: *mode = GL_TRIANGLE_STRIP; *verts = primCount + 2; break;
        case D3DPT_TRIANGLEFAN:   *mode = GL_TRIANGLE_FAN;   *verts = primCount + 2; break;
        default:                  *mode = GL_TRIANGLES;      *verts = primCount * 3; break;
    }
}

} // namespace

// ---- Resource creation ----------------------------------------------------
HRESULT WINAPI GLDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                       D3DFORMAT Format, D3DPOOL Pool,
                                       IDirect3DTexture9 **ppTexture, HANDLE *) {
    if (!ppTexture) return E_INVALIDARG;
    *ppTexture = new GLTexture(this, Width, Height, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                             D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *) {
    if (!ppVolumeTexture) return E_INVALIDARG;
    *ppVolumeTexture = new GLVolumeTexture(this, Width, Height, Depth, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                           D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *) {
    if (!ppCubeTexture) return E_INVALIDARG;
    *ppCubeTexture = new GLCubeTexture(this, EdgeLength, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                            IDirect3DVertexBuffer9 **ppVB, HANDLE *) {
    if (!ppVB) return E_INVALIDARG;
    *ppVB = new GLVertexBuffer(this, Length, Usage, FVF, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                           IDirect3DIndexBuffer9 **ppIB, HANDLE *) {
    if (!ppIB) return E_INVALIDARG;
    *ppIB = new GLIndexBuffer(this, Length, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pElements,
                                                 IDirect3DVertexDeclaration9 **ppDecl) {
    if (!ppDecl || !pElements) return E_INVALIDARG;
    *ppDecl = new GLVertexDeclaration(this, pElements);
    return D3D_OK;
}

// ---- Geometry binding (device holds non-owning refs; the app owns lifetime) -
HRESULT WINAPI GLDevice::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData,
                                         UINT OffsetInBytes, UINT Stride) {
    if (StreamNumber >= 4) return D3D_OK;  // TODO: support >4 streams if needed
    Stream &s = streams_[StreamNumber];
    GLVertexBuffer *vb = static_cast<GLVertexBuffer *>(pStreamData);
#if defined(__EMSCRIPTEN__)
    if (vb && Stride) vb->noteStride(Stride);   // ?vbarena: placement alignment (pre-sync only)
    // Place arena-eligible buffers NOW (first bind), so ident/fold below see the final
    // identity — deferring to draw time would build the first VAO without the arena
    // offset. Same thread the draw-path glName() calls run on; a pending upload landing
    // here is safe (resource Unlock already flushed any open batch).
    if (vb && !vb->inArena() && !vb->isDynamic() && KB_VbArenaEnabled()) {
        KB_EnsureCtxOnThread();
        vb->glName();
    }
    size_t ident = vb ? vb->bindIdent() : 0;
    unsigned effOff = OffsetInBytes, fold = 0;
    if (vb && vb->inArena()) {
        unsigned total = vb->arenaOff() + OffsetInBytes;
        if (StreamNumber == 0 && Stride && (total % Stride) == 0) { fold = total / Stride; effOff = 0; }
        else effOff = total;     // unfoldable: VAO carries the arena offset (still correct)
    }
    // No-change fast path — by GL bind identity, so two arena co-residents don't flush.
    // A pending upload on a DIFFERENT object must take the slow path: the batch-start
    // applyVertexState/glName is what replays it (appends never re-sync).
    if (s_kbStreamIdent[StreamNumber] == ident && s.offset == effOff && s.stride == Stride
        && !(vb && vb != s.vb && vb->pendingUpload())) {
        s.vb = vb;
        if (StreamNumber == 0) s_kbStream0Fold = fold;
        return D3D_OK;
    }
    KB_FlushTagged(7);
    s_kbStreamIdent[StreamNumber] = ident;
    s.vb = vb; s.offset = effOff; s.stride = Stride;
    if (StreamNumber == 0) s_kbStream0Fold = fold;
    return D3D_OK;
#else
    // No-change fast path: the engine re-sets identical bindings around most draws;
    // an unconditional flush here kept draw batches at size 1 (flushes/f == draws/f).
    if (s.vb == vb && s.offset == OffsetInBytes && s.stride == Stride) return D3D_OK;
    KB_FlushTagged(7);
    s.vb = vb; s.offset = OffsetInBytes; s.stride = Stride;
    return D3D_OK;
#endif
}

HRESULT WINAPI GLDevice::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
    GLIndexBuffer *ib = static_cast<GLIndexBuffer *>(pIndexData);
#if defined(__EMSCRIPTEN__)
    if (ib && !ib->inArena() && !ib->isDynamic() && KB_VbArenaEnabled()) {
        KB_EnsureCtxOnThread();                  // place at first bind (see SetStreamSource)
        ib->glName();
    }
    size_t ident = ib ? ib->bindIdent() : 0;
    if (s_kbIbIdent == ident && !(ib && ib != ib_ && ib->pendingUpload())) {
        ib_ = ib;               // same GL identity (arena co-resident or same object)
        return D3D_OK;
    }
    KB_FlushTagged(8);
    s_kbIbIdent = ident;
    ib_ = ib;
    return D3D_OK;
#else
    if (ib_ == ib) return D3D_OK;   // no-change fast path
    KB_FlushTagged(8);
    ib_ = ib;
    return D3D_OK;
#endif
}

HRESULT WINAPI GLDevice::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
    GLVertexDeclaration *d = static_cast<GLVertexDeclaration *>(pDecl);
    if (decl_ == d) return D3D_OK;   // no-change fast path
    KB_FlushTagged(9);
    decl_ = d;
    return D3D_OK;
}

// ---- Draw path ------------------------------------------------------------
void GLDevice::ensureBuiltinProgram() {
    if (builtinProg_) return;
    unsigned vs = compile(GL_VERTEX_SHADER, kBuiltinVS);
    unsigned fs = compile(GL_FRAGMENT_SHADER, kBuiltinFS);
    builtinProg_ = glCreateProgram();
    glAttachShader(builtinProg_, vs);
    glAttachShader(builtinProg_, fs);
    GLBindAttribLocations(builtinProg_);
    glLinkProgram(builtinProg_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    builtinViewportLoc_  = glGetUniformLocation(builtinProg_, "uViewport");
    builtinTexLoc_       = glGetUniformLocation(builtinProg_, "uTex");
    builtinUseTexLoc_    = glGetUniformLocation(builtinProg_, "uUseTexture");
    builtinColorOpLoc_   = glGetUniformLocation(builtinProg_, "uColorOp");
    builtinAlphaFuncLoc_ = glGetUniformLocation(builtinProg_, "uAlphaFunc");
    builtinAlphaRefLoc_  = glGetUniformLocation(builtinProg_, "uAlphaRef");
}

// D3DCMP_* -> the GL compare-func enum (GL_NEVER..GL_ALWAYS). The built-in
// fragment shader emulates the alpha test against these values; 0 means the test
// is disabled.
static GLenum alphaFuncToGL(DWORD d3dCmp) {
    switch (d3dCmp) {
        case D3DCMP_NEVER:        return GL_NEVER;
        case D3DCMP_LESS:         return GL_LESS;
        case D3DCMP_EQUAL:        return GL_EQUAL;
        case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
        case D3DCMP_GREATER:      return GL_GREATER;
        case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
        default:                  return GL_ALWAYS;
    }
}

// Bind the built-in program and set its frame/texture uniforms before a draw.
void GLDevice::bindBuiltinForDraw() {
    ensureBuiltinProgram();
    if (curProgram_ != builtinProg_) { glUseProgram(builtinProg_); curProgram_ = builtinProg_; }
    if (builtinViewportLoc_ >= 0)
        glUniform2f(builtinViewportLoc_, (float)fbWidth_, (float)fbHeight_);
    bool sampling = applyTextures();
    if (builtinTexLoc_ >= 0)    glUniform1i(builtinTexLoc_, 0);
    if (builtinUseTexLoc_ >= 0) glUniform1i(builtinUseTexLoc_, sampling ? 1 : 0);

    // Fixed-function stage-0 combine: SELECTARG1 (texture only) vs MODULATE
    // (texture * diffuse). Both arg slots default to the (TEXTURE, DIFFUSE) pair,
    // which is all the engine's 2D path uses.
    if (builtinColorOpLoc_ >= 0)
        glUniform1i(builtinColorOpLoc_, texStage0_.colorOp == D3DTOP_SELECTARG1 ? 0 : 1);

    // Alpha test (emulated by discard). Pass the GL compare func, or 0 to disable.
    if (builtinAlphaFuncLoc_ >= 0)
        glUniform1i(builtinAlphaFuncLoc_, alphaTest_.enable ? (int)alphaFuncToGL(alphaTest_.func) : 0);
    if (builtinAlphaRefLoc_ >= 0)
        glUniform1f(builtinAlphaRefLoc_, (float)alphaTest_.ref / 255.0f);
}

unsigned g_kbVaoEpoch = 1;   // (legacy; kept for the extern) — selective invalidation below

// Selective VAO-cache invalidation. The old design bumped g_kbVaoEpoch on EVERY buffer/decl
// death and wiped the WHOLE cache, so the streaming system's periodic geometry eviction
// rebuilt every VAO at once next frame = a periodic STUTTER (user-confirmed: ?novao=1, which
// has no cache to wipe, removed it). Instead, record just the dead resource and drop ONLY the
// cache entries that reference it. Destructors (gl_resources.cpp, possibly off-thread) push
// here; applyVertexState drains on the render thread BEFORE any miss-build, so a reused GL
// name can't alias a stale entry (its entries are erased first).
static std::mutex            g_kbDeadMu;
static std::vector<unsigned> g_kbDeadBufs;    // dead VB/IB GL buffer names (one shared namespace)
static std::vector<const void*> g_kbDeadDecls;// dead GLVertexDeclaration addresses
extern "C" void KB_VaoNoteDeadBuf(unsigned name) {
    if (!name) return;
    std::lock_guard<std::mutex> g(g_kbDeadMu); g_kbDeadBufs.push_back(name);
}
extern "C" void KB_VaoNoteDeadDecl(const void *decl) {
    std::lock_guard<std::mutex> g(g_kbDeadMu); g_kbDeadDecls.push_back(decl);
}

// ---- Draw-composition diagnostic (KB_DRAWCOMP=1) ----------------------------------
int g_kbDrawComp = -1;   // -1 = unread env, 0 = off, 1 = on (read lazily in KB_DrawCompFrame)
static std::unordered_map<uint64_t, uint32_t> g_kbDrawCompMap;
void KB_DrawCompTick(uint64_t key) { ++g_kbDrawCompMap[key]; }
// Called once/frame from SwapBuffers: every ~60 frames, summarize how many draws are duplicate
// geometry (instanceable) vs unique (mergeable), then clear for the next frame.
extern "C" void KB_DrawCompFrame() {
    if (g_kbDrawComp < 0) { const char *e = getenv("KB_DRAWCOMP"); g_kbDrawComp = (e && *e == '1') ? 1 : 0; }
    if (!g_kbDrawComp) { return; }
    static int fr = 0;
    if (++fr % 60 == 0 && !g_kbDrawCompMap.empty()) {
        uint32_t total = 0, instanceable = 0, maxdup = 0, dupBuckets = 0;
        for (const auto &kv : g_kbDrawCompMap) {
            total += kv.second;
            if (kv.second > 1) { instanceable += kv.second - 1; ++dupBuckets; }
            if (kv.second > maxdup) maxdup = kv.second;
        }
        fprintf(stderr, "[perf/comp] draws=%u distinct=%zu instanceable=%u (%.0f%%) dupGeoms=%u maxdup=%u\n",
                total, g_kbDrawCompMap.size(), instanceable,
                total ? 100.0 * instanceable / total : 0.0, dupBuckets, maxdup);
    }
    g_kbDrawCompMap.clear();
}

void GLDevice::flushInstanceRun() {
    int n = s_iN; s_iN = 0;                       // close first (re-entrancy safe)
    if (n < 1) return;
    const int mc = s_iMatCount;
    const float *mats = s_iMatrices.data();
    // The run's vertex stream offset was folded into baseVertex (s_iGeom.off==0 for normalized
    // runs), so applyVertexState below must set up the VAO at THIS offset, not the live stream
    // offset (which is the last appended draw's real offset). Override for the duration of the
    // flush; the guard restores it so the breaking draw that follows still sees its real offset.
    unsigned kbSavedOff = streams_[0].offset;
    streams_[0].offset = s_iGeom.off;
    struct KbOffGuard { GLDevice *d; unsigned o; ~KbOffGuard() { d->streams_[0].offset = o; } } kbOffGuard{ this, kbSavedOff };
    auto bindElem = [&]() {
        unsigned elem = ib_ ? ib_->glName() : 0;
        if (!curVaoEnt_ || curVaoEnt_->elem != elem) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem); if (curVaoEnt_) curVaoEnt_->elem = elem;
        }
    };
    // Per-instance NORMAL render — always correct (restores each matrix into vsConst_ and draws
    // the same as if it were never deferred). Used for validate mode, n==1, or any time the
    // instanced path can't apply. This is the safety net: instancing only ever ADDS a fast path.
    auto emitNormal = [&]() {
        for (int i = 0; i < n; ++i) {
            std::memcpy(vsConst_ + s_iMatBase * 4, mats + (size_t)i * mc * 4, (size_t)mc * 4 * sizeof(float));
            if (vsDirtyMin_ > vsDirtyMax_) vsDirtyBaseVer_ = vsVer_;
            if (s_iMatBase < vsDirtyMin_) vsDirtyMin_ = s_iMatBase;
            if (s_iMatBase + mc - 1 > vsDirtyMax_) vsDirtyMax_ = s_iMatBase + mc - 1;
            ++vsVer_;
            if (!useDrawProgram()) continue;
            applyVertexState(); bindElem();
            GLsizei v = s_iVarying ? s_iVertsArr[i]    : s_iVerts;
            const void *o = s_iVarying ? s_iOffsArr[i] : s_iOffset;
            GLint bv = s_iVarying ? s_iBaseVertArr[i]  : (GLint)s_iGeom.baseVert;
            kbDrawElementsBV(s_iMode, v, s_iIdxType, o, bv);
        }
    };

    ++g_kbInstRuns;
    if (n >= 2) g_kbInstSaved += (n - 1);
    extern int g_kbHasMultiDraw;
    if (g_kbInstEnable != 2 || n < 2 || g_kbHasMultiDraw != 1) { emitNormal(); return; }

    // INSTANCED path. Free attribute locations = those the decl doesn't use (prefer high).
    bool used[16] = {};
    if (decl_) for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
        int l = GLAttribLocation(e.Usage, e.UsageIndex); if (l >= 0 && l < 16) used[l] = true;
    }
    int locs[8], got = 0;
    for (int l = 15; l >= 0 && got < mc; --l) if (!used[l]) locs[got++] = l;
    if (got < mc) { emitNormal(); return; }       // no room (16-attrib budget) -> safe fallback

    g_kbInstActive = 1; g_kbInstMatBase = s_iMatBase; g_kbInstMatCount = mc;
    for (int i = 0; i < mc; ++i) g_kbInstLocs[i] = locs[i];
    if (!useDrawProgram()) { g_kbInstActive = 0; emitNormal(); return; }  // instanced variant not ready
    applyVertexState();

    if (!s_instVbo) glGenBuffers(1, &s_instVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_instVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_iMatrices.size() * sizeof(float)), mats, GL_DYNAMIC_DRAW);
    const GLsizei stride = (GLsizei)(mc * 4 * sizeof(float));
    for (int i = 0; i < mc; ++i) {
        glEnableVertexAttribArray(locs[i]);
        glVertexAttribPointer(locs[i], 4, GL_FLOAT, GL_FALSE, stride, (const void *)(size_t)(i * 4 * sizeof(float)));
        glVertexAttribDivisor(locs[i], 1);
    }
    bindElem();
    if (s_iVarying) {
        // Each sub-draw: instanceCount=1, baseInstance=i -> reads matrix[i] from the instanced
        // attribute. Different index ranges in the SAME buffer collapse into one GL call.
        static std::vector<GLsizei> icounts; static std::vector<GLuint> binsts;
        icounts.assign(n, 1); binsts.resize(n);
        for (int i = 0; i < n; ++i) binsts[i] = (GLuint)i;
        glMultiDrawElementsInstancedBaseVertexBaseInstanceWEBGL(
            s_iMode, s_iVertsArr.data(), s_iIdxType, s_iOffsArr.data(),
            icounts.data(), s_iBaseVertArr.data(), binsts.data(), n);
    } else {
        glDrawElementsInstancedBaseVertexBaseInstanceWEBGL(s_iMode, s_iVerts, s_iIdxType,
            const_cast<void *>(s_iOffset), n, (int)s_iGeom.baseVert, 0);
    }
    // Restore the cached VAO: disable the instance attribs (free locations, divisor back to 0).
    for (int i = 0; i < mc; ++i) { glDisableVertexAttribArray(locs[i]); glVertexAttribDivisor(locs[i], 0); }
    g_kbInstActive = 0;
}

#if defined(__EMSCRIPTEN__)
// KB smodel instanced fast path (engine side: r_draw_staticmodel.cpp KB_TryDrawSmodelInstanced).
// Submit N instances of the CURRENTLY BOUND geometry in one call; `mats` carries each
// instance's vec4 rows for vs regs [matBase, matBase+matCount). Reuses the ?inst
// machinery (instanced shader variant, instance-attribute matrix, free-location pick).
// The per-instance loop fallback keeps this strictly an additive fast path.
void GLDevice::KB_DrawXSurfInstanced(unsigned matBase, int matCount, float *mats,
                                     int n, unsigned gapMask, unsigned baseIndex, unsigned triCount) {
    if (!ib_ || n <= 0 || matCount <= 0 || matCount > 8) return;
    KB_EnsureCtxOnThread();
    // Gap rows (span rows with no per-prim arg) are instance-invariant: replicate their
    // CURRENT constant values into every instance's rows. Must happen before the
    // last-instance mirror below reads mats (gap rows arrive uninitialized).
    if (gapMask) {
        for (int r = 0; r < matCount; ++r) {
            if (!(gapMask & (1u << r))) continue;
            const float *cur = vsConst_ + (matBase + r) * 4;
            for (int i = 0; i < n; ++i)
                std::memcpy(mats + ((size_t)i * matCount + r) * 4, cur, 4 * sizeof(float));
        }
    }
    // Route the LAST instance's rows through the normal constant path first: keeps
    // vsConst_/dirty/version bookkeeping truthful for whatever draws next (the
    // instanced variant ignores those uniform regs), and its flush-tag closes any
    // open batch before our submission.
    SetVertexShaderConstantF(matBase, mats + (size_t)(n - 1) * matCount * 4, (UINT)matCount);
    KB_FlushBatchedDraws();
    if (s_iN > 0) flushInstanceRun();        // close any open detected ?inst run
    extern unsigned long g_kbDraws; g_kbDraws += (unsigned long)n;
    // ?vbarena stream-0 fold (smodel surfs are single-stream; un-fold if not)
    INT baseVertex = 0;
    if (s_kbStream0Fold) {
        if (streams_[1].vb || streams_[2].vb || streams_[3].vb) {
            KB_FlushTagged(7);
            streams_[0].offset = s_kbStream0Fold * streams_[0].stride;
            s_kbStream0Fold = 0; s_kbStreamIdent[0] = ~(size_t)0;
        } else {
            baseVertex = (INT)s_kbStream0Fold;
        }
    }
    bool is16 = (ib_->format() == D3DFMT_INDEX16);
    GLenum idxType = is16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    size_t idxSize = is16 ? 2 : 4;
    const void *offset = reinterpret_cast<const void *>(
        (size_t)baseIndex * idxSize + (ib_->inArena() ? ib_->arenaOff() : 0u));
    GLsizei verts = (GLsizei)triCount * 3;
    auto bindElem = [&]() {
        unsigned elem = ib_->glName();
        if (!curVaoEnt_ || curVaoEnt_->elem != elem) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem);
            if (curVaoEnt_) curVaoEnt_->elem = elem;
        }
    };
    auto emitLoop = [&]() {                  // safety net: per-instance normal draws
        for (int i = 0; i < n; ++i) {
            SetVertexShaderConstantF(matBase, mats + (size_t)i * matCount * 4, (UINT)matCount);
            if (!useDrawProgram()) continue;
            applyVertexState();
            bindElem();
            kbDrawElementsBV(GL_TRIANGLES, verts, idxType, offset, baseVertex);
        }
    };
    ++g_kbInstRuns; g_kbInstSaved += (unsigned long)(n - 1);
    if (g_kbHasBaseVertexExt != 1 || n < 2) { emitLoop(); return; }
    bool used[16] = {};
    if (decl_) for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
        int l = GLAttribLocation(e.Usage, e.UsageIndex); if (l >= 0 && l < 16) used[l] = true;
    }
    int locs[8], got = 0;
    for (int l = 15; l >= 0 && got < matCount; --l) if (!used[l]) locs[got++] = l;
    if (got < matCount) { emitLoop(); return; }
    g_kbInstActive = 1; g_kbInstMatBase = matBase; g_kbInstMatCount = matCount;
    for (int i = 0; i < matCount; ++i) g_kbInstLocs[i] = locs[i];
    if (!useDrawProgram()) { g_kbInstActive = 0; emitLoop(); return; }
    applyVertexState();
    if (!s_instVbo) glGenBuffers(1, &s_instVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_instVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)n * matCount * 4 * sizeof(float)),
                 mats, GL_DYNAMIC_DRAW);
    const GLsizei stride = (GLsizei)(matCount * 4 * sizeof(float));
    for (int i = 0; i < matCount; ++i) {
        glEnableVertexAttribArray(locs[i]);
        glVertexAttribPointer(locs[i], 4, GL_FLOAT, GL_FALSE, stride,
                              (const void *)(size_t)(i * 4 * sizeof(float)));
        glVertexAttribDivisor(locs[i], 1);
    }
    bindElem();
    glDrawElementsInstancedBaseVertexBaseInstanceWEBGL(GL_TRIANGLES, verts, idxType,
        const_cast<void *>(offset), n, baseVertex, 0);
    for (int i = 0; i < matCount; ++i) { glDisableVertexAttribArray(locs[i]); glVertexAttribDivisor(locs[i], 0); }
    g_kbInstActive = 0;
}

extern "C" void KB_DrawXSurfInstancedC(void *dev, unsigned matBase, int matCount,
                                       float *mats, int n, unsigned gapMask,
                                       unsigned baseIndex, unsigned triCount) {
    static_cast<GLDevice *>(reinterpret_cast<IDirect3DDevice9 *>(dev))
        ->KB_DrawXSurfInstanced(matBase, matCount, mats, n, gapMask, baseIndex, triCount);
}
#endif

void GLDevice::applyVertexState() {
    // ?novao=1 kill switch: bypass the VAO cache (full attrib re-spec per draw, the
    // pre-B100 path) to bisect geometry corruption vs the cache.
    static int noVao = -1;
    if (noVao < 0) { const char *v = getenv("KB_NOVAO"); noVao = (v && *v == '1') ? 1 : 0; }
#if defined(__EMSCRIPTEN__)
    // KB_DUMPSKIN=1: one-shot dump of any SKINNED vertex decl (has BLENDINDICES). Identifies the
    // M16 viewmodel spazz — prints each element's D3DDECLTYPE so a mis-decoded bone-index/weight
    // format (e.g. indices normalized, or D3DCOLOR BGRA-swizzled) is visible at a glance.
    static int kbDumpSkin = -1;
    if (kbDumpSkin < 0) { const char *v = getenv("KB_DUMPSKIN"); kbDumpSkin = (v && *v == '1') ? 1 : 0; }
    if (kbDumpSkin && decl_) {
        static std::set<const void *> logged;
        bool hasSkin = false;
        for (const D3DVERTEXELEMENT9 &e : decl_->elements())
            if (e.Usage == D3DDECLUSAGE_BLENDINDICES) { hasSkin = true; break; }
        if (hasSkin && logged.insert((const void *)decl_).second) {
            fprintf(stderr, "[kbskin] skinned decl=%p:\n", (const void *)decl_);
            for (const D3DVERTEXELEMENT9 &e : decl_->elements())
                fprintf(stderr, "  stream=%u offset=%u type=%u usage=%u usageIdx=%u\n",
                        e.Stream, e.Offset, e.Type, e.Usage, e.UsageIndex);
        }
    }
#endif
    // A D3DUSAGE_DYNAMIC stream's bound offset changes every draw (the NOOVERWRITE/DISCARD ring),
    // so a per-(buffer,offset) cache entry never hits — it just accumulates throwaway VAOs until
    // the size cap nukes the whole cache = the periodic stutter (user-confirmed: ?novao=1 removed
    // it). Route dynamic draws through the shared-VAO re-spec path (no alloc, no cache churn);
    // only static layouts (stable offset) get cached, where hits actually pay off.
    bool dynamic = false;
    for (int i = 0; i < 4; ++i) if (streams_[i].vb && streams_[i].vb->isDynamic()) { dynamic = true; break; }
    if (noVao || dynamic) {
        // Shared re-spec VAO. The old code disabled all 16 attribs + re-enabled + re-bound the
        // buffer PER ELEMENT every draw (~35 GL calls/dynamic-draw = a top web-tax source). But a
        // dynamic stream almost always reuses the SAME decl draw-to-draw — only the per-draw OFFSET
        // changes. So when the decl is unchanged we skip the disables/enables entirely (the shared
        // VAO's enabled set + formats persist) and only re-point; and we bind each buffer once, not
        // per element. Same-decl dynamic draws drop from ~35 calls to ~(1 bind + N point).
        static const void *s_dynLastDecl = nullptr;
        static unsigned    s_dynEnabledMask = 0;
        bool freshVao = !vao_;
        if (freshVao) { glGenVertexArrays(1, &vao_); s_dynLastDecl = nullptr; s_dynEnabledMask = 0; }
        if (curVao_ != vao_) glBindVertexArray(vao_);
        curVao_ = vao_; curVaoEnt_ = nullptr;   // element-bind gate disabled (no entry)
        if (!decl_) return;
        bool sameDecl = !noVao && s_dynLastDecl == (const void *)decl_;
        unsigned newMask = 0;
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc >= 0 && loc < 16 && e.Stream < 4 && streams_[e.Stream].vb) newMask |= (1u << loc);
        }
        if (!sameDecl) {
            unsigned dis = s_dynEnabledMask & ~newMask;            // were on, no longer needed
            for (int i = 0; i < 16; ++i) if (dis & (1u << i)) glDisableVertexAttribArray(i);
            glVertexAttrib4f(GLAttribLocation(D3DDECLUSAGE_COLOR, 0), 1.0f, 1.0f, 1.0f, 1.0f);
        }
        unsigned boundBuf = ~0u;
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc < 0 || e.Stream >= 4) continue;
            const Stream &s = streams_[e.Stream];
            if (!s.vb) continue;
            GLint size; GLenum type; GLboolean norm;
            declType(e.Type, &size, &type, &norm);
            unsigned bn = s.vb->glName();
            if (bn != boundBuf) { glBindBuffer(GL_ARRAY_BUFFER, bn); boundBuf = bn; }   // once per buffer
            if (!sameDecl || !(s_dynEnabledMask & (1u << loc))) glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, size, type, norm, s.stride,
                                  reinterpret_cast<const void *>(size_t(s.offset + e.Offset)));
        }
        s_dynEnabledMask = newMask; s_dynLastDecl = decl_;
        return;
    }

    // Selective invalidation: drop ONLY cache entries that reference a just-destroyed buffer
    // (VB name in key[1/4/7/10] or the captured element binding) or declaration (key[0]). A
    // reused GL name/decl-addr would otherwise alias a stale VAO. Draining here — before the
    // miss-build below — guarantees a reused name's stale entries are gone before a new entry
    // could be created for it. (Was a wholesale clear on any death = the periodic stutter.)
    {
        std::vector<unsigned>     dBufs;
        std::vector<const void *> dDecls;
        {
            std::lock_guard<std::mutex> g(g_kbDeadMu);
            if (!g_kbDeadBufs.empty())  dBufs.swap(g_kbDeadBufs);
            if (!g_kbDeadDecls.empty()) dDecls.swap(g_kbDeadDecls);
        }
        if (!dBufs.empty() || !dDecls.empty()) {
            bool erasedCur = false;
            for (auto it = vaoCache_.begin(); it != vaoCache_.end(); ) {
                const auto &k = it->first;
                bool dead = false;
                for (const void *d : dDecls) if (k[0] == (unsigned)(uintptr_t)d) { dead = true; break; }
                if (!dead)
                    for (unsigned nm : dBufs)
                        if (k[1]==nm || k[4]==nm || k[7]==nm || k[10]==nm || it->second.elem==nm) { dead = true; break; }
                if (dead) {
                    if (curVaoEnt_ == &it->second) erasedCur = true;
                    glDeleteVertexArrays(1, &it->second.vao);
                    it = vaoCache_.erase(it);
                } else ++it;
            }
            if (erasedCur) {
                // The live VAO (and the merge code's s_bElemSlot pointer into it) was erased.
                curVaoEnt_ = nullptr; curVao_ = 0; s_bElemSlot = nullptr; glBindVertexArray(0);
            }
        }
    }
    // Safety cap (rare): bound cache size to avoid unbounded VAO accumulation.
    if (vaoCache_.size() > 4096) {
        for (auto &kv : vaoCache_) glDeleteVertexArrays(1, &kv.second.vao);
        vaoCache_.clear();
        curVaoEnt_ = nullptr; curVao_ = 0; s_bElemSlot = nullptr;
        glBindVertexArray(0);
    }

    // Cache key: declaration + every stream's (buffer, offset, stride). glName() runs
    // even on hits — pending CPU-side uploads sync at bind time there.
    std::array<unsigned, 13> key{};
    key[0] = (unsigned)(uintptr_t)decl_;
    int n = 1;
    for (int i = 0; i < 4; ++i) {
        const Stream &s = streams_[i];
        key[n++] = s.vb ? s.vb->glName() : 0u;
        key[n++] = (unsigned)s.offset;
        key[n++] = (unsigned)s.stride;
    }
    auto it = vaoCache_.find(key);
    if (it != vaoCache_.end()) {
        if (curVao_ != it->second.vao) { glBindVertexArray(it->second.vao); curVao_ = it->second.vao; }
        curVaoEnt_ = &it->second;
        return;
    }

    // Miss: build a fresh VAO (starts with all arrays disabled — no per-draw disables).
    unsigned vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    curVao_ = vao;
    // D3D's default (unspecified) diffuse colour is white; GL's default generic
    // attribute is (0,0,0,1). Without this, a texcoord-only vertex would multiply
    // the texture by black. Context-global (not VAO state) — re-set per miss is cheap.
    glVertexAttrib4f(GLAttribLocation(D3DDECLUSAGE_COLOR, 0), 1.0f, 1.0f, 1.0f, 1.0f);
    if (decl_) {
        for (const D3DVERTEXELEMENT9 &e : decl_->elements()) {
            int loc = GLAttribLocation(e.Usage, e.UsageIndex);
            if (loc < 0 || e.Stream >= 4) continue;
            const Stream &s = streams_[e.Stream];
            if (!s.vb) continue;

            GLint size; GLenum type; GLboolean norm;
            declType(e.Type, &size, &type, &norm);
            glBindBuffer(GL_ARRAY_BUFFER, s.vb->glName());
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, size, type, norm, s.stride,
                                  reinterpret_cast<const void *>(size_t(s.offset + e.Offset)));
        }
    }
    VaoEntry &ent = vaoCache_[key];
    ent.vao = vao; ent.elem = 0;   // fresh VAO has no element binding captured yet
    curVaoEnt_ = &ent;
}

HRESULT WINAPI GLDevice::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                       UINT PrimitiveCount) {
    KB_FlushBatchedDraws();   // also attaches the context to this thread (first use)
    extern unsigned long g_kbDraws; ++g_kbDraws;
#if defined(__EMSCRIPTEN__)
    // ?vbarena: same stream-0 fold rules as DrawIndexedPrimitive.
    if (s_kbStream0Fold) {
        if (streams_[1].vb || streams_[2].vb || streams_[3].vb) {
            streams_[0].offset = s_kbStream0Fold * streams_[0].stride;
            s_kbStream0Fold = 0;
            s_kbStreamIdent[0] = ~(size_t)0;
        } else {
            StartVertex += s_kbStream0Fold;
        }
    }
#endif
    if (!useDrawProgram()) return D3D_OK;   // shader still linking -> skip (pops in next frame)
    applyVertexState();

    GLenum mode; GLsizei verts;
    primInfo(PrimitiveType, PrimitiveCount, &mode, &verts);
    glDrawArrays(mode, (GLint)StartVertex, verts);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex,
                                              UINT /*MinVertexIndex*/, UINT /*NumVertices*/,
                                              UINT startIndex, UINT primCount) {
    if (!ib_) return D3D_OK;
    KB_EnsureCtxOnThread();
    extern unsigned long g_kbDraws; ++g_kbDraws;
#if defined(__EMSCRIPTEN__)
    // ?vbarena: apply the stream-0 placement fold to this draw's baseVertex. Folding is
    // only valid single-stream (baseVertex shifts EVERY stream's fetch); a multi-stream
    // draw un-folds the bind back to a plain VAO offset and re-specifies.
    if (s_kbStream0Fold) {
        if (streams_[1].vb || streams_[2].vb || streams_[3].vb) {
            KB_FlushTagged(7);
            streams_[0].offset = s_kbStream0Fold * streams_[0].stride;
            s_kbStream0Fold = 0;
            s_kbStreamIdent[0] = ~(size_t)0;   // next SetStreamSource(0) re-derives
        } else {
            BaseVertexIndex += (INT)s_kbStream0Fold;
        }
    }
#endif
#if defined(__EMSCRIPTEN__)
    // KB_DRAWCOMP=1: per-frame draw-composition histogram. Key = the GEOMETRY identity
    // (decl + stream0 VB + IB + index range). Repeated identical geometry (the same prop drawn
    // many times -> INSTANCEABLE into one call) becomes a high-count bucket; unique world
    // surfaces stay count-1 (-> need MERGING, not instancing). The instanceable% decides which
    // reducer to build. Pointers (not glName) so it has zero GL side effects.
    {
        extern int g_kbDrawComp; extern void KB_DrawCompTick(uint64_t);
        if (g_kbDrawComp) {
            uint64_t k = (uint64_t)(uintptr_t)decl_ * 1000003ull;
            k ^= ((uint64_t)(uintptr_t)(streams_[0].vb)) << 17;
            k ^= ((uint64_t)(uintptr_t)ib_) << 7;
            k ^= ((uint64_t)startIndex << 1) ^ (uint64_t)primCount;
            KB_DrawCompTick(k);
        }
    }
#endif

    GLenum mode; GLsizei verts;
    primInfo(Type, primCount, &mode, &verts);
    bool is16 = (ib_->format() == D3DFMT_INDEX16);
    GLenum idxType = is16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    size_t idxSize = is16 ? 2 : 4;
#if defined(__EMSCRIPTEN__)
    // ?vbarena: the IB's arena placement biases every index byte offset (placed at
    // SetIndices, so arenaOff is final here).
    const void *offset = reinterpret_cast<const void *>(
        size_t(startIndex) * idxSize + (ib_->inArena() ? ib_->arenaOff() : 0u));
#else
    const void *offset = reinterpret_cast<const void *>(size_t(startIndex) * idxSize);
#endif

#if defined(__EMSCRIPTEN__)
    // ---- INSTANCING detection (?inst) -------------------------------------------------
    if (g_kbInstEnable < 0) { const char *e = getenv("KB_INST"); g_kbInstEnable = e ? (*e - '0') : 0;
                              if (g_kbInstEnable < 0 || g_kbInstEnable > 2) g_kbInstEnable = 0; }
    if (g_kbMdraw < 0) { const char *e = getenv("KB_NOMDRAW"); g_kbMdraw = (e && *e == '1') ? 0 : 1; }  // ENV from index.html (worker can't read location.search)
    if (g_kbInstEnable) {
        g_kbInstDev = this;
        // Since the last draw, was the only per-object change a CONTIGUOUS-span vs-const block
        // (<=8 regs so it fits the 16-attribute budget alongside geometry)? That span is the
        // per-object matrix + any adjacent per-object regs (lighting/colour). Previously this
        // required a SINGLE SetVertexShaderConstantF call of <=4 regs (matrix only) — which broke
        // ~78% of instanceable draws whose per-object data is set across MULTIPLE VSC calls
        // (multiCall). Now we accept the union span; flushInstanceRun still falls back safely when
        // there aren't enough free attribute locations. (Gap regs in the span are replicated per
        // instance — harmless: unchanged values, same for every instance.)
        bool matOnly = !g_kbNonMatrixDirty
                    && g_kbVscChangedMin <= g_kbVscChangedMax
                    && (g_kbVscChangedMax - g_kbVscChangedMin + 1) <= 8;
        unsigned mBase = g_kbVscChangedMin;
        int      mCount = (g_kbVscChangedMin <= g_kbVscChangedMax)
                        ? (int)(g_kbVscChangedMax - g_kbVscChangedMin + 1) : 0;
        // Fold a stride-aligned SINGLE-stream vertex OFFSET into baseVertex so the GL stream offset
        // becomes 0. Skinned/static models all share ONE cache VB but each draws at a different
        // offset (skinnedCachedOffset) — without this they look like different geometry (off
        // differs) and never collapse; folding the offset to baseVertex makes them share one VAO
        // (off 0) so the multiDraw path packs them into a single call. (multiDraw supplies per-draw
        // baseVertex for free.) Default-on; ?nomdraw disables the whole loose-geometry path.
        unsigned normOff = streams_[0].offset, normBaseVert = (unsigned)BaseVertexIndex;
        if (g_kbMdraw == 1 && streams_[0].stride && (streams_[0].offset % streams_[0].stride) == 0
            && !streams_[1].vb && !streams_[2].vb && !streams_[3].vb) {
            normBaseVert += streams_[0].offset / streams_[0].stride;
            normOff = 0;
        }
        // Geometry identity by BIND identity (?vbarena: arena co-residents share a GL
        // name, so same-format draws from different engine buffers can extend one run;
        // without the arena bindIdent() is the object address — the old behavior). The
        // IB ident carries the index format: co-residents may mix 16/32-bit indices.
        uintptr_t kbVbId = streams_[0].vb ? (uintptr_t)streams_[0].vb->bindIdent() : 0;
        uintptr_t kbIbId = ((uintptr_t)ib_->bindIdent() << 1) | (is16 ? 1u : 0u);
        InstGeom g{ (uintptr_t)decl_, kbVbId, kbIbId,
                    startIndex, primCount, normBaseVert, normOff, streams_[0].stride };
        // Diagnostic: a same-geometry draw that CAN'T extend a run — why?
        if (!matOnly && ((s_iN > 0 && g == s_iGeom) || (s_haveLast && g == s_lastGeom))) {
            int rng = (g_kbVscChangedMin <= g_kbVscChangedMax)
                    ? (int)(g_kbVscChangedMax - g_kbVscChangedMin + 1) : 0;
            if (g_kbNonMatrixDirty) { ++g_kbBrk[0]; if ((unsigned)g_kbLastDirtyCause < 12u) ++g_kbBrkCause[g_kbLastDirtyCause]; }
            else if (g_kbVscCalls != 1) ++g_kbBrk[1];
            else { ++g_kbBrk[2]; if (rng > g_kbBrkMaxRange) g_kbBrkMaxRange = rng; }
        }
        if (s_iN > 0) {
            bool same  = (g == s_iGeom);
            bool loose = (g_kbMdraw == 1) && !same && kbLooseGeom(g, s_iGeom);   // shared buffer, diff range
            if (matOnly && (same || loose) && mBase == s_iMatBase && mCount == s_iMatCount) {
                const float *src = vsConst_ + mBase * 4;          // append this instance's matrix
                s_iMatrices.insert(s_iMatrices.end(), src, src + mCount * 4);
                if (g_kbMdraw == 1) {                             // record per-draw geometry (multiDraw)
                    s_iVertsArr.push_back(verts); s_iOffsArr.push_back(offset);
                    s_iBaseVertArr.push_back((GLint)g.baseVert);   // offset already folded into baseVert
                    if (loose) s_iVarying = true;
                }
                ++s_iN; kbInstResetTrack(); return D3D_OK;        // deferred
            }
            flushInstanceRun();                                   // pattern broke -> emit run
        }
        // DIAGNOSTIC: classify matOnly draws that aren't continuing an identical-geometry run.
        if (matOnly && s_haveLast && !(g == s_lastGeom)) {
            bool sameBuf = g.decl == s_lastGeom.decl && g.vb == s_lastGeom.vb && g.ib == s_lastGeom.ib
                        && g.off == s_lastGeom.off && g.stride == s_lastGeom.stride;
            if (sameBuf) ++g_kbMdrawSameBuf; else ++g_kbMdrawDiffBuf;
        }
        bool startSame  = (g == s_lastGeom);
        bool startLoose = (g_kbMdraw == 1) && !startSame && kbLooseGeom(g, s_lastGeom);
        if (matOnly && s_haveLast && (startSame || startLoose) && mBase == s_lastMatBase && mCount == s_lastMatCount) {
            s_iN = 1; s_iGeom = g; s_iMatBase = mBase; s_iMatCount = mCount;
            s_iMode = mode; s_iVerts = verts; s_iIdxType = idxType; s_iOffset = offset;
            s_iMatrices.assign(vsConst_ + mBase * 4, vsConst_ + mBase * 4 + mCount * 4);
            if (g_kbMdraw == 1) {                                 // init per-draw geometry with the head
                s_iVertsArr.assign(1, verts); s_iOffsArr.assign(1, offset);
                s_iBaseVertArr.assign(1, (GLint)g.baseVert); s_iVarying = false;   // offset folded in
            }
            s_haveLast = false; kbInstResetTrack(); return D3D_OK;
        }
        // Drawn normally below; remember as the potential run head (its matrix range is known).
        s_lastGeom = g; s_lastMatBase = mBase; s_lastMatCount = mCount; s_haveLast = (mCount > 0);
        kbInstResetTrack();
    }
    KbDrawTimer kbtm_;        // ?perfms=1 frame-split accounting
    if (!g_kbBatchEnable) {   // batching off: the proven immediate path
        if (!useDrawProgram()) return D3D_OK;
        applyVertexState();
        unsigned elem = ib_->glName();
        if (!curVaoEnt_ || curVaoEnt_->elem != elem) {   // VAO captures the element binding
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem);
            if (curVaoEnt_) curVaoEnt_->elem = elem;
        }
        kbDrawElementsBV(mode, verts, idxType, offset, BaseVertexIndex);
        return D3D_OK;
    }
    // Batch path: if a batch is open, device state is IDENTICAL to the batch's first draw
    // (every mutator flushes first), so just append — zero GL calls for this draw.
    // ?vbarena caveat: without the multi-draw extension the CPU index-merge flush reads
    // s_bIb's CPU shadow, which only covers ONE engine buffer — co-resident IB switches
    // (same GL ident, different object) must not extend such a batch.
    if (s_bN > 0) {
        if (mode == s_bMode && idxType == s_bType && s_bN < kMaxBatch
            && (g_kbHasMultiDraw == 1 || ib_ == s_bIb)) {
            s_bCounts[s_bN]     = verts;
            s_bOffsets[s_bN]    = offset;
            s_bInstCounts[s_bN] = 1;
            s_bBaseVerts[s_bN]  = BaseVertexIndex;
            s_bBaseInst[s_bN]   = 0;
            ++s_bN;
            ++g_kbBatchedDraws;
            // Snapshot indices NOW (shadow data is current at draw time); entry 0
            // catches up here when the batch first reaches size 2.
            if (g_kbHasMultiDraw != 1 && s_bMode == GL_TRIANGLES && s_bIb)
                while (s_bMergedN < s_bN) kbMergeAppend(s_bMergedN);
            return D3D_OK;
        }
        KB_FlushTagged(10);   // mode/type changed or batch full
    }
    // Batch start: do the full per-state setup ONCE, then defer this draw.
    if (!useDrawProgram()) return D3D_OK;   // shader still linking -> skip (pops in next frame)
    applyVertexState();
    {
        unsigned elem = ib_->glName();
        if (!curVaoEnt_ || curVaoEnt_->elem != elem) {   // VAO captures the element binding
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem);
            if (curVaoEnt_) curVaoEnt_->elem = elem;
        }
        // The merge-flush rebinds its scratch IBO into whatever VAO is current; let it
        // record that into this VAO's tracked element slot (safe for the batch's
        // lifetime: every cache mutation is preceded by a flush).
        s_bElemSlot = curVaoEnt_ ? &curVaoEnt_->elem : nullptr;
    }
    s_bMode = mode; s_bType = idxType; s_bIb = ib_;
    s_bCounts[0] = verts; s_bOffsets[0] = offset; s_bInstCounts[0] = 1;
    s_bBaseVerts[0] = BaseVertexIndex; s_bBaseInst[0] = 0;
    s_bN = 1;
    return D3D_OK;
#else
    if (!useDrawProgram()) return D3D_OK;   // shader still linking -> skip (pops in next frame)
    applyVertexState();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_->glName());
    glDrawElementsBaseVertex(mode, verts, idxType, const_cast<void *>(offset), BaseVertexIndex);
    return D3D_OK;
#endif
}
