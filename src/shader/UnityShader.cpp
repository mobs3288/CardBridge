#include "shader/UnityShader.h"

#include <algorithm>
#include <cstring>

namespace cardbridge::shader {

namespace {

uint32_t U32(const Bytes& b, size_t p) {
    uint32_t v = 0;
    if (p + 4 <= b.size()) std::memcpy(&v, b.data() + p, 4);
    return v;
}

// A token of the binding tail: either a string or a plain integer.
struct Tok {
    bool isStr = false;
    std::string s;
    uint32_t v = 0;
};

// The tail is a stream of 4-byte words in which strings are a length word followed by the characters padded to 4.
// A word is read as a string only when it is a plausible length and the characters are identifier-like.
std::vector<Tok> Tokenize(const Bytes& sp, size_t from) {
    std::vector<Tok> out;
    size_t p = (from + 3) & ~size_t(3);
    while (p + 4 <= sp.size()) {
        uint32_t w = U32(sp, p);
        bool str = false;
        if (w >= 1 && w <= 64 && p + 4 + w <= sp.size()) {
            str = true;
            for (uint32_t i = 0; i < w; ++i) {
                unsigned char c = sp[p + 4 + i];
                if (!(std::isalnum(c) || c == '_' || c == '$')) { str = false; break; }
            }
        }
        Tok t;
        if (str) {
            t.isStr = true;
            t.s.assign(reinterpret_cast<const char*>(sp.data() + p + 4), w);
            p += 4 + ((w + 3) & ~3u);
        } else {
            t.v = w;
            p += 4;
        }
        out.push_back(std::move(t));
    }
    return out;
}

void ParseTail(const std::vector<Tok>& t, UnityProgram& prog) {
    size_t i = 0;
    // Constant buffers: name, size, parameter count, then per parameter name + 6 words
    // (0, rows, cols, isMatrix, array size, byte offset). They run until a string is followed by 0 (a texture) or 1 (a binding).
    auto isStr = [&](size_t k) { return k < t.size() && t[k].isStr; };
    auto num = [&](size_t k) { return k < t.size() && !t[k].isStr ? t[k].v : 0u; };
    while (i < t.size() && !isStr(i)) ++i;
    while (isStr(i) && i + 2 < t.size() && !t[i + 1].isStr && !t[i + 2].isStr && num(i + 2) < 64 && i + 3 < t.size()) {
        // A constant-buffer record has size > 0 and a small parameter count; texture records are name,0,reg,packed.
        uint32_t size = num(i + 1), count = num(i + 2);
        if (size == 0 || size > 65536) break;
        // Check the first parameter really follows (string then 0,rows,cols).
        if (count > 0 && !(isStr(i + 3) && !t[i + 4].isStr)) break;
        CbLayout cb;
        cb.name = t[i].s;
        cb.size = size;
        i += 3;
        for (uint32_t k = 0; k < count && isStr(i); ++k) {
            CbParam p;
            p.name = t[i].s;
            p.rows = num(i + 2);
            p.cols = num(i + 3);
            p.isMatrix = num(i + 4) != 0;
            p.offset = num(i + 6);
            cb.params.push_back(std::move(p));
            i += 7;
        }
        prog.cbuffers.push_back(std::move(cb));
    }
    // Then a count (ignored), the texture records name,0,register,packed(dimension | sampler << 8), and the
    // constant-buffer bindings name,1,slot,0.
    for (; i < t.size(); ++i) {
        if (!isStr(i)) continue;
        if (i + 3 < t.size() && !t[i + 1].isStr && !t[i + 2].isStr && !t[i + 3].isStr) {
            uint32_t a = t[i + 1].v, b = t[i + 2].v, c = t[i + 3].v;
            if (a == 0) {
                TextureBinding tb;
                tb.name = t[i].s;
                tb.reg = b;
                tb.dimension = c & 0xFF;
                tb.sampler = (c >> 8) & 0xFF;
                prog.textures.push_back(std::move(tb));
                i += 3;
            } else if (a == 1 && c == 0) {
                for (auto& cb : prog.cbuffers)
                    if (cb.name == t[i].s) cb.slot = static_cast<int>(b);
                i += 3;
            }
        }
    }
}

// Keywords sit in the header before the code: version, type, four statistics, then a count and the strings.
void ParseHeader(const Bytes& sp, size_t dxbcAt, UnityProgram& prog) {
    size_t p = 6 * 4;
    if (p + 4 > dxbcAt) return;
    uint32_t count = U32(sp, p);
    p += 4;
    for (uint32_t k = 0; k < count && p + 4 <= dxbcAt; ++k) {
        uint32_t n = U32(sp, p);
        if (n > 128 || p + 4 + n > dxbcAt) return;
        prog.keywords.emplace_back(reinterpret_cast<const char*>(sp.data() + p + 4), n);
        p += 4 + ((n + 3) & ~3u);
    }
}

} // namespace

std::vector<UnityProgram> ParseUnityShaderBlob(const Bytes& blob, std::vector<std::string>* why) {
    std::vector<UnityProgram> out;
    uint32_t n = U32(blob, 0);
    if (n == 0 || n > 4096 || 4 + size_t(n) * 8 > blob.size()) {
        if (why) why->push_back("not a Unity shader program blob");
        return out;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t off = U32(blob, 4 + i * 8), len = U32(blob, 8 + i * 8);
        if (size_t(off) + len > blob.size()) { if (why) why->push_back("program " + std::to_string(i) + " is out of range"); continue; }
        Bytes sp(blob.begin() + off, blob.begin() + off + len);
        size_t at = 0;
        while (at + 4 <= sp.size() && std::memcmp(sp.data() + at, "DXBC", 4) != 0) ++at;
        UnityProgram prog;
        std::string err;
        if (at + 32 > sp.size() || !ParseDxbc(sp.data() + at, sp.size() - at, prog.code, &err)) {
            if (why) why->push_back("program " + std::to_string(i) + ": " + (err.empty() ? "no DXBC" : err));
            continue;
        }
        size_t total = U32(sp, at + 24);
        ParseHeader(sp, at, prog);
        ParseTail(Tokenize(sp, at + total), prog);
        out.push_back(std::move(prog));
    }
    return out;
}

bool PickForwardPass(const std::vector<UnityProgram>& programs, size_t& pixelIndex, size_t& vertexIndex) {
    // The forward pass is the biggest pixel program; of equal-size variants take the one with the fewest inputs (no shadow map).
    size_t best = programs.size(), bestTokens = 0;
    for (size_t i = 0; i < programs.size(); ++i)
        if (programs[i].isPixel() && programs[i].code.tokens.size() > bestTokens) { best = i; bestTokens = programs[i].code.tokens.size(); }
    if (best == programs.size()) return false;
    for (size_t i = 0; i < programs.size(); ++i) {
        if (!programs[i].isPixel() || programs[i].code.tokens.size() * 100 < bestTokens * 90) continue;
        if (programs[i].code.inputs.size() < programs[best].code.inputs.size()) best = i;
    }
    pixelIndex = best;
    // The vertex program whose outputs cover the pixel program's inputs (same semantics and registers).
    size_t bestV = programs.size(), bestScore = 0;
    for (size_t v = 0; v < programs.size(); ++v) {
        if (!programs[v].isVertex()) continue;
        size_t score = 0;
        for (const auto& in : programs[best].code.inputs)
            for (const auto& o : programs[v].code.outputs)
                if (in.semantic == o.semantic && in.index == o.index && (in.mask & ~o.mask) == 0 && in.semantic != "SV_IsFrontFace") ++score;
        if (score > bestScore) { bestScore = score; bestV = v; }
    }
    if (bestV == programs.size()) return false;
    vertexIndex = bestV;
    return true;
}

} // namespace cardbridge::shader
