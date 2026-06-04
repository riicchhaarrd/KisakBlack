// gl_program.cpp — programmable shader path: create/bind shaders, upload the c#
// constant registers, and link+cache the bound (vs, ps) pair into a GL program.
#include "gl_d3d9.h"
#include "gl_shader.h"
#include "gl_resources.h"

#include <GL/glew.h>
#include <cstdio>
#include <cstring>

HRESULT WINAPI GLDevice::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLVertexShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
    if (!ppShader || !pFunction) return E_INVALIDARG;
    *ppShader = new GLPixelShader(this, pFunction);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexShader(IDirect3DVertexShader9 *pShader) {
    vs_ = static_cast<GLVertexShader *>(pShader);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShader(IDirect3DPixelShader9 *pShader) {
    ps_ = static_cast<GLPixelShader *>(pShader);
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetVertexShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    if (pData && StartRegister + Vec4Count <= 256)
        std::memcpy(vsConst_ + StartRegister * 4, pData, Vec4Count * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT WINAPI GLDevice::SetPixelShaderConstantF(UINT StartRegister, const float *pData, UINT Vec4Count) {
    if (pData && StartRegister + Vec4Count <= 256)
        std::memcpy(psConst_ + StartRegister * 4, pData, Vec4Count * 4 * sizeof(float));
    return D3D_OK;
}

// Pick the draw program: the linked (vs,ps) pair if both are bound and valid,
// otherwise the built-in pre-transformed program. Sets the program's uniforms.
void GLDevice::useDrawProgram() {
    if (!(vs_ && ps_ && vs_->ok() && ps_->ok())) {
        bindBuiltinForDraw();
        return;
    }

    uint64_t key = (uint64_t(vs_->glShader()) << 32) | ps_->glShader();
    auto it = progCache_.find(key);
    if (it == progCache_.end()) {
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vs_->glShader());
        glAttachShader(prog, ps_->glShader());
        // Match the device's canonical attribute locations (see gl_shader.cpp).
        GLBindAttribLocations(prog);
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            fprintf(stderr, "[gl] program link failed: %s\n", log);
        }
        LinkedProgram lp{prog,
                         glGetUniformLocation(prog, "vsc"),
                         glGetUniformLocation(prog, "psc")};
#ifdef __EMSCRIPTEN__
        // Alpha-test-via-discard uniforms (present only in ES fragment shaders).
        lp.alphaFuncLoc = glGetUniformLocation(prog, "uAlphaTestFunc");
        lp.alphaRefLoc  = glGetUniformLocation(prog, "uAlphaRef");
#endif
        // Query the sampler uniform locations ONCE here, not per-draw: each
        // glGetUniformLocation is a synchronous round-trip on the proxied web context.
        for (int i = 0; i < kMaxStages; ++i) {
            char name[4];                 // "s0".."s15"
            snprintf(name, sizeof(name), "s%d", i);
            lp.samplerLoc[i] = glGetUniformLocation(prog, name);
        }
        extern unsigned long g_kbProgLinks;
        ++g_kbProgLinks;
        it = progCache_.emplace(key, lp).first;
    }

    const LinkedProgram &lp = it->second;
    glUseProgram(lp.prog);
    if (lp.vscLoc >= 0) glUniform4fv(lp.vscLoc, 256, vsConst_);
    if (lp.pscLoc >= 0) glUniform4fv(lp.pscLoc, 256, psConst_);

#ifdef __EMSCRIPTEN__
    // Feed the in-shader alpha test. uAlphaTestFunc carries the D3DCMP_* value
    // (1..8) when enabled, 0 when disabled; uAlphaRef is the normalized [0,1] ref.
    if (lp.alphaFuncLoc >= 0)
        glUniform1i(lp.alphaFuncLoc, alphaTestOn_ ? (int)alphaFunc_ : 0);
    if (lp.alphaRefLoc >= 0)
        glUniform1f(lp.alphaRefLoc, (float)alphaRef_ / 255.0f);
#endif

    // Bind each referenced sampler s# to texture unit # and the matching texture.
    // Uses the cached location (queried once at link) — no per-draw glGetUniformLocation.
    for (int i = 0; i < kMaxStages; ++i) {
        int loc = lp.samplerLoc[i];
        if (loc < 0) continue;
        glUniform1i(loc, i);
        if (boundTexName_[i]) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(boundTexTarget_[i], boundTexName_[i]);
            applyStageSampler(i, boundTexTarget_[i]);
        }
    }
}
