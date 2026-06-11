// gl_resources.h — GL-backed D3D9 geometry resources.
//
// Vertex/index buffers map to GL buffer objects; the vertex declaration just
// caches the element list (it is consumed at draw time to set up vertex attrib
// pointers). GL handle types are kept as plain `unsigned` so this header stays
// free of any GL include.
#ifndef KISAK_GL_RESOURCES_H
#define KISAK_GL_RESOURCES_H

#include "gl_object.h"
#include <vector>
#include <mutex>

// DEFERRED GL (all resource classes below): the engine creates and Lock/Unlocks
// resources from loader threads. On the de-proxied web build the GL context is LOCAL
// to the render worker; Emscripten forwards GL calls from context-less threads to the
// browser main thread, which has no context there either -> "undefined.createBuffer"
// crash (the proxied build masked this because the DOM thread held the proxied
// context). So: when an op runs OFF the GL thread, the GL work (creation + upload) is
// recorded as pending against the CPU shadow and replayed by sync(), which glName()
// triggers at bind time on the render thread. On the GL thread behavior is unchanged.
//
// CONCURRENT LOCKS (the SMP world flicker): the engine's dynamic VB/IB are a SINGLE
// shared ring (gfxBuf.dynamic*BufferPool[1], D3D9 NOOVERWRITE contract) — the
// frontend appends frame N+1 geometry while the backend tess-fills frame N's draws
// from the SAME object, so two Lock/Unlock windows legitimately overlap. Per-object
// single-slot lock bookkeeping gets clobbered by the second Lock and one range is
// never uploaded -> stale/garbage geometry for one frame. So buffers keep a UNION of
// all outstanding lock ranges under a mutex, fold it into the pending range at every
// Unlock (over-upload is safe: later folds re-upload), and sync() snapshots+clears
// pend under the same mutex. glName()'s unlocked pend check is only a fast hint: the
// data a draw needs was folded either on this thread or before the SMP handoff, both
// of which it observes.
class GLVertexBuffer final : public GLObject<IDirect3DVertexBuffer9> {
public:
    GLVertexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);
    ~GLVertexBuffer() override;

    // IDirect3DResource9
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    // IDirect3DVertexBuffer9
    HRESULT WINAPI Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT WINAPI Unlock() override;
    HRESULT WINAPI GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override;

    unsigned glName() { if (!vbo_ || pendMax_ > pendMin_) sync(); return vbo_; }
    void     sync();   // GL thread only: create + replay pending uploads

private:
    IDirect3DDevice9         *device_;
    unsigned                  vbo_ = 0;
    UINT                      length_;
    DWORD                     usage_;
    DWORD                     fvf_;
    D3DPOOL                   pool_;
    std::vector<unsigned char> shadow_;  // CPU mirror backing Lock()
    std::mutex                lockMu_;        // guards lock/pend bookkeeping (FE+BE lock concurrently)
    UINT                      lockDepth_   = 0;
    UINT                      outMin_      = ~0u; // union of outstanding lock ranges
    UINT                      outMax_      = 0;
    bool                      outDiscard_  = false;
    UINT                      pendMin_     = ~0u; // deferred dirty range [pendMin_, pendMax_)
    UINT                      pendMax_     = 0;
    bool                      pendDiscard_ = false;
};

class GLIndexBuffer final : public GLObject<IDirect3DIndexBuffer9> {
public:
    GLIndexBuffer(IDirect3DDevice9 *device, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~GLIndexBuffer() override;

    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_INDEXBUFFER; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
    HRESULT WINAPI Unlock() override;
    HRESULT WINAPI GetDesc(D3DINDEXBUFFER_DESC *pDesc) override;

    unsigned glName() { if (!ibo_ || pendMax_ > pendMin_) sync(); return ibo_; }
    void     sync();
    D3DFORMAT format() const { return format_; }
    const unsigned char *shadowData() const { return shadow_.data(); }   // CPU mirror (merge-flush)
    UINT     length() const { return length_; }

private:
    IDirect3DDevice9         *device_;
    unsigned                  ibo_ = 0;
    UINT                      length_;
    DWORD                     usage_;
    D3DFORMAT                 format_;
    D3DPOOL                   pool_;
    std::vector<unsigned char> shadow_;
    std::mutex                lockMu_;
    UINT                      lockDepth_   = 0;
    UINT                      outMin_      = ~0u;
    UINT                      outMax_      = 0;
    bool                      outDiscard_  = false;
    UINT                      pendMin_     = ~0u;
    UINT                      pendMax_     = 0;
    bool                      pendDiscard_ = false;
};

class GLSurface;

// 2D texture. Cube/volume textures follow the same pattern.
class GLTexture final : public GLObject<IDirect3DTexture9> {
public:
    GLTexture(IDirect3DDevice9 *device, UINT width, UINT height, UINT levels,
              DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~GLTexture() override;

    // IDirect3DResource9
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    // IDirect3DBaseTexture9
    DWORD   WINAPI SetLOD(DWORD) override { return 0; }
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return levels_; }
    // IDirect3DTexture9
    HRESULT WINAPI GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT WINAPI GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
    HRESULT WINAPI LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT WINAPI UnlockRect(UINT Level) override;
    HRESULT WINAPI AddDirtyRect(const RECT *) override { return D3D_OK; }

