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

// Poll the async link for completion (NON-blocking on web via KHR_parallel_shader_compile)
// and, once linked, cache all uniform locations. Returns false while still compiling.
// Blocking on glGetProgramiv(GL_LINK_STATUS) here would serialize ~120 synchronous shader
// compiles on the DOM thread on the first 3D frame -> multi-second "page unresponsive".
bool GLDevice::finalizeProgram(LinkedProgram &lp) {
#if defined(__EMSCRIPTEN__)
    static int parallel = -1;               // -1 unknown, 0 unsupported, 1 supported
    if (parallel < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        parallel = (ext && strstr(ext, "parallel_shader_compile")) ? 1 : 0;
    }
    if (parallel) {
        GLint done = 0;
        glGetProgramiv(lp.prog, 0x91B1 /*GL_COMPLETION_STATUS_KHR*/, &done);
        if (!done) return false;            // still compiling -> caller skips the draw
    }
#endif
    GLint ok = 0;
    glGetProgramiv(lp.prog, GL_LINK_STATUS, &ok);   // ready now (or native): does not stall
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(lp.prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[gl] program link failed: %s\n", log);
    }
    lp.vscLoc = glGetUniformLocation(lp.prog, "vsc");
    lp.pscLoc = glGetUniformLocation(lp.prog, "psc");
#ifdef __EMSCRIPTEN__
    lp.alphaFuncLoc = glGetUniformLocation(lp.prog, "uAlphaTestFunc");
    lp.alphaRefLoc  = glGetUniformLocation(lp.prog, "uAlphaRef");
#endif
    for (int i = 0; i < kMaxStages; ++i) {
        char name[4]; snprintf(name, sizeof(name), "s%d", i);
        lp.samplerLoc[i] = glGetUniformLocation(lp.prog, name);
    }
    lp.ready = true;
    return true;
}

// Pick the draw program: the linked (vs,ps) pair if both are bound and valid,
// otherwise the built-in pre-transformed program. Sets the program's uniforms.
// Returns false if the (vs,ps) program is still linking — caller must skip the draw.
bool GLDevice::useDrawProgram() {
    if (!(vs_ && ps_ && vs_->ok() && ps_->ok())) {
        bindBuiltinForDraw();
        return true;
    }

    uint64_t key = (uint64_t(vs_->glShader()) << 32) | ps_->glShader();
    auto it = progCache_.find(key);
    if (it == progCache_.end()) {
        unsigned prog = glCreateProgram();
        glAttachShader(prog, vs_->glShader());
        glAttachShader(prog, ps_->glShader());
        // Match the device's canonical attribute locations (see gl_shader.cpp).
        GLBindAttribLocations(prog);
        glLinkProgram(prog);                // kicks off the (async on web) link
        LinkedProgram lp; lp.prog = prog;   // ready=false; finalized once the link completes
        extern unsigned long g_kbProgLinks;
        ++g_kbProgLinks;
        it = progCache_.emplace(key, lp).first;
    }

    LinkedProgram &lp = it->second;
    if (!lp.ready && !finalizeProgram(lp))
        return false;                        // still compiling this frame -> skip the draw

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
    return true;
}
