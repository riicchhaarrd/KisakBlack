// gl_state.cpp — render-state, sampler-state and texture binding translation.
//
// Translates the D3D9 state the renderer sets into immediate GL calls. Only the
// states the engine actually uses are handled (extracted from the renderer);
// unhandled states are ignored rather than asserting, so new ones degrade
// gracefully. Alpha test (removed from core GL) and the stage-0 fixed-function
// combine (D3DTSS_COLOROP/COLORARG1/2) are tracked here and folded into the
// built-in fragment shader at draw time (see gl_d3d9_draw.cpp).
#include "gl_d3d9.h"
#include "gl_resources.h"

#include <GL/glew.h>
extern "C" void KB_FlushBatchedDraws();  // batched-draw flush (gl_d3d9_draw.cpp)


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
    KB_FlushBatchedDraws();
    // Skip the GL call when this state already holds this value — on the proxied web
    // context every glEnable/glBlendFunc/glDepthMask is a cross-thread marshaled call, and
    // the engine re-sets the same blend/depth/cull states constantly between draws.
    if ((unsigned)State < 256) {
        if (rsSet_[State] && rsCache_[State] == Value) return D3D_OK;
        rsSet_[State] = 1; rsCache_[State] = Value;
    }
    switch (State) {
        case D3DRS_ZENABLE:
            if (Value) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
            break;
        case D3DRS_ZWRITEENABLE:    glDepthMask(Value ? GL_TRUE : GL_FALSE); break;
        case D3DRS_DEPTHBIAS:
        case D3DRS_SLOPESCALEDEPTHBIAS: {
            // The engine biases the shadowmap build (r_state.cpp). Dropping these gave
            // every near-cascade texel self-shadowing acne = black gun/surfaces up
            // close, shimmering as the map re-rendered each frame. D3D bias is in
            // normalized depth units; GL units are smallest-resolvable-depth steps
            // (~2^-24 for D24), hence the 2^24 scale.
            DWORD bBits = (State == D3DRS_DEPTHBIAS) ? Value : rsCache_[D3DRS_DEPTHBIAS];
            DWORD sBits = (State == D3DRS_SLOPESCALEDEPTHBIAS) ? Value : rsCache_[D3DRS_SLOPESCALEDEPTHBIAS];
            float bias, slope;
            memcpy(&bias, &bBits, 4); memcpy(&slope, &sBits, 4);
            if (bias != 0.0f || slope != 0.0f) {
                extern unsigned long g_kbBiasSets; ++g_kbBiasSets;
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(slope, bias * 16777216.0f);
            } else {
                glPolygonOffset(0.0f, 0.0f);
                glDisable(GL_POLYGON_OFFSET_FILL);
            }
            break;
        }
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
        // Alpha test — foliage/grass/fences use an alpha-cutout; without it their
        // transparent (alpha-0) texels render opaque. Keep the compatibility-profile
        // state for programmable shaders and mirror it into the built-in shader state.
        case D3DRS_ALPHATESTENABLE:
            alphaTest_.enable = (Value != 0);
            alphaTestOn_ = (Value != 0);
#ifndef __EMSCRIPTEN__
            if (Value) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
#endif
            // On WebGL2/GLES the cutout is done with discard in-shader (the func/ref
            // become uAlphaTestFunc/uAlphaRef uniforms uploaded in useDrawProgram).
            break;
        case D3DRS_ALPHAFUNC:
            alphaTest_.func = Value;
            alphaFunc_ = Value;
#ifndef __EMSCRIPTEN__
            glAlphaFunc(glCmp(alphaFunc_), (GLfloat)alphaRef_ / 255.0f);
#endif
            break;
        case D3DRS_ALPHAREF:
            alphaTest_.ref = Value & 0xff;
            alphaRef_ = Value & 0xff;
#ifndef __EMSCRIPTEN__
            glAlphaFunc(glCmp(alphaFunc_), (GLfloat)alphaRef_ / 255.0f);
#endif
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
    KB_FlushBatchedDraws();
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

// Fixed-function texture-stage state. Only stage 0 drives the built-in program's
// tex/diffuse combine (D3DTSS_COLOROP/COLORARG1/COLORARG2); other stages and the
// alpha-op cascade are accepted and ignored (the 2D path never uses them).
HRESULT WINAPI GLDevice::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage != 0) return D3D_OK;
    switch (Type) {
        case D3DTSS_COLOROP:   texStage0_.colorOp   = Value; break;
        case D3DTSS_COLORARG1: texStage0_.colorArg1 = Value; break;
        case D3DTSS_COLORARG2: texStage0_.colorArg2 = Value; break;
        default: break;  // ALPHAOP/ALPHAARG* etc.: accepted, not yet emulated
    }
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
    KB_FlushBatchedDraws();
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
        default: {  // D3DRTYPE_TEXTURE
            GLTexture *t = static_cast<GLTexture *>(static_cast<IDirect3DTexture9 *>(pTexture));
            name   = t->glName();
            target = GL_TEXTURE_2D;
            boundTexIsDepth_[Stage] = t->isDepth();   // shadow-map sampling needs a shadow sampler
            break;
        }
        }
    }
    if (!pTexture || target != GL_TEXTURE_2D) boundTexIsDepth_[Stage] = false;
    boundTexName_[Stage]   = name;
    boundTexTarget_[Stage] = target;
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetScissorRect(const RECT *pRect) {
    KB_FlushBatchedDraws();
    if (pRect)
        // Y flip only for the window target — see SetViewport (FBO sub-rects keep
        // D3D placement so sampled atlases match D3D-convention coordinates).
        glScissor(pRect->left,
                  (fboActive_ && dsLive_ && (fbWidth_ != bbWidth_ || fbHeight_ != bbHeight_))
                      ? pRect->top : fbHeight_ - pRect->bottom,
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

// Apply a sampler stage's D3D filter/wrap to the texture currently bound on that
// unit (the shader path binds s0..s15 itself). Without this only stage 0 got its
// state, so lookup/lightmap textures that need CLAMP wrapped as REPEAT and read
// wrong edge values — wrong lighting and edge artifacts. Min filter stays
// non-mipmapped to match texture completeness (not every level is guaranteed).
void GLDevice::applyStageSampler(unsigned stage, unsigned target) {
    if (stage >= (unsigned)kMaxStages) return;
    const GLSamplerState &s = samplers_[stage];
    // DEPTH textures (shadowmaps, sampled raw with compare NONE): LINEAR makes the
    // texture INCOMPLETE in ES3 (depth + linear is unfilterable) -> every sample
    // returns 0 -> everything reads "fully in shadow" (the black gun/world). The
    // engine's shadowmap sampler state asks for bilinear; force NEAREST instead.
    if (boundTexIsDepth_[stage]) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, glFilter(s.minFilter));
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, glFilter(s.magFilter));
    }
    glTexParameteri(target, GL_TEXTURE_WRAP_S,     glWrap(s.addressU));
    glTexParameteri(target, GL_TEXTURE_WRAP_T,     glWrap(s.addressV));
}
