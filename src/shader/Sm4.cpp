#include "shader/Sm4.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace cardbridge::shader {

namespace {

// D3D10_SB_OPCODE_TYPE values used by Shader Forge programs (the rest are rejected when met).
enum Op : uint16_t {
    OP_ADD = 0, OP_AND = 1, OP_DISCARD = 13, OP_DIV = 14, OP_DP2 = 15, OP_DP3 = 16, OP_DP4 = 17, OP_ELSE = 18, OP_ENDIF = 21,
    OP_EQ = 24, OP_EXP = 25, OP_FRC = 26, OP_GE = 29, OP_IF = 31, OP_LOG = 47, OP_LT = 49, OP_MAD = 50, OP_MIN = 51, OP_MAX = 52,
    OP_CUSTOMDATA = 53, OP_MOV = 54, OP_MOVC = 55, OP_MUL = 56, OP_NE = 57, OP_NOP = 58, OP_OR = 60, OP_RET = 62, OP_ROUND_NE = 64,
    OP_ROUND_NI = 65, OP_ROUND_PI = 66, OP_ROUND_Z = 67, OP_RSQ = 68, OP_SAMPLE = 69, OP_SAMPLE_C = 70, OP_SAMPLE_C_LZ = 71,
    OP_SAMPLE_L = 72, OP_SQRT = 75, OP_SINCOS = 77, OP_DCL_FIRST = 88, OP_DCL_TEMPS = 104,
};

uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float Flt(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct Reader {
    const std::vector<uint32_t>& t;
    size_t p;
};

bool ReadOperand(Reader& r, size_t end, Sm4Operand& o, std::string* why) {
    if (r.p >= end) { if (why) *why = "operand overruns the instruction"; return false; }
    uint32_t tok = r.t[r.p++];
    uint32_t numComp = tok & 3, selMode = (tok >> 2) & 3, sel = (tok >> 4) & 0xFF;
    o.type = static_cast<uint8_t>((tok >> 12) & 0xFF);
    uint32_t dim = (tok >> 20) & 3;
    o.numComponents = numComp == 1 ? 1 : 4;
    o.selMode = static_cast<uint8_t>(selMode);
    if (selMode == 0) o.mask = static_cast<uint8_t>(sel & 0xF);
    else if (selMode == 1) for (int i = 0; i < 4; ++i) o.swz[i] = static_cast<uint8_t>((sel >> (2 * i)) & 3);
    else for (int i = 0; i < 4; ++i) o.swz[i] = static_cast<uint8_t>(sel & 3);
    uint32_t ext = tok >> 31;
    while (ext) {   // extended operand tokens: modifiers
        if (r.p >= end) return false;
        uint32_t et = r.t[r.p++];
        if ((et & 0x3F) == 1) o.modifier = static_cast<uint8_t>((et >> 6) & 0xFF);
        ext = et >> 31;
    }
    if (o.type == 4) {   // immediate
        uint32_t n = numComp == 1 ? 1 : 4;
        for (uint32_t i = 0; i < n; ++i) {
            if (r.p >= end) return false;
            o.imm[i] = r.t[r.p++];
        }
        if (n == 1) for (int i = 1; i < 4; ++i) o.imm[i] = o.imm[0];
        return true;
    }
    for (uint32_t d = 0; d < dim; ++d) {
        uint32_t repr = (tok >> (22 + 3 * d)) & 7;
        if (repr == 0) {                       // 32-bit immediate index
            if (r.p >= end) return false;
            (d == 0 ? o.index0 : o.index1) = static_cast<int32_t>(r.t[r.p++]);
        } else if (repr == 2 || repr == 3) {   // relative: [rN.c (+ imm)]
            int32_t base = 0;
            if (repr == 3) { if (r.p >= end) return false; base = static_cast<int32_t>(r.t[r.p++]); }
            Sm4Operand rel;
            if (!ReadOperand(r, end, rel, why)) return false;
            (d == 0 ? o.index0 : o.index1) = base;
            o.relReg = rel.index0;
            o.relComp = rel.swz[0];
        } else {
            if (why) *why = "unsupported operand index representation";
            return false;
        }
    }
    return true;
}

} // namespace

bool DecodeSm4(const std::vector<uint32_t>& tokens, Sm4Program& out, std::string* why) {
    if (tokens.size() < 2) { if (why) *why = "empty program"; return false; }
    out.major = (tokens[0] >> 4) & 0xF;
    out.minor = tokens[0] & 0xF;
    out.isPixel = (tokens[0] >> 16) == 0;
    size_t p = 2;
    std::vector<size_t> ifStack;
    while (p < tokens.size()) {
        uint32_t tok = tokens[p];
        uint16_t op = static_cast<uint16_t>(tok & 0x7FF);
        size_t len = (tok >> 24) & 0x7F;
        if (op == OP_CUSTOMDATA) { len = tokens[p + 1]; }
        if (len == 0 || p + len > tokens.size()) { if (why) *why = "bad instruction length"; return false; }
        size_t end = p + len;
        if (op >= OP_DCL_FIRST && op != OP_CUSTOMDATA) {
            if (op == OP_DCL_TEMPS) out.tempCount = tokens[p + 1];
            p = end;
            continue;
        }
        if (op == OP_CUSTOMDATA || op == OP_NOP) { p = end; continue; }
        Sm4Instr ins;
        ins.op = op;
        ins.saturate = (tok & 0x2000) != 0;
        ins.testNonZero = (tok & 0x40000) != 0;
        Reader r{tokens, p + 1};
        uint32_t ext = tok >> 31;
        while (ext && r.p < end) ext = tokens[r.p++] >> 31;   // extended opcode tokens (sample offsets): not used for values here
        while (r.p < end) {
            Sm4Operand o;
            if (!ReadOperand(r, end, o, why)) return false;
            ins.operands.push_back(o);
        }
        size_t idx = out.code.size();
        if (op == OP_IF) ifStack.push_back(idx);
        else if (op == OP_ELSE) {
            if (ifStack.empty()) { if (why) *why = "else without if"; return false; }
            out.code[ifStack.back()].jump = static_cast<int>(idx);   // if jumps to the else
            ifStack.back() = idx;                                    // the else now waits for its endif
            ins.jump = -2;                                           // marker: this is an else
        } else if (op == OP_ENDIF) {
            if (ifStack.empty()) { if (why) *why = "endif without if"; return false; }
            out.code[ifStack.back()].jump = static_cast<int>(idx);
            ifStack.pop_back();
        }
        out.code.push_back(std::move(ins));
        p = end;
    }
    return true;
}

namespace {

struct Machine {
    const Sm4Program& prog;
    Sm4Io& io;
    std::vector<uint32_t>& temps;   // 4 words per temp register (a reusable buffer: the interpreter runs once per pixel)
    std::string* why;

