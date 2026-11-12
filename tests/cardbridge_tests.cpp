// CardBridge tests. Unit tests need no game files. Integration tests (card ->
// PMX against real data) run only when the environment names the inputs:
//
//   CARDBRIDGE_TEST_LIBRARY  a CardBridge library folder
//   CARDBRIDGE_TEST_MODS     the mods folder
//   CARDBRIDGE_TEST_CARD     a character card .png
//   CARDBRIDGE_TEST_GOLDEN   tests/golden/<card>.json (counts and hashes from the reference run)
//   CARDBRIDGE_TEST_OUT      scratch folder for the exported result
//
// Without them the integration tests are skipped, not failed.

#include "assemble/Assembler.h"
#include "assemble/AutoCloth.h"
#include "assemble/DynamicBone.h"
#include "assemble/HeelConfig.h"
#include "assemble/Pose.h"
#include "assemble/PhysicsSim.h"
#include "card/Abmx.h"
#include "card/CardFile.h"
#include "card/MaterialEdits.h"
#include "core/FileIo.h"
#include "core/Inflate.h"
#include "core/Lz4.h"
#include "core/MsgPack.h"
#include "core/Sha256.h"
#include "core/ZipArchive.h"
#include "export/AdapterSelect.h"
#include "export/BoneLabels.h"
#include "export/Exporter.h"
#include "export/MmdNames.h"
#include "export/PmxIk.h"
#include "export/PmxPhysics.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"
#include "export/SkinShade.h"
#include "export/SkinAoBake.h"
#include "library/Library.h"
#include "material/MaterialWorkbench.h"
#include "material/UvDiagnostics.h"
#include "pmx/PmxWriter.h"
#include "unity/TextureDecode.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "shape/Shape.h"
#include "resolve/Resolver.h"

#include "PmxReader.h"
#include "SpringRef.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace cardbridge;

namespace cardbridge::exporter::detail {   // Preview.cpp: the linear-light helpers both renderers share
float ToLinear(float c);
float ToSrgb(float c);
assemble::SceneMaterial LinearColours(const assemble::SceneMaterial& mat);
}

namespace {

int g_failed = 0, g_run = 0, g_skipped = 0;

#define CHECK(cond) do { ++g_run; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void test(const char* name, const std::function<void()>& f) {
    std::printf("[ RUN  ] %s\n", name);
    int before = g_failed;
    try { f(); } catch (const std::exception& e) { ++g_failed; std::printf("  FAIL exception: %s\n", e.what()); }
    std::printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

std::string env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }

Bytes B(std::initializer_list<int> l) { Bytes b; for (int x : l) b.push_back(static_cast<uint8_t>(x)); return b; }

// A minimal valid PNG (1x1) so card-loader tests need no files.
Bytes tinyPng() {
    // 1x1 RGBA PNG generated with Python zlib, CRCs included
    return B({0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
              0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x02, 0x00,
              0x00, 0x05, 0x00, 0x01, 0x7a, 0x5e, 0xab, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82});
}

std::string tempPath(const std::string& name) {
    std::string base = env("CARDBRIDGE_TEST_OUT");
    if (base.empty()) base = env("TEMP");
    if (base.empty()) base = ".";
    for (char& c : base) if (c == '\\') c = '/';
    return base + "/cardbridge_" + name;
}

// A minimal .zip with stored (uncompressed) entries, written to tempPath for tests that need a loose-texture mod on disk.
// The reader never checks the CRC, so it stays zero. Returns the path, or "" when the file could not be written.
std::string writeStoredZip(const std::string& name, const std::vector<std::pair<std::string, Bytes>>& entries) {
    Bytes zip;
    auto u16 = [&](uint16_t v) { zip.push_back(static_cast<uint8_t>(v & 0xff)); zip.push_back(static_cast<uint8_t>((v >> 8) & 0xff)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) zip.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff)); };
    struct Local { std::string name; uint32_t offset; };
    std::vector<Local> locals;
    for (const auto& [nm, data] : entries) {
        locals.push_back({nm, static_cast<uint32_t>(zip.size())});
        u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0); u32(0);   // stored, no time
        u32(static_cast<uint32_t>(data.size())); u32(static_cast<uint32_t>(data.size()));
        u16(static_cast<uint16_t>(nm.size())); u16(0);
        zip.insert(zip.end(), nm.begin(), nm.end());
        zip.insert(zip.end(), data.begin(), data.end());
    }
    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [nm, data] = entries[i];
        u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0); u32(0);
        u32(static_cast<uint32_t>(data.size())); u32(static_cast<uint32_t>(data.size()));
        u16(static_cast<uint16_t>(nm.size())); u16(0); u16(0); u16(0); u16(0); u32(0); u32(locals[i].offset);
        zip.insert(zip.end(), nm.begin(), nm.end());
    }
    const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
    u32(0x06054b50); u16(0); u16(0); u16(static_cast<uint16_t>(entries.size())); u16(static_cast<uint16_t>(entries.size())); u32(cdSize); u32(cdOffset); u16(0);
    const std::string path = tempPath(name);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "";
    const size_t written = std::fwrite(zip.data(), 1, zip.size(), f);
    std::fclose(f);
    return written == zip.size() ? path : "";
}

// ---- PMX read-back (tests/PmxReader.h reads the written bytes, independent of the writer) ----

pmxread::V3 operator-(pmxread::V3 a, pmxread::V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float lengthOf(pmxread::V3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A capsule's long axis in the world: +Y turned by a rigid body's rotation in PMX order (roll about z, then pitch about x, then yaw about y).
pmxread::V3 capsuleAxis(pmxread::V3 e) {
    pmxread::V3 v{0, 1, 0};
    v = {v.x * std::cos(e.z) - v.y * std::sin(e.z), v.x * std::sin(e.z) + v.y * std::cos(e.z), v.z};
    v = {v.x, v.y * std::cos(e.x) - v.z * std::sin(e.x), v.y * std::sin(e.x) + v.z * std::cos(e.x)};
    v = {v.x * std::cos(e.y) + v.z * std::sin(e.y), v.y, -v.x * std::sin(e.y) + v.z * std::cos(e.y)};
    return v;
}

// Everything that can be wrong with the physics of a written PMX, each finding one line: bodies on missing bones or far from them,
// capsules not along their bone, joints on missing bodies, an anchor that is not kinematic, a chain body that is, collision filters that
// let a chain hit anything but the colliders, vertex morphs and display frames pointing past their lists, and chains with no anchor.
std::vector<std::string> pmxPhysicsFindings(const pmxread::Model& m) {
    std::vector<std::string> out;
    const int nb = static_cast<int>(m.bodies.size());
    auto bit = [](uint16_t mask, int group) { return (mask >> group & 1) != 0; };
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const auto& b = m.bodies[i];
        const std::string who = "body " + std::to_string(i) + " '" + b.name + "'";
        if (b.bone < 0 || b.bone >= static_cast<int>(m.bones.size())) { out.push_back(who + ": bone " + std::to_string(b.bone) + " does not exist"); continue; }
        if (b.shape < 0 || b.shape > 2) out.push_back(who + ": unknown shape");
        if (b.mode < 0 || b.mode > 2) out.push_back(who + ": unknown mode");
        if (b.mode != 0 && !(b.mass > 0)) out.push_back(who + ": a moving body without mass");
        if (b.mode != 0 && (b.moveDamping < 0 || b.moveDamping > 1 || b.rotDamping < 0 || b.rotDamping > 1)) out.push_back(who + ": damping outside 0..1");
        if (!(b.size.x > 0)) out.push_back(who + ": no radius");
        const pmxread::V3 head = m.bones[static_cast<size_t>(b.bone)].pos;
        const float reach = b.shape == 2 ? 0.5f * b.size.y + b.size.x : b.size.x;
        const float dist = lengthOf(b.pos - head);
        if (b.group == exporter::kPmxGroupChain && b.mode != 0 && dist > reach + 1e-3f) out.push_back(who + ": its centre is " + std::to_string(dist) + " from its bone, further than its own half length " + std::to_string(reach));
        if (b.group == exporter::kPmxGroupChain && b.mode != 0 && b.shape == 2) {
            // a chain link runs from its bone's head: the axis points from the head to the centre
            const pmxread::V3 axis = capsuleAxis(b.rot), want = b.pos - head;
            const float l = lengthOf(want);
            const float d = l > 1e-6f ? (axis.x * want.x + axis.y * want.y + axis.z * want.z) / l : 1.0f;
            if (d < 0.999f) out.push_back(who + ": the capsule does not follow its bone (cos " + std::to_string(d) + ")");
        }
        if (b.group != exporter::kPmxGroupChain) {
            if (b.mode != 0) out.push_back(who + ": a collider that moves");
            if (!bit(b.mask, exporter::kPmxGroupChain)) out.push_back(who + ": a collider that does not meet chains");
            if ((b.mask & ~(1u << exporter::kPmxGroupChain)) != 0) out.push_back(who + ": a collider that meets more than chains");
        } else if (b.mode != 0) {
            if (bit(b.mask, exporter::kPmxGroupChain)) out.push_back(who + ": a chain body that meets chains (mask " + std::to_string(b.mask) + ")");
            // it meets only colliders it does not already overlap: a body that starts inside one is thrown out of it, and the model with it
            for (size_t k = 0; k < m.bodies.size(); ++k) {
                const auto& c = m.bodies[k];
                if (c.group == exporter::kPmxGroupChain || !bit(b.mask, c.group)) continue;
                auto core = [](const pmxread::RigidBody& x, Vec3& p, Vec3& q) {
                    const pmxread::V3 ax = capsuleAxis(x.rot);
                    const float h = x.shape == 2 ? 0.5f * x.size.y : 0.0f;
                    p = {x.pos.x - ax.x * h, x.pos.y - ax.y * h, x.pos.z - ax.z * h};
                    q = {x.pos.x + ax.x * h, x.pos.y + ax.y * h, x.pos.z + ax.z * h};
                };
                Vec3 a0, a1, c0, c1;
                core(b, a0, a1);
                core(c, c0, c1);
                if (exporter::SegmentDistance(a0, a1, c0, c1) < b.size.x + c.size.x) out.push_back(who + ": it may meet collider " + std::to_string(k) + " '" + c.name + "' but starts inside it");
            }
        } else if (b.mask != 0) {
            out.push_back(who + ": an anchor that collides");
        }
    }
    std::vector<std::vector<int>> linked(m.bodies.size());
    for (size_t i = 0; i < m.joints.size(); ++i) {
        const auto& j = m.joints[i];
        if (j.a < 0 || j.a >= nb || j.b < 0 || j.b >= nb || j.a == j.b) { out.push_back("joint " + std::to_string(i) + ": does not join two bodies"); continue; }
        linked[static_cast<size_t>(j.a)].push_back(j.b);
        linked[static_cast<size_t>(j.b)].push_back(j.a);
        if (j.type != 0) out.push_back("joint " + std::to_string(i) + ": not a spring 6-DOF joint");
        for (float l : {j.rotLower.x, j.rotLower.y, j.rotLower.z}) if (!(l <= 0)) out.push_back("joint " + std::to_string(i) + ": a lower rotation limit above 0");
        for (float u : {j.rotUpper.x, j.rotUpper.y, j.rotUpper.z}) if (!(u >= 0)) out.push_back("joint " + std::to_string(i) + ": an upper rotation limit below 0");
        if (!(j.springRot.x > 0 && j.springRot.y > 0 && j.springRot.z > 0)) out.push_back("joint " + std::to_string(i) + ": no rotation spring");
        // the joint stands where the second body meets the first: on or near the second body
        const auto& bb = m.bodies[static_cast<size_t>(j.b)];
        const float reach = bb.shape == 2 ? 0.5f * bb.size.y + bb.size.x : bb.size.x;
        if (lengthOf(j.pos - bb.pos) > reach + 1e-3f) out.push_back("joint " + std::to_string(i) + ": far from its second body");
    }
    // every moving body is tied to a kinematic one through joints
    std::vector<char> reached(m.bodies.size(), 0);
    std::vector<int> stack;
    for (int i = 0; i < nb; ++i) if (m.bodies[static_cast<size_t>(i)].mode == 0 && m.bodies[static_cast<size_t>(i)].group == exporter::kPmxGroupChain) { reached[static_cast<size_t>(i)] = 1; stack.push_back(i); }
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        for (int n : linked[static_cast<size_t>(at)]) if (!reached[static_cast<size_t>(n)]) { reached[static_cast<size_t>(n)] = 1; stack.push_back(n); }
    }
    for (int i = 0; i < nb; ++i)
        if (m.bodies[static_cast<size_t>(i)].mode != 0 && !reached[static_cast<size_t>(i)]) out.push_back("body " + std::to_string(i) + " is not joined to an anchor");
    for (size_t i = 0; i < m.bones.size(); ++i) {   // IK: the target and links exist, the links are the target's ancestors nearest first, the handle stands on the target
        const auto& b = m.bones[i];
        if (!b.isIk()) continue;
        const int n = static_cast<int>(m.bones.size());
        const std::string who = "IK bone " + std::to_string(i) + " '" + b.name + "'";
        if (b.ikTarget < 0 || b.ikTarget >= n || b.ikTarget == static_cast<int>(i)) { out.push_back(who + ": no target"); continue; }
        if (b.ikLinks.empty() || b.ikLoop <= 0 || !(b.ikLimit > 0)) out.push_back(who + ": no links, loops or angle limit");
        int expect = m.bones[static_cast<size_t>(b.ikTarget)].parent;
        for (const auto& l : b.ikLinks) {
            if (l.bone < 0 || l.bone >= n || l.bone != expect) { out.push_back(who + ": a link that is not the next ancestor of the target"); break; }
            if (l.limited && (l.lower.x > l.upper.x || l.lower.y > l.upper.y || l.lower.z > l.upper.z)) out.push_back(who + ": a limit with min above max");
            expect = m.bones[static_cast<size_t>(l.bone)].parent;
        }
        if (lengthOf(b.pos - m.bones[static_cast<size_t>(b.ikTarget)].pos) > 1e-3f) out.push_back(who + ": its handle is not on the target");
    }
    for (size_t i = 0; i < m.morphs.size(); ++i)
        for (const auto& vo : m.morphs[i].vertices)
            if (vo.first < 0 || vo.first >= static_cast<int>(m.vertices.size())) { out.push_back("morph " + std::to_string(i) + ": a vertex index past the list"); break; }
    for (size_t i = 0; i < m.frames.size(); ++i)
        for (const auto& it : m.frames[i].items)
            if (it.second < 0 || it.second >= static_cast<int>(it.first == 0 ? m.bones.size() : m.morphs.size())) { out.push_back("frame " + std::to_string(i) + ": an item past its list"); break; }
    for (size_t i = 0; i < m.bones.size(); ++i) {
        if (m.bones[i].parent >= static_cast<int>(m.bones.size())) out.push_back("bone " + std::to_string(i) + ": parent past the list");
        if (m.bones[i].tailBone >= static_cast<int>(m.bones.size())) out.push_back("bone " + std::to_string(i) + ": tail past the list");
    }
    return out;
}

pmxread::Model readPmx(const Bytes& b) { return pmxread::Read(b.data(), b.size()); }

