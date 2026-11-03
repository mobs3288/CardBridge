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
#include "assemble/DynamicBone.h"
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
#include "export/BoneLabels.h"
#include "export/Exporter.h"
#include "export/PmxIk.h"
#include "export/PmxPhysics.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"
#include "library/Library.h"
#include "pmx/PmxWriter.h"
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

    test("physics: a chain becomes an anchor, a capsule per bone and joints; colliders become kinematic bodies; read back from the bytes", [] {
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

        col.height = 0.4f;
        col.axis = {0, 1, 0};
        s.colliders.push_back(col);
        assemble::SceneCollider inside = col;
        inside.inside = true;
        s.colliders.push_back(inside);
        pmx::Model m;
        for (const auto& b : s.bones) { pmx::Bone pb; pb.name = b.name; pb.parent = b.parent; pb.position = b.pos * 10.0f; m.bones.push_back(pb); }
        exporter::AddPhysics(s, m, 10.0f);
        CHECK(m.rigidBodies.size() == 6);   // collider + anchor + one per chain bone (the inside collider is not exported)
        CHECK(m.joints.size() == 4);        // anchor-root, root-1, 1-2, 2-3
        CHECK(m.rigidBodies[0].mode == 0 && m.rigidBodies[0].group != exporter::kPmxGroupChain && m.rigidBodies[0].shape == 2);
        CHECK(m.rigidBodies[1].mode == 0 && m.rigidBodies[1].bone == 0);   // the anchor follows the chain's parent bone
        CHECK(m.rigidBodies[2].mode == 2 && m.rigidBodies[3].mode == 1 && m.rigidBodies[3].group == 1);   // the root only turns, the rest are free
        CHECK(std::fabs(m.rigidBodies[3].position.y - 9.5f) < 1e-3f);   // halfway from bone 2 (y 1.0) to bone 3 (0.9), scaled by 10: 9.5
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
        // the filter: chains meet colliders (bit 0), colliders meet chains (bit 1), the anchor meets nothing
        CHECK(back.bodies[0].mask == 0x0002 && back.bodies[1].mask == 0 && (back.bodies[3].mask & 0x0002) == 0);
        // a broken file is found: a chain that meets chains, an anchor that moves, a joint on a missing body
        pmxread::Model broken = back;
        broken.bodies[3].mask = 0xFFFF;
        broken.bodies[1].mode = 1;
        broken.joints[0].b = 99;
        CHECK(pmxPhysicsFindings(broken).size() >= 3);
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
            CHECK(maxAngle < 1.9);                                  // at most a little past the widest limit (1.5 rad; the reference stop is soft)
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
            const double tipDrop = (back.bodies[1].pos.y - ref.jointPoint(ref.pinCount() - 1).y) / scale;   // below where the anchor stands, metres
            std::printf("  ribbon droop: tip %.3f m below its rest height over a strand of 0.3 m, speed %.4f\n", tipDrop, ref.speed());
            CHECK(tipDrop > 0.0 && tipDrop < 0.15);
            CHECK(ref.speed() < 0.05);
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
        s.expressions.push_back(ex);
        pmx::Model m;
        for (int i = 0; i < 10; ++i) m.vertices.push_back({});
        size_t added = exporter::AddExpressionMorphs(s, m, {5}, 2.0f);
        CHECK(added >= 1 && m.morphs[0].name == "Eyes 0 closed" && m.morphs[0].panel == 2);
        // vertex 1 (PMX 6): open from 100 to 0 = -1 * (0,1,0), close from 0 to 100 = +1 * (0,-2,0), scaled by 2: (0,-6,0); vertex 2 (PMX 7): (2,0,0)
        std::map<int, Vec3> got;
        for (const auto& [v, d] : m.morphs[0].vertices) got[v] = d;
        CHECK(got.size() == 2 && std::fabs(got[6].y + 6.0f) < 1e-4f && std::fabs(got[7].x - 2.0f) < 1e-4f);
        // a mesh that was not exported contributes nothing
        pmx::Model m2;
        m2.vertices.resize(10);
        CHECK(exporter::AddExpressionMorphs(s, m2, {-1}, 2.0f) == 0);
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
        // weights sum to 1 on every vertex
        size_t badWeights = 0;
        for (const auto& m : scene.meshes)
            for (const auto& v : m.vertices) {
                float s = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
                if (std::abs(s - 1.0f) > 1e-3f) ++badWeights;
            }
        CHECK(badWeights == 0);
        {   // every transform of the body and head rig prefabs is a scene bone (by name; the face bones included)
            std::string why;
            auto base = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
            CHECK(base != nullptr);
            if (base) {
                std::set<std::string> rigNames;
                for (const char* prefab : {"p_cf_body_bone", "p_cf_head_bone"}) {
                    auto roots = base->findXformsByName(prefab);
                    CHECK(!roots.empty());
                    if (roots.empty()) continue;
                    for (int64_t id : base->subtree(roots[0])) rigNames.insert(base->xform(id)->name);
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
            size_t ikBack = 0;
            for (const auto& b : back.bones) if (b.isIk()) ++ikBack;
            CHECK(ikBack == exporter::FindLimbs(scene).size() && back.bones.size() == scene.bones.size() + ikBack);   // the scene's bones, then one IK bone per limb
            std::printf("  read back: %zu IK bones (%zu limbs in the rig)\n", ikBack, exporter::FindLimbs(scene).size());
            size_t expectBodies = 0, expectJoints = 0;
            for (const auto& col : scene.colliders)
                if (col.bone >= 0 && col.bone < static_cast<int>(scene.bones.size()) && !col.inside && col.radius > 0.0f) ++expectBodies;
            for (const auto& chain : scene.chains) {
                size_t valid = 0;
                for (int b : chain.bones) if (b >= 0 && b < static_cast<int>(scene.bones.size())) ++valid;
                if (valid < 2) continue;
                expectBodies += 1 + valid;
                expectJoints += valid;
            }
            CHECK(back.bodies.size() == expectBodies && back.joints.size() == expectJoints);
            const auto findings = pmxPhysicsFindings(back);
            for (size_t i = 0; i < findings.size() && i < 10; ++i) std::printf("  physics finding: %s\n", findings[i].c_str());
            CHECK(findings.empty());
            size_t kinematic = 0, moving = 0;
            for (const auto& b : back.bodies) (b.mode == 0 ? kinematic : moving)++;
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
            {   // MikuMikuDance keeps the number of listed morphs and of frames in a byte: more corrupt its heap
                size_t listed = 0;
                for (const auto& f : back.frames) for (const auto& it : f.items) listed += it.first == 1;
                CHECK(listed <= 255 && back.frames.size() <= 254);
                CHECK(back.morphs.size() >= listed);   // what does not fit stays in the file, unlisted
                std::printf("  read back: %zu morphs listed in the display frames of %zu\n", listed, back.morphs.size());
            }
            {   // facing -Z: the toes lie in front of the ankle, on the -Z side
                const pmxread::Bone* foot = nullptr; const pmxread::Bone* toe = nullptr;
                for (const auto& b : back.bones) { if (b.name == "cf_j_foot_L") foot = &b; if (b.name == "cf_j_toes_L") toe = &b; }
                CHECK(foot && toe && toe->pos.z < foot->pos.z);
            }