    uint32_t* Reg(const Sm4Operand& o) {
        if (o.type == 0) {
            size_t i = static_cast<size_t>(o.index0) * 4;
            return i + 4 <= temps.size() ? &temps[i] : nullptr;
        }
        if (o.type == 2) return reinterpret_cast<uint32_t*>(io.output[std::min(o.index0, 31)]);
        return nullptr;
    }

    // Four component values (raw bits) of a source operand, swizzled; modifiers are applied by the caller per type.
    void Fetch(const Sm4Operand& o, uint32_t v[4]) {
        const uint32_t* base = nullptr;
        uint32_t tmp[4];
        if (o.type == 4) {
            for (int i = 0; i < 4; ++i) tmp[i] = o.imm[i];
            base = tmp;
        } else if (o.type == 0) {
            size_t i = static_cast<size_t>(o.index0) * 4;
            base = i + 4 <= temps.size() ? &temps[i] : nullptr;
        } else if (o.type == 1) {
            base = reinterpret_cast<const uint32_t*>(io.input[std::min(o.index0, 31)]);
        } else if (o.type == 2) {
            base = reinterpret_cast<const uint32_t*>(io.output[std::min(o.index0, 31)]);
        } else if (o.type == 8) {   // constant buffer cb[slot][element]
            size_t slot = static_cast<size_t>(o.index0);
            int32_t el = o.index1;
            if (o.relReg >= 0) {
                size_t ri = static_cast<size_t>(o.relReg) * 4 + static_cast<size_t>(o.relComp);
                if (ri < temps.size()) el += static_cast<int32_t>(temps[ri]);
            }
            if (slot < io.constantBuffers.size() && io.constantBuffers[slot]) base = reinterpret_cast<const uint32_t*>(io.constantBuffers[slot] + static_cast<size_t>(el) * 4);
        }
        if (!base) { for (int i = 0; i < 4; ++i) v[i] = 0; return; }
        if (o.type == 4 && o.numComponents == 1) { for (int i = 0; i < 4; ++i) v[i] = base[0]; return; }
        for (int i = 0; i < 4; ++i) v[i] = base[o.swz[i]];
    }

