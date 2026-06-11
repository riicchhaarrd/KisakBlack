// gl_resources.cpp — GL-backed vertex/index buffers + vertex declaration.
#include "gl_resources.h"
#include "gl_format.h"

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

// ---- GLVertexBuffer -------------------------------------------------------
GLVertexBuffer::GLVertexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage,
                               DWORD fvf, D3DPOOL pool)
    : device_(device), length_(length), usage_(usage), fvf_(fvf), pool_(pool),
      shadow_(length, 0) {
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
        if (upDiscard)
            glBufferData(GL_ARRAY_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
}

extern unsigned g_kbVaoEpoch;   // VAO cache invalidation (gl_d3d9_draw.cpp)
GLVertexBuffer::~GLVertexBuffer() { ++g_kbVaoEpoch; if (vbo_) glDeleteBuffers(1, &vbo_); }

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
    KB_FlushTagged(11);   // pending draws read the PRE-update contents
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
        glGenBuffers(1, &ibo_);
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        glBufferData(GL_COPY_WRITE_BUFFER, length_, shadow_.data(),
                     (usage_ & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        g_kbBufBytes += length_;
    } else if (upMax > upMin) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, ibo_);
        if (upDiscard)
            glBufferData(GL_COPY_WRITE_BUFFER, length_, nullptr, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_COPY_WRITE_BUFFER, upMin, upMax - upMin, shadow_.data() + upMin);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        g_kbBufBytes += upMax - upMin;
    }
}

GLIndexBuffer::~GLIndexBuffer() { ++g_kbVaoEpoch; if (ibo_) glDeleteBuffers(1, &ibo_); }

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
    KB_FlushTagged(11);
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
    // A render-target texture is never Lock/Unlocked, so allocate level-0 storage
    // now to make it complete for FBO colour attachment.
    if (usage_ & D3DUSAGE_RENDERTARGET) {
        unsigned internal, fmt, type; int bpp;
        if (D3DToGLFormat(format_, &internal, &fmt, &type, &bpp))
            glTexImage2D(GL_TEXTURE_2D, 0, internal, width_, height_, 0, fmt, type, nullptr);
        else
            // Unsupported RT format (e.g. D3DFMT_G16R16 = 0x22): allocate a renderable
            // RGBA8 fallback at the real size so the FBO colour attachment is COMPLETE.
            // Otherwise the texture has no storage (0x0), every draw to it fails with
            // GL_INVALID_FRAMEBUFFER_OPERATION ("Attachment has zero size"), and the
            // hundreds of proxied error lines per frame lock up the in-game scene.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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

void GLCubeTexture::maybeGenMips() {
    // Single-mip cubes (reflection probes shipped without a chain) make the shader's
    // gloss->LOD blur (textureLod) a no-op: everything reflects MIRROR SHARP (the
    // "vaseline" gloss). Once all 6 level-0 faces have data, build the chain ourselves.
    if (mipsGenned_ || levels_ != 1 || level0Faces_ != 0x3F || !tex_) return;
    mipsGenned_ = true;
    // ?nomips=1 kill switch: the three "auto-generated mip chain" lines are the last
    // GL ops before the deproxy context's GPU channel wedges on NVIDIA/ANGLE-GL
    // (every later compile/link returns false/empty). Also: with S3TC working these
    // cubes are COMPRESSED, where GenerateMipmap is invalid anyway.
    // DISABLED on web: glGenerateMipmap on cube maps WEDGES the NVIDIA/ANGLE-GL
    // context service-side (compressed AND uncompressed — the 64x64 RGBA probe cube
    // reproduced it with the compressed guard already in place). The gloss blur loses
    // its mip chain until a CPU-side mip build replaces this (TODO). ?mips=1 re-enables
    // for experiments.
    { static int wantMips = -1;
      if (wantMips < 0) { const char *v = getenv("KB_MIPS"); wantMips = (v && *v == '1') ? 1 : 0; }
      if (!wantMips) return; }
    { int blockBytes = 0;
      if (D3DCompressedGLFormat(format_, &blockBytes) != 0) return; }  // compressed: no GenerateMipmap
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_);
    int maxLvl = 0; for (UINT e = edge_; e > 1; e >>= 1) ++maxLvl;
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLvl);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    static int prints = 0;
    if (prints < 3) { ++prints; fprintf(stderr, "[gl] cube %ux%u: auto-generated mip chain (gloss blur enabled)\n", edge_, edge_); }
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
        if (D3DToGLFormat(format_, &internal, &fmt, &type, &bpp))
            glTexImage2D(GL_TEXTURE_2D, 0, internal, width_, height_, 0, fmt, type, nullptr);
        else
            // Unsupported RT format (e.g. D3DFMT_G16R16 = 0x22): allocate a renderable RGBA8
            // fallback at the real size so this surface is a COMPLETE FBO colour attachment.
            // Without storage the texture is 0x0, the FBO is incomplete, every draw to it
            // fails (GL_INVALID_FRAMEBUFFER_OPERATION "zero size"), and the readback path
            // (GetRenderTargetData glReadBuffer/glReadPixels) operates on a broken FBO and
            // stalls the render thread -> the in-game spawn deadlock.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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

GLVertexDeclaration::~GLVertexDeclaration() { ++g_kbVaoEpoch; }

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
