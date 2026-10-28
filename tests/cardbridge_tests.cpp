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
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "core/Inflate.h"
#include "core/Lz4.h"
#include "core/MsgPack.h"
#include "core/Sha256.h"
#include "core/ZipArchive.h"
#include "export/Exporter.h"
#include "library/Library.h"
#include "pmx/PmxWriter.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace cardbridge;

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
