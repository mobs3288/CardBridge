#pragma once

// An independent PMX 2.0 / 2.1 reader for the tests. It shares nothing with pmx/PmxWriter.cpp (written from the format description, not from
// the writer), so a field the writer puts in the wrong place shows up as a different value here. Parses every section: vertices, faces,
// textures, materials, bones (append, fixed axis, local axis, IK), morphs, display frames, rigid bodies and joints.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pmxread {

struct V3 { float x = 0, y = 0, z = 0; };

struct Vertex { V3 pos, normal; int weightType = 0; int bone[4] = {-1, -1, -1, -1}; float weight[4] = {0, 0, 0, 0}; };

struct Material { std::string name; int texture = -1; uint32_t indexCount = 0; uint8_t drawFlags = 0; };

struct IkLink { int bone = -1; bool limited = false; V3 lower, upper; };

struct Bone {
    std::string name, nameEn;
    V3 pos;
    int parent = -1, layer = 0;
    uint16_t flags = 0;
    int tailBone = -1;
    V3 tailOffset;
    int appendParent = -1;
    float appendRate = 0.0f;
    int ikTarget = -1, ikLoop = 0;
    float ikLimit = 0.0f;
    std::vector<IkLink> ikLinks;
    bool isIk() const { return (flags & 0x0020) != 0; }
};

struct Morph {
    std::string name;
    int panel = 0, type = 0;
    size_t count = 0;
    std::vector<std::pair<int, V3>> vertices;   // type 1
    std::vector<int> refs;                      // group / flip: morph indices; material: material indices; bone: bone indices
};

struct Frame { std::string name; bool special = false; std::vector<std::pair<int, int>> items; };   // (0 bone / 1 morph, index)

struct RigidBody {
    std::string name;
    int bone = -1;
    int group = 0;
    uint16_t mask = 0;
    int shape = 0;
    V3 size, pos, rot;
    float mass = 0, moveDamping = 0, rotDamping = 0, restitution = 0, friction = 0;
    int mode = 0;
};

struct Joint {
    std::string name;
    int type = 0, a = -1, b = -1;
    V3 pos, rot, moveLower, moveUpper, rotLower, rotUpper, springMove, springRot;
};

struct Model {
    float version = 0;
    int addUv = 0;
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> textures;
    std::vector<Material> materials;
    std::vector<Bone> bones;
    std::vector<Morph> morphs;
    std::vector<Frame> frames;
    std::vector<RigidBody> bodies;
    std::vector<Joint> joints;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : d_(data), n_(size) {}

    template <class T> T raw() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, d_ + p_, sizeof(T));
        p_ += sizeof(T);
        return v;
    }
    uint8_t u8() { return raw<uint8_t>(); }
    uint16_t u16() { return raw<uint16_t>(); }
    int32_t i32() { return raw<int32_t>(); }
    float f32() { return raw<float>(); }
    V3 v3() { V3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    void skip(size_t k) { need(k); p_ += k; }
    size_t pos() const { return p_; }
    bool atEnd() const { return p_ == n_; }

    // A signed index of `size` bytes.
    int sidx(int size) {
        if (size == 1) return static_cast<int8_t>(u8());
        if (size == 2) return static_cast<int16_t>(u16());
        return i32();
    }
    // An unsigned index (vertices): -1 is never stored.
    uint32_t uidx(int size) {
        if (size == 1) return u8();
        if (size == 2) return u16();
        return static_cast<uint32_t>(i32());
    }
    // A length-prefixed string, as UTF-8.
    std::string text(int encoding) {
        const int32_t len = i32();
        if (len < 0) throw std::runtime_error("negative text length");
        need(static_cast<size_t>(len));
        std::string out;
        if (encoding == 1) {
            out.assign(reinterpret_cast<const char*>(d_ + p_), static_cast<size_t>(len));
        } else {
            for (int32_t i = 0; i + 1 < len; i += 2) {
                uint32_t cu = d_[p_ + i] | (d_[p_ + i + 1] << 8);
                if (cu >= 0xD800 && cu < 0xDC00 && i + 3 < len) {
                    const uint32_t lo = d_[p_ + i + 2] | (d_[p_ + i + 3] << 8);
                    cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
                if (cu < 0x80) out += static_cast<char>(cu);
                else if (cu < 0x800) { out += static_cast<char>(0xC0 | (cu >> 6)); out += static_cast<char>(0x80 | (cu & 0x3F)); }
                else if (cu < 0x10000) { out += static_cast<char>(0xE0 | (cu >> 12)); out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cu & 0x3F)); }
                else { out += static_cast<char>(0xF0 | (cu >> 18)); out += static_cast<char>(0x80 | ((cu >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cu & 0x3F)); }
            }
        }
        p_ += static_cast<size_t>(len);
        return out;
    }

private:
    void need(size_t k) const {
        if (p_ + k > n_) throw std::runtime_error("PMX ends in the middle of a field at byte " + std::to_string(p_));
    }
    const uint8_t* d_;
    size_t n_, p_ = 0;
};

