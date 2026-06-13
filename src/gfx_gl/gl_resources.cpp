// gl_resources.cpp — GL-backed vertex/index buffers + vertex declaration.
#include "gl_resources.h"
#include "gl_format.h"
#include "gl_optrace.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Perf counters (defined in gl_query.cpp) — attribute per-frame stall frames to
// texture/buffer upload work; surfaced in glcontext_sdl.cpp's [stall] line.
extern unsigned long g_kbTexUploads, g_kbTexBytes, g_kbBufBytes;

// ---- GL-thread detection (see the DEFERRED GL note in gl_resources.h) -----
// On the MT web build the GL context is local to ONE worker (recorded by
// glcontext_sdl.cpp at creation); GL calls from any other thread must be deferred.
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <pthread.h>
pthread_t g_kbGLThread = (pthread_t)0;  // set by EmWebGLContext::init
static bool kbOnGLThread() { return !g_kbGLThread || pthread_equal(pthread_self(), g_kbGLThread); }
#else
static inline bool kbOnGLThread() { return true; }
#endif

// Batched-draw flush (gl_d3d9_draw.cpp): pending draws reference the CURRENT GPU
// contents of buffers/textures — they must execute before any new upload lands.
extern "C" void KB_FlushBatchedDraws();
extern "C" void KB_FlushTagged(int cause); // +flush-cause telemetry

// Shadow stack: default ON (KB_SHADOWS=0 / ?shadows=0 disables). Gates depth-texture
// creation, SetDepthStencilSurface honoring and the sampler2DShadow variants in one
// switch. OFF is broken-by-design now: the engine still renders+samples the shadowmap,
// and without real depth storage the sampled garbage VARIES with worker timing per
// frame = flickering sun-shadow factors on world surfaces.
extern "C" int KB_ShadowsEnabled() {
    static int en = -1;
    if (en < 0) { const char *v = getenv("KB_SHADOWS"); en = (v && *v == '0') ? 0 : 1; }
    return en;
}

// ---- Static-geometry buffer ARENA (web, ?vbarena) --------------------------
// Place STATIC (non-dynamic) vertex/index buffers inside a few big shared GL buffers
// instead of one GL buffer each. Different models then bind the SAME GL name, and the
// draw layer (gl_d3d9_draw.cpp SetStreamSource/SetIndices) folds each buffer's arena
// placement into per-draw baseVertex / index offsets — switching models then changes
// no GL vertex state at all: the vtx-cause batch flushes disappear and cross-model
// draws can ride one multi-draw / instancing run. Allocation happens at first bind on
// the GL thread (sync(), where the bind stride is known — placement must be
// stride-aligned for the baseVertex fold); frees (destructors, possibly off-thread)
// only touch the mutex-guarded free list, never GL. Freed ranges are reused by later
// buffers; VAOs keyed on the arena name stay valid (per-draw baseVertex selects data),
// so arena frees do NOT go through the VAO dead-buffer invalidation.
#if defined(__EMSCRIPTEN__)
namespace {
struct KbArenaBlock { unsigned name; UINT off, size; };
struct KbArena {
    GLenum bindTarget;            // upload bind point (IBs use COPY_WRITE: ELEMENT is VAO state)
    UINT   chunkSize;
    std::mutex mu;
    std::vector<KbArenaBlock> freeList;   // kept sorted by (name, off); adjacent blocks coalesce
    unsigned long usedBytes = 0, chunks = 0;

    void insertSorted(KbArenaBlock nb) {             // caller holds mu
        size_t i = 0;
        while (i < freeList.size() && (freeList[i].name < nb.name ||
               (freeList[i].name == nb.name && freeList[i].off < nb.off))) ++i;
        // coalesce with predecessor / successor when contiguous in the same chunk
        if (i > 0 && freeList[i-1].name == nb.name && freeList[i-1].off + freeList[i-1].size == nb.off) {
            freeList[i-1].size += nb.size;
            if (i < freeList.size() && freeList[i].name == nb.name &&
                freeList[i-1].off + freeList[i-1].size == freeList[i].off) {
                freeList[i-1].size += freeList[i].size;
                freeList.erase(freeList.begin() + i);
            }
            return;
        }
        if (i < freeList.size() && freeList[i].name == nb.name && nb.off + nb.size == freeList[i].off) {
            freeList[i].off = nb.off; freeList[i].size += nb.size;
            return;
        }
        freeList.insert(freeList.begin() + i, nb);
    }
    // GL thread only (may create a chunk). First-fit honoring `align`; the alignment gap
    // and the tail remainder stay on the free list.
    bool alloc(UINT size, UINT align, unsigned *nameOut, UINT *offOut) {
        if (!align) align = 4;
        std::lock_guard<std::mutex> g(mu);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < freeList.size(); ++i) {
                KbArenaBlock b = freeList[i];
                UINT aoff = (b.off + align - 1) / align * align;
                UINT gap = aoff - b.off;
                if (b.size < gap || b.size - gap < size) continue;
                freeList.erase(freeList.begin() + i);
                if (gap) insertSorted(KbArenaBlock{b.name, b.off, gap});
                UINT rest = b.size - gap - size;
                if (rest) insertSorted(KbArenaBlock{b.name, aoff + size, rest});
                usedBytes += size;
                *nameOut = b.name; *offOut = aoff;
                return true;
            }
            if (pass == 1 || size > chunkSize) break;
            unsigned name = 0;                       // no fit: grow by one chunk and retry
            glGenBuffers(1, &name);
            glBindBuffer(bindTarget, name);
            glBufferData(bindTarget, chunkSize, nullptr, GL_STATIC_DRAW);
            glBindBuffer(bindTarget, 0);
            insertSorted(KbArenaBlock{name, 0, chunkSize});
            ++chunks;
            fprintf(stderr, "[vbarena] chunk #%lu created (%u MB, target 0x%x)\n",
                    chunks, chunkSize >> 20, bindTarget);
        }
        return false;                                // caller falls back to an own buffer
    }
    void free(unsigned name, UINT off, UINT size) {  // any thread; no GL calls
        std::lock_guard<std::mutex> g(mu);
        usedBytes -= size;
        insertSorted(KbArenaBlock{name, off, size});
    }
};
KbArena g_kbVbArena{GL_ARRAY_BUFFER,      48u << 20};
KbArena g_kbIbArena{GL_COPY_WRITE_BUFFER, 16u << 20};
} // namespace
extern "C" int KB_VbArenaEnabled() {
    // Default ON (user-validated H6); ?novbarena (or ?vbarena=0) = escape hatch.
    static int envOn = -1;
    if (envOn < 0) {
        const char *no = getenv("KB_NOVBARENA");
        const char *v  = getenv("KB_VBARENA");
        envOn = ((no && *no == '1') || (v && *v == '0')) ? 0 : 1;
    }
    // The arena's baseVertex folds need a REAL base-vertex draw path. WebGL2 core has
    // none (the plain glDrawElementsBaseVertex stub DROPS basevertex — the H5 world/gun
    // corruption); kbDrawElementsBV needs WEBGL_draw_instanced_base_vertex_base_instance.
    // The flag is -1 until context init, so pre-context buffer ctors see "off" and stay
    // own-buffer — per-buffer decisions are made once at placement, so mixing is safe.
    extern int g_kbHasBaseVertexExt;
    return envOn == 1 && g_kbHasBaseVertexExt == 1;
}
#else
extern "C" int KB_VbArenaEnabled() { return 0; }
#endif