void unitTests() {
    test("create materials: garments and eyes are drawn in linear light (the game's iris is 16/255 off drawn as stored), skin as stored", [] {
        using assemble::CreateRunsInLinearLight;
        CHECK(CreateRunsInLinearLight("cf_m_eye_create") && CreateRunsInLinearLight("cf_m_eyewhite_create") && CreateRunsInLinearLight("cf_m_clothesN_create"));
        CHECK(!CreateRunsInLinearLight("cf_m_face_create") && !CreateRunsInLinearLight("cf_m_body_create") && !CreateRunsInLinearLight(""));
    });
    test("item prefab roots keep their own transform under the node the game parents them to", [] {
        // SetParent(parent, worldPositionStays: false): a hair or accessory root with an offset and scale keeps both.
        const Mat4 parent = Mat4::TRS({1, 2, 3}, Quat{}, {1, 1, 1});
        const Mat4 got = assemble::PrefabRootPlacement(parent, {0, 0.051f, 0}, Quat{}, {0.98f, 1, 1});
        const Vec3 p = got.point({0, 0, 0});
        CHECK(std::fabs(p.x - 1) < 1e-5f && std::fabs(p.y - 2.051f) < 1e-5f && std::fabs(p.z - 3) < 1e-5f);
        const Vec3 x = got.dir({1, 0, 0});
        CHECK(std::fabs(x.x - 0.98f) < 1e-5f && std::fabs(x.y) < 1e-5f && std::fabs(x.z) < 1e-5f);
        // The identity root must leave the parent alone (every ordinary prefab).
        const Mat4 same = assemble::PrefabRootPlacement(parent, {0, 0, 0}, Quat{}, {1, 1, 1});
        for (int i = 0; i < 16; ++i) CHECK(std::fabs(same.m[i] - parent.m[i]) < 1e-6f);
    });

    test("linear light: colour properties are decoded, floats and texture scales are left alone", [] {
        using namespace cardbridge;
        for (float c : {0.0f, 0.02f, 0.2f, 0.5f, 1.0f}) CHECK(std::fabs(exporter::detail::ToSrgb(exporter::detail::ToLinear(c)) - c) < 1e-4f);
        CHECK(std::fabs(exporter::detail::ToLinear(0.5f) - 0.2140f) < 1e-3f);
        assemble::SceneMaterial m;
        m.props.emplace_back("_Color2", Vec4{0.5f, 0.5f, 0.5f, 0.5f});
        m.props.emplace_back("_ShadowColor", Vec4{1, 0, 0.5f, 1});
        m.props.emplace_back("_MainTex_ST", Vec4{0.5f, 0.5f, 0.5f, 0.5f});
        m.props.emplace_back("_rimpower", Vec4{0.5f, 0, 0, 0});
        const auto lin = exporter::detail::LinearColours(m);
        CHECK(std::fabs(lin.props[0].second.x - 0.2140f) < 1e-3f && lin.props[0].second.w == 0.5f);   // alpha is not converted
        CHECK(std::fabs(lin.props[1].second.z - 0.2140f) < 1e-3f && lin.props[1].second.x == 1.0f);
        CHECK(lin.props[2].second.x == 0.5f && lin.props[3].second.x == 0.5f);
    });
    test("MaterialEditor: every texture property of a material is read with its tiling, not only the main texture", [] {
        Bytes b;
        auto str = [&](const char* s) { b.push_back(static_cast<uint8_t>(0xa0 | std::strlen(s))); for (const char* c = s; *c; ++c) b.push_back(static_cast<uint8_t>(*c)); };
        auto f32 = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); b.push_back(0xca); for (int k = 3; k >= 0; --k) b.push_back(static_cast<uint8_t>(u >> (8 * k))); };
        auto row = [&](int slot, const char* prop, int texId, bool tiling) {
            b.push_back(0x80 | (tiling ? 8 : 6));
            str("CoordinateIndex"); b.push_back(1);
            str("ObjectType"); b.push_back(2);
            str("Slot"); b.push_back(static_cast<uint8_t>(slot));
            str("MaterialName"); str("mf_m_primmaterial");
            str("Property"); str(prop);
            str("TexID"); if (texId < 0) b.push_back(0xc0); else b.push_back(static_cast<uint8_t>(texId));
            if (tiling) { str("Scale"); b.push_back(0x92); f32(2); f32(3); str("Offset"); b.push_back(0x92); f32(0.5f); f32(0.25f); }
        };
        b.push_back(0x94);
        row(21, "MainTex", 9, false);
        row(21, "NormalMap", 10, true);
        row(21, "LineMask", -1, true);    // only the tiling of the game's own map
        row(21, "AnotherRamp", -1, false);   // nothing to apply: dropped
        Value payload(Value::Map{{Value(std::string("MaterialTexturePropertyList")), Value(b)}});
        Value ext(Value::Map{{Value(std::string("com.deathweasel.bepinex.materialeditor")), Value(Value::Array{Value(int64_t(1)), payload})}});
        const auto me = card::LoadMaterialEdits(ext, 1);
        CHECK(me.mainTexFor(1, 2, 21, "mf_m_primmaterial") == 9);
        const auto edits = me.texEditsFor(1, 2, 21, "mf_m_primmaterial");
        CHECK(edits.size() == 3);
        bool normal = false, mask = false;
        for (const auto* e : edits) {
            if (e->property == "NormalMap") normal = e->texId == 10 && e->hasScale && e->scale[0] == 2 && e->scale[1] == 3 && e->hasOffset && e->offset[0] == 0.5f && e->offset[1] == 0.25f;
            if (e->property == "LineMask") mask = e->texId < 0 && e->hasScale;
        }
        CHECK(normal && mask);
        CHECK(me.texEditsFor(1, 2, 22, "mf_m_primmaterial").empty());
    });
    test("shape sliders: key frames interpolate inside 0..1, extrapolate beyond it (SliderUnlocker)", [] {
        Bytes d;
        auto i32 = [&](int32_t v) { for (int k = 0; k < 4; ++k) d.push_back(static_cast<uint8_t>(v >> (8 * k))); };
        auto f32 = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); i32(static_cast<int32_t>(u)); };
        i32(2);
        for (const char* name : {"k", "cf_s_Mouth_x"}) {
            d.push_back(static_cast<uint8_t>(std::strlen(name)));
            for (const char* c = name; *c; ++c) d.push_back(static_cast<uint8_t>(*c));
            i32(2);
            const float keys[2][9] = {{0, 0, 0, 0, 0, 0, 1, 1, 1}, {1, 2, 3, 90, 0, 0, 3, 1, 1}};
            for (int k = 0; k < 2; ++k) { i32(k); for (float f : keys[k]) f32(f); }
        }
        auto anm = shape::AnimKeys::Parse(d);
        const bool all[3] = {true, true, true};
        Vec3 v[3];
        auto near = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        CHECK(anm.get("k", 0.5f, v, all));
        CHECK(near(v[0].x, 0.5f) && near(v[0].y, 1.0f) && near(v[2].x, 2.0f) && near(v[1].x, 45.0f));
        anm.get("k", 1.5f, v, all);
        CHECK(near(v[0].x, 1.5f) && near(v[0].z, 4.5f) && near(v[2].x, 4.0f) && near(v[1].x, 135.0f));
        anm.get("k", -0.5f, v, all);
        CHECK(near(v[0].y, -1.0f) && near(v[2].x, 0.0f) && near(v[1].x, -45.0f));
        anm.get("cf_s_Mouth_x", 1.5f, v, all);   // the plugin leaves these rotations clamped
        CHECK(near(v[0].x, 1.5f) && near(v[1].x, 90.0f));
    });
    test("typeBone correction table: int32 count, 9 floats a row; a truncated table is refused", [] {
        Bytes d;
        auto i32 = [&](int32_t v) { for (int k = 0; k < 4; ++k) d.push_back(static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * k)) & 0xFF)); };
        auto f32 = [&](float f) { uint32_t u; std::memcpy(&u, &f, 4); i32(static_cast<int32_t>(u)); };
        i32(3);
        for (int row = 0; row < 3; ++row)
            for (int k = 0; k < 9; ++k) f32(static_cast<float>(row * 10 + k));
        shape::ShapeState S;
        CHECK(shape::ParseCorrectTable(d, S));
        CHECK(S.correct(2).pos.x == 20.0f && S.correct(2).rot.y == 24.0f && S.correct(1).scl.z == 18.0f);
        d.resize(d.size() - 4);
        shape::ShapeState T;
        CHECK(!shape::ParseCorrectTable(d, T));
    });
    test("slider tables name every body and face slider the card format has", [] {
        CHECK(shape::BodySliders().size() == 44 && shape::FaceSliders().size() == 52);
        for (const auto& si : shape::BodySliders()) CHECK(si.group[0] && si.label[0]);
        for (const auto& si : shape::FaceSliders()) CHECK(si.group[0] && si.label[0]);
    });
    test("scene validation: finds out-of-range indices, bad weights and a part floating above the head", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"root", -1, {}, "rig"});
        SceneTexture t; t.width = 1; t.height = 1; t.rgba = {255, 255, 255, 255};
        s.textures.push_back(t);
        s.materials.push_back(SceneMaterial{});
        auto addPart = [&](const std::string& id, float y0, float y1) {
            SceneMesh m;
            m.name = id;
            for (float y : {y0, y1, y1}) { SceneVertex v; v.pos = {0, y, 0}; m.vertices.push_back(v); }
            SceneSubmesh sub; sub.material = 0; sub.indices = {0, 1, 2};
            m.submeshes.push_back(sub);
            s.meshes.push_back(m);
            ScenePart p; p.id = id; p.meshes = {static_cast<int>(s.meshes.size()) - 1};
            s.parts.push_back(p);
        };
        addPart("head", 1.3f, 1.6f);
        addPart("outfit0.top", 0.8f, 1.4f);
        CHECK(exporter::ValidateScene(s).empty());
        addPart("outfit0.acc44", 2.0f, 2.5f);   // a vest lifted by its attach node
        auto f = exporter::ValidateScene(s);
        CHECK(f.size() == 1 && f[0].find("outfit0.acc44") != std::string::npos);
        s.meshes[0].vertices[0].weight[0] = 0.5f;
        s.meshes[1].submeshes[0].indices[2] = 9;
        CHECK(exporter::ValidateScene(s).size() == 3);
    });
    test("item cut-out: a main_item texture's alpha below 0.25 where its triangles lie cuts it, empty UV space does not", [] {
        using namespace assemble;
        Scene s;
        SceneTexture t; t.width = 2; t.height = 1; t.rgba = {255, 255, 255, 0, 255, 255, 255, 255};   // left texel cut, right solid
        s.textures.push_back(t);
        SceneMaterial m; m.shader = "Shader Forge/main_item"; m.texture = 0;
        s.materials.push_back(m);
        SceneMesh mesh;
        for (float u : {0.6f, 0.9f, 0.9f}) { SceneVertex v; v.uv = {u, 0.5f}; mesh.vertices.push_back(v); }
        SceneSubmesh sub; sub.material = 0; sub.indices = {0, 1, 2};
        mesh.submeshes.push_back(sub);
        s.meshes.push_back(mesh);
        CHECK(!exporter::HasCutout(s, s.materials[0]));   // the triangle samples only the solid half
        for (auto& v : s.meshes[0].vertices) v.uv.x -= 0.5f;
        CHECK(exporter::HasCutout(s, s.materials[0]));
        CHECK((exporter::MmdMaterialLook(s, s.materials[0]).drawFlags & 0x16) == 0);   // no ground shadow, shadow map or edge for the cut mesh
        s.materials[0].shader = "Shader Forge/main_opaque";
        CHECK(!exporter::HasCutout(s, s.materials[0]));
    });
    test("double-layered mesh: MMD's edge would poke the inner layer through, so it gets none; a single surface keeps its edge", [] {
        using namespace assemble;
        auto quad = [](bool doubled, bool alpha) {
            Scene s;
            SceneMaterial m; m.shader = alpha ? "Shader Forge/main_alpha" : "Shader Forge/main_hair"; m.alpha = alpha;
            s.materials.push_back(m);
            SceneMesh mesh;
            const float px[4] = {0, 1, 1, 0}, py[4] = {0, 0, 1, 1};
            for (int i = 0; i < 4; ++i) { SceneVertex v; v.pos = {px[i], py[i], 0}; v.normal = {0, 0, 1}; mesh.vertices.push_back(v); }
            SceneSubmesh sub; sub.material = 0; sub.indices = {0, 1, 2, 0, 2, 3};
            if (doubled) sub.indices.insert(sub.indices.end(), {0, 2, 1, 0, 3, 2});   // the inner layer, wound the other way
            mesh.submeshes.push_back(sub);
            s.meshes.push_back(mesh);
            return s;
        };
        Scene dbl = quad(true, true), sgl = quad(false, true), hair = quad(true, false);
        CHECK(exporter::HasInnerLayer(dbl, dbl.materials[0]));
        CHECK(!exporter::HasInnerLayer(sgl, sgl.materials[0]));
        CHECK((exporter::MmdMaterialLook(dbl, dbl.materials[0]).drawFlags & 0x10) == 0);   // no edge on the double layer (garment)
        CHECK((exporter::MmdMaterialLook(hair, hair.materials[0]).drawFlags & 0x10) == 0);   // no edge on the double layer (hair: it would clip)
        CHECK((exporter::MmdMaterialLook(sgl, sgl.materials[0]).drawFlags & 0x10) != 0);   // the single surface keeps its edge
    });
    test("KoiClothesOverlayX: overlays are read for the asked coordinate only, with the replace flag", [] {
        Bytes png = tinyPng();
        auto overlayFor = [&](int coordinate, bool replace) {   // {coordinate: {"ct_top_parts_B": [png, replace]}} as MessagePack
            Bytes m = B({0x81, coordinate, 0x81, 0xae});
            for (char c : std::string("ct_top_parts_B")) m.push_back(static_cast<uint8_t>(c));
            m.push_back(0x92);
            m.push_back(0xc4);
            m.push_back(static_cast<uint8_t>(png.size()));
            m.insert(m.end(), png.begin(), png.end());
            m.push_back(replace ? 0xc3 : 0xc2);
            return m;
        };
        auto extendedWith = [&](Bytes overlays) {
            Value::Array kcox;
            kcox.emplace_back(int64_t{1});
            kcox.emplace_back(Value::Map{{Value(std::string("Overlays")), Value(std::move(overlays))}});
            return Value(Value::Map{{Value(std::string("KCOX")), Value(std::move(kcox))}});
        };
        card::MaterialEdits::Image img;
        bool replace = false;
        card::MaterialEdits other = card::LoadMaterialEdits(extendedWith(overlayFor(1, false)), 0);
        CHECK(!other.decodeClothesOverlay("ct_top_parts_B", img, replace));
        card::MaterialEdits me = card::LoadMaterialEdits(extendedWith(overlayFor(2, true)), 2);
        CHECK(me.decodeClothesOverlay("ct_top_parts_B", img, replace));
        CHECK(replace && img.width == 1 && img.height == 1);
        CHECK(!me.decodeClothesOverlay("ct_clothesTop", img, replace));
    });
    test("KoiSkinOverlayX: the older BodyUnder/BodyOver/FaceUnder layout maps under layers to their own slots", [] {
        auto extendedWith = [&](Bytes bodyUnder, Bytes bodyOver, Bytes faceUnder) {   // KSOX: [1, {name: bytes}], the older layout
            Value::Map ksox;
            if (!bodyUnder.empty()) ksox.emplace_back(Value(std::string("BodyUnder")), Value(std::move(bodyUnder)));
            if (!bodyOver.empty()) ksox.emplace_back(Value(std::string("BodyOver")), Value(std::move(bodyOver)));
            if (!faceUnder.empty()) ksox.emplace_back(Value(std::string("FaceUnder")), Value(std::move(faceUnder)));
            Value::Array arr;
            arr.emplace_back(int64_t{1});
            arr.emplace_back(Value(std::move(ksox)));
            return Value(Value::Map{{Value(std::string("KSOX")), Value(std::move(arr))}});
        };
        Bytes png = tinyPng();
        card::MaterialEdits::Image img;
        card::MaterialEdits me = card::LoadMaterialEdits(extendedWith(png, png, png), 0);
        CHECK(me.decodeSkinOverlay(card::MaterialEdits::OverlayBodyUnder, img) && img.width == 1 && img.height == 1);
        CHECK(me.decodeSkinOverlay(card::MaterialEdits::OverlayFaceUnder, img) && img.width == 1);
        CHECK(me.decodeSkinOverlay(card::MaterialEdits::OverlayBody, img) && img.width == 1);   // the over layer joins the finished-skin slot
        card::MaterialEdits underOnly = card::LoadMaterialEdits(extendedWith(png, {}, {}), 0);
        CHECK(underOnly.decodeSkinOverlay(card::MaterialEdits::OverlayBodyUnder, img));
        CHECK(!underOnly.decodeSkinOverlay(card::MaterialEdits::OverlayBody, img));   // no over layer: the finished-skin slot stays empty
    });

    test("dynamic bone: a stiff chain keeps its pose, a loose one hangs down, a collider pushes it out", [] {
        using assemble::DbCollider; using assemble::DbParams; using assemble::DbTree;
        // a chain of four nodes one unit apart along +X, root fixed at the origin
        auto chain = [] {
            std::vector<Mat4> local(4);
            std::vector<int> parent = {-1, 0, 1, 2};
            for (int i = 1; i < 4; ++i) local[static_cast<size_t>(i)] = Mat4::TRS({1, 0, 0}, Quat{}, {1, 1, 1});
            DbTree t;
            t.Init(local, parent, Mat4{});
            return t;
        };
        DbParams p;
        p.root = 0;
        p.stiffness = 1.0f; p.elasticity = 0.1f; p.damping = 0.2f;
        p.force = {0, -0.01f, 0};
        DbTree stiff = chain();
        assemble::SettleDynamicBone(stiff, p, {});
        for (int i = 1; i < 4; ++i) CHECK(std::fabs(stiff.WorldOf(i).translation().y) < 1e-3f && std::fabs(stiff.WorldOf(i).translation().x - i) < 1e-3f);

        p.stiffness = 0.0f; p.elasticity = 0.0f; p.damping = 0.5f;
        DbTree loose = chain();
        assemble::SettleDynamicBone(loose, p, {});
        Vec3 tip = loose.WorldOf(3).translation();
        CHECK(tip.y < -2.5f && std::fabs(tip.x) < 0.2f);   // hangs straight down from the root
        for (int i = 1; i < 4; ++i) {   // bone lengths are kept
            Vec3 a = loose.WorldOf(i).translation(), b = loose.WorldOf(i - 1).translation();
            CHECK(std::fabs(std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)) - 1.0f) < 1e-2f);
        }

        DbCollider c;   // a sphere in the way of the hanging chain
        c.center = {0.2f, -1.0f, 0.0f};
        c.radius = 0.5f;
        DbTree blocked = chain();
        assemble::SettleDynamicBone(blocked, p, {c});
        Vec3 b1 = blocked.WorldOf(1).translation();
        CHECK(std::sqrt((b1.x - 0.2f) * (b1.x - 0.2f) + (b1.y + 1.0f) * (b1.y + 1.0f)) >= 0.4f);   // the length constraint runs after the collision, so a little overlap can remain
    });

    test("KKABMX: the compressed bone table is read, one entry for every coordinate or one per coordinate", [] {
        // [ ["cf_J_FaceBase", [[[0.92, 0.92, 0.96], 1.0]]], ["cf_d_sk_top", [ [[0.9,1,0.9],1.0,[0,0,0],[0,0,0]], [[1,1,1],1.0,[0,0,0],[0,0,0]] ]] ] as MessagePack
        Bytes m = B({0x92, 0x92, 0xad});
        for (char ch : std::string("cf_J_FaceBase")) m.push_back(static_cast<uint8_t>(ch));
        auto f32 = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); m.push_back(0xca); for (int s = 24; s >= 0; s -= 8) m.push_back(static_cast<uint8_t>(u >> s)); };
        m.push_back(0x91); m.push_back(0x92); m.push_back(0x93); f32(0.92f); f32(0.92f); f32(0.96f); f32(1.0f);
        m.push_back(0x92); m.push_back(0xab);
        for (char ch : std::string("cf_d_sk_top")) m.push_back(static_cast<uint8_t>(ch));
        m.push_back(0x92);
        for (int k = 0; k < 2; ++k) { m.push_back(0x94); m.push_back(0x93); f32(k == 0 ? 0.9f : 1.0f); f32(1.0f); f32(k == 0 ? 0.9f : 1.0f); f32(1.0f); m.push_back(0x93); f32(0); f32(0); f32(0); m.push_back(0x93); f32(0); f32(0); f32(0); }
        // wrap as an uncompressed block (a single LZ4 literal run) in ext type 99
        Bytes block;
        block.push_back(static_cast<uint8_t>(m.size() < 15 ? m.size() << 4 : 0xF0));
        size_t extra = m.size() >= 15 ? m.size() - 15 : 0;
        if (m.size() >= 15) { while (extra >= 255) { block.push_back(255); extra -= 255; } block.push_back(static_cast<uint8_t>(extra)); }
        block.insert(block.end(), m.begin(), m.end());
        Bytes ext = {0xc9, 0, 0, 0, 0, 99, 0xce, 0, 0, 0, 0};
        uint32_t ul = static_cast<uint32_t>(m.size());
        for (int i = 0; i < 4; ++i) ext[7 + static_cast<size_t>(i)] = static_cast<uint8_t>(ul >> (24 - 8 * i));
        uint32_t el = static_cast<uint32_t>(1 + 5 + block.size());
        for (int i = 0; i < 4; ++i) ext[1 + static_cast<size_t>(i)] = static_cast<uint8_t>(el >> (24 - 8 * i));
        ext.insert(ext.end(), block.begin(), block.end());
        Value payload(Value::Map{{Value(std::string("boneData")), Value(ext)}});
        Value::Array arr;
        arr.emplace_back(int64_t{0});
        arr.emplace_back(payload);
        Value extended(Value::Map{{Value(std::string("KKABMPlugin.ABMData")), Value(arr)}});
        card::AbmxTable t = card::LoadAbmx(extended);
        const card::AbmxMod* face = t.find("cf_J_FaceBase", 3);   // one entry: every coordinate
        CHECK(face && std::fabs(face->scale.z - 0.96f) < 1e-6f && face->hasScale());
        const card::AbmxMod* sk0 = t.find("cf_d_sk_top", 0);
        const card::AbmxMod* sk1 = t.find("cf_d_sk_top", 1);
        CHECK(sk0 && sk1 && sk0->hasScale() && !sk1->hasScale());
        CHECK(t.find("cf_d_sk_top", 5) == nullptr);
    });

    test("image bytes (the loose textures mods ship) decode to RGBA", [] {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        CHECK(card::DecodeImageBytes(tinyPng(), w, h, rgba));
        CHECK(w == 1 && h == 1 && rgba.size() == 4);
        CHECK(!card::DecodeImageBytes(B({1, 2, 3}), w, h, rgba));
    });

    test("loose PNGs get Sideloader's wrap rule: clamp/repeat by file name, repeat otherwise", [] {
        // The game serves loose PNGs from the mod's folder as-is and picks the wrap mode from the png's path
        // (Sideloader.dll GetPng: "clamp" in the path clamps, "repeat" in it repeats, else Unity's default).
        // Decal mods (tattoos, nip textures) ship the image named _clamp because the placement shader samples
        // outside 0..1 and must see the transparent border instead of a tile.
        const std::string zipPath = writeStoredZip("loose-wrap-rule.zip", {
            {"abdata/chara/testtatto/mark_clamp.png", tinyPng()},
            {"abdata/chara/testtatto/mark_repeat.png", tinyPng()},
            {"abdata/chara/testtatto/mark_plain.png", tinyPng()},
        });
        CHECK(!zipPath.empty());
        if (zipPath.empty()) return;
        resolve::ModIndex mi;
        resolve::ListTable lists;
        const std::string emptyDir = tempPath("empty-mod-root");
        std::filesystem::create_directories(emptyDir);
        resolve::Resolver resolver(mi, lists, emptyDir);
        resolve::ResolvedItem item;
        item.status = resolve::ItemStatus::ResolvedMod;
        item.modPath = zipPath;
        auto wrapOf = [&](const char* ab, const char* texture) {
            auto t = resolver.openLooseTexture(item, ab, texture, nullptr);
            return t ? t->wrapMode : -1;
        };
        CHECK(wrapOf("chara/testtatto.unity3d", "mark_clamp") == 1);   // clamp when the name says clamp
        CHECK(wrapOf("chara/testtatto.unity3d", "mark_repeat") == 0);  // repeat when the name says repeat
        CHECK(wrapOf("chara/testtatto.unity3d", "mark_plain") == 0);   // else Unity's default (repeat)
        CHECK(wrapOf("chara/testtatto.unity3d", "missing") == -1);     // a name with nothing behind it is not found, not guessed
        std::remove(zipPath.c_str());
    });

    test("sha256", [] {
        std::string s = "abc";
        CHECK(Sha256Hex(reinterpret_cast<const uint8_t*>(s.data()), s.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(Sha256Hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    });
    test("lz4: literals, matches, overlapping match, bad input", [] {
        // token 0x40: 4 literals "abcd", then (last sequence) literals only
        Bytes lit = B({0x40, 'a', 'b', 'c', 'd'});
        CHECK(Lz4DecompressBlock(lit.data(), lit.size(), 4) == B({'a', 'b', 'c', 'd'}));
        // "a" literal then a match of length 4+? offset 1 -> "aaaaa" (overlap): token 0x10|0 => 1 literal, match len 4
        Bytes ov = B({0x10, 'a', 0x01, 0x00, 0x00 + 0x10 /*unused*/});
        ov.pop_back();
        ov.push_back(0x10); ov.push_back('b');   // trailing literal-only sequence "b"
        Bytes out = Lz4DecompressBlock(ov.data(), ov.size(), 6);
        CHECK(out == B({'a', 'a', 'a', 'a', 'a', 'b'}));
        bool threw = false;
        try { Lz4DecompressBlock(lit.data(), lit.size(), 9); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        threw = false;
        Bytes bad = B({0x10, 'a', 0x09, 0x00});   // offset beyond what was written
        try { Lz4DecompressBlock(bad.data(), bad.size(), 8); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });
    test("inflate: stored block, fixed Huffman, truncated", [] {
        Bytes stored = B({0x01, 0x03, 0x00, 0xfc, 0xff, 'x', 'y', 'z'});
        CHECK(InflateRaw(stored.data(), stored.size(), 3) == B({'x', 'y', 'z'}));
        // zlib.compress(b"hello hello hello", wbits=-15) from Python
        Bytes fixed = B({0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x00});
        Bytes got = InflateRaw(fixed.data(), fixed.size(), 17);
        CHECK(std::string(got.begin(), got.end()) == "hello hello hello");
        bool threw = false;
        try { InflateRaw(fixed.data(), 3, 17); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });
    test("msgpack: maps, arrays, ints, bin, nested, errors", [] {
        // {"a": [1, -2, 300], "b": bin(2), "c": 1.5, "d": nil}
        Bytes d = B({0x84, 0xa1, 'a', 0x93, 0x01, 0xfe, 0xcd, 0x01, 0x2c, 0xa1, 'b', 0xc4, 0x02, 0xaa, 0xbb, 0xa1, 'c', 0xcb,
                     0x3f, 0xf8, 0, 0, 0, 0, 0, 0, 0xa1, 'd', 0xc0});
        Value v = MsgPackParse(d);
        CHECK(v.at("a").at(size_t(2)).asInt() == 300);
        CHECK(v.at("a").at(size_t(1)).asInt() == -2);
        CHECK(v.at("b").asBin() == B({0xaa, 0xbb}));
        CHECK(v.at("c").asFloat() == 1.5);
        CHECK(v.at("d").isNil());
        CHECK(v.at("missing").isNil());
        bool threw = false;
        try { MsgPackParse(B({0x93, 0x01})); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { MsgPackParse(B({0x01, 0x02})); } catch (const FormatError&) { threw = true; }   // trailing bytes
        CHECK(threw);
    });
    test("csv line splitting", [] {
        auto r = resolve::SplitCsvLine("1,\"a,b\",\"say \"\"hi\"\"\",");
        CHECK(r.size() == 4 && r[0] == "1" && r[1] == "a,b" && r[2] == "say \"hi\"" && r[3].empty());
    });
    test("utf-8 to utf-16le", [] {
        CHECK(pmx::Utf8ToUtf16Le("A") == B({'A', 0}));
        CHECK(pmx::Utf8ToUtf16Le("\xE8\xA1\xA8") == B({0x68, 0x88}));           // 表
        CHECK(pmx::Utf8ToUtf16Le("\xF0\x9F\x98\x80") == B({0x3d, 0xd8, 0x00, 0xde}));   // U+1F600
    });
    test("card loader rejects non-cards with specific errors", [] {
        using card::CardErrorKind;
        auto kindOf = [](const Bytes& data) {
            std::string p = tempPath("probe.bin");
            fileio::WriteFile(p, data.data(), data.size());
            try { card::LoadCard(p); } catch (const card::CardError& e) { return e.kind; }
            return CardErrorKind::CorruptPayload;   // not reached for these inputs
        };
        CHECK(kindOf(B({0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0})) == CardErrorKind::NotPng);
        Bytes png = tinyPng();
        CHECK(kindOf(png) == CardErrorKind::NoPayload);
        Bytes trunc(png.begin(), png.begin() + 40);
        CHECK(kindOf(trunc) == CardErrorKind::TruncatedPng);
        Bytes junk = png;
        for (int i = 0; i < 20; ++i) junk.push_back(0x7f);
        CHECK(kindOf(junk) == CardErrorKind::UnknownMarker);
        // a Koikatsu outfit file marker after a valid PNG
        Bytes cl = png;
        for (uint8_t x : {100, 0, 0, 0}) cl.push_back(x);
        std::string mark = "\xE3\x80\x90KoiKatuClothes\xE3\x80\x91";
        cl.push_back(static_cast<uint8_t>(mark.size()));
        cl.insert(cl.end(), mark.begin(), mark.end());
        CHECK(kindOf(cl) == CardErrorKind::ClothesFile);
    });
    test("outfit names: the seven Koikatsu slots, with a numbered fallback", [] {
        card::Card c;
        c.family = card::CardFamily::Koikatsu;
        c.outfits.resize(7);
        CHECK(c.outfitName(0) == "School Uniform (In School)");
        CHECK(c.outfitName(1) == "School Uniform (Going Home)");
        CHECK(c.outfitName(2) == "Gym Uniform");
        CHECK(c.outfitName(3) == "Swimsuit");
        CHECK(c.outfitName(4) == "Club Activities");
        CHECK(c.outfitName(5) == "Casual Clothes");
        CHECK(c.outfitName(6) == "Sleeping Over");
        CHECK(c.outfitName(7) == "Outfit 8");
        CHECK(c.outfitName(-1) == "Outfit 0");
        // A family whose coordinate order is not known gets neutral names.
        c.family = card::CardFamily::KoikatsuSunshine;
        CHECK(c.outfitName(0) == "Outfit 1");
    });
    test("pmx writer: counts, validation errors", [] {
        pmx::Model m;
        m.name = "t";
        pmx::Bone b; b.name = "root"; m.bones.push_back(b);
        for (int i = 0; i < 3; ++i) { pmx::Vertex v; v.position = {float(i), 0, 0}; m.vertices.push_back(v); }
        m.indices = {0, 1, 2};
        pmx::Material mat; mat.name = "m"; mat.indexCount = 3; m.materials.push_back(mat);
        Bytes out = pmx::WritePmx(m);
        CHECK(out.size() > 100 && out[0] == 'P' && out[1] == 'M' && out[2] == 'X' && out[3] == ' ');
        m.indices = {0, 1, 7};
        bool threw = false;
        try { pmx::WritePmx(m); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        m.indices = {0, 1, 2};
        m.materials[0].indexCount = 6;
        threw = false;
        try { pmx::WritePmx(m); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });
    test("nude body: under pieces show when their garments are off or the nude box is ticked", [] {
        using namespace assemble;
        Scene sc;
        auto add = [&](const std::string& id, bool visible = true) { ScenePart p; p.id = id; p.visibleInPreview = visible; sc.parts.push_back(p); return sc.parts.size() - 1; };
        size_t body = add("body"), top = add("top"), bra = add("bra"), hiddenByGame = add("hid", false);
        size_t pTop = add("body.under.top", false), pBoth = add("body.under.bra+top", false), pGame = add("body.under.always", false);
        sc.parts[pTop].underPiece = true; sc.parts[pTop].underOf = "body"; sc.parts[pTop].coveredBy = {"top"};
        sc.parts[pBoth].underPiece = true; sc.parts[pBoth].underOf = "body"; sc.parts[pBoth].coveredBy = {"bra", "top"};
        sc.parts[pGame].underPiece = true; sc.parts[pGame].underOf = "body"; sc.parts[pGame].coveredBy = {""};
        size_t nude = add("nude");
        sc.parts[nude].nudeSwitch = true;
        (void)hiddenByGame;
        std::vector<char> hidden(sc.parts.size(), 0);
        hidden[nude] = 1;
        auto eff = exporter::EffectiveHiddenParts(sc, hidden);
        CHECK(eff[pTop] == 1 && eff[pBoth] == 1 && eff[pGame] == 1);   // everything dressed: no skin
        hidden[top] = 1;
        eff = exporter::EffectiveHiddenParts(sc, hidden);
        CHECK(eff[pTop] == 2 && eff[pBoth] == 1 && eff[pGame] == 1);   // top off: the skin only it covers; the bra still covers its part
        hidden[bra] = 1;
        eff = exporter::EffectiveHiddenParts(sc, hidden);
        CHECK(eff[pTop] == 2 && eff[pBoth] == 2 && eff[pGame] == 1);   // a garment the game hides keeps covering
        hidden[top] = hidden[bra] = 0;
        hidden[nude] = 0;
        eff = exporter::EffectiveHiddenParts(sc, hidden);
        CHECK(eff[pTop] == 2 && eff[pBoth] == 2 && eff[pGame] == 2);   // nude box: everything, clothes stay
        CHECK(eff[top] == 0 && eff[body] == 0);
        hidden[body] = 1;
        eff = exporter::EffectiveHiddenParts(sc, hidden);
        CHECK(eff[pTop] == 1);   // no body, no skin pieces
    });
    test("hidden item: not in the export at all, its own bones pruned, preview 1:1 with the viewer", [] {
        using namespace assemble;
        Scene sc;
        // Rig bones (shared, never pruned while anything exported uses them).
        const int root = static_cast<int>(sc.bones.size()); sc.bones.push_back({"cf_j_root", -1, {0, 0, 0}, "rig"});
        const int spine = static_cast<int>(sc.bones.size()); sc.bones.push_back({"cf_j_spine01", root, {0, 1, 0}, "rig"});
        // Hair's own joint chain and a top's own joint, each owned by its part.
        const int hairJoint = static_cast<int>(sc.bones.size()); sc.bones.push_back({"cf_J_hairB_top", spine, {0, 2, 0}, "hair.back"});
        const int hairTip = static_cast<int>(sc.bones.size()); sc.bones.push_back({"hair.back/end", hairJoint, {0, 2.5f, 0}, "hair.back"});
        const int topJoint = static_cast<int>(sc.bones.size()); sc.bones.push_back({"outfit0.top/joint", spine, {0, 1.5f, 0}, "outfit0.top"});
        // One mesh per part, weighted to its own joint (body to the rig).
        auto mesh = [&](int bone, Vec3 p) {
            SceneMesh m;
            m.vertices.resize(3);
            for (int i = 0; i < 3; ++i) {
                m.vertices[static_cast<size_t>(i)].pos = p;
                m.vertices[static_cast<size_t>(i)].normal = {0, 0, 1};
                m.vertices[static_cast<size_t>(i)].bone[0] = bone;
                m.vertices[static_cast<size_t>(i)].weight[0] = 1;
            }
            m.submeshes.push_back({0, {0, 1, 2}});
            sc.meshes.push_back(m);
            return static_cast<int>(sc.meshes.size()) - 1;
        };
        SceneMaterial mat; mat.name = "m"; mat.diffuse = {1, 1, 1, 1};
        sc.materials.push_back(mat);
        auto part = [&](const std::string& id, const std::string& group, int mi) {
            ScenePart p; p.id = id; p.group = group; p.meshes = {mi}; sc.parts.push_back(p);
            return sc.parts.size() - 1;
        };
        const size_t body = part("body", "Body", mesh(spine, {0, 1, 0}));
        const size_t hair = part("hair.back", "Hair", mesh(hairTip, {0, 2.5f, 0}));
        const size_t top = part("outfit0.top", "Outfit", mesh(topJoint, {0, 1.5f, 0}));
        // Hair physics belongs to the hair part, body jiggle to the body.
        { SceneChain c; c.part = "hair.back"; c.bones = {hairJoint, hairTip}; sc.chains.push_back(c); }
        { SceneChain c; c.part = "rig"; c.bones = {root, spine}; sc.chains.push_back(c); }
        { SceneCollider col; col.name = "body"; col.bone = spine; col.radius = 0.1f; sc.colliders.push_back(col); }
        const size_t bonesBefore = sc.bones.size();
        std::vector<char> hidden(sc.parts.size(), 0);
        hidden[hair] = 1;   // the hair item is unticked in the list
        exporter::ApplyPartStateForExport(sc, hidden);
        CHECK(!sc.parts[hair].includeInExport);                 // gone from the PMX, not just transparent
        CHECK(sc.parts[body].includeInExport && sc.parts[top].includeInExport);
        CHECK(sc.boneIndex("cf_J_hairB_top") < 0 && sc.boneIndex("hair.back/end") < 0);   // its bones are not exported
        CHECK(sc.boneIndex("cf_j_spine01") >= 0 && sc.bones[static_cast<size_t>(sc.boneIndex("cf_j_spine01"))].parent == sc.boneIndex("cf_j_root"));
        CHECK(sc.bones.size() == bonesBefore - 2);              // exactly the two hair bones left
        CHECK(sc.chains.size() == 1 && sc.chains[0].part == "rig");   // hair physics left with it, body jiggle stays
        for (const auto& m : sc.meshes)                          // every remaining weight names a bone that exists
            for (const auto& v : m.vertices)
                for (int k = 0; k < 4; ++k) if (v.weight[k] > 0) CHECK(v.bone[k] >= 0 && static_cast<size_t>(v.bone[k]) < sc.bones.size());
        CHECK(exporter::ValidateScene(sc).empty());
        // 1:1: the exported picture takes the same hidden flags as the viewer.
        const auto eff = exporter::ExportHiddenParts(sc);
        CHECK(eff[hair] == 1 && eff[body] == 0 && eff[top] == 0);
        CHECK(exporter::PruneExportBones(sc) == 0);              // idempotent
        // Pruning also works without Apply when a part was excluded at assembly.
        Scene sc2;
        sc2.bones.push_back({"cf_j_root", -1, {0, 0, 0}, "rig"});
        sc2.bones.push_back({"outfit0.bra/joint", 0, {0, 1, 0}, "outfit0.bra"});
        { ScenePart p; p.id = "outfit0.bra"; p.group = "Outfit"; p.includeInExport = false; sc2.parts.push_back(p); }
        { ScenePart p; p.id = "body"; p.group = "Body"; sc2.parts.push_back(p); }
        CHECK(exporter::PruneExportBones(sc2) == 1 && sc2.bones.size() == 1 && sc2.boneIndex("cf_j_root") >= 0);
    });
    test("shader layers: the header comments name, blend and default state; other files are found by name", [] {
        const std::string dir = fileio::Join(std::filesystem::temp_directory_path().string(), "cardbridge_layers_test");
        fileio::CreateDirectories(dir);
        fileio::WriteTextFile(fileio::Join(dir, "b_glow.hlsl"), "// layer: Glow\n// blend: Multiply\n// default: on\nfloat4 ps(VO i) : SV_Target { return 1; }\n");
        fileio::WriteTextFile(fileio::Join(dir, "a_plain.hlsl"), "float4 ps(VO i) : SV_Target { return 0; }\n");
        fileio::WriteTextFile(fileio::Join(dir, "c_odd.hlsl"), "// blend: sideways\n// default: maybe\nfloat4 ps(VO i) : SV_Target { return 0; }\n");
        auto layers = exporter::LoadShaderLayers({dir, fileio::Join(dir, "missing")});
        CHECK(layers.size() == 3);
        if (layers.size() == 3) {
            CHECK(layers[0].file == "a_plain.hlsl" && layers[0].name == "a_plain" && layers[0].blend == "add" && !layers[0].defaultOn);
            CHECK(layers[1].name == "Glow" && layers[1].blend == "multiply" && layers[1].defaultOn);
            CHECK(layers[2].blend == "add" && !layers[2].defaultOn);   // unknown values fall back
            CHECK(layers[1].source.find("SV_Target") != std::string::npos);
        }
        CHECK(std::string(exporter::ShaderLayerPrelude()).find("VO vs(VI i)") != std::string::npos);
        for (const char* f : {"a_plain.hlsl", "b_glow.hlsl", "c_odd.hlsl"}) fileio::RemoveFile(fileio::Join(dir, f));
    });
    test("pmx writer: material and group morphs", [] {
        pmx::Model m;
        m.name = "t";
        pmx::Bone b; b.name = "root"; m.bones.push_back(b);
        for (int i = 0; i < 3; ++i) { pmx::Vertex v; v.position = {float(i), 0, 0}; m.vertices.push_back(v); }
        m.indices = {0, 1, 2};
        pmx::Material mat; mat.name = "m"; mat.indexCount = 3; m.materials.push_back(mat);
        Bytes plain = pmx::WritePmx(m);
        pmx::Morph hide;
        hide.name = "Hide m";
        pmx::MaterialOffset off; off.material = 0; off.add = false; off.diffuse = {1, 1, 1, 0};
        hide.materials.push_back(off);
        m.morphs.push_back(hide);
        pmx::Morph grp; grp.name = "All"; grp.group.emplace_back(0, 1.0f);
        m.morphs.push_back(grp);
        Bytes withMorphs = pmx::WritePmx(m);
        // each morph: two names, panel, type, count, then 4 + 1 + 4*4 + 3*4 + 4 + 3*4 + 4*4 + 4 + 3*4*4 bytes (material) or 8 (group)
        CHECK(withMorphs.size() > plain.size() + 100);
        m.morphs[1].group[0].first = 5;   // a group member that does not exist
        bool threw = false;
        try { pmx::WritePmx(m); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        m.morphs[1].group[0].first = 0;
        m.morphs[0].materials[0].material = 9;   // a material that does not exist
        threw = false;
        try { pmx::WritePmx(m); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });

    test("pmx writer: vertex morphs, tail bones, rigid bodies, joints and display frames", [] {
        pmx::Model m;
        m.name = "t";
        pmx::Bone a; a.name = "a"; a.tailBone = 1; m.bones.push_back(a);
        pmx::Bone b; b.name = "b"; b.parent = 0; m.bones.push_back(b);
        for (int i = 0; i < 3; ++i) { pmx::Vertex v; v.position = {float(i), 0, 0}; m.vertices.push_back(v); }
        m.indices = {0, 1, 2};
        pmx::Material mat; mat.name = "m"; mat.indexCount = 3; m.materials.push_back(mat);
        const size_t plain = pmx::WritePmx(m).size();
        pmx::Morph vm; vm.name = "smile"; vm.vertices.emplace_back(1, Vec3{0, 0.1f, 0});
        m.morphs.push_back(vm);
        pmx::RigidBody rb; rb.name = "body"; rb.bone = 1; rb.shape = 2; m.rigidBodies.push_back(rb);
        pmx::RigidBody anchor; anchor.name = "anchor"; anchor.bone = 0; m.rigidBodies.push_back(anchor);
        pmx::Joint j; j.name = "j"; j.bodyA = 1; j.bodyB = 0; m.joints.push_back(j);
        pmx::DisplayFrame root; root.name = "Root"; root.special = true; root.bones = {0};
        pmx::DisplayFrame exp; exp.name = "Exp"; exp.special = true; exp.morphs = {0};
        pmx::DisplayFrame grp; grp.name = "Face"; grp.bones = {1};
        m.frames = {root, exp, grp};
        const size_t full = pmx::WritePmx(m).size();
        // rigid body: 2 names (4 + 2 chars each ...), bone, group, mask, shape, 3 vec3, 5 floats, mode -> more than 80 bytes each; joint 4 + 2*3 + 8 * 12 + ...
        CHECK(full > plain + 2 * 80 + 100);
        pmx::Model bad = m;
        bad.joints[0].bodyB = 7;   // a body that does not exist
        bool threw = false;
        try { pmx::WritePmx(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        bad = m;
        bad.morphs[0].vertices[0].first = 3;   // a vertex that does not exist
        threw = false;
        try { pmx::WritePmx(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        bad = m;
        bad.frames[2].bones = {5};
        threw = false;
        try { pmx::WritePmx(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });

    test("bone labels: English guesses, helper flags and display groups", [] {
        using namespace exporter;
        CHECK(BoneLabelEn("cf_J_Mayu_L") == "Left Brow");
        CHECK(BoneLabelEn("cf_J_Eye01_s_R") == "Right Eye 01");
        CHECK(BoneLabelEn("cf_d_arm01_L") == "Left Arm 01 (driven)");
        CHECK(BoneLabelEn("outfit0.acc03/joint 4").rfind("outfit0.acc03: ", 0) == 0);
        CHECK(IsHelperBone("cf_d_arm01_L") && IsHelperBone("cf_s_head") && IsHelperBone("a_n_hair_pony"));
        CHECK(!IsHelperBone("cf_j_hips") && !IsHelperBone("cf_J_FaceBase"));
        assemble::Scene s;
        auto add = [&](const char* name, int parent, const char* source = "rig") { s.bones.push_back({name, parent, {}, source}); return static_cast<int>(s.bones.size()) - 1; };
        int root = add("cf_n_height", -1);
        int hips = add("cf_j_hips", root);
        int arm = add("cf_j_arm00_L", hips);
        int hand = add("cf_j_hand_L", arm);
        int eye = add("cf_J_Eye01_s_L", hips);
        int brow = add("cf_J_Mayu_R", hips);
        int drv = add("cf_d_arm01_L", arm);
        int strand = add("hair.back/strand 1", hips, "outfit0.acc01");
        CHECK(BoneGroup(s, static_cast<size_t>(arm)) == "Arms");
        CHECK(BoneGroup(s, static_cast<size_t>(hand)) == "Hands");
        CHECK(BoneGroup(s, static_cast<size_t>(eye)) == "Face: Eyes");
        CHECK(BoneGroup(s, static_cast<size_t>(brow)) == "Face: Brows");
        CHECK(BoneGroup(s, static_cast<size_t>(strand)) == "Accessories");
        (void)drv;
        auto frames = BoneFrames(s);
        CHECK(frames[0].first == "Root" && frames[0].second.size() == 1 && frames[0].second[0] == root);
        size_t listed = 0;
        std::set<int> seen;
        for (const auto& f : frames) for (int b : f.second) { ++listed; seen.insert(b); }
        CHECK(listed == s.bones.size() && seen.size() == s.bones.size());   // every bone in exactly one frame
        bool helpers = false;
        for (const auto& f : frames) if (f.first == "Helpers") helpers = std::find(f.second.begin(), f.second.end(), drv) != f.second.end();
        CHECK(helpers);
    });

    test("MMD names: the body bones answer to a motion's bone names, the hierarchy follows", [] {
        using namespace exporter;
        assemble::Scene s;
        auto add = [&](const char* name, int parent, float y = 0.0f) { s.bones.push_back({name, parent, {0, y, 0}, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        add("p_cf_body_bone", -1);
        const int cRoot = add("cf_t_root", 0), cHips = add("cf_t_hips", cRoot), cWaist = add("cf_t_waist_L", cHips);
        add("cf_j_root", 0);
        add("cf_n_height", 4);
        const int hips = add("cf_j_hips", 5, 12.5f), spine = add("cf_j_spine01", hips, 12.6f), arm = add("cf_j_arm00_L", spine, 15.0f);
        const int thumb = add("cf_j_thumb01_R", arm), foot = add("cf_j_foot_L", hips), toes = add("cf_j_toes_L", foot);
        const int headB = add("cf_j_head", spine, 16.0f), eyeL = add("cf_J_hitomi_tx_L", headB, 16.5f), eyeR = add("cf_J_hitomi_tx_R", headB, 16.5f);
        for (const char* nm : {"cf_Ohitomi_L", "cf_Ohitomi_R"}) {   // an eye white: points on a ball of radius 0.6 centred behind the pupil bone
            assemble::SceneMesh em;
            em.name = nm;
            const float cx = nm[11] == 'L' ? 1.0f : -1.0f;
            for (int i = 0; i < 60; ++i) {
                const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / 60.0f, r = std::sqrt(1.0f - z * z), a = 2.4f * static_cast<float>(i);
                assemble::SceneVertex sv;
                sv.pos = {cx + 0.6f * r * std::cos(a), 16.5f + 0.6f * r * std::sin(a), -0.5f + 0.6f * z};
                em.vertices.push_back(sv);
            }
            s.meshes.push_back(std::move(em));
        }
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.position = b.pos; pb.parent = b.parent; m.bones.push_back(pb); }
        pmx::Bone ik; ik.name = "cb_ik_foot_L"; ik.parent = 0; ik.ikTarget = foot; m.bones.push_back(ik);
        for (int i = 0; i < 12; ++i) { pmx::Vertex v; v.bone[0] = headB; m.vertices.push_back(v); }   // 0-2 white L, 3-5 iris L, 6-8 white R, 9-11 iris R
        for (int i = 0; i < 12; ++i) m.indices.push_back(i);
        // the left eye as the game builds it: a flat white cap in front of the pupil bone, the iris a patch just above it
        const Vec3 capL[6] = {{1.0f, 16.5f, 0.05f}, {1.2f, 16.5f, 0.05f}, {1.0f, 16.7f, -0.2f}, {1.0f, 16.5f, 0.06f}, {1.1f, 16.55f, 0.06f}, {0.95f, 16.45f, 0.05f}};
        for (int i = 0; i < 6; ++i) m.vertices[static_cast<size_t>(i)].position = capL[i];
        for (const char* nm : {"head/cf_Ohitomi_L#0", "head/cf_Ohitomi_L02#0", "head/cf_Ohitomi_R#0", "head/cf_Ohitomi_R02#0"}) { pmx::Material mt; mt.name = nm; mt.indexCount = 3; m.materials.push_back(mt); }
        { pmx::Morph blink; blink.name = "blink"; blink.vertices = {{0, {0, -1, 0}}, {1, {0, -1, 0}}, {2, {0, -1, 0}}}; m.morphs.push_back(blink); }   // the white's own shape
        const std::vector<int> added = ApplyMmdBoneNames(s, m, 1.0f).ik;
        auto find = [&](const char* n) { for (size_t i = 0; i < m.bones.size(); ++i) if (m.bones[i].name == n) return static_cast<int>(i); return -1; };
        CHECK(find("全ての親") == 0 && find("センター") == cRoot && find("グルーブ") == cHips && find("腰") == cWaist);
        CHECK(find("下半身") == hips && find("上半身") == spine && find("左腕") == arm && find("右親指０") == thumb && find("左足首") == foot && find("左つま先") == toes);
        CHECK(m.bones[static_cast<size_t>(hips)].parent == cWaist && m.bones[static_cast<size_t>(spine)].parent == cWaist);   // the halves turn apart
        CHECK(m.bones[static_cast<size_t>(spine)].parent < spine && m.bones[static_cast<size_t>(cWaist)].position.y == 12.5f);
        const int fik = find("左足ＩＫ");
        CHECK(fik >= 0 && m.bones[static_cast<size_t>(fik)].ikTarget == foot && added.size() == 1 && m.bones[static_cast<size_t>(added[0])].name == "左つま先ＩＫ");
        const int both = find("両目");
        CHECK(both >= 0 && m.bones[static_cast<size_t>(both)].parent == find("頭") && added.size() == 1);
        CHECK(find("左目") == eyeL && find("右目") == eyeR && m.bones[static_cast<size_t>(eyeL)].appendParent == both && m.bones[static_cast<size_t>(eyeR)].layer > m.bones[static_cast<size_t>(both)].layer);
        CHECK(std::fabs(m.bones[static_cast<size_t>(eyeL)].position.z + 0.5f) < 0.02f && m.bones[static_cast<size_t>(eyeL)].position.y == 16.5f);   // the pivot moved straight back to the ball's centre
        for (int i : {0, 1, 2}) CHECK(m.vertices[static_cast<size_t>(i)].bone[0] == eyeL && m.vertices[static_cast<size_t>(i)].weight[0] == 1.0f && m.vertices[static_cast<size_t>(i)].weight[1] == 0.0f);   // the whites turn with the eyeball
        for (int i : {6, 7, 8}) CHECK(m.vertices[static_cast<size_t>(i)].bone[0] == eyeR && m.vertices[static_cast<size_t>(i)].weight[0] == 1.0f);
        {   // the iris takes the white's mean blink offset
            int irisOffsets = 0;
            for (const auto& [vi, d] : m.morphs[0].vertices) if (vi >= 3 && vi <= 5 && d.y == -1.0f) ++irisOffsets;
            CHECK(irisOffsets == 3 && m.morphs[0].vertices.size() == 3 + 3);
            // the white and the iris keep the game's shape and place (laid on a smaller ball they shrank and sank into the socket skin)
            for (int i = 0; i < 6; ++i) {
                const Vec3 p = m.vertices[static_cast<size_t>(i)].position;
                CHECK(p.x == capL[i].x && p.y == capL[i].y && p.z == capL[i].z);
            }
        }
        for (int i : {3, 4, 5, 9, 10, 11}) CHECK(m.vertices[static_cast<size_t>(i)].bone[0] == headB);   // the iris patches (cf_Ohitomi_L02 / R02) are not the whites
        pmx::Model mo;   // morphs: the blink is a copy of the game's closed eyes, once
        pmx::Morph e; e.name = "Eyes 0 closed"; e.vertices = {{3, {0, 1, 0}}};
        mo.morphs.push_back(e);
        CHECK(AddMmdMorphs(mo).size() == 1 && mo.morphs[1].name == "まばたき" && mo.morphs[1].vertices.size() == 1 && AddMmdMorphs(mo).empty());
        pmx::Morph op; op.name = "Mouth 0 open"; op.vertices = {{0, {0, -0.2f, 0}}, {1, {0, -0.1f, 0}}};   // the vowels are made from the open mouth
        for (float x : {-1.0f, 1.0f}) { pmx::Vertex v; v.position = {x, 16.7f, 0}; mo.vertices.push_back(v); }
        mo.morphs.push_back(op);
        const std::vector<int> vowels = AddMmdMorphs(mo);
        CHECK(vowels.size() == 5 && mo.morphs[static_cast<size_t>(vowels[0])].name == "あ" && mo.morphs[static_cast<size_t>(vowels[4])].name == "お");
        const auto& morphU = mo.morphs[static_cast<size_t>(vowels[2])]; const auto& morphE = mo.morphs[static_cast<size_t>(vowels[3])];
        CHECK(morphU.vertices.size() == 2 && std::fabs(morphU.vertices[0].second.y) > std::fabs(morphE.vertices[0].second.y) * 0.0f);   // う: made from the open mouth, scaled only
        CHECK(std::fabs(morphE.vertices[0].second.y) > std::fabs(morphU.vertices[0].second.y) && morphE.vertices[0].second.x == 0.0f);   // え opens wider than う (1.2 x the open mouth vs 0.6 x), nothing reshaped sideways
        CHECK(AddMmdMorphs(mo).empty());
        // With the game's pattern names: each standard morph copies the pattern of its meaning, from its own panel (def_cl is a brow and an
        // eye pattern), and the winks are the closed eyes cut on the rig's left eye side (+x here; the model is not turned yet).
        pmx::Model g;
        for (float x : {-1.0f, -1.0f, 1.0f}) { pmx::Vertex v; v.position = {x, 16.7f, 0}; g.vertices.push_back(v); }
        pmx::Bone leftEye; leftEye.name = "cf_J_hitomi_tx_L"; leftEye.position = {1.0f, 16.8f, 0}; g.bones.push_back(leftEye);
        auto morph = [&](const char* n, int panel, std::vector<std::pair<int, Vec3>> v) { pmx::Morph m; m.name = n; m.panel = panel; m.vertices = std::move(v); g.morphs.push_back(m); };
        morph("Brow 0 closed", 1, {{1, {0, 0.5f, 0}}});
        morph("Eyes 0 closed", 2, {{0, {0, -1, 0}}, {2, {0, -1, 0}}});
        morph("Mouth 0 open", 3, {{0, {0, -0.1f, 0}}});
        morph("Mouth 26 open", 3, {{1, {0, -0.3f, 0.1f}}});
        const std::map<std::string, std::string> shapes = {{"Brow 0 closed", "def_cl"}, {"Eyes 0 closed", "def_cl"}, {"Mouth 0 open", "def_op"}, {"Mouth 26 open", "a_l_op"}};
        AddMmdMorphs(g, shapes);
        auto get = [&](const char* n) -> const pmx::Morph* { for (const auto& m : g.morphs) if (m.name == n) return &m; return nullptr; };
        const pmx::Morph* a = get("あ");
        CHECK(a && a->panel == 3 && a->vertices.size() == 1 && a->vertices[0].first == 1 && a->vertices[0].second.y == -0.3f);   // the game's あ, as it is
        const pmx::Morph* blink = get("まばたき");
        CHECK(blink && blink->panel == 2 && blink->vertices.size() == 2);   // the eyes' def_cl, not the brows'
        const pmx::Morph* winkL = get("ウィンク２"); const pmx::Morph* winkR = get("ｳｨﾝｸ２右");
        CHECK(winkL && winkR && winkL->vertices.size() == 1 && winkL->vertices[0].first == 2 && winkR->vertices.size() == 1 && winkR->vertices[0].first == 0);
        const pmx::Morph* i = get("い");
        CHECK(!get("ワ") && i && i->vertices.size() == 1 && i->vertices[0].first == 0);   // a pattern the head lacks: no morph, except a vowel (the plain open mouth, scaled)
    });

    test("pmx rotation: +Y turned onto an axis, in the yaw-pitch-roll order of rigid bodies", [] {
        // R = Ry(y) Rx(x) Rz(z) applied to +Y must give the axis
        auto apply = [](Vec3 e, Vec3 v) {
            auto rz = [](Vec3 p, float a) { return Vec3{p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a), p.z}; };
            auto rx = [](Vec3 p, float a) { return Vec3{p.x, p.y * std::cos(a) - p.z * std::sin(a), p.y * std::sin(a) + p.z * std::cos(a)}; };
            auto ry = [](Vec3 p, float a) { return Vec3{p.x * std::cos(a) + p.z * std::sin(a), p.y, -p.x * std::sin(a) + p.z * std::cos(a)}; };
            return ry(rx(rz(v, e.z), e.x), e.y);
        };
        for (Vec3 axis : {Vec3{1, 0, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0.3f, -0.8f, 0.5f}, Vec3{-0.6f, 0.1f, -0.7f}}) {
            Vec3 want = normalize(axis);
            Vec3 got = apply(exporter::PmxRotationFromYAxis(axis), {0, 1, 0});
            CHECK(std::fabs(got.x - want.x) < 1e-4f && std::fabs(got.y - want.y) < 1e-4f && std::fabs(got.z - want.z) < 1e-4f);
        }
    });

    test("pmx: a half turn about Y faces the model -Z, turns bodies and joints with it, mirrors IK limits and undoes itself", [] {
        pmx::Model m;
        pmx::Vertex v; v.position = {1, 2, 3}; v.normal = {0, 0, 1}; m.vertices.push_back(v);
        pmx::Bone b0; b0.name = "root"; b0.position = {0.5f, 0, -2}; m.bones.push_back(b0);
        pmx::Bone b1; b1.name = "knee"; b1.parent = 0; b1.position = {1, 1, 1}; b1.tailOffset = {0, 0, 4}; m.bones.push_back(b1);
        pmx::Bone ik; ik.name = "ik"; ik.ikTarget = 1; ik.ikLinks.push_back({1, true, {0.1f, -0.2f, -0.3f}, {2.0f, 0.4f, 0.5f}}); m.bones.push_back(ik);
        pmx::Morph mo; mo.name = "v"; mo.vertices.emplace_back(0, Vec3{1, 1, 1}); m.morphs.push_back(mo);
        pmx::RigidBody rb; rb.position = {3, 4, 5}; rb.rotation = {0.3f, 0.5f, -0.2f}; m.rigidBodies.push_back(rb);
        pmx::Joint j; j.position = {1, 1, 1}; j.rotation = {0, -1.0f, 0}; m.joints.push_back(j);
        const pmx::Model orig = m;
        pmx::TurnAboutY(m);
        CHECK(m.vertices[0].position.x == -1 && m.vertices[0].position.y == 2 && m.vertices[0].position.z == -3 && m.vertices[0].normal.z == -1);
        CHECK(m.bones[1].position.x == -1 && m.bones[1].position.z == -1 && m.bones[1].tailOffset.z == -4);
        const auto& lk = m.bones[2].ikLinks[0];   // about x and z the limits mirror, about y they stay
        CHECK(lk.lower.x == -2.0f && lk.upper.x == -0.1f && lk.lower.y == -0.2f && lk.upper.y == 0.4f && lk.lower.z == -0.5f && lk.upper.z == 0.3f);
        CHECK(m.morphs[0].vertices[0].second.x == -1 && m.morphs[0].vertices[0].second.y == 1 && m.morphs[0].vertices[0].second.z == -1);
        CHECK(m.rigidBodies[0].position.x == -3 && m.rigidBodies[0].position.z == -5 && std::fabs(m.rigidBodies[0].rotation.y - (0.5f - 3.14159265f)) < 1e-5f && m.rigidBodies[0].rotation.x == 0.3f);
        CHECK(std::fabs(m.joints[0].rotation.y - (-1.0f + 3.14159265f)) < 1e-5f);
        pmx::TurnAboutY(m);
        CHECK(std::fabs(m.vertices[0].position.x - 1) < 1e-6f && std::fabs(m.bones[1].position.z - 1) < 1e-6f && std::fabs(m.rigidBodies[0].rotation.y - 0.5f) < 1e-5f && std::fabs(m.joints[0].rotation.y - (-1.0f)) < 1e-5f);
        CHECK(m.bones[2].ikLinks[0].lower.x == orig.bones[2].ikLinks[0].lower.x && m.bones[2].ikLinks[0].upper.z == orig.bones[2].ikLinks[0].upper.z);
    });

    test("physics: a chain becomes a kinematic root, a capsule per bone and joints; colliders become kinematic bodies; read back from the bytes", [] {
        assemble::Scene s;
        s.bones.push_back({"head", -1, {0, 1.2f, 0}, "rig"});
        for (int i = 0; i < 4; ++i) s.bones.push_back({"b" + std::to_string(i), i == 0 ? 0 : i, {0, 1.1f - 0.1f * float(i), 0}, "rig"});
        assemble::SceneChain ch;
        ch.name = "strand";
        ch.bones = {1, 2, 3, 4};
        ch.stiffness = 0.2f;
        s.chains.push_back(ch);
        assemble::SceneCollider col;
        col.name = "head";
        col.bone = 0;
        col.center = {0, 1.2f, 0};
        col.radius = 0.1f;
        col.height = 0.4f;
        col.axis = {0, 1, 0};
        s.colliders.push_back(col);
        assemble::SceneCollider inside = col;
        inside.inside = true;
        s.colliders.push_back(inside);
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        exporter::AddPhysics(s, m, 10.0f);
        CHECK(m.rigidBodies.size() == 5);   // collider + one per chain bone (the inside collider is not exported)
        CHECK(m.joints.size() == 3);        // root-1, 1-2, 2-3
        CHECK(m.rigidBodies[0].mode == 0 && m.rigidBodies[0].group != exporter::kPmxGroupChain && m.rigidBodies[0].shape == 2);
        CHECK(m.rigidBodies[1].mode == 0 && m.rigidBodies[1].bone == 1 && m.rigidBodies[1].mask == 0);   // the root follows its bone and meets nothing
        CHECK(m.rigidBodies[2].mode == 1 && m.rigidBodies[3].mode == 1 && m.rigidBodies[3].group == 1);   // the rest are free
        CHECK(std::fabs(m.rigidBodies[2].position.y - 9.5f) < 1e-3f);   // halfway from bone 2 (y 1.0) to bone 3 (0.9), scaled by 10: 9.5
        for (const auto& j : m.joints) CHECK(j.bodyA >= 0 && j.bodyB >= 0 && j.bodyA != j.bodyB);
        m.vertices.resize(3);
        m.indices = {0, 1, 2};
        pmx::Material mat; mat.name = "m"; mat.indexCount = 3; m.materials.push_back(mat);
        pmx::Morph vm; vm.name = "v"; vm.vertices.emplace_back(2, Vec3{0, 1, 0}); m.morphs.push_back(vm);
        const pmxread::Model back = readPmx(pmx::WritePmx(m));   // consistent enough to serialise, and read again by a second reader
        CHECK(back.bodies.size() == m.rigidBodies.size() && back.joints.size() == m.joints.size() && back.bones.size() == m.bones.size());
        const auto findings = pmxPhysicsFindings(back);
        for (const auto& f : findings) std::printf("  physics finding: %s\n", f.c_str());
        CHECK(findings.empty());
        for (size_t i = 0; i < m.rigidBodies.size(); ++i) {   // numbers come back as written
            CHECK(back.bodies[i].group == m.rigidBodies[i].group && back.bodies[i].mask == m.rigidBodies[i].mask && back.bodies[i].mode == m.rigidBodies[i].mode);
            CHECK(std::fabs(back.bodies[i].pos.y - m.rigidBodies[i].position.y) < 1e-5f && std::fabs(back.bodies[i].size.x - m.rigidBodies[i].size.x) < 1e-5f);
        }
        // the filter: chains meet colliders (bit 0), colliders meet chains (bit 1), the root meets nothing
        CHECK(back.bodies[0].mask == 0x0002 && back.bodies[1].mask == 0 && (back.bodies[2].mask & 0x0002) == 0);
        // a broken file is found: a chain that meets chains, an anchor that moves, a joint on a missing body
        pmxread::Model broken = back;
        broken.bodies[3].mask = 0xFFFF;
        broken.bodies[1].mode = 1;
        broken.joints[0].b = 99;
        CHECK(pmxPhysicsFindings(broken).size() >= 3);
    });

    test("physics: the distribution curves reach the joints (stiff where a ponytail is tied, free toward its tip)", [] {
        assemble::Scene s;
        s.bones.push_back({"head", -1, {0, 1.2f, 0}, "rig"});
        for (int i = 0; i < 5; ++i) s.bones.push_back({"b" + std::to_string(i), i == 0 ? 0 : i, {0, 1.1f - 0.2f * float(i), 0}, "rig"});
        assemble::SceneChain ch;
        ch.name = "pony";
        ch.bones = {1, 2, 3, 4, 5};
        ch.stiffness = 1.0f;   // the component's own value: a chain of stiffness 1 does not move at all
        ch.boneStiffness = {1.0f, 1.0f, 0.5f, 0.2f, 0.0f};   // ... but its curve lets the tip go
        s.chains.push_back(ch);
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        exporter::AddPhysics(s, m, 10.0f);
        CHECK(m.joints.size() == 4);
        // joint k hangs bone k + 1 (the chain's root has none): the stiff bone barely turns, the looser ones turn freely
        CHECK(m.joints[0].rotUpper.z <= 0.06f);
        CHECK(m.joints[1].rotUpper.z > 0.5f && m.joints[2].rotUpper.z > 0.5f && m.joints[3].rotUpper.z > 0.5f);
        // without the per-bone values the whole chain is as stiff as the component says
        s.chains[0].boneStiffness.clear();
        pmx::Model flat;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; flat.bones.push_back(pb); }
        exporter::AddPhysics(s, flat, 10.0f);
        for (const auto& j : flat.joints) CHECK(j.rotUpper.z <= 0.06f);
    });

    test("physics: a stub node pointing back up the chain gets no body, its parent carries on downward", [] {
        assemble::Scene s;
        s.bones.push_back({"hip", -1, {0, 1.3f, 0}, "rig"});
        for (int i = 0; i < 4; ++i) s.bones.push_back({"s" + std::to_string(i), i == 0 ? 0 : i, {0, 1.2f - 0.2f * float(i), 0}, "rig"});
        s.bones.push_back({"s3_end", 4, {0, 0.7f, 0}, "item"});   // above its parent (0.6), as the skirts' end nodes are
        assemble::SceneChain ch;
        ch.name = "skirt";
        ch.bones = {1, 2, 3, 4, 5};
        ch.stiffness = 0.5f;
        s.chains.push_back(ch);
        CHECK(exporter::PhysicsChainBones(s, ch).size() == 4);
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        exporter::AddPhysics(s, m, 10.0f);
        CHECK(m.rigidBodies.size() == 4 && m.joints.size() == 3);
        CHECK(m.rigidBodies[3].position.y < 6.0f);   // the last link hangs below its bone (y 0.6 -> 6), not above it
        s.bones[5].pos = {0, 0.5f, 0};   // a real continuation stays
        CHECK(exporter::PhysicsChainBones(s, ch).size() == 5);
    });

    test("physics: the body's own jiggle chains become bodies only when their switch is on (bust and butt both on by default)", [] {
        auto make = [](bool bustOwn) {
            assemble::Scene s;
            s.bones.push_back({"cf_j_chest01", -1, {0, 1.2f, 0}, "rig"});
            s.bones.push_back({"cf_j_chest02", 0, {0, 1.15f, 0.05f}, "rig"});
            s.bones.push_back({bustOwn ? "cf_s_bnip02_L" : "hair_a", 1, {0, 1.05f, 0.1f}, "rig"});
            s.bones.push_back({bustOwn ? "cf_s_bnip02_L2" : "hair_b", 2, {0, 0.95f, 0.12f}, "rig"});
            assemble::SceneChain ch;
            ch.name = bustOwn ? "bust" : "hair";
            ch.bones = {2, 3};
            ch.characterOwn = bustOwn;
            ch.damping = 0.1f; ch.elasticity = 0.1f; ch.stiffness = 0.1f;
            s.chains.push_back(ch);
            return s;
        };
        auto count = [](const assemble::Scene& s, bool bust, bool butt, float intensity) {
            pmx::Model m;
            for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
            exporter::AddPhysics(s, m, 10.0f, bust, butt, intensity);
            return m;
        };
        {   // the default export: breast jiggle on, the butt off (explicit args; both defaults are on)
            pmx::Model m = count(make(true), true, false, 1.0f);
            CHECK(m.rigidBodies.size() == 3 && m.joints.size() == 2);   // anchor body + the jiggle chain, and the anchor joint + the link joint
            CHECK(m.rigidBodies[0].mode == 0 && m.rigidBodies[0].bone == 1);   // the anchor: kinematic, on the parent (chest) bone
            CHECK(m.joints[0].bodyA == 0 && m.joints[0].bodyB == 1 && m.joints[0].rotUpper.x == 0.0f && m.joints[0].springRot.x == 0.0f);   // rigid
            pmx::Model off = count(make(true), false, false, 1.0f);
            CHECK(off.rigidBodies.empty() && off.joints.empty());      // boobs off: the part stays rigid
            pmx::Model hair = count(make(false), false, false, 1.0f);
            CHECK(hair.rigidBodies.size() == 2);                       // hair is not the body's own: always simulated
            pmx::Model jig = count(make(true), true, false, 1.0f);
            pmx::Model plain = count(make(false), true, false, 1.0f);
            // the reference models' breast bodies: mode 2 (the body follows its bone with physics on top), heavy fixed damping, no
            // friction, no collider, no joint spring (the rotation limits are the jiggle), weight tapering toward the tip
            CHECK(jig.rigidBodies[1].mode == 2 && jig.rigidBodies[2].mode == 2);
            CHECK(jig.rigidBodies[2].moveDamping == 0.9f && jig.rigidBodies[2].rotDamping == 0.9f && jig.rigidBodies[2].friction == 0.0f);
            CHECK(jig.rigidBodies[2].mask == 0);
            CHECK(jig.rigidBodies[1].mass == 1.0f && jig.rigidBodies[2].mass == 0.5f);   // 1, 1/2, ...
            CHECK(jig.joints.size() == 2 && jig.joints[1].springRot.x > 0.0f && jig.joints[1].rotUpper.x > 0.0f && jig.joints[1].rotUpper.x <= 0.60f);
            CHECK(jig.joints[1].rotUpper.y == 0.6f * jig.joints[1].rotUpper.x);   // the reference models' narrow axes: sway 0.6, twist 0.4
            CHECK(jig.joints[1].rotUpper.z == 0.4f * jig.joints[1].rotUpper.x);
            pmx::Model loud = count(make(true), true, false, 2.0f);
            CHECK(loud.joints[1].rotUpper.x > jig.joints[1].rotUpper.x);   // the intensity opens the envelope
            CHECK(plain.rigidBodies[1].mode == 1 && plain.rigidBodies[1].friction == 0.5f);   // hair keeps the strand conversion
            CHECK((plain.rigidBodies[1].mask & 0x0001) != 0);   // a hair strand still meets the colliders (group 0 here: this scene has none)
        }
        {   // the butt chain rides the butt switch
            assemble::Scene s;
            s.bones.push_back({"hips", -1, {0, 1.0f, 0}, "rig"});
            s.bones.push_back({"cf_s_siri_L", 0, {0.05f, 0.95f, -0.05f}, "rig"});
            s.bones.push_back({"cf_s_siri_L2", 1, {0.05f, 0.85f, -0.08f}, "rig"});
            assemble::SceneChain ch;
            ch.name = "butt";
            ch.bones = {1, 2};
            ch.characterOwn = true;
            s.chains.push_back(ch);
            CHECK(count(s, true, false, 1.0f).rigidBodies.empty());   // default off
            CHECK(count(s, true, true, 1.0f).rigidBodies.size() == 3); // on: anchor + chain bodies
            CHECK(count(s, false, true, 1.0f).rigidBodies.size() == 3); // independent of the boobs switch
            const pmx::Model butt = count(s, true, true, 1.0f);
            CHECK(butt.joints[1].rotUpper.x > 0.0f && butt.joints[1].rotUpper.x <= 0.30f);   // the butt's envelope is smaller than the bust's
        }
    });

    test("physics: garments hanging in front of the thighs turn with the legs, after them; the sides, the back and the hair do not", [] {
        assemble::Scene s;
        s.bones.push_back({"hips", -1, {0, 1.0f, 0}, "rig"});
        for (int side = 0; side < 2; ++side) {   // the game's leg bones; the toes point to +Z, so that is the front
            const std::string n = side == 0 ? "L" : "R";
            const float x = side == 0 ? 0.1f : -0.1f;
            const int t = static_cast<int>(s.bones.size());
            s.bones.push_back({"cf_j_thigh00_" + n, 0, {x, 0.9f, 0}, "rig"});
            s.bones.push_back({"cf_j_leg01_" + n, t, {x, 0.5f, 0}, "rig"});
            s.bones.push_back({"cf_j_foot_" + n, t + 1, {x, 0.1f, 0}, "rig"});
            s.bones.push_back({"cf_j_toes_" + n, t + 2, {x, 0.05f, 0.1f}, "rig"});
        }
        auto addChain = [&](const std::string& name, int parent, Vec3 top) {
            assemble::SceneChain ch;
            ch.name = name;
            for (int i = 0; i < 4; ++i) {
                ch.bones.push_back(static_cast<int>(s.bones.size()));
                s.bones.push_back({name + std::to_string(i), i == 0 ? parent : ch.bones[static_cast<size_t>(i) - 1], {top.x, top.y - 0.15f * float(i), top.z}, "item"});
            }
            s.chains.push_back(ch);
            return ch.bones[0];
        };
        const int frontLeft = addChain("frontL", 0, {0.1f, 1.0f, 0.15f});
        const int centreTop = static_cast<int>(s.bones.size());
        s.bones.push_back({"centre_top", 0, {0, 1.0f, 0.15f}, "item"});   // carries the centre chain alone
        const int centre = addChain("centre", centreTop, {0, 1.0f, 0.15f});
        const int side = addChain("side", 0, {0.25f, 1.0f, 0});
        const int back = addChain("back", 0, {0.1f, 1.0f, -0.15f});
        const int hair = addChain("hair", 0, {0, 1.5f, -0.1f});
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        exporter::AddPhysics(s, m, 10.0f);
        const auto& fl = m.bones[static_cast<size_t>(frontLeft)];
        CHECK(fl.appendParent == 1 && fl.appendRatio > 0.7f && fl.appendRatio <= 0.75f && fl.layer == 1);
        CHECK(m.bones[static_cast<size_t>(frontLeft) + 3].layer == 1);   // what hangs from it moves after it too
        const auto& c = m.bones[static_cast<size_t>(centre)];
        const auto& ct = m.bones[static_cast<size_t>(centreTop)];
        CHECK(c.appendParent >= 0 && ct.appendParent >= 0 && c.appendParent != ct.appendParent);   // one leg each
        CHECK(c.appendRatio > 0.35f && c.appendRatio < 0.5f && std::fabs(c.appendRatio - ct.appendRatio) < 1e-4f && ct.layer == 1);
        for (int b : {side, back, hair}) CHECK(m.bones[static_cast<size_t>(b)].appendParent < 0 && m.bones[static_cast<size_t>(b)].layer == 0);
        CHECK(m.bones[1].layer == 0 && m.bones[1].appendParent < 0);   // the legs themselves are untouched
    });

    test("physics: the exported spring numbers settle like the DynamicBone sim and do not explode", [] {
        using namespace assemble;
        struct Setup { const char* what; float damping, elasticity, stiffness; };
        for (const Setup& su : {Setup{"default hair", 0.1f, 0.1f, 0.1f}, Setup{"loose", 0.06f, 0.03f, 0.0f}, Setup{"stiff", 0.2f, 0.3f, 0.6f}}) {
            Scene s;
            s.bones.push_back({"head", -1, {0, 1.2f, 0}, "rig"});
            for (int i = 0; i < 4; ++i) s.bones.push_back({"b" + std::to_string(i), i == 0 ? 0 : i, {0, 1.1f - 0.1f * float(i), 0}, "rig"});
            SceneChain ch;
            ch.name = "strand";
            ch.bones = {1, 2, 3, 4};
            ch.damping = su.damping; ch.elasticity = su.elasticity; ch.stiffness = su.stiffness;
            s.chains.push_back(ch);
            const float scale = 12.5f;
            pmx::Model m;
            for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * scale; m.bones.push_back(pb); }
            exporter::AddPhysics(s, m, scale);
            m.vertices.resize(1);
            const pmxread::Model back = readPmx(pmx::WritePmx(m));
            CHECK(pmxPhysicsFindings(back).empty());

            // the head moves 0.3 m to the side over a tenth of a second; both sims follow the tip's lag behind it
            Pose pose;
            std::vector<Mat4> pw = BoneWorlds(s, pose);
            PhysicsSim sim;
            sim.Reset(s, pw);
            std::vector<Mat4> skin(s.bones.size());
            springref::Chain ref(back, 9.8 * scale);
            std::vector<double> lagSim, lagRef;
            double peakSim = 0, peakRef = 0, maxAngle = 0;
            for (int f = 0; f < 600; ++f) {
                const float x = 0.3f * std::min(1.0f, float(f + 1) / 6.0f);
                pose.edit(0).pos = {x, 0, 0};
                pw = BoneWorlds(s, pose);
                sim.Step(s, pw, 1, skin);
                ref.moveAnchors({x * scale, 0});
                ref.step();
                lagSim.push_back(x - skin[4].point(s.bones[4].pos).x);
                lagRef.push_back(x - ref.jointPoint(ref.pinCount() - 1).x / scale);
                peakSim = std::max(peakSim, std::fabs(lagSim.back()));
                peakRef = std::max(peakRef, std::fabs(lagRef.back()));
                for (size_t k = 0; k < ref.pinCount(); ++k) maxAngle = std::max(maxAngle, std::fabs(ref.relativeAngle(k)));
            }
            auto settledAt = [](const std::vector<double>& lag, double band) {
                int last = 0;
                for (size_t i = 0; i < lag.size(); ++i) if (std::fabs(lag[i]) > band) last = static_cast<int>(i);
                return last;
            };
            const int tSim = settledAt(lagSim, 0.015), tRef = settledAt(lagRef, 0.015);
            std::printf("  %-12s peak lag sim %.3f m ref %.3f m; within 1.5 cm for good after %.2f s (sim) %.2f s (ref); final lag ref %.4f m; speed %.4f; largest joint angle %.2f rad\n", su.what, peakSim, peakRef, tSim / 60.0, tRef / 60.0, lagRef.back(), ref.speed(), maxAngle);
            CHECK(peakRef < 0.45);                                  // never flung past the head's own move
            CHECK(std::fabs(lagRef.back()) < 0.01 && ref.speed() < 0.5);   // hangs straight under the anchor again and is at rest
            CHECK(tRef <= 420);                                     // and does so within 7 s
            CHECK(tRef <= tSim * 3 + 60);                           // in the same time as the sim, give or take a factor
            bool anyLocked = false;   // a joint gravity would push onto its stop is written locked (zero range): the planar reference's soft stop cannot model that
            for (const auto& j : back.joints) anyLocked = anyLocked || j.rotUpper.z == 0.0f;
            CHECK(anyLocked || maxAngle < 2.5);   // the reference's stop only removes the velocity into the 0.6 rad limit, so it leaks: this only catches an explosion                                  // at most a little past the widest limit (1.5 rad; the reference stop is soft)
        }

        {   // a strand that stands out sideways droops under gravity, but only as far as its stiffness allows
            Scene s;
            s.bones.push_back({"head", -1, {0, 1.2f, 0}, "rig"});
            for (int i = 0; i < 4; ++i) s.bones.push_back({"b" + std::to_string(i), i == 0 ? 0 : i, {0.1f * float(i), 1.2f, 0}, "rig"});
            SceneChain ch;
            ch.name = "ribbon";
            ch.bones = {1, 2, 3, 4};
            ch.damping = 0.1f; ch.elasticity = 0.03f; ch.stiffness = 0.0f;   // as loose as it gets
            s.chains.push_back(ch);
            const float scale = 12.5f;
            pmx::Model m;
            for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * scale; m.bones.push_back(pb); }
            exporter::AddPhysics(s, m, scale);
            m.vertices.resize(1);
            const pmxread::Model back = readPmx(pmx::WritePmx(m));
            CHECK(pmxPhysicsFindings(back).empty());
            springref::Chain ref(back, 9.8 * scale);
            for (int f = 0; f < 900; ++f) ref.step();
            const double tipDrop = (back.bodies[0].pos.y - ref.jointPoint(ref.pinCount() - 1).y) / scale;   // below where the anchor stands, metres
            std::printf("  ribbon droop: tip %.3f m below its rest height over a strand of 0.3 m, speed %.4f\n", tipDrop, ref.speed());
            bool locked = true;   // as loose as it gets, standing out sideways: gravity beats the spring, so every joint is written locked
            for (const auto& j : back.joints) locked = locked && j.rotUpper.z == 0.0f && j.rotLower.z == 0.0f;
            CHECK(locked || (tipDrop > 0.0 && tipDrop < 0.15 && ref.speed() < 0.05));
        }

        {   // the reference solver itself: one link on a spring rings at sqrt(K / I) and loses energy at the damping it was given
            pmxread::Model one;
            pmxread::RigidBody anchor, link;
            anchor.mode = 0; anchor.size = {0.1f, 0, 0}; anchor.pos = {0, 0, 0};
            link.mode = 1; link.shape = 2; link.mass = 2.0f; link.size = {0.1f, 1.8f, 0}; link.pos = {0, -1.0f, 0};
            link.moveDamping = link.rotDamping = 0.0f;
            one.bodies = {anchor, link};
            pmxread::Joint j;
            j.a = 0; j.b = 1; j.pos = {0, 0, 0}; j.springRot = {50, 50, 50}; j.rotLower = {-3, -3, -3}; j.rotUpper = {3, 3, 3};
            one.joints = {j};
            springref::Chain pend(one, 0.0);   // no gravity: a pure torsional pendulum
            // inertia about the pivot: Bullet's box about the centre plus m r^2
            const double lx = 0.2, ly = 2.0, inertia = 2.0 / 12.0 * (ly * ly + lx * lx) + 2.0 * 1.0;
            const double period = 6.283185307 * std::sqrt(inertia / 50.0);
            // give it a push: move the anchor sideways a little and back
            pend.moveAnchors({0.2, 0});
            pend.step();
            pend.moveAnchors({0, 0});
            double prev = pend.relativeAngle(0), t = 1.0 / 60.0;
            std::vector<double> crossings;
            for (int f = 0; f < 600; ++f) {
                pend.step();
                t += 1.0 / 60.0;
                const double a = pend.relativeAngle(0);
                if (prev < 0 && a >= 0) crossings.push_back(t);
                prev = a;
            }
            CHECK(crossings.size() >= 3);
            if (crossings.size() >= 3) {
                const double measured = (crossings.back() - crossings.front()) / double(crossings.size() - 1);
                std::printf("  reference pendulum: period %.3f s measured, %.3f s from sqrt(K / I)\n", measured, period);
                CHECK(std::fabs(measured - period) < 0.06 * period);
            }
        }
    });

    test("pmx writer: IK bones come back through the independent reader", [] {
        pmx::Model m;
        m.name = "t";
        for (int i = 0; i < 4; ++i) { pmx::Bone b; b.name = "b" + std::to_string(i); b.parent = i - 1; b.position = {0, float(3 - i), 0}; m.bones.push_back(b); }
        pmx::Bone ik;
        ik.name = "ik";
        ik.parent = 0;
        ik.position = {0, 0, 0};
        ik.ikTarget = 3;
        ik.ikLoop = 25;
        ik.ikLimit = 0.75f;
        pmx::IkLink knee; knee.bone = 2; knee.limited = true; knee.lower = {0.01f, 0, 0}; knee.upper = {2.5f, 0, 0};
        pmx::IkLink hip; hip.bone = 1;
        ik.ikLinks = {knee, hip};
        m.bones.push_back(ik);
        pmx::Bone after; after.name = "after"; after.parent = 4; m.bones.push_back(after);   // a bone after the IK one still reads right
        for (int i = 0; i < 3; ++i) { pmx::Vertex v; v.position = {float(i), 0, 0}; m.vertices.push_back(v); }
        m.indices = {0, 1, 2};
        pmx::Material mat; mat.name = "m"; mat.indexCount = 3; m.materials.push_back(mat);
        const pmxread::Model back = readPmx(pmx::WritePmx(m));
        CHECK(back.bones.size() == 6);
        CHECK(!back.bones[3].isIk() && back.bones[4].isIk() && !back.bones[5].isIk());
        CHECK((back.bones[4].flags & 0x0020) != 0 && (back.bones[4].flags & 0x001E) == 0x001E);
        CHECK(back.bones[4].ikTarget == 3 && back.bones[4].ikLoop == 25 && std::fabs(back.bones[4].ikLimit - 0.75f) < 1e-6f && back.bones[4].ikLinks.size() == 2);
        CHECK(back.bones[4].ikLinks[0].bone == 2 && back.bones[4].ikLinks[0].limited && std::fabs(back.bones[4].ikLinks[0].upper.x - 2.5f) < 1e-6f && std::fabs(back.bones[4].ikLinks[0].lower.x - 0.01f) < 1e-6f);
        CHECK(back.bones[4].ikLinks[1].bone == 1 && !back.bones[4].ikLinks[1].limited);
        CHECK(back.bones[5].parent == 4 && back.bones[5].name == "after");
        pmx::Model bad = m;
        bad.bones[4].ikLinks[0].bone = 9;
        bool threw = false;
        try { pmx::WritePmx(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
        bad = m;
        bad.bones[4].ikLinks.clear();   // an IK bone has to have links to turn
        threw = false;
        try { pmx::WritePmx(bad); } catch (const FormatError&) { threw = true; }
        CHECK(threw);
    });

    test("IK: limbs are found by role, the handle stands on the end bone, only a bent limb gets a bend limit", [] {
        using namespace assemble;
        Scene s;
        auto add = [&](const std::string& name, int parent, Vec3 pos) { s.bones.push_back({name, parent, pos, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        const int root = add("p_cf_body_bone", -1, {0, 0, 0});
        // a leg bent a little at the knee, in the Y-Z plane, the way the knee bends: the lower leg goes back
        const int thigh = add("cf_j_thigh00_L", root, {0.09f, 1.07f, -0.02f});
        const int knee = add("cf_j_leg01_L", thigh, {0.09f, 0.57f, -0.03f});
        const int ankle = add("cf_j_leg03_L", knee, {0.09f, 0.10f, -0.05f});
        // an arm held straight along x: no bend direction, no limit
        const int arm = add("cf_j_arm00_R", root, {-0.2f, 1.5f, 0});
        const int fore = add("cf_j_forearm01_R", arm, {-0.5f, 1.5f, 0});
        const int hand = add("cf_j_hand_R", fore, {-0.8f, 1.5f, 0});
        // a left arm whose bones are not a parent chain: left out
        add("cf_j_arm00_L", root, {0.2f, 1.5f, 0});
        add("cf_j_forearm01_L", root, {0.5f, 1.5f, 0});
        add("cf_j_hand_L", root, {0.8f, 1.5f, 0});
        const auto limbs = exporter::FindLimbs(s);
        CHECK(limbs.size() == 2);
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        const size_t before = m.bones.size();
        const std::vector<int> ik = exporter::AddIk(s, m, 10.0f);
        CHECK(ik.size() == 2 && m.bones.size() == before + 2);
        for (int i : ik) {
            const pmx::Bone& b = m.bones[static_cast<size_t>(i)];
            CHECK(b.parent == root && b.ikTarget >= 0 && b.ikLinks.size() == 2 && b.ikLoop > 0 && b.ikLimit > 0);
            CHECK(b.ikLinks[0].bone == m.bones[static_cast<size_t>(b.ikTarget)].parent && b.ikLinks[1].bone == m.bones[static_cast<size_t>(b.ikLinks[0].bone)].parent);   // nearest the target first
            const Vec3 d{b.position.x - m.bones[static_cast<size_t>(b.ikTarget)].position.x, b.position.y - m.bones[static_cast<size_t>(b.ikTarget)].position.y, b.position.z - m.bones[static_cast<size_t>(b.ikTarget)].position.z};
            CHECK(std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) < 1e-4f);   // the handle stands on the end bone
        }
        const pmx::Bone* legIk = nullptr;
        const pmx::Bone* armIk = nullptr;
        for (int i : ik) { if (m.bones[static_cast<size_t>(i)].ikTarget == ankle) legIk = &m.bones[static_cast<size_t>(i)]; if (m.bones[static_cast<size_t>(i)].ikTarget == hand) armIk = &m.bones[static_cast<size_t>(i)]; }
        CHECK(legIk && armIk);
        if (legIk && armIk) {
            CHECK(legIk->ikLinks[0].bone == knee && legIk->ikLinks[1].bone == thigh);
            CHECK(armIk->ikLinks[0].bone == fore && armIk->ikLinks[1].bone == arm);
            // the leg bends about +x (the lower leg goes toward -z): the knee may flex between 0.5 and 150 degrees about x, the other axes are locked
            CHECK(legIk->ikLinks[0].limited && std::fabs(legIk->ikLinks[0].lower.x - 0.0087f) < 1e-5f && std::fabs(legIk->ikLinks[0].upper.x - 2.62f) < 1e-5f);
            CHECK(legIk->ikLinks[0].lower.y == 0 && legIk->ikLinks[0].upper.y == 0 && legIk->ikLinks[0].lower.z == 0 && legIk->ikLinks[0].upper.z == 0);
            CHECK(!legIk->ikLinks[1].limited);
            CHECK(!armIk->ikLinks[0].limited);   // straight: nothing says which way it bends
        }
        // the sign: flexing the knee by a positive angle about +x moves the ankle toward -z, the way the leg is already bent
        {
            const Vec3 u{0, 0.57f - 1.07f, -0.01f}, l{0, 0.10f - 0.57f, -0.02f};
            const float c = 0.3f;   // 0.3 rad about +x
            const Vec3 rl{l.x, l.y * std::cos(c) - l.z * std::sin(c), l.y * std::sin(c) + l.z * std::cos(c)};
            CHECK(rl.z < l.z);
            Vec3 lo, hi;
            CHECK(exporter::BendLimits({0, 1.07f, -0.02f}, {0, 0.57f, -0.03f}, {0, 0.10f, -0.05f}, lo, hi) && lo.x > 0 && hi.x > lo.x);
            (void)u;
            // bent the other way (the lower leg goes toward +z) the limit is on the negative side of x
            CHECK(exporter::BendLimits({0, 1.07f, -0.02f}, {0, 0.57f, 0.03f}, {0, 0.10f, 0.20f}, lo, hi) && hi.x < 0 && lo.x < hi.x);
            // a bend about a diagonal axis cannot be written as a per-axis limit: left free
            CHECK(!exporter::BendLimits({0, 0, 0}, {1, 0, 0}, {1.5f, 0.5f, 0.5f}, lo, hi));
        }
        {   // arms=false (the MMD names): the legs only
            pmx::Model legs = m;
            legs.bones.resize(before);
            const std::vector<int> legIks = exporter::AddIk(s, legs, 10.0f, false);
            CHECK(legIks.size() == 1 && legs.bones[static_cast<size_t>(legIks[0])].ikTarget == ankle);
        }
        Scene none;
        none.bones.push_back({"root", -1, {}, "rig"});
        pmx::Model nm;
        nm.bones.push_back({});
        CHECK(exporter::AddIk(none, nm, 10.0f).empty() && nm.bones.size() == 1);   // a rig without limbs gets none
    });

    test("pose: skin matrices, baking, automatic A-pose", [] {
        using namespace assemble;
        Scene s;
        auto add = [&](const char* name, int parent, Vec3 pos) { s.bones.push_back({name, parent, pos, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        const int hips = add("cf_j_hips", -1, {0, 1, 0});
        const int sh = add("cf_j_shoulder_L", hips, {0.1f, 1.4f, 0});
        const int arm = add("cf_j_arm00_L", sh, {0.2f, 1.4f, 0});
        const int fore = add("cf_j_forearm01_L", arm, {0.5f, 1.4f, 0});
        const int shR = add("cf_j_shoulder_R", hips, {-0.1f, 1.4f, 0});
        const int armR = add("cf_j_arm00_R", shR, {-0.2f, 1.4f, 0});
        const int foreR = add("cf_j_forearm01_R", armR, {-0.5f, 1.4f, 0});
        SceneMesh mesh;
        SceneVertex v;
        v.pos = {0.35f, 1.4f, 0}; v.normal = {0, 1, 0}; v.bone[0] = arm; v.weight[0] = 1;   // on the upper arm
        mesh.vertices.push_back(v);
        SceneVertex w = v;   // half arm, half forearm
        w.bone[0] = arm; w.bone[1] = fore; w.weight[0] = 0.5f; w.weight[1] = 0.5f;
        mesh.vertices.push_back(w);
        SceneBlendShape bs; bs.name = "x"; bs.offsets = {{0, {0, 1, 0}}};
        mesh.shapes.push_back(bs);
        s.meshes.push_back(mesh);
        SceneCollider col; col.bone = arm; col.center = {0.35f, 1.4f, 0}; col.axis = {1, 0, 0}; col.radius = 0.05f;
        s.colliders.push_back(col);

        Pose none;
        for (const Mat4& m : SkinMatrices(s, none)) {   // rest pose: identity everywhere
            for (int i = 0; i < 16; ++i) CHECK(std::fabs(m.m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-5f);
        }
        // a bone turned 90 degrees about z at its own position: the point 0.15 along +x from it ends 0.15 along +y
        Pose p;
        p.edit(static_cast<size_t>(arm)).rot = {0, 0, 90};
        auto skin = SkinMatrices(s, p);
        Vec3 moved = skin[static_cast<size_t>(arm)].point({0.35f, 1.4f, 0});
        CHECK(std::fabs(moved.x - 0.2f) < 1e-4f && std::fabs(moved.y - 1.55f) < 1e-4f);
        Vec3 child = BoneWorlds(s, p)[static_cast<size_t>(fore)].translation();   // the forearm bone swings with it
        CHECK(std::fabs(child.x - 0.2f) < 1e-4f && std::fabs(child.y - 1.7f) < 1e-4f);
        // a local move and a scale
        Pose q;
        q.edit(static_cast<size_t>(sh)).pos = {0, 0.05f, 0};
        q.edit(static_cast<size_t>(arm)).scl = {2, 2, 2};
        auto world = BoneWorlds(s, q);
        CHECK(std::fabs(world[static_cast<size_t>(arm)].translation().y - 1.45f) < 1e-4f);   // the shoulder carries the arm up
        CHECK(std::fabs(world[static_cast<size_t>(fore)].translation().x - 0.8f) < 1e-4f);   // the arm's scale doubles the offset of its child (0.3 -> 0.6)

        // A-pose: the left and right arms come down, mirrored, and the forearm keeps its distance from the arm
        std::vector<Quat> base;
        CHECK(MakeAPose(s, 40.0f, base));
        Pose a;
        a.base = base;
        auto aw = BoneWorlds(s, a);
        Vec3 f = aw[static_cast<size_t>(fore)].translation(), a0 = aw[static_cast<size_t>(arm)].translation();
        Vec3 fr = aw[static_cast<size_t>(foreR)].translation();
        const float drop = std::atan2(a0.y - f.y, std::fabs(f.x - a0.x)) * 180.0f / 3.14159265f;   // below horizontal, from the arm joint
        CHECK(std::fabs(drop - 40.0f) < 0.5f);   // clavicle 20 % and arm 80 % turn about the same axis: 40 degrees below the horizontal in all
        CHECK(std::fabs(std::sqrt((f.x - a0.x) * (f.x - a0.x) + (f.y - a0.y) * (f.y - a0.y)) - 0.3f) < 1e-3f);
        CHECK(std::fabs(fr.y - f.y) < 1e-4f && std::fabs(fr.x + f.x) < 1e-4f);   // mirrored
        Scene noArms;
        noArms.bones.push_back({"root", -1, {}, "rig"});
        CHECK(!MakeAPose(noArms, 40.0f, base) && base.empty());

        // baking: vertices, normals, bone positions, colliders and blend shape offsets follow
        Scene b = s;
        BakePose(b, p);
        CHECK(std::fabs(b.meshes[0].vertices[0].pos.x - 0.2f) < 1e-4f && std::fabs(b.meshes[0].vertices[0].pos.y - 1.55f) < 1e-4f);
        CHECK(std::fabs(b.meshes[0].vertices[0].normal.x + 1.0f) < 1e-4f);   // +y turned to -x
        CHECK(std::fabs(b.meshes[0].shapes[0].offsets[0].second.x + 1.0f) < 1e-4f);
        CHECK(std::fabs(b.bones[static_cast<size_t>(fore)].pos.y - 1.7f) < 1e-4f);
        CHECK(std::fabs(b.colliders[0].center.y - 1.55f) < 1e-4f && std::fabs(b.colliders[0].axis.y - 1.0f) < 1e-4f);
        CHECK(b.pose.size() == 1 && b.pose[0].bone == "cf_j_arm00_L" && !b.aposeApplied);
        Scene c = s;
        BakePose(c, none);   // nothing to bake
        CHECK(c.pose.empty() && std::fabs(c.meshes[0].vertices[0].pos.y - 1.4f) < 1e-6f);
        BakePose(c, a);
        CHECK(c.aposeApplied);
    });

    test("software renderer: the pose moves what is drawn, the framing stays", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"root", -1, {0, 0, 0}, "rig"});
        s.bones.push_back({"arm", 0, {0, 0, 0}, "rig"});
        s.materials.push_back({});
        s.materials[0].diffuse = {0.9f, 0.1f, 0.1f, 1};
        SceneMesh mesh;
        auto vert = [&](Vec3 p, int bone) { SceneVertex v; v.pos = p; v.normal = {0, 0, 1}; v.bone[0] = bone; v.weight[0] = 1; mesh.vertices.push_back(v); };
        vert({-1, -1, 0}, 0); vert({1, 1, 0}, 0);                               // two loose corners that fix the framing
        vert({0.4f, -0.1f, 0}, 1); vert({0.9f, -0.1f, 0}, 1); vert({0.65f, 0.1f, 0}, 1);   // a triangle on the arm, out on +x
        SceneSubmesh sm;
        sm.material = 0;
        sm.indices = {2, 3, 4};
        mesh.submeshes.push_back(sm);
        s.meshes.push_back(mesh);
        ScenePart part;
        part.id = "p";
        part.meshes = {0};
        s.parts.push_back(part);

        const int W = 200, H = 200;
        auto centroid = [&](const std::vector<uint32_t>& px, float& cx, float& cy) {
            double sx = 0, sy = 0, n = 0;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (px[static_cast<size_t>(y) * W + x] != 0xFFE2E4E8u) { sx += x; sy += y; n += 1; }
            cx = n > 0 ? static_cast<float>(sx / n) : -1.0f;
            cy = n > 0 ? static_cast<float>(sy / n) : -1.0f;
            return n;
        };
        exporter::ViewCamera cam;
        exporter::ViewOptions vo;
        exporter::ViewStatePtr st = exporter::NewViewState();
        std::vector<uint32_t> px;
        exporter::RenderView(s, cam, W, H, vo, *st, px);
        float rx, ry;
        const double restPixels = centroid(px, rx, ry);
        CHECK(restPixels > 100);
        // the arm turned 90 degrees about z at the origin: the triangle goes from +x to +y
        Pose pose;
        pose.edit(1).rot = {0, 0, 90};
        const std::vector<Mat4> skin = SkinMatrices(s, pose);
        vo.skin = &skin;
        exporter::RenderView(s, cam, W, H, vo, *st, px);
        float px2, py2;
        const double posedPixels = centroid(px, px2, py2);
        CHECK(posedPixels > 100 && std::fabs(posedPixels - restPixels) < 0.1 * restPixels);   // the same triangle
        CHECK(std::fabs(px2 - W * 0.5f) < 8.0f && py2 < ry - 20.0f);                            // now about the centre line (the triangle is 0.1 thick), higher up
        CHECK(std::fabs(ry - H * 0.5f) < 6.0f && std::fabs(rx - W * 0.5f) > 20.0f);             // it was off to the side
        // an empty skin or none is the rest pose
        vo.skin = nullptr;
        exporter::RenderView(s, cam, W, H, vo, *st, px);
        float rx2, ry2;
        centroid(px, rx2, ry2);
        CHECK(std::fabs(rx2 - rx) < 0.01f && std::fabs(ry2 - ry) < 0.01f);
    });

    test("pose file: JSON round trip, malformed files are refused with a reason, MakePose by bone name", [] {
        using namespace assemble;
        PoseFile f;
        f.aPose = true;
        f.aPoseDegrees = 35.0f;
        BonePose e;
        e.pos = {0.01f, -0.02f, 0.03f};
        e.rot = {10, 20, -30};
        e.scl = {1, 1.5f, 1};
        f.bones.emplace_back("cf_j_arm00_L", e);
        BonePose z;
        z.rot = {0, 0, 90};
        f.bones.emplace_back("cf_j_hips", z);
        PoseFile back;
        std::string err;
        CHECK(PoseFileFromJson(PoseFileToJson(f), back, &err));
        CHECK(back.aPose && std::fabs(back.aPoseDegrees - 35.0f) < 1e-6f && back.bones.size() == 2);
        CHECK(back.bones.size() == 2 && back.bones[0].first == "cf_j_arm00_L" && std::fabs(back.bones[0].second.rot.z + 30.0f) < 1e-5f && std::fabs(back.bones[0].second.scl.y - 1.5f) < 1e-6f && std::fabs(back.bones[0].second.pos.y + 0.02f) < 1e-6f);
        // a bone with only some of the fields keeps the others at rest
        CHECK(PoseFileFromJson(R"({"format":"cardbridge-pose","version":1,"bones":{"b":{"rotation_euler_degrees":[1,2,3]}}})", back, &err));
        CHECK(!back.aPose && back.bones.size() == 1 && back.bones[0].second.scl.x == 1.0f && back.bones[0].second.pos.x == 0.0f && back.bones[0].second.rot.y == 2.0f);
        for (const char* bad : {"", "not json", "[]", R"({"format":"other","version":1})", R"({"format":"cardbridge-pose"})", R"({"format":"cardbridge-pose","version":2})",
                                R"({"format":"cardbridge-pose","version":1,"bones":[]})", R"({"format":"cardbridge-pose","version":1,"bones":{"b":{"position":[1,2]}}})",
                                R"({"format":"cardbridge-pose","version":1,"bones":{"b":{"position":[1,2,"x"]}}})", R"({"format":"cardbridge-pose","version":1,"a_pose":"yes"})"}) {
            err.clear();
            CHECK(!PoseFileFromJson(bad, back, &err) && !err.empty());
        }
        // MakePose: edits by name, unknown names listed, the A-pose made for a rig that has arms
        Scene s;
        auto add = [&](const char* name, int parent, Vec3 pos) { s.bones.push_back({name, parent, pos, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        const int hips = add("cf_j_hips", -1, {0, 1, 0});
        const int sh = add("cf_j_shoulder_L", hips, {0.1f, 1.4f, 0});
        const int arm = add("cf_j_arm00_L", sh, {0.2f, 1.4f, 0});
        add("cf_j_forearm01_L", arm, {0.5f, 1.4f, 0});
        PoseFile g;
        g.bones.emplace_back("cf_j_hips", z);
        g.bones.emplace_back("no_such_bone", z);
        std::vector<std::string> unknown;
        Pose p = MakePose(s, g, &unknown);
        CHECK(unknown.size() == 1 && unknown[0] == "no_such_bone" && p.bones.size() > static_cast<size_t>(hips) && p.bones[static_cast<size_t>(hips)].rot.z == 90.0f && p.base.empty());
        g.aPose = true;
        g.aPoseDegrees = 40.0f;
        p = MakePose(s, g);
        CHECK(p.base.size() == s.bones.size());
        Scene noArms;
        noArms.bones.push_back({"root", -1, {}, "rig"});
        CHECK(MakePose(noArms, g).base.empty());
        // a saved pose and the same edits made by hand give the same skin
        Pose manual;
        manual.edit(static_cast<size_t>(hips)).rot = {0, 0, 90};
        const auto a1 = SkinMatrices(s, MakePose(s, PoseFile{false, 40.0f, {{"cf_j_hips", z}}}));
        const auto a2 = SkinMatrices(s, manual);
        bool same = a1.size() == a2.size();
        for (size_t i = 0; same && i < a1.size(); ++i) for (int k = 0; k < 16; ++k) same = same && std::fabs(a1[i].m[k] - a2[i].m[k]) < 1e-6f;
        CHECK(same);
    });

    test("dragging a bone: turns about world axes keep the parent and A-pose frames, Euler round trip", [] {
        using namespace assemble;
        // Euler -> matrix -> Euler comes back (away from the poles)
        for (Vec3 e : {Vec3{10, 20, 30}, Vec3{-40, 70, -15}, Vec3{0, 0, 90}, Vec3{25, -120, 5}}) {
            const Vec3 back = EulerFromMatrix(Mat4::TRS({0, 0, 0}, shape::EulerToQuat(e), {1, 1, 1}));
            CHECK(std::fabs(back.x - e.x) < 1e-2f && std::fabs(back.y - e.y) < 1e-2f && std::fabs(back.z - e.z) < 1e-2f);
        }
        // RotationBetween turns one direction onto another, also when they are opposite
        for (Vec3 to : {Vec3{0, 1, 0}, Vec3{0.3f, -0.4f, 0.8f}, Vec3{-1, 0, 0}}) {
            const Vec3 got = normalize(RotationBetween({1, 0, 0}, to).dir({1, 0, 0}));
            const Vec3 want = normalize(to);
            CHECK(std::fabs(got.x - want.x) < 1e-4f && std::fabs(got.y - want.y) < 1e-4f && std::fabs(got.z - want.z) < 1e-4f);
        }
        Scene s;
        auto add = [&](const char* name, int parent, Vec3 pos) { s.bones.push_back({name, parent, pos, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        const int root = add("root", -1, {0, 1, 0});
        const int a = add("a", root, {0.2f, 1, 0});
        const int b = add("b", a, {0.4f, 1, 0});
        Pose pose;
        pose.edit(static_cast<size_t>(root)).rot = {0, 0, 90};   // the whole chain turned up: a is at (0, 1.2), b at (0, 1.4)
        // now drag a toward the world +x again: turn its tip from +y to +x about the world z axis
        const Mat4 turn = RotationBetween({0, 1, 0}, {1, 0, 0});
        pose.edit(static_cast<size_t>(a)).rot = TurnBone(s, pose, static_cast<size_t>(a), turn);
        const auto w = BoneWorlds(s, pose);
        CHECK(std::fabs(w[static_cast<size_t>(a)].translation().y - 1.2f) < 1e-4f);   // a did not move, it turned
        CHECK(std::fabs(w[static_cast<size_t>(b)].translation().x - 0.2f) < 1e-3f && std::fabs(w[static_cast<size_t>(b)].translation().y - 1.2f) < 1e-3f);   // b swung to the right of a
        // with an A-pose base on the bone the result is the same in the world
        Pose posed;
        posed.base.assign(s.bones.size(), Quat{});
        posed.base[static_cast<size_t>(a)] = shape::EulerToQuat({0, 0, -30});
        posed.edit(static_cast<size_t>(root)).rot = {0, 0, 90};
        const Vec3 before = BoneWorlds(s, posed)[static_cast<size_t>(b)].translation();
        const Vec3 tip0{before.x - 0, before.y - 1.2f, before.z};
        (void)tip0;
        posed.edit(static_cast<size_t>(a)).rot = TurnBone(s, posed, static_cast<size_t>(a), RotationBetween(normalize({before.x, before.y - 1.2f, before.z}), {1, 0, 0}));
        const Vec3 after = BoneWorlds(s, posed)[static_cast<size_t>(b)].translation();
        CHECK(std::fabs(after.y - 1.2f) < 1e-3f && after.x > 0.15f);
        // MoveBone: the edit moves the bone by the asked world distance whatever the parent's turn
        Pose moved;
        moved.edit(static_cast<size_t>(root)).rot = {0, 0, 90};
        moved.edit(static_cast<size_t>(a)).pos = MoveBone(s, moved, static_cast<size_t>(a), {0.1f, 0, 0});
        CHECK(std::fabs(BoneWorlds(s, moved)[static_cast<size_t>(a)].translation().x - 0.1f) < 1e-4f);
    });

    test("physics sim: a chain lags behind a moving anchor, settles again, stays out of colliders", [] {
        using namespace assemble;
        Scene s;
        auto add = [&](const char* name, int parent, Vec3 pos) { s.bones.push_back({name, parent, pos, "rig"}); return static_cast<int>(s.bones.size()) - 1; };
        const int root = add("anchor", -1, {0, 1.0f, 0});
        add("p1", root, {0, 0.9f, 0});
        add("p2", 1, {0, 0.8f, 0});
        add("p3", 2, {0, 0.7f, 0});
        SceneChain ch;
        ch.name = "strand";
        ch.bones = {0, 1, 2, 3};
        ch.damping = 0.1f; ch.elasticity = 0.1f; ch.stiffness = 0.1f;
        s.chains.push_back(ch);
        Pose rest;
        std::vector<Mat4> pw = BoneWorlds(s, rest);
        PhysicsSim sim;
        sim.Reset(s, pw);
        CHECK(sim.particleCount() == 4);
        std::vector<Mat4> skin(s.bones.size());
        // at rest nothing moves: the skin stays the identity
        for (int i = 0; i < 60; ++i) sim.Step(s, pw, 1, skin);
        CHECK(std::fabs(skin[3].point(s.bones[3].pos).x) < 1e-4f && std::fabs(skin[3].point(s.bones[3].pos).y - 0.7f) < 1e-4f);
        // the anchor jumps 0.3 to the right: the tip does not follow at once, then catches up
        rest.edit(0).pos = {0.3f, 0, 0};
        pw = BoneWorlds(s, rest);
        sim.Step(s, pw, 1, skin);
        const float lag = 0.3f - skin[3].point(s.bones[3].pos).x;   // how far the tip has not yet followed
        CHECK(lag > 0.05f);
        bool moving = true;
        for (int i = 0; i < 600 && moving; ++i) moving = sim.Step(s, pw, 1, skin);
        CHECK(!moving);
        CHECK(std::fabs(skin[3].point(s.bones[3].pos).x - 0.3f) < 0.02f);   // settled under the anchor again
        // a sphere collider in the way keeps the tip out
        s.colliders.push_back({"ball", 0, {0.02f, 0.7f, 0}, {0, 1, 0}, 0.05f, 0.0f, false});   // rides the anchor bone, which stands at x 0.3: the ball sits at x 0.32
        sim.Reset(s, pw);
        for (int i = 0; i < 300; ++i) sim.Step(s, pw, 1, skin);
        const Vec3 tip = skin[3].point(s.bones[3].pos);
        const float d = std::sqrt((tip.x - 0.32f) * (tip.x - 0.32f) + (tip.y - 0.7f) * (tip.y - 0.7f) + tip.z * tip.z);
        CHECK(d > 0.04f);   // on the ball's surface, give or take the length the chain keeps
    });

    test("physics sim: the body's own jiggle chains follow their switches, like the export", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"cf_j_chest01", -1, {0, 1.2f, 0}, "rig"});
        s.bones.push_back({"cf_j_chest02", 0, {0, 1.15f, 0.05f}, "rig"});
        s.bones.push_back({"cf_s_bnip02_L", 1, {0, 1.05f, 0.1f}, "rig"});
        s.bones.push_back({"cf_s_bnip02_L2", 2, {0, 0.95f, 0.12f}, "rig"});
        SceneChain ch;
        ch.name = "bust";
        ch.bones = {2, 3};
        ch.characterOwn = true;
        ch.damping = 0.1f; ch.elasticity = 0.1f; ch.stiffness = 0.1f;
        s.chains.push_back(ch);
        Pose rest;
        std::vector<Mat4> pw = BoneWorlds(s, rest);
        {   // the default (boobs on): the chain is simulated
            PhysicsSim sim;
            sim.Reset(s, pw, true, false, 1.0f);   // at full intensity, so the tip lags behind the jump
            CHECK(sim.particleCount() == 2);
            std::vector<Mat4> skin(s.bones.size());
            rest.edit(0).pos = {0.3f, 0, 0};
            pw = BoneWorlds(s, rest);
            sim.Step(s, pw, 1, skin);
            CHECK(std::fabs(0.3f - skin[3].point(s.bones[3].pos).x) > 0.05f);   // the tip lags behind the jump
        }
        {   // boobs off: no particles at all
            PhysicsSim sim;
            sim.Reset(s, pw, false, false, 1.0f);
            CHECK(sim.particleCount() == 0 && sim.empty());
        }
    });

    test("auto cloth: hanging garment gets chains that follow their limb, torso stays rigid", [] {
        using namespace assemble;
        // Synthetic shoulder coat accessory: rigid anchor at chest + hanging
        // panel to the forearm side. garment tops are intentionally excluded
        // (body-fit), so this regression now uses an accessory; the test
        // verifies rest preservation and forearm follow via chain parent.
        Scene s;
        auto add = [&](const char* name, int parent, Vec3 pos) {
            SceneBone b; b.name = name; b.parent = parent; b.pos = pos; b.source = "rig";
            b.world = Mat4::TRS(pos, Quat{}, Vec3{1, 1, 1});
            s.bones.push_back(b); return static_cast<int>(s.bones.size()) - 1;
        };
        // Body: chest (anchor) -> upper arm -> forearm (all rig).
        int chest = add("cf_j_chest01", -1, {0, 1.30f, 0});
        int upper = add("cf_j_arm00_L", chest, {-0.20f, 1.25f, 0});
        int fore  = add("cf_j_forearm01_L", upper, {-0.42f, 0.95f, 0});
        (void)fore;
        // Part: accessory chest drape (category 124) with hanging panel - garment tops are excluded from synthesis
        ScenePart part; part.id = "outfit0.acc99"; part.group = "Accessories"; part.category = 124;
        part.attachNode = "cf_j_chest01";
        part.status = "resolved-base"; part.includeInExport = true;
        int partIdx = static_cast<int>(s.parts.size());
        s.parts.push_back(part);
        SceneMesh mesh; mesh.name = "top"; mesh.part = partIdx; mesh.reshapable = true;
        // Torso verts near chest (must NOT become cloth).
        for (int i = 0; i < 100; ++i) {
            SceneVertex v; v.pos = {0.02f * (i % 5 - 2), 1.20f, 0.05f}; v.normal = {0, 0, 1};
            v.bone[0] = chest; v.weight[0] = 1.0f; for (int k = 1; k < 4; ++k) { v.bone[k] = chest; v.weight[k] = 0; }
            const Mat4 iw = inverse(s.bones[static_cast<size_t>(chest)].world);
            v.local[0] = iw.point(v.pos); v.nlocal[0] = iw.dir(v.normal);
            for (int k = 1; k < 4; ++k) { v.local[k] = v.local[0]; v.nlocal[k] = v.nlocal[0]; }
            mesh.vertices.push_back(v);
        }
        // Sleeve verts hanging to the forearm/wrist (must get physics follow).
        for (int i = 0; i < 400; ++i) {
            const float t = static_cast<float>(i % 40) / 40.0f; // 0 root -> 1 tip
            SceneVertex v; v.pos = {-0.20f - 0.25f * t, 1.25f - 0.50f * t, 0.02f}; v.normal = {0, 0, 1};
            v.bone[0] = fore; v.weight[0] = 1.0f; for (int k = 1; k < 4; ++k) { v.bone[k] = fore; v.weight[k] = 0; }
            const Mat4 iw = inverse(s.bones[static_cast<size_t>(fore)].world);
            v.local[0] = iw.point(v.pos); v.nlocal[0] = iw.dir(v.normal);
            for (int k = 1; k < 4; ++k) { v.local[k] = v.local[0]; v.nlocal[k] = v.nlocal[0]; }
            mesh.vertices.push_back(v);
        }
        int meshIdx = static_cast<int>(s.meshes.size());
        s.meshes.push_back(mesh);
        s.parts[static_cast<size_t>(partIdx)].meshes.push_back(meshIdx);
        // One small trinket part that must be left alone (regression: no chains).
        ScenePart trinket; trinket.id = "outfit0.acc99"; trinket.group = "Accessories"; trinket.category = 124;
        trinket.status = "resolved-mod"; trinket.attachNode = "cf_j_chest01";
        SceneMesh tm; tm.name = "ring"; tm.part = static_cast<int>(s.parts.size());
        for (int i = 0; i < 10; ++i) { SceneVertex v; v.pos = {0, 1.30f, 0.1f}; v.normal = {0, 0, 1}; v.bone[0] = chest; v.weight[0] = 1; for (int k = 1; k < 4; ++k) { v.bone[k] = chest; v.local[k] = inverse(s.bones[static_cast<size_t>(chest)].world).point(v.pos); v.nlocal[k] = inverse(s.bones[static_cast<size_t>(chest)].world).dir(v.normal); } v.local[0] = inverse(s.bones[static_cast<size_t>(chest)].world).point(v.pos); v.nlocal[0] = inverse(s.bones[static_cast<size_t>(chest)].world).dir(v.normal); tm.vertices.push_back(v); }
        int tMesh = static_cast<int>(s.meshes.size()); s.meshes.push_back(tm);
        trinket.meshes.push_back(tMesh);
        s.parts.push_back(trinket);
        // One authored part that must stay untouched.
        ScenePart authored; authored.id = "hair.back"; authored.group = "Hair"; authored.category = 101; authored.status = "resolved-mod";
        int hairBone = add("hair/strand", chest, {0, 1.4f, -0.1f});
        int hairBone2 = add("hair/strand2", hairBone, {0, 1.2f, -0.15f});
        s.parts.push_back(authored);
        SceneChain ach; ach.name = "hair strand"; ach.part = "hair.back"; ach.bones = {hairBone, hairBone2};
        ach.damping = 0.1f; ach.elasticity = 0.1f; ach.stiffness = 0.1f;
        s.chains.push_back(ach);
        const size_t bonesBefore = s.bones.size(), chainsBefore = s.chains.size();
        // Snapshot torso verts: they must not move when auto-cloth runs.
        std::vector<Vec3> torsoBefore;
        for (size_t i = 0; i < 100; ++i) torsoBefore.push_back(s.meshes[static_cast<size_t>(meshIdx)].vertices[i].pos);
        // Synthesize.
        const int made = SynthesizeAutoCloth(s);
        CHECK(made >= 1);
        CHECK(s.chains.size() == chainsBefore + static_cast<size_t>(made));
        // Authored chain untouched.
        CHECK(s.chains.front().bones.size() == 2 && s.chains.front().name == "hair strand");
        // Rest pose preserved: torso + sleeve tips still at rest positions.
        Pose rest; std::vector<Mat4> skin = SkinMatrices(s, rest);
        for (size_t i = 0; i < torsoBefore.size(); ++i) {
            const Vec3 p = SkinPoint(s.meshes[static_cast<size_t>(meshIdx)].vertices[i], skin);
            const float dx = p.x - torsoBefore[i].x, dy = p.y - torsoBefore[i].y, dz = p.z - torsoBefore[i].z;
            CHECK(dx * dx + dy * dy + dz * dz < 1e-8f); // no collapse at rest
        }
        // Base-pose follow: move the forearm +0.2 X; sleeve tips must follow
        // substantially (auto chain inherits the limb), not detach to chest.
        Pose moved; moved.edit(static_cast<size_t>(fore)).pos = {0.20f, 0, 0};
        std::vector<Mat4> skin2 = SkinMatrices(s, moved);
        float followed = 0;
        for (size_t i = 100; i < 500; ++i) {
            const Vec3 p0 = SkinPoint(s.meshes[static_cast<size_t>(meshIdx)].vertices[i], skin);
            const Vec3 p1 = SkinPoint(s.meshes[static_cast<size_t>(meshIdx)].vertices[i], skin2);
            followed += (p1.x - p0.x); // mean X travel of sleeve verts
        }
        followed /= 400.0f;
        CHECK(followed > 0.08f); // inherits forearm motion (0.20 raw * weights); chest-only anchor gives ~0
        // Weight normalization.
        for (size_t i = 0; i < s.meshes[static_cast<size_t>(meshIdx)].vertices.size(); ++i) {
            float sum = 0; for (int k = 0; k < 4; ++k) sum += s.meshes[static_cast<size_t>(meshIdx)].vertices[i].weight[k];
            CHECK(std::fabs(sum - 1.0f) < 1e-4f);
        }
        (void)bonesBefore;
    });

    test("expression morphs: shape offsets relative to the resting weight, at the mesh's first vertex", [] {
        assemble::Scene s;
        assemble::SceneMesh mesh;
        mesh.name = "face";
        assemble::SceneBlendShape open, close;
        open.name = "open"; open.rest = 100.0f; open.offsets = {{1, {0, 1, 0}}};
        close.name = "close"; close.rest = 0.0f; close.offsets = {{1, {0, -2, 0}}, {2, {1, 0, 0}}};
        mesh.shapes = {open, close};
        s.meshes.push_back(mesh);
        assemble::SceneExpression ex;
        ex.name = "Eyes 0 closed";
        ex.panel = 2;
        ex.targets = {{0, 0, 0.0f}, {0, 1, 100.0f}};
        ex.shape = "def_cl";
        s.expressions.push_back(ex);
        pmx::Model m;
        for (int i = 0; i < 10; ++i) m.vertices.push_back({});
        std::map<std::string, std::string> shapes;
        size_t added = exporter::AddExpressionMorphs(s, m, {5}, 2.0f, &shapes);
        CHECK(added >= 1 && m.morphs[0].name == "Eyes 0 closed" && m.morphs[0].panel == 2);
        CHECK(shapes.size() == 1 && shapes["Eyes 0 closed"] == "def_cl");   // the pattern each expression plays, for the standard MMD morphs
        // vertex 1 (PMX 6): open from 100 to 0 = -1 * (0,1,0), close from 0 to 100 = +1 * (0,-2,0), scaled by 2: (0,-6,0); vertex 2 (PMX 7): (2,0,0)
        std::map<int, Vec3> got;
        for (const auto& [v, d] : m.morphs[0].vertices) got[v] = d;
        CHECK(got.size() == 2 && std::fabs(got[6].y + 6.0f) < 1e-4f && std::fabs(got[7].x - 2.0f) < 1e-4f);
        // a mesh that was not exported contributes nothing
        pmx::Model m2;
        m2.vertices.resize(10);
        CHECK(exporter::AddExpressionMorphs(s, m2, {-1}, 2.0f) == 0);
    });
    test("clothing: the outfit replaces clothes but never deletes hair carried as accessories", [] {
        auto accPart = [](int64_t type, int64_t id, const char* parent) {
            Value::Array empty;
            Value::Map m;
            m.emplace_back(Value(std::string("type")), Value(type));
            m.emplace_back(Value(std::string("id")), Value(id));
            m.emplace_back(Value(std::string("parentKey")), Value(std::string(parent)));
            return Value(std::move(m));
        };
        auto clothPart = [](int64_t id) {
            Value::Map m;
            m.emplace_back(Value(std::string("id")), Value(id));
            m.emplace_back(Value(std::string("colorInfo")), Value(Value::Array{}));
            return Value(std::move(m));
        };
        auto mkClothes = [&](std::initializer_list<int64_t> ids) {
            Value::Array parts;
            for (int64_t id : ids) parts.push_back(clothPart(id));
            while (parts.size() < 9) parts.push_back(clothPart(0));
            Value::Array sub{Value(int64_t{0}), Value(int64_t{0}), Value(int64_t{0})};
            Value::Map m;
            m.emplace_back(Value(std::string("parts")), Value(std::move(parts)));
            m.emplace_back(Value(std::string("subPartsId")), Value(std::move(sub)));
            return Value(std::move(m));
        };
        auto mkAcc = [&](std::initializer_list<Value> parts) {
            Value::Array arr(parts.begin(), parts.end());
            while (arr.size() < 20) arr.push_back(accPart(120, 0, ""));
            Value::Map m;
            m.emplace_back(Value(std::string("version")), Value(std::string("0.0.0")));
            m.emplace_back(Value(std::string("parts")), Value(std::move(arr)));
            return Value(std::move(m));
        };

        card::Card c;
        c.family = card::CardFamily::Koikatsu;
        c.status = Value(Value::Map{{Value(std::string("coordinateType")), Value(int64_t{2})}});
        Value::Array hairParts;
        {
            Value::Map p;
            p.emplace_back(Value(std::string("id")), Value(int64_t{500}));
            hairParts.push_back(Value(std::move(p)));
            hairParts.push_back(Value(Value::Map{}));
        }
        c.hair = Value(Value::Map{{Value(std::string("parts")), Value(std::move(hairParts))}});
        c.outfits.resize(7);
        // Outfit 2: top 56, hair accessories in slots 8 (type 123) and 9 (type 122).
        c.outfits[2].clothes = mkClothes({56, 0, 203, 0, 0, 0, 0, 6, 6});
        c.outfits[2].accessory = mkAcc({accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""),
                                        accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""),
                                        accPart(123, 568, "a_n_headside"), accPart(122, 15, "a_n_headside")});
        c.rows = {
            {"yamadamod.acchairF2", 568, 100018882, "outfit2.accessory8.ChaFileAccessory.PartsInfo.id", 123},
            {"com.DeathWeasel.AccessoryHairBack", 15, 100002206, "outfit2.accessory9.ChaFileAccessory.PartsInfo.id", 122},
        };

        card::ClothingFile cl;
        cl.supported = true;
        cl.name = "Test Outfit";
        // Clothing stores mod ids as locals: top local 100019615 -> slot 1, necklace local 100011501 -> slot 801.
        cl.outfit.clothes = mkClothes({100019615, 100020035, 0, 7, 0, 100007362, 0, 100027702, 100027702});
        cl.outfit.accessory = mkAcc({accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""),
                                     accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""), accPart(120, 0, ""),
                                     accPart(124, 100011501, "a_n_neck")});
        cl.outfit.enableMakeup = false;
        cl.rows = {
            {"M2M.NurseOutfit", 1, 100019615, "ChaFileClothes.ClothesTop", 105},
            {"Mint_E403.SuperMiniTightSkirt", 2822, 100020035, "ChaFileClothes.ClothesBot", 106},
            {"com.Blanketman.NonLightStockings", 1486, 100007362, "ChaFileClothes.ClothesPants", 110},
            {"ws54090.stiletto", 11, 100027702, "ChaFileClothes.ClothesShoesInner", 112},
            {"cream002", 801, 100011501, "accessory8.ChaFileAccessory.PartsInfo.id", 124},
        };

        card::ApplyClothingOptions opt;
        auto rep = card::ApplyClothing(c, cl, opt);
        CHECK(rep.outfit == 2);
        const auto& parts = c.outfits[2].clothes.at("parts").asArray();
        CHECK(parts[0].at("id").asInt() == 1 && parts[1].at("id").asInt() == 2822 && parts[5].at("id").asInt() == 1486);
        CHECK(parts[7].at("id").asInt() == 11 && parts[3].at("id").asInt() == 7);   // the base id stays
        const auto& acc = c.outfits[2].accessory.at("parts").asArray();
        // Hair stays in its slots; the necklace moved to the first empty slot (0).
        CHECK(acc[8].at("type").asInt() == 123 && acc[8].at("id").asInt() == 568);
        CHECK(acc[9].at("type").asInt() == 122 && acc[9].at("id").asInt() == 15);
        CHECK(acc[0].at("type").asInt() == 124 && acc[0].at("id").asInt() == 801);
        CHECK(c.hair.at("parts").at(size_t(0)).at("id").asInt() == 500);   // the hair slot is untouched
        bool hasTopRow = false;
        for (const auto& r : c.rows) if (r.property == "outfit2.ChaFileClothes.ClothesTop" && r.slot == 1) hasTopRow = true;
        CHECK(hasTopRow);
        bool hairRowKept = false;
        for (const auto& r : c.rows) if (r.property == "outfit2.accessory8.ChaFileAccessory.PartsInfo.id" && r.slot == 568) hairRowKept = true;
        CHECK(hairRowKept);   // the resolver still finds the hair
        // Added-clothing provenance: the file's parts are marked apart from the base model's.
        CHECK(c.hasClothing && c.clothingOutfit == 2 && c.clothingName == "Test Outfit");
        CHECK(c.clothingPartIds.count("outfit2.top") != 0 && c.clothingPartIds.count("outfit2.bottom") != 0);
        CHECK(c.clothingPartIds.count("outfit2.shoes_inner") != 0 && c.clothingPartIds.count("outfit2.acc00") != 0);
        CHECK(c.clothingPartIds.count("outfit2.acc08") == 0);   // the kept hair accessory is base, not added
    });
    test("clothing: a non-hair accessory in the way is replaced, and an empty outfit slot never clears the character", [] {
        auto accPart = [](int64_t type, int64_t id, const char* parent) {
            Value::Map m;
            m.emplace_back(Value(std::string("type")), Value(type));
            m.emplace_back(Value(std::string("id")), Value(id));
            m.emplace_back(Value(std::string("parentKey")), Value(std::string(parent)));
            return Value(std::move(m));
        };
        auto clothPart = [](int64_t id) {
            Value::Map m;
            m.emplace_back(Value(std::string("id")), Value(id));
            return Value(std::move(m));
        };
        auto mkClothes = [&](int64_t top) {
            Value::Array parts;
            for (int i = 0; i < 9; ++i) parts.push_back(clothPart(i == 0 ? top : 0));
            Value::Array sub{Value(int64_t{0}), Value(int64_t{0}), Value(int64_t{0})};
            Value::Map m;
            m.emplace_back(Value(std::string("parts")), Value(std::move(parts)));
            m.emplace_back(Value(std::string("subPartsId")), Value(std::move(sub)));
            return Value(std::move(m));
        };
        auto mkAcc = [&](std::initializer_list<Value> parts) {
            Value::Array arr(parts.begin(), parts.end());
            while (arr.size() < 20) arr.push_back(accPart(120, 0, ""));
            Value::Map m;
            m.emplace_back(Value(std::string("parts")), Value(std::move(arr)));
            return Value(std::move(m));
        };
        card::Card c;
        c.family = card::CardFamily::Koikatsu;
        c.status = Value(Value::Map{{Value(std::string("coordinateType")), Value(int64_t{0})}});
        c.outfits.resize(7);
        c.outfits[0].clothes = mkClothes(56);
        c.outfits[0].accessory = mkAcc({accPart(121, 10, "a_n_headside")});   // a hat, not hair
        card::ClothingFile cl;
        cl.supported = true;
        cl.name = "Hat only";
        cl.outfit.clothes = mkClothes(56);
        cl.outfit.accessory = mkAcc({accPart(121, 99, "a_n_headside"), accPart(120, 0, "")});
        auto rep = card::ApplyClothing(c, cl, card::ApplyClothingOptions{});
        const auto& acc = c.outfits[0].accessory.at("parts").asArray();
        CHECK(acc[0].at("id").asInt() == 99);   // the hat slot is taken by the outfit
        CHECK(rep.hairPreserved == 0);
    });

    test("clothing: heeled shoes merge with their resolver rows (heels reach settleHeels)", [] {
        // The viewer/CLI clothes workflow: ApplyClothing -> ItemsFromCard -> Assemble ->
        // settleHeels. If the shoe ids/rows were lost in the merge, settleHeels would see
        // empty slots and pose nothing. This covers the merge half (Assembly is covered by
        // the end-to-end conversion validation in the session notes).
        auto clothPart = [](int64_t id) {
            Value::Map m;
            m.emplace_back(Value(std::string("id")), Value(id));
            m.emplace_back(Value(std::string("colorInfo")), Value(Value::Array{}));
            return Value(std::move(m));
        };
        auto mkClothes = [&](std::initializer_list<int64_t> ids) {
            Value::Array parts;
            for (int64_t id : ids) parts.push_back(clothPart(id));
            while (parts.size() < 9) parts.push_back(clothPart(0));
            Value::Array sub{Value(int64_t{0}), Value(int64_t{0}), Value(int64_t{0})};
            Value::Map m;
            m.emplace_back(Value(std::string("parts")), Value(std::move(parts)));
            m.emplace_back(Value(std::string("subPartsId")), Value(std::move(sub)));
            return Value(std::move(m));
        };
        card::Card c;
        c.family = card::CardFamily::Koikatsu;
        c.status = Value(Value::Map{{Value(std::string("coordinateType")), Value(int64_t{0})}});
        c.outfits.resize(7);
        c.outfits[0].clothes = mkClothes({0, 0, 0, 0, 0, 0, 0, 0, 0});   // barefoot: slots empty
        c.rows = {};
        card::ClothingFile cl;
        cl.supported = true;
        cl.name = "Heels";
        // Vanilla Stiletto pumps (local -> slot 11) in both shoe slots.
        cl.outfit.clothes = mkClothes({0, 0, 0, 0, 0, 0, 0, 100027702, 100027702});
        cl.outfit.accessory = Value(Value::Map{});
        cl.rows = {
            {"ws54090.stiletto", 11, 100027702, "ChaFileClothes.ClothesShoesInner", 112},
            {"ws54090.stiletto", 11, 100027702, "ChaFileClothes.ClothesShoesOuter", 112},
        };
        auto rep = card::ApplyClothing(c, cl, card::ApplyClothingOptions{});
        CHECK(rep.outfit == 0 && rep.clothesReplaced == 2);   // both shoe slots replaced
        const auto& parts = c.outfits[0].clothes.at("parts").asArray();
        CHECK(parts[7].at("id").asInt() == 11 && parts[8].at("id").asInt() == 11);   // translated
        // ItemsFromCard must surface both slots as Outfit/112 so Assemble builds them.
        auto items = resolve::ItemsFromCard(c, 0);
        bool inner = false, outer = false;
        for (const auto& it : items) {
            if (it.partId == "outfit0.shoes_inner") { inner = true; CHECK(it.category == 112 && it.id == 11 && it.slot == 7); }
            if (it.partId == "outfit0.shoes_outer") { outer = true; CHECK(it.category == 112 && it.id == 11 && it.slot == 8); }
        }
        CHECK(inner && outer);
        // Resolver rows for both slots must be present (the heel config lookup keys on Name
        // from the list row; geometry fallback needs only category/id).
        bool ri = false, ro = false;
        for (const auto& r : c.rows) {
            if (r.property == "outfit0.ChaFileClothes.ClothesShoesInner" && r.slot == 11) ri = true;
            if (r.property == "outfit0.ChaFileClothes.ClothesShoesOuter" && r.slot == 11) ro = true;
        }
        CHECK(ri && ro);
        // Repeated application must not accumulate ids (same file -> same slots).
        auto rep2 = card::ApplyClothing(c, cl, card::ApplyClothingOptions{});
        CHECK(c.outfits[0].clothes.at("parts").at(size_t(7)).at("id").asInt() == 11);
        CHECK(rep2.clothesReplaced >= 0);
    });

    test("clothing: mod pattern ids in colorInfo are translated from local to slot (the Wiz H bug)", [] {
        // Outfit files store pattern ids as session-local (100000421) while the
        // resolver finds them by author Slot (2016115). Without the translation the
        // pattern never resolves, the pattern mask stays empty and the garment
        // loses its pattern colours (gold trim baked as the base colour).
        auto colorEntry = [](int64_t pattern) {
            Value::Array tiling;
            tiling.emplace_back(1.0);
            tiling.emplace_back(1.0);
            Value::Array white;
            for (int k = 0; k < 4; ++k) white.emplace_back(1.0);
            Value::Map m;
            m.emplace_back(Value(std::string("pattern")), Value(pattern));
            m.emplace_back(Value(std::string("baseColor")), Value(white));
            m.emplace_back(Value(std::string("patternColor")), Value(white));
            m.emplace_back(Value(std::string("tiling")), Value(tiling));
            return Value(std::move(m));
        };
        auto clothPart = [&](int64_t id, std::initializer_list<int64_t> patterns) {
            Value::Array ci;
            for (int64_t p : patterns) ci.push_back(colorEntry(p));
            while (ci.size() < 4) ci.push_back(colorEntry(0));
            Value::Map m;
            m.emplace_back(Value(std::string("id")), Value(id));
            m.emplace_back(Value(std::string("colorInfo")), Value(std::move(ci)));
            return Value(std::move(m));
        };
        auto mkClothes = [&](Value top) {
            Value::Array parts;
            parts.push_back(std::move(top));
            for (int i = 1; i < 9; ++i) parts.push_back(clothPart(0, {}));
            Value::Array sub{Value(int64_t{0}), Value(int64_t{0}), Value(int64_t{0})};
            Value::Map m;
            m.emplace_back(Value(std::string("parts")), Value(std::move(parts)));
            m.emplace_back(Value(std::string("subPartsId")), Value(std::move(sub)));
            return Value(std::move(m));
        };
        card::Card c;
        c.family = card::CardFamily::Koikatsu;
        c.status = Value(Value::Map{{Value(std::string("coordinateType")), Value(int64_t{0})}});
        c.outfits.resize(7);
        c.outfits[0].clothes = mkClothes(clothPart(56, {0}));
        c.rows = {};
        card::ClothingFile cl;
        cl.supported = true;
        cl.name = "Wiz H-like";
        // Base top id (no row) with a mod pattern referenced by its local id.
        cl.outfit.clothes = mkClothes(clothPart(2, {100000421, 100001387, 0, 27}));
        cl.outfit.accessory = Value(Value::Map{});
        cl.rows = {
            {"HeartCat_pattern", 2016115, 100000421, "ChaFileClothes.ClothesTopPattern0", 430},
            {"koikoi.happy.nu.cloth_texture", 5801, 100001387, "ChaFileClothes.ClothesTopPattern1", 430},
        };
        card::ApplyClothing(c, cl, card::ApplyClothingOptions{});
        const Value& ci = c.outfits[0].clothes.at("parts").at(size_t(0)).at("colorInfo");
        CHECK(ci.at(size_t(0)).at("pattern").asInt() == 2016115);  // top pattern 0: local -> slot
        CHECK(ci.at(size_t(1)).at("pattern").asInt() == 5801);     // top pattern 1: local -> slot
        CHECK(ci.at(size_t(2)).at("pattern").asInt() == 0);        // none stays none
        CHECK(ci.at(size_t(3)).at("pattern").asInt() == 27);       // base pattern is untouched
        bool hasTopPatRow = false;
        for (const auto& r : c.rows) if (r.property == "outfit0.ChaFileClothes.ClothesTopPattern0" && r.slot == 2016115) hasTopPatRow = true;
        CHECK(hasTopPatRow);  // resolver can find the translated pattern
    });

    test("adapter selection: discrete, integrated, Arc and software (synthetic descriptions)", [] {
        using namespace exporter;
        constexpr uint64_t MB = 1024ULL * 1024ULL;
        constexpr uint64_t GB = 1024ULL * MB;
        AdapterDesc nvidiaD = {0x10DE, 0x1234, "NVIDIA GeForce RTX 4070", 8 * GB, 0, 0, 0, 0};
        AdapterDesc amdD = {0x1002, 0x1234, "AMD Radeon RX 7800 XT", 16 * GB, 0, 0, 0, 1};
        AdapterDesc amdI = {0x1002, 0x1234, "AMD Radeon Graphics (integrated)", 512 * MB, 0, 8 * GB, 0, 2};
        AdapterDesc arcD = {0x8086, 0x1234, "Intel Arc A770", 16 * GB, 0, 0, 0, 3};
        AdapterDesc intelI = {0x8086, 0x1234, "Intel UHD Graphics 620", 128 * MB, 0, 16 * GB, 0, 4};
        AdapterDesc basic = {0x1414, 0x8C, "Microsoft Basic Render Driver", MB, 0, 0, 0, 5};
        AdapterDesc warp = {0x1414, 0, "WARP", 0, 0, 0, kAdapterFlagSoftware, 6};
        AdapterDesc soft = {0x10DE, 0x1234, "NVIDIA (WARP)", 8 * GB, 0, 0, kAdapterFlagSoftware, 7};
        // Scores: discrete cards win, integrated stay suitable but lose, software/virtual are unsuitable.
        CHECK(ScoreAdapter(nvidiaD).suitable && ScoreAdapter(amdD).suitable && ScoreAdapter(arcD).suitable);
        CHECK(ScoreAdapter(amdI).suitable && ScoreAdapter(intelI).suitable);
        CHECK(!ScoreAdapter(basic).suitable && !ScoreAdapter(warp).suitable && !ScoreAdapter(soft).suitable);
        CHECK(ScoreAdapter(nvidiaD).score > ScoreAdapter(intelI).score);
        CHECK(ScoreAdapter(amdD).score > ScoreAdapter(amdI).score);
        // Intel Arc discrete competes fairly: same memory, same score as AMD/NVIDIA discrete (vendor has no
        // discrete bonus; the memory signal orders them, not the vendor).
        CHECK(std::abs(ScoreAdapter(arcD).score - ScoreAdapter(amdD).score) < 200);
        CHECK(std::abs(ScoreAdapter(arcD).score - ScoreAdapter(nvidiaD).score) < 2200);
        // No automatic Intel penalty: Intel integrated equals AMD integrated with the same memory.
        AdapterDesc amdILow = {0x1002, 0x1234, "AMD Radeon Graphics (integrated)", 128 * MB, 0, 16 * GB, 0, 0};
        CHECK(ScoreAdapter(amdILow).score == ScoreAdapter(intelI).score);
        CHECK(ScoreAdapter(intelI).cls == AdapterClass::IntegratedLikely);
        CHECK(ScoreAdapter(nvidiaD).cls == AdapterClass::DiscreteLikely);
        // Multi-GPU: discrete beats integrated regardless of vendor; Arc beats AMD integrated.
        {
            std::vector<AdapterDesc> v = {intelI, nvidiaD};
            CHECK(SelectBestAdapter(v) == 1);
            std::vector<AdapterDesc> v2 = {amdI, arcD};
            CHECK(SelectBestAdapter(v2) == 1);
            std::vector<AdapterDesc> v3 = {intelI};
            CHECK(SelectBestAdapter(v3) == 0);   // alone, integrated is usable
            std::vector<AdapterDesc> v4 = {basic, warp};
            CHECK(SelectBestAdapter(v4) == -1);  // no hardware: fallback, not a software pick
            CHECK(SelectBestAdapter(v4, true) >= 0);  // explicit software fallback allowed
            std::vector<AdapterDesc> v5 = {basic, intelI};
            CHECK(SelectBestAdapter(v5) == 1);
        }
        // Description heuristic catches a basic driver without the flag.
        AdapterDesc basicNoFlag = {0x1414, 0x8C, "Microsoft Basic Render Driver", 0, 0, 0, 0, 0};
        CHECK(!ScoreAdapter(basicNoFlag).suitable);
    });

    test("skinning: bone-index and weight validation matches on CPU (GPU shader mirrors it)", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"b0", -1, {0, 0, 0}, "rig"});
        s.bones.push_back({"b1", 0, {1, 0, 0}, "rig"});
        const std::vector<Mat4> skin = {Mat4::TRS({1, 0, 0}, Quat{}, {1, 1, 1}), Mat4::TRS({0, 2, 0}, Quat{}, {1, 1, 1})};
        auto vert = [](int b0, float w0, int b1, float w1, int b2, float w2, int b3, float w3) {
            SceneVertex v;
            v.pos = {0, 0, 0};
            v.normal = {0, 0, 1};
            v.bone[0] = b0; v.weight[0] = w0;
            v.bone[1] = b1; v.weight[1] = w1;
            v.bone[2] = b2; v.weight[2] = w2;
            v.bone[3] = b3; v.weight[3] = w3;
            return v;
        };
        auto finite = [](Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        // Valid bone index: normal skinning (identity-ish offset applied).
        {
            Vec3 p = SkinPoint(vert(0, 1, 0, 0, 0, 0, 0, 0), skin);
            CHECK(finite(p) && std::fabs(p.x - 1.0f) < 1e-5f);
        }
        // Negative, equal-to-count and greater-than-count indices are rejected, never accessed.
        CHECK(SkinPoint(vert(-1, 1, 0, 0, 0, 0, 0, 0), skin).x == 0.0f);   // no valid influence: rest fallback
        CHECK(SkinPoint(vert(2, 1, 0, 0, 0, 0, 0, 0), skin).x == 0.0f);    // == count rejected
        CHECK(SkinPoint(vert(99, 1, 0, 0, 0, 0, 0, 0), skin).x == 0.0f);   // > count rejected
        // One valid + one invalid: the valid influence is preserved (both implementations do this).
        {
            Vec3 p = SkinPoint(vert(0, 1, 99, 1, 0, 0, 0, 0), skin);
            CHECK(finite(p) && std::fabs(p.x - 1.0f) < 1e-5f);
        }
        // Zero-weight, negative, NaN and infinite weights are all rejected; no NaN/inf is produced.
        CHECK(SkinPoint(vert(0, 0, 0, 0, 0, 0, 0, 0), skin).x == 0.0f);
        CHECK(SkinPoint(vert(0, -1, 0, 0, 0, 0, 0, 0), skin).x == 0.0f);
        CHECK(finite(SkinPoint(vert(0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0, 0), skin)));
        CHECK(SkinPoint(vert(0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0, 0), skin).x == 0.0f);
        CHECK(SkinPoint(vert(0, std::numeric_limits<float>::infinity(), 0, 0, 0, 0, 0, 0), skin).x == 0.0f);
        CHECK(SkinPoint(vert(0, -std::numeric_limits<float>::infinity(), 0, 0, 0, 0, 0, 0), skin).x == 0.0f);
        // No valid influences: the rest position (existing fallback behaviour).
        CHECK(SkinPoint(vert(-5, 1, 7, 1, 100, 1, -1, 1), skin).x == 0.0f && SkinPoint(vert(-5, 1, 7, 1, 100, 1, -1, 1), skin).y == 0.0f);
        // Directions follow the same rule.
        CHECK(finite(SkinDirection(vert(0, 1, 0, 0, 0, 0, 0, 0), skin)));
        CHECK(SkinDirection(vert(-1, 1, 0, 0, 0, 0, 0, 0), skin).z == 1.0f);
        CHECK(finite(SkinDirection(vert(0, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0, 0), skin)));
    });

    test("skinning: rest pose and extreme rotations stay finite (CPU/GPU numerical parity in SkinBench)", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"root", -1, {0, 0, 0}, "rig"});
        SceneVertex v;
        v.pos = {1, 2, 3};
        v.normal = {0, 0, 1};
        v.bone[0] = 0;
        v.weight[0] = 1;
        const std::vector<Mat4> rest = {Mat4{}};
        Vec3 atRest = SkinPoint(v, rest);
        CHECK(std::fabs(atRest.x - 1.0f) < 1e-5f && std::fabs(atRest.y - 2.0f) < 1e-5f);
        // Extreme rotation: 180 degrees about x (point goes to x, -y, -z).
        const std::vector<Mat4> flipped = {Mat4::TRS({0, 0, 0}, shape::EulerToQuat({180, 0, 0}), {1, 1, 1})};
        Vec3 p = SkinPoint(v, flipped);
        CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
        CHECK(std::fabs(p.x - 1.0f) < 1e-4f && std::fabs(p.y + 2.0f) < 1e-4f && std::fabs(p.z + 3.0f) < 1e-4f);
    });

    test("heel config: Stiletto text parses, angles fold past 60, missing keys default to 0", [] {
        using namespace assemble;
        HeelConfig c;
        CHECK(ParseStilettoHeel("angleAnkle=15\r\nangleLeg=0\r\nheight=0.028\r\n", c));
        CHECK(std::fabs(c.ankleDeg - 15) < 1e-5f && std::fabs(c.legDeg) < 1e-5f && std::fabs(c.heightMeters - 0.028f) < 1e-6f);
        CHECK(std::fabs(c.toeDeg) < 1e-5f && std::fabs(c.shoeScaleX - 1) < 1e-5f);   // absent -> 0 / 1
        CHECK(ParseStilettoHeel("angleAnkle=350\nheight=0.01", c));   // 350 -> -(360-350) = -10
        CHECK(std::fabs(c.ankleDeg + 10) < 1e-4f);
        HeelConfig none;
        CHECK(!ParseStilettoHeel("hello\nworld=1", none));   // no heel keys
        CHECK(std::fabs(HeelAngleInRange(45) - 45) < 1e-6f && std::fabs(HeelAngleInRange(350) + 10) < 1e-4f);
    });

    test("heel config: known-table names round-trip, flat shoes stay flat", [] {
        using namespace assemble;
        HeelConfig hc;
        CHECK(KnownHeelConfig("HM25", hc) && std::fabs(hc.ankleDeg - 15) < 1e-5f && std::fabs(hc.heightMeters - 0.028f) < 1e-6f);
        CHECK(KnownHeelConfig("High Heeled Gal Sandals", hc) && std::fabs(hc.ankleDeg - 30) < 1e-5f && std::fabs(hc.legDeg - 16) < 1e-5f);
        CHECK(KnownHeelConfig("Middle Heel Pumps", hc) && std::fabs(hc.ankleDeg) < 1e-5f && std::fabs(hc.heightMeters - 0.018f) < 1e-6f);   // height-only
        CHECK(!KnownHeelConfig("no such shoe", hc));
        CHECK(KnownHeelTable().size() == 50);
        for (const auto& [n, v] : KnownHeelTable()) CHECK(!n.empty() && v.hasConfig);
    });

    test("shoe warp: identity, uniform/non-uniform scale, rotation, translate, shear (Stiletto order)", [] {
        using namespace assemble;
        auto near = [](Vec3 a, Vec3 b, float eps = 1e-4f) { return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps; };
        HeelConfig id;
        CHECK(ShoeWarpIsIdentity(id));
        CHECK(near(ShoeWarpMatrix(id).point({1, 2, 3}), {1, 2, 3}));
        // Ankle pitch alone cancels in the warp (R(-a)*I*I*I*R(a) = I): pose is separate.
        HeelConfig pitched; pitched.ankleDeg = 10;
        CHECK(ShoeWarpIsIdentity(pitched));   // warp fields identity: the ankle cancels out
        CHECK(near(ShoeWarpMatrix(pitched).point({1, 2, 3}), {1, 2, 3}, 1e-4f));
        HeelConfig uni; uni.shoeScaleX = uni.shoeScaleY = uni.shoeScaleZ = 2;
        CHECK(!ShoeWarpIsIdentity(uni) && near(ShoeWarpMatrix(uni).point({1, 1, 1}), {2, 2, 2}));
        HeelConfig non; non.shoeScaleX = 2; non.shoeScaleY = 3; non.shoeScaleZ = 4;
        CHECK(near(ShoeWarpMatrix(non).point({1, 1, 1}), {2, 3, 4}));
        HeelConfig rot; rot.shoeAngleDeg = 90;
        CHECK(near(ShoeWarpMatrix(rot).point({0, 1, 0}), {0, 0, 1}, 1e-4f));   // +90 about X: +Y -> +Z
        HeelConfig tr; tr.shoeTranslateY = 0.5f; tr.shoeTranslateZ = -0.25f;
        CHECK(near(ShoeWarpMatrix(tr).point({1, 1, 1}), {1, 1.5f, 0.75f}));
        HeelConfig shy; shy.shoeShearY = 45;   // tan(45)=1: Y += Z
        CHECK(near(ShoeWarpMatrix(shy).point({0, 0, 1}), {0, 1, 1}, 1e-4f));
        HeelConfig shz; shz.shoeShearZ = 45;   // tan(45)=1: Z += Y
        CHECK(near(ShoeWarpMatrix(shz).point({0, 1, 0}), {0, 1, 1}, 1e-4f));
        // Combined: S then R2 (scale before rotation in bind space): scale X by 2, rotate 90.
        HeelConfig comb; comb.shoeScaleX = 2; comb.shoeAngleDeg = 90;
        // (1,0,0) scaled -> (2,0,0), rotated about X by 90 -> (2,0,0) (on axis).
        CHECK(near(ShoeWarpMatrix(comb).point({1, 0, 0}), {2, 0, 0}, 1e-4f));
        // (0,1,0) scaled -> (0,1,0), rotated -> (0,0,1).
        CHECK(near(ShoeWarpMatrix(comb).point({0, 1, 0}), {0, 0, 1}, 1e-4f));
    });

    test("heel config: parser edge cases (BOM, trim, duplicates, malformed, non-finite, shoe* not folded)", [] {
        using namespace assemble;
        HeelConfig c;
        // UTF-8 BOM, spaces around key/value, \r-only separators, duplicate key (last wins, as in the original Trim+SetValue).
        std::string bom = "\xEF\xBB\xBF"; bom += " angleAnkle = 5\ragleWanted=1\nangleAnkle=20\nheight=  0.02 ";
        CHECK(ParseStilettoHeel(bom, c));
        CHECK(std::fabs(c.ankleDeg - 20) < 1e-5f && std::fabs(c.heightMeters - 0.02f) < 1e-6f);
        // Zero scale means 1 (Setup), malformed numeric leaves the field at default without failing the file.
        CHECK(ParseStilettoHeel("shoeScaleX=0\nangleAnkle=abc\nheight=0.01", c));
        CHECK(std::fabs(c.shoeScaleX - 1) < 1e-5f && std::fabs(c.ankleDeg) < 1e-5f && std::fabs(c.heightMeters - 0.01f) < 1e-6f);
        // Non-finite values are rejected, never stored.
        CHECK(!ParseStilettoHeel("angleAnkle=inf\nheight=nan", c));
        // shoeAngle / shoeShear are raw degrees in the original (no GetAngleInRange): 350 stays 350, shear 350 stays 350.
        CHECK(ParseStilettoHeel("shoeAngle=350\nshoeShearY=350", c));
        CHECK(std::fabs(c.shoeAngleDeg - 350) < 1e-4f && std::fabs(c.shoeShearY - 350) < 1e-4f);
        // Angle folding itself: only ankle/toe/leg fold.
        CHECK(ParseStilettoHeel("angleToes=350", c) && std::fabs(c.toeDeg + 10) < 1e-4f);
    });

    test("heel angles: folding boundaries and warp finiteness at extremes", [] {
        using namespace assemble;
        auto nearf = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
        CHECK(nearf(HeelAngleInRange(0), 0));
        CHECK(nearf(HeelAngleInRange(60), 60));            // boundary: not folded (60.001 threshold)
        CHECK(nearf(HeelAngleInRange(60.001f), 60.001f));
        CHECK(nearf(HeelAngleInRange(61), -299));           // -(360-61)
        CHECK(nearf(HeelAngleInRange(90), -270));
        CHECK(nearf(HeelAngleInRange(180), -180));
        CHECK(nearf(HeelAngleInRange(270), -90));
        CHECK(nearf(HeelAngleInRange(350), -10));           // folds to the equivalent negative pitch
        CHECK(nearf(HeelAngleInRange(360), 0));
        CHECK(nearf(HeelAngleInRange(-90), -90));           // negative: never folded (original behaviour)
        CHECK(nearf(HeelAngleInRange(-350), -350));         // trig-equivalent to +10; not folded
        // Every finite input stays finite through the warp trig.
        for (float d : {0.0f, 60.0f, 180.0f, 270.0f, 360.0f, -180.0f, 720.0f, 350.0f}) {
            HeelConfig h; h.ankleDeg = d; h.legDeg = d; h.shoeAngleDeg = d;
            auto m = ShoeWarpMatrix(h);
            for (float v : m.m) CHECK(std::isfinite(v));
        }
        // Shear at 90 degrees: tan is huge but finite (no NaN/inf at the pole).
        HeelConfig she; she.shoeShearY = 90; she.shoeShearZ = -90;
        for (float v : ShoeWarpMatrix(she).m) CHECK(std::isfinite(v));
    });

    test("shoe warp: skinned deformation and single application", [] {
        using namespace assemble;
        auto near = [](Vec3 a, Vec3 b, float eps = 1e-4f) { return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps; };
        // Simulate the skinning stage: bone world is identity, bindpose moves a bind-space vertex.
        // Identity warp must be exactly the unwarped result (no regression, PMX bytes unchanged).
        HeelConfig id;
        Mat4 bind = Mat4::TRS({0.1f, 0.2f, 0.3f}, Quat{0, 0, 0, 1}, {1, 1, 1});
        Vec3 pt{0.5f, -0.25f, 1.0f};
        Vec3 unwarped = bind.point(pt);
        Vec3 warped = (ShoeWarpMatrix(id) * bind).point(pt);
        CHECK(near(warped, unwarped, 1e-5f));
        // Non-identity scale visibly deforms and stays finite (bind-space warp, then skinning).
        HeelConfig sc; sc.shoeScaleY = 2;
        Vec3 scaled = (ShoeWarpMatrix(sc) * bind).point(pt);
        CHECK(!near(scaled, unwarped) && std::isfinite(scaled.y));
        // Applying the warp twice is not the same as once: the pipeline must bake it once (into locals).
        HeelConfig tr; tr.shoeTranslateY = 0.5f;
        Mat4 once = ShoeWarpMatrix(tr);
        Mat4 twice = once * once;
        CHECK(!near(twice.point({0, 0, 0}), once.point({0, 0, 0}), 1e-3f));
        CHECK(near(once.point({0, 0, 0}), {0, 0.5f, 0}));
        CHECK(near(twice.point({0, 0, 0}), {0, 1.0f, 0}));
        // Ankle pitch cancels in the warp itself (pose is separate): warp with pitch + scale is scale-only for Y-from-origin.
        HeelConfig ps; ps.ankleDeg = 20; ps.shoeScaleX = 3;
        // (1,0,0) is on the X axis: warp leaves it at (3,0,0) regardless of the ankle pitch.
        CHECK(near(ShoeWarpMatrix(ps).point({1, 0, 0}), {3, 0, 0}, 1e-3f));
    });

    test("heel pose: flat shoe geometry triggers nothing, heeled geometry lifts and pitches", [] {
        // Direct settleHeels is exercised end-to-end (Assemble path); here check detection
        // thresholds via a tiny synthetic shoe: a level sole with no drop must not warn, and
        // a shoe with a 2 cm heel stack must warn once per foot. The full pose maths are
        // covered by the MMD audit + conversion validation in the session notes.
        using namespace assemble;
        Scene s;
        s.bones.push_back({"cf_j_leg01_L", -1, {0, 0.5f, 0}, "rig"});
        s.bones.push_back({"cf_j_leg03_L", 0, {0, 0.1f, 0}, "rig"});
        s.bones.push_back({"cf_j_foot_L", 1, {0, 0.05f, 0}, "rig"});
        s.bones.push_back({"cf_j_toes_L", 2, {0, 0.04f, 0.12f}, "rig"});
        s.bones.push_back({"cf_j_leg01_R", -1, {0.1f, 0.5f, 0}, "rig"});
        s.bones.push_back({"cf_j_leg03_R", 4, {0.1f, 0.1f, 0}, "rig"});
        s.bones.push_back({"cf_j_foot_R", 5, {0.1f, 0.05f, 0}, "rig"});
        s.bones.push_back({"cf_j_toes_R", 6, {0.1f, 0.04f, 0.12f}, "rig"});
        // Flat sole at Y=0 (level): toeLow == heelLow -> height 0 -> no pose, no warning.
        SceneMesh flat;
        flat.name = "flatshoes";
        for (int i = 0; i < 60; ++i) {
            SceneVertex v;
            v.pos = {i % 2 ? 0.08f : -0.08f, 0.0f, (i / 10) * 0.02f};
            v.normal = {0, 1, 0};
            v.bone[0] = 2; v.weight[0] = 1;
            if (i % 3 == 0) { v.bone[0] = 3; }
            flat.vertices.push_back(v);
        }
        s.meshes.push_back(flat);
        ScenePart p;
        p.id = "outfit0.shoes_outer"; p.group = "Outfit"; p.includeInExport = true; p.visibleInPreview = true;
        p.meshes = {0};
        s.parts.push_back(p);
        assemble::SettleHeels(s);
        CHECK(s.issues.empty());
        // Heeled version: a heel stack under the rear sole on the L side (x<0 is L):
        // top of the stack just below the sole, bottom 2 cm lower.
        for (int i = 0; i < 10; ++i) {
            SceneVertex top; top.pos = {-0.08f, -0.01f, 0.01f + i * 0.001f}; top.normal = {0, 1, 0};
            top.bone[0] = 2; top.weight[0] = 1;
            SceneVertex bot; bot.pos = {-0.08f, -0.03f, 0.03f + i * 0.001f}; bot.normal = {0, 1, 0};
            bot.bone[0] = 2; bot.weight[0] = 1;
            s.meshes[0].vertices.push_back(top);
            s.meshes[0].vertices.push_back(bot);
        }
        assemble::SettleHeels(s);
        CHECK(s.issues.size() == 1);   // one side (L only: X side selection)
        float lo = 1e30f;
        for (const auto& v : s.meshes[0].vertices) lo = std::min(lo, v.pos.y);
        CHECK(std::fabs(lo) < 1e-5f);
        // Repeated calls without a re-shape must not accumulate (toe/heel already aligned).
        auto poseOnce = s.bones[2].pos;
        float yOnce = s.meshes[0].vertices[0].pos.y;
        assemble::SettleHeels(s);
        CHECK(std::fabs(s.bones[2].pos.x - poseOnce.x) < 1e-6f && std::fabs(s.bones[2].pos.y - poseOnce.y) < 1e-6f);
        CHECK(std::fabs(s.meshes[0].vertices[0].pos.y - yOnce) < 1e-6f);
    });

    // Tiny rig + flat sole: reused by the following pose cases.
    static auto makeBareRig = [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"cf_j_leg01_L", -1, {0, 0.5f, 0}, "rig"});
        s.bones.push_back({"cf_j_leg03_L", 0, {0, 0.1f, 0}, "rig"});
        s.bones.push_back({"cf_j_foot_L", 1, {0, 0.05f, 0}, "rig"});
        s.bones.push_back({"cf_j_toes_L", 2, {0, 0.04f, 0.12f}, "rig"});
        s.bones.push_back({"cf_j_leg01_R", -1, {0.1f, 0.5f, 0}, "rig"});
        s.bones.push_back({"cf_j_leg03_R", 4, {0.1f, 0.1f, 0}, "rig"});
        s.bones.push_back({"cf_j_foot_R", 5, {0.1f, 0.05f, 0}, "rig"});
        s.bones.push_back({"cf_j_toes_R", 6, {0.1f, 0.04f, 0.12f}, "rig"});
        SceneMesh flat;
        flat.name = "flatshoes";
        for (int i = 0; i < 60; ++i) {
            SceneVertex v;
            v.pos = {i % 2 ? 0.08f : -0.08f, 0.0f, (i / 10) * 0.02f};
            v.normal = {0, 1, 0};
            v.bone[0] = 2; v.weight[0] = 1;
            if (i % 3 == 0) v.bone[0] = 3;
            flat.vertices.push_back(v);
        }
        s.meshes.push_back(flat);
        ScenePart p;
        p.id = "outfit0.shoes_outer"; p.group = "Outfit"; p.includeInExport = true; p.visibleInPreview = true;
        p.meshes = {0};
        s.parts.push_back(p);
        return s;
    };

    test("heel pose: barefoot, height-only platform, flat-config suppression and hidden shoe", [] {
        using namespace assemble;
        // Barefoot: no shoe part at all -> nothing happens.
        {
            Scene s;
            s.bones.push_back({"cf_j_leg01_L", -1, {0, 0.5f, 0}, "rig"});
            s.bones.push_back({"cf_j_leg03_L", 0, {0, 0.1f, 0}, "rig"});
            s.bones.push_back({"cf_j_foot_L", 1, {0, 0.05f, 0}, "rig"});
            s.bones.push_back({"cf_j_toes_L", 2, {0, 0.04f, 0.12f}, "rig"});
            s.bones.push_back({"cf_j_leg01_R", -1, {0.1f, 0.5f, 0}, "rig"});
            s.bones.push_back({"cf_j_leg03_R", 4, {0.1f, 0.1f, 0}, "rig"});
            s.bones.push_back({"cf_j_foot_R", 5, {0.1f, 0.05f, 0}, "rig"});
            s.bones.push_back({"cf_j_toes_R", 6, {0.1f, 0.04f, 0.12f}, "rig"});
            SettleHeels(s);
            CHECK(s.issues.empty());
        }
        // Height-only platform (Geta 0/0/30mm): lifts the whole model to y=0, no pitch.
        {
            Scene s = makeBareRig();
            s.bones[1].pos = {0, 0.1f, 0};   // leg03 pivot (L) raised; foot stays
            s.parts[0].hasHeelConfig = true;
            s.parts[0].heelAnkleDeg = 0; s.parts[0].heelLegDeg = 0; s.parts[0].heelToeDeg = 0;
            s.parts[0].heelHeightMeters = 0.03f;
            s.parts[0].heelConfigSource = "zipmod test (Stiletto)";
            auto footBefore = s.bones[2].pos;
            SettleHeels(s);
            CHECK(std::fabs(s.bones[2].pos.x - footBefore.x) < 1e-5f);   // no pitch about the ankle
            CHECK(std::fabs(s.bones[2].pos.z - footBefore.z) < 1e-5f);
            // Ground contact: lowest vertex at y=0 after the lift.
            float lo = 1e30f;
            for (const auto& v : s.meshes[0].vertices) lo = std::min(lo, v.pos.y);
            CHECK(std::fabs(lo) < 1e-5f);
            CHECK(s.issues.size() == 2);   // one per side
            for (const auto& v : s.meshes[0].vertices) CHECK(std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z));
        }
        // Flat in config (0/0/0) suppresses geometry: even a heeled shape does not pose.
        {
            Scene s = makeBareRig();
            for (int i = 0; i < 10; ++i) {
                SceneVertex top; top.pos = {-0.08f, -0.01f, 0.01f + i * 0.001f}; top.normal = {0, 1, 0};
                top.bone[0] = 2; top.weight[0] = 1;
                SceneVertex bot; bot.pos = {-0.08f, -0.03f, 0.03f + i * 0.001f}; bot.normal = {0, 1, 0};
                bot.bone[0] = 2; bot.weight[0] = 1;
                s.meshes[0].vertices.push_back(top);
                s.meshes[0].vertices.push_back(bot);
            }
            s.parts[0].hasHeelConfig = true;   // zeros: explicitly flat
            s.parts[0].heelHeightMeters = 0;
            s.parts[0].heelConfigSource = "known test (flat)";
            auto yBefore = s.meshes[0].vertices[0].pos.y;
            SettleHeels(s);
            CHECK(s.issues.empty());
            CHECK(std::fabs(s.meshes[0].vertices[0].pos.y - yBefore) < 1e-6f);
        }
        // Hidden heeled shoe is ignored: hiding the shoe goes flat again on the re-pose.
        {
            Scene s = makeBareRig();
            for (int i = 0; i < 10; ++i) {
                SceneVertex top; top.pos = {-0.08f, -0.01f, 0.01f + i * 0.001f}; top.normal = {0, 1, 0};
                top.bone[0] = 2; top.weight[0] = 1;
                SceneVertex bot; bot.pos = {-0.08f, -0.03f, 0.03f + i * 0.001f}; bot.normal = {0, 1, 0};
                bot.bone[0] = 2; bot.weight[0] = 1;
                s.meshes[0].vertices.push_back(top);
                s.meshes[0].vertices.push_back(bot);
            }
            s.parts[0].visibleInPreview = false;
            auto yBefore = s.meshes[0].vertices[0].pos.y;
            SettleHeels(s);
            CHECK(s.issues.empty());
            CHECK(std::fabs(s.meshes[0].vertices[0].pos.y - yBefore) < 1e-6f);
        }
    });

    test("heel pose: inner flat + outer heeled picks the outer only; extreme config stays finite", [] {
        using namespace assemble;
        Scene s = makeBareRig();
        // Heeled stack on the L side (same shape as the earlier threshold test).
        for (int i = 0; i < 10; ++i) {
            SceneVertex top; top.pos = {-0.08f, -0.01f, 0.01f + i * 0.001f}; top.normal = {0, 1, 0};
            top.bone[0] = 2; top.weight[0] = 1;
            SceneVertex bot; bot.pos = {-0.08f, -0.03f, 0.03f + i * 0.001f}; bot.normal = {0, 1, 0};
            bot.bone[0] = 2; bot.weight[0] = 1;
            s.meshes[0].vertices.push_back(top);
            s.meshes[0].vertices.push_back(bot);
        }
        // Inner slot explicitly flat, outer slot geometry -> outer still poses.
        ScenePart inner; inner.id = "outfit0.shoes_inner"; inner.group = "Outfit"; inner.includeInExport = true; inner.visibleInPreview = true;
        inner.meshes = {0}; inner.hasHeelConfig = true; inner.heelHeightMeters = 0; inner.heelConfigSource = "known flat";
        s.parts.push_back(inner);
        SettleHeels(s);
        CHECK(s.issues.size() == 1);
        CHECK(s.issues[0].message.find("outfit0.shoes_outer") != std::string::npos);
        // Extreme config pitch (350 -> -10 after folding) stays finite end to end.
        Scene e = makeBareRig();
        e.parts[0].hasHeelConfig = true;
        e.parts[0].heelAnkleDeg = 350;
        e.parts[0].heelHeightMeters = 0.02f;
        e.parts[0].heelConfigSource = "zipmod test";
        SettleHeels(e);
        for (const auto& m : e.meshes) for (const auto& v : m.vertices)
            CHECK(std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z));
        for (const auto& b : e.bones) CHECK(std::isfinite(b.pos.x) && std::isfinite(b.pos.y) && std::isfinite(b.pos.z));
        CHECK(exporter::ValidateScene(e).empty());
    });

    test("heel pose: vertical stack still pitches to flush sole (contact, not stack lean)", [] {
        using namespace assemble;
        Scene s = makeBareRig();
        // Vertical heel column under the rear sole: top and bottom share the same z, so a
        // stack-lean estimate reads ~0 and would skip. The sole must still be pitched so the
        // heel block and the toe ball sit on the same ground plane ("straight to the
        // bottom plane" — toe and heel contacts flush, not one floating).
        for (int i = 0; i < 10; ++i) {
            SceneVertex top; top.pos = {-0.08f, -0.01f, 0.01f}; top.normal = {0, 1, 0};
            top.bone[0] = 2; top.weight[0] = 1;
            SceneVertex bot; bot.pos = {-0.08f, -0.03f, 0.01f}; bot.normal = {0, 1, 0};
            bot.bone[0] = 2; bot.weight[0] = 1;
            s.meshes[0].vertices.push_back(top);
            s.meshes[0].vertices.push_back(bot);
        }
        // Representative contact vertices as in production: heel = global lowest,
        // toe = lowest in the front band. Track them through the pose.
        float heelY0 = 1e30f, toeY0 = 1e30f;
        size_t heelIdx = 0, toeIdx = 0;
        float maxZ = 0;
        for (const auto& v : s.meshes[0].vertices) maxZ = std::max(maxZ, v.pos.z);
        for (size_t i = 0; i < s.meshes[0].vertices.size(); ++i) {
            const auto& v = s.meshes[0].vertices[i];
            if (v.pos.y < heelY0) { heelY0 = v.pos.y; heelIdx = i; }
            if (v.pos.z > 0.6f * maxZ && v.pos.y < toeY0) { toeY0 = v.pos.y; toeIdx = i; }
        }
        CHECK(heelIdx < s.meshes[0].vertices.size() && toeIdx < s.meshes[0].vertices.size() && heelIdx != toeIdx);
        SettleHeels(s);
        CHECK(!s.issues.empty());
        CHECK(s.issues[0].message.find("sole contact") != std::string::npos);
        const auto& after = s.meshes[0].vertices;
        float heelY = after[heelIdx].pos.y;
        float toeY = after[toeIdx].pos.y;
        float lo = 1e30f;
        for (const auto& v : after) lo = std::min(lo, v.pos.y);
        CHECK(std::fabs(lo) < 1e-5f);   // lifted onto the ground plane
        CHECK(std::fabs(heelY - toeY) < 0.003f);   // both contacts flush
        CHECK(heelY > -1e-5f && toeY > -1e-5f);
    });

    // ---- Material Workbench (Stage 1) ----
    auto makeMatScene = [] {
        using namespace assemble;
        Scene s;
        s.name = "matfixture";
        s.bones.push_back({"root", -1, {0, 0, 0}, "rig"});
        SceneTexture t;
        t.name = "main";
        t.width = 2; t.height = 2; t.rgba.assign(16, 255);
        s.textures.push_back(t);
        SceneMaterial a;
        a.name = "mf_m_top";
        a.shader = "Shader Forge/main_item";
        a.texture = 0;
        a.diffuse = {0.9f, 0.9f, 0.9f, 1.0f};
        a.props.emplace_back("_Color", Vec4{0.9f, 0.9f, 0.9f, 1.0f});
        a.props.emplace_back("_SpecularColor", Vec4{0.1f, 0.1f, 0.1f, 1.0f});
        a.props.emplace_back("_SpecularPower", Vec4{0.2f, 0, 0, 0});
        a.texProps.push_back({"_MainTex", 0, {1, 1, 0, 0}});
        a.specular = {0.02f, 0.02f, 0.02f};
        s.materials.push_back(a);
        SceneMaterial b;
        b.name = "cf_m_hair";
        b.shader = "Shader Forge/main_hair";
        b.diffuse = {0.2f, 0.2f, 0.5f, 1.0f};
        b.props.emplace_back("_Color", Vec4{0.2f, 0.2f, 0.5f, 1.0f});
        s.materials.push_back(b);
        auto vert = [](SceneMesh& m, float x) { SceneVertex v; v.pos = {x, 0, 0}; v.normal = {0, 0, 1}; v.uv = {0.5f, 0.5f}; m.vertices.push_back(v); };
        SceneMesh m0; vert(m0, 0); vert(m0, 1); vert(m0, 0.5f);
        m0.submeshes.push_back({0, {0, 1, 2}});
        s.meshes.push_back(m0);
        vert(m0, 0); vert(m0, 1); vert(m0, 0.5f);
        m0.submeshes[0].material = 1;
        s.meshes.push_back(m0);
        ScenePart p0; p0.id = "outfit0.top"; p0.group = "Outfit"; p0.meshes = {0}; s.parts.push_back(p0);
        ScenePart p1; p1.id = "hair.back"; p1.group = "Hair"; p1.meshes = {1}; s.parts.push_back(p1);
        return s;
    };

    test("workbench: classification guesses old shader names, Generic is the fallback", [&] {
        using namespace material;
        CHECK(ClassifyMaterial("Shader Forge/main_skin", "cf_m_body") == MaterialClass::Skin);
        CHECK(ClassifyMaterial("Shader Forge/main_hair", "cf_m_hair") == MaterialClass::Hair);
        CHECK(ClassifyMaterial("Shader Forge/main_item", "mf_m_top") == MaterialClass::Fabric);
        CHECK(ClassifyMaterial("Shader Forge/main_opaque", "x") == MaterialClass::Fabric);
        CHECK(ClassifyMaterial("Shader Forge/main_alpha", "x") == MaterialClass::Fabric);
        CHECK(ClassifyMaterial("Shader Forge/toon_eye", "x") == MaterialClass::Eyes);
        CHECK(ClassifyMaterial("Shader Forge/toon_nose", "x") == MaterialClass::Eyelashes);
        CHECK(ClassifyMaterial("Shader Forge/glasses", "cf_m_lens") == MaterialClass::Glass);
        CHECK(ClassifyMaterial("Shader Forge/mnpb", "x") == MaterialClass::Generic);
        CHECK(ClassifyMaterial("unknown shader", "thing") == MaterialClass::Generic);
        MaterialClass c;
        CHECK(ClassFromName("Fabric/Default", c) && c == MaterialClass::Fabric);
        CHECK(ClassFromName("glass", c) && c == MaterialClass::Glass);
        CHECK(!ClassFromName("not-a-class", c));
        CHECK(std::string(ClassName(MaterialClass::Eyelashes)) == "Eyelashes");
    });

    test("workbench: presets set shading but never overwrite the colour", [&] {
        using namespace material;
        for (MaterialClass cls : {MaterialClass::Skin, MaterialClass::Fabric, MaterialClass::Hair, MaterialClass::Eyes, MaterialClass::Eyelashes, MaterialClass::Metal, MaterialClass::Plastic, MaterialClass::Leather, MaterialClass::Glass, MaterialClass::Rubber, MaterialClass::Generic}) {
            MaterialOverride p = PresetForClass(cls);
            CHECK(p.class_ && *p.class_ == cls);
            CHECK(!p.baseColor.has_value());
            CHECK(p.preset && *p.preset == PresetName(cls));
        }
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        ApplyPreset(st, 0, MaterialClass::Metal);
        ApplyOverrides(s, st);
        CHECK(s.materials[0].diffuse.x == 0.9f);   // colour untouched
        CHECK(st.overrides[0]->roughness && std::fabs(*st.overrides[0]->roughness - 0.25f) < 1e-6f);
        // Effective view reflects the preset.
        EffectiveMaterial e = EffectiveOf(st, 0);
        CHECK(e.cls == MaterialClass::Metal && e.metallic == 1.0f && std::fabs(e.roughness - 0.25f) < 1e-5f);
        // Fabric preset carries transmission; skin carries subsurface.
        CHECK(PresetForClass(MaterialClass::Fabric).transmission && std::fabs(PresetForClass(MaterialClass::Fabric).transmission->strength - 0.25f) < 1e-6f);
        CHECK(PresetForClass(MaterialClass::Skin).subsurface && std::fabs(PresetForClass(MaterialClass::Skin).subsurface->strength - 0.35f) < 1e-6f);
    });

    test("workbench: base colour and opacity write diffuse and _Color, nothing else", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o;
        o.baseColor = Vec4{0.2f, 0.4f, 0.6f, 0.8f};
        o.opacity = 0.5f;
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        CHECK(std::fabs(s.materials[0].diffuse.x - 0.2f) < 1e-6f && std::fabs(s.materials[0].diffuse.w - 0.5f) < 1e-6f);
        bool colorOk = false;
        for (const auto& pr : s.materials[0].props) if (pr.first == "_Color") colorOk = std::fabs(pr.second.x - 0.2f) < 1e-6f && std::fabs(pr.second.w - 0.5f) < 1e-6f;
        CHECK(colorOk);
        CHECK(s.materials[1].diffuse.z == 0.5f);   // the other material is untouched
        CHECK(st.IsDirty() && st.OverrideCount() == 1);
    });

    test("workbench: the base colour is _Color when stored (diffuse stays white in a real scene), and is written even when absent", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        // A real assembled material keeps its colour in _Color; diffuse is the
        // PMX-facing fallback and stays white. The inspector must show _Color.
        s.materials[1].diffuse = {1, 1, 1, 1};
        WorkbenchState st;
        CaptureOriginals(st, s);
        EffectiveMaterial e = EffectiveOf(st, 1);
        CHECK(std::fabs(e.baseColor.x - 0.2f) < 1e-6f && std::fabs(e.baseColor.z - 0.5f) < 1e-6f);
        CHECK(std::fabs(e.opacity - 1.0f) < 1e-6f);
        // A create-job material (skin, outfit) has no stored _Color: an edit
        // must add it, or the game shader sees no tint at all.
        for (auto& pr : s.materials[1].props) if (pr.first == "_Color") pr.first = "_Removed";
        WorkbenchState st2;
        CaptureOriginals(st2, s);
        MaterialOverride o;
        o.baseColor = Vec4{0.25f, 0.5f, 0.75f, 1.0f};
        SetOverride(st2, 1, o);
        ApplyOverrides(s, st2);
        bool added = false;
        for (const auto& pr : s.materials[1].props) if (pr.first == "_Color") added = std::fabs(pr.second.x - 0.25f) < 1e-6f && std::fabs(pr.second.z - 0.75f) < 1e-6f;
        CHECK(added);
    });

    test("workbench: HSV round trip and known colours (colour picker maths)", [&] {
        using namespace material;
        auto close3 = [](const Vec3& a, const Vec3& b) {
            return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f && std::fabs(a.z - b.z) < 1e-5f;
        };
        CHECK(close3(RgbToHsv(Vec3{1, 0, 0}), Vec3{0, 1, 1}));
        CHECK(close3(RgbToHsv(Vec3{0, 1, 0}), Vec3{1.0f / 3.0f, 1, 1}));
        CHECK(close3(RgbToHsv(Vec3{0, 0, 1}), Vec3{2.0f / 3.0f, 1, 1}));
        CHECK(close3(RgbToHsv(Vec3{0, 0, 0}), Vec3{0, 0, 0}));
        CHECK(close3(RgbToHsv(Vec3{1, 1, 1}), Vec3{0, 0, 1}));
        CHECK(close3(RgbToHsv(Vec3{0.5f, 0.5f, 0.5f}), Vec3{0, 0, 0.5f}));
        // h wraps: 1.0 is red again, negatives wrap too.
        CHECK(close3(HsvToRgb(1.0f, 1, 1), Vec3{1, 0, 0}));
        CHECK(close3(HsvToRgb(-0.25f, 1, 1), Vec3{0.5f, 0, 1}));
        for (const Vec3 rgb : {Vec3{1, 0, 0}, Vec3{0.2f, 0.6f, 0.9f}, Vec3{0.9f, 0.8f, 0.2f}, Vec3{0.25f, 0.25f, 0.3f}}) {
            const Vec3 hsv = RgbToHsv(rgb);
            CHECK(close3(HsvToRgb(hsv.x, hsv.y, hsv.z), rgb));
        }
        // Out-of-range inputs are clamped, never produce garbage.
        CHECK(close3(HsvToRgb(0, 2.0f, -1.0f), Vec3{0, 0, 0}));
        CHECK(close3(RgbToHsv(Vec3{2, -1, 0.5f}), RgbToHsv(Vec3{1, 0, 0.5f})));
    });

    test("workbench: MakeTintedTexture multiplies RGBA, clamps and rehashes the pixels", [&] {
        using namespace material;
        assemble::SceneTexture t;
        t.name = "skin";
        t.width = 2; t.height = 1;
        t.rgba = {100, 150, 200, 255, 10, 20, 30, 128};
        t.pixelHash = 1234;
        const assemble::SceneTexture out = MakeTintedTexture(t, Vec4{0.5f, 2.0f, 0.0f, 0.5f});
        CHECK(out.width == 2 && out.height == 1 && out.rgba.size() == 8);
        CHECK(out.rgba[0] == 50 && out.rgba[1] == 255 && out.rgba[2] == 0 && out.rgba[3] == 128);
        // 10*0.5 = 5; 20*2 = 40; 30*0 = 0; alpha 128*0.5 = 64.
        CHECK(out.rgba[4] == 5 && out.rgba[5] == 40 && out.rgba[6] == 0 && out.rgba[7] == 64);
        // The copy owns fresh pixels: the source is untouched, and the hash
        // identifies the new pixels (so a consumer can tell the copy changed).
        CHECK(t.rgba[0] == 100 && t.rgba[1] == 150 && t.rgba[2] == 200 && t.pixelHash == 1234);
        CHECK(out.pixelHash != 0 && out.pixelHash != t.pixelHash);
        CHECK(out.pixelHash == Fnv1a64(out.rgba.data(), out.rgba.size()));
        // Identity ratio keeps the values exactly.
        const assemble::SceneTexture same = MakeTintedTexture(t, Vec4{1, 1, 1, 1});
        CHECK(same.rgba == t.rgba);
        // Malformed pixel data is returned as-is instead of read out of bounds.
        assemble::SceneTexture broken;
        broken.name = "broken"; broken.width = 4; broken.height = 4; broken.rgba = {1, 2, 3};
        const assemble::SceneTexture kept = MakeTintedTexture(broken, Vec4{0, 0, 0, 0});
        CHECK(kept.rgba == broken.rgba);
        // The tint copy records its own format so the inspector does not repeat DXT5.
        CHECK(out.format == "RGBA8 (tint copy)");
    });

    test("workbench: Unity format names round-trip to the inspector string", [] {
        CHECK(unity::FormatName(1) == "Alpha8");
        CHECK(unity::FormatName(3) == "RGB24");
        CHECK(unity::FormatName(4) == "RGBA32");
        CHECK(unity::FormatName(12) == "DXT5");
        CHECK(unity::FormatName(99) == "UnityFormat 99");
    });

    test("workbench: UV diagnostics flag missing, out-of-range, stretch, overlap, seams", [] {
        using namespace assemble;
        auto mkVert = [](float px, float py, float u, float v) {
            SceneVertex s; s.pos = {px, py, 0}; s.normal = {0, 0, 1}; s.uv = {u, v}; return s;
        };
        // One small open sheet (two triangles sharing an edge) + one isolated
        // triangle with the same UV extent (overlap) + one NaN vertex.
        SceneMesh m;
        m.vertices.push_back(mkVert(0, 0, 0, 0));
        m.vertices.push_back(mkVert(1, 0, 1, 0));
        m.vertices.push_back(mkVert(0, 1, 0, 1));
        m.vertices.push_back(mkVert(1, 1, 1, 1));
        m.vertices.push_back(mkVert(2, 0, std::nan(""), 0.5f));
        m.vertices.push_back(mkVert(3, 0, 1.5f, 0.5f));   // out of range (u=1.5)
        m.vertices.push_back(mkVert(2, 1, 0.5f, 0.5f));
        SceneSubmesh sub;
        sub.material = 0;
        sub.indices = {0, 1, 2, 0, 1, 3, 3, 4, 5, 4, 6, 5};
        m.submeshes.push_back(sub);
        CHECK(material::CountMissingUv(m) == 1);
        CHECK(material::FindMissingUv(m).size() == 1 && material::FindMissingUv(m)[0] == 4);
        float worst = 0;
        CHECK(material::CountOutOfRange(m, 0.02f, worst) == 1 && std::fabs(worst - 0.48f) < 1e-4f);
        // With a zero margin the same vertex still counts; with 0.6 it does not.
        CHECK(material::CountOutOfRange(m, 0.6f, worst) == 0);
        const material::UvReport rep = material::DiagnoseUv(m, 1024, 1024);
        CHECK(rep.missingUvVerts == 1 && rep.outOfRangeVerts == 1);
        CHECK(rep.triangles == 4);
        CHECK(rep.islands == 1);            // all four share a vertex chain (0-1-3-4-5)
        CHECK(rep.overlapPairs == 0);       // one island: nothing to overlap with
        // Two disjoint islands with the same UV extent overlap each other.
        SceneMesh two;
        two.vertices.push_back(mkVert(0, 0, 0, 0));
        two.vertices.push_back(mkVert(1, 0, 1, 0));
        two.vertices.push_back(mkVert(0, 1, 0, 1));
        two.vertices.push_back(mkVert(2, 0, 0, 0));
        two.vertices.push_back(mkVert(3, 0, 1, 0));
        two.vertices.push_back(mkVert(2, 1, 0, 1));
        SceneSubmesh tsub;
        tsub.material = 0;
        tsub.indices = {0, 1, 2, 3, 4, 5};
        two.submeshes.push_back(tsub);
        CHECK(material::UvIslands(two).size() == 2);
        CHECK(material::UvOverlapPairs(two) == 1);
        CHECK(rep.boundaryEdges > 0 && rep.totalEdges > 0 && rep.seamTriangles > 0);
        CHECK(rep.stretchMin >= 0.0f && rep.stretchMax >= rep.stretchMin);
        CHECK(rep.worstStretchTri >= 0 && rep.worstStretchTri < 4);
        // A closed tetra (every edge shared by two triangles) has no seams.
        SceneMesh closed;
        closed.vertices.push_back(mkVert(0, 0, 0, 0));
        closed.vertices.push_back(mkVert(1, 0, 1, 0));
        closed.vertices.push_back(mkVert(0, 1, 0, 1));
        closed.vertices.push_back(mkVert(1, 1, 0.5f, 0.5f));
        SceneSubmesh csub;
        csub.material = 0;
        csub.indices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
        closed.submeshes.push_back(csub);
        int total = 0, seamTris = 0;
        CHECK(material::CountSeams(closed, total, seamTris) == 0 && total == 6 && seamTris == 0);
        CHECK(material::UvIslands(closed).size() == 1);
        // Stretch: a dense-UV triangle vs a sparse one — densities differ.
        SceneMesh st;
        st.vertices.push_back(mkVert(0, 0, 0, 0));
        st.vertices.push_back(mkVert(1, 0, 1, 0));
        st.vertices.push_back(mkVert(0, 1, 0, 1));
        st.vertices.push_back(mkVert(10, 0, 0.2f, 0));
        st.vertices.push_back(mkVert(11, 0, 0.4f, 0));
        st.vertices.push_back(mkVert(10, 1, 0.2f, 0.2f));
        SceneSubmesh ssub;
        ssub.material = 0;
        ssub.indices = {0, 1, 2, 3, 4, 5};
        st.submeshes.push_back(ssub);
        float mn, mx, mean;
        int worst2;
        std::vector<material::UvStretchTri> per;
        material::UvStretchAll(st, 1024, 1024, mn, mx, mean, worst2, &per);
        CHECK(per.size() == 2 && mx > mn * 4.0f && worst2 >= 0);
        // UvTrisForMaterial picks only the named material's triangles.
        Scene scene;
        scene.meshes.push_back(m);
        SceneMesh other;
        other.vertices = {mkVert(0, 0, 0, 0), mkVert(1, 0, 1, 0), mkVert(0, 1, 0, 1)};
        SceneSubmesh osub;
        osub.material = 1;
        osub.indices = {0, 1, 2};
        other.submeshes.push_back(osub);
        scene.meshes.push_back(other);
        auto tris0 = material::UvTrisForMaterial(scene, 0);
        auto tris1 = material::UvTrisForMaterial(scene, 1);
        CHECK(tris0.size() == 4 && tris1.size() == 1);
        CHECK(tris0[0].mesh == 0 && tris1[0].mesh == 1);
    });

    test("workbench: an unanalyseable shader keeps the cheap _Color path, no tint copy", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        s.materials[0].shaderIndex = 0;
        assemble::SceneShader sh;
        sh.name = "Shader Forge/main_item";
        s.shaders.push_back(sh);   // empty blob: cannot be analysed
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o;
        o.baseColor = Vec4{0.5f, 0.5f, 0.5f, 1.0f};
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        CHECK(s.textures.size() == 1);            // no tint copy allocated
        CHECK(st.changedTextures.empty());        // and no texture upload requested
        CHECK(s.materials[0].texture == 0);       // original main texture still bound
        CHECK(std::fabs(s.materials[0].diffuse.x - 0.5f) < 1e-6f);
        bool colorOk = false;
        for (const auto& pr : s.materials[0].props) if (pr.first == "_Color") colorOk = std::fabs(pr.second.x - 0.5f) < 1e-6f;
        CHECK(colorOk);
    });

    test("workbench: roughness maps to _SpecularPower, specular writes colour and derived gloss", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o;
        o.roughness = 0.8f;   // strength 0.2
        o.specular = Vec3{1, 0.5f, 0};
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        float power = -1;
        for (const auto& pr : s.materials[0].props) if (pr.first == "_SpecularPower") power = pr.second.x;
        CHECK(std::fabs(power - 0.2f) < 1e-5f);
        CHECK(std::fabs(s.materials[0].specular.x - 0.2f) < 1e-5f && std::fabs(s.materials[0].specular.y - 0.1f) < 1e-5f);
        EffectiveMaterial e = EffectiveOf(st, 0);
        CHECK(std::fabs(e.roughness - 0.8f) < 1e-5f && std::fabs(e.specularPower - 0.2f) < 1e-5f);
    });

    test("workbench: reset restores the original bit for bit, reset-parameter keeps siblings", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        const auto before = s.materials[0];
        MaterialOverride o;
        o.baseColor = Vec4{1, 0, 0, 1};
        o.roughness = 0.1f;
        o.opacity = 0.3f;
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        CHECK(s.materials[0].diffuse.x == 1.0f && s.materials[0].diffuse.y == 0.0f);
        ResetParameter(st, 0, ResetParam::BaseColor);
        ApplyOverrides(s, st);
        CHECK(std::fabs(s.materials[0].diffuse.x - before.diffuse.x) < 1e-6f);
        CHECK(std::fabs(s.materials[0].diffuse.w - 0.3f) < 1e-6f);   // opacity survives the param reset
        ResetMaterial(st, 0);
        ApplyOverrides(s, st);
        CHECK(std::fabs(s.materials[0].diffuse.x - before.diffuse.x) < 1e-6f && s.materials[0].props.size() == before.props.size());
        CHECK(!st.IsDirty());
    });

    test("workbench: JSON round trip, version and unknown-field tolerance, unknown material warned", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o;
        o.class_ = MaterialClass::Fabric;
        o.baseColor = Vec4{0.2f, 0.4f, 0.6f, 0.75f};
        o.roughness = 0.8f;
        o.preset = "Fabric/Default";
        SetOverride(st, 0, o);
        MaterialFile f = MakeMaterialFile(s, st, "deadbeef");
        const std::string text = MaterialFileToJson(f);
        MaterialFile back;
        std::string err;
        CHECK(MaterialFileFromJson(text, back, &err));
        CHECK(back.entries.size() == 1 && back.cardSha256 == "deadbeef");
        CHECK(back.entries[0].part == "outfit0.top" && back.entries[0].material == "mf_m_top");
        // Apply the reloaded file to a fresh state with the same scene.
        assemble::Scene s2 = makeMatScene();
        WorkbenchState st2;
        CaptureOriginals(st2, s2);
        std::vector<std::string> unknown;
        CHECK(ApplyMaterialFile(s2, st2, back, &unknown) == 1 && unknown.empty());
        CHECK(s2.materials[0].diffuse.x == 0.2f && s2.materials[0].diffuse.w == 0.75f);
        // Unknown material: warned, not fatal.
        MaterialFile bad;
        MaterialEntry e; e.part = "outfit0.top"; e.material = "nope"; e.ov.opacity = 0.5f;
        bad.entries.push_back(e);
        std::vector<std::string> unk2;
        CHECK(ApplyMaterialFile(s2, st2, bad, &unk2) == 0 && unk2.size() == 1);
        bool warned = false;
        for (const auto& is : s2.issues) warned = warned || is.message.find("nope") != std::string::npos;
        CHECK(warned);
        // Version / format rejections with reasons.
        for (const char* badJson : {"", "not json", "[]", R"({"format":"other","version":1})", R"({"format":"cardbridge-materials","version":2})",
                                    R"({"format":"cardbridge-materials","version":1,"materials":[{"material":"m","class":"Nope"}]})",
                                    R"({"format":"cardbridge-materials","version":1,"materials":[{"part":"p"}]})"}) {
            err.clear();
            CHECK(!MaterialFileFromJson(badJson, back, &err) && !err.empty());
        }
        // Unknown fields are ignored.
        CHECK(MaterialFileFromJson(R"({"format":"cardbridge-materials","version":1,"future":true,"materials":[{"part":"p","material":"m","opacity":0.5,"future":1}]})", back, &err));
        CHECK(back.entries.size() == 1 && back.entries[0].ov.opacity && std::fabs(*back.entries[0].ov.opacity - 0.5f) < 1e-6f);
    });

    test("workbench: undo/redo is one step per edit, redo dies on a new edit", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o1; o1.opacity = 0.2f;
        MaterialOverride o2; o2.opacity = 0.4f;
        SetOverride(st, 0, o1);
        CHECK(CanUndo(st) && !CanRedo(st));
        SetOverride(st, 0, o2);
        CHECK(st.undoStack.size() == 2);
        CHECK(Undo(st) && std::fabs(*st.overrides[0]->opacity - 0.2f) < 1e-6f);
        CHECK(Undo(st) && !st.IsDirty());
        CHECK(Redo(st) && std::fabs(*st.overrides[0]->opacity - 0.2f) < 1e-6f);
        MaterialOverride o3; o3.opacity = 0.9f;
        SetOverride(st, 0, o3);
        CHECK(!CanRedo(st));   // the new edit killed the redo branch
        CHECK(!Redo(st));
    });

    test("workbench: texture enable/disable, rebind and replace; missing is warned not crashed", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        CHECK(SetTextureEnabled(s, st, 0, "_MainTex", false));
        CHECK(s.materials[0].texture == -1);
        ApplyOverrides(s, st);
        CHECK(s.materials[0].texture == -1);
        SetTextureEnabled(s, st, 0, "_MainTex", true);
        ApplyOverrides(s, st);
        CHECK(s.materials[0].texture == 0);
        assemble::SceneTexture rep;
        rep.name = "rep"; rep.width = 1; rep.height = 1; rep.rgba = {10, 20, 30, 255};
        CHECK(ReplaceTexture(s, st, 0, "_MainTex", rep) == 1);
        CHECK(s.materials[0].texture == 1 && s.textures.size() == 2);
        // Out-of-range rebind is refused with a warning and no state change.
        const size_t issuesBefore = s.issues.size();
        CHECK(!RebindTexture(s, st, 0, "_MainTex", 99));
        CHECK(s.issues.size() > issuesBefore && s.materials[0].texture == 1);
        // A broken replacement texture is refused.
        assemble::SceneTexture broken; broken.name = "broken";
        CHECK(ReplaceTexture(s, st, 0, "_MainTex", broken) == -1);
    });

    test("workbench: shared materials edit every use; copy/paste and multi-apply keep instances separate", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        // Make material 0 shared: a second submesh on the same mesh names it.
        s.meshes[0].vertices.push_back(s.meshes[0].vertices[2]);
        s.meshes[0].submeshes.push_back({0, {0, 1, 2}});
        WorkbenchState st;
        CaptureOriginals(st, s);
        CHECK(CountSubmeshesForMaterial(s, 0) == 2);
        CHECK(PartForMaterial(s, 0) == "outfit0.top");
        CHECK(FindMaterialIndex(s, "outfit0.top", "mf_m_top") == 0);
        CHECK(FindMaterialIndex(s, "hair.back", "mf_m_top") == -1);
        MaterialOverride o; o.opacity = 0.5f;
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        int edited = 0;
        for (const auto& mesh : s.meshes) for (const auto& sub : mesh.submeshes) if (sub.material == 0) ++edited;
        CHECK(edited == 2);
        // Copy/paste + multi-apply.
        MaterialOverride cp = CopyOverride(st, 0);
        PasteOverride(st, 1, cp);
        ApplyOverrides(s, st);
        CHECK(s.materials[1].diffuse.w == 0.5f);
        PasteToMultiple(st, {1}, MaterialOverride{});
        ApplyOverrides(s, st);
        CHECK(s.materials[1].diffuse.w == 1.0f);
        CHECK(MaterialIdColor(0).x != MaterialIdColor(1).x || MaterialIdColor(0).y != MaterialIdColor(1).y);
        CHECK(MaterialIdColor(0).x == MaterialIdColor(0).x);   // deterministic
    });

    test("workbench: PMX shows the edited colour/alpha and the manifest names the file", [&] {
        using namespace material;
        assemble::Scene s = makeMatScene();
        WorkbenchState st;
        CaptureOriginals(st, s);
        MaterialOverride o;
        o.baseColor = Vec4{0.2f, 0.4f, 0.6f, 0.5f};
        o.roughness = 0.9f;   // unsupported in PMX: must be reported, not silently dropped
        o.metallic = 1.0f;    // unsupported too
        SetOverride(st, 0, o);
        ApplyOverrides(s, st);
        ReportUnsupported(s, st);
        bool reported = false;
        for (const auto& is : s.issues) reported = reported || is.message.find("metallic") != std::string::npos;
        CHECK(reported);
        auto unsupported = UnsupportedFields(o);
        CHECK(unsupported.size() >= 2);
        const exporter::MmdLook look = exporter::MmdMaterialLook(s, s.materials[0]);
        CHECK(std::fabs(look.diffuse.x - 0.2f) < 1e-5f && std::fabs(look.diffuse.w - 0.5f) < 1e-5f);
        // Real export pairing: the fixture JSON changes the PMX and the manifest says so.
        card::Card c;
        c.path = "card.png";
        exporter::ExportOptions eo;
        eo.outDir = tempPath("material_export");
        eo.overwrite = true;
        eo.materialFile = "m.json";
        eo.materialWorkbench = nlohmann::json::parse(MaterialFileToJson(MakeMaterialFile(s, st, "deadbeef")), nullptr, false);
        eo.materialWorkbench["file"] = "m.json";
        const auto r = exporter::ExportScene(s, c, eo);
        CHECK(fileio::Exists(r.pmxPath) && fileio::Exists(r.manifestPath));
        Bytes pmxBytes = fileio::ReadFile(r.pmxPath);
        Bytes manifestBytes = fileio::ReadFile(r.manifestPath);
        const std::string manifestText(manifestBytes.begin(), manifestBytes.end());
        CHECK(manifestText.find("\"material_workbench\"") != std::string::npos);
        CHECK(manifestText.find("mf_m_top") != std::string::npos);
        CHECK(manifestText.find("\"file\": \"m.json\"") != std::string::npos);
        // The PMX carries the edited material (via the independent reader's
        // names) and the edited diffuse reaches it through MmdMaterialLook.
        const pmxread::Model back = readPmx(pmxBytes);
        bool sawMaterial = false;
        for (const auto& m : back.materials) if (m.name.find("outfit0.top") != std::string::npos) sawMaterial = true;
        CHECK(sawMaterial && back.materials.size() > 0);
    });

    test("viewport: diagnostic modes render deterministically on a synthetic scene", [] {
        using namespace assemble;
        Scene s;
        s.bones.push_back({"root", -1, {0, 0, 0}, "rig"});
        s.materials.push_back({}); s.materials.back().diffuse = {0.9f, 0.1f, 0.1f, 1.0f};
        s.materials.push_back({}); s.materials.back().diffuse = {0.1f, 0.5f, 0.9f, 1.0f};
        SceneMesh mesh;
        auto pv = [&](Vec3 pos, Vec2 uv) { SceneVertex v; v.pos = pos; v.normal = {0, 0, 1}; v.uv = uv; mesh.vertices.push_back(v); };
        // Triangle A: base colour material 0 (red), left. Triangle B: material 1 (blue), right.
        pv({-0.9f, -0.7f, 0}, {0.15f, 0.15f}); pv({-0.05f, -0.7f, 0}, {0.45f, 0.15f}); pv({-0.48f, 0.7f, 0}, {0.3f, 0.85f});
        pv({0.05f, -0.7f, 0}, {0.55f, 0.15f}); pv({0.9f, -0.7f, 0}, {0.85f, 0.15f}); pv({0.48f, 0.7f, 0}, {0.7f, 0.85f});
        SceneSubmesh smA; smA.material = 0; smA.indices = {0, 1, 2};
        SceneSubmesh smB; smB.material = 1; smB.indices = {3, 4, 5};
        mesh.submeshes.push_back(smA); mesh.submeshes.push_back(smB);
        s.meshes.push_back(mesh);
        ScenePart part; part.id = "p"; part.meshes = {0}; part.visibleInPreview = true;
        s.parts.push_back(part);
        const int W = 128, H = 128;
        const uint32_t bg = 0xFFE2E4E8u;
        exporter::ViewCamera cam;
        exporter::ViewStatePtr st = exporter::NewViewState();
        std::vector<uint32_t> px;
        auto render = [&](exporter::ViewMode mode, const std::vector<exporter::DiagnosticMaterial>* diag = nullptr, float yawDeg = 0.0f) {
            cam.yawDeg = yawDeg;
            px.clear();
            exporter::ViewOptions vo;
            vo.mode = mode;
            vo.diagnostics = diag;
            exporter::RenderView(s, cam, W, H, vo, *st, px);
            return px;
        };
        auto nonBg = [&](const std::vector<uint32_t>& p) { int n = 0; for (uint32_t v : p) if (v != bg) ++n; return n; };
        auto ch = [](uint32_t v, int k) { return static_cast<int>((v >> (16 - 8 * k)) & 255u); };   // 0=R, 1=G, 2=B
        // Lit renders and is deterministic.
        const auto lit = render(exporter::ViewMode::Lit);
        CHECK(nonBg(lit) > 50 && lit == render(exporter::ViewMode::Lit));
        // BaseColor: pure red-ish / blue-ish pixels, no texture.
        {
            const auto p = render(exporter::ViewMode::BaseColor);
            bool red = false, blue = false;
            for (uint32_t v : p) {
                const int r = ch(v, 0), g = ch(v, 1), b = ch(v, 2);
                if (r > 180 && g < 80 && b < 80) red = true;
                if (b > 180 && r < 80) blue = true;
            }
            CHECK(red && blue);
        }
        // Normal: world normal +Z renders as (128, 128, 255).
        {
            const auto p = render(exporter::ViewMode::Normal);
            bool ok = false;
            for (uint32_t v : p) {
                const int r = ch(v, 0), g = ch(v, 1), b = ch(v, 2);
                if (b > 230 && std::abs(r - 128) <= 3 && std::abs(g - 128) <= 3) ok = true;
            }
            CHECK(ok);
        }
        // Roughness / metallic with diagnostics: exact grays.
        {
            std::vector<exporter::DiagnosticMaterial> d(2);
            d[0].roughness = 0.2f; d[1].roughness = 0.9f; d[0].metallic = 0.8f; d[1].metallic = 0.3f;
            const auto rp = render(exporter::ViewMode::Roughness, &d);
            const auto mp = render(exporter::ViewMode::Metallic, &d);
            bool r51 = false, r230 = false, m204 = false, m77 = false;
            for (uint32_t v : rp) {
                const int r = ch(v, 0);
                if (std::abs(r - 51) <= 2) r51 = true;
                if (std::abs(r - 230) <= 2) r230 = true;
            }
            for (uint32_t v : mp) {
                const int r = ch(v, 0);
                if (std::abs(r - 204) <= 2) m204 = true;
                if (std::abs(r - 77) <= 2) m77 = true;
            }
            CHECK(r51 && r230 && m204 && m77);
        }
        // MaterialID: one deterministic colour per material, stable across calls, matching MaterialIdColor.
        {
            const auto p1 = render(exporter::ViewMode::MaterialID);
            const auto p2 = render(exporter::ViewMode::MaterialID);
            const Vec3 c0 = material::MaterialIdColor(0), c1 = material::MaterialIdColor(1);
            auto q = [](float f) { return static_cast<int>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
            bool found0 = false, found1 = false;
            for (uint32_t v : p1) {
                if (std::abs(ch(v, 0) - q(c0.x)) <= 3 && std::abs(ch(v, 1) - q(c0.y)) <= 3 && std::abs(ch(v, 2) - q(c0.z)) <= 3) found0 = true;
                if (std::abs(ch(v, 0) - q(c1.x)) <= 3 && std::abs(ch(v, 1) - q(c1.y)) <= 3 && std::abs(ch(v, 2) - q(c1.z)) <= 3) found1 = true;
            }
            CHECK(found0 && found1 && p1 == p2);
            CHECK(material::MaterialIdColor(3).x != material::MaterialIdColor(5).x || material::MaterialIdColor(3).y != material::MaterialIdColor(5).y || material::MaterialIdColor(3).z != material::MaterialIdColor(5).z);
        }
        // Wireframe: strictly less fill than Lit, deterministic.
        {
            const auto wire = render(exporter::ViewMode::Wireframe);
            const int nWire = nonBg(wire), nFull = nonBg(lit);
            CHECK(nWire > 20 && nWire * 3 < nFull && wire == render(exporter::ViewMode::Wireframe));
        }
        // UVChecker: deterministic and paints the mesh.
        {
            const auto p = render(exporter::ViewMode::UVChecker);
            CHECK(nonBg(p) > 50 && p == render(exporter::ViewMode::UVChecker));
        }
        // Transmission with alpha strength 0: the term is black (mesh pixels differ only where it was lit).
        {
            const auto p = render(exporter::ViewMode::Transmission);
            bool anyDark = false;
            for (uint32_t v : p) if (v != bg && ch(v, 0) < 30) anyDark = true;
            CHECK(anyDark);
        }
        // Transmission with full strength and thin fabric: visible from behind (front light faces -Z here).
        {
            std::vector<exporter::DiagnosticMaterial> d(2);
            for (auto& m : d) { m.transmissionStrength = 1.0f; m.transmissionThickness = 0.0f; }
            const auto p = render(exporter::ViewMode::Transmission, &d, 180.0f);
            bool anyLit = false;
            for (uint32_t v : p) if (v != bg && ch(v, 0) > 30) anyLit = true;
            CHECK(anyLit);
        }
        // Opacity: semantic alpha as gray (own scene, opaque materials kept in the main one).
        {
            Scene s2;
            s2.bones.push_back({"root", -1, {0, 0, 0}, "rig"});
            s2.materials.push_back({}); s2.materials.back().diffuse = {0.9f, 0.1f, 0.1f, 0.5f};
            SceneMesh m2;
            auto pv2 = [&](Vec3 pos, Vec2 uv) { SceneVertex v; v.pos = pos; v.normal = {0, 0, 1}; v.uv = uv; m2.vertices.push_back(v); };
            pv2({-0.9f, -0.7f, 0}, {0.1f, 0.1f}); pv2({0.9f, -0.7f, 0}, {0.9f, 0.1f}); pv2({0.0f, 0.7f, 0}, {0.5f, 0.9f});
            SceneSubmesh sm2; sm2.material = 0; sm2.indices = {0, 1, 2};
            m2.submeshes.push_back(sm2);
            s2.meshes.push_back(m2);
            ScenePart part2; part2.id = "p2"; part2.meshes = {0}; part2.visibleInPreview = true;
            s2.parts.push_back(part2);
            exporter::ViewStatePtr st2 = exporter::NewViewState();
            px.clear();
            cam.yawDeg = 0.0f;   // the transmission check above rotates it; stay facing the face
            exporter::ViewOptions vo2; vo2.mode = exporter::ViewMode::Opacity;
            exporter::RenderView(s2, cam, W, H, vo2, *st2, px);
            bool gray128 = false;
            for (uint32_t v : px) {
                const int r = ch(v, 0), g = ch(v, 1), b = ch(v, 2);
                if (std::abs(r - 128) <= 2 && std::abs(g - 128) <= 2 && std::abs(b - 128) <= 2) { gray128 = true; break; }
            }
            CHECK(gray128);
        }
        // Name table: round trip, unknown is Lit.
        CHECK(std::string_view{exporter::ViewModeName(exporter::ViewMode::Transmission)} == "transmission");
        CHECK(exporter::ViewModeFromName("transmission") == exporter::ViewMode::Transmission);
        CHECK(exporter::ViewModeFromName("not_a_mode") == exporter::ViewMode::Lit);
    });
    test("skin shading: a body crease darkens in UV space, flat skin stays lit and the strength scales", [] {
        assemble::Scene scene;
        scene.textures.resize(1);
        auto& tex = scene.textures[0];
        tex.name = "body_create_result";
        tex.width = tex.height = 64;
        tex.rgba.assign(64 * 64 * 4, 200);
        for (size_t i = 0; i < 64 * 64; ++i) tex.rgba[i * 4 + 3] = 255;
        // A smooth valley along z (a cleavage-like crease) on a 17 x 17 grid, with matching normals and UVs.
        assemble::SceneMesh mesh;
        mesh.name = "body_skin";
        const int N = 16;
        const float A = 0.14f, S = 0.035f;   // a deep crease, like the under-breast fold on the body
        for (int iz = 0; iz <= N; ++iz) {
            for (int ix = 0; ix <= N; ++ix) {
                const float x = -0.2f + 0.4f * static_cast<float>(ix) / static_cast<float>(N);
                const float z = -0.2f + 0.4f * static_cast<float>(iz) / static_cast<float>(N);
                const float e = std::exp(-(x * x) / (2.0f * S * S));
                const float dydx = A * x / (S * S) * e;
                assemble::SceneVertex v;
                v.pos = {x, -A * e, z};
                v.normal = normalize(Vec3{-dydx, 1.0f, 0.0f});
                v.uv = {(x + 0.2f) / 0.4f, (z + 0.2f) / 0.4f};
                v.uv1 = v.uv2 = v.uv3 = v.uv;
                mesh.vertices.push_back(v);
            }
        }
        assemble::SceneSubmesh sub;
        for (int iz = 0; iz < N; ++iz) {
            for (int ix = 0; ix < N; ++ix) {
                const uint32_t a = static_cast<uint32_t>(iz * (N + 1) + ix), b = a + 1;
                const uint32_t d = static_cast<uint32_t>((iz + 1) * (N + 1) + ix), c = d + 1;
                sub.indices.insert(sub.indices.end(), {a, b, c, a, c, d});
            }
        }
        mesh.submeshes.push_back(std::move(sub));
        scene.meshes.push_back(std::move(mesh));
        scene.skinShade.meshes = {0};
        scene.skinShade.texture = 0;
        auto pixel = [&](int x, int y) { return static_cast<int>(tex.rgba[(static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4]); };
        CHECK(exporter::ApplySkinShade(scene, 1.0f, 1.0f, 1.0f, 1.0f, true));
        const int crease = pixel(32, 32);
        const int flat = pixel(61, 32);
        CHECK(crease < flat - 10);   // the valley is darker than the nearly flat side
        CHECK(flat > 180);           // and the flat skin is essentially untouched
        CHECK(exporter::ApplySkinShade(scene, 0.0f, 0.0f, 1.0f, 1.0f, false));
        CHECK(pixel(32, 32) == 200);  // strength 0 restores the create result exactly
        CHECK(exporter::ApplySkinShade(scene, 1.0f, 1.0f, 1.0f, 1.0f, false));
        const int one = pixel(32, 32);
        CHECK(exporter::ApplySkinShade(scene, 2.0f, 2.0f, 1.0f, 1.0f, false));
        CHECK(pixel(32, 32) < one);   // more strength digs the crease deeper
    });
    test("koikatsu AO bake: the base body normal shades the valley, the line mask darkens and strength 0 restores exactly", [] {
        auto makeScene = [&]() {
            assemble::Scene scene;
            scene.textures.resize(1);
            auto& tex = scene.textures[0];
            tex.name = "body_create_result";
            tex.width = tex.height = 64;
            tex.rgba.assign(64 * 64 * 4, 200);
            for (size_t i = 0; i < 64 * 64; ++i) tex.rgba[i * 4 + 3] = 244;
            assemble::SceneMesh mesh;
            mesh.name = "body_skin";
            const int N = 16;
            const float A = 0.14f, S = 0.035f;
            for (int iz = 0; iz <= N; ++iz) {
                for (int ix = 0; ix <= N; ++ix) {
                    const float x = -0.2f + 0.4f * static_cast<float>(ix) / static_cast<float>(N);
                    const float z = -0.2f + 0.4f * static_cast<float>(iz) / static_cast<float>(N);
                    const float e = std::exp(-(x * x) / (2.0f * S * S));
                    const float dydx = A * x / (S * S) * e;
                    assemble::SceneVertex v;
                    v.pos = {x, -A * e, z};
                    v.normal = normalize(Vec3{-dydx, 1.0f, 0.0f});
                    v.uv = {(x + 0.2f) / 0.4f, (z + 0.2f) / 0.4f};
                    v.uv1 = v.uv2 = v.uv3 = v.uv;
                    mesh.vertices.push_back(v);
                }
            }
            assemble::SceneSubmesh sub;
            for (int iz = 0; iz < N; ++iz)
                for (int ix = 0; ix < N; ++ix) {
                    const uint32_t a = static_cast<uint32_t>(iz * (N + 1) + ix), b = a + 1;
                    const uint32_t d = static_cast<uint32_t>((iz + 1) * (N + 1) + ix), c = d + 1;
                    sub.indices.insert(sub.indices.end(), {a, b, c, a, c, d});
                }
            mesh.submeshes.push_back(std::move(sub));
            scene.meshes.push_back(std::move(mesh));
            scene.skinShade.meshes = {0};
            scene.skinShade.texture = 0;
            return scene;
        };
        // A striped base body normal map (DXT5nm convention: X in alpha as 0..255, Y in green). Flat left half,
        // alternating +/-0.4 slopes on the right: the neighbourhood-average deviation the cavity bake keys on.
        auto fillFlatNormal = [](std::vector<uint8_t>& out, int w, int h) {
            out.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 128);
            for (size_t i = 0; i < static_cast<size_t>(w) * static_cast<size_t>(h); ++i) { out[i * 4 + 3] = 128; out[i * 4 + 1] = 128; }
        };
        auto fillStripes = [&](std::vector<uint8_t>& out, int w, int h, bool everywhere) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 4;
                    if (!everywhere && x < 32) { out[i + 3] = 128; out[i + 1] = 128; out[i + 0] = 128; out[i + 2] = 128; continue; }
                    const int stripe = (x / 3) % 2;
                    out[i + 3] = stripe ? 76 : 178;   // X in alpha: -0.4 / +0.4
                    out[i + 1] = 128;                 // Y in green: 0
                    out[i + 0] = 128;
                    out[i + 2] = 128;
                }
        };
        auto pixel = [&](assemble::Scene& s, int x, int y) { return static_cast<int>(s.textures[0].rgba[(static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4]); };
        {   // flat skin stays untouched, the striped half darkens (the cavity factor), alpha preserved, strength 0 restores
            assemble::Scene scene = makeScene();
            fillFlatNormal(scene.aoBake.baseNormal, 64, 64);
            fillStripes(scene.aoBake.baseNormal, 64, 64, false);
            scene.aoBake.baseNormalWidth = scene.aoBake.baseNormalHeight = 64;
            CHECK(exporter::ApplySkinAoBake(scene, 1.0f, true));
            const int flatZone = pixel(scene, 10, 32);
            const int stripeZone = pixel(scene, 50, 32);
            CHECK(flatZone == 200);              // flat skin: exactly untouched
            CHECK(stripeZone < 200 - 20);        // striped (high-deviation) areas shade darker
            CHECK(stripeZone < flatZone);
            for (size_t i = 0; i < scene.textures[0].rgba.size(); i += 4) CHECK(scene.textures[0].rgba[i + 3] == 244);
            CHECK(exporter::ApplySkinAoBake(scene, 0.0f, false));
            CHECK(pixel(scene, 50, 32) == 200);
            CHECK(pixel(scene, 10, 32) == 200);
        }
        {   // a vertical ink line through the valley centre (G channel, like cf_body_00_line): the line darkens at strength 1
            assemble::Scene scene = makeScene();
            fillFlatNormal(scene.aoBake.baseNormal, 64, 64);
            scene.aoBake.baseNormalWidth = scene.aoBake.baseNormalHeight = 64;
            scene.aoBake.lineMask.assign(64 * 64 * 4, 0);
            scene.aoBake.lineMaskWidth = scene.aoBake.lineMaskHeight = 64;
            for (int y = 0; y < 64; ++y) for (int x = 30; x <= 34; ++x) {   // a 5-texel line at u ~ 0.47-0.53
                const size_t i = static_cast<size_t>(y) * 64 + static_cast<size_t>(x);
                scene.aoBake.lineMask[i * 4 + 1] = 255;
            }
            CHECK(exporter::ApplySkinAoBake(scene, 1.0f, true));
            const int onLine = pixel(scene, 32, 32);
            CHECK(exporter::ApplySkinAoBake(scene, 0.0f, false));
            CHECK(pixel(scene, 32, 32) == 200);
            CHECK(onLine < 200 - 15);
        }
        {   // the new bake yields to SkinShade's darkening (attenuation: no stacking in a full crease)
            assemble::Scene shadeOnly = makeScene();
            CHECK(exporter::ApplySkinShade(shadeOnly, 2.0f, 2.0f, 1.0f, 1.0f, true));
            const int creaseShade = pixel(shadeOnly, 32, 32);
            assemble::Scene shadeThenAo = makeScene();
            fillFlatNormal(shadeThenAo.aoBake.baseNormal, 64, 64);
            fillStripes(shadeThenAo.aoBake.baseNormal, 64, 64, true);   // stripes everywhere: the valley centre is strongly marked too
            shadeThenAo.aoBake.baseNormalWidth = shadeThenAo.aoBake.baseNormalHeight = 64;
            CHECK(exporter::ApplySkinShade(shadeThenAo, 2.0f, 2.0f, 1.0f, 1.0f, true));
            CHECK(exporter::ApplySkinAoBake(shadeThenAo, 1.0f, false));
            const int creaseCombined = pixel(shadeThenAo, 32, 32);
            if (std::getenv("CARDBRIDGE_TEST_DEBUG_AO"))
                std::fprintf(stderr, "crease debug: shade %d combined %d\n", creaseShade, creaseCombined);
            CHECK(creaseCombined < creaseShade);               // the AO pass still contributes, a little
            CHECK(creaseCombined >= static_cast<int>(creaseShade * 0.6f) - 1);   // but never at full strength: it yields to SkinShade (att scales the AO contribution down further as SkinShade's darkening deepens)
        }
    });
    test("skin AO diagnostic factor: 255 when off, darkens covered texels at strength 1, and the token round-trips", [] {
        assemble::Scene scene;
        scene.textures.resize(1);
        auto& tex = scene.textures[0];
        tex.name = "body_create_result";
        tex.width = tex.height = 64;
        tex.rgba.assign(64 * 64 * 4, 200);
        for (size_t i = 0; i < 64 * 64; ++i) tex.rgba[i * 4 + 3] = 244;
        assemble::SceneMesh mesh;
        mesh.name = "body_skin";
        const int N = 16;
        const float A = 0.14f, S = 0.035f;
        for (int iz = 0; iz <= N; ++iz) {
            for (int ix = 0; ix <= N; ++ix) {
                const float x = -0.2f + 0.4f * static_cast<float>(ix) / static_cast<float>(N);
                const float z = -0.2f + 0.4f * static_cast<float>(iz) / static_cast<float>(N);
                const float e = std::exp(-(x * x) / (2.0f * S * S));
                const float dydx = A * x / (S * S) * e;
                assemble::SceneVertex v;
                v.pos = {x, -A * e, z};
                v.normal = normalize(Vec3{-dydx, 1.0f, 0.0f});
                v.uv = {(x + 0.2f) / 0.4f, (z + 0.2f) / 0.4f};
                v.uv1 = v.uv2 = v.uv3 = v.uv;
                mesh.vertices.push_back(v);
            }
        }
        assemble::SceneSubmesh sub;
        for (int iz = 0; iz < N; ++iz)
            for (int ix = 0; ix < N; ++ix) {
                const uint32_t a = static_cast<uint32_t>(iz * (N + 1) + ix), b = a + 1;
                const uint32_t d = static_cast<uint32_t>((iz + 1) * (N + 1) + ix), c = d + 1;
                sub.indices.insert(sub.indices.end(), {a, b, c, a, c, d});
            }
        mesh.submeshes.push_back(std::move(sub));
        scene.meshes.push_back(std::move(mesh));
        scene.skinShade.meshes = {0};
        scene.skinShade.texture = 0;
        // A flat base normal map lets the bake run; strengths 0/1 contrast in the factor output.
        std::vector<uint8_t> flatNm(64 * 64 * 4, 128);
        for (size_t i = 0; i < static_cast<size_t>(64) * static_cast<size_t>(64); ++i) { flatNm[i * 4 + 3] = 128; flatNm[i * 4 + 1] = 128; }
        scene.aoBake.baseNormal = flatNm;
        scene.aoBake.baseNormalWidth = scene.aoBake.baseNormalHeight = 64;
        {   // AO strength 0: the factor is uniformly 255 ("contributes nothing")
            CHECK(exporter::ApplySkinAoBake(scene, 0.0f, true));
            const auto factor = exporter::BuildSkinAoFactor(scene);
            CHECK(factor.size() == 64 * 64);
            CHECK(std::all_of(factor.begin(), factor.end(), [](uint8_t v) { return v == 255; }));
        }
        {   // a striped base normal map: covered texels darken below 255, flat corners stay... covered too; the count of <255 must match coverage
            std::vector<uint8_t> nm(64 * 64 * 4, 128);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    const size_t i = (static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4;
                    const int stripe = (x / 3) % 2;
                    nm[i + 3] = stripe ? 76 : 178;
                    nm[i + 1] = 128;
                }
            scene.aoBake.baseNormal = nm;
            scene.aoBake.baseNormalWidth = scene.aoBake.baseNormalHeight = 64;
            scene.aoBake.detailNormal.clear();
            CHECK(exporter::ApplySkinAoBake(scene, 1.0f, true));
            const auto factor = exporter::BuildSkinAoFactor(scene);
            int below = 0;
            for (uint8_t v : factor) if (v < 250) ++below;
            CHECK(below > 0);
        }
        CHECK(exporter::ViewModeFromName("skin-ao") == exporter::ViewMode::SkinAO);
        CHECK(std::string_view{exporter::ViewModeName(exporter::ViewMode::SkinAO)} == "skin-ao");
        CHECK(std::string_view{exporter::ViewModeLabel(exporter::ViewMode::SkinAO)} == "SkinAO");
        CHECK(std::string_view{exporter::ViewModeTooltip(exporter::ViewMode::SkinAO)}.find("white") != std::string_view::npos);
    });
    test("nipple tint: the pink default applies to the body skin material and the card's colour comes back", [] {
        assemble::Scene scene;
        assemble::SceneMaterial mat;
        mat.name = "cf_m_body";
        mat.cardSkin = true;
        mat.cardNippleColour = {0.95f, 0.77f, 0.73f, 1.0f};
        mat.props.emplace_back("_overcolor1", mat.cardNippleColour);
        scene.materials.push_back(mat);
        auto prop = [&](const char* name) {
            for (const auto& pr : scene.materials[0].props) if (pr.first == name) return pr.second;
            return Vec4{};
        };
        CHECK(exporter::ApplyNippleTint(scene, assemble::kDefaultNippleColour) == 1);
        Vec4 got = prop("_overcolor1");
        CHECK(std::fabs(got.x - assemble::kDefaultNippleColour.x) < 1e-6f);
        CHECK(std::fabs(got.y - assemble::kDefaultNippleColour.y) < 1e-6f);
        CHECK(std::fabs(got.z - assemble::kDefaultNippleColour.z) < 1e-6f);
        CHECK(exporter::ApplyNippleTint(scene, assemble::kDefaultNippleColour) == 0);   // already the same
        CHECK(exporter::ApplyNippleTint(scene, Vec4{0, 0, 0, 0}) == 1);                 // w <= 0: card's own colour
        got = prop("_overcolor1");
        CHECK(std::fabs(got.x - 0.95f) < 1e-6f && std::fabs(got.y - 0.77f) < 1e-6f && std::fabs(got.z - 0.73f) < 1e-6f);
        assemble::SceneMaterial other;   // a non-skin material is never touched
        other.name = "cf_m_hair";
        other.props.emplace_back("_overcolor1", Vec4{0.1f, 0.2f, 0.3f, 1.0f});
        scene.materials.push_back(other);
        CHECK(exporter::ApplyNippleTint(scene, assemble::kDefaultNippleColour) == 1);   // only the body material
        CHECK(std::fabs(scene.materials[1].props[0].second.x - 0.1f) < 1e-6f);
    });
}


