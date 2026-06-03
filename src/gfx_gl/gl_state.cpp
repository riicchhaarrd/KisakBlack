// gl_state.cpp — render-state, sampler-state and texture binding translation.
//
// Translates the D3D9 state the renderer sets into immediate GL calls. Only the
// states the engine actually uses are handled (extracted from the renderer);
// unhandled states are ignored rather than asserting, so new ones degrade
// gracefully. Alpha test (removed from core GL) and the fixed-function texture
// blend cascade (D3DTSS_*) are not handled here yet — TODO(task #5: fold into
// the shader path).
#include "gl_d3d9.h"
#include "gl_resources.h"

#include <GL/glew.h>

namespace {

GLenum glBlend(DWORD d) {
    switch (d) {
        case D3DBLEND_ZERO:        return GL_ZERO;
        case D3DBLEND_ONE:         return GL_ONE;
        case D3DBLEND_SRCALPHA:    return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        default:                   return GL_ONE;
    }
}

GLenum glCmp(DWORD d) {
    switch (d) {
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

GLenum glBlendEq(DWORD d) {
    switch (d) {
        case D3DBLENDOP_SUBTRACT:    return GL_FUNC_SUBTRACT;
        case D3DBLENDOP_REVSUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case D3DBLENDOP_MIN:         return GL_MIN;
        case D3DBLENDOP_MAX:         return GL_MAX;
        default:                     return GL_FUNC_ADD;
    }
}

GLint glFilter(DWORD d) { return d == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR; }
GLint glWrap(DWORD d)   { return d == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT; }

} // namespace

HRESULT WINAPI GLDevice::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    switch (State) {
        case D3DRS_ZENABLE:
            if (Value) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            break;
        case D3DRS_ZWRITEENABLE:    glDepthMask(Value ? GL_TRUE : GL_FALSE); break;
        case D3DRS_ZFUNC:           glDepthFunc(glCmp(Value));               break;
        case D3DRS_CULLMODE:
            // The vertex path flips Y in clip space (D3D's Y-down screen -> GL's
            // Y-up), which reverses triangle winding. So the GL front face is the
            // inverse of the naive D3D->GL mapping: D3DCULL_CW -> GL_CCW and
            // D3DCULL_CCW -> GL_CW. (Without this every visible triangle is culled
            // as a back face and the whole frame renders black.)
            if (Value == D3DCULL_NONE) { glDisable(GL_CULL_FACE); }
            else {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
                glFrontFace(Value == D3DCULL_CW ? GL_CCW : GL_CW);
            }
            break;
        case D3DRS_ALPHABLENDENABLE:
            if (Value) glEnable(GL_BLEND); else glDisable(GL_BLEND);
            break;
        case D3DRS_SRCBLEND:  blendSrc_  = Value; glBlendFunc(glBlend(blendSrc_), glBlend(blendDest_)); break;
        case D3DRS_DESTBLEND: blendDest_ = Value; glBlendFunc(glBlend(blendSrc_), glBlend(blendDest_)); break;
        case D3DRS_BLENDOP:   glBlendEquation(glBlendEq(Value)); break;
        case D3DRS_COLORWRITEENABLE:
            glColorMask((Value & D3DCOLORWRITEENABLE_RED)   ? GL_TRUE : GL_FALSE,
                        (Value & D3DCOLORWRITEENABLE_GREEN) ? GL_TRUE : GL_FALSE,
                        (Value & D3DCOLORWRITEENABLE_BLUE)  ? GL_TRUE : GL_FALSE,
                        (Value & D3DCOLORWRITEENABLE_ALPHA) ? GL_TRUE : GL_FALSE);
            break;
        case D3DRS_SCISSORTESTENABLE:
            if (Value) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
            break;
        case D3DRS_FILLMODE:
            glPolygonMode(GL_FRONT_AND_BACK, Value == D3DFILL_WIREFRAME ? GL_LINE : GL_FILL);
            break;
        default:
            break;  // unhandled state: ignore (see file header)
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler >= (DWORD)kMaxStages) return D3D_OK;
    GLSamplerState &s = samplers_[Sampler];
    switch (Type) {
        case D3DSAMP_MINFILTER: s.minFilter = Value; break;
        case D3DSAMP_MAGFILTER: s.magFilter = Value; break;
        case D3DSAMP_ADDRESSU:  s.addressU  = Value; break;
        case D3DSAMP_ADDRESSV:  s.addressV  = Value; break;
        default: break;
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
    if (Stage >= (DWORD)kMaxStages) return D3D_OK;
    // Resolve to a GL name + target now (via the resource's D3D type) so the bind
    // path stays type-correct for 2D, cube and volume textures alike — a blind
    // downcast to GLTexture would read tex_ at the wrong offset for cube/volume.
    unsigned name = 0, target = GL_TEXTURE_2D;
    if (pTexture) {
        switch (pTexture->GetType()) {
        case D3DRTYPE_CUBETEXTURE:
            name   = static_cast<GLCubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(pTexture))->glName();
            target = GL_TEXTURE_CUBE_MAP;
            break;
        case D3DRTYPE_VOLUMETEXTURE:
            name   = static_cast<GLVolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(pTexture))->glName();
            target = GL_TEXTURE_3D;
            break;
        default:  // D3DRTYPE_TEXTURE
            name   = static_cast<GLTexture *>(static_cast<IDirect3DTexture9 *>(pTexture))->glName();
            target = GL_TEXTURE_2D;
            break;
        }
    }
    boundTexName_[Stage]   = name;
    boundTexTarget_[Stage] = target;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetScissorRect(const RECT *pRect) {
    if (pRect)
        glScissor(pRect->left, fbHeight_ - pRect->bottom,
                  pRect->right - pRect->left, pRect->bottom - pRect->top);
    return D3D_OK;
}

// Bind the stage-0 texture for the built-in single-texture program. Returns true
// if a texture is bound (caller flips the program's uUseTexture uniform).
bool GLDevice::applyTextures() {
    unsigned name = boundTexName_[0], target = boundTexTarget_[0];
    if (!name) return false;
    const GLSamplerState &s = samplers_[0];
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(target, name);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, glFilter(s.minFilter));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, glFilter(s.magFilter));
    glTexParameteri(target, GL_TEXTURE_WRAP_S,     glWrap(s.addressU));
    glTexParameteri(target, GL_TEXTURE_WRAP_T,     glWrap(s.addressV));
    return true;
}
