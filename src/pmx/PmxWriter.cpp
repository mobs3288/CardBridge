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

void TurnAboutY(Model& m) {
    auto turn = [](Vec3& v) { v.x = -v.x; v.z = -v.z; };
    auto yawPi = [](float y) { return y > 0.0f ? y - 3.14159265f : y + 3.14159265f; };   // + half a turn, kept in (-pi, pi]
    for (auto& v : m.vertices) { turn(v.position); turn(v.normal); }
    for (auto& b : m.bones) {
        turn(b.position);
        turn(b.tailOffset);
        for (auto& l : b.ikLinks) {
            if (!l.limited) continue;
            const float xl = l.lower.x, zl = l.lower.z;
            l.lower.x = -l.upper.x; l.upper.x = -xl;
            l.lower.z = -l.upper.z; l.upper.z = -zl;
        }
    }
    for (auto& mo : m.morphs) for (auto& vo : mo.vertices) turn(vo.second);
    // A body's or joint's rotation is its frame in the model: turned with everything, the yaw (the outer rotation of PMX's Y-X-Z Euler order)
    // gains half a turn and the frame's own axes, the springs and limits in them, stay as they were.
    for (auto& rb : m.rigidBodies) { turn(rb.position); rb.rotation.y = yawPi(rb.rotation.y); }
    for (auto& j : m.joints) { turn(j.position); j.rotation.y = yawPi(j.rotation.y); }
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
    for (const auto& b : m.bones) {
        if (b.parent >= static_cast<int>(m.bones.size())) throw FormatError("PMX: bone parent out of range");
        if (b.tailBone >= static_cast<int>(m.bones.size())) throw FormatError("PMX: bone tail out of range");
        if (b.ikTarget >= static_cast<int>(m.bones.size())) throw FormatError("PMX: IK target out of range");
        if (b.ikTarget >= 0 && b.ikLinks.empty()) throw FormatError("PMX: an IK bone without links");
        for (const auto& l : b.ikLinks)
            if (l.bone < 0 || l.bone >= static_cast<int>(m.bones.size())) throw FormatError("PMX: IK link out of range");
    }
    for (const auto& rb : m.rigidBodies)
        if (rb.bone >= static_cast<int>(m.bones.size())) throw FormatError("PMX: a rigid body points past the bone list");
    for (const auto& j : m.joints)
        if (j.bodyA >= static_cast<int>(m.rigidBodies.size()) || j.bodyB >= static_cast<int>(m.rigidBodies.size())) throw FormatError("PMX: a joint points past the rigid body list");
    for (const auto& mo : m.morphs)
        for (const auto& vo : mo.vertices)
            if (vo.first < 0 || static_cast<size_t>(vo.first) >= m.vertices.size()) throw FormatError("PMX: a vertex morph points past the vertex list");

    int vSize = m.vertices.size() <= 255 ? 1 : (m.vertices.size() <= 65535 ? 2 : 4);
    int tSize = signedSize(m.textures.size());
    int mSize = signedSize(m.materials.size());
    int bSize = signedSize(m.bones.size());
    int morphSize = signedSize(m.morphs.size());
    int rbSize = signedSize(m.rigidBodies.size());
    for (const auto& mo : m.morphs) {
        if ((!mo.materials.empty()) + (!mo.group.empty()) + (!mo.vertices.empty()) > 1) throw FormatError("PMX: a morph is of more than one kind");
        for (const auto& off : mo.materials)
            if (off.material >= static_cast<int>(m.materials.size())) throw FormatError("PMX: a material morph points past the material list");
        for (const auto& gr : mo.group)
            if (gr.first < 0 || gr.first >= static_cast<int>(m.morphs.size())) throw FormatError("PMX: a group morph points past the morph list");
    }

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
    o.u8(static_cast<uint8_t>(morphSize));
    o.u8(static_cast<uint8_t>(rbSize));
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
        o.sidx(mat.sphere, tSize);                         // sphere texture
        o.u8(mat.sphere >= 0 ? mat.sphereMode : 0);        // sphere mode: none, multiply or add
        if (mat.toon >= 0) { o.u8(0); o.sidx(mat.toon, tSize); }   // the material's own ramp
        else { o.u8(1); o.u8(0); }                                  // shared toon01
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
        uint16_t flags = static_cast<uint16_t>(b.tailBone >= 0 ? (b.flags | 0x0001) : (b.flags & ~0x0001));   // bit 0: the tail is a bone, else an offset
        flags = static_cast<uint16_t>(b.ikTarget >= 0 ? (flags | 0x0020) : (flags & ~0x0020));              // bit 5: IK
        flags = static_cast<uint16_t>(b.appendParent >= 0 ? (flags | 0x0100) : (flags & ~0x0300));   // bit 8: rotation follows the append parent
        o.u16(flags);
        if (b.tailBone >= 0) o.sidx(b.tailBone, bSize);
        else o.vec3(b.tailOffset);
        if (b.appendParent >= 0) { o.sidx(b.appendParent, bSize); o.f32(b.appendRatio); }
        if (b.ikTarget >= 0) {
            o.sidx(b.ikTarget, bSize);
            o.i32(b.ikLoop);
            o.f32(b.ikLimit);
            o.i32(static_cast<int32_t>(b.ikLinks.size()));
            for (const auto& l : b.ikLinks) {
                o.sidx(l.bone, bSize);
                o.u8(l.limited ? 1 : 0);
                if (l.limited) { o.vec3(l.lower); o.vec3(l.upper); }
            }
        }
    }

    o.i32(static_cast<int32_t>(m.morphs.size()));
    for (const auto& mo : m.morphs) {
        o.text(mo.name);
        o.text(mo.nameEn.empty() ? mo.name : mo.nameEn);
        o.u8(static_cast<uint8_t>(mo.panel));
        if (!mo.group.empty()) {
            o.u8(0);
            o.i32(static_cast<int32_t>(mo.group.size()));
            for (const auto& gr : mo.group) { o.sidx(gr.first, morphSize); o.f32(gr.second); }
        } else if (!mo.vertices.empty()) {
            o.u8(1);
            o.i32(static_cast<int32_t>(mo.vertices.size()));
            for (const auto& vo : mo.vertices) { o.uidx(static_cast<uint32_t>(vo.first), vSize); o.vec3(vo.second); }
        } else {
            o.u8(8);
            o.i32(static_cast<int32_t>(mo.materials.size()));
            for (const auto& off : mo.materials) {
                o.sidx(off.material, mSize);
                o.u8(off.add ? 1 : 0);
                o.vec4(off.diffuse);
                o.vec3(off.specular);
                o.f32(off.specularPower);
                o.vec3(off.ambient);
                o.vec4(off.edgeColor);
                o.f32(off.edgeSize);
                o.vec4(off.texture);
                o.vec4(off.sphere);
                o.vec4(off.toon);
            }
        }
    }

    // Display frames: the two MMD requires (Root, Exp) plus the caller's own, or by default one listing every bone.
    if (m.frames.empty()) {
        o.i32(3);
        o.text("Root"); o.text("Root"); o.u8(1);
        o.i32(m.bones.empty() ? 0 : 1);
        if (!m.bones.empty()) { o.u8(0); o.sidx(0, bSize); }
        o.text("\xE8\xA1\xA8\xE6\x83\x85"); o.text("Exp"); o.u8(1);   // 表情
        o.i32(static_cast<int32_t>(m.morphs.size()));
        for (size_t i = 0; i < m.morphs.size(); ++i) { o.u8(1); o.sidx(static_cast<int>(i), morphSize); }
        o.text("Bones"); o.text("Bones"); o.u8(0);
        o.i32(static_cast<int32_t>(m.bones.size()));
        for (size_t i = 0; i < m.bones.size(); ++i) { o.u8(0); o.sidx(static_cast<int>(i), bSize); }
    } else {
        o.i32(static_cast<int32_t>(m.frames.size()));
        for (const auto& f : m.frames) {
            o.text(f.name); o.text(f.nameEn.empty() ? f.name : f.nameEn); o.u8(f.special ? 1 : 0);
            o.i32(static_cast<int32_t>(f.bones.size() + f.morphs.size()));
            for (int bi : f.bones) { if (bi < 0 || bi >= static_cast<int>(m.bones.size())) throw FormatError("PMX: a display frame lists a missing bone"); o.u8(0); o.sidx(bi, bSize); }
            for (int mi : f.morphs) { if (mi < 0 || mi >= static_cast<int>(m.morphs.size())) throw FormatError("PMX: a display frame lists a missing morph"); o.u8(1); o.sidx(mi, morphSize); }
        }
    }

    o.i32(static_cast<int32_t>(m.rigidBodies.size()));
    for (const auto& rb : m.rigidBodies) {
        o.text(rb.name); o.text(rb.nameEn.empty() ? rb.name : rb.nameEn);
        o.sidx(rb.bone, bSize);
        o.u8(rb.group); o.u16(rb.mask); o.u8(rb.shape);
        o.vec3(rb.size); o.vec3(rb.position); o.vec3(rb.rotation);
        o.f32(rb.mass); o.f32(rb.moveDamping); o.f32(rb.rotDamping); o.f32(rb.restitution); o.f32(rb.friction);
        o.u8(rb.mode);
    }
    o.i32(static_cast<int32_t>(m.joints.size()));
    for (const auto& j : m.joints) {
        o.text(j.name); o.text(j.nameEn.empty() ? j.name : j.nameEn);
        o.u8(0);                                  // spring 6-DOF
        o.sidx(j.bodyA, rbSize); o.sidx(j.bodyB, rbSize);
        o.vec3(j.position); o.vec3(j.rotation);
        o.vec3(j.moveLower); o.vec3(j.moveUpper); o.vec3(j.rotLower); o.vec3(j.rotUpper);
        o.vec3(j.springMove); o.vec3(j.springRot);
    }
    return o.b;
}

} // namespace cardbridge::pmx
