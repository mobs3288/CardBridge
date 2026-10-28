#include "shader/Dxbc.h"

#include <cstring>

namespace cardbridge::shader {

namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string CStr(const uint8_t* base, size_t size, uint32_t off) {
    if (off >= size) return {};
    size_t e = off;
    while (e < size && base[e]) ++e;
    return std::string(reinterpret_cast<const char*>(base + off), e - off);
}

// RDEF: constant buffers and resource bindings.
void ParseRdef(const uint8_t* c, size_t n, Dxbc& out) {
    if (n < 28) return;
    uint32_t cbCount = U32(c), cbOff = U32(c + 4), resCount = U32(c + 8), resOff = U32(c + 12);
    out.minor = c[16];
    out.major = c[17];
    out.programType = static_cast<uint32_t>(c[18] | (c[19] << 8)) == 0xFFFF ? 0u : 1u;   // 0xFFFF pixel, 0xFFFE vertex
    // The chunk's offsets are relative to its own start.
    for (uint32_t i = 0; i < resCount; ++i) {
        size_t p = resOff + i * 32u;
        if (p + 32 > n) break;
        ResourceBinding r;
        r.name = CStr(c, n, U32(c + p));
        r.type = U32(c + p + 4);
        r.dimension = U32(c + p + 12);
        r.bindPoint = U32(c + p + 20);
        out.resources.push_back(std::move(r));
    }
    for (uint32_t i = 0; i < cbCount; ++i) {
        size_t p = cbOff + i * 24u;
        if (p + 24 > n) break;
        ConstantBuffer cb;
        cb.name = CStr(c, n, U32(c + p));
        uint32_t varCount = U32(c + p + 4), varOff = U32(c + p + 8);
        cb.size = U32(c + p + 12);
        const uint32_t stride = out.major >= 5 ? 40u : 24u;
        for (uint32_t v = 0; v < varCount; ++v) {
            size_t q = varOff + v * stride;
            if (q + 24 > n) break;
            CbVariable var;
            var.name = CStr(c, n, U32(c + q));
            var.offset = U32(c + q + 4);
            var.size = U32(c + q + 8);
            cb.vars.push_back(std::move(var));
        }
        out.cbuffers.push_back(std::move(cb));
    }
}

// ISGN / OSGN (and the ISG1/OSG1 variants): the input / output signature.
void ParseSignature(const uint8_t* c, size_t n, std::vector<SignatureElement>& out, uint32_t stride) {
    if (n < 8) return;
    uint32_t count = U32(c);
    for (uint32_t i = 0; i < count; ++i) {
        size_t p = 8 + i * stride;
        if (p + 24 > n) break;
        SignatureElement e;
        e.semantic = CStr(c, n, U32(c + p + (stride == 32 ? 4 : 0)));
        size_t q = p + (stride == 32 ? 4 : 0);
        e.index = U32(c + q + 4);
        e.systemValue = U32(c + q + 8);
        e.reg = U32(c + q + 16);
        e.mask = c[q + 20];
        out.push_back(std::move(e));
    }
}

} // namespace

bool ParseDxbc(const uint8_t* data, size_t size, Dxbc& out, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (size < 32 || std::memcmp(data, "DXBC", 4) != 0) return fail("not a DXBC container");
    uint32_t total = U32(data + 24), chunks = U32(data + 28);
    if (total > size) return fail("container is truncated");
    bool haveCode = false;
    for (uint32_t i = 0; i < chunks; ++i) {
        size_t off = 32 + static_cast<size_t>(i) * 4;
        if (off + 4 > size) return fail("bad chunk table");
        uint32_t co = U32(data + off);
        if (co + 8 > size) return fail("bad chunk offset");
        const uint8_t* c = data + co + 8;
        size_t cn = U32(data + co + 4);
        if (co + 8 + cn > size) return fail("chunk overruns the container");
        char tag[5] = {static_cast<char>(data[co]), static_cast<char>(data[co + 1]), static_cast<char>(data[co + 2]), static_cast<char>(data[co + 3]), 0};
        std::string t(tag);
        if (t == "RDEF") ParseRdef(c, cn, out);
        else if (t == "ISGN") ParseSignature(c, cn, out.inputs, 24);
        else if (t == "OSGN") ParseSignature(c, cn, out.outputs, 24);
        else if (t == "ISG1") ParseSignature(c, cn, out.inputs, 32);
        else if (t == "OSG1" || t == "OSG5") ParseSignature(c, cn, out.outputs, 32);
        else if (t == "SHDR" || t == "SHEX") {
            out.tokens.resize(cn / 4);
            std::memcpy(out.tokens.data(), c, out.tokens.size() * 4);
            haveCode = true;
        }
    }
    if (!haveCode) return fail("no SHDR/SHEX chunk");
    uint32_t ver = out.tokens.empty() ? 0 : out.tokens[0];
    out.programType = ver >> 16;                  // 0 pixel, 1 vertex, 2 geometry
    out.major = (ver >> 4) & 0xF;
    out.minor = ver & 0xF;
    return true;
}

std::vector<Bytes> CarveDxbc(const Bytes& blob) {
    std::vector<Bytes> out;
    for (size_t i = 0; i + 32 <= blob.size();) {
        if (std::memcmp(blob.data() + i, "DXBC", 4) == 0) {
            uint32_t total = U32(blob.data() + i + 24);
            if (total >= 32 && i + total <= blob.size()) {
                out.emplace_back(blob.begin() + static_cast<std::ptrdiff_t>(i), blob.begin() + static_cast<std::ptrdiff_t>(i + total));
                i += total;
                continue;
            }
        }
        ++i;
    }
    return out;
}

} // namespace cardbridge::shader
