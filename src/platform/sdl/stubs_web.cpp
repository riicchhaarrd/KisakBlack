//============================================================================
// stubs_web.cpp  (Emscripten / WebGL2 only)
//
// Link-time stubs for symbols the engine references that have no implementation
// on the wasm32 / WebGL2 target. Mirrors the role of stubs_bink_nvapi.cpp on the
// Linux desktop build. Two groups:
//
//   1. Desktop-GL entry points absent from WebGL2 / GLES3. The gfx_gl D3D9->GL
//      layer calls a handful of fixed-function / desktop-only GL functions. Here
//      they are mapped onto the nearest WebGL2 primitive, or made no-ops where
//      the feature simply does not exist on the web (wireframe, fixed-function
//      alpha test). The real alpha-test-via-discard work lands in milestone 5.
//
//   2. libvpx (VP8/VP9 encoder, used only for in-game video recording/streaming).
//      Not compiled for the web; stub to "unavailable" so the module links. These
//      are normally dead-code-eliminated (vpx_init/encode aren't on the boot path)
//      but we define the whole surface so a link never depends on DCE.
//
// The entire file is compiled out on every non-Emscripten target.
//============================================================================
#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
#include <cstdio>

extern "C" {

//--- 1. Missing desktop-GL functions -----------------------------------------

// Fixed-function alpha test does not exist in GLES3/WebGL2; it is emulated with
// `discard` in the translated fragment shaders (milestone 5). No-op the state
// setter so existing call sites link and run.
void glAlphaFunc(GLenum /*func*/, GLclampf /*ref*/) {}

// No polygon-fill mode (wireframe) in GLES3/WebGL2. No-op.
void glPolygonMode(GLenum /*face*/, GLenum /*mode*/) {}

// Single-target draw-buffer select -> the GLES3 array form. GL_BACK on the
// default framebuffer maps to GL_BACK; an FBO attachment passes straight through.
void glDrawBuffer(GLenum mode) {
    GLenum bufs[1] = { mode };
    glDrawBuffers(1, bufs);
}

// Occurrence query result: GLES3 exposes the unsigned variant only. Forward.
void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params) {
    GLuint tmp = 0;
    glGetQueryObjectuiv(id, pname, &tmp);
    if (params) *params = (GLint)tmp;
}

// No base-vertex draw in WebGL2. For milestone 2 we forward to plain
// glDrawElements; correct whenever BaseVertexIndex == 0 (the common path).
// NOTE: a non-zero base vertex is silently ignored here — full emulation (fold
// the base vertex into the bound attribute offsets) is a milestone-5 renderer
// task and is flagged in WEB_PORT_TRIAGE.md.
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                              const void *indices, GLint basevertex) {
    static int warned = 0;
    if (basevertex != 0 && !warned) {
        warned = 1;
        fprintf(stderr, "[web] glDrawElementsBaseVertex: basevertex=%d ignored "
                        "(WebGL2 has no base-vertex draw; see M5)\n", basevertex);
    }
    glDrawElements(mode, count, type, indices);
}

} // extern "C"


//--- 2. libvpx (VP8/VP9 encoder) — unavailable on web ------------------------
// Pull the real vpx headers for exact types, then define the symbols the engine
// references to a safe "error / no data" result.
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>

extern "C" {

vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t * /*ctx*/) {
    return VPX_CODEC_OK; // nothing to tear down
}

vpx_codec_err_t vpx_codec_encode(vpx_codec_ctx_t * /*ctx*/, const vpx_image_t * /*img*/,
                                 vpx_codec_pts_t /*pts*/, unsigned long /*duration*/,
                                 vpx_enc_frame_flags_t /*flags*/, unsigned long /*deadline*/) {
    return VPX_CODEC_ERROR; // encoder not available
}

const vpx_codec_cx_pkt_t *vpx_codec_get_cx_data(vpx_codec_ctx_t * /*ctx*/,
                                                vpx_codec_iter_t * /*iter*/) {
    return nullptr; // no encoded data
}

void vpx_img_free(vpx_image_t * /*img*/) {}

// The remaining vpx entry points vpx.cpp references are normally DCE'd off the
// boot path; define them too so linking never relies on that.
vpx_image_t *vpx_img_alloc(vpx_image_t *img, vpx_img_fmt_t /*fmt*/,
                           unsigned int /*d_w*/, unsigned int /*d_h*/, unsigned int /*align*/) {
    return img ? nullptr : nullptr; // allocation fails -> caller treats vpx as off
}

vpx_codec_err_t vpx_codec_enc_config_default(vpx_codec_iface_t * /*iface*/,
                                             vpx_codec_enc_cfg_t * /*cfg*/, unsigned int /*usage*/) {
    return VPX_CODEC_ERROR;
}

vpx_codec_err_t vpx_codec_enc_init_ver(vpx_codec_ctx_t * /*ctx*/, vpx_codec_iface_t * /*iface*/,
                                       const vpx_codec_enc_cfg_t * /*cfg*/, vpx_codec_flags_t /*flags*/,
                                       int /*ver*/) {
    return VPX_CODEC_ERROR;
}

} // extern "C"

#endif // __EMSCRIPTEN__
