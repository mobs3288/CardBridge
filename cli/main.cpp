// cardbridge - Koikatsu character card to PMX, offline.
//
//   cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir> [options]
//   cardbridge inspect <card.png> [--library <dir>] [--mods <dir>] [--json]
//   cardbridge library import --game <install> --library <dir> [--minimal]
//   cardbridge library check  --library <dir> [--hashes]
//
// Only `library import` ever reads a game install. Every other command works
// from the card, the CardBridge library folder and the user's mods folder.

#include "shader/Dxbc.h"
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "export/Exporter.h"
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

constexpr const char* kVersion = "0.1.0-m1";

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
        "cardbridge " "0.1.0-m1" " - Koikatsu character card to PMX, offline\n"
        "\n"
        "  cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir>\n"
        "             [--outfit N] [--scale F] [--overwrite] [--mod-cache <file>]\n"
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
    // cardbridge shader <file.dxbc|blob>: lists the constant buffers, resources and signatures of each DXBC program.
    if (a.positional.size() < 2) { std::fprintf(stderr, "usage: cardbridge shader <file>\n"); return 1; }
    Bytes data = fileio::ReadFile(a.positional[1]);
    std::vector<Bytes> progs = shader::CarveDxbc(data);
    if (progs.empty()) { std::fprintf(stderr, "no DXBC program found in %s\n", a.positional[1].c_str()); return 2; }
    int n = 0;
    for (const Bytes& b : progs) {
        shader::Dxbc d;
        std::string why;
        if (!shader::ParseDxbc(b.data(), b.size(), d, &why)) { std::printf("program %d: %s\n", n++, why.c_str()); continue; }
        std::printf("program %d: %s_%u_%u, %zu tokens\n", n++, d.programType == 0 ? "ps" : d.programType == 1 ? "vs" : "xs", d.major, d.minor, d.tokens.size());
        for (const auto& cb : d.cbuffers) {
            std::printf("  cbuffer %s (%u bytes)\n", cb.name.c_str(), cb.size);
            for (const auto& v : cb.vars) std::printf("    +%-4u %-4u %s\n", v.offset, v.size, v.name.c_str());
        }
        for (const auto& r : d.resources) std::printf("  resource %s type %u slot %u\n", r.name.c_str(), r.type, r.bindPoint);
        for (const auto& e : d.inputs) std::printf("  in  %s%u r%u mask %x\n", e.semantic.c_str(), e.index, e.reg, e.mask);
        for (const auto& e : d.outputs) std::printf("  out %s%u r%u mask %x\n", e.semantic.c_str(), e.index, e.reg, e.mask);
    }
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
    auto t0 = std::chrono::steady_clock::now();
    assemble::Scene scene = assemble::Assemble(c, *ctx.resolver, ao);
    auto t1 = std::chrono::steady_clock::now();

    size_t parts = 0, failed = 0;
    for (const auto& p : scene.parts) {
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

    exporter::ExportOptions eo;
    eo.outDir = out;
    eo.overwrite = a.has("overwrite");
    eo.scale = a.opt.count("scale") ? static_cast<float>(std::atof(a.get("scale").c_str())) : 12.5f;
    eo.outfit = ao.outfit >= 0 ? ao.outfit : static_cast<int>(c.status.at("coordinateType").asInt());
    eo.assetLibrary = a.get("library");
    eo.modsFolder = a.get("mods");
    try {
        auto r = exporter::ExportScene(scene, c, eo);
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
    Args a = parseArgs(argc, argv, {"overwrite", "minimal", "hashes", "json", "help", "version"});
    if (a.has("version")) { std::printf("cardbridge %s\n", kVersion); return 0; }
    if (a.positional.empty() || a.has("help")) { usage(); return a.has("help") ? 0 : 1; }
    const std::string& cmd = a.positional[0];
    try {
        if (cmd == "convert") return cmdConvert(a);
        if (cmd == "inspect") return cmdInspect(a);
        if (cmd == "library") return cmdLibrary(a);
        if (cmd == "shader") return cmdShader(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
    usage();
    return 1;
}