// ---- GLVertexBuffer -------------------------------------------------------
GLVertexBuffer::GLVertexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage,
                               DWORD fvf, D3DPOOL pool)
    : device_(device), length_(length), usage_(usage), fvf_(fvf), pool_(pool),
      shadow_(length, 0) {
#if defined(__EMSCRIPTEN__)
    // ?vbarena: static buffers defer creation to sync() at first bind (the proven
    // off-thread-creation path), where the bind stride is known for aligned placement.
    if (KB_VbArenaEnabled() && !(usage_ & D3DUSAGE_DYNAMIC)) return;
#endif
    if (!kbOnGLThread()) return;  // created lazily in sync() on the GL thread
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, length_,
                 nullptr, (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GLVertexBuffer::sync() {
    UINT upMin, upMax;
    bool upDiscard;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_;
        pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false;
    }
    if (!vbo_) {
#if defined(__EMSCRIPTEN__)
        // ?vbarena: place static buffers in the shared arena, aligned to the bind stride
        // so the draw layer can fold the placement into baseVertex. No stride recorded
        // (bound only via odd paths) or stride not 4-aligned -> own-buffer fallback.
        if (KB_VbArenaEnabled() && !(usage_ & D3DUSAGE_DYNAMIC)
            && strideHint_ && (strideHint_ % 4) == 0
            && g_kbVbArena.alloc(length_, strideHint_, &vbo_, &arenaOff_)) {
            arena_ = true;
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferSubData(GL_ARRAY_BUFFER, arenaOff_, length_, shadow_.data());
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            g_kbBufBytes += length_;
            return;
        }
#endif
        // First touch on the GL thread: create and upload the whole shadow (covers any
        // pending ranges in one go).
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, length_, shadow_.data(),
                     (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_kbBufBytes += length_;
    } else if (upMax > upMin) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (upDiscard && !arena_)
            glBufferData(GL_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        // Arena residents re-upload in place (no orphan possible — the chunk is shared);
        // the shadow holds the full current contents either way.
        glBufferSubData(GL_ARRAY_BUFFER, arenaOff_ + upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
}

// Selective VAO-cache invalidation: note the dead buffer/decl so only the cache entries that
// reference it are dropped (the wholesale clear on every death was the periodic stutter).
extern "C" void KB_VaoNoteDeadBuf(unsigned name);
extern "C" void KB_VaoNoteDeadDecl(const void *decl);
GLVertexBuffer::~GLVertexBuffer() {
#if defined(__EMSCRIPTEN__)
    // Arena resident: return the range; the shared chunk (and VAOs referencing it) live on.
    if (arena_) { g_kbVbArena.free(vbo_, arenaOff_, length_); return; }
#endif
    if (vbo_) { KB_VaoNoteDeadBuf(vbo_); glDeleteBuffers(1, &vbo_); }
}

HRESULT WINAPI GLVertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return E_INVALIDARG;
    if (SizeToLock == 0) SizeToLock = length_ - OffsetToLock;  // 0 means "to end" in D3D
    {
        // Locks legitimately overlap across threads (frontend appends frame N+1 into the
        // shared dynamic ring while the backend tess-fills frame N) — accumulate a UNION
        // of outstanding ranges instead of single-slot fields a second Lock would clobber.
        std::lock_guard<std::mutex> g(lockMu_);
        ++lockDepth_;
        if (!(Flags & D3DLOCK_READONLY)) {
            if (OffsetToLock < outMin_) outMin_ = OffsetToLock;
            if (OffsetToLock + SizeToLock > outMax_) outMax_ = OffsetToLock + SizeToLock;
            if (Flags & D3DLOCK_DISCARD) outDiscard_ = true;
        }
    }
    *ppbData = shadow_.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::Unlock() {
    // FLUSH ONLY ON DISCARD: a DISCARD lock orphans the buffer (new storage), so any
    // pending batched draws that reference the OLD storage by offset would render
    // garbage — flush them first. A NOOVERWRITE lock appends to a fresh region and
    // leaves prior data intact, so batched draws stay valid; NOT flushing there lets
    // same-material dynamic geometry collapse into one merged draw (the big GL-call cut
    // — dynamic-buffer unlocks were the #1 batch-breaker, ~1900 flushes/frame).
    { std::lock_guard<std::mutex> g(lockMu_); if (outDiscard_) KB_FlushTagged(11); }
    UINT upMin = ~0u, upMax = 0;
    bool upDiscard = false, uploadNow = false;
    extern int g_kbCoalesceEnable;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        if (lockDepth_) --lockDepth_;
        // Fold the whole outstanding union into the pending range at EVERY unlock: the
        // other thread's still-open range may upload half-written, but its own Unlock
        // re-folds it, so the final upload always carries complete data.
        if (outDiscard_) pendDiscard_ = true;
        if (outMin_ < pendMin_) pendMin_ = outMin_;
        if (outMax_ > pendMax_) pendMax_ = outMax_;
        if (!lockDepth_) { outMin_ = ~0u; outMax_ = 0; outDiscard_ = false; }
        // DYNAMIC buffers defer even on the GL thread when coalescing: the engine does
        // many small Lock/Unlock cycles per frame (FX quads, skinned chunks); folding
        // them into the pending range and uploading ONCE at next bind replaces N
        // bind+upload call pairs with one (the in-game bufKB/f was ~11MB across
        // hundreds of calls). Off the GL thread sync() replays it at next bind.
        uploadNow = kbOnGLThread() && vbo_ && !(g_kbCoalesceEnable && (usage_ & D3DUSAGE_DYNAMIC));
        if (uploadNow) {
            upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_;
            pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false;
        }
    }
    if (uploadNow && upMax > upMin) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        // D3DLOCK_DISCARD = "I'm overwriting the whole buffer, give me fresh storage."
        // Orphan first (glBufferData NULL) so the upload does NOT block on the GPU still
        // reading last frame's geometry from this VBO — without it glBufferSubData forces
        // an implicit sync (the main per-frame dynamic-mesh stall). The engine only
        // DISCARDs at offset 0 (see rb_backend/r_shade), so the orphaned tail is never drawn.
        if (upDiscard)
            glBufferData(GL_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
    return D3D_OK;
}

HRESULT WINAPI GLVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DVERTEXBUFFER_DESC{};
    pDesc->Format = D3DFMT_UNKNOWN; pDesc->Type = D3DRTYPE_VERTEXBUFFER;
    pDesc->Usage = usage_; pDesc->Pool = pool_; pDesc->Size = length_; pDesc->FVF = fvf_;
    return D3D_OK;
}

// ---- GLIndexBuffer --------------------------------------------------------
GLIndexBuffer::GLIndexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage,
                             D3DFORMAT format, D3DPOOL pool)
    : device_(device), length_(length), usage_(usage), format_(format), pool_(pool),
      shadow_(length, 0) {
#if defined(__EMSCRIPTEN__)
    // ?vbarena: static IBs defer creation to sync() at first use for arena placement.
    if (KB_VbArenaEnabled() && !(usage_ & D3DUSAGE_DYNAMIC)) return;
#endif
    if (!kbOnGLThread()) return;  // created lazily in sync() on the GL thread
    glGenBuffers(1, &ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, length_,
                 nullptr, (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void GLIndexBuffer::sync() {
    UINT upMin, upMax;
    bool upDiscard;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_;
        pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false;
    }
    // Uploads go through GL_COPY_WRITE_BUFFER: the ELEMENT_ARRAY binding is VAO
    // STATE, so binding here silently rewrote (then zeroed) the element binding of
    // whatever VAO was current — the per-VAO bind-skip cache then drew with the
    // WRONG index buffer (the spazzing-triangle corruption under the VAO cache).
    if (!ibo_) {
#if defined(__EMSCRIPTEN__)
        // ?vbarena: static IBs join the shared index arena. Align 4: covers both index
        // sizes so the draw layer's byte-offset addition stays index-aligned.
        if (KB_VbArenaEnabled() && !(usage_ & D3DUSAGE_DYNAMIC)
            && g_kbIbArena.alloc(length_, 4, &ibo_, &arenaOff_)) {
            arena_ = true;
            glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
            glBufferSubData(GL_COPY_WRITE_BUFFER, arenaOff_, length_, shadow_.data());
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            g_kbBufBytes += length_;
            return;
        }
#endif
        KB_OpTag("ibCreate", length_, 0, 0);
        glGenBuffers(1, &ibo_);
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        glBufferData(GL_COPY_WRITE_BUFFER, length_, shadow_.data(),
                     (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        g_kbBufBytes += length_;
    } else if (upMax > upMin) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        if (upDiscard && !arena_)
            glBufferData(GL_COPY_WRITE_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_COPY_WRITE_BUFFER, arenaOff_ + upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
}

GLIndexBuffer::~GLIndexBuffer() {
#if defined(__EMSCRIPTEN__)
    if (arena_) { g_kbIbArena.free(ibo_, arenaOff_, length_); return; }
#endif
    if (ibo_) { KB_VaoNoteDeadBuf(ibo_); glDeleteBuffers(1, &ibo_); }
}

HRESULT WINAPI GLIndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
    if (!ppbData) return E_INVALIDARG;
    if (SizeToLock == 0) SizeToLock = length_ - OffsetToLock;
    {
        // See GLVertexBuffer::Lock — outstanding-range UNION; concurrent FE/BE locks.
        std::lock_guard<std::mutex> g(lockMu_);
        ++lockDepth_;
        if (!(Flags & D3DLOCK_READONLY)) {
            if (OffsetToLock < outMin_) outMin_ = OffsetToLock;
            if (OffsetToLock + SizeToLock > outMax_) outMax_ = OffsetToLock + SizeToLock;
            if (Flags & D3DLOCK_DISCARD) outDiscard_ = true;
        }
    }
    *ppbData = shadow_.data() + OffsetToLock;
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::Unlock() {
    // Flush pending batched draws only on DISCARD (orphan) — see GLVertexBuffer::Unlock.
    { std::lock_guard<std::mutex> g(lockMu_); if (outDiscard_) KB_FlushTagged(11); }
    UINT upMin = ~0u, upMax = 0;
    bool upDiscard = false, uploadNow = false;
    extern int g_kbCoalesceEnable;
    {
        std::lock_guard<std::mutex> g(lockMu_);
        if (lockDepth_) --lockDepth_;
        if (outDiscard_) pendDiscard_ = true;
        if (outMin_ < pendMin_) pendMin_ = outMin_;
        if (outMax_ > pendMax_) pendMax_ = outMax_;
        if (!lockDepth_) { outMin_ = ~0u; outMax_ = 0; outDiscard_ = false; }
        uploadNow = kbOnGLThread() && ibo_ && !(g_kbCoalesceEnable && (usage_ & D3DUSAGE_DYNAMIC));
        if (uploadNow) {
            upMin = pendMin_; upMax = pendMax_; upDiscard = pendDiscard_;
            pendMin_ = ~0u; pendMax_ = 0; pendDiscard_ = false;
        }
    }
    if (uploadNow && upMax > upMin) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
        // See GLVertexBuffer::Unlock — orphan on DISCARD to avoid the GPU sync stall.
        if (upDiscard)
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
    return D3D_OK;
}

HRESULT WINAPI GLIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DINDEXBUFFER_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage = usage_; pDesc->Pool = pool_; pDesc->Size = length_;
    return D3D_OK;
}

// ---- GLTexture ------------------------------------------------------------
// D3D's CreateTexture(Levels=0) means "full mip chain"; compute it from the size.
static UINT FullMipCount(UINT w, UINT h) {
    UINT m = (w > h) ? w : h, n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

GLTexture::GLTexture(IDirect3DDevice9 *device, UINT width, UINT height, UINT levels,
                     DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), width_(width), height_(height),
      levels_(levels ? levels : FullMipCount(width, height)), usage_(usage), format_(format), pool_(pool) {
    levelShadow_.resize(levels_);
    if (kbOnGLThread()) createGL();  // else deferred to sync() on the GL thread
}

// Allocate RENDERABLE, completeness-verified level-0 storage for a render-target
// texture. D3DToGLFormat maps the accurate SAMPLING format, but several D3D RT formats
// (notably A16B16G16R16 -> GL_RGBA16 unorm) are NOT color-renderable on WebGL2 — an
// FBO with such a colour attachment is GL_FRAMEBUFFER_UNSUPPORTED (0x8cdd) and every
// draw to it no-ops, so the HDR scene + post-fx pyramid render black. Try renderable
// candidates (HDR float, then RGBA8) and keep the first that yields a COMPLETE FBO.
void KB_AllocRenderableRT(unsigned tex, UINT w, UINT h, D3DFORMAT d3dfmt) {
    unsigned internal = 0, fmt = 0, type = 0; int bpp = 0;
    bool mapped = D3DToGLFormat(d3dfmt, &internal, &fmt, &type, &bpp);
    struct Cand { unsigned internal, fmt, type; };
    Cand cands[3]; int nc = 0;
    if (mapped && internal != GL_RGBA16)                     // accurate map, if renderable
        cands[nc++] = { internal, fmt, type };
    cands[nc++] = { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };    // HDR (renderable via EXT_color_buffer_float)
    cands[nc++] = { GL_RGBA8,   GL_RGBA, GL_UNSIGNED_BYTE }; // always-renderable last resort

    glBindTexture(GL_TEXTURE_2D, tex);
    GLuint probe = 0; glGenFramebuffers(1, &probe);
    GLint prevFbo = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, probe);
    int chosen = nc - 1;
    for (int i = 0; i < nc; ++i) {
        while (glGetError() != GL_NO_ERROR) {}
        glTexImage2D(GL_TEXTURE_2D, 0, cands[i].internal, w, h, 0, cands[i].fmt, cands[i].type, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) { chosen = i; break; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glDeleteFramebuffers(1, &probe);
    static int rtN = 0;
    if (++rtN <= 12)
        fprintf(stderr, "[gl] RT %ux%u d3dfmt=%u -> internal=0x%x\n", w, h, (unsigned)d3dfmt, cands[chosen].internal);
}

void GLTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    // DEPTH textures (the hardware-shadow path): the engine creates the shadow map as a
    // depth-format texture, renders into it via SetDepthStencilSurface, then SAMPLES it
    // with depth comparison (texldp). Allocate real depth storage and enable compare mode
    // so a sampler2DShadow lookup returns the PCF comparison result like D3D9 hardware
    // shadows do. (The old renderbuffer-backed path made every shadow read garbage ->
    // the long-standing black gun/surfaces bug on native AND web.)
    {
        unsigned internal = 0, fmt = 0, type = 0, attach = 0;
        switch ((unsigned)format_) {
            case 75: /*D3DFMT_D24S8*/ case 77: /*D3DFMT_D24X8*/ case 79: /*D3DFMT_D24FS8*/
                internal = GL_DEPTH24_STENCIL8; fmt = GL_DEPTH_STENCIL;
                type = GL_UNSIGNED_INT_24_8; attach = GL_DEPTH_STENCIL_ATTACHMENT; break;
            case 80: /*D3DFMT_D16*/ case 70: /*D3DFMT_D16_LOCKABLE*/
                internal = GL_DEPTH_COMPONENT16; fmt = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_SHORT; attach = GL_DEPTH_ATTACHMENT; break;
            case 71: /*D3DFMT_D32*/ case 73: /*D3DFMT_D24X4S4*/
                internal = GL_DEPTH_COMPONENT24; fmt = GL_DEPTH_COMPONENT;
                type = GL_UNSIGNED_INT; attach = GL_DEPTH_ATTACHMENT; break;
        }
        // D3DUSAGE_DEPTHSTENCIL is the authoritative signal: the engine's shadowmap
        // texture arrives with a GARBAGE format (gfxMetrics.shadowmapFormat* reads
        // misaligned in the decompile — observed 374936), which fell through to color
        // storage -> attached as depth -> FBO incomplete -> black passes (B59 run).
        if (!internal && (usage_ & D3DUSAGE_DEPTHSTENCIL)) {
            internal = GL_DEPTH24_STENCIL8; fmt = GL_DEPTH_STENCIL;
            type = GL_UNSIGNED_INT_24_8; attach = GL_DEPTH_STENCIL_ATTACHMENT;
        }
        if (internal && KB_ShadowsEnabled()) {
            isDepth_ = true;
            glTexImage2D(GL_TEXTURE_2D, 0, internal, width_, height_, 0, fmt, type, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            fprintf(stderr, "[gl] depth texture %ux%u fmt=%u (shadow map)\n", width_, height_, (unsigned)format_);
            return;
        }
    }
    // A render-target texture is never Lock/Unlocked, so allocate level-0 storage now
    // to make it complete for FBO colour attachment. CRITICAL: the storage must be a
    // RENDERABLE format. D3DToGLFormat maps A16B16G16R16 -> GL_RGBA16 (the accurate
    // sampling format) but RGBA16 UNORM is NOT color-renderable in WebGL2 — only
    // RGBA16F is. An RGBA16 colour attachment makes the FBO GL_FRAMEBUFFER_UNSUPPORTED
    // (0x8cdd), so every HDR/bloom RT (the scene + the post-fx pyramid) rendered
    // nothing = the whole scene went black mid-game. Pick a renderable internalformat
    // and VERIFY completeness, falling back until one works.
    if (usage_ & D3DUSAGE_RENDERTARGET) {
        KB_AllocRenderableRT(tex_, width_, height_, format_);
        glBindTexture(GL_TEXTURE_2D, tex_);
    } else {
        // Give every texture a neutral 1x1 level-0 immediately so it is COMPLETE even
        // before (or if) the engine uploads its pixels — otherwise a not-yet-uploaded
        // (e.g. streamed) texture is incomplete and samples as a debug colour (magenta
        // on Mesa). The real LockRect/UnlockRect redefines level 0 with the true data.
        static const unsigned char gray[4] = { 128, 128, 128, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, gray);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GLTexture::ensureDepthStorage() {
    if (isDepth_) return;
    KB_OpTag("dsRetrofit", width_, height_, 0);
    if (!tex_) createGL();
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    isDepth_ = true;
    fprintf(stderr, "[gl] retrofitted %ux%u texture to depth storage (shadow DS)\n", width_, height_);
}

GLTexture::~GLTexture() { if (tex_) glDeleteTextures(1, &tex_); }

HRESULT WINAPI GLTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->Width  = width_  >> Level ? width_  >> Level : 1;
    pDesc->Height = height_ >> Level ? height_ >> Level : 1;
    return D3D_OK;
}

HRESULT WINAPI GLTexture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
    if (!ppSurfaceLevel || Level >= levels_) return E_INVALIDARG;
    *ppSurfaceLevel = new GLSurface(this, Level);
    return D3D_OK;
}

HRESULT WINAPI GLTexture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *, DWORD) {
    if (!pLockedRect || Level >= levels_) return E_INVALIDARG;
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    int blockBytes = 0;
    if (D3DCompressedGLFormat(format_, &blockBytes)) {
        // DXT/BC: the lock surface is a grid of 4x4 blocks; Pitch is bytes per block row.
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (levelShadow_[Level].size() != (size_t)bw * bh * blockBytes) levelShadow_[Level].assign((size_t)bw * bh * blockBytes, 0);  // preserve: D3D9 Lock keeps contents
        pLockedRect->Pitch = (int)(bw * blockBytes);
    } else {
        int bpp = D3DFormatBpp(format_);
        if (levelShadow_[Level].size() != (size_t)w * h * bpp) levelShadow_[Level].assign((size_t)w * h * bpp, 0);  // preserve
        pLockedRect->Pitch = (int)(w * bpp);
    }
    lockLevel_ = Level;
    dirty_     = true;
    pLockedRect->pBits = levelShadow_[Level].data();
    return D3D_OK;
}

HRESULT WINAPI GLTexture::UnlockRect(UINT Level) {
    KB_FlushTagged(11);
    if (Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!kbOnGLThread() || !tex_) {
        pendLevels_ |= 1u << Level;  // replayed by sync() at next bind on the GL thread
        return D3D_OK;
    }
    uploadLevel(Level);
    return D3D_OK;
}

void GLTexture::sync() {
    if (!tex_) createGL();
    if (pendLevels_) {
        unsigned p = pendLevels_;
        pendLevels_ = 0;
        for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
            if (p & 1) uploadLevel(L);
    }
}

void GLTexture::uploadLevel(UINT Level) {
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    // One-time GPU capability report — tells us if S3TC (DXT) uploads can work.
    static bool reported = false;
    if (!reported) {
        reported = true;
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        // Desktop name: GL_EXT_texture_compression_s3tc; WebGL name:
        // WEBGL_compressed_texture_s3tc. Matching only the desktop spelling made
        // every web context report S3TC=NO (a long-lived red herring).
        bool s3tc = ext && (strstr(ext, "texture_compression_s3tc") ||
                            strstr(ext, "compressed_texture_s3tc"));
        fprintf(stderr, "[gl] renderer=%s | GL=%s | S3TC=%s\n",
                glGetString(GL_RENDERER), glGetString(GL_VERSION), s3tc ? "YES" : "NO");
    }
    KB_OpTag("tex2dUp", (unsigned)format_, (w << 16) | h, Level);
    glBindTexture(GL_TEXTURE_2D, tex_);
    while (glGetError() != GL_NO_ERROR) {}  // drain prior errors
    int blockBytes = 0; unsigned cfmt = D3DCompressedGLFormat(format_, &blockBytes);
    ++g_kbTexUploads;
    g_kbTexBytes += (unsigned long)levelShadow_[Level].size();
    if (cfmt) {
        glCompressedTexImage2D(GL_TEXTURE_2D, Level, cfmt, w, h, 0,
                               (GLsizei)levelShadow_[Level].size(), levelShadow_[Level].data());
    } else {
        unsigned internal, format, type; int bpp;
        if (!D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
            fprintf(stderr, "[gl] GLTexture: unsupported format 0x%x (level not uploaded)\n", format_);
            glBindTexture(GL_TEXTURE_2D, 0); return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        const void *pixels = levelShadow_[Level].data();
        std::vector<unsigned char> swz;
        if (D3DFormatNeedsBGRASwizzle(format_)) {
            // WebGL2 has no GL_BGRA: the bytes are BGRA, so swap B<->R into a temp.
            // X8R8G8B8 carries no real alpha — force the X byte opaque so it samples solid.
            swz = levelShadow_[Level];
            for (size_t i = 0; i + 3 < swz.size(); i += 4) { unsigned char t = swz[i]; swz[i] = swz[i + 2]; swz[i + 2] = t; }
            if (format_ == D3DFMT_X8R8G8B8) for (size_t i = 3; i < swz.size(); i += 4) swz[i] = 255;
            pixels = swz.data();
        } else if (format_ == D3DFMT_G16R16) {
            // WebGL2 has no 16-bit unorm RG (GL_RG16); D3DToGLFormat maps G16R16 -> RG8, so
            // down-convert here. Source is 4 bytes/px: R16 then G16, each little-endian, so
            // the high byte of each 16-bit channel is the 8-bit value. (Restores the
            // secondary lightmap — see gl_format.cpp.)
            const std::vector<unsigned char> &src = levelShadow_[Level];
            swz.resize((src.size() / 4) * 2);
            for (size_t i = 0, o = 0; i + 3 < src.size(); i += 4, o += 2) {
                swz[o]     = src[i + 1];   // R = high byte of low word
                swz[o + 1] = src[i + 3];   // G = high byte of high word
            }
            pixels = swz.data();
        }
        glTexImage2D(GL_TEXTURE_2D, Level, internal, w, h, 0, format, type, pixels);
    }
    GLenum uerr = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (uerr != GL_NO_ERROR)
        fprintf(stderr, "[gl] texture upload error 0x%x: L%u fmt=0x%x %ux%u %s\n",
                uerr, Level, (unsigned)format_, w, h, cfmt ? "DXT" : "raw");
}

// ?lmarray: copy this texture's level-0 pixels into one layer of a GL_TEXTURE_2D_ARRAY, reusing the
// retained CPU shadow (no readback / renderability needed). Stage 1a = primary lightmap (D3DFMT_L8 ->
// R8/RED, no swizzle). Secondary G16R16 (s13/s14) will need the RG8 down-convert applied here first.
void GLTexture::KB_UploadIntoArrayLayer(unsigned arrayTex, int layer) {
    glName();   // ensure the GL texture exists; the level-0 CPU shadow persists after upload
    if (levelShadow_.empty() || levelShadow_[0].empty()) return;
    unsigned internal, format, type; int bpp;
    if (!D3DToGLFormat(format_, &internal, &format, &type, &bpp)) return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, arrayTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                    (GLsizei)width_, (GLsizei)height_, 1, format, type, levelShadow_[0].data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

// ---- GLVolumeTexture (GL_TEXTURE_3D) --------------------------------------
GLVolumeTexture::GLVolumeTexture(IDirect3DDevice9 *device, UINT w, UINT h, UINT d, UINT levels,
                                 DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), width_(w), height_(h), depth_(d),
      levels_(levels ? levels : 1), usage_(usage), format_(format), pool_(pool) {
    levelShadow_.resize(levels_);
    if (kbOnGLThread()) createGL();
}

void GLVolumeTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_3D, tex_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_3D, 0);
}

void GLVolumeTexture::sync() {
    if (!tex_) createGL();
    if (pendLevels_) {
        unsigned p = pendLevels_;
        pendLevels_ = 0;
        for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
            if (p & 1) uploadLevel(L);
    }
}
GLVolumeTexture::~GLVolumeTexture() { if (tex_) glDeleteTextures(1, &tex_); }

HRESULT WINAPI GLVolumeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_; if (device_) device_->AddRef(); return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DVOLUME_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_VOLUME; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->Width  = width_  >> Level ? width_  >> Level : 1;
    pDesc->Height = height_ >> Level ? height_ >> Level : 1;
    pDesc->Depth  = depth_  >> Level ? depth_  >> Level : 1;
    return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *, DWORD) {
    if (!pLockedVolume || Level >= levels_) return E_INVALIDARG;
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    UINT d = depth_  >> Level ? depth_  >> Level : 1;
    int bpp = D3DFormatBpp(format_);
    if (levelShadow_[Level].size() != (size_t)w * h * d * bpp) levelShadow_[Level].assign((size_t)w * h * d * bpp, 0);  // preserve (model-lighting volume: per-frame partial patches!)
    dirty_ = true;
    pLockedVolume->RowPitch   = (int)(w * bpp);
    pLockedVolume->SlicePitch = (int)(w * h * bpp);
    pLockedVolume->pBits      = levelShadow_[Level].data();
    return D3D_OK;
}
HRESULT WINAPI GLVolumeTexture::UnlockBox(UINT Level) {
    KB_FlushTagged(11);
    if (Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!kbOnGLThread() || !tex_) { pendLevels_ |= 1u << Level; return D3D_OK; }
    uploadLevel(Level);
    return D3D_OK;
}

void GLVolumeTexture::uploadLevel(UINT Level) {
    UINT w = width_  >> Level ? width_  >> Level : 1;
    UINT h = height_ >> Level ? height_ >> Level : 1;
    UINT d = depth_  >> Level ? depth_  >> Level : 1;
    unsigned internal, format, type; int bpp;
    if (D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
        glBindTexture(GL_TEXTURE_3D, tex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_3D, Level, internal, w, h, d, 0, format, type, levelShadow_[Level].data());
        glBindTexture(GL_TEXTURE_3D, 0);
    } else {
        fprintf(stderr, "[gl] GLVolumeTexture: unsupported format 0x%x\n", format_);
    }
}

// ---- GLCubeTexture (GL_TEXTURE_CUBE_MAP) ----------------------------------
GLCubeTexture::GLCubeTexture(IDirect3DDevice9 *device, UINT edgeLen, UINT levels,
                             DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : device_(device), edge_(edgeLen),
      levels_(levels ? levels : FullMipCount(edgeLen, edgeLen)), usage_(usage), format_(format), pool_(pool) {
    for (auto &face : levelShadow_) face.resize(levels_);
    if (kbOnGLThread()) createGL();
}

void GLCubeTexture::createGL() {
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                    levels_ > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, levels_ - 1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

#if defined(__EMSCRIPTEN__)
// ---- CPU DXT decode + box-filter mip build (cube reflection probes) ----------------
// glGenerateMipmap WEDGES the ANGLE-GL context service-side (compressed AND uncompressed),
// so single-mip probe cubes get their chain built on the CPU instead.
static inline void kbRGB565(unsigned v, unsigned char *o) {
    o[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
    o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
    o[2] = (unsigned char)((v & 31) * 255 / 31);
}
static void kbDecodeColorBlock(const unsigned char *b, bool dxt1, unsigned char out[16][4]) {
    unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    unsigned char col[4][4];
    kbRGB565(c0, col[0]); col[0][3] = 255;
    kbRGB565(c1, col[1]); col[1][3] = 255;
    if (!dxt1 || c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            col[2][i] = (unsigned char)((2 * col[0][i] + col[1][i]) / 3);
            col[3][i] = (unsigned char)((col[0][i] + 2 * col[1][i]) / 3);
        }
        col[2][3] = col[3][3] = 255;
    } else {                       // DXT1 3-color + transparent-black mode
        for (int i = 0; i < 3; ++i) {
            col[2][i] = (unsigned char)((col[0][i] + col[1][i]) / 2);
            col[3][i] = 0;
        }
        col[2][3] = 255; col[3][3] = 0;
    }
    unsigned bits = b[4] | (b[5] << 8) | (b[6] << 16) | ((unsigned)b[7] << 24);
    for (int i = 0; i < 16; ++i) {
        const unsigned char *c = col[(bits >> (2 * i)) & 3];
        out[i][0] = c[0]; out[i][1] = c[1]; out[i][2] = c[2]; out[i][3] = c[3];
    }
}
static void kbDecodeAlphaDXT5(const unsigned char *b, unsigned char a[16]) {
    unsigned a0 = b[0], a1 = b[1];
    unsigned char pal[8];
    pal[0] = (unsigned char)a0; pal[1] = (unsigned char)a1;
    if (a0 > a1) { for (int i = 1; i < 7; ++i) pal[1 + i] = (unsigned char)(((7 - i) * a0 + i * a1) / 7); }
    else { for (int i = 1; i < 5; ++i) pal[1 + i] = (unsigned char)(((5 - i) * a0 + i * a1) / 5); pal[6] = 0; pal[7] = 255; }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (unsigned long long)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i) a[i] = pal[(bits >> (3 * i)) & 7];
}
static void kbDecodeDXT(const unsigned char *src, unsigned fourcc, unsigned w, unsigned h,
                        std::vector<unsigned char> &out) {
    out.assign((size_t)w * h * 4, 0);
    unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
    int bb = (fourcc == 0x31545844u) ? 8 : 16;   // 'DXT1' : DXT3/5
    for (unsigned by = 0; by < bh; ++by)
        for (unsigned bx = 0; bx < bw; ++bx) {
            const unsigned char *b = src + ((size_t)by * bw + bx) * bb;
            unsigned char texel[16][4], alpha[16];
            bool hasA = false;
            if (fourcc == 0x35545844u)      { kbDecodeAlphaDXT5(b, alpha); hasA = true; b += 8; }   // 'DXT5'
            else if (fourcc == 0x33545844u) { for (int i = 0; i < 16; ++i) alpha[i] = (unsigned char)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17); hasA = true; b += 8; }   // 'DXT3'
            kbDecodeColorBlock(b, fourcc == 0x31545844u, texel);
            for (int i = 0; i < 16; ++i) {
                unsigned x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                unsigned char *d = &out[((size_t)y * w + x) * 4];
                d[0] = texel[i][0]; d[1] = texel[i][1]; d[2] = texel[i][2];
                d[3] = hasA ? alpha[i] : texel[i][3];
            }
        }
}
static void kbBoxHalve(const std::vector<unsigned char> &src, unsigned w, unsigned h,
                       std::vector<unsigned char> &dst, unsigned &ow, unsigned &oh) {
    ow = w > 1 ? w >> 1 : 1; oh = h > 1 ? h >> 1 : 1;
    dst.assign((size_t)ow * oh * 4, 0);
    for (unsigned y = 0; y < oh; ++y)
        for (unsigned x = 0; x < ow; ++x) {
            unsigned x0 = x * 2, y0 = y * 2;
            unsigned x1 = (x0 + 1 < w) ? x0 + 1 : x0, y1 = (y0 + 1 < h) ? y0 + 1 : y0;
            for (int c = 0; c < 4; ++c) {
                unsigned s = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c]
                           + src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
                dst[((size_t)y * ow + x) * 4 + c] = (unsigned char)(s / 4);
            }
        }
}
#endif

void GLCubeTexture::maybeGenMips() {
    // Single-mip cubes (reflection probes shipped without a chain) make the shader's
    // gloss->LOD blur (textureLod) a no-op: everything reflects MIRROR SHARP (the
    // "vaseline" gloss). Once all 6 level-0 faces have data, build the chain ourselves.
    if (mipsGenned_ || levels_ != 1 || level0Faces_ != 0x3F || !tex_) return;
    mipsGenned_ = true;
    KB_OpTag("genMips", edge_, 0, 0);
#if defined(__EMSCRIPTEN__)
    // CPU mip build (default ON): decode DXT level 0 -> RGBA8, box-filter halves, upload
    // the chain manually. All levels must share one internal format, so a DXT level 0 is
    // re-specified as RGBA8 too (probes are small; exact decode, no quality loss).
    // KB_NOCUBEMIPS=1 disables (back to sharp probes); KB_MIPS=1 forces the legacy GPU
    // glGenerateMipmap experiment (KNOWN to wedge ANGLE-GL service-side — diagnosis only).
    { static int off = -1;
      if (off < 0) { const char *v = getenv("KB_NOCUBEMIPS"); off = (v && *v == '1') ? 1 : 0; }
      if (off) return; }
    { static int wantGpu = -1;
      if (wantGpu < 0) { const char *v = getenv("KB_MIPS"); wantGpu = (v && *v == '1') ? 1 : 0; }
      if (wantGpu) {
          int blockBytes = 0;
          if (D3DCompressedGLFormat(format_, &blockBytes) != 0) return;  // compressed: invalid for GenerateMipmap
          glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
          int maxLvl = 0; for (UINT e = edge_; e > 1; e >>= 1) ++maxLvl;
          glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLvl);
          glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
          glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
          glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
          return;
      } }
    int blockBytes = 0; unsigned cfmt = D3DCompressedGLFormat(format_, &blockBytes);
    unsigned internal = GL_RGBA8, fmtGL = GL_RGBA, typeGL = GL_UNSIGNED_BYTE; int bpp = 4;
    if (!cfmt) {
        // Uncompressed: box-filter the existing byte layout channel-wise and upload the
        // chain in the SAME format as level 0 — only 4-byte byte-typed formats qualify.
        if (!D3DToGLFormat(format_, &internal, &fmtGL, &typeGL, &bpp) || bpp != 4 || typeGL != GL_UNSIGNED_BYTE)
            return;
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int maxLvl = 0; for (UINT e = edge_; e > 1; e >>= 1) ++maxLvl;
    for (unsigned f = 0; f < 6; ++f) {
        if (levelShadow_[f][0].empty()) continue;   // level0Faces_ says uploaded; be safe
        std::vector<unsigned char> rgba;
        if (cfmt) {
            kbDecodeDXT(levelShadow_[f][0].data(), (unsigned)format_, edge_, edge_, rgba);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, edge_, edge_, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());   // re-spec level 0 uncompressed
        } else {
            rgba = levelShadow_[f][0];
        }
        unsigned w = edge_, h = edge_;
        std::vector<unsigned char> next;
        for (int L = 1; L <= maxLvl; ++L) {
            unsigned ow, oh;
            kbBoxHalve(rgba, w, h, next, ow, oh);
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, L,
                         cfmt ? GL_RGBA8 : internal, ow, oh, 0,
                         cfmt ? GL_RGBA : fmtGL, GL_UNSIGNED_BYTE, next.data());
            rgba.swap(next); w = ow; h = oh;
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLvl);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    static int prints = 0;
    if (prints < 3) { ++prints; fprintf(stderr, "[gl] cube %ux%u: CPU-built mip chain (%s, gloss blur enabled)\n", edge_, edge_, cfmt ? "DXT->RGBA8" : "raw"); }
#else
    // Native: glGenerateMipmap works; compressed cubes keep level 0 only.
    { int blockBytes = 0;
      if (D3DCompressedGLFormat(format_, &blockBytes) != 0) return; }
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    int maxLvl = 0; for (UINT e = edge_; e > 1; e >>= 1) ++maxLvl;
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLvl);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
#endif
}

void GLCubeTexture::sync() {
    if (!tex_) createGL();
    if (pendAny_) {
        pendAny_ = false;
        for (unsigned f = 0; f < 6; ++f) {
            unsigned p = pendLevels_[f];
            pendLevels_[f] = 0;
            for (UINT L = 0; L < levels_ && p; ++L, p >>= 1)
                if (p & 1) { uploadFaceLevel(f, L); if (L == 0) level0Faces_ |= 1u << f; }
        }
    }
    maybeGenMips();
}
GLCubeTexture::~GLCubeTexture() { if (tex_) glDeleteTextures(1, &tex_); }

HRESULT WINAPI GLCubeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_; if (device_) device_->AddRef(); return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
    if (!pDesc || Level >= levels_) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE; pDesc->Usage = usage_; pDesc->Pool = pool_;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    pDesc->Width = e; pDesc->Height = e;
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level,
                                                IDirect3DSurface9 **ppSurface) {
    if (!ppSurface || (unsigned)face >= 6 || level >= levels_) return E_INVALIDARG;
    *ppSurface = new GLSurface(this, face, level);
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::LockRect(D3DCUBEMAP_FACES FaceType, UINT Level,
                                       D3DLOCKED_RECT *pLockedRect, const RECT *, DWORD) {
    if (!pLockedRect || (unsigned)FaceType >= 6 || Level >= levels_) return E_INVALIDARG;
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    std::vector<unsigned char> &shadow = levelShadow_[FaceType][Level];
    int blockBytes = 0;
    if (D3DCompressedGLFormat(format_, &blockBytes)) {
        UINT bw = (e + 3) / 4, bh = (e + 3) / 4;
        if (shadow.size() != (size_t)bw * bh * blockBytes) shadow.assign((size_t)bw * bh * blockBytes, 0);  // preserve
        pLockedRect->Pitch = (int)(bw * blockBytes);
    } else {
        int bpp = D3DFormatBpp(format_);
        if (shadow.size() != (size_t)e * e * bpp) shadow.assign((size_t)e * e * bpp, 0);  // preserve
        pLockedRect->Pitch = (int)(e * bpp);
    }
    dirty_ = true;
    pLockedRect->pBits = shadow.data();
    return D3D_OK;
}
HRESULT WINAPI GLCubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
    KB_FlushTagged(11);
    if ((unsigned)FaceType >= 6 || Level >= levels_ || !dirty_) return D3D_OK;
    dirty_ = false;
    if (!kbOnGLThread() || !tex_) {
        pendLevels_[(unsigned)FaceType] |= 1u << Level;
        pendAny_ = true;
        return D3D_OK;
    }
    uploadFaceLevel((unsigned)FaceType, Level);
    if (Level == 0) { level0Faces_ |= 1u << (unsigned)FaceType; maybeGenMips(); }
    return D3D_OK;
}

void GLCubeTexture::uploadFaceLevel(unsigned Face, UINT Level) {
    UINT e = edge_ >> Level ? edge_ >> Level : 1;
    std::vector<unsigned char> &shadow = levelShadow_[Face][Level];
    GLenum target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + Face;
    KB_OpTag("cubeUp", (unsigned)format_, e, Level);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    int blockBytes = 0; unsigned cfmt = D3DCompressedGLFormat(format_, &blockBytes);
    if (cfmt) {
        glCompressedTexImage2D(target, Level, cfmt, e, e, 0,
                               (GLsizei)shadow.size(), shadow.data());
    } else {
        unsigned internal, format, type; int bpp;
        if (!D3DToGLFormat(format_, &internal, &format, &type, &bpp)) {
            fprintf(stderr, "[gl] GLCubeTexture: unsupported format 0x%x (face %d level %d not uploaded)\n",
                    format_, (int)Face, (int)Level);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0); return;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(target, Level, internal, e, e, 0, format, type, shadow.data());
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

// ---- GLSurface ------------------------------------------------------------
GLSurface::GLSurface(GLTexture *owner, UINT level)
    : owner_(owner), level_(level),
      width_(owner->width()  >> level ? owner->width()  >> level : 1),
      height_(owner->height() >> level ? owner->height() >> level : 1),
      format_(owner->format()) {}

GLSurface::GLSurface(GLCubeTexture *owner, D3DCUBEMAP_FACES face, UINT level)
    : cubeOwner_(owner), cubeFace_(face), level_(level),
      width_(owner->edgeLength() >> level ? owner->edgeLength() >> level : 1),
      height_(owner->edgeLength() >> level ? owner->edgeLength() >> level : 1),
      format_(owner->format()) {}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, bool sysmem)
    : device_(device), width_(width), height_(height), format_(format), sysmem_(sysmem) {
    if (sysmem_) {
        shadow_.assign((size_t)width_ * height_ * D3DFormatBpp(format_), 0);
    } else {
        // Standalone render target: an immutable-storage GL texture.
        unsigned internal, fmt, type; int bpp;
        glGenTextures(1, &ownTex_);
        glBindTexture(GL_TEXTURE_2D, ownTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        (void)internal; (void)fmt; (void)type; (void)bpp;
        // Renderable, completeness-verified storage (see KB_AllocRenderableRT): RGBA16
        // unorm is NOT color-renderable on WebGL2, so HDR RTs must use RGBA16F or the
        // FBO is GL_FRAMEBUFFER_UNSUPPORTED and the scene renders black.
        KB_AllocRenderableRT(ownTex_, width_, height_, format_);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, GLBackbufferTag)
    : device_(device), width_(width), height_(height), format_(format), backbuffer_(true) {}

GLSurface::GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, GLDepthStencilTag)
    : device_(device), width_(width), height_(height), format_(format), depthStencil_(true) {
    // Shadow path: back the depth-stencil surface with a REAL sampleable depth texture
    // (compare mode for sampler2DShadow). The engine's shadowmapFormatSecondary arrives
    // as garbage from the decompiled metrics struct (e.g. 374936), so the format is NOT
    // trusted — always allocate DEPTH24_STENCIL8. Without this the shadow FBO attached
    // a color texture as depth -> incomplete -> whole passes no-op'd (black regions).
    if (KB_ShadowsEnabled() && kbOnGLThread()) {
        glGenTextures(1, &ownTex_);
        glBindTexture(GL_TEXTURE_2D, ownTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE /* manual-PCF engine: raw depth reads */);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width_, height_, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        ownTexIsDepth_ = true;
    }
}

GLSurface::~GLSurface() { if (ownTex_) glDeleteTextures(1, &ownTex_); }

unsigned GLSurface::texName() const {
    if (owner_) return owner_->glName();
    if (cubeOwner_) return cubeOwner_->glName();
    return ownTex_;
}

HRESULT WINAPI GLSurface::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (owner_) return owner_->GetDevice(ppDevice);
    if (cubeOwner_) return cubeOwner_->GetDevice(ppDevice);
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLSurface::GetDesc(D3DSURFACE_DESC *pDesc) {
    if (owner_) return owner_->GetLevelDesc(level_, pDesc);
    if (cubeOwner_) return cubeOwner_->GetLevelDesc(level_, pDesc);
    if (!pDesc) return E_INVALIDARG;
    *pDesc = D3DSURFACE_DESC{};
    pDesc->Format = format_; pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = depthStencil_ ? D3DUSAGE_DEPTHSTENCIL : (sysmem_ ? 0 : D3DUSAGE_RENDERTARGET);
    pDesc->Pool = sysmem_ ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->Width = width_; pDesc->Height = height_;
    return D3D_OK;
}

HRESULT WINAPI GLSurface::LockRect(D3DLOCKED_RECT *lr, const RECT *r, DWORD f) {
    if (owner_) return owner_->LockRect(level_, lr, r, f);
    if (cubeOwner_) return cubeOwner_->LockRect(cubeFace_, level_, lr, r, f);
    if (!lr) return E_INVALIDARG;
    if (!sysmem_) return E_FAIL;  // only system-memory surfaces are CPU-lockable here
    lr->Pitch = (int)(width_ * D3DFormatBpp(format_));
    lr->pBits = shadow_.data();
    return D3D_OK;
}

HRESULT WINAPI GLSurface::UnlockRect() {
    if (owner_) return owner_->UnlockRect(level_);
    if (cubeOwner_) return cubeOwner_->UnlockRect(cubeFace_, level_);
    return D3D_OK;  // sysmem: nothing to flush
}

HRESULT WINAPI GLSurface::GetContainer(REFIID, void **ppContainer) {
    if (!ppContainer) return E_INVALIDARG;
    if (owner_) { owner_->AddRef(); *ppContainer = owner_; }
    else if (cubeOwner_) { cubeOwner_->AddRef(); *ppContainer = cubeOwner_; }
    else                 { AddRef();             *ppContainer = this; }
    return D3D_OK;
}

// ---- GLVertexDeclaration --------------------------------------------------
GLVertexDeclaration::GLVertexDeclaration(IDirect3DDevice9 *device,
                                         const D3DVERTEXELEMENT9 *elements)
    : device_(device) {
    // Copy up to (and excluding) the D3DDECL_END terminator (Stream == 0xFF).
    for (const D3DVERTEXELEMENT9 *e = elements; e && e->Stream != 0xFF; ++e)
        elements_.push_back(*e);
}

GLVertexDeclaration::~GLVertexDeclaration() { KB_VaoNoteDeadDecl(this); }

HRESULT WINAPI GLVertexDeclaration::GetDevice(IDirect3DDevice9 **ppDevice) {
    if (!ppDevice) return E_INVALIDARG;
    *ppDevice = device_;
    if (device_) device_->AddRef();
    return D3D_OK;
}

HRESULT WINAPI GLVertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
    UINT n = (UINT)elements_.size() + 1;  // +1 for the terminator D3D reports
    if (pNumElements) *pNumElements = n;
    if (pElement) {
        for (size_t i = 0; i < elements_.size(); ++i) pElement[i] = elements_[i];
        pElement[elements_.size()] = D3DVERTEXELEMENT9 D3DDECL_END();
    }
    return D3D_OK;
}
