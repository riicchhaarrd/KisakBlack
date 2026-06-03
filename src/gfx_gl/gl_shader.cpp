// gl_shader.cpp — DX9 bytecode → GLSL 120 translator + GL shader objects.
#include "gl_shader.h"

#include <GL/glew.h>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace {

// --- DX9 bytecode constants (documented format) ---
enum {  // opcodes (token & 0xFFFF) — D3DSIO_*
    OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7,
    OP_DP3 = 8, OP_DP4 = 9, OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13,
    OP_EXP = 14, OP_LOG = 15, OP_LIT = 16, OP_DST = 17, OP_LRP = 18, OP_FRC = 19,
    OP_M4x4 = 20, OP_M4x3 = 21, OP_M3x4 = 22, OP_M3x3 = 23, OP_M3x2 = 24,
    OP_CALL = 25, OP_CALLNZ = 26, OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29, OP_LABEL = 30,
    OP_POW = 32, OP_CRS = 33, OP_SGN = 34, OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37,
    OP_REP = 38, OP_ENDREP = 39, OP_IF = 40, OP_IFC = 41, OP_ELSE = 42, OP_ENDIF = 43,
    OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48,
    OP_TEXKILL = 65, OP_TEXLD = 66, OP_TEXLDP = 67, OP_TEXLDB = 68,
    OP_CMP = 88, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92,
    OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95,
    OP_DCL = 31, OP_DEF = 81, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum {  // register types
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4,
    RT_ATTROUT = 5, RT_OUTPUT = 6, RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9,
    RT_SAMPLER = 10, RT_CONSTBOOL = 11, RT_LOOP = 12, RT_LABEL = 16, RT_PREDICATE = 19,
};
// In a pixel shader RT_ADDR(3) is the texture register t#; in a vertex shader it is
// the address register a0. They are disambiguated by c.isPixel where it matters.

struct Operand {
    int type = 0, reg = 0, swizzle = 0xE4, writemask = 0xF, smod = 0, dmod = 0, rel = 0;
    int relType = 0, relReg = 0, relComp = 0;   // a0/aL relative-addressing source
};

Operand decodeParam(DWORD t) {
    Operand o;
    o.reg       = (int)(t & 0x7FF);
    o.type      = (int)(((t >> 28) & 0x7) | ((t & 0x1800) >> 8));
    o.writemask = (int)((t >> 16) & 0xF);          // dst
    o.dmod      = (int)((t >> 20) & 0xF);          // dst modifier (_sat = 1)
    o.swizzle   = (int)((t >> 16) & 0xFF);         // src
    o.smod      = (int)((t >> 24) & 0xF);          // src modifier (D3DSPSM_*)
    o.rel       = (int)((t >> 13) & 0x3);          // relative addressing mode (!=0 => extra addr token)
    return o;
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

struct Ctx {
    bool isPixel = false;
    const char *cArr() const { return isPixel ? "psc" : "vsc"; }
    std::map<int, std::pair<int,int>> inputs;   // reg -> (usage, usageIndex)
    std::map<int, std::pair<int,int>> outputs;  // reg -> (usage, usageIndex)  (vertex)
    std::map<int, int> samplerDim;              // sampler reg -> 2 (2D) / 3 (cube) / 4 (volume)
    std::map<int, float[4]> defs;
    std::map<int, int> idefs;                   // DEFI integer constant reg -> count (.x)
    std::set<int> usedTemps;
    bool usedConst = false, usedAddr = false, usedLoop = false;
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

// Relative-addressing index expression for c[a0.x + reg] style accesses.
std::string relIndex(Ctx &c, const Operand &o) {
    std::ostringstream s;
    if (o.relType == RT_LOOP) { c.usedLoop = true; s << "(aL + " << o.reg << ")"; }
    else                      { c.usedAddr = true; s << "(aAddr + " << o.reg << ")"; }
    return s.str();
}

std::string regName(Ctx &c, const Operand &o, bool isDest) {
    switch (o.type) {
        case RT_TEMP:    c.usedTemps.insert(o.reg); { std::ostringstream s; s << "r" << o.reg; return s.str(); }
        case RT_CONST:
            if (!o.rel && c.defs.count(o.reg)) { std::ostringstream s; s << "c" << o.reg << "_def"; return s.str(); }
            c.usedConst = true;
            { std::ostringstream s; s << c.cArr() << "[" << (o.rel ? relIndex(c, o) : std::to_string(o.reg)) << "]"; return s.str(); }
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
        case RT_RASTOUT:  return o.reg == 0 ? "gl_Position" : (o.reg == 1 ? "vec4(gl_FogFragCoord)" : "vec4(gl_PointSize)");
        case RT_COLOROUT: return "gl_FragColor";
        case RT_DEPTHOUT: return "vec4(gl_FragDepth)";
        case RT_SAMPLER:  { std::ostringstream s; s << "s" << o.reg; return s.str(); }
        default: (void)isDest; return "vec4(0.0)";
    }
}

// Apply a D3DSPSM_* source modifier.
std::string applyMod(const std::string &e, int mod) {
    switch (mod) {
        case 0:  return e;                              // none
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
        case 13: return "(1.0 - " + e + ")";            // not (boolean)
        default: return e;
    }
}

std::string srcExpr(Ctx &c, const Operand &o) {
    return applyMod(regName(c, o, false) + swizStr(o.swizzle), o.smod);
}

// Texture-sample function + coordinate dimensions for a sampler's declared type.
const char *sampleFn(Ctx &c, int reg) {
    auto it = c.samplerDim.find(reg);
    int dim = it != c.samplerDim.end() ? it->second : 2;
    return dim == 3 ? "textureCube" : dim == 4 ? "texture3D" : "texture2D";
}
const char *sampleSwiz(Ctx &c, int reg) {
    auto it = c.samplerDim.find(reg);
    int dim = it != c.samplerDim.end() ? it->second : 2;
    return dim == 2 ? ".xy" : ".xyz";   // cube/volume need 3 coords
}

// Emit one arithmetic/texture instruction as `dst<mask> = (<expr>)<mask>;`.
void emitInstr(Ctx &c, int op, int ctrl, const Operand *src, int nsrc, const Operand &dst,
               std::ostringstream &body, const std::string &ind) {
    (void)ctrl;
    auto s = [&](int i) { return srcExpr(c, src[i]); };
    if (op == OP_TEXKILL) {
        body << ind << "if (any(lessThan((" << regName(c, dst, true) << ").xyz, vec3(0.0)))) discard;\n";
        return;
    }
    if (op == OP_MOVA) {  // address register: aAddr = int(round(src.x))
        c.usedAddr = true;
        body << ind << "aAddr = int(floor(" << s(0) << ".x + 0.5));\n";
        return;
    }
    auto matrix = [&](int rows, int dim) {  // dst = mul(src0, matrix at src1)
        c.usedConst = true;
        static const char *dm = "xyzw";
        const std::string coord = dim == 3 ? ".xyz" : "";
        std::string dn = regName(c, dst, true);
        for (int r = 0; r < rows; ++r) {
            Operand row = src[1]; row.reg += r;
            body << ind << dn << "." << dm[r] << " = dot(" << s(0) << coord << ", "
                 << regName(c, row, false) << coord << ");\n";
        }
    };
    switch (op) {
        case OP_M4x4: matrix(4, 4); return;
        case OP_M4x3: matrix(3, 4); return;
        case OP_M3x4: matrix(4, 3); return;
        case OP_M3x3: matrix(3, 3); return;
        case OP_M3x2: matrix(2, 3); return;
        default: break;
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
        case OP_SGN:   expr = "sign(" + s(0) + ")"; break;
        case OP_SLT:   expr = "vec4(lessThan(" + s(0) + ", " + s(1) + "))"; break;
        case OP_SGE:   expr = "vec4(greaterThanEqual(" + s(0) + ", " + s(1) + "))"; break;
        case OP_CMP:   expr = "mix(" + s(2) + ", " + s(1) + ", vec4(greaterThanEqual(" + s(0) + ", vec4(0.0))))"; break;  // src0>=0 ? src1 : src2
        case OP_RCP:   expr = "vec4(1.0 / (" + s(0) + ").x)"; break;
        case OP_RSQ:   expr = "vec4(inversesqrt(abs((" + s(0) + ").x)))"; break;
        case OP_EXP:   expr = "vec4(exp2((" + s(0) + ").x))"; break;
        case OP_LOG:   expr = "vec4(log2(abs((" + s(0) + ").x)))"; break;
        case OP_POW:   expr = "vec4(pow(abs((" + s(0) + ").x), (" + s(1) + ").x))"; break;
        case OP_NRM:   expr = "vec4(normalize((" + s(0) + ").xyz), 0.0)"; break;
        case OP_CRS:   expr = "vec4(cross((" + s(0) + ").xyz, (" + s(1) + ").xyz), 0.0)"; break;
        case OP_SINCOS:expr = "vec4(cos((" + s(0) + ").x), sin((" + s(0) + ").x), 0.0, 0.0)"; break;
        case OP_DP3:   expr = "vec4(dot((" + s(0) + ").xyz, (" + s(1) + ").xyz))"; break;
        case OP_DP4:   expr = "vec4(dot(" + s(0) + ", " + s(1) + "))"; break;
        case OP_DP2ADD:expr = "vec4(dot((" + s(0) + ").xy, (" + s(1) + ").xy) + (" + s(2) + ").x)"; break;
        case OP_DSX:   expr = "dFdx(" + s(0) + ")"; break;
        case OP_DSY:   expr = "dFdy(" + s(0) + ")"; break;
        case OP_DST:   expr = "vec4(1.0, (" + s(0) + ").y * (" + s(1) + ").y, (" + s(0) + ").z, (" + s(1) + ").w)"; break;
        case OP_LIT: { std::string n = s(0);  // (1, max(x,0), x>0 ? pow(max(y,0), clamp(w,-128,128)) : 0, 1)
            expr = "vec4(1.0, max((" + n + ").x, 0.0), ((" + n + ").x > 0.0 ? pow(max((" + n +
                   ").y, 0.0), clamp((" + n + ").w, -128.0, 128.0)) : 0.0), 1.0)"; break; }
        case OP_TEXLD:
        case OP_TEXLDP:
        case OP_TEXLDB:
        case OP_TEXLDL:
        case OP_TEXLDD: {
            int sreg = src[1].reg;
            expr = std::string(sampleFn(c, sreg)) + "(" + regName(c, src[1], false) + ", (" + s(0) + ")" + sampleSwiz(c, sreg) + ")";
            break;
        }
        default: (void)nsrc; return;
    }
    std::string dn = regName(c, dst, true);
    std::string m  = maskStr(dst.writemask);
    if (dst.dmod & 1) expr = "clamp(" + expr + ", 0.0, 1.0)";   // _sat destination modifier
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

bool TranslateD3D9Shader(const DWORD *tok, std::string &out, bool *outIsPixel) {
    Ctx c;
    DWORD ver = *tok++;
    c.isPixel = (ver >> 16) == 0xFFFF;
    if (outIsPixel) *outIsPixel = c.isPixel;

    std::ostringstream body;
    int indent = 1, loopId = 0;
    auto ind = [](int n) { return std::string(2 * (n < 1 ? 1 : n), ' '); };
    // D3DSHADER_COMPARISON (ctrl): 1 GT, 2 EQ, 3 GE, 4 LT, 5 NE, 6 LE.
    auto cmp = [&](const Operand &a, int cc, const Operand &b) {
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
            DWORD declTok = *tok;
            Operand reg = decodeParam(tok[1]);
            tok += len;
            if (reg.type == RT_SAMPLER) {
                int sttype = (int)((declTok >> 27) & 0xF);  // D3DSAMPLER_TEXTURE_TYPE: 2=2D 3=cube 4=vol
                c.samplerDim[reg.reg] = (sttype == 3 || sttype == 4) ? sttype : 2;
            } else {
                int usage = (int)(declTok & 0x1F);
                int index = (int)((declTok >> 16) & 0xF);
                if (reg.type == RT_INPUT)       c.inputs[reg.reg]  = {usage, index};
                else if (reg.type == RT_OUTPUT) c.outputs[reg.reg] = {usage, index};
            }
            continue;
        }
        if (op == OP_DEF) {
            Operand reg = decodeParam(tok[0]);
            float f[4]; std::memcpy(f, &tok[1], sizeof(f));
            std::memcpy(c.defs[reg.reg], f, sizeof(f));
            tok += len; continue;
        }
        if (op == OP_DEFI) {   // integer constant (loop counts)
            Operand reg = decodeParam(tok[0]);
            c.idefs[reg.reg] = (int)tok[1];
            tok += len; continue;
        }
        if (op == OP_DEFB || op == OP_LABEL || op == OP_CALL || op == OP_CALLNZ ||
            op == OP_RET  || op == OP_SETP) { tok += len; continue; }  // unsupported control: skip

        // Structural control flow with no GLSL destination.
        if (op == OP_ELSE)    { if (indent > 1) --indent; body << ind(indent) << "} else {\n"; ++indent; tok += len; continue; }
        if (op == OP_ENDIF || op == OP_ENDREP || op == OP_ENDLOOP) { if (indent > 1) --indent; body << ind(indent) << "}\n"; tok += len; continue; }
        if (op == OP_BREAK)   { body << ind(indent) << "break;\n"; tok += len; continue; }

        // Decode dst + sources, consuming a following address token for any
        // relatively-addressed operand (c[a0.x + n]).
        Operand ops[6]; int nops = 0;
        { const DWORD *p = tok, *e = tok + len;
          while (p < e && nops < 6) {
              Operand o = decodeParam(*p++);
              if (o.rel && p < e) { Operand a = decodeParam(*p++);
                                    o.relType = a.type; o.relReg = a.reg; o.relComp = a.swizzle & 3; }
              ops[nops++] = o;
          } }
        tok += len;

        if (op == OP_IF)     { body << ind(indent) << "if (bool((" << srcExpr(c, ops[0]) << ").x)) {\n"; ++indent; continue; }
        if (op == OP_IFC)    { body << ind(indent) << "if (" << cmp(ops[0], ctrl, ops[1]) << ") {\n"; ++indent; continue; }
        if (op == OP_BREAKC) { body << ind(indent) << "if (" << cmp(ops[0], ctrl, ops[1]) << ") break;\n"; continue; }
        if (op == OP_REP || op == OP_LOOP) {
            int cnt = 4;                                   // fallback when count is a runtime int reg
            const Operand &countOp = (op == OP_LOOP && nops > 1) ? ops[1] : ops[0];
            auto di = c.idefs.find(countOp.reg);
            if (di != c.idefs.end() && di->second > 0) cnt = di->second;
            c.usedLoop = true;
            int id = loopId++;
            body << ind(indent) << "for (int aL" << id << " = 0; aL" << id << " < " << cnt << "; ++aL" << id << ") {\n";
            body << ind(indent + 1) << "aL = aL" << id << ";\n";
            ++indent; continue;
        }

        // Arithmetic / texture: ops[0] = dst, ops[1..] = sources.
        Operand dst = ops[0];
        Operand src[4]; int nsrc = nops - 1; if (nsrc < 0) nsrc = 0; if (nsrc > 4) nsrc = 4;
        for (int i = 0; i < nsrc; ++i) src[i] = ops[1 + i];
        emitInstr(c, op, ctrl, src, nsrc, dst, body, ind(indent));
    }

    // Assemble the GLSL translation unit.
    std::ostringstream o;
    o << "#version 120\n";
    if (c.isPixel) {
        for (auto &in : c.inputs)  o << "varying vec4 " << varyingName(in.second.first, in.second.second) << ";\n";
        for (auto &sm : c.samplerDim) {
            const char *t = sm.second == 3 ? "samplerCube" : sm.second == 4 ? "sampler3D" : "sampler2D";
            o << "uniform " << t << " s" << sm.first << ";\n";
        }
    } else {
        for (auto &in : c.inputs)  o << "attribute vec4 " << GLAttribName(in.second.first, in.second.second) << ";\n";
        for (auto &ou : c.outputs)
            if (ou.second.first != D3DDECLUSAGE_POSITION && ou.second.first != D3DDECLUSAGE_POSITIONT)
                o << "varying vec4 " << varyingName(ou.second.first, ou.second.second) << ";\n";
    }
    if (c.usedConst) o << "uniform vec4 " << c.cArr() << "[256];\n";
    for (auto &d : c.defs)
        o << "const vec4 c" << d.first << "_def = vec4(" << d.second[0] << ", " << d.second[1]
          << ", " << d.second[2] << ", " << d.second[3] << ");\n";
    o << "void main() {\n";
    if (c.usedAddr) o << "  int aAddr = 0;\n";
    if (c.usedLoop) o << "  int aL = 0;\n";
    for (int r : c.usedTemps) o << "  vec4 r" << r << " = vec4(0.0);\n";
    o << body.str();
    o << "}\n";
    out = o.str();
    return true;
}

// ---- GL shader objects ----------------------------------------------------
static unsigned compileGL(GLenum stage, const std::string &src, const char *label) {
    unsigned s = glCreateShader(stage);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[gl] %s translate/compile failed:\n%s\nGLSL:\n%s\n", label, log, src.c_str());
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLVertexShader::GLVertexShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    bool isPixel = false;
    if (TranslateD3D9Shader(function, glsl_, &isPixel) && !isPixel)
        shader_ = compileGL(GL_VERTEX_SHADER, glsl_, "vertex shader");
}
GLVertexShader::~GLVertexShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLVertexShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }

GLPixelShader::GLPixelShader(IDirect3DDevice9 *device, const DWORD *function) : device_(device) {
    bool isPixel = false;
    if (TranslateD3D9Shader(function, glsl_, &isPixel) && isPixel)
        shader_ = compileGL(GL_FRAGMENT_SHADER, glsl_, "pixel shader");
}
GLPixelShader::~GLPixelShader() { if (shader_) glDeleteShader(shader_); }
HRESULT WINAPI GLPixelShader::GetDevice(IDirect3DDevice9 **pp) { if (!pp) return E_INVALIDARG; *pp = device_; if (device_) device_->AddRef(); return D3D_OK; }
