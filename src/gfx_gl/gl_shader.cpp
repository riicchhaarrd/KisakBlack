// gl_shader.cpp — DX9 bytecode → GLSL 120 translator + GL shader objects.
#include "gl_shader.h"
#include "gl_optrace.h"

#include <GL/glew.h>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>   // EM_ASM (raw-JS context probe in compileGL)
#endif

namespace {

// --- DX9 bytecode constants (documented format) ---
enum {  // opcodes (token & 0xFFFF) — D3DSIO_*
    OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7,
    OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13,
    OP_EXP = 14, OP_LOG = 15, OP_LRP = 18, OP_FRC = 19,
    OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29, OP_LABEL = 30,
    OP_POW = 32, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37,
    OP_REP = 38, OP_ENDREP = 39, OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43,
    OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,
    OP_TEXKILL = 65, OP_TEXLD = 66, OP_CMP = 88, OP_DP2ADD = 90, OP_SETP = 94,
    OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_TEXLDL = 95,
    OP_DCL = 31, OP_DEF = 81, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum {  // register types
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_TEXTURE = 3, RT_RASTOUT = 4,
    RT_ATTROUT = 5, RT_OUTPUT = 6, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10,
    RT_MISCTYPE = 17,   // ps_3.0 vPos (reg 0 = pixel position) / vFace (reg 1)
};

struct Operand { int type, reg, swizzle, writemask, mod, dmod;
                 bool rel = false; int relComp = 0; };  // c[a0.<relComp> + reg]

Operand decodeParam(DWORD t) {
    Operand o;
    o.reg       = (int)(t & 0x7FF);
    o.type      = (int)(((t >> 28) & 0x7) | ((t & 0x1800) >> 8));
    o.writemask = (int)((t >> 16) & 0xF);
    o.swizzle   = (int)((t >> 16) & 0xFF);
    o.mod       = (int)((t >> 24) & 0xF);   // source modifier (D3DSPSM_*)
    o.dmod      = (int)((t >> 20) & 0xF);   // dest modifier (_sat = bit 0)
    return o;
}

// Decode a parameter that may use RELATIVE ADDRESSING (bit 13): `c[a0.x + N]`, the
// skinned-mesh bone-matrix indexing pattern. In sm2.0+ the relative flag adds an
// EXTRA address token after the parameter; treating it as the next source operand
// read bones from garbage registers (the M16 viewmodel spazz). Returns tokens used.
int decodeParamRel(const DWORD *tok, Operand &o) {
    o = decodeParam(tok[0]);
    if (tok[0] & 0x2000) {                    // D3DSHADER_ADDRMODE_RELATIVE
        o.rel = true;
        Operand a = decodeParam(tok[1]);      // address token: a0 (or aL) + swizzle
        o.relComp = a.swizzle & 0x3;          // .x/.y/.z/.w of the address register
        return 2;
    }
    return 1;
}

const char *kComp = "xyzw";

std::string maskStr(int m) {  // dest write-mask, e.g. 0b0111 -> ".xyz"
    if (m == 0xF || m == 0) return "";
    std::string s = ".";
    for (int i = 0; i < 4; ++i) if (m & (1 << i)) s += kComp[i];
    return s;
}

std::string swizStr(int sw) {  // src swizzle as a 4-component selector, "" if identity
    if (sw == 0xE4) return "";  // .xyzw
    std::string s = ".";
    for (int i = 0; i < 4; ++i) s += kComp[(sw >> (i * 2)) & 0x3];
    return s;
}

// ---- D3D9 disassembler (?dumpenv) ------------------------------------------
// Faithful asm text rebuilt from the SAME token stream the translator consumes, but
// through NONE of the translation logic — so a translated-GLSL-vs-asm diff exposes
// translator bugs instead of reflecting them (the envmap over-reflection hunt).
static const char *kbOpName(int op) {
    switch (op) {
        case OP_MOV: return "mov";   case OP_ADD: return "add";   case OP_SUB: return "sub";
        case OP_MAD: return "mad";   case OP_MUL: return "mul";   case OP_RCP: return "rcp";
        case OP_RSQ: return "rsq";   case OP_DP3: return "dp3";   case OP_DP4: return "dp4";
        case OP_MIN: return "min";   case OP_MAX: return "max";   case OP_SLT: return "slt";
        case OP_SGE: return "sge";   case OP_EXP: return "exp";   case OP_LOG: return "log";
        case OP_LRP: return "lrp";   case OP_FRC: return "frc";   case OP_POW: return "pow";
        case OP_ABS: return "abs";   case OP_NRM: return "nrm";   case OP_SINCOS: return "sincos";
        case OP_MOVA: return "mova"; case OP_TEXKILL: return "texkill";
        case OP_TEXLD: return "texld"; case OP_CMP: return "cmp"; case OP_DP2ADD: return "dp2add";
        case OP_DSX: return "dsx";   case OP_DSY: return "dsy";   case OP_TEXLDD: return "texldd";
        case OP_TEXLDL: return "texldl";
        default: return "op?";
    }
}
static std::string kbRegAsm(const Operand &o) {
    const char *t;
    switch (o.type) {
        case RT_TEMP: t = "r"; break;       case RT_INPUT: t = "v"; break;
        case RT_CONST: t = "c"; break;      case RT_TEXTURE: t = "t"; break;
        case RT_SAMPLER: t = "s"; break;    case RT_COLOROUT: t = "oC"; break;
        case RT_DEPTHOUT: t = "oDepth"; break; case RT_OUTPUT: t = "o"; break;
        case RT_RASTOUT: t = "oPos"; break; case RT_MISCTYPE: t = "vMisc"; break;
        default: t = "x"; break;
    }
    char buf[32]; snprintf(buf, sizeof buf, "%s%d", t, o.reg);
    return buf;
}
static std::string kbSrcAsm(const Operand &o) {
    std::string s = kbRegAsm(o);
    if (o.rel) { s += "[a0."; s += kComp[o.relComp & 3]; s += "]"; }
    s += swizStr(o.swizzle);
    switch (o.mod) {   // D3DSPSM_*
        case 1:  return "-" + s;
        case 2:  return s + "_bias";
        case 3:  return "-" + s + "_bias";
        case 4:  return s + "_bx2";
        case 5:  return "-" + s + "_bx2";
        case 6:  return "1-" + s;
        case 7:  return s + "_x2";
        case 8:  return "-" + s + "_x2";
        case 9:  return s + "_dz";
        case 10: return s + "_dw";
        case 11: return s + "_abs";
        case 12: return "-" + s + "_abs";
        case 13: return "!" + s;
        default: return s;
    }
}

// GLSL output dialect. The desktop build emits `#version 120` compat GLSL; the
// WebGL2 build emits `#version 300 es` (GLSL ES 3.00). The translator is
// parameterized on this — the 120 path is preserved verbatim, not replaced.
#if defined(__EMSCRIPTEN__)
static const bool kEmitES_default = true;
#else
static const bool kEmitES_default = false;
#endif

struct Ctx {
    bool isPixel = false;
    bool emitES = kEmitES_default;              // GLSL ES 3.00 vs #version 120
    const char *cArr() const { return isPixel ? "psc" : "vsc"; }
    // Pixel-shader color output: gl_FragColor in 120, a declared `out` in ES 3.00.
    const char *fragColorName() const { return emitES ? "fragColor" : "gl_FragColor"; }
    // Texture sampler call: texture2D in 120, overloaded texture() in ES 3.00.
    const char *texFn() const { return emitES ? "texture" : "texture2D"; }
    unsigned shadowMask = 0;   // samplers typed sampler2DShadow (depth-compare lookups)
    // ?lmarray: sampler stages to emit as sampler2DArray instead of sampler2D, sampling layer
    // uLmLayer (the per-draw lightmap page). Dissolves the per-surface lightmap BIND (s12/13/14)
    // into a per-draw layer index so lit-world draws can collapse into one multi-draw. 0 = off.
    unsigned lmArrayMask = 0;
    // True only when stage sN is an lmArray stage, a plain 2D sampler (not shadow), AND we emit ES
    // 3.00 (sampler2DArray needs GLSL ES 3.00 / desktop 130+; the #version 120 path stays untouched).
    bool lmA(int sN) const {
        return emitES && ((lmArrayMask >> sN) & 1) && !((shadowMask >> sN) & 1)
            && (samplerDim.count(sN) ? samplerDim.at(sN) : 2) == 2;
    }
    // ?matarray stage 2: sampler stages to emit as sampler2DArray sampling layer uMatLayer —
    // the per-draw MATERIAL layer in its bucket's stage array (consolidation; the lmarray
    // mechanism generalized). Per-variant, set via KB_SetMatArrayTranslateMask before the
    // translate call (mask differs per material bucket, unlike the global lmArrayMask).
    unsigned matArrayMask = 0;
    bool matA(int sN) const {
        return emitES && ((matArrayMask >> sN) & 1) && !((shadowMask >> sN) & 1) && !lmA(sN)
            && (samplerDim.count(sN) ? samplerDim.at(sN) : 2) == 2;
    }
    std::map<int, std::pair<int,int>> inputs;   // reg -> (usage, usageIndex)
    std::map<int, std::pair<int,int>> outputs;  // reg -> (usage, usageIndex)  (vertex)
    std::set<int> samplers;
    std::map<int, int> samplerDim;   // sampler reg -> D3DSTT: 2=2D, 3=CUBE, 4=VOLUME
    std::map<int, float[4]> defs;
    std::map<int, int> idefs;                   // DEFI integer constant reg -> count (.x)
    std::set<int> usedTemps;
    bool usedConst = false;
    bool usedA0    = false;   // MOVA / relative addressing: declare ivec4 a0
    int  maxConst  = -1;   // highest runtime-constant register referenced (sizes vsc[]/psc[])
};

// D3DDECLUSAGE -> a stable varying name shared between the vs and ps stages.
std::string varyingName(int usage, int index) {
    static const char *u[] = {"POSITION","BLENDW","BLENDI","NORMAL","PSIZE","TEXCOORD",
                              "TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG",
                              "DEPTH","SAMPLE"};
    std::ostringstream s;
    s << "v_" << (usage >= 0 && usage < 14 ? u[usage] : "X") << index;
    return s.str();
}

std::string regName(Ctx &c, const Operand &o, bool isDest) {
    switch (o.type) {
        case RT_TEMP:    c.usedTemps.insert(o.reg); { std::ostringstream s; s << "r" << o.reg; return s.str(); }
        case RT_CONST:
            if (o.rel) {
                // c[a0.<comp> + N]: the runtime index can reach any register, so the
                // array must span all 256 (clamped — OOB indexing is UB in GLSL).
                c.usedConst = true; c.usedA0 = true; c.maxConst = 255;
                std::ostringstream s;
                s << c.cArr() << "[clamp(a0." << kComp[o.relComp] << " + " << o.reg << ", 0, 255)]";
                return s.str();
            }
            if (c.defs.count(o.reg)) { std::ostringstream s; s << "c" << o.reg << "_def"; return s.str(); }
            c.usedConst = true; if (o.reg > c.maxConst) c.maxConst = o.reg;
            { std::ostringstream s; s << c.cArr() << "[" << o.reg << "]"; return s.str(); }
        case RT_INPUT:
            if (c.isPixel) { auto it = c.inputs.find(o.reg);
                             return it != c.inputs.end() ? varyingName(it->second.first, it->second.second) : "vec4(0.0)"; }
            else { auto it = c.inputs.find(o.reg);
                   return it != c.inputs.end() ? GLAttribName(it->second.first, it->second.second) : "aPos"; }
        case RT_OUTPUT: {
            auto it = c.outputs.find(o.reg);
            if (it != c.outputs.end()) {
                if (it->second.first == D3DDECLUSAGE_POSITION || it->second.first == D3DDECLUSAGE_POSITIONT)
                    return "gl_Position";
                return varyingName(it->second.first, it->second.second);
            }
            return o.reg == 0 ? "gl_Position" : "vec4(0.0)";
        }
        case RT_RASTOUT:  return "gl_Position";
        case RT_COLOROUT: return c.fragColorName();
        case RT_SAMPLER:  { std::ostringstream s; s << "s" << o.reg; return s.str(); }
        case RT_MISCTYPE: // vPos (pixel position) / vFace (front-facing). vPos was
                          // unhandled (-> vec4(0)), breaking every screen-space sample
                          // (light/refraction passes read the scene buffer at vPos*texel).
            return o.reg == 1 ? "vec4(gl_FrontFacing ? 1.0 : -1.0)" : "gl_FragCoord";
        default: (void)isDest; return "vec4(0.0)";
    }
}

// Apply a D3DSPSM_* source modifier (bias/sign/x2 etc. expand packed normals;
// missing these left lighting/normal-map maths under-saturated -> washed out).
std::string applyMod(const std::string &e, int mod) {
    switch (mod) {
        case 1:  return "(-" + e + ")";                 // negate
        case 2:  return "(" + e + " - 0.5)";            // bias
        case 3:  return "(-(" + e + " - 0.5))";         // bias-negate
        case 4:  return "(2.0*(" + e + ") - 1.0)";      // sign / _bx2
        case 5:  return "(-(2.0*(" + e + ") - 1.0))";   // _bx2 negate
        case 6:  return "(1.0 - " + e + ")";            // complement
        case 7:  return "(2.0*(" + e + "))";            // _x2
        case 8:  return "(-2.0*(" + e + "))";           // _x2 negate
        case 11: return "abs(" + e + ")";               // abs
        case 12: return "(-abs(" + e + "))";            // abs-negate
        default: return e;                              // none / unsupported (dz,dw,not)
    }
}

std::string srcExpr(Ctx &c, const Operand &o) {
    return applyMod(regName(c, o, false) + swizStr(o.swizzle), o.mod);
}

// Emit one instruction as `dst<mask> = (<expr>)<mask>;`.
void emitInstr(Ctx &c, int op, const Operand *src, int nsrc, const Operand &dst,
               std::ostringstream &body, const std::string &ind, int ctrl = 0) {
    auto s = [&](int i) { return srcExpr(c, src[i]); };
    // TEXKILL discards the fragment when any of the tested register's xyz < 0; it has
    // no destination, so handle it before the dst<mask>= path below.
    if (op == OP_TEXKILL) {
        body << ind << "if (any(lessThan((" << regName(c, dst, true) << ").xyz, vec3(0.0)))) discard;\n";
        return;
    }
    // MOVA loads the address register a0 (round-to-nearest per D3D9 spec); a0 then
    // drives relative constant addressing (bone-matrix indexing in skinned meshes).
    if (op == OP_MOVA) {
        c.usedA0 = true;
        std::string m = maskStr(dst.writemask);
        body << ind << "a0" << m << " = ivec4(floor(" << s(0) << " + 0.5))" << m << ";\n";
        return;
    }
    std::string expr;
    switch (op) {
        case OP_MOV:   expr = s(0); break;
        case OP_ADD:   expr = s(0) + " + " + s(1); break;
        case OP_SUB:   expr = s(0) + " - " + s(1); break;
        case OP_MUL:   expr = s(0) + " * " + s(1); break;
        case OP_MAD:   expr = s(0) + " * " + s(1) + " + " + s(2); break;
        case OP_LRP:   expr = "mix(" + s(2) + ", " + s(1) + ", " + s(0) + ")"; break;  // dst = src2 + src0*(src1-src2)
        case OP_MIN:   expr = "min(" + s(0) + ", " + s(1) + ")"; break;
        case OP_MAX:   expr = "max(" + s(0) + ", " + s(1) + ")"; break;
        case OP_FRC:   expr = "fract(" + s(0) + ")"; break;
        case OP_ABS:   expr = "abs(" + s(0) + ")"; break;
        case OP_SLT:   expr = "vec4(lessThan(" + s(0) + ", " + s(1) + "))"; break;
        case OP_SGE:   expr = "vec4(greaterThanEqual(" + s(0) + ", " + s(1) + "))"; break;
        case OP_CMP:   expr = "mix(" + s(2) + ", " + s(1) + ", vec4(greaterThanEqual(" + s(0) + ", vec4(0.0))))"; break;  // src0>=0 ? src1 : src2
        case OP_RCP:   expr = "vec4(1.0 / (" + s(0) + ").x)"; break;
        case OP_RSQ:   expr = "vec4(inversesqrt((" + s(0) + ").x))"; break;
        case OP_EXP:   expr = "vec4(exp2((" + s(0) + ").x))"; break;
        case OP_LOG:   expr = "vec4(log2(abs((" + s(0) + ").x)))"; break;
        case OP_POW:   expr = "vec4(pow(abs((" + s(0) + ").x), (" + s(1) + ").x))"; break;
        case OP_NRM:   expr = "vec4(normalize((" + s(0) + ").xyz), 0.0)"; break;
        case OP_SINCOS:expr = "vec4(cos((" + s(0) + ").x), sin((" + s(0) + ").x), 0.0, 0.0)"; break;
        case OP_DP3:   expr = "vec4(dot((" + s(0) + ").xyz, (" + s(1) + ").xyz))"; break;
        case OP_DP4:   expr = "vec4(dot(" + s(0) + ", " + s(1) + "))"; break;
        case OP_DP2ADD:expr = "vec4(dot((" + s(0) + ").xy, (" + s(1) + ").xy) + (" + s(2) + ").x)"; break;
        // Screen-space partial derivatives. Dropping these (the dest stayed vec4(0)) broke
        // every pixel shader that uses ddx/ddy — mip selection and distance detail — which
        // is what made distant surfaces blow out to white/blue on mp_mountain. dFdx/dFdy are
        // core in both GLSL ES 3.00 (WebGL2) and desktop 1.20.
        case OP_DSX:   expr = "dFdx(" + s(0) + ")"; break;
        case OP_DSY:   expr = "dFdy(" + s(0) + ")"; break;
        case OP_TEXLD: {
            // The instruction-token control bits select the texld FLAVOUR (ignored before
            // -- which broke every projected lookup): 1 = texldp (projective divide; on a
            // depth texture this is the D3D9 hardware-shadow compare), 2 = texldb (bias).
            std::string samp = regName(c, src[1], false);
            int sreg = src[1].reg;
            bool shadow = (c.shadowMask >> sreg) & 1;
            if (shadow) {
                // sampler2DShadow: PCF depth-comparison result. KB_sc does the projective
                // divide AND the render-target Y flip (see prologue note).
                if (ctrl == 1)
                    expr = c.emitES ? "vec4(texture(" + samp + ", KB_sc(" + s(0) + ")))"
                                    : "vec4(shadow2D(" + samp + ", KB_sc(" + s(0) + ")).x)";
                else
                    expr = c.emitES ? "vec4(texture(" + samp + ", KB_sc(vec4((" + s(0) + ").xyz, 1.0))))"
                                    : "vec4(shadow2D(" + samp + ", KB_sc(vec4((" + s(0) + ").xyz, 1.0))).x)";
            } else if (c.lmA(sreg)) {
                // lightmap array: sample layer uLmLayer instead of a bound 2D texture (?lmarray).
                // (lit lightmap lookups are plain 2D fetches — never projective/biased here.)
                expr = "texture(" + samp + ", vec3((" + s(0) + ").xy, uLmLayer))";
            } else if (c.matA(sreg)) {
                // ?matarray stage 2: material stage rides its bucket's texture array; the
                // per-draw layer (this material's index in the bucket) comes via uMatLayer.
                expr = "texture(" + samp + ", vec3((" + s(0) + ").xy, uMatLayer))";
            } else if (ctrl == 1) {
                expr = std::string(c.emitES ? "textureProj" : "texture2DProj") + "(" + samp + ", " + s(0) + ")";
            } else {
                // Coordinate arity follows the sampler's declared dimension: cube and
                // volume lookups take .xyz (model lighting samples a 3D texture in the
                // VERTEX shader; .xy there is a GLSL type error or wrong texel).
                int dim = c.samplerDim.count(sreg) ? c.samplerDim.at(sreg) : 2;
                const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
                const char *fn = c.emitES ? "texture"
                                  : dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                if (ctrl == 2)
                    expr = std::string(fn) + "(" + samp + ", (" + s(0) + ")" + coords + ", (" + s(0) + ").w)";
                else
                    expr = std::string(fn) + "(" + samp + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        case OP_TEXLDL: {
            // Explicit-LOD sample: the LOD rides in coord .w. DROPPING it sampled mip 0
            // forever — every cubemap reflection razor-sharp = the "vaseline gloss"
            // (CoD blurs reflections by gloss via this LOD).
            int sreg2 = src[1].reg;
            int dim = c.samplerDim.count(sreg2) ? c.samplerDim.at(sreg2) : 2;
            const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
            if (c.emitES)
                expr = "textureLod(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ", (" + s(0) + ").w)";
            else {
                const char *fn = dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                expr = std::string(fn) + "(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        case OP_TEXLDD: {
            // Explicit-gradient sample (ddx/ddy in src[2]/src[3]).
            int sreg2 = src[1].reg;
            int dim = c.samplerDim.count(sreg2) ? c.samplerDim.at(sreg2) : 2;
            const char *coords = (dim == 3 || dim == 4) ? ".xyz" : ".xy";
            if (c.emitES && nsrc >= 4)
                expr = "textureGrad(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords +
                       ", (" + srcExpr(c, src[2]) + ")" + coords + ", (" + srcExpr(c, src[3]) + ")" + coords + ")";
            else {
                const char *fn = c.emitES ? "texture"
                                  : dim == 4 ? "texture3D" : dim == 3 ? "textureCube" : "texture2D";
                expr = std::string(fn) + "(" + regName(c, src[1], false) + ", (" + s(0) + ")" + coords + ")";
            }
            break;
        }
        default: (void)nsrc; return;
    }
    std::string dn = regName(c, dst, true);
    std::string m  = maskStr(dst.writemask);
    if (dst.dmod & 1) expr = "clamp(" + expr + ", 0.0, 1.0)";   // _sat: clamp result to [0,1]
    body << ind << dn << m << " = (" << expr << ")" << m << ";\n";
}

} // namespace

// ---- Canonical vertex-attribute mapping (shared, see gl_shader.h) ----------
//
// One generic attribute location per (usage, usageIndex), laid out to fit the 16
// locations GL guarantees. The same (usage,index) always yields the same location
// and name across the translator, the linker and the device's vertex setup.
int GLAttribLocation(int usage, int usageIndex) {
    switch (usage) {
        case D3DDECLUSAGE_POSITION:
        case D3DDECLUSAGE_POSITIONT:   return 0;
        case D3DDECLUSAGE_BLENDWEIGHT: return 1;
        case D3DDECLUSAGE_BLENDINDICES:return 2;
        case D3DDECLUSAGE_NORMAL:      return 3;
        case D3DDECLUSAGE_TANGENT:     return 4;
        case D3DDECLUSAGE_COLOR:       return usageIndex <= 1 ? 5 + usageIndex : -1;   // COLOR0..1  -> 5,6
        case D3DDECLUSAGE_TEXCOORD:    return usageIndex <= 8 ? 7 + usageIndex : -1;   // TEXCOORD0..8 -> 7..15
        default:                       return -1;
    }
}

std::string GLAttribName(int usage, int usageIndex) {
    std::ostringstream s;
    switch (usage) {
        case D3DDECLUSAGE_POSITION:
        case D3DDECLUSAGE_POSITIONT:    return "aPos";
        case D3DDECLUSAGE_BLENDWEIGHT:  return "aBlendWeight";
        case D3DDECLUSAGE_BLENDINDICES: return "aBlendIndices";
        case D3DDECLUSAGE_NORMAL:       return "aNormal";
        case D3DDECLUSAGE_TANGENT:      return "aTangent";
        case D3DDECLUSAGE_COLOR:        s << "aColor"    << usageIndex; return s.str();
        case D3DDECLUSAGE_TEXCOORD:     s << "aTexCoord" << usageIndex; return s.str();
        default:                        return "aPos";
    }
}

void GLBindAttribLocations(unsigned program) {
    // Bind every canonical name; GL ignores those a given shader doesn't declare.
    static const int kUsages[] = { D3DDECLUSAGE_POSITION, D3DDECLUSAGE_BLENDWEIGHT,
                                   D3DDECLUSAGE_BLENDINDICES, D3DDECLUSAGE_NORMAL,
                                   D3DDECLUSAGE_TANGENT, D3DDECLUSAGE_COLOR,
                                   D3DDECLUSAGE_TEXCOORD };
    for (int u : kUsages) {
        int maxIdx = (u == D3DDECLUSAGE_TEXCOORD) ? 8 : (u == D3DDECLUSAGE_COLOR ? 1 : 0);
        for (int i = 0; i <= maxIdx; ++i) {
            int loc = GLAttribLocation(u, i);
            if (loc >= 0) glBindAttribLocation(program, loc, GLAttribName(u, i).c_str());
        }
    }
}

static bool KB_DumpEnvWanted() {
    static int en = -1;
    if (en < 0) en = getenv("KB_DUMPENV") ? 1 : 0;
    return en == 1;
}

// ?lmarray (KB_LMARRAY=1): which sampler stages to translate as sampler2DArray (layer = uLmLayer).
// Stage 1a = primary lightmap only (s12); extend to s12|s13|s14 once primary is verified. Cached.
unsigned KB_LmArrayMask() {
    static int mask = -1;
    if (mask < 0) { const char *e = getenv("KB_LMARRAY"); mask = (e && *e == '1') ? (1 << 12) : 0; }
    return (unsigned)mask;
}

// ?matarray stage 2: the pending per-variant material-array mask for the NEXT translate
// call (glShaderMat sets it around its TranslateD3D9Shader invocation; single-threaded
// translate path — the GL thread). 0 = plain translation.
static unsigned g_kbMatTranslateMask = 0;
extern "C" void KB_SetMatArrayTranslateMask(unsigned mask) { g_kbMatTranslateMask = mask; }

bool TranslateD3D9Shader(const DWORD *tok, std::string &out, bool *outIsPixel,
                         unsigned shadowSamplerMask, unsigned *outSamplerMask) {
    Ctx c;
    // Diagnostic/test override: KB_GLSL_ES=1 forces GLSL ES 3.00 emit (=0 forces
    // #version 120) on any build, so the ES output can be dumped + checked offline
    // without an Emscripten compile. Defaults to kEmitES_default otherwise.
    if (const char *e = getenv("KB_GLSL_ES")) c.emitES = (e[0] != '0');
    DWORD ver = *tok++;
    c.isPixel = (ver >> 16) == 0xFFFF;
    c.shadowMask = shadowSamplerMask;
    if (c.isPixel) c.lmArrayMask = KB_LmArrayMask();   // lightmaps are sampled in the pixel shader
    if (c.isPixel) c.matArrayMask = g_kbMatTranslateMask;   // ?matarray stage-2 variant (0 = plain)
    if (outIsPixel) *outIsPixel = c.isPixel;

    std::ostringstream body;
    std::ostringstream dis;                       // ?dumpenv: side-channel D3D9 disassembly
    const bool wantAsm = KB_DumpEnvWanted();
    int indent = 1, loopId = 0;
    auto ind = [](int n) { return std::string(2 * (n < 1 ? 1 : n), ' '); };
    // D3DSHADER_COMPARISON (ctrl): 1 GT, 2 EQ, 3 GE, 4 LT, 5 NE, 6 LE.
    auto cmpStr = [&](int cc, const Operand &a, const Operand &b) {
        static const char *ops[] = { ">", ">", "==", ">=", "<", "!=", "<=" };
        const char *o = (cc >= 1 && cc <= 6) ? ops[cc] : ">";
        return "(" + srcExpr(c, a) + ").x " + o + " (" + srcExpr(c, b) + ").x";
    };
    for (;;) {
        DWORD t = *tok++;
        int op = (int)(t & 0xFFFF);
        if (op == OP_END) break;
        if (op == OP_COMMENT) { tok += (t >> 16) & 0x7FFF; continue; }
        int len  = (int)((t >> 24) & 0xF);
        int ctrl = (int)((t >> 16) & 0xFF);   // comparison field for IFC/BREAKC

        if (op == OP_DCL) {
            DWORD usageTok = *tok;
            Operand reg = decodeParam(tok[1]);
            tok += len;
            if (wantAsm) {
                static const char *dims[] = {"?", "?", "2d", "cube", "volume"};
                int d = (int)((usageTok >> 27) & 0xF);
                dis << "dcl" << (reg.type == RT_SAMPLER ? (std::string("_") + (d >= 2 && d <= 4 ? dims[d] : "?")) : "")
                    << " " << kbRegAsm(reg) << "\n";
            }
            if (reg.type == RT_SAMPLER) {
                c.samplers.insert(reg.reg);
                // dcl_2d/dcl_cube/dcl_volume: texture type in usage token bits 27..30.
                // Ignoring this typed EVERY sampler sampler2D; sampling the bound 3D
                // model-lighting volume (or a cubemap) through a sampler2D is a
                // sampler-type conflict that INVALIDATES the draw on WebGL2 — static
                // models went black/vanished (cupboard/drapes/paintings).
                c.samplerDim[reg.reg] = (int)((usageTok >> 27) & 0xF);
            }
            else {
                int usage = (int)(usageTok & 0x1F);
                int index = (int)((usageTok >> 16) & 0xF);
                if (reg.type == RT_INPUT)       c.inputs[reg.reg]  = {usage, index};
                else if (reg.type == RT_OUTPUT) c.outputs[reg.reg] = {usage, index};
            }
            continue;
        }
        if (op == OP_DEF) {
            Operand reg = decodeParam(tok[0]);
            float f[4]; std::memcpy(f, &tok[1], sizeof(f));
            std::memcpy(c.defs[reg.reg], f, sizeof(f));
            if (wantAsm)
                dis << "def c" << reg.reg << ", " << f[0] << ", " << f[1] << ", " << f[2] << ", " << f[3] << "\n";
            tok += len; continue;
        }
        if (op == OP_DEFI) { Operand reg = decodeParam(tok[0]); c.idefs[reg.reg] = (int)tok[1]; tok += len; continue; }
        if (op == OP_DEFB || op == OP_LABEL || op == OP_CALL || op == OP_CALLNZ ||
            op == OP_RET  || op == OP_SETP) { tok += len; continue; }

        // Structured control flow. (Dropping IF/ELSE/ENDIF would run BOTH branches
        // unconditionally, so an `if(lit) {..} else {flat}` left the world flat.)
        if (op == OP_ELSE)    { if (indent > 1) --indent; body << ind(indent) << "} else {\n"; ++indent; tok += len; continue; }
        if (op == OP_ENDIF || op == OP_ENDREP || op == OP_ENDLOOP) { if (indent > 1) --indent; body << ind(indent) << "}\n"; tok += len; continue; }
        if (op == OP_BREAK)   { body << ind(indent) << "break;\n"; tok += len; continue; }
        if (op == OP_IF)      { Operand a = decodeParam(tok[0]); body << ind(indent) << "if (bool((" << srcExpr(c, a) << ").x)) {\n"; ++indent; tok += len; continue; }
        if (op == OP_IFC)     { Operand a = decodeParam(tok[0]), b = decodeParam(tok[1]); body << ind(indent) << "if (" << cmpStr(ctrl, a, b) << ") {\n"; ++indent; tok += len; continue; }
        if (op == OP_BREAKC)  { Operand a = decodeParam(tok[0]), b = decodeParam(tok[1]); body << ind(indent) << "if (" << cmpStr(ctrl, a, b) << ") break;\n"; tok += len; continue; }
        if (op == OP_REP || op == OP_LOOP) {
            Operand cnt = decodeParam(tok[0]);
            int n = 4; auto di = c.idefs.find(cnt.reg); if (di != c.idefs.end() && di->second > 0) n = di->second;
            int id = loopId++;
            body << ind(indent) << "for (int aL" << id << " = 0; aL" << id << " < " << n << "; ++aL" << id << ") {\n";
            ++indent; tok += len; continue;
        }

        // Arithmetic / sample: dst then sources, decoded sequentially — a parameter
        // with the relative-addressing bit consumes an extra address token, so fixed
        // tok[1+i] offsets would read the address token as the next source.
        Operand dst;
        int k = decodeParamRel(tok, dst);
        Operand src[4];                      // texldd carries 4 sources (coords, sampler, ddx, ddy)
        int nsrc = 0;
        while (k < len && nsrc < 4) k += decodeParamRel(tok + k, src[nsrc++]);
        tok += len;
        if (wantAsm) {
            dis << kbOpName(op) << ((dst.dmod & 1) ? "_sat" : "") << " "
                << kbRegAsm(dst) << maskStr(dst.writemask);
            for (int si = 0; si < nsrc; ++si) dis << ", " << kbSrcAsm(src[si]);
            dis << "\n";
        }
        emitInstr(c, op, src, nsrc, dst, body, ind(indent), ctrl);
    }

    // Assemble the GLSL translation unit. Two dialects, parameterized on c.emitES:
    //   * #version 120 (desktop compat): attribute/varying, gl_FragColor,
    //     texture2D, fixed-function GL_ALPHA_TEST does the cutout.
    //   * #version 300 es (WebGL2): in/out, a declared fragColor out, texture(),
    //     explicit precision, and alpha test emulated with discard (no
    //     GL_ALPHA_TEST in ES) against uAlphaTestFunc/uAlphaRef uniforms that the
    //     state layer feeds in place of glAlphaFunc.
    std::ostringstream o;
    if (c.emitES) {
        o << "#version 300 es\n";
        o << "precision highp float;\n";
        o << "precision highp int;\n";
        // ES 3.00 has NO default precision for sampler3D (unlike sampler2D/Cube) —
        // same trap as sampler2DShadow in B66. Declare it unconditionally.
        o << "precision highp sampler3D;\n";
        if (c.isPixel) {
            for (auto &in : c.inputs)  o << "in vec4 " << varyingName(in.second.first, in.second.second) << ";\n";
            // ES 3.00 defines NO default precision for sampler2DShadow — without this
            // single line every shadow variant failed to compile (and the affected
            // draws fell to the builtin, which used to fail too -> invisible geometry).
            if (c.shadowMask) o << "precision highp sampler2DShadow;\n";
            bool anyLmArray = false, anyMatArray = false;
            for (int sN : c.samplers) { if (c.lmA(sN)) anyLmArray = true; if (c.matA(sN)) anyMatArray = true; }
            if (anyLmArray || anyMatArray) {
                // ES 3.00 has no default precision for sampler2DArray (same trap as sampler3D/2DShadow).
                o << "precision highp sampler2DArray;\n";
            }
            if (anyLmArray)  o << "uniform float uLmLayer;\n";    // per-draw lightmap page (state layer)
            if (anyMatArray) o << "uniform float uMatLayer;\n";   // per-draw bucket layer (?matarray stage 2)
            for (int sN : c.samplers)
            {
                int dim = c.samplerDim.count(sN) ? c.samplerDim.at(sN) : 2;
                const char *ty = ((c.shadowMask >> sN) & 1) ? "sampler2DShadow"
                                 : (c.lmA(sN) || c.matA(sN)) ? "sampler2DArray"
                                 : dim == 4 ? "sampler3D" : dim == 3 ? "samplerCube" : "sampler2D";
                o << "uniform " << ty << " s" << sN << ";\n";
            }
            o << "out vec4 " << c.fragColorName() << ";\n";
            // Alpha-test-via-discard uniforms (D3DCMP_* in uAlphaTestFunc; 0 = off).
            o << "uniform int uAlphaTestFunc;\n";
            o << "uniform float uAlphaRef;\n";
            if (c.shadowMask)
                // Shadow lookup coord: projective divide. No Y adjustment: FBO
                // viewports keep D3D placement (gl_d3d9.cpp SetViewport), so the
                // stored shadow atlas matches D3D-convention coords exactly.
                o << "vec3 KB_sc(vec4 c) { return c.xyz / c.w; }\n";
        } else {
            for (auto &in : c.inputs)  o << "in vec4 " << GLAttribName(in.second.first, in.second.second) << ";\n";
            for (auto &ou : c.outputs)
                if (ou.second.first != D3DDECLUSAGE_POSITION && ou.second.first != D3DDECLUSAGE_POSITIONT)
                    o << "out vec4 " << varyingName(ou.second.first, ou.second.second) << ";\n";
        }
    } else {
        o << "#version 120\n";
        if (c.isPixel) {
            for (auto &in : c.inputs)  o << "varying vec4 " << varyingName(in.second.first, in.second.second) << ";\n";
            for (int sN : c.samplers)
            {
                int dim = c.samplerDim.count(sN) ? c.samplerDim.at(sN) : 2;
                const char *ty = ((c.shadowMask >> sN) & 1) ? "sampler2DShadow"
                                 : dim == 4 ? "sampler3D" : dim == 3 ? "samplerCube" : "sampler2D";
                o << "uniform " << ty << " s" << sN << ";\n";
            }
            if (c.shadowMask)
                o << "vec3 KB_sc(vec4 c) { return c.xyz / c.w; }\n";
        } else {
            for (auto &in : c.inputs)  o << "attribute vec4 " << GLAttribName(in.second.first, in.second.second) << ";\n";
            for (auto &ou : c.outputs)
                if (ou.second.first != D3DDECLUSAGE_POSITION && ou.second.first != D3DDECLUSAGE_POSITIONT)
                    o << "varying vec4 " << varyingName(ou.second.first, ou.second.second) << ";\n";
        }
    }
    // Size the constant array to the highest register the shader references (not a blanket
    // 256), so the device uploads only that many vec4s per draw. Relative addressing
    // (c[a0.x+N]) forces maxConst to 255 — the runtime index can reach any register.
    if (c.usedConst) o << "uniform vec4 " << c.cArr() << "[" << (c.maxConst + 1) << "];\n";
    for (auto &d : c.defs)
        o << "const vec4 c" << d.first << "_def = vec4(" << d.second[0] << ", " << d.second[1]
          << ", " << d.second[2] << ", " << d.second[3] << ");\n";
    // Depth-pre-pass invariance: COD draws world/model geometry in a depth PRE-PASS, then
    // re-draws color with an equal/<= depth test — which requires the SAME vertex to produce
    // the EXACT same gl_Position.z in both passes. D3D9 guarantees that ("position
    // invariance"); GLSL does NOT unless gl_Position is declared invariant, so the prepass
    // and color shaders' depths drift by a ULP and surfaces fail the depth test and drop out
    // (the heavy world-geometry flicker on the web build). Declaring it invariant forces the
    // compiler to compute it identically across shaders/passes.
    if (!c.isPixel) o << "invariant gl_Position;\n";
    o << "void main() {\n";
    for (int r : c.usedTemps) o << "  vec4 r" << r << " = vec4(0.0);\n";
    if (c.usedA0) o << "  ivec4 a0 = ivec4(0);\n";
    o << body.str();
    // D3D9 -> GL clip-space depth fixup. D3D9 vertex shaders emit clip-space z in [0,w]
    // (post-divide NDC z in [0,1]); OpenGL/WebGL2 expects [-w,w] (NDC z in [-1,1]). On
    // desktop GL this is normally handled with glClipControl(GL_ZERO_TO_ONE), but that
    // entry point does NOT exist in WebGL2/GLES3, so we must remap in the shader. Without
    // it, every depth value is compressed into the far half [0.5,1.0] of the depth buffer
    // -> half the precision is wasted and distant coplanar surfaces z-fight (buildings
    // flickering in and out). z' = 2z - w maps NDC [0,1] -> [-1,1] exactly.
    if (!c.isPixel)
        o << "  gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n";
    // ES alpha test: D3D's fixed-function cutout, done in-shader. D3DCMP_*:
    // 1=NEVER 2=LESS 3=EQUAL 4=LEQUAL 5=GREATER 6=NOTEQUAL 7=GEQUAL 8=ALWAYS.
    // Discard when the test FAILS. uAlphaTestFunc==0 disables it.
    if (c.emitES && c.isPixel) {
        const char *a = c.fragColorName();
        o << "  if (uAlphaTestFunc != 0) {\n"
          << "    float aT = " << a << ".a; bool passA = true;\n"
          << "    if      (uAlphaTestFunc == 1) passA = false;\n"
          << "    else if (uAlphaTestFunc == 2) passA = (aT <  uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 3) passA = (aT == uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 4) passA = (aT <= uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 5) passA = (aT >  uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 6) passA = (aT != uAlphaRef);\n"
          << "    else if (uAlphaTestFunc == 7) passA = (aT >= uAlphaRef);\n"
          << "    if (!passA) discard;\n"
          << "  }\n";
    }
    o << "}\n";
    if (outSamplerMask) {
        unsigned m = 0;
        for (int sN : c.samplers) if (sN >= 0 && sN < 32) m |= 1u << sN;
        *outSamplerMask = m;
    }
    out = o.str();
    // ?dumpenv=1: print cube-sampling PIXEL shaders (the envmap/specular users) with the
    // ORIGINAL D3D9 disassembly side-by-side, so the term math can be diffed op-by-op
    // against the translation. world=1 marks lightmap-sampling (lit-world) techniques —
    // the mirror-surfaces class; up to 2 model + 2 world shaders per run.
    if (KB_DumpEnvWanted() && c.isPixel) {
        bool hasCube = false;
        for (auto &kv : c.samplerDim) if (kv.second == 3) hasCube = true;
        bool isWorld = c.samplers.count(12) != 0;   // s12 = primary lightmap
        static int dumpedModel = 0, dumpedWorld = 0;
        int &slot = isWorld ? dumpedWorld : dumpedModel;
        if (hasCube && slot < 2) {
            ++slot;
            fprintf(stderr, "[gl] ===== ENVMAP PS DUMP %s%d world=%d =====\n"
                            "[gl] --- d3d9 asm ---\n%s"
                            "[gl] --- glsl ---\n%s\n[gl] ===== END DUMP =====\n",
                    isWorld ? "W" : "M", slot, isWorld ? 1 : 0,
                    dis.str().c_str(), out.c_str());
        }
    }
    return true;
}

// ---- GL shader objects ----------------------------------------------------
#if defined(__EMSCRIPTEN__)
// DIRECT GL getters: the engine's GL calls go through GLEW function pointers, which on
// wasm are indirect calls wrapped by EMULATE_FUNCTION_POINTER_CASTS thunks — a layer
// that has already mangled one call shape in this port (see the struct-by-value bug).
// Status getters through that path read 0 with EMPTY info logs for shaders that a raw
// JS context compiles fine. emscripten_gl* are ordinary exported functions (the same
// ones GetProcAddress hands out) — calling them DIRECTLY bypasses GLEW + the thunks.
extern "C" {
    void emscripten_glGetShaderiv(unsigned shader, unsigned pname, int *params);
    void emscripten_glGetShaderInfoLog(unsigned shader, int maxLength, int *length, char *infoLog);
}
extern int g_kbCtxIsLocal;   // 1 = direct calls legal; 0 = proxied, use GLEW dispatch
static inline void KB_glGetShaderiv(unsigned sh, unsigned pn, int *p) {
    if (g_kbCtxIsLocal) emscripten_glGetShaderiv(sh, pn, p); else glGetShaderiv(sh, pn, p);
}
static inline void KB_glGetShaderInfoLog(unsigned sh, int n, int *len, char *log) {
    if (g_kbCtxIsLocal) emscripten_glGetShaderInfoLog(sh, n, len, log); else glGetShaderInfoLog(sh, n, len, log);
}
#else
#define KB_glGetShaderiv      glGetShaderiv
#define KB_glGetShaderInfoLog glGetShaderInfoLog
#endif

static unsigned compileGL(GLenum stage, const std::string &src, const char *label) {
    // Diagnostic: dump every translated shader to $KB_DUMP_GLSL/<n>.<vert|frag>.
    if (const char *dir = getenv("KB_DUMP_GLSL")) {
        static int n = 0;
        char path[512];
        snprintf(path, sizeof(path), "%s/%04d.%s", dir, n++, stage == GL_VERTEX_SHADER ? "vert" : "frag");
        if (FILE *f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    KB_OpTag("compile", stage, (unsigned)src.size(), 0);
    unsigned s = glCreateShader(stage);
    if (!s) {
        // glCreateShader=0 = NO GL CONTEXT on this thread (or context lost) — never a
        // GLSL problem. (Pre-B38 this happened for loader-thread creates; now deferred.)
        fprintf(stderr, "[gl] %s: glCreateShader returned 0 (no context on this thread / context lost)\n", label);
        return 0;
    }
    while (glGetError() != GL_NO_ERROR) {}  // drain stale errors so the report below is ours
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    KB_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
#if defined(__EMSCRIPTEN__)
    // One-shot cross-check: does the GLEW/funcptr path agree with the direct call?
    {
        static bool checked = false;
        if (!checked) {
            checked = true;
            GLint okGlew = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &okGlew);
            if (okGlew != ok)
                fprintf(stderr, "[gl] GETTER MISMATCH: glGetShaderiv direct=%d via-GLEW=%d "
                                "(GLEW/fpcast path is mangling getter results)\n", ok, okGlew);
        }
    }
#endif
    if (!ok) {
        char log[2048];
        log[0] = 0;
        KB_glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        if (!log[0]) {
            // status=0 + EMPTY log = the compile is PENDING, not failed: with
            // KHR_parallel_shader_compile (auto-enabled; present on real GPUs, absent on
            // SwiftShader) WebGL's COMPILE_STATUS returns false WITHOUT blocking until the
            // async compile finishes. The shader is fine — the link path resolves it.
            // One-time deep probe: force completion with a BLOCKING query (any non-status
            // getter must wait, via the sync GPU-process path — no event loop needed) and
            // report what the shader REALLY is once forced.
#if defined(__EMSCRIPTEN__)
            // RAW-JS probe, bypassing every wasm/emscripten layer: ask the browser itself.
            // GL_SHADER_TYPE reading 0 via the C path means the driver returned null for
            // queries — on a LOST context every GL call silently no-ops exactly like this
            // (and the webglcontextlost event can never fire here: event delivery needs
            // this worker's event loop, which the render thread never pumps).
            static int probes = 0;
            if (probes < 3) {
                ++probes;
                EM_ASM({
                    try {
                        var sh = GL.shaders[$0];
                        var lost = GLctx.isContextLost();
                        var ty = GLctx.getShaderParameter(sh, 0x8B4F /*SHADER_TYPE*/);
                        var err = GLctx.getError();
                        var st = GLctx.getShaderParameter(sh, 0x8B81 /*COMPILE_STATUS*/);
                        var lg = GLctx.getShaderInfoLog(sh);
                        console.error('[gl] RAW probe shader=' + $0 + ': contextLost=' + lost +
                                      ' obj=' + (sh ? sh.constructor.name : 'null') +
                                      ' type=' + ty + ' err=0x' + (err ? err.toString(16) : '0') +
                                      ' status=' + st + ' log="' + (lg || '') + '"');
                        // Fresh recompile of the SAME source: compiles are synchronous now,
                        // so this status/log is LIVE — if the source genuinely fails on this
                        // driver, the real error text appears here.
                        var src = UTF8ToString($1);
                        var s2 = GLctx.createShader($2);
                        GLctx.shaderSource(s2, src);
                        GLctx.compileShader(s2);
                        var st2 = GLctx.getShaderParameter(s2, 0x8B81);
                        var ty2 = GLctx.getShaderParameter(s2, 0x8B4F);
                        var lg2 = GLctx.getShaderInfoLog(s2) || '';
                        GLctx.deleteShader(s2);
                        console.error('[gl] RAW2 fresh recompile: type=' + ty2 + ' status=' + st2 +
                                      ' srcLen=' + src.length + ' log="' + lg2.substring(0, 300) + '"' +
                                      ' src0="' + src.substring(0, 60).split(String.fromCharCode(10)).join(' / ') + '"');
                    } catch (e) { console.error('[gl] RAW probe threw: ' + e.message); }
                }, (int)s, src.c_str(), (int)stage);
            }
#endif
            return s;
        }
        fprintf(stderr, "[gl] %s translate/compile failed (shader=%u glErr=0x%x):\n%s\nGLSL:\n%s\n",
                label, s, glGetError(), log, src.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Constructors only TRANSLATE (pure CPU, any thread). The GL compile happens lazily
// in glShader() on the thread that links/draws, which is the one with the GL context
// current — see the note in gl_shader.h (loader threads have no context on the
// de-proxied web build; glCreateShader there returns 0 with an empty info log).
GLVertexShader::GLVertexShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    bool isPixel = false;
    translatedOk_ = TranslateD3D9Shader(function, glsl_, &isPixel) && !isPixel;
}
// Failed compiles RETRY with a cooldown instead of latching: a transiently-distressed
// GPU process (the de-proxy map-load storm) fails compiles with empty logs; latching
// turned a bad moment into permanently-black materials. Bounded: 8 tries, 30 presents
// apart — a real GLSL error just prints a few times and stops.
extern unsigned long g_kbPresentEnter;
static bool kbCompileGate(unsigned shader, int &tries, unsigned long &lastPres) {
    if (shader) return false;
    if (tries == 0) return true;
    if (tries >= 8) return false;
    return g_kbPresentEnter - lastPres >= 30;
}

unsigned GLVertexShader::glShader() {
    if (translatedOk_ && kbCompileGate(shader_, tries_, lastTryPres_)) {
        lastTryPres_ = g_kbPresentEnter;
        shader_ = compileGL(GL_VERTEX_SHADER, glsl_, "vertex shader");
        tries_ = shader_ ? 0 : tries_ + 1;
    }
    return shader_;
}
// Instanced variant: rewire the per-object matrix (vsc[matBase..matBase+matCount-1]) to read
// from instanced vertex attributes (kbInstRow0..N at locations locs[], divisor 1 set by the
// draw path). Pure text transform of the already-validated GLSL — no bytecode re-decode.
unsigned GLVertexShader::glShaderInstanced(unsigned matBase, int matCount, const int *locs) {
    if (!translatedOk_ || matCount < 1 || matCount > 8) return 0;   // up to 8 instanced vsc regs
    unsigned long long key = ((unsigned long long)matBase << 32) | ((unsigned long long)matCount << 24)
                           | ((unsigned)locs[0] & 0xff);
    auto it = instVariants_.find(key);
    if (it != instVariants_.end()) return it->second;

    std::string src = glsl_;
    // 1) Declare the instanced attributes right after the "#version" line.
    std::string decls;
    for (int i = 0; i < matCount; ++i)
        decls += "layout(location=" + std::to_string(locs[i]) + ") in vec4 kbInstRow"
               + std::to_string(i) + ";\n";
    size_t nl = src.find('\n');
    if (nl == std::string::npos) { instVariants_[key] = 0; return 0; }
    src.insert(nl + 1, decls);
    // 2) Replace every vsc[matBase+i] (exact, bracket-terminated -> no false 197 vs 1970 hits)
    //    with the matching instanced attribute.
    for (int i = 0; i < matCount; ++i) {
        std::string from = "vsc[" + std::to_string(matBase + i) + "]";
        std::string to   = "kbInstRow" + std::to_string(i);
        for (size_t pos = 0; (pos = src.find(from, pos)) != std::string::npos; )
        { src.replace(pos, from.size(), to); pos += to.size(); }
    }
    unsigned sh = compileGL(GL_VERTEX_SHADER, src, "vertex shader (instanced)");
    instVariants_[key] = sh;
    return sh;
}
GLVertexShader::~GLVertexShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLVertexShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }

// Walk the token stream to its END marker (0x0000FFFF), skipping comment blocks, so the
// bytecode can be copied for shadow-variant retranslation.
static size_t shaderTokenLen(const DWORD *t) {
    if (!t) return 0;
    size_t n = 1;   // version token
    while (t[n] != 0x0000FFFFu) {
        if ((t[n] & 0xFFFFu) == 0xFFFEu) n += ((t[n] >> 16) & 0x7FFF) + 1;  // comment block
        else ++n;
        if (n > (1u << 20)) return 0;   // runaway guard: malformed stream
    }
    return n + 1;
}

GLPixelShader::GLPixelShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    bool isPixel = false;
    translatedOk_ = TranslateD3D9Shader(function, glsl_, &isPixel, 0, &samplerMask_) && isPixel;
    if (translatedOk_) {
        size_t n = shaderTokenLen(function);
        if (n) func_.assign(function, function + n);
    }
}
unsigned GLPixelShader::glShader(unsigned shadowMask) {
    shadowMask &= samplerMask_;          // only samplers this shader actually reads
    if (shadowMask == 0 || func_.empty()) {
        if (translatedOk_ && kbCompileGate(shader_, tries_, lastTryPres_)) {
            lastTryPres_ = g_kbPresentEnter;
            shader_ = compileGL(GL_FRAGMENT_SHADER, glsl_, "pixel shader");
            tries_ = shader_ ? 0 : tries_ + 1;
        }
        return shader_;
    }
    Variant &v = variants_[shadowMask];
    if (kbCompileGate(v.gl, v.tries, v.lastPres)) {
        v.lastPres = g_kbPresentEnter;
        // Retranslate with the masked samplers typed sampler2DShadow (texldp becomes the
        // depth-compared textureProj — D3D9 hardware-shadow semantics) and compile.
        std::string glsl; bool isPix = false;
        if (TranslateD3D9Shader(func_.data(), glsl, &isPix, shadowMask) && isPix)
            v.gl = compileGL(GL_FRAGMENT_SHADER, glsl, "pixel shader (shadow variant)");
        v.tries = v.gl ? 0 : v.tries + 1;
    }
    return v.gl;
}
unsigned GLPixelShader::glShaderMat(unsigned matMask, unsigned shadowMask) {
    matMask &= samplerMask_;             // only samplers this shader actually reads
    if (matMask == 0 || func_.empty())
        return glShader(shadowMask);
    Variant &v = matVariants_[((unsigned long long)matMask << 32) | (shadowMask & samplerMask_)];
    if (kbCompileGate(v.gl, v.tries, v.lastPres)) {
        v.lastPres = g_kbPresentEnter;
        // Retranslate with matMask's stages typed sampler2DArray (layer = uMatLayer) on top
        // of any shadow typing — the pending-mask global (defined above) routes it into
        // the translator.
        KB_SetMatArrayTranslateMask(matMask);
        std::string glsl; bool isPix = false;
        bool ok = TranslateD3D9Shader(func_.data(), glsl, &isPix, shadowMask) && isPix;
        KB_SetMatArrayTranslateMask(0);
        if (ok)
            v.gl = compileGL(GL_FRAGMENT_SHADER, glsl, "pixel shader (matarray variant)");
        v.tries = v.gl ? 0 : v.tries + 1;
    }
    return v.gl;
}
GLPixelShader::~GLPixelShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLPixelShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }
