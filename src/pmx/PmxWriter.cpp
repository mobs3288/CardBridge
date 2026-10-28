#include "pmx/PmxWriter.h"

#include <cmath>
#include <cstring>

namespace cardbridge::pmx {

namespace {

struct Out {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
    void i32(int32_t v) { uint32_t u = static_cast<uint32_t>(v); for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(u >> (8 * i))); }
    void f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); i32(static_cast<int32_t>(u)); }
    void vec2(Vec2 v) { f32(v.x); f32(v.y); }
    void vec3(Vec3 v) { f32(v.x); f32(v.y); f32(v.z); }
    void vec4(Vec4 v) { f32(v.x); f32(v.y); f32(v.z); f32(v.w); }
    void text(const std::string& s) {
        Bytes t = Utf8ToUtf16Le(s);
        i32(static_cast<int32_t>(t.size()));
        b.insert(b.end(), t.begin(), t.end());
    }
    // Signed index of `size` bytes (-1 = none).
    void sidx(int v, int size) {
        if (size == 1) u8(static_cast<uint8_t>(static_cast<int8_t>(v)));
        else if (size == 2) u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
        else i32(v);
    }
    // Unsigned vertex index.
    void uidx(uint32_t v, int size) {
        if (size == 1) u8(static_cast<uint8_t>(v));
        else if (size == 2) u16(static_cast<uint16_t>(v));
        else i32(static_cast<int32_t>(v));
    }
};

int signedSize(size_t count) { return count <= 127 ? 1 : (count <= 32767 ? 2 : 4); }

} // namespace

Bytes Utf8ToUtf16Le(const std::string& s) {
    Bytes out;
    size_t i = 0;
    auto put = [&](uint32_t cu) { out.push_back(static_cast<uint8_t>(cu)); out.push_back(static_cast<uint8_t>(cu >> 8)); };
    while (i < s.size()) {
        uint32_t c = static_cast<uint8_t>(s[i]);
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xe0) == 0xc0) { c &= 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { c &= 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { c &= 0x07; extra = 3; }
        else { c = 0xfffd; }
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xc0) != 0x80) { c = 0xfffd; break; }
            c = (c << 6) | (static_cast<uint8_t>(s[i]) & 0x3f);
            ++i;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            put(0xd800 + (c >> 10));
            put(0xdc00 + (c & 0x3ff));
        } else {
            put(c);
        }
    }
    return out;
}