// Parses a whole file; throws std::runtime_error on a malformed or truncated one. Also fails when bytes are left over after the joints
// (PMX 2.1 soft bodies are not read, a 2.0 file ends there).
inline Model Read(const uint8_t* data, size_t size) {
    Reader r(data, size);
    Model m;
    if (size < 4 || std::memcmp(data, "PMX ", 4) != 0) throw std::runtime_error("not a PMX file");
    r.skip(4);
    m.version = r.f32();
    if (r.u8() != 8) throw std::runtime_error("unexpected globals count");
    const int enc = r.u8();
    m.addUv = r.u8();
    const int vS = r.u8(), tS = r.u8(), matS = r.u8(), bS = r.u8(), moS = r.u8(), rbS = r.u8();
    m.name = r.text(enc);
    r.text(enc); r.text(enc); r.text(enc);

    const int32_t nv = r.i32();
    for (int32_t i = 0; i < nv; ++i) {
        Vertex v;
        v.pos = r.v3(); v.normal = r.v3();
        r.skip(8);                                    // uv
        r.skip(static_cast<size_t>(m.addUv) * 16);
        v.weightType = r.u8();
        switch (v.weightType) {
            case 0: v.bone[0] = r.sidx(bS); v.weight[0] = 1.0f; break;
            case 1: v.bone[0] = r.sidx(bS); v.bone[1] = r.sidx(bS); v.weight[0] = r.f32(); v.weight[1] = 1.0f - v.weight[0]; break;
            case 2: case 4:
                for (int k = 0; k < 4; ++k) v.bone[k] = r.sidx(bS);
                for (int k = 0; k < 4; ++k) v.weight[k] = r.f32();
                break;
            case 3: v.bone[0] = r.sidx(bS); v.bone[1] = r.sidx(bS); v.weight[0] = r.f32(); v.weight[1] = 1.0f - v.weight[0]; r.skip(36); break;
            default: throw std::runtime_error("unknown vertex weight type");
        }
        r.f32();                                      // edge scale
        m.vertices.push_back(v);
    }
    const int32_t ni = r.i32();
    for (int32_t i = 0; i < ni; ++i) m.indices.push_back(r.uidx(vS));
    const int32_t nt = r.i32();
    for (int32_t i = 0; i < nt; ++i) m.textures.push_back(r.text(enc));
    const int32_t nm = r.i32();
    for (int32_t i = 0; i < nm; ++i) {
        Material mat;
        mat.name = r.text(enc); r.text(enc);
        r.skip(16 + 12 + 4 + 12);                     // diffuse, specular, power, ambient
        mat.drawFlags = r.u8();
        r.skip(16 + 4);                               // edge colour, size
        mat.texture = r.sidx(tS);
        r.sidx(tS);                                   // sphere
        r.u8();                                       // sphere mode
        if (r.u8() == 0) r.sidx(tS); else r.u8();     // toon
        r.text(enc);                                  // memo
        mat.indexCount = static_cast<uint32_t>(r.i32());
        m.materials.push_back(mat);
    }
    const int32_t nb = r.i32();
    for (int32_t i = 0; i < nb; ++i) {
        Bone b;
        b.name = r.text(enc); b.nameEn = r.text(enc);
        b.pos = r.v3();
        b.parent = r.sidx(bS);
        b.layer = r.i32();
        b.flags = r.u16();
        if (b.flags & 0x0001) b.tailBone = r.sidx(bS); else b.tailOffset = r.v3();
        if (b.flags & 0x0300) { b.appendParent = r.sidx(bS); b.appendRate = r.f32(); }
        if (b.flags & 0x0400) r.skip(12);
        if (b.flags & 0x0800) r.skip(24);
        if (b.flags & 0x2000) r.skip(4);
        if (b.flags & 0x0020) {
            b.ikTarget = r.sidx(bS);
            b.ikLoop = r.i32();
            b.ikLimit = r.f32();
            const int32_t links = r.i32();
            for (int32_t k = 0; k < links; ++k) {
                IkLink l;
                l.bone = r.sidx(bS);
                l.limited = r.u8() != 0;
                if (l.limited) { l.lower = r.v3(); l.upper = r.v3(); }
                b.ikLinks.push_back(l);
            }
        }
        m.bones.push_back(b);
    }
    const int32_t nmo = r.i32();
    for (int32_t i = 0; i < nmo; ++i) {
        Morph mo;
        mo.name = r.text(enc); r.text(enc);
        mo.panel = r.u8();
        mo.type = r.u8();
        const int32_t cnt = r.i32();
        mo.count = static_cast<size_t>(cnt);
        for (int32_t k = 0; k < cnt; ++k) {
            switch (mo.type) {
                case 0: case 9: mo.refs.push_back(r.sidx(moS)); r.f32(); break;
                case 1: { const int vi = static_cast<int>(r.uidx(vS)); mo.vertices.emplace_back(vi, r.v3()); break; }
                case 2: mo.refs.push_back(r.sidx(bS)); r.skip(28); break;
                case 3: case 4: case 5: case 6: case 7: r.uidx(vS); r.skip(16); break;
                case 8: mo.refs.push_back(r.sidx(matS)); r.skip(1 + 16 + 12 + 4 + 12 + 16 + 4 + 48); break;
                case 10: mo.refs.push_back(r.sidx(rbS)); r.skip(1 + 24); break;
                default: throw std::runtime_error("unknown morph type");
            }
        }
        m.morphs.push_back(std::move(mo));
    }
    const int32_t nf = r.i32();
    for (int32_t i = 0; i < nf; ++i) {
        Frame f;
        f.name = r.text(enc); r.text(enc);
        f.special = r.u8() != 0;
        const int32_t cnt = r.i32();
        for (int32_t k = 0; k < cnt; ++k) {
            const int kind = r.u8();
            f.items.emplace_back(kind, r.sidx(kind == 0 ? bS : moS));
        }
        m.frames.push_back(std::move(f));
    }
    const int32_t nr = r.i32();
    for (int32_t i = 0; i < nr; ++i) {
        RigidBody b;
        b.name = r.text(enc); r.text(enc);
        b.bone = r.sidx(bS);
        b.group = r.u8();
        b.mask = r.u16();
        b.shape = r.u8();
        b.size = r.v3(); b.pos = r.v3(); b.rot = r.v3();
        b.mass = r.f32(); b.moveDamping = r.f32(); b.rotDamping = r.f32(); b.restitution = r.f32(); b.friction = r.f32();
        b.mode = r.u8();
        m.bodies.push_back(b);
    }
    const int32_t nj = r.i32();
    for (int32_t i = 0; i < nj; ++i) {
        Joint j;
        j.name = r.text(enc); r.text(enc);
        j.type = r.u8();
        j.a = r.sidx(rbS); j.b = r.sidx(rbS);
        j.pos = r.v3(); j.rot = r.v3();
        j.moveLower = r.v3(); j.moveUpper = r.v3(); j.rotLower = r.v3(); j.rotUpper = r.v3(); j.springMove = r.v3(); j.springRot = r.v3();
        m.joints.push_back(j);
    }
    if (m.version < 2.05f && !r.atEnd()) throw std::runtime_error("bytes left over after the joints");
    return m;
}

} // namespace pmxread
