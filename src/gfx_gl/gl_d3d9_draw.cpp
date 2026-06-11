// gl_d3d9_draw.cpp — geometry resource creation + the draw path for GLDevice.
//
// The built-in GLSL program here only covers the *pre-transformed* (D3DFVF_XYZRHW /
// D3DDECLUSAGE_POSITIONT) vertex path — i.e. the 2D / fixed-function geometry the
// engine submits in screen space. Programmable 3D shaders go through the DX9
// bytecode→GLSL translator instead (task #5); when a vertex shader is bound this
// built-in is bypassed.
#include "gl_d3d9.h"
#include "gl_resources.h"
#include "gl_shader.h"   // GLAttribLocation / GLAttribName / GLBindAttribLocations

#include <GL/glew.h>
#include <cstdio>
#include <vector>
#include <mutex>

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
    size_t off = (size_t)s_bOffsets[i];
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

extern "C" void KB_FlushBatchedDraws() {
    KB_EnsureCtxOnThread();
    if (!s_bN) return;
    ++g_kbBatchFlushes;
    if (s_bN == 1) {
        glDrawElementsBaseVertex(s_bMode, s_bCounts[0], s_bType,
                                 const_cast<void *>(s_bOffsets[0]), s_bBaseVerts[0]);
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
            glDrawElementsBaseVertex(s_bMode, s_bCounts[i], s_bType,
                                     const_cast<void *>(s_bOffsets[i]), s_bBaseVerts[i]);
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
}
#else
extern "C" void KB_FlushBatchedDraws() {}   // native: draws are immediate, nothing to flush
extern "C" void KB_FlushTagged(int) {}
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
    // No-change fast path: the engine re-sets identical bindings around most draws;
    // an unconditional flush here kept draw batches at size 1 (flushes/f == draws/f).
    if (s.vb == vb && s.offset == OffsetInBytes && s.stride == Stride) return D3D_OK;
    KB_FlushTagged(7);
    s.vb = vb; s.offset = OffsetInBytes; s.stride = Stride;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
    GLIndexBuffer *ib = static_cast<GLIndexBuffer *>(pIndexData);
    if (ib_ == ib) return D3D_OK;   // no-change fast path
    KB_FlushTagged(8);
    ib_ = ib;
    return D3D_OK;
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

void GLDevice::applyVertexState() {
    // ?novao=1 kill switch: bypass the VAO cache (full attrib re-spec per draw, the
    // pre-B100 path) to bisect geometry corruption vs the cache.
    static int noVao = -1;
    if (noVao < 0) { const char *v = getenv("KB_NOVAO"); noVao = (v && *v == '1') ? 1 : 0; }
    // A D3DUSAGE_DYNAMIC stream's bound offset changes every draw (the NOOVERWRITE/DISCARD ring),
    // so a per-(buffer,offset) cache entry never hits — it just accumulates throwaway VAOs until
    // the size cap nukes the whole cache = the periodic stutter (user-confirmed: ?novao=1 removed
    // it). Route dynamic draws through the shared-VAO re-spec path (no alloc, no cache churn);
    // only static layouts (stable offset) get cached, where hits actually pay off.
    bool dynamic = false;
    for (int i = 0; i < 4; ++i) if (streams_[i].vb && streams_[i].vb->isDynamic()) { dynamic = true; break; }
    if (noVao || dynamic) {
        if (!vao_) glGenVertexArrays(1, &vao_);
        if (curVao_ != vao_) glBindVertexArray(vao_);
        curVao_ = vao_; curVaoEnt_ = nullptr;   // element-bind gate disabled (no entry)
        for (int i = 0; i < 16; ++i) glDisableVertexAttribArray(i);
        glVertexAttrib4f(GLAttribLocation(D3DDECLUSAGE_COLOR, 0), 1.0f, 1.0f, 1.0f, 1.0f);
        if (!decl_) return;
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

    GLenum mode; GLsizei verts;
    primInfo(Type, primCount, &mode, &verts);
    bool is16 = (ib_->format() == D3DFMT_INDEX16);
    GLenum idxType = is16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    size_t idxSize = is16 ? 2 : 4;
    const void *offset = reinterpret_cast<const void *>(size_t(startIndex) * idxSize);

#if defined(__EMSCRIPTEN__)
    KbDrawTimer kbtm_;        // ?perfms=1 frame-split accounting
    if (!g_kbBatchEnable) {   // batching off: the proven immediate path
        if (!useDrawProgram()) return D3D_OK;
        applyVertexState();
        unsigned elem = ib_->glName();
        if (!curVaoEnt_ || curVaoEnt_->elem != elem) {   // VAO captures the element binding
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elem);
            if (curVaoEnt_) curVaoEnt_->elem = elem;
        }
        glDrawElementsBaseVertex(mode, verts, idxType, const_cast<void *>(offset), BaseVertexIndex);
        return D3D_OK;
    }
    // Batch path: if a batch is open, device state is IDENTICAL to the batch's first draw
    // (every mutator flushes first), so just append — zero GL calls for this draw.
    if (s_bN > 0) {
        if (mode == s_bMode && idxType == s_bType && s_bN < kMaxBatch) {
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
