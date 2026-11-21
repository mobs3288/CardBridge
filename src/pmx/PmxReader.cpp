#include "pmx/PmxReader.h"

#include "core/FileIo.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace cardbridge::pmx {
namespace {

void appendUtf8(std::string& out, uint32_t cu) {
    if (cu < 0x80) out += static_cast<char>(cu);
    else if (cu < 0x800) {
        out += static_cast<char>(0xC0 | (cu >> 6));
        out += static_cast<char>(0x80 | (cu & 0x3F));
    } else if (cu < 0x10000) {
        out += static_cast<char>(0xE0 | (cu >> 12));
        out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cu & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cu >> 18));
        out += static_cast<char>(0x80 | ((cu >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cu & 0x3F));
    }
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : d_(data), n_(size) {}

    size_t pos() const { return p_; }
    size_t remaining() const { return n_ - p_; }

    void need(size_t k) const {
        if (k > remaining()) fail("unexpected end of file");
    }
    [[noreturn]] void fail(const std::string& what) const {
        throw FormatError("PMX import: " + what + " at byte " + std::to_string(p_));
    }

    uint8_t u8() { need(1); return d_[p_++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, d_ + p_, 2); p_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, d_ + p_, 4); p_ += 4; return v; }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); return v; }
    Vec2 v2() { return {f32(), f32()}; }
    Vec3 v3() { return {f32(), f32(), f32()}; }
    Vec4 v4() { return {f32(), f32(), f32(), f32()}; }
    void skip(size_t k) { need(k); p_ += k; }

    int sidx(int size) {
        if (size == 1) return static_cast<int8_t>(u8());
        if (size == 2) return static_cast<int16_t>(u16());
        return i32();
    }
    uint32_t uidx(int size) {
        if (size == 1) return u8();
        if (size == 2) return u16();
        return u32();
    }

    // A length-prefixed text field. Encoding 0 is UTF-16LE, 1 is UTF-8 (kept
    // verbatim, including any bytes that are not valid UTF-8).
    std::string text(int encoding) {
        int32_t len = i32();
        if (len < 0) fail("negative text length");
        need(static_cast<size_t>(len));
        const uint8_t* s = d_ + p_;
        p_ += static_cast<size_t>(len);
        if (encoding == 1) return std::string(reinterpret_cast<const char*>(s), static_cast<size_t>(len));
        std::string out;
        for (int32_t i = 0; i + 1 < len;) {
            uint32_t cu = static_cast<uint32_t>(s[i]) | (static_cast<uint32_t>(s[i + 1]) << 8);
            i += 2;
            if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < len) {
                uint32_t lo = static_cast<uint32_t>(s[i]) | (static_cast<uint32_t>(s[i + 1]) << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cu = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            appendUtf8(out, cu);
        }
        return out;
    }

    int32_t count(const char* what, size_t minBytes) {
        int32_t c = i32();
        if (c < 0) fail(std::string("negative ") + what + " count");
        if (minBytes && static_cast<size_t>(c) > remaining() / minBytes)
            fail(std::string("implausible ") + what + " count");
        return c;
    }

    Bytes rest() {
        Bytes b(remaining());
        if (!b.empty()) std::memcpy(b.data(), d_ + p_, b.size());
        p_ = n_;
        return b;
    }

private:
    const uint8_t* d_;
    size_t n_, p_ = 0;
};

void checkSize(int size, const char* what) {
    if (size != 1 && size != 2 && size != 4)
        throw FormatError(std::string("PMX import: invalid ") + what + " index size");
}

} // namespace