    void FetchF(const Sm4Operand& o, float v[4]) {
        uint32_t raw[4];
        Fetch(o, raw);
        for (int i = 0; i < 4; ++i) {
            float f = Flt(raw[i]);
            if (o.modifier & 2) f = std::fabs(f);
            if (o.modifier & 1) f = -f;
            v[i] = f;
        }
    }

    void Store(const Sm4Operand& d, const float v[4], bool sat) {
        uint32_t* r = Reg(d);
        if (!r) return;
        for (int i = 0; i < 4; ++i)
            if (d.mask & (1 << i)) {
                float f = v[i];
                if (sat) f = std::isnan(f) ? 0.0f : std::min(1.0f, std::max(0.0f, f));
                r[i] = Bits(f);
            }
    }
    void StoreBits(const Sm4Operand& d, const uint32_t v[4]) {
        uint32_t* r = Reg(d);
        if (!r) return;
        for (int i = 0; i < 4; ++i) if (d.mask & (1 << i)) r[i] = v[i];
    }

    bool Run() {
        temps.assign(static_cast<size_t>(std::max<uint32_t>(prog.tempCount, 1)) * 4, 0);
        size_t pc = 0;
        while (pc < prog.code.size()) {
            const Sm4Instr& in = prog.code[pc];
            const auto& ops = in.operands;
            float a[4], b[4], c[4], res[4];
            switch (in.op) {
            case OP_MOV: FetchF(ops[1], a); { uint32_t raw[4]; Fetch(ops[1], raw); if (ops[1].modifier) Store(ops[0], a, in.saturate); else if (in.saturate) Store(ops[0], a, true); else StoreBits(ops[0], raw); } break;
            case OP_ADD: FetchF(ops[1], a); FetchF(ops[2], b); for (int i = 0; i < 4; ++i) res[i] = a[i] + b[i]; Store(ops[0], res, in.saturate); break;
            case OP_MUL: FetchF(ops[1], a); FetchF(ops[2], b); for (int i = 0; i < 4; ++i) res[i] = a[i] * b[i]; Store(ops[0], res, in.saturate); break;
            case OP_MAD: FetchF(ops[1], a); FetchF(ops[2], b); FetchF(ops[3], c); for (int i = 0; i < 4; ++i) res[i] = a[i] * b[i] + c[i]; Store(ops[0], res, in.saturate); break;
            case OP_DIV: FetchF(ops[1], a); FetchF(ops[2], b); for (int i = 0; i < 4; ++i) res[i] = a[i] / b[i]; Store(ops[0], res, in.saturate); break;
            case OP_MIN: FetchF(ops[1], a); FetchF(ops[2], b); for (int i = 0; i < 4; ++i) res[i] = std::fmin(a[i], b[i]); Store(ops[0], res, in.saturate); break;
            case OP_MAX: FetchF(ops[1], a); FetchF(ops[2], b); for (int i = 0; i < 4; ++i) res[i] = std::fmax(a[i], b[i]); Store(ops[0], res, in.saturate); break;
            case OP_DP2: FetchF(ops[1], a); FetchF(ops[2], b); { float d = a[0] * b[0] + a[1] * b[1]; for (int i = 0; i < 4; ++i) res[i] = d; } Store(ops[0], res, in.saturate); break;
            case OP_DP3: FetchF(ops[1], a); FetchF(ops[2], b); { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; for (int i = 0; i < 4; ++i) res[i] = d; } Store(ops[0], res, in.saturate); break;
            case OP_DP4: FetchF(ops[1], a); FetchF(ops[2], b); { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; for (int i = 0; i < 4; ++i) res[i] = d; } Store(ops[0], res, in.saturate); break;
            case OP_SQRT: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::sqrt(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_RSQ: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = 1.0f / std::sqrt(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_EXP: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::exp2(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_LOG: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::log2(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_FRC: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = a[i] - std::floor(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_ROUND_NE: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::nearbyint(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_ROUND_NI: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::floor(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_ROUND_PI: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::ceil(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_ROUND_Z: FetchF(ops[1], a); for (int i = 0; i < 4; ++i) res[i] = std::trunc(a[i]); Store(ops[0], res, in.saturate); break;
            case OP_SINCOS: {
                FetchF(ops[2], a);
                float s[4], co[4];
                for (int i = 0; i < 4; ++i) { s[i] = std::sin(a[i]); co[i] = std::cos(a[i]); }
                if (ops[0].type != 8 && ops[0].type != 4 && ops[0].mask) Store(ops[0], s, in.saturate);
                if (ops[1].mask && ops[1].type != 4) Store(ops[1], co, in.saturate);
                break;
            }
            case OP_LT: case OP_GE: case OP_EQ: case OP_NE: {
                FetchF(ops[1], a); FetchF(ops[2], b);
                uint32_t m[4];
                for (int i = 0; i < 4; ++i) {
                    bool t = in.op == OP_LT ? a[i] < b[i] : in.op == OP_GE ? a[i] >= b[i] : in.op == OP_EQ ? a[i] == b[i] : a[i] != b[i];
                    m[i] = t ? 0xFFFFFFFFu : 0u;
                }
                StoreBits(ops[0], m);
                break;
            }
            case OP_AND: case OP_OR: {
                uint32_t x[4], y[4], m[4];
                Fetch(ops[1], x); Fetch(ops[2], y);
                for (int i = 0; i < 4; ++i) m[i] = in.op == OP_AND ? (x[i] & y[i]) : (x[i] | y[i]);
                StoreBits(ops[0], m);
                break;
            }
            case OP_MOVC: {
                uint32_t cond[4], x[4], y[4], m[4];
                Fetch(ops[1], cond); Fetch(ops[2], x); Fetch(ops[3], y);
                for (int i = 0; i < 4; ++i) m[i] = cond[i] ? x[i] : y[i];
                if (in.saturate) { float f[4]; for (int i = 0; i < 4; ++i) f[i] = Flt(m[i]); Store(ops[0], f, true); }
                else StoreBits(ops[0], m);
                break;
            }
            case OP_SAMPLE: case OP_SAMPLE_L: case OP_SAMPLE_C: case OP_SAMPLE_C_LZ: {
                // dst, coord, resource, sampler [, lod | reference]
                float coord[4], tex[4] = {1, 1, 1, 1};
                FetchF(ops[1], coord);
                float lod = -1.0f;
                if (in.op == OP_SAMPLE_L && ops.size() > 4) { float l[4]; FetchF(ops[4], l); lod = l[0]; }
                int tex_reg = ops[2].index0, smp = ops[3].index0;
                if (in.op == OP_SAMPLE_C || in.op == OP_SAMPLE_C_LZ) {
                    for (int i = 0; i < 4; ++i) tex[i] = 1.0f;   // shadow comparison: treat as fully lit
                } else if (io.textures) {
                    io.textures->Sample(tex_reg, smp, coord, lod, tex);
                }
                // The resource operand's swizzle picks which texture channel lands in which destination component.
                for (int i = 0; i < 4; ++i) res[i] = tex[ops[2].swz[i]];
                Store(ops[0], res, in.saturate);
                break;
            }
            case OP_DISCARD: {
                uint32_t x[4];
                Fetch(ops[0], x);
                bool nz = x[0] != 0;
                if (nz == in.testNonZero) { io.discarded = true; return true; }
                break;
            }
            case OP_IF: {
                uint32_t x[4];
                Fetch(ops[0], x);
                bool nz = x[0] != 0;
                if (nz != in.testNonZero) { pc = static_cast<size_t>(in.jump) + 1; continue; }   // past the else (run its body) or past the endif
                break;
            }
            case OP_ELSE: pc = static_cast<size_t>(in.jump) + 1; continue;   // the true branch is done: skip to after the endif
            case OP_ENDIF: break;
            case OP_RET: return true;
            default:
                if (why) *why = "unsupported SM4 opcode " + std::to_string(in.op);
                return false;
            }
            if (io.trace && !ops.empty() && ops[0].type == 0 && in.op != OP_IF && in.op != OP_DISCARD) {
                const uint32_t* r = &temps[static_cast<size_t>(ops[0].index0) * 4];
                std::fprintf(stderr, "    [%zu] op%u -> r%d = %.4f %.4f %.4f %.4f\n", pc, in.op, ops[0].index0, Flt(r[0]), Flt(r[1]), Flt(r[2]), Flt(r[3]));
            }
            ++pc;
        }
        return true;
    }
};

} // namespace

bool RunSm4(const Sm4Program& prog, Sm4Io& io, std::string* why) {
    static thread_local std::vector<uint32_t> buffer;
    Machine m{prog, io, buffer, why};
    return m.Run();
}

std::string DisassembleSm4(const Sm4Program& prog) {
    const char* names[128] = {};
    auto N = [&](uint16_t op, const char* s) { if (op < 128) names[op] = s; };
    N(OP_ADD, "add"); N(OP_AND, "and"); N(OP_DISCARD, "discard"); N(OP_DIV, "div"); N(OP_DP2, "dp2"); N(OP_DP3, "dp3");
    N(OP_DP4, "dp4"); N(OP_ELSE, "else"); N(OP_ENDIF, "endif"); N(OP_EQ, "eq"); N(OP_EXP, "exp"); N(OP_FRC, "frc");
    N(OP_GE, "ge"); N(OP_IF, "if"); N(OP_LOG, "log"); N(OP_LT, "lt"); N(OP_MAD, "mad"); N(OP_MIN, "min"); N(OP_MAX, "max");
    N(OP_CUSTOMDATA, "customdata"); N(OP_MOV, "mov"); N(OP_MOVC, "movc"); N(OP_MUL, "mul"); N(OP_NE, "ne"); N(OP_NOP, "nop");
    N(OP_OR, "or"); N(OP_RET, "ret"); N(OP_ROUND_NE, "round_ne"); N(OP_ROUND_NI, "round_ni"); N(OP_ROUND_PI, "round_pi");
    N(OP_ROUND_Z, "round_z"); N(OP_RSQ, "rsq"); N(OP_SAMPLE, "sample"); N(OP_SAMPLE_C, "sample_c"); N(OP_SAMPLE_C_LZ, "sample_c_lz");
    N(OP_SAMPLE_L, "sample_l"); N(OP_SQRT, "sqrt"); N(OP_SINCOS, "sincos"); N(OP_DCL_FIRST, "dcl"); N(OP_DCL_TEMPS, "dcl_temps");
    auto regOf = [](const Sm4Operand& o) {
        std::string s = o.type == 0 ? "r" : o.type == 1 ? "v" : o.type == 2 ? "o" : o.type == 4 ? "imm" : o.type == 8 ? "cb" : "t";
        s += std::to_string(o.type == 8 ? o.index1 * 16 + o.index0 : o.index0);
        return s;
    };
    std::string out;
    for (size_t i = 0; i < prog.code.size(); ++i) {
        const Sm4Instr& in = prog.code[i];
        out += std::to_string(i) + ") " + (in.op < 128 && names[in.op] ? names[in.op] : ("op" + std::to_string(in.op)));
        if (in.saturate) out += "_sat";
        for (const Sm4Operand& o : in.operands) {
            out += " " + regOf(o);
            if (o.selMode == 1) { const char cs[4] = {'x','y','z','w'}; out += "."; for (int c = 0; c < 4; ++c) out += cs[o.swz[c] & 3]; }
            else if (o.selMode == 0) { const char cs[4] = {'x','y','z','w'}; out += "."; for (int i = 0; i < 4; ++i) out += (o.mask & (1u << i)) ? cs[i] : '-'; }
            else if (o.selMode == 2) { const char cs[4] = {'x','y','z','w'}; out += "."; for (int c = 0; c < 4; ++c) out += cs[o.swz[c] & 3]; }
            if (o.type == 4) { float f[4]; for (int c = 0; c < 4; ++c) std::memcpy(&f[c], &o.imm[c], 4); out += "("; for (int c = 0; c < 4; ++c) { char buf[16]; std::snprintf(buf, 16, "%.3f", f[c]); out += buf; if (c < 3) out += ","; } out += ")"; }
            if (o.modifier) out += (o.modifier == 1 ? "-x" : o.modifier == 2 ? "|x|" : "-|x|");
        }
        if (in.jump >= 0) out += " -> " + std::to_string(in.jump);
        out += "\n";
    }
    return out;
}

} // namespace cardbridge::shader
