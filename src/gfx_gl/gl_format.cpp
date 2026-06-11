// gl_format.cpp — D3DFORMAT → OpenGL format mapping.
#include "gl_format.h"

#include <GL/glew.h>

bool D3DToGLFormat(D3DFORMAT fmt, unsigned *internal, unsigned *format, unsigned *type, int *bpp) {
#if defined(__EMSCRIPTEN__)
    // WebGL2 (GLES3) is far stricter than desktop GL about texImage2D enums: it has
    // NO GL_BGRA upload format, NO GL_UNSIGNED_INT_8_8_8_8[_REV] packed type, and no
    // sized GL_RGB5 / GL_LUMINANCE8_ALPHA8 internal formats — passing any of those
    // gives GL_INVALID_ENUM (0x500). So the BGRA byte-order D3D formats upload as
    // plain RGBA8 / GL_RGBA / GL_UNSIGNED_BYTE, and the upload site swaps B<->R in the
    // bytes (D3DFormatNeedsBGRASwizzle). R5G6B5 uses the GLES sized GL_RGB565; A8L8
    // uses the legacy *unsized* GL_LUMINANCE_ALPHA that WebGL2 still accepts.
    switch (fmt) {
        case D3DFMT_A8R8G8B8: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bpp = 4; return true;
        case D3DFMT_X8R8G8B8: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bpp = 4; return true;
        case D3DFMT_A8B8G8R8: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bpp = 4; return true;
        case D3DFMT_R5G6B5:   *internal = GL_RGB565; *format = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; *bpp = 2; return true;
        case D3DFMT_A8:       *internal = GL_R8; *format = GL_RED; *type = GL_UNSIGNED_BYTE; *bpp = 1; return true;
        case D3DFMT_L8:       *internal = GL_R8; *format = GL_RED; *type = GL_UNSIGNED_BYTE; *bpp = 1; return true;
        case D3DFMT_A8L8:     *internal = GL_LUMINANCE_ALPHA; *format = GL_LUMINANCE_ALPHA; *type = GL_UNSIGNED_BYTE; *bpp = 2; return true;
        case D3DFMT_A16B16G16R16F: *internal = GL_RGBA16F; *format = GL_RGBA; *type = GL_HALF_FLOAT; *bpp = 8; return true;
        case D3DFMT_G16R16F:  *internal = GL_RG16F; *format = GL_RG; *type = GL_HALF_FLOAT; *bpp = 4; return true;
        case D3DFMT_R32F:     *internal = GL_R32F; *format = GL_RED; *type = GL_FLOAT; *bpp = 4; return true;
        // G16R16 is the secondary lightmap (lightmaps[].secondaryB, "lightmapLum") and the
        // "$g16r16" default. 16-bit *normalized* RG (GL_RG16) is absent from WebGL2 core
        // (needs EXT_texture_norm16), so we down-convert to 8-bit RG8 at upload time (see
        // the G16R16 branch in GLTexture::UnlockRect) — 8 bits is plenty for a smooth
        // lightmap. WITHOUT this the secondary lightmap never uploads and samples as the
        // neutral (~white) placeholder, washing the world out (overblown lighting). bpp=4
        // keeps the lock/shadow buffer sized for the incoming 16-bit source pixels.
        case D3DFMT_G16R16:   *internal = GL_RG8; *format = GL_RG; *type = GL_UNSIGNED_BYTE; *bpp = 4; return true;
        // A16B16G16R16 (GL_RGBA16) likewise absent from WebGL2 core; leave unhandled until needed.
        default: return false;
    }
#else
    // D3D's *A8R8G8B8 is BGRA byte order in memory, so upload it as GL_BGRA.
    switch (fmt) {
        case D3DFMT_A8R8G8B8: *internal = GL_RGBA8; *format = GL_BGRA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_X8R8G8B8: *internal = GL_RGB8;  *format = GL_BGRA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_A8B8G8R8: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_R5G6B5:   *internal = GL_RGB5;  *format = GL_RGB;  *type = GL_UNSIGNED_SHORT_5_6_5;     *bpp = 2; return true;
        case D3DFMT_A8:       *internal = GL_R8;    *format = GL_RED;  *type = GL_UNSIGNED_BYTE;            *bpp = 1; return true;
        case D3DFMT_L8:       *internal = GL_R8;    *format = GL_RED;  *type = GL_UNSIGNED_BYTE;            *bpp = 1; return true;
        // A8L8: luminance+alpha. The compatibility context (#version 120 GLSL with
        // gl_FragColor/texture2D) keeps GL_LUMINANCE_ALPHA, which samples as (L,L,L,A)
        // exactly like D3D — so shaders reading .rgb (luminance) and .a (alpha) match.
        case D3DFMT_A8L8:     *internal = GL_LUMINANCE8_ALPHA8; *format = GL_LUMINANCE_ALPHA; *type = GL_UNSIGNED_BYTE; *bpp = 2; return true;
        // Float / deep render-target formats (HDR scene, bloom, depth resolves). These
        // are rendered to and sampled entirely on the GPU, so only the GL internal
        // format matters; an unmapped HDR target gets no storage and samples as garbage
        // (which composites a colour cast onto the 3D scene). D3D's *16R16F etc. are
        // R-first in memory, matching GL_RGBA/RG/RED order.
        case D3DFMT_A16B16G16R16F: *internal = GL_RGBA16F; *format = GL_RGBA; *type = GL_HALF_FLOAT;     *bpp = 8; return true;
        case D3DFMT_A16B16G16R16:  *internal = GL_RGBA16;  *format = GL_RGBA; *type = GL_UNSIGNED_SHORT;  *bpp = 8; return true;
        case D3DFMT_G16R16F:       *internal = GL_RG16F;   *format = GL_RG;   *type = GL_HALF_FLOAT;      *bpp = 4; return true;
        case D3DFMT_G16R16:        *internal = GL_RG16;    *format = GL_RG;   *type = GL_UNSIGNED_SHORT;   *bpp = 4; return true;
        case D3DFMT_R32F:          *internal = GL_R32F;    *format = GL_RED;  *type = GL_FLOAT;            *bpp = 4; return true;
        default: return false;
    }
#endif
}

bool D3DFormatNeedsBGRASwizzle(D3DFORMAT fmt) {
#if defined(__EMSCRIPTEN__)
    // These are the BGRA byte-order formats remapped to GL_RGBA above; their bytes
    // need B and R swapped at upload so the sampled colour is correct.
    return fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8;
#else
    (void)fmt; return false;   // desktop uploads BGRA directly via GL_BGRA
#endif
}

int D3DFormatBpp(D3DFORMAT fmt) {
    unsigned i, f, t; int bpp;
    return D3DToGLFormat(fmt, &i, &f, &t, &bpp) ? bpp : 4;
}

unsigned D3DCompressedGLFormat(D3DFORMAT fmt, int *blockBytes) {
    switch ((unsigned)fmt) {
        case 0x31545844: if (blockBytes) *blockBytes = 8;  return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;  // 'DXT1'
        case 0x33545844: if (blockBytes) *blockBytes = 16; return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;  // 'DXT3'
        case 0x35545844: if (blockBytes) *blockBytes = 16; return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;  // 'DXT5'
        default: return 0;
    }
}