Model ReadPmx(const uint8_t* data, size_t size) {
    if (!data || size < 4) throw FormatError("PMX import: file is too short");
    Reader r(data, size);
    if (std::memcmp(data, "PMX ", 4) != 0) throw FormatError("PMX import: bad magic (expected 'PMX ')");
    r.skip(4);

    Model m;
    m.format.preserve = true;
    m.format.version = r.f32();
    if (m.format.version < 2.0f - 1e-4f || m.format.version > 2.1f + 1e-4f)
        throw FormatError("PMX import: unsupported PMX version " + std::to_string(m.format.version));

    m.format.globalsCount = r.u8();
    if (m.format.globalsCount < 8) throw FormatError("PMX import: too few global parameters");
    for (int i = 0; i < 8; ++i) m.format.globals[i] = r.u8();
    m.format.extraGlobals.resize(m.format.globalsCount - 8);
    for (size_t i = 0; i < m.format.extraGlobals.size(); ++i) m.format.extraGlobals[i] = r.u8();

    const int enc = m.format.encoding();
    if (enc != 0 && enc != 1) throw FormatError("PMX import: unknown text encoding");
    const int addUv = m.format.additionalUv();
    if (addUv < 0 || addUv > 4) throw FormatError("PMX import: invalid additional UV count");
    const int vSize = m.format.vertexSize(), tSize = m.format.textureSize();
    const int mSize = m.format.materialSize(), bSize = m.format.boneSize();
    const int moSize = m.format.morphSize(), rbSize = m.format.rigidBodySize();
    checkSize(vSize, "vertex"); checkSize(tSize, "texture");
    checkSize(mSize, "material"); checkSize(bSize, "bone");
    checkSize(moSize, "morph"); checkSize(rbSize, "rigid body");

    m.name = r.text(enc);
    m.nameEn = r.text(enc);
    m.comment = r.text(enc);
    m.commentEn = r.text(enc);

    // Vertices.
    int32_t nv = r.count("vertex", 4 + 4 + 2 + static_cast<size_t>(addUv) * 16 + 1 + bSize + 4);
    m.vertices.reserve(static_cast<size_t>(nv));
    for (int32_t i = 0; i < nv; ++i) {
        Vertex v;
        v.position = r.v3();
        v.normal = r.v3();
        v.uv = r.v2();
        for (int k = 0; k < addUv; ++k) v.additionalUv[k] = r.v4();
        v.deformType = r.u8();
        switch (v.deformType) {
            case 0: // BDEF1
                v.bone[0] = r.sidx(bSize);
                v.weight[0] = 1.0f;
                break;
            case 1: // BDEF2
                v.bone[0] = r.sidx(bSize);
                v.bone[1] = r.sidx(bSize);
                v.weight[0] = r.f32();
                break;
            case 2: case 4: // BDEF4, QDEF
                for (int k = 0; k < 4; ++k) v.bone[k] = r.sidx(bSize);
                for (int k = 0; k < 4; ++k) v.weight[k] = r.f32();
                break;
            case 3: // SDEF
                v.bone[0] = r.sidx(bSize);
                v.bone[1] = r.sidx(bSize);
                v.weight[0] = r.f32();
                v.sdefC = r.v3();
                v.sdefR0 = r.v3();
                v.sdefR1 = r.v3();
                break;
            default:
                throw FormatError("PMX import: unknown vertex deform type " + std::to_string(v.deformType) +
                                  " on vertex " + std::to_string(i));
        }
        v.edgeScale = r.f32();
        m.vertices.push_back(v);
    }

    // Faces (indices are already ordered by material).
    int32_t ni = r.count("index", static_cast<size_t>(vSize));
    m.indices.reserve(static_cast<size_t>(ni));
    for (int32_t i = 0; i < ni; ++i) m.indices.push_back(r.uidx(vSize));
    if (ni % 3 != 0) throw FormatError("PMX import: face count is not a multiple of 3");

    // Textures.
    int32_t nt = r.count("texture", 4);
    for (int32_t i = 0; i < nt; ++i) m.textures.push_back(r.text(enc));

    // Materials.
    int32_t nm = r.count("material", 4 + 4 + 12 + 4 + 12 + 1 + 16 + 4 + 2 * tSize + 2 + 4 + 4);
    for (int32_t i = 0; i < nm; ++i) {
        Material mat;
        mat.name = r.text(enc);
        mat.nameEn = r.text(enc);
        mat.diffuse = r.v4();
        mat.specular = r.v3();
        mat.specularPower = r.f32();
        mat.ambient = r.v3();
        mat.drawFlags = r.u8();
        mat.edgeColor = r.v4();
        mat.edgeSize = r.f32();
        mat.texture = r.sidx(tSize);
        mat.sphere = r.sidx(tSize);
        mat.sphereMode = r.u8();
        mat.toonFlag = r.u8();
        if (mat.toonFlag == 0) mat.toon = r.sidx(tSize);
        else if (mat.toonFlag == 1) mat.sharedToon = r.u8();
        else throw FormatError("PMX import: unknown toon flag on material " + std::to_string(i));
        mat.memo = r.text(enc);
        mat.indexCount = static_cast<uint32_t>(r.i32());
        m.materials.push_back(std::move(mat));
    }

    // Bones.
    int32_t nb = r.count("bone", 8 + 12 + bSize + 4 + 2);
    for (int32_t i = 0; i < nb; ++i) {
        Bone b;
        b.name = r.text(enc);
        b.nameEn = r.text(enc);
        b.position = r.v3();
        b.parent = r.sidx(bSize);
        b.layer = r.i32();
        b.flags = r.u16();
        b.flagsFromSource = true;
        if (b.flags & 0x0001) b.tailBone = r.sidx(bSize);
        else b.tailOffset = r.v3();
        if (b.flags & 0x0300) { b.appendParent = r.sidx(bSize); b.appendRatio = r.f32(); }
        if (b.flags & 0x0400) b.fixedAxis = r.v3();
        if (b.flags & 0x0800) { b.localAxisX = r.v3(); b.localAxisZ = r.v3(); }
        if (b.flags & 0x2000) b.externalParentKey = r.i32();
        if (b.flags & 0x0020) {
            b.ikTarget = r.sidx(bSize);
            b.ikLoop = r.i32();
            b.ikLimit = r.f32();
            int32_t nl = r.count("IK link", static_cast<size_t>(bSize) + 1);
            for (int32_t k = 0; k < nl; ++k) {
                IkLink link;
                link.bone = r.sidx(bSize);
                link.limited = r.u8() != 0;
                if (link.limited) { link.lower = r.v3(); link.upper = r.v3(); }
                b.ikLinks.push_back(link);
            }
        }
        m.bones.push_back(std::move(b));
    }

    // Morphs — every type is read in full so unsupported kinds survive a round trip.
    int32_t nmo = r.count("morph", 8 + 2);
    for (int32_t i = 0; i < nmo; ++i) {
        Morph mo;
        mo.name = r.text(enc);
        mo.nameEn = r.text(enc);
        mo.panel = r.u8();
        mo.type = r.u8();
        int32_t no = r.count("morph offset", 4);
        for (int32_t k = 0; k < no; ++k) {
            switch (mo.type) {
                case 0: { // group
                    int index = r.sidx(moSize);
                    mo.group.emplace_back(index, r.f32());
                    break;
                }
                case 1: { // vertex
                    int index = static_cast<int>(r.uidx(vSize));
                    mo.vertices.emplace_back(index, r.v3());
                    break;
                }
                case 2: { // bone
                    BoneMorphOffset off;
                    off.bone = r.sidx(bSize);
                    off.move = r.v3();
                    off.rotate = r.v4();
                    mo.bones.push_back(off);
                    break;
                }
                case 3: case 4: case 5: case 6: case 7: { // UV / extra UV
                    int index = static_cast<int>(r.uidx(vSize));
                    mo.uv.emplace_back(index, r.v4());
                    break;
                }
                case 8: { // material
                    MaterialOffset off;
                    off.material = r.sidx(mSize);
                    off.add = r.u8() != 0;
                    off.diffuse = r.v4();
                    off.specular = r.v3();
                    off.specularPower = r.f32();
                    off.ambient = r.v3();
                    off.edgeColor = r.v4();
                    off.edgeSize = r.f32();
                    off.texture = r.v4();
                    off.sphere = r.v4();
                    off.toon = r.v4();
                    mo.materials.push_back(off);
                    break;
                }
                case 9: { // flip
                    int index = r.sidx(moSize);
                    mo.flips.emplace_back(index, r.f32());
                    break;
                }
                case 10: { // impulse
                    ImpulseOffset off;
                    off.body = r.sidx(rbSize);
                    off.localFlag = r.u8();
                    off.velocity = r.v3();
                    off.torque = r.v3();
                    mo.impulses.push_back(off);
                    break;
                }
                default:
                    throw FormatError("PMX import: unknown morph type " + std::to_string(mo.type) +
                                      " on morph " + std::to_string(i));
            }
        }
        m.morphs.push_back(std::move(mo));
    }

    // Display frames, including the exact interleaving of bones and morphs.
    int32_t nf = r.count("display frame", 8 + 1 + 4);
    for (int32_t i = 0; i < nf; ++i) {
        DisplayFrame f;
        f.name = r.text(enc);
        f.nameEn = r.text(enc);
        f.special = r.u8() != 0;
        int32_t nfi = r.count("display frame item", 1);
        for (int32_t k = 0; k < nfi; ++k) {
            uint8_t kind = r.u8();
            if (kind > 1) throw FormatError("PMX import: unknown display frame item type");
            f.items.emplace_back(kind, r.sidx(kind == 0 ? bSize : moSize));
        }
        m.frames.push_back(std::move(f));
    }

    // Rigid bodies.
    int32_t nr = r.count("rigid body", 8 + bSize + 1 + 2 + 1 + 36 + 20 + 1);
    for (int32_t i = 0; i < nr; ++i) {
        RigidBody rb;
        rb.name = r.text(enc);
        rb.nameEn = r.text(enc);
        rb.bone = r.sidx(bSize);
        rb.group = r.u8();
        rb.mask = r.u16();
        rb.shape = r.u8();
        rb.size = r.v3();
        rb.position = r.v3();
        rb.rotation = r.v3();
        rb.mass = r.f32();
        rb.moveDamping = r.f32();
        rb.rotDamping = r.f32();
        rb.restitution = r.f32();
        rb.friction = r.f32();
        rb.mode = r.u8();
        m.rigidBodies.push_back(std::move(rb));
    }

    // Joints.
    int32_t nj = r.count("joint", 8 + 1 + 2 * rbSize + 6 * 12);
    for (int32_t i = 0; i < nj; ++i) {
        Joint j;
        j.name = r.text(enc);
        j.nameEn = r.text(enc);
        j.type = r.u8();
        if (j.type > 1) throw FormatError("PMX import: unknown joint type " + std::to_string(j.type));
        j.bodyA = r.sidx(rbSize);
        j.bodyB = r.sidx(rbSize);
        j.position = r.v3();
        j.rotation = r.v3();
        j.moveLower = r.v3();
        j.moveUpper = r.v3();
        j.rotLower = r.v3();
        j.rotUpper = r.v3();
        if (j.type == 0) {
            j.springMove = r.v3();
            j.springRot = r.v3();
        }
        m.joints.push_back(std::move(j));
    }

    m.format.trailing = r.rest();  // PMX 2.1 soft bodies and any later sections
    return m;
}

Model ReadPmxFile(const std::string& path) {
    Bytes bytes = fileio::ReadFile(path);
    return ReadPmx(bytes.data(), bytes.size());
}

} // namespace cardbridge::pmx
