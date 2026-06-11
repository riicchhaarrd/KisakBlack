// gl_shader.h — DX9 shader bytecode → GLSL translation + GL shader objects.
//
// The game ships compiled DX9 SM3 vertex/pixel shader bytecode (in fastfiles).
// TranslateD3D9Shader() parses that token stream and emits GLSL 120; GLVertexShader
// / GLPixelShader wrap the compiled GL shader. At draw time the device links the
// bound (vs, ps) pair into a program and uploads the c# constant registers as the
// `vsc[]` / `psc[]` uniform arrays.
//
// Opcode coverage is the common arithmetic + sampling subset (mov/add/sub/mad/mul/
// rcp/rsq/dp3/dp4/min/max/frc/texld, plus dcl/def); unsupported opcodes make the
// translator return false with a diagnostic so gaps are visible, not silent.
//
// NOTE: token encodings follow the documented DX9 bytecode format and are verified
// end-to-end by tests/smoke_shader.cpp. They still need validation against real
// fastfile shaders once those are available (tracked in the README).
#ifndef KISAK_GL_SHADER_H
#define KISAK_GL_SHADER_H

#include "gl_object.h"
#include <string>
#include <vector>
#include <map>

// Translate a DX9 shader token stream to GLSL. On success fills `glslOut` and
// `*outIsPixel`; on an unsupported token returns false (glslOut holds a comment).
// `shadowSamplerMask` bit N declares sampler sN as sampler2DShadow and emits its
// texldp lookups as depth-compared textureProj — the D3D9 hardware-shadow idiom.
// `outSamplerMask` (optional) reports which sampler registers the shader uses.
bool TranslateD3D9Shader(const DWORD *tokens, std::string &glslOut, bool *outIsPixel,
                         unsigned shadowSamplerMask = 0, unsigned *outSamplerMask = nullptr);

// Canonical generic-vertex-attribute mapping for a D3D vertex-declaration usage +
// usage index. A single source of truth shared by the translator (which names the
// `attribute` inputs), the program linker (glBindAttribLocation) and the device's
// vertex-array setup, so all three agree on which stream feeds which input. The
// index matters: TEXCOORD0 and TEXCOORD1 are distinct attributes, so dropping it
// would collapse them to one name and break the GLSL ("aTexCoord redeclared").
// Returns a location in [0,15], or -1 for an unsupported usage.
int         GLAttribLocation(int usage, int usageIndex);
std::string GLAttribName(int usage, int usageIndex);
// Bind the full canonical name->location set on a program before linking. Names
// the shader does not declare are simply ignored by GL.
void        GLBindAttribLocations(unsigned program);

// LAZY GL COMPILE: the engine calls Create{Vertex,Pixel}Shader from loader threads.
// On the de-proxied web build the GL context is LOCAL to the render worker, so
// glCreateShader on a loader thread silently returns 0 (empty info log) -> black
// world. Translation (pure CPU) still happens in the constructor; the GL compile is
// deferred into glShader(), whose only callers are the program-link path that runs
// at draw time on the render thread with the context current. ok() reports
// translation success so material loading can proceed off-thread.
class GLVertexShader final : public GLObject<IDirect3DVertexShader9> {
public:
    GLVertexShader(IDirect3DDevice9 *device, const DWORD *function);
    ~GLVertexShader() override;
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT WINAPI GetFunction(void *, UINT *pSize) override { if (pSize) *pSize = 0; return D3D_OK; }
    unsigned glShader();                       // lazy: compiles on first use (GL thread)
    bool ok() const { return translatedOk_; }
private:
    IDirect3DDevice9 *device_;
    unsigned          shader_ = 0;
    int               tries_ = 0;            // failed compile attempts (retry with cooldown)
    unsigned long     lastTryPres_ = 0;      // present # of the last attempt
    bool              translatedOk_ = false;
    std::string       glsl_;
};

class GLPixelShader final : public GLObject<IDirect3DPixelShader9> {
public:
    GLPixelShader(IDirect3DDevice9 *device, const DWORD *function);
    ~GLPixelShader() override;
    HRESULT WINAPI GetDevice(IDirect3DDevice9 **ppDevice) override;
    HRESULT WINAPI GetFunction(void *, UINT *pSize) override { if (pSize) *pSize = 0; return D3D_OK; }
    unsigned glShader() { return glShader(0); }
    // Shadow-sampler variant: same bytecode retranslated with the masked samplers typed
    // sampler2DShadow (depth-compare lookups). Mask 0 = the plain shader. Variants are
    // cached; the program cache keys on GL shader ids so (vs, ps-variant) pairs link
    // independently.
    unsigned glShader(unsigned shadowMask);
    bool ok() const { return translatedOk_; }
private:
    IDirect3DDevice9 *device_;
    unsigned          shader_ = 0;
    int               tries_ = 0;            // failed compile attempts (retry with cooldown)
    unsigned long     lastTryPres_ = 0;      // present # of the last attempt
    bool              translatedOk_ = false;
    unsigned          samplerMask_ = 0;        // sampler registers this shader reads
    std::string       glsl_;
    std::vector<DWORD> func_;                  // bytecode copy for variant retranslation
    // shadowMask -> compiled variant. A transiently-distressed GPU process fails
    // compiles with empty logs; retry with cooldown instead of caching failure forever.
    struct Variant { unsigned gl = 0; int tries = 0; unsigned long lastPres = 0; };
    std::map<unsigned, Variant> variants_;
};

#endif // KISAK_GL_SHADER_H
