// cardbridge - Koikatsu character card to PMX, offline.
//
//   cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir> [options]
//   cardbridge inspect <card.png> [--library <dir>] [--mods <dir>] [--json]
//   cardbridge library import --game <install> --library <dir> [--minimal]
//   cardbridge library check  --library <dir> [--hashes]
//
// Only `library import` ever reads a game install. Every other command works
// from the card, the CardBridge library folder and the user's mods folder.

#include "core/Version.h"
#include "shader/Dxbc.h"
#include "shader/UnityShader.h"
#include "shader/Sm4.h"
#include <cstring>
#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "export/ColourGrade.h"
#include "export/Exporter.h"
#include <stb_image_write.h>

#include "export/GpuView.h"
#include "export/SkeletonOverlay.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace cardbridge;

namespace {

constexpr const char* kVersion = CARDBRIDGE_VERSION;

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opt;
    std::vector<std::string> flags;
    bool has(const std::string& f) const { return std::find(flags.begin(), flags.end(), f) != flags.end(); }
    std::string get(const std::string& k, const std::string& def = {}) const {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

Args parseArgs(int argc, char** argv, const std::vector<std::string>& boolFlags) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            if (std::find(boolFlags.begin(), boolFlags.end(), key) != boolFlags.end()) a.flags.push_back(key);
            else if (i + 1 < argc) a.opt[key] = argv[++i];
            else a.opt[key] = "";
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

#ifdef _WIN32
// argv arrives in the ANSI code page; re-read the command line as UTF-16 so
// Japanese or bracketed paths survive.
std::vector<std::string> utf8Args() {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

void usage() {
    std::puts(
        "cardbridge " CARDBRIDGE_VERSION " - Koikatsu character card to PMX, offline\n"
        "\n"
        "  cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir>\n"
        "             [--outfit N] [--scale F] [--overwrite] [--no-bake | --bake-lit] [--no-grade] [--no-toon-ramp] [--mod-cache <file>]\n"
        "             [--apose [--apose-degrees N]]   arms down from the T-pose (40 degrees by default)\n"
        "             [--pose <file.json>]            a pose saved by the viewer (bone edits and the A-pose switch), baked in\n"
        "             [--unity-facing]                the front toward +Z as built (the PMX default is -Z, MikuMikuDance's front)\n"
        "             [--no-mmd-names]                keep the rig's own bone names; by default the body bones get MMD's\n"
        "                                             standard (Japanese) names so a motion (VMD) finds them\n"
        "             [--no-physics]                  no rigid bodies or joints: hair and skirt stay rigid, nothing can shiver\n"
        "  cardbridge inspect <card.png> [--library <dir>] [--mods <dir>] [--json]\n"
        "  cardbridge library import --game <install> --library <dir> [--minimal]\n"
        "  cardbridge library check --library <dir> [--hashes]\n"
        "\n"
        "Only 'library import' reads a game install (once, to copy files into the library).\n"
        "Exit codes: 0 ok, 1 usage, 2 not a supported card, 3 conversion failed.");
}

std::string defaultCache() {
    const char* la = std::getenv("LOCALAPPDATA");
    return la ? std::string(la) + "/CardBridge/modindex.json" : std::string();
}

std::string normalizeSlashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

void printIssues(const std::vector<std::string>& v, const char* prefix) {
    for (const auto& s : v) std::fprintf(stderr, "%s%s\n", prefix, s.c_str());
}

// Shared by convert and inspect: mods index, base lists, resolver.
struct Context {
    resolve::ModIndex mods;
    resolve::ListTable lists;
    std::unique_ptr<resolve::Resolver> resolver;
    bool haveLibrary = false;
};

bool buildContext(const Args& a, Context& ctx) {
    std::vector<std::string> issues;
    std::string modsDir = normalizeSlashes(a.get("mods"));
    if (!modsDir.empty()) {
        std::string cache = a.get("mod-cache", defaultCache());
        if (!cache.empty()) {
            std::string dir = cache.substr(0, cache.find_last_of("/\\"));
            if (!dir.empty() && dir != cache) { try { fileio::CreateDirectories(dir); } catch (...) {} }
        }
        resolve::ModIndex::Stats st;
        std::fprintf(stderr, "indexing mods in %s ...\n", modsDir.c_str());
        ctx.mods = resolve::ModIndex::Build(modsDir, normalizeSlashes(cache), &issues, &st, [](size_t d, size_t n) {
            std::fprintf(stderr, "  %zu / %zu\r", d, n);
        });
        std::fprintf(stderr, "  %zu mods files (%zu from cache, %zu read, %zu unreadable), %zu distinct GUIDs\n",
                     st.zips, st.reused, st.opened, st.failed, ctx.mods.guidCount());
        printIssues(issues, "  note: ");
        issues.clear();
    }
    std::string lib = normalizeSlashes(a.get("library"));
    std::string abdata;
    if (!lib.empty()) {
        auto chk = library::CheckLibrary(lib, false);
        if (!chk.ok) {
            printIssues(chk.issues, "library: ");
            return false;
        }
        abdata = library::AbdataRoot(lib);
        size_t rows = ctx.lists.loadBaseGame(abdata, &issues);
        printIssues(issues, "  note: ");
        std::fprintf(stderr, "library: %s profile, %zu list rows\n", chk.profile.c_str(), rows);
        ctx.haveLibrary = true;
    }
    ctx.resolver = std::make_unique<resolve::Resolver>(ctx.mods, ctx.lists, abdata);
    return true;
}

int cmdLibrary(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    const std::string& sub = a.positional[1];
    std::string lib = normalizeSlashes(a.get("library"));
    if (lib.empty()) { std::fprintf(stderr, "--library <dir> is required\n"); return 1; }
    if (sub == "import") {
        std::string game = normalizeSlashes(a.get("game"));
        if (game.empty()) { std::fprintf(stderr, "--game <install folder> is required for import\n"); return 1; }
        library::ImportOptions o;
        o.gameRoot = game;
        o.libraryRoot = lib;
        o.profile = a.has("minimal") ? library::Profile::Minimal : library::Profile::Full;
        std::fprintf(stderr, "copying game data into %s (the game folder is only read) ...\n", lib.c_str());
        auto r = library::ImportLibrary(o, [](size_t d, size_t n) { std::fprintf(stderr, "  %zu / %zu\r", d, n); });
        printIssues(r.issues, "  problem: ");
        std::printf("library import: %zu files (%zu copied, %zu already present), %.1f MB\n", r.files, r.copied, r.unchanged,
                    static_cast<double>(r.bytes) / 1048576.0);
        return r.issues.empty() ? 0 : 3;
    }
    if (sub == "check") {
        auto r = library::CheckLibrary(lib, a.has("hashes"));
        printIssues(r.issues, "  problem: ");
        std::printf("library %s: %s, %zu files%s\n", lib.c_str(), r.ok ? "OK" : "PROBLEMS", r.files, r.profile.empty() ? "" : (" (" + r.profile + " profile)").c_str());
        return r.ok ? 0 : 3;
    }
    usage();
    return 1;
}

bool loadCard(const std::string& path, card::Card& c, int& code) {
    try {
        c = card::LoadCard(path);
        return true;
    } catch (const card::CardError& e) {
        std::fprintf(stderr, "%s: %s [%s]\n", path.c_str(), e.what(), card::ErrorKindName(e.kind));
        code = 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        code = 3;
    }
    return false;
}

int cmdInspect(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    card::Card c;
    int code = 0;
    if (!loadCard(normalizeSlashes(a.positional[1]), c, code)) return code;

    nlohmann::json j;
    j["file"] = fileio::FileName(c.path);
    j["sha256"] = c.sha256;
    j["family"] = card::FamilyName(c.family);
    j["marker"] = c.marker;
    j["format_version"] = c.formatVersion;
    j["supported"] = c.supported;
    if (!c.supportNote.empty()) j["note"] = c.supportNote;
    j["blocks"] = nlohmann::json::array();
    for (const auto& b : c.blocks) j["blocks"].push_back({{"name", b.name}, {"version", b.version}, {"size", b.size}});
    if (c.supported) {
        j["character"] = c.displayName();
        j["outfits"] = c.outfits.size();
        j["resolver_rows"] = c.rows.size();
        std::set<std::string> guids;
        for (const auto& r : c.rows) guids.insert(r.modId);
        j["mods_named_by_card"] = guids.size();

        Context ctx;
        if (a.has("json") || !a.get("mods").empty() || !a.get("library").empty()) {
            if (!buildContext(a, ctx)) return 3;
        }
        int outfit = a.opt.count("outfit") ? std::atoi(a.get("outfit").c_str()) : static_cast<int>(c.status.at("coordinateType").asInt());
        j["outfit"] = outfit;
        j["items"] = nlohmann::json::array();
        for (const auto& ref : resolve::ItemsFromCard(c, outfit)) {
            nlohmann::json it = {{"id", ref.partId}, {"category", ref.category}, {"item", ref.id}};
            if (ctx.resolver && (ctx.haveLibrary || !a.get("mods").empty())) {
                auto r = ctx.resolver->resolve(c, ref);
                it["status"] = resolve::StatusName(r.status);
                if (!r.modGuid.empty()) it["mod"] = r.modGuid;
                if (!r.detail.empty()) it["detail"] = r.detail;
                if (r.ok()) it["bundle"] = r.bundleRel();
            }
            j["items"].push_back(std::move(it));
        }
    }
    std::puts(j.dump(2).c_str());
    return 0;
}

int cmdShader(const Args& a) {
    // cardbridge shader <blob>: the programs of a decompressed Unity shader blob with their bindings, and the forward pass.
    if (a.positional.size() < 2) { std::fprintf(stderr, "usage: cardbridge shader <blob>\n"); return 1; }
    Bytes data = fileio::ReadFile(a.positional[1]);
    std::vector<std::string> why;
    auto progs = shader::ParseUnityShaderBlob(data, &why);
    for (const auto& w : why) std::fprintf(stderr, "  skipped: %s\n", w.c_str());
    if (progs.empty()) { std::fprintf(stderr, "no programs found in %s\n", a.positional[1].c_str()); return 2; }
    size_t pi = 0, vi = 0;
    bool fwd = shader::PickForwardPass(progs, pi, vi);
    for (size_t n = 0; n < progs.size(); ++n) {
        const auto& d = progs[n];
        std::printf("program %zu: %s_%u_%u, %zu tokens%s\n", n, d.isPixel() ? "ps" : d.isVertex() ? "vs" : "xs", d.code.major, d.code.minor, d.code.tokens.size(),
                    fwd && (n == pi || n == vi) ? "   <== forward pass" : "");
        if (!d.keywords.empty()) { std::printf("  keywords:"); for (const auto& k : d.keywords) std::printf(" %s", k.c_str()); std::printf("\n"); }
        for (const auto& cb : d.cbuffers) {
            std::printf("  cb%d %s (%u bytes)\n", cb.slot, cb.name.c_str(), cb.size);
            for (const auto& v : cb.params) std::printf("    +%-4u %ux%u%s %s\n", v.offset, v.rows, v.cols, v.isMatrix ? " matrix" : "", v.name.c_str());
        }
        for (const auto& t : d.textures) std::printf("  t%u s%u dim%u %s\n", t.reg, t.sampler, t.dimension, t.name.c_str());
    }
    return 0;
}

int cmdShaderRun(const Args& a) {
    // cardbridge shader-run <blob>: runs the forward pixel program once with synthetic inputs (white textures, unit
    // vectors) to prove the interpreter handles every instruction of the shader; prints the result colour.
    if (a.positional.size() < 2) { std::fprintf(stderr, "usage: cardbridge shader-run <blob>\n"); return 1; }
    Bytes data = fileio::ReadFile(a.positional[1]);
    auto progs = shader::ParseUnityShaderBlob(data, nullptr);
    size_t pi = 0, vi = 0;
    if (!shader::PickForwardPass(progs, pi, vi)) { std::fprintf(stderr, "no forward pass\n"); return 2; }
    const auto& ps = progs[pi];
    shader::Sm4Program sm;
    std::string why;
    if (!shader::DecodeSm4(ps.code.tokens, sm, &why)) { std::fprintf(stderr, "decode: %s\n", why.c_str()); return 2; }
    std::printf("decoded %zu instructions, %u temps\n", sm.code.size(), sm.tempCount);
    struct Tex : shader::ISm4Textures {
        const shader::UnityProgram* p;
        void Sample(int t, int, const float*, float, float out[4]) override {
            bool normal = false;
            for (const auto& b : p->textures) if (static_cast<int>(b.reg) == t && b.name.find("Normal") != std::string::npos) normal = true;
            out[0] = normal ? 0.5f : 1.0f; out[1] = normal ? 0.5f : 1.0f; out[2] = 1.0f; out[3] = 1.0f;
        }
    } tex;
    tex.p = &ps;
    std::vector<std::vector<float>> cbs(8);
    shader::Sm4Io io;
    for (const auto& cb : ps.cbuffers) {
        if (cb.slot < 0 || cb.slot >= 8) continue;
        cbs[static_cast<size_t>(cb.slot)].assign(cb.size / 4 + 4, 0.0f);
        for (const auto& pm : cb.params) {
            float* f = &cbs[static_cast<size_t>(cb.slot)][pm.offset / 4];
            if (pm.name.size() > 3 && pm.name.compare(pm.name.size() - 3, 3, "_ST") == 0) { f[0] = 1; f[1] = 1; }
            else if (pm.name == "_LightColor0" || pm.name == "_ShadowColor") { f[0] = f[1] = f[2] = 0.9f; f[3] = 1; }
            else if (pm.name == "_alpha_a" || pm.name == "_alpha_b") f[0] = 1;
            else if (pm.name == "_WorldSpaceLightPos0") { f[1] = 1; }
        }
    }
    io.constantBuffers.resize(8, nullptr);
    for (size_t i = 0; i < 8; ++i) if (!cbs[i].empty()) io.constantBuffers[i] = cbs[i].data();
    io.textures = &tex;
    for (const auto& in : ps.code.inputs) {
        if (in.reg >= 32) continue;
        float* v = io.input[in.reg];
        if (in.semantic == "SV_IsFrontFace") { uint32_t m = 0xFFFFFFFFu; std::memcpy(v, &m, 4); }
        else { v[0] = 0.3f; v[1] = 0.5f; v[2] = 0.8f; v[3] = 1.0f; }
    }
    if (!shader::RunSm4(sm, io, &why)) { std::fprintf(stderr, "run: %s\n", why.c_str()); return 2; }
    std::printf("%s -> %.4f %.4f %.4f %.4f\n", io.discarded ? "discarded" : "colour", io.output[0][0], io.output[0][1], io.output[0][2], io.output[0][3]);
    return 0;
}

int cmdConvert(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    std::string out = normalizeSlashes(a.get("out"));
    if (out.empty() || a.get("library").empty()) { std::fprintf(stderr, "--library and --out are required\n"); return 1; }
    card::Card c;
    int code = 0;
    if (!loadCard(normalizeSlashes(a.positional[1]), c, code)) return code;
    if (!c.supported) {
        std::fprintf(stderr, "%s: %s\n", c.path.c_str(), c.supportNote.c_str());
        return 2;
    }
    Context ctx;
    if (!buildContext(a, ctx)) return 3;

    assemble::AssembleOptions ao;
    if (a.opt.count("outfit")) ao.outfit = std::atoi(a.get("outfit").c_str());
    // --body "4=0.8,13=0.1" / --face "..." set sliders (index=value, 0..1 is the maker's range); --nipple-inside pushes the nipples in.
    for (const char* which : {"body", "face"}) {
        std::string spec = a.get(which);
        std::vector<float>& vals = std::string(which) == "body" ? ao.bodyShape : ao.faceShape;
        const Value& card0 = std::string(which) == "body" ? c.body.at("shapeValueBody") : c.face.at("shapeValueFace");
        for (size_t i = 0; i < card0.size(); ++i) vals.push_back(static_cast<float>(card0.at(i).asFloat()));
        for (size_t at = 0; at < spec.size();) {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(at, end - at);
            at = end + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            size_t idx = static_cast<size_t>(std::atoi(item.c_str()));
            if (idx < vals.size()) vals[idx] = static_cast<float>(std::atof(item.c_str() + eq + 1));
        }
    }
    ao.nippleInside = a.has("nipple-inside");
    if (a.opt.count("nipple-offset")) ao.nippleOffset = static_cast<float>(std::atof(a.get("nipple-offset").c_str()));
    ao.log = [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); };
    auto t0 = std::chrono::steady_clock::now();
    assemble::Scene scene = assemble::Assemble(c, *ctx.resolver, ao);
    auto t1 = std::chrono::steady_clock::now();
    size_t parts = 0, failed = 0;
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.nudeSwitch) continue;   // derived from other parts
        ++parts;
        bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
        if (!ok) ++failed;
        std::fprintf(stderr, "  %-22s %-18s %zu mesh(es)%s%s\n", p.id.c_str(), p.status.c_str(), p.meshes.size(),
                     p.modName.empty() ? "" : "  mod: ", p.modName.c_str());
    }
    for (const auto& is : scene.issues)
        std::fprintf(stderr, "  %s%s%s: %s\n", is.severity.c_str(), is.part.empty() ? "" : " [", is.part.empty() ? "" : (is.part + "]").c_str(), is.message.c_str());
    for (const auto& is : scene.issues)
        if (is.severity == "error") { std::fprintf(stderr, "conversion failed.\n"); return 3; }