Bytes WritePmx(const Model& m) {
    if (m.indices.size() % 3 != 0) throw FormatError("PMX: index count is not a multiple of 3");
    uint64_t sum = 0;
    for (const auto& mat : m.materials) sum += mat.indexCount;
    if (sum != m.indices.size()) throw FormatError("PMX: material index counts do not add up to the index buffer");
    for (uint32_t i : m.indices)
        if (i >= m.vertices.size()) throw FormatError("PMX: an index points past the vertex list");
    for (const auto& v : m.vertices)
        for (int k = 0; k < 4; ++k)
            if (v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= m.bones.size())
                if (v.weight[k] > 0) throw FormatError("PMX: a vertex is bound to a bone that does not exist");
    for (const auto& mat : m.materials)
        if (mat.texture >= static_cast<int>(m.textures.size())) throw FormatError("PMX: material texture index out of range");
    for (const auto& b : m.bones)
        if (b.parent >= static_cast<int>(m.bones.size())) throw FormatError("PMX: bone parent out of range");

    int vSize = m.vertices.size() <= 255 ? 1 : (m.vertices.size() <= 65535 ? 2 : 4);
    int tSize = signedSize(m.textures.size());
    int mSize = signedSize(m.materials.size());
    int bSize = signedSize(m.bones.size());

    Out o;
    o.b.insert(o.b.end(), {'P', 'M', 'X', ' '});
    o.f32(2.0f);
    o.u8(8);
    o.u8(0);                                  // text encoding: UTF-16LE
    o.u8(0);                                  // additional UV count
    o.u8(static_cast<uint8_t>(vSize));
    o.u8(static_cast<uint8_t>(tSize));
    o.u8(static_cast<uint8_t>(mSize));
    o.u8(static_cast<uint8_t>(bSize));
    o.u8(1);                                  // morph index size (no morphs)
    o.u8(1);                                  // rigid body index size (none)
    o.text(m.name);
    o.text(m.nameEn.empty() ? m.name : m.nameEn);
    o.text(m.comment);
    o.text(m.commentEn);

    o.i32(static_cast<int32_t>(m.vertices.size()));
    for (const auto& v : m.vertices) {
        o.vec3(v.position);
        o.vec3(v.normal);
        o.vec2(v.uv);
        int n = 0;
        for (int k = 0; k < 4; ++k) if (v.weight[k] > 0) ++n;
        if (n <= 1) {
            o.u8(0);
            o.sidx(v.bone[0], bSize);
        } else if (n == 2) {
            // BDEF2 takes the first two bones that carry weight.
            int idx[2], c = 0;
            float w[2] = {0, 0};
            for (int k = 0; k < 4 && c < 2; ++k)
                if (v.weight[k] > 0) { idx[c] = v.bone[k]; w[c] = v.weight[k]; ++c; }
            o.u8(1);
            o.sidx(idx[0], bSize);
            o.sidx(idx[1], bSize);
            o.f32(w[0] / (w[0] + w[1]));
        } else {
            float sum4 = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
            o.u8(2);
            for (int k = 0; k < 4; ++k) o.sidx(v.weight[k] > 0 ? v.bone[k] : -1, bSize);
            for (int k = 0; k < 4; ++k) o.f32(v.weight[k] / sum4);
        }
        o.f32(v.edgeScale);
    }

    o.i32(static_cast<int32_t>(m.indices.size()));
    for (uint32_t i : m.indices) o.uidx(i, vSize);

    o.i32(static_cast<int32_t>(m.textures.size()));
    for (const auto& t : m.textures) o.text(t);

    o.i32(static_cast<int32_t>(m.materials.size()));
    for (const auto& mat : m.materials) {
        o.text(mat.name);
        o.text(mat.nameEn.empty() ? mat.name : mat.nameEn);
        o.vec4(mat.diffuse);
        o.vec3(mat.specular);
        o.f32(mat.specularPower);
        o.vec3(mat.ambient);
        o.u8(mat.drawFlags);
        o.vec4(mat.edgeColor);
        o.f32(mat.edgeSize);
        o.sidx(mat.texture, tSize);
        o.sidx(-1, tSize);       // sphere texture
        o.u8(0);                 // sphere mode: none
        o.u8(1);                 // shared toon
        o.u8(0);                 // toon01
        o.text(mat.memo);
        o.i32(static_cast<int32_t>(mat.indexCount));
    }

    o.i32(static_cast<int32_t>(m.bones.size()));
    for (const auto& b : m.bones) {
        o.text(b.name);
        o.text(b.nameEn.empty() ? b.name : b.nameEn);
        o.vec3(b.position);
        o.sidx(b.parent, bSize);
        o.i32(b.layer);
        o.u16(b.flags);          // bit 0 clear: tail is an offset
        o.vec3(b.tailOffset);
    }

    o.i32(0);                    // morphs

    // Display frames: the two MMD requires (Root, Exp) plus one listing every bone.
    o.i32(3);
    o.text("Root"); o.text("Root"); o.u8(1);
    o.i32(m.bones.empty() ? 0 : 1);
    if (!m.bones.empty()) { o.u8(0); o.sidx(0, bSize); }
    o.text("\xE8\xA1\xA8\xE6\x83\x85"); o.text("Exp"); o.u8(1);   // 表情
    o.i32(0);
    o.text("Bones"); o.text("Bones"); o.u8(0);
    o.i32(static_cast<int32_t>(m.bones.size()));
    for (size_t i = 0; i < m.bones.size(); ++i) { o.u8(0); o.sidx(static_cast<int>(i), bSize); }

    o.i32(0);                    // rigid bodies
    o.i32(0);                    // joints
    return o.b;
}

} // namespace cardbridge::pmx