    unsigned  glName() { if (!tex_ || pendLevels_) sync(); return tex_; }
    void      sync();
    bool      isDepth() const { return isDepth_; }   // depth-format (shadow map) texture
    // Retrofit: the engine's shadowmap image reaches CreateTexture with format AND
    // usage that don't identify it as depth (decompiled flag soup). When it shows up
    // at SetDepthStencilSurface, convert the storage to real depth + compare mode.
    void      ensureDepthStorage();
    UINT      width()  const { return width_; }
    UINT      height() const { return height_; }
    D3DFORMAT format() const { return format_; }

private:
    void createGL();              // glGen + storage/placeholder (GL thread only)
    void uploadLevel(UINT Level); // shadow -> GL upload for one mip (GL thread only)

    IDirect3DDevice9 *device_;
    unsigned          tex_ = 0;
    UINT              width_, height_, levels_;
    DWORD             usage_;
    D3DFORMAT         format_;
    D3DPOOL           pool_;
    std::vector<std::vector<unsigned char>> levelShadow_;  // CPU mirror per mip level
    UINT              lockLevel_ = 0;
    bool              dirty_     = false;
    unsigned          pendLevels_ = 0;  // bitmask of mips awaiting upload on the GL thread
    bool              isDepth_   = false;  // D24S8/D16-family: GL depth texture + compare mode
};

// A 3D (volume) texture — colour-grading LUTs etc. Maps to a GL_TEXTURE_3D.
class GLVolumeTexture final : public GLObject<IDirect3DVolumeTexture9> {
public:
    GLVolumeTexture(IDirect3DDevice9 *device, UINT w, UINT h, UINT d, UINT levels,
                    DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~GLVolumeTexture() override;
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    DWORD   WINAPI SetLOD(DWORD) override { return 0; }
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return levels_; }
    HRESULT WINAPI GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) override;
    HRESULT WINAPI LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *, DWORD) override;
    HRESULT WINAPI UnlockBox(UINT Level) override;

    unsigned  glName() { if (!tex_ || pendLevels_) sync(); return tex_; }
    void      sync();
    D3DFORMAT format() const { return format_; }
private:
    void createGL();
    void uploadLevel(UINT Level);

    IDirect3DDevice9 *device_;
    unsigned          tex_ = 0;
    UINT              width_, height_, depth_, levels_;
    DWORD             usage_;
    D3DFORMAT         format_;
    D3DPOOL           pool_;
    std::vector<std::vector<unsigned char>> levelShadow_;
    bool              dirty_ = false;
    unsigned          pendLevels_ = 0;
};

// A cube texture (sky / reflection probes). Maps to a GL_TEXTURE_CUBE_MAP; D3D's
// six D3DCUBEMAP_FACES (POSITIVE_X..NEGATIVE_Z = 0..5) line up exactly with GL's
// consecutive GL_TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z targets. Faces are square
// (edgeLen x edgeLen). Per-face/per-level CPU shadow backs LockRect like GLTexture.
class GLCubeTexture final : public GLObject<IDirect3DCubeTexture9> {
public:
    GLCubeTexture(IDirect3DDevice9 *device, UINT edgeLen, UINT levels,
                  DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~GLCubeTexture() override;

    // IDirect3DResource9
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    // IDirect3DBaseTexture9
    DWORD   WINAPI SetLOD(DWORD) override { return 0; }
    DWORD   WINAPI GetLOD() override { return 0; }
    DWORD   WINAPI GetLevelCount() override { return levels_; }
    // IDirect3DCubeTexture9
    HRESULT WINAPI GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
    HRESULT WINAPI GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level,
                                     IDirect3DSurface9 **ppCubeMapSurface) override;
    HRESULT WINAPI LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect,
                            const RECT *pRect, DWORD Flags) override;
    HRESULT WINAPI UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override;

    unsigned  glName() { if (!tex_ || pendAny_) sync(); return tex_; }
    void      maybeGenMips();
    void      sync();
    UINT      edgeLength() const { return edge_; }
    D3DFORMAT format() const { return format_; }

private:
    void createGL();
    void uploadFaceLevel(unsigned Face, UINT Level);

    IDirect3DDevice9 *device_;
    unsigned          tex_ = 0;
    UINT              edge_, levels_;
    DWORD             usage_;
    D3DFORMAT         format_;
    D3DPOOL           pool_;
    std::vector<std::vector<unsigned char>> levelShadow_[6];  // [face][level] CPU mirror
    bool              dirty_ = false;
    unsigned          pendLevels_[6] = {};  // per-face bitmask of pending mips
    bool              pendAny_ = false;
    unsigned          level0Faces_ = 0;     // faces with level-0 data uploaded
    bool              mipsGenned_ = false;  // auto-genned mips for a 1-mip cube
};

// Tag selecting the back-buffer constructor: a surface that is a view onto the
// window's default framebuffer (FBO 0) rather than any texture or CPU buffer.
struct GLBackbufferTag {};