    std::vector<Mat4> liveSkin;
    // --apose [--apose-degrees N] brings the arms down from the T-pose (clavicle and upper arm), 40 degrees by default; the PMX is written in that pose.
    // --pose <file.json> bakes a pose saved by the viewer (bone edits and the A-pose switch); with --apose the arms come down as well.
    assemble::PoseFile poseFile;
    if (a.opt.count("pose")) {
        std::string err;
        try {
            const Bytes raw = fileio::ReadFile(a.get("pose"));
            if (!assemble::PoseFileFromJson(std::string(raw.begin(), raw.end()), poseFile, &err)) {
                std::fprintf(stderr, "error: %s: %s\n", a.get("pose").c_str(), err.c_str());
                return 1;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: cannot read the pose file %s: %s\n", a.get("pose").c_str(), e.what());
            return 1;
        }
    }
    if (a.has("apose")) poseFile.aPose = true;
    if (a.opt.count("apose-degrees")) poseFile.aPoseDegrees = static_cast<float>(std::atof(a.get("apose-degrees").c_str()));
    if (poseFile.aPose || !poseFile.bones.empty()) {
        std::vector<std::string> unknown;
        const assemble::Pose pose = assemble::MakePose(scene, poseFile, &unknown);
        for (const auto& name : unknown) std::fprintf(stderr, "  pose: this rig has no bone '%s'; skipped\n", name.c_str());
        if (poseFile.aPose && pose.base.empty()) std::fprintf(stderr, "  a-pose: this rig has no arm bones to turn\n");
        // Test aid: CARDBRIDGE_GPU_SKIN_TEST=1 leaves the scene at rest and poses the GPU view through its quick re-skin (what the viewer does live).
        if (std::getenv("CARDBRIDGE_GPU_SKIN_TEST")) liveSkin = assemble::SkinMatrices(scene, pose);
        else assemble::BakePose(scene, pose);
        if (!pose.base.empty()) std::fprintf(stderr, "  a-pose: arms down %.0f degrees\n", poseFile.aPoseDegrees);
        if (!poseFile.bones.empty()) std::fprintf(stderr, "  pose: %zu bone edit(s) from %s\n", poseFile.bones.size() - unknown.size(), a.get("pose").c_str());
    }

    // --hide outfit0.top,outfit0.bra unticks parts and --nude ticks the Nude body box: the picture and the PMX start that way (the viewer's checkboxes)
    std::vector<char> hiddenParts(scene.parts.size(), 0);
    {
        std::string list = "," + a.get("hide") + ",";
        for (size_t i = 0; i < scene.parts.size(); ++i) {
            if (list.find("," + scene.parts[i].id + ",") != std::string::npos) hiddenParts[i] = 1;
            if (scene.parts[i].nudeSwitch) hiddenParts[i] = a.has("nude") ? 0 : 1;
        }
    }
    exporter::ApplyPartStateForExport(scene, hiddenParts);
    const std::vector<char> effHidden = exporter::EffectiveHiddenParts(scene, hiddenParts);

    exporter::LoadColourGrade(a.get("library"));   // the game's colour grading, when the library holds it
    if (a.has("no-grade")) exporter::SetColourGradeEnabled(false);
    // Default: colour only (the viewer or engine lights it, over a white material) with the preview's grading baked in.
    // --bake-lit puts the toon shading in the texture too (the material is then unlit: black in an engine that lights it itself).
    if (!a.has("no-bake")) {   // the game's shaders run over every material's UV layout, so the PMX textures carry their colour
        std::string bakeNote;
        auto tb0 = std::chrono::steady_clock::now();
        exporter::BakeShaderTextures(scene, 1024, a.has("bake-lit"), &bakeNote,
                                     [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); }, true, !a.has("no-toon-ramp"));
        std::fprintf(stderr, "  %s (%.1fs)\n", bakeNote.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - tb0).count());
    }

    std::vector<uint8_t> gpuRgba, plainRgba;   // written after the export, which with --overwrite clears the folder first
    if (a.has("gpu-view")) {   // debugging aid: the same scene through Direct3D 11, written next to the other pictures
        exporter::GpuView gpu;
        std::string why;
        if (!gpu.Init(&why) || !gpu.Load(scene, &why)) std::fprintf(stderr, "  gpu view: %s\n", why.c_str());
        else {
            if (!liveSkin.empty()) gpu.SetSkin(&liveSkin);
            if (a.opt.count("layers-dir")) {   // the extra shader layers in that folder, all switched on for the picture
                std::string report;
                auto defs = exporter::LoadShaderLayers({normalizeSlashes(a.get("layers-dir"))});
                gpu.SetLayers(defs, &report);
                for (size_t i = 0; i < gpu.layerCount(); ++i) gpu.SetLayerEnabled(i, true);
                std::fprintf(stderr, "  gpu view: %zu layer(s)%s%s\n", defs.size(), report.empty() ? "" : ", problems: ", report.c_str());
            }
            exporter::ViewCamera cam;
            cam.pitchDeg = 4;
            // Test aid: CARDBRIDGE_GPU_CAM=yaw,pitch,zoom,panX,panY frames the picture (a close-up of the chest, say 0,4,3.5,0,0.2).
            if (const char* gc = std::getenv("CARDBRIDGE_GPU_CAM")) std::sscanf(gc, "%f,%f,%f,%f,%f", &cam.yawDeg, &cam.pitchDeg, &cam.zoom, &cam.panX, &cam.panY);
            std::vector<uint32_t> px;
            auto g0 = std::chrono::steady_clock::now();
            bool ok = gpu.Render(cam, 640, 800, &effHidden, px);
            ok = ok && gpu.Render(cam, 640, 800, &effHidden, px);
            if (ok && a.has("gpu-bones")) {   // the skeleton over the picture (--gpu-bones-helpers: the driven, shape and attach nodes too)
                std::vector<Vec3> posed;
                if (!liveSkin.empty()) for (size_t i = 0; i < scene.bones.size(); ++i) posed.push_back(liveSkin[i].point(scene.bones[i].pos));
                exporter::DrawSkeletonOverlay(scene, posed, gpu, px, 640, 800, -1, -1, a.has("gpu-bones-helpers"), nullptr);
            }
            std::fprintf(stderr, "  gpu view: %s, second frame %.1f ms, %d material(s) without a shader\n", ok ? "ok" : "failed",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() / 2, gpu.skippedMaterials());
            for (const auto& m : scene.materials) if (m.shaderIndex < 0) std::fprintf(stderr, "    no shader: %s (%s)\n", m.name.c_str(), m.shader.c_str());
            if (ok && !a.has("no-bake")) {   // the baked textures on plain materials: what a PMX viewer shows (the viewer's X key)
                gpu.SetPlain(true);
                std::vector<uint32_t> pp;
                if (gpu.Render(cam, 640, 800, &effHidden, pp)) {
                    plainRgba.resize(pp.size() * 4);
                    for (size_t i = 0; i < pp.size(); ++i) { plainRgba[i * 4] = (pp[i] >> 16) & 255; plainRgba[i * 4 + 1] = (pp[i] >> 8) & 255; plainRgba[i * 4 + 2] = pp[i] & 255; plainRgba[i * 4 + 3] = 255; }
                }
                gpu.SetPlain(false);
            }
            if (ok) {
                gpuRgba.resize(px.size() * 4);
                for (size_t i = 0; i < px.size(); ++i) { gpuRgba[i * 4] = (px[i] >> 16) & 255; gpuRgba[i * 4 + 1] = (px[i] >> 8) & 255; gpuRgba[i * 4 + 2] = px[i] & 255; gpuRgba[i * 4 + 3] = 255; }
            }
        }
    }

    exporter::ExportOptions eo;
    eo.outDir = out;
    eo.overwrite = a.has("overwrite");
    eo.faceMinusZ = !a.has("unity-facing");
    eo.physics = !a.has("no-physics");
    eo.mmdNames = !a.has("no-mmd-names");
    eo.scale = a.opt.count("scale") ? static_cast<float>(std::atof(a.get("scale").c_str())) : 12.5f;
    eo.outfit = ao.outfit >= 0 ? ao.outfit : static_cast<int>(c.status.at("coordinateType").asInt());
    eo.assetLibrary = a.get("library");
    eo.modsFolder = a.get("mods");
    eo.log = [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); };
    try {
        auto r = exporter::ExportScene(scene, c, eo);
        if (!gpuRgba.empty()) stbi_write_png((out + "/gpu_view.png").c_str(), 640, 800, 4, gpuRgba.data(), 640 * 4);
        if (!plainRgba.empty()) stbi_write_png((out + "/gpu_plain.png").c_str(), 640, 800, 4, plainRgba.data(), 640 * 4);
        double secs = std::chrono::duration<double>(t1 - t0).count();
        std::printf("wrote %s\n  %zu vertices, %zu triangles, %zu materials, %zu bones, %zu textures; %zu of %zu card items loaded (assembled in %.1fs)\n  manifest: %s\n",
                    r.pmxPath.c_str(), r.vertices, r.triangles, r.materials, r.bones, r.textures, parts - failed - 0, parts, secs, r.manifestPath.c_str(), r.previewPath.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "export failed: %s\n", e.what());
        return 3;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    std::vector<std::string> wide = utf8Args();
    std::vector<char*> ptrs;
    for (auto& s : wide) ptrs.push_back(s.data());
    argc = static_cast<int>(ptrs.size());
    argv = ptrs.data();
#endif
    Args a = parseArgs(argc, argv, {"overwrite", "minimal", "hashes", "json", "help", "version", "no-bake", "bake-flat", "bake-lit", "no-grade", "no-toon-ramp", "gpu-view", "nipple-inside", "nude", "apose", "unity-facing", "no-physics", "no-mmd-names", "gpu-bones", "gpu-bones-helpers"});
    if (a.has("version")) { std::printf("cardbridge %s\n", kVersion); return 0; }
    if (a.positional.empty() || a.has("help")) { usage(); return a.has("help") ? 0 : 1; }
    const std::string& cmd = a.positional[0];
    try {
        if (cmd == "convert") return cmdConvert(a);
        if (cmd == "inspect") return cmdInspect(a);
        if (cmd == "library") return cmdLibrary(a);
        if (cmd == "shader") return cmdShader(a);
        if (cmd == "shader-run") return cmdShaderRun(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
    usage();
    return 1;
}
