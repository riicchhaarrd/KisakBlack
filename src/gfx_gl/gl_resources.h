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

    unsigned glName() const { return vbo_; }

private:
    IDirect3DDevice9         *device_;
    unsigned                  vbo_ = 0;
    UINT                      length_;
    DWORD                     usage_;
    DWORD                     fvf_;
    D3DPOOL                   pool_;
    std::vector<unsigned char> shadow_;  // CPU mirror backing Lock()
    UINT                      lockOffset_ = 0;
    UINT                      lockSize_   = 0;
    bool                      dirty_      = false;
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

    unsigned glName() const { return ibo_; }
    D3DFORMAT format() const { return format_; }

private:
    IDirect3DDevice9         *device_;
    unsigned                  ibo_ = 0;
    UINT                      length_;
    DWORD                     usage_;
    D3DFORMAT                 format_;
    D3DPOOL                   pool_;
    std::vector<unsigned char> shadow_;
    UINT                      lockOffset_ = 0;
    UINT                      lockSize_   = 0;
    bool                      dirty_      = false;
};

class GLSurface;

// 2D texture. Cube/volume textures will follow the same pattern.
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

    unsigned  glName() const { return tex_; }
    UINT      width()  const { return width_; }
    UINT      height() const { return height_; }
    D3DFORMAT format() const { return format_; }

private:
    IDirect3DDevice9 *device_;
    unsigned          tex_ = 0;
    UINT              width_, height_, levels_;
    DWORD             usage_;
    D3DFORMAT         format_;
    D3DPOOL           pool_;
    std::vector<std::vector<unsigned char>> levelShadow_;  // CPU mirror per mip level
    UINT              lockLevel_ = 0;
    bool              dirty_     = false;
};

// A surface is a view onto one mip level of a texture (from GetSurfaceLevel).
// Standalone render-target/depth surfaces will extend this later.
class GLSurface final : public GLObject<IDirect3DSurface9> {
public:
    GLSurface(GLTexture *owner, UINT level);

    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    D3DRESOURCETYPE WINAPI GetType() override { return D3DRTYPE_SURFACE; }
    DWORD   WINAPI SetPriority(DWORD) override { return 0; }
    DWORD   WINAPI GetPriority() override { return 0; }
    void    WINAPI PreLoad() override {}
    HRESULT WINAPI GetContainer(REFIID, void **ppContainer) override;
    HRESULT WINAPI GetDesc(D3DSURFACE_DESC *pDesc) override;
    HRESULT WINAPI LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
    HRESULT WINAPI UnlockRect() override;

    // For binding as an FBO colour attachment.
    unsigned texName() const { return owner_->glName(); }
    UINT     level()   const { return level_; }
    UINT     width()   const { return owner_->width()  >> level_ ? owner_->width()  >> level_ : 1; }
    UINT     height()  const { return owner_->height() >> level_ ? owner_->height() >> level_ : 1; }

private:
    GLTexture *owner_;  // non-owning: the texture outlives its surface views
    UINT       level_;
};

class GLVertexDeclaration final : public GLObject<IDirect3DVertexDeclaration9> {
public:
    GLVertexDeclaration(IDirect3DDevice9 *device, const D3DVERTEXELEMENT9 *elements);

    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT WINAPI GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override;

    const std::vector<D3DVERTEXELEMENT9> &elements() const { return elements_; }

private:
    IDirect3DDevice9              *device_;
    std::vector<D3DVERTEXELEMENT9> elements_;  // excludes the D3DDECL_END terminator
};

#endif // KISAK_GL_RESOURCES_H
