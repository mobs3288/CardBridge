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
#include "card/Abmx.h"
#include "card/CardFile.h"
#include "card/MaterialEdits.h"
#include "core/FileIo.h"
#include "core/Inflate.h"
#include "core/Lz4.h"
#include "core/MsgPack.h"
#include "core/Sha256.h"
#include "core/ZipArchive.h"
#include "export/Exporter.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"
#include "library/Library.h"
#include "pmx/PmxWriter.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "shape/Shape.h"
#include "resolve/Resolver.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
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
    });
}

} // namespace

int main() {
    unitTests();
    integrationTests();
    std::printf("\n%d checks, %d failed, %d integration suite(s) skipped\n", g_run, g_failed, g_skipped);
    return g_failed ? 1 : 0;
}