// Tag selecting the depth-stencil constructor. The default framebuffer already
// owns a depth-stencil buffer and render-to-texture FBOs auto-allocate one, so a
// depth-stencil surface is currently a metadata-only handle (dims + format) that
// the renderer attaches to a render target; it carries no GL object of its own.
struct GLDepthStencilTag {};

// A surface is either a view onto one mip level of a texture (GetSurfaceLevel),
// a standalone render target (owns a GL texture, renderable + readable), a
// system-memory surface (owns a CPU buffer, lockable — the target of
// GetRenderTargetData and CreateOffscreenPlainSurface), or the back buffer
// (a view onto the default framebuffer, bound as FBO 0).
class GLSurface final : public GLObject<IDirect3DSurface9> {
public:
    GLSurface(GLTexture *owner, UINT level);                                       // texture-level view
    GLSurface(GLCubeTexture *owner, D3DCUBEMAP_FACES face, UINT level);            // cube-face view
    GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, // standalone
              bool sysmem);
    GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, // back buffer (FBO 0)
              GLBackbufferTag);
    GLSurface(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format, // depth-stencil handle
              GLDepthStencilTag);

    ~GLSurface() override;

    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_SURFACE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI GetContainer(REFIID, void **ppContainer) override;
    HRESULT WINAPI GetDesc(D3DSURFACE_DESC *pDesc) override;
    HRESULT WINAPI LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT WINAPI UnlockRect() override;

    unsigned  texName() const;  // renderable GL texture (FBO attachment), or 0 if sysmem
    UINT      level()   const { return level_; }
    UINT      width()   const { return width_; }
    UINT      height()  const { return height_; }
    D3DFORMAT format()  const { return format_; }
    bool      sysmem()  const { return sysmem_; }
    bool      isBackbuffer()   const { return backbuffer_; }
    bool      isDepthStencil() const { return depthStencil_; }
    bool      texIsDepthStencil() const { return ownTexIsDepth_; }
    GLTexture *ownerTex() const { return owner_; }
    std::vector<unsigned char> &shadow() { return shadow_; }

private:
    IDirect3DDevice9 *device_ = nullptr;  // null for texture-level views (delegate to owner)
    GLTexture        *owner_  = nullptr;  // texture-level view (non-owning)
    GLCubeTexture    *cubeOwner_ = nullptr; // cube-face view (non-owning)
    D3DCUBEMAP_FACES  cubeFace_ = D3DCUBEMAP_FACE_POSITIVE_X;
    unsigned          ownTex_ = 0;        // standalone render target: owned GL texture
    bool              ownTexIsDepth_ = false; // ownTex_ has DEPTH_STENCIL storage (shadow DS)
    UINT              level_  = 0;
    UINT              width_  = 0;
    UINT              height_ = 0;
    D3DFORMAT         format_ = D3DFMT_UNKNOWN;
    bool              sysmem_ = false;
    bool              backbuffer_ = false;   // view onto the default framebuffer (FBO 0)
    bool              depthStencil_ = false; // metadata-only depth-stencil handle
    std::vector<unsigned char> shadow_;   // sysmem backing for LockRect
};

// The window's swap chain. The GL backend renders directly into the default
// framebuffer, so the swap chain is a thin shim: Present() forwards to the
// device (which swaps the GL window), and GetBackBuffer() hands back the
// device's back-buffer surface (FBO 0). Created on demand by GetSwapChain().
class GLSwapChain final : public GLObject<IDirect3DSwapChain9> {
public:
    GLSwapChain(IDirect3DDevice9 *device, GLSurface *backbuffer)
        : device_(device), backbuffer_(backbuffer) {}

    HRESULT WINAPI Present(const RECT *src, const RECT *dst, HWND hwnd,
                           const RGNDATA *dirty, DWORD /*flags*/) override {
        return device_->Present(src, dst, hwnd, dirty);
    }
    // Read the displayed frame into a system-memory surface (the screenshot path).
    HRESULT WINAPI GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override;
    HRESULT WINAPI GetBackBuffer(UINT /*iBackBuffer*/, D3DBACKBUFFER_TYPE,
                                 IDirect3DSurface9 **ppBackBuffer) override {
        if (!ppBackBuffer) return E_INVALIDARG;
        backbuffer_->AddRef();
        *ppBackBuffer = backbuffer_;
        return D3D_OK;
    }

private:
    IDirect3DDevice9 *device_;
    GLSurface        *backbuffer_;  // non-owning; owned by the device
};

class GLVertexDeclaration final : public GLObject<IDirect3DVertexDeclaration9> {
public:
    GLVertexDeclaration(IDirect3DDevice9 *device, const D3DVERTEXELEMENT9 *elements);
    ~GLVertexDeclaration() override;   // bumps g_kbVaoEpoch (cached VAOs key on the decl address)

    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT WINAPI GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override;

    const std::vector<D3DVERTEXELEMENT9> &elements() const { return elements_; }

private:
    IDirect3DDevice9              *device_;
    std::vector<D3DVERTEXELEMENT9> elements_;  // excludes the D3DDECL_END terminator
};

#endif // KISAK_GL_RESOURCES_H