void integrationTests() {
    std::string lib = env("CARDBRIDGE_TEST_LIBRARY"), mods = env("CARDBRIDGE_TEST_MODS"), cardPath = env("CARDBRIDGE_TEST_CARD");
    if (lib.empty() || mods.empty() || cardPath.empty()) {
        ++g_skipped;
        std::printf("[ SKIP ] integration tests (set CARDBRIDGE_TEST_LIBRARY / _MODS / _CARD)\n");
        return;
    }
    test("card -> PMX end to end", [&] {
        card::Card c = card::LoadCard(cardPath);
        CHECK(c.supported);
        CHECK(c.family == card::CardFamily::Koikatsu);
        CHECK(c.blocks.size() == 5);
        CHECK(!c.outfits.empty());

        std::vector<std::string> issues;
        resolve::ModIndex mi = resolve::ModIndex::Build(mods, env("CARDBRIDGE_TEST_MODCACHE"), &issues);
        resolve::ListTable lists;
        lists.loadBaseGame(library::AbdataRoot(lib), &issues);
        resolve::Resolver resolver(mi, lists, library::AbdataRoot(lib));

        // every GUID the card names is installed
        for (const auto& r : c.rows) CHECK(mi.find(r.modId) != nullptr);
        // a mod's list rows are found again by their raw (category, id), which is how a card without resolve rows names them
        for (const auto& r : c.rows) {
            const auto* files = mi.find(r.modId);
            if (!files || (*files)[0].listIds.size() < 2) continue;
            const auto& e = (*files)[0];
            bool found = false;
            for (const auto* hit : mi.findByListId(static_cast<int>(e.listIds[0]), e.listIds[1])) found = found || hit->path == e.path;
            CHECK(found);
            break;
        }

        assemble::AssembleOptions ao;
        ao.outfit = 0;
        assemble::Scene scene = assemble::Assemble(c, resolver, ao);
        for (const auto& is : scene.issues) CHECK(is.severity != "error");
        CHECK(scene.bones.size() > 100);
        CHECK(!scene.meshes.empty());
        {   // a hair item's own bones hang on the head (through cf_J_FaceUp_ty), never on the model root: left there they stay behind when the head turns
            const int faceUp = scene.boneIndex("cf_J_FaceUp_ty");
            for (const auto& b : scene.bones)
                if (b.source.rfind("hair", 0) == 0 && faceUp >= 0) CHECK(b.parent != 0 || b.name == "cf_j_root");
            // the body's third UV set (TEXCOORD2, where the shader places the underhair) is read: without it the underhair covered the whole body
            for (const auto& m : scene.meshes)
                if (m.name == "o_body_a") {
                    size_t differs = 0;
                    for (const auto& v : m.vertices) differs += std::fabs(v.uv2.x - v.uv.x) + std::fabs(v.uv2.y - v.uv.y) > 1e-4f;
                    CHECK(differs > m.vertices.size() / 2);
                }
            // the curves of a chain's DynamicBone come through per bone, in range
            for (const auto& ch : scene.chains) {
                CHECK(ch.boneStiffness.size() == ch.boneDamping.size() && ch.boneStiffness.size() == ch.boneElasticity.size());
                CHECK(ch.boneStiffness.empty() || ch.boneStiffness.size() == ch.bones.size());
                for (float v : ch.boneStiffness) CHECK(v >= 0.0f && v <= 1.0f);
            }
        }
        {   // the iris and eye white create passes ran (a 1x1 placeholder is left when the shader could not run), and the iris carries colour
            int irises = 0;
            for (const auto& t : scene.textures)
                if (t.name == "cf_m_eye_create_result") {
                    ++irises;
                    CHECK(t.width == 512 && t.height == 512 && t.rgba.size() == 512u * 512u * 4u);
                    size_t lit = 0;
                    for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) if (t.rgba[i + 3] > 128 && t.rgba[i] + t.rgba[i + 1] + t.rgba[i + 2] > 60) ++lit;
                    CHECK(lit > 20000);
                }
            CHECK(irises == 0 || irises == 2);
        }
        // weights sum to 1 on every vertex
        size_t badWeights = 0;
        for (const auto& m : scene.meshes)
            for (const auto& v : m.vertices) {
                float s = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
                if (std::abs(s - 1.0f) > 1e-3f) ++badWeights;
            }
        CHECK(badWeights == 0);
        {   // a hair prefab root keeps its own transform under cf_J_FaceUp_ty (the game's SetParent(parent, false)); the base side hair
            // p_cf_hair_s_02 has a root 5.1 cm up and scaled 0.98 in x, so cancelling the root is visible
            card::Card rooted = c;
            auto mutableChild = [](Value& v, const std::string& key) -> Value* {
                if (!v.isMap()) return nullptr;
                for (auto& kv : v.map()) if (kv.first.isStr() && kv.first.asStr() == key) return &kv.second;
                return nullptr;
            };
            Value* parts = mutableChild(rooted.hair, "parts");
            CHECK(parts && parts->isArray() && parts->array().size() >= 3);
            Value* sideId = parts && parts->isArray() && parts->array().size() >= 3 ? mutableChild(parts->array()[2], "id") : nullptr;
            CHECK(sideId != nullptr);
            if (sideId) *sideId = Value(int64_t(2));
            if (sideId) {
                assemble::AssembleOptions rootedOpt = ao;
                rootedOpt.outfit = 0;
                assemble::Scene rootedScene = assemble::Assemble(rooted, resolver, rootedOpt);
                resolve::ItemRef side;
                for (const auto& r : resolve::ItemsFromCard(rooted, 0)) if (r.partId == "hair.side") side = r;
                CHECK(side.category == 103 && side.id == 2);
                const resolve::ResolvedItem item = resolver.resolve(rooted, side);
                CHECK(item.status == resolve::ItemStatus::ResolvedBase);
                std::string rootedWhy;
                auto uf = item.ok() ? resolver.openBundle(item, &rootedWhy) : nullptr;
                CHECK(uf != nullptr);
                if (uf) {
                    const int64_t hairRoot = uf->findPrefabRoot(item.assetName());
                    int64_t hairBone = 0;
                    if (hairRoot)
                        for (int64_t id : uf->subtree(hairRoot))
                            if (const unity::Xform* x = uf->xform(id); x && x->name == "cf_J_hairS_top") { hairBone = id; break; }
                    CHECK(hairRoot != 0 && hairBone != 0);
                    int bi = -1;
                    for (size_t i = 0; i < rootedScene.bones.size() && bi < 0; ++i)
                        if (rootedScene.bones[i].source == "hair.side" && rootedScene.bones[i].name == "cf_J_hairS_top") bi = static_cast<int>(i);
                    const int faceUp = rootedScene.boneIndex("cf_J_FaceUp_ty");
                    CHECK(bi >= 0 && faceUp >= 0);
                    if (bi >= 0 && faceUp >= 0 && hairRoot && hairBone != 0) {
                        const Mat4& parentWorld = rootedScene.bones[static_cast<size_t>(faceUp)].world;
                        const Mat4 expected = parentWorld * uf->worldMatrix(hairBone);
                        const Mat4& got = rootedScene.bones[static_cast<size_t>(bi)].world;
                        float worst = 0;
                        for (int i = 0; i < 16; ++i) worst = std::max(worst, std::fabs(got.m[i] - expected.m[i]));
                        const Vec3 withoutRoot = (parentWorld * inverse(uf->worldMatrix(hairRoot)) * uf->worldMatrix(hairBone)).translation();
                        std::printf("  hair root: worst matrix delta %.6f; y with root %.4f, without it %.4f\n", worst, got.translation().y, withoutRoot.y);
                        CHECK(worst < 1e-4f);
                        CHECK(std::fabs(got.translation().y - withoutRoot.y) > 0.03f);   // the test would catch the old cancellation
                    }
                }
            }
        }
        {   // accessory physics: a hair accessory is skinned to its own DynamicBone chain, never pinned rigidly to the head, and a static
            // piece hangs on the item's node (so a chain that moves it takes the mesh along)
            std::map<std::string, std::set<int>> accChains;
            for (const auto& ch : scene.chains) {
                if (ch.part.find(".acc") == std::string::npos) continue;
                bool owned = true;
                for (int b : ch.bones)
                    if (b < 0 || b >= static_cast<int>(scene.bones.size()) || scene.bones[static_cast<size_t>(b)].source != ch.part) owned = false;
                if (!owned) continue;   // a garment worn through an accessory slot binds to the body skeleton; not an accessory chain
                CHECK(ch.bones.size() >= 2);
                std::set<int> bones;
                for (int b : ch.bones) {
                    CHECK(b == ch.bones[0] || bones.count(scene.bones[static_cast<size_t>(b)].parent) > 0);   // a chain, not a loose set
                    bones.insert(b);
                }
                accChains[ch.part].insert(bones.begin(), bones.end());
            }
            CHECK(!accChains.empty());
            std::map<std::string, std::set<int>> driven;
            size_t deadChains = 0;
            for (const auto& m : scene.meshes) {
                if (m.part < 0 || m.part >= static_cast<int>(scene.parts.size())) continue;
                const std::string& partId = scene.parts[static_cast<size_t>(m.part)].id;
                auto ci = accChains.find(partId);
                if (ci == accChains.end()) continue;
                for (const auto& v : m.vertices)
                    for (int k = 0; k < 4; ++k)
                        if (v.weight[k] > 0.0f) driven[partId].insert(v.bone[k]);
            }
            for (const auto& [partId, chainBones] : accChains) {
                bool used = false;
                for (int b : chainBones) used = used || driven[partId].count(b) > 0;
                if (!used) ++deadChains;
            }
            CHECK(deadChains == 0);   // every accessory chain moves some geometry
            std::set<std::string> hairParts, skinnedHairParts;
            size_t hairVerts = 0, onChain = 0, foreign = 0;
            for (const auto& m : scene.meshes) {
                if (m.part < 0 || m.part >= static_cast<int>(scene.parts.size())) continue;
                const std::string& partId = scene.parts[static_cast<size_t>(m.part)].id;
                if (partId.find(".acc") == std::string::npos || m.name.rfind("cf_hair", 0) != 0) continue;
                hairParts.insert(partId);
                bool skinnedMesh = false;
                for (const auto& v : m.vertices) {
                    int weights = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (v.weight[k] <= 0.0f) continue;
                        ++weights;
                        ++hairVerts;
                        if (v.bone[k] < 0 || v.bone[k] >= static_cast<int>(scene.bones.size())) { ++foreign; continue; }
                        if (scene.bones[static_cast<size_t>(v.bone[k])].source != partId) ++foreign;
                        auto ci = accChains.find(partId);
                        if (ci != accChains.end() && ci->second.count(v.bone[k])) ++onChain;
                    }
                    if (weights > 1) skinnedMesh = true;
                }
                if (skinnedMesh) skinnedHairParts.insert(partId);
            }
            CHECK(!hairParts.empty());
            CHECK(!skinnedHairParts.empty());
            for (const auto& p : skinnedHairParts) CHECK(accChains.count(p) == 1);   // hair that swings in the game swings here
            CHECK(onChain > 100);   // the strands are actually driven by their chains
            CHECK(foreign == 0);    // no piece is left behind on the head bone
            std::printf("  accessory hair: %zu parts (%zu skinned), %zu chains, %zu of %zu vertex weights on chains\n",
                        hairParts.size(), skinnedHairParts.size(), accChains.size(), onChain, hairVerts);
        }
        {   // a prefab and a nested node can share a name: the prefab root is the parentless one (co_bra_00's p_o_top_swim02_low)
            std::string prefabWhy;
            auto bra = resolver.openBaseBundle("chara/co_bra_00.unity3d", &prefabWhy);
            if (bra) {
                const auto all = bra->findXformsByName("p_o_top_swim02_low");
                CHECK(all.size() >= 2);
                const int64_t root = bra->findPrefabRoot("p_o_top_swim02_low");
                CHECK(root != 0 && bra->xform(root) && bra->xform(root)->parent == 0);
                CHECK(std::find(all.begin(), all.end(), root) != all.end());
                bool nested = false;
                for (int64_t id : all) if (id != root && bra->xform(id) && bra->xform(id)->parent != 0) nested = true;
                CHECK(nested);
                CHECK(!all.empty() && all[0] != root);   // the old first-match lookup picks the nested node here
                CHECK(bra->findPrefabRoot("p_o_top_swim02_low_not_there") == 0);
            }
        }
        {   // every transform of the body and head rig prefabs is a scene bone (by name; the face bones included)
            std::string why;
            auto base = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
            CHECK(base != nullptr);
            if (base) {
                std::set<std::string> rigNames;
                for (const char* prefab : {"p_cf_body_bone", "p_cf_head_bone"}) {
                    const int64_t rigRoot = base->findPrefabRoot(prefab);
                    CHECK(rigRoot != 0);
                    if (!rigRoot) continue;
                    for (int64_t id : base->subtree(rigRoot)) rigNames.insert(base->xform(id)->name);
                }
                size_t missing = 0;
                for (const auto& n : rigNames) if (scene.boneIndex(n) < 0) { ++missing; std::printf("  rig bone missing from the scene: %s\n", n.c_str()); }
                CHECK(missing == 0);
                CHECK(rigNames.size() > 400);
                std::printf("  rig has %zu distinct bones, scene has %zu\n", rigNames.size(), scene.bones.size());
                for (const char* face : {"cf_J_Eye_tx_L", "cf_J_Mayu_L", "cf_J_MouthBase_ty", "cf_J_FaceLow_tz", "cf_J_Nose_tip", "cf_J_EarLow_L"})
                    CHECK(scene.boneIndex(face) >= 0);
            }
            // the table is well formed for a PMX: unique names, a parent always before its child
            std::set<std::string> names;
            size_t disorder = 0;
            for (size_t i = 0; i < scene.bones.size(); ++i) {
                names.insert(scene.bones[i].name);
                if (scene.bones[i].parent >= static_cast<int>(i)) ++disorder;
            }
            CHECK(names.size() == scene.bones.size());
            CHECK(disorder == 0);
            // the face's expressions, the body's jiggle chains and the colliders came through
            bool headLoaded = false;
            for (const auto& p : scene.parts) if (p.id == "head" && !p.meshes.empty()) headLoaded = true;
            std::printf("  head loaded: %d\n", headLoaded ? 1 : 0);
            if (headLoaded) CHECK(!scene.expressions.empty());
            // the accessory attach nodes come from the head model (bo_head_00's pony node has its local y up; the generic rig's points down)
            for (const auto& p : scene.parts) {
                int pony = scene.boneIndex("a_n_hair_pony");
                if (p.id == "head" && p.bundle == "chara/bo_head_00.unity3d" && pony >= 0) CHECK(scene.bones[static_cast<size_t>(pony)].world.m[5] > 0.0f);
            }
            CHECK(!scene.colliders.empty());
            std::printf("  %zu expressions, %zu physics chains, %zu colliders\n", scene.expressions.size(), scene.chains.size(), scene.colliders.size());
        }
        {   // re-shaping in place: the same sliders change nothing, other sliders give what a fresh assembly gives
            auto deviation = [](const assemble::Scene& a, const assemble::Scene& b, size_t* count = nullptr) {
                float worst = 0;
                size_t counted = 0;
                for (size_t mi = 0; mi < a.meshes.size() && mi < b.meshes.size(); ++mi) {
                    if (a.meshes[mi].vertices.size() != b.meshes[mi].vertices.size()) return 1e9f;
                    for (size_t v = 0; v < a.meshes[mi].vertices.size(); ++v) {
                        const Vec3 d{a.meshes[mi].vertices[v].pos.x - b.meshes[mi].vertices[v].pos.x, a.meshes[mi].vertices[v].pos.y - b.meshes[mi].vertices[v].pos.y,
                                     a.meshes[mi].vertices[v].pos.z - b.meshes[mi].vertices[v].pos.z};
                        worst = std::max(worst, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
                        ++counted;
                    }
                }
                if (count) *count = counted;
                return worst;
            };
            // the blend shape offsets: the largest difference in any offset of any shape
            auto shapeDeviation = [](const assemble::Scene& a, const assemble::Scene& b, size_t* count = nullptr) {
                float worst = 0;
                size_t counted = 0;
                for (size_t mi = 0; mi < a.meshes.size() && mi < b.meshes.size(); ++mi) {
                    if (a.meshes[mi].shapes.size() != b.meshes[mi].shapes.size()) return 1e9f;
                    for (size_t si = 0; si < a.meshes[mi].shapes.size(); ++si) {
                        const auto& x = a.meshes[mi].shapes[si].offsets;
                        const auto& y = b.meshes[mi].shapes[si].offsets;
                        if (x.size() != y.size()) return 1e9f;
                        for (size_t i = 0; i < x.size(); ++i) {
                            const Vec3 d{x[i].second.x - y[i].second.x, x[i].second.y - y[i].second.y, x[i].second.z - y[i].second.z};
                            worst = std::max(worst, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
                            ++counted;
                        }
                    }
                }
                if (count) *count = counted;
                return worst;
            };
            auto areolaOf = [](const assemble::Scene& sc) {   // the areola size on the materials that carry the card's value
                std::vector<float> out;
                for (const auto& m : sc.materials) if (m.cardAreola) for (const auto& pr : m.props) if (pr.first == "_nipsize") out.push_back(pr.second.x);
                return out;
            };
            assemble::Scene again = scene;
            assemble::AssembleOptions same = ao;
            for (size_t i = 0; i < c.body.at("shapeValueBody").size(); ++i) same.bodyShape.push_back(static_cast<float>(c.body.at("shapeValueBody").at(i).asFloat()));
            for (size_t i = 0; i < c.face.at("shapeValueFace").size(); ++i) same.faceShape.push_back(static_cast<float>(c.face.at("shapeValueFace").at(i).asFloat()));
            CHECK(assemble::Reshape(again, c, resolver, same));
            CHECK(deviation(scene, again) < 1e-4f);   // the identity re-shape is exact
            size_t shapeOffsets = 0;
            CHECK(shapeDeviation(scene, again, &shapeOffsets) < 1e-5f);   // and so are the blend shape offsets
            CHECK(areolaOf(scene) == areolaOf(again));
            CHECK(!areolaOf(scene).empty());   // the body's cf_m_body (mm_base) is followed from the prefab, so _nipsize has a consumer
            assemble::AssembleOptions other = same;
            other.bodyShape[0] = std::min(1.0f, other.bodyShape[0] + 0.3f);   // height
            other.bodyShape[4] = std::max(0.0f, other.bodyShape[4] - 0.3f);
            other.faceShape[0] = std::min(1.0f, other.faceShape[0] + 0.3f);
            other.areolaSize = 0.93f;
            auto t0 = std::chrono::steady_clock::now();
            CHECK(assemble::Reshape(again, c, resolver, other));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            assemble::Scene fresh = assemble::Assemble(c, resolver, other);
            size_t counted = 0;
            const float worst = deviation(fresh, again, &counted);
            std::printf("  re-shape %.0f ms; against a fresh assembly: worst vertex offset %.2f mm over %zu vertices\n", ms, worst * 1000.0f, counted);
            CHECK(worst < 0.01f);   // within a centimetre everywhere
            size_t offsetsCounted = 0;
            const float shapeWorst = shapeDeviation(fresh, again, &offsetsCounted);
            std::printf("  blend shape offsets after the re-shape: worst %.3f mm over %zu offsets; the identity re-shape moved %zu offsets by less than 0.01 mm\n", shapeWorst * 1000.0f, offsetsCounted, shapeOffsets);
            CHECK(shapeWorst < 1e-3f);   // (offsetsCounted is 0 in a library without the head)
            CHECK(areolaOf(again) == areolaOf(fresh));
            for (float a : areolaOf(again)) CHECK(std::fabs(a - 0.93f) < 1e-6f);
            CHECK(!again.chainsSettled);
            CHECK(ms < 2000.0);
        }
        {   // the nude body: what a garment's mask cuts from the body is kept as under pieces, nothing is lost
            const assemble::ScenePart* body0 = nullptr;
            bool nudeSwitch = false;
            size_t pieces = 0;
            for (const auto& p : scene.parts) {
                if (p.id == "body") body0 = &p;
                if (p.nudeSwitch) nudeSwitch = true;
                if (p.underPiece) {
                    ++pieces;
                    CHECK(!p.visibleInPreview && p.startHidden && !p.coveredBy.empty() && !p.underOf.empty());
                    for (int mi : p.meshes) for (const auto& sm : scene.meshes[static_cast<size_t>(mi)].submeshes) CHECK(!sm.indices.empty());
                }
            }
            CHECK(body0 != nullptr);
            CHECK(nudeSwitch == (pieces > 0));
        }

        // ---- golden facts from the Python reference run (tests/golden/*.json) ----
        std::string goldenPath = env("CARDBRIDGE_TEST_GOLDEN");
        if (!goldenPath.empty()) {
            Bytes raw = fileio::ReadFile(goldenPath);
            auto g = nlohmann::json::parse(raw.begin(), raw.end());
            CHECK(c.marker == g["marker"].get<std::string>());
            CHECK(static_cast<int>(c.outfits.size()) == g["outfits"].get<int>());
            CHECK(static_cast<int>(c.rows.size()) == g["resolver_rows"].get<int>());
            CHECK(c.blocks.size() == g["blocks"].size());

            auto partNamed = [&](const std::string& id) -> const assemble::ScenePart* {
                for (const auto& p : scene.parts) if (p.id == id) return &p;
                return nullptr;
            };
            auto countsOf = [&](const assemble::ScenePart& p, const std::string& mesh) {
                std::pair<size_t, size_t> r{0, 0};
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    if (m.name != mesh) continue;
                    r.first += m.vertices.size();
                    for (const auto& sm : m.submeshes) r.second += sm.indices.size() / 3;
                }
                return r;
            };
            // top: every reference renderer that is shown by default must match vertex and triangle counts
            const assemble::ScenePart* top = partNamed("outfit0.top");
            CHECK(top != nullptr);
            if (top) {
                CHECK(top->status == "resolved-mod");
                CHECK(top->bundle == g["top_resolution"]["main_ab"].get<std::string>());
                for (const auto& r : g["top_prefab"]["renderers"]) {
                    std::string name = r["renderer"];
                    if (name == "n_top_b") continue;   // pulled-off variant, left out on purpose
                    auto got = countsOf(*top, name);
                    CHECK(got.first == r["vertices"].get<size_t>());
                    CHECK(got.second == r["triangles"].get<size_t>());
                }
            }
            const assemble::ScenePart* body = partNamed("body");
            CHECK(body != nullptr);
            if (body) {
                const auto& br = g["body"][0];
                auto got = countsOf(*body, "o_body_a");
                CHECK(got.first == br["vertices"].get<size_t>());
                CHECK(got.second == br["triangles"].get<size_t>());
            }
            // textures that exist in the reference decode to the same pixels
            size_t compared = 0;
            for (const auto& t : scene.textures) {
                auto it = g["top_prefab"]["textures"].find(t.name);
                if (it == g["top_prefab"]["textures"].end()) continue;
                if ((*it)["width"].get<int>() != t.width || (*it)["height"].get<int>() != t.height) continue;
                CHECK(std::to_string(t.pixelHash) == (*it)["rgba_fnv1a64"].get<std::string>());
                ++compared;
            }
            CHECK(compared >= 2);
            std::printf("  golden: %zu reference textures compared\n", compared);
        }

        {
            auto findings = exporter::ValidateScene(scene);
            for (const auto& f : findings) std::printf("  scene finding: %s\n", f.c_str());
            CHECK(findings.empty());
        }
        exporter::ExportOptions eo;
        eo.outDir = tempPath("e2e");
        eo.overwrite = true;
        auto res = exporter::ExportScene(scene, c, eo);
        CHECK(res.vertices > 1000 && res.triangles > 1000 && res.materials > 0 && res.bones > 100);
        CHECK(fileio::Exists(res.pmxPath));
        CHECK(fileio::Exists(res.manifestPath));
        std::printf("  exported %s: %zu verts, %zu tris, %zu materials, %zu bones, %zu textures\n", res.pmxPath.c_str(), res.vertices,
                    res.triangles, res.materials, res.bones, res.textures);
        {   // read the file back with the independent reader: counts, physics, morphs and frames all hold together
            const pmxread::Model back = readPmx(fileio::ReadFile(res.pmxPath));
            CHECK(back.vertices.size() == res.vertices && back.indices.size() == res.triangles * 3 && back.materials.size() == res.materials && back.bones.size() == res.bones);
            size_t ikBack = 0, legs = 0;
            for (const auto& l : exporter::FindLimbs(scene)) if (l.role == "foot") ++legs;   // the MMD names leave the arms to the motion's FK keys
            for (const auto& b : back.bones) if (b.isIk()) ++ikBack;
            // The semi-standard bones the MMD names add (AddMmdStandardBones), each found by name and placed after its parent and append parent.
            size_t standard = 0;
            for (const auto& b : back.bones)
                for (const char* n : {"操作中心", "左肩P", "右肩P", "左肩C", "右肩C", "左腕捩", "右腕捩", "左手捩", "右手捩", "腰キャンセル左", "腰キャンセル右", "左足IK親", "右足IK親",
                                      "上半身1", "左手先", "右手先", "左親指先", "右親指先", "左人差指先", "右人差指先", "左中指先", "右中指先", "左薬指先", "右薬指先", "左小指先", "右小指先"})
                    if (b.name == n) ++standard;
            CHECK(standard == 26);
            bool ordered = true;
            for (size_t i = 0; i < back.bones.size(); ++i) {
                const auto& b = back.bones[i];
                ordered = ordered && b.parent < static_cast<int>(i) && (b.appendParent < 0 || b.appendParent < static_cast<int>(i) || b.layer > back.bones[static_cast<size_t>(b.appendParent)].layer);
            }
            CHECK(ordered);   // MikuMikuDance evaluates bones in list order: a parent after its child lags a frame behind
            CHECK(ikBack >= legs && back.bones.size() == scene.bones.size() + ikBack + 1 + standard);   // (+1: 両目) the scene's bones, then one IK bone per leg (and a toe IK under each foot IK)
            std::printf("  read back: %zu IK bones (%zu legs in the rig)\n", ikBack, legs);
            {   // MikuMikuDance draws the ground shadow and self-shadow only for materials that ask for them (the body did not)
                bool bodyShadow = false;
                for (const auto& m : back.materials) if (m.name.rfind("body/", 0) == 0) bodyShadow = (m.drawFlags & 0x1E) == 0x1E;
                CHECK(bodyShadow);
            }
            size_t expectBodies = 0, expectJoints = 0;
            for (const auto& col : scene.colliders)
                if (col.bone >= 0 && col.bone < static_cast<int>(scene.bones.size()) && !col.inside && col.radius > 0.0f) ++expectBodies;
            for (const auto& chain : scene.chains) {
                if (chain.characterOwn) continue;   // the body's own jiggle chains stay rigid in the PMX
                const size_t valid = exporter::PhysicsChainBones(scene, chain).size();
                if (valid < 2) continue;
                expectBodies += valid;
                expectJoints += valid - 1;
            }
            CHECK(back.bodies.size() == expectBodies && back.joints.size() == expectJoints);
            size_t accBodies = 0;
            for (const auto& b : back.bodies)
                if (b.bone >= 0 && b.bone < static_cast<int>(back.bones.size()) && back.bones[static_cast<size_t>(b.bone)].name.find(".acc") != std::string::npos)
                    ++accBodies;
            CHECK(accBodies > 0);   // the accessory hair's chains reach the PMX, not just the scene
            std::printf("  accessory physics: %zu rigid bodies on accessory bones\n", accBodies);
            const auto findings = pmxPhysicsFindings(back);
            for (size_t i = 0; i < findings.size() && i < 10; ++i) std::printf("  physics finding: %s\n", findings[i].c_str());
            CHECK(findings.empty());
            size_t kinematic = 0, moving = 0;
            for (const auto& b : back.bodies) (b.mode == 0 ? kinematic : moving)++;
            {   // MikuMikuDance keeps the number of listed morphs and of frames in a byte: more corrupt its heap
                size_t listed = 0;
                for (const auto& f : back.frames) for (const auto& it : f.items) listed += it.first == 1;
                CHECK(listed <= 255 && back.frames.size() <= 254);
                CHECK(back.morphs.size() >= listed);   // what does not fit stays in the file, unlisted
                std::printf("  read back: %zu morphs listed in the display frames of %zu\n", listed, back.morphs.size());
            }
            {   // MikuMikuDance writes depth for the face's see-through mouth window: the teeth must be drawn before the face to show through it
                int face = -1, lastTooth = -1;
                for (size_t i = 0; i < back.materials.size(); ++i) {
                    const std::string& n = back.materials[i].name;
                    if (face < 0 && n.find("cf_O_face") != std::string::npos) face = static_cast<int>(i);
                    if (n.find("cf_O_tooth") != std::string::npos || n.find("cf_O_canine") != std::string::npos) lastTooth = static_cast<int>(i);
                }
                CHECK(face < 0 || lastTooth < face);
            }
            {   // facing -Z: the toes lie in front of the ankle, on the -Z side
                const pmxread::Bone* foot = nullptr; const pmxread::Bone* toe = nullptr;
                for (const auto& b : back.bones) { if (b.name == "cf_j_foot_L" || b.name == "左足首") foot = &b; if (b.name == "cf_j_toes_L" || b.name == "左つま先") toe = &b; }
                CHECK(foot && toe && toe->pos.z < foot->pos.z);
            }
            std::printf("  read back: %zu rigid bodies (%zu kinematic, %zu moving), %zu joints, %zu morphs, %zu display frames\n", back.bodies.size(), kinematic, moving, back.joints.size(), back.morphs.size(), back.frames.size());
        }
    });
}

} // namespace

int main() {
    unitTests();
    integrationTests();
    std::printf("\n%d checks, %d failed, %d integration suite(s) skipped\n", g_run, g_failed, g_skipped);
    return g_failed ? 1 : 0;
}
