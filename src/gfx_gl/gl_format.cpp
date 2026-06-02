// gl_format.cpp — D3DFORMAT → OpenGL format mapping.
#include "gl_format.h"

#include <GL/glew.h>

bool D3DToGLFormat(D3DFORMAT fmt, unsigned *internal, unsigned *format, unsigned *type, int *bpp) {
    // D3D's *A8R8G8B8 is BGRA byte order in memory, so upload it as GL_BGRA.
    switch (fmt) {
        case D3DFMT_A8R8G8B8: *internal = GL_RGBA8; *format = GL_BGRA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_X8R8G8B8: *internal = GL_RGB8;  *format = GL_BGRA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_A8B8G8R8: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_INT_8_8_8_8_REV; *bpp = 4; return true;
        case D3DFMT_R5G6B5:   *internal = GL_RGB5;  *format = GL_RGB;  *type = GL_UNSIGNED_SHORT_5_6_5;     *bpp = 2; return true;
        case D3DFMT_A8:       *internal = GL_R8;    *format = GL_RED;  *type = GL_UNSIGNED_BYTE;            *bpp = 1; return true;
        case D3DFMT_L8:       *internal = GL_R8;    *format = GL_RED;  *type = GL_UNSIGNED_BYTE;            *bpp = 1; return true;
        default: return false;
    }
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
