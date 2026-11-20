#include "library/Library.h"

#include "core/FileIo.h"
#include "core/ZipArchive.h"
#include "unity/UnityScene.h"
#include "unity/UnitySerialized.h"

#include <stb_image_write.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>

namespace cardbridge::library {

std::string AbdataRoot(const std::string& libraryRoot) { return fileio::Join(libraryRoot, "abdata"); }

GameInfo ReadGameInfo(const std::string& libraryRoot) {
    GameInfo g;
    try {
        Bytes raw = fileio::ReadFile(fileio::Join(libraryRoot, "library.json"));
        auto j = nlohmann::json::parse(raw.begin(), raw.end());
        std::string game = j.value("game", "");
        if (game == "koikatsu") g.game = SourceGame::Koikatsu;
        else if (game == "koikatsu-sunshine") g.game = SourceGame::KoikatsuSunshine;
        g.unityVersion = j.value("unity_version", "");
        g.serializedFormat = j.value("serialized_format", 0);
    } catch (const std::exception&) {
        // Pre-KKS libraries have no game fields; callers treat them as Unknown.
    }
    return g;
}

namespace {

// The engine version is written in the bundle header; it is the most direct
// evidence of which game the abdata came from, and it survives a library copy.
GameInfo DetectGameInfo(const std::string& gameRoot) {
    GameInfo g;
    const std::string path = fileio::Join(fileio::Join(gameRoot, "abdata"), "chara/oo_base.unity3d");
    try {
        auto bundle = unity::UnityBundle::Open(fileio::ReadFile(path), path);
        g.unityVersion = bundle->engineVersion();
        if (const auto* sn = bundle->serializedNode()) {
            unity::SerializedFile sf(bundle, *sn);
            g.serializedFormat = sf.formatVersion();
        }
        const size_t dot = g.unityVersion.find('.');
        const int major = std::atoi(g.unityVersion.substr(0, dot).c_str());
        if (major >= 2019) g.game = SourceGame::KoikatsuSunshine;
        else if (major >= 5 && major < 2019) g.game = SourceGame::Koikatsu;
    } catch (const std::exception&) {
        // Fall back to the install's data folder name below.
    }
    if (g.game == SourceGame::Unknown) {
        if (fileio::Exists(fileio::Join(gameRoot, "KoikatsuSunshine_Data"))) g.game = SourceGame::KoikatsuSunshine;
        else if (fileio::Exists(fileio::Join(gameRoot, "Koikatu_Data"))) g.game = SourceGame::Koikatsu;
    }
    return g;
}

// Reads the footer of an exe with an embedded library; false when there is none.
bool ReadFooter(const std::string& exePath, uint64_t& offset, uint64_t& size) {
    try {
        const uint64_t fileSize = fileio::FileSize(exePath);
        if (fileSize < 24) return false;
        const Bytes f = fileio::ReadRange(exePath, fileSize - 24, 24);
        if (std::string(f.begin(), f.begin() + 8) != "CBLIBPK1") return false;
        auto u64 = [&](size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | f[at + static_cast<size_t>(i)]; return v; };
        offset = u64(8);
        size = u64(16);
        return size > 22 && offset + size + 24 <= fileSize;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

bool HasEmbeddedLibrary(const std::string& exePath) {
    uint64_t off = 0, size = 0;
    return ReadFooter(exePath, off, size);
}

bool ExtractEmbeddedLibrary(const std::string& exePath, const std::string& destParent, const std::function<void(size_t, size_t)>& progress, std::string* why,
                            const std::function<bool()>& cancelled) {
    uint64_t off = 0, size = 0;
    if (!ReadFooter(exePath, off, size)) { if (why) *why = "this exe carries no library"; return false; }
    try {
        ZipArchive zip(exePath, off, size);
        size_t done = 0;
        for (const auto& e : zip.entries()) {
            if (cancelled && cancelled()) throw Cancelled{};
            if (!e.name.empty() && e.name.back() == '/') { ++done; continue; }
            if (e.name.find("..") != std::string::npos || e.name.rfind("library/", 0) != 0) continue;   // only library/..., nothing that climbs out
            const std::string dst = fileio::Join(destParent, e.name);
            if (!(fileio::Exists(dst) && fileio::FileSize(dst) == e.size)) {
                const Bytes data = zip.read(e);
                fileio::CreateDirectories(dst.substr(0, dst.find_last_of('/')));
                fileio::WriteFile(dst, data.data(), data.size());
            }
            if (progress) progress(++done, zip.entries().size());
        }
        return true;
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& ex) {
        if (why) *why = ex.what();
        return false;
    }
}

bool ExtractColourGrade(const std::string& gameRoot, const std::string& libraryRoot, std::string* why) {
    const std::string dir = fileio::Join(fileio::Join(gameRoot, "abdata"), "map/scene");
    std::vector<std::string> bundles = fileio::ListFiles(dir, {".unity3d"});
    std::sort(bundles.begin(), bundles.end());
    for (const std::string& path : bundles) {
        try {
            auto bundle = unity::UnityBundle::Open(fileio::ReadFile(path), path);
            unity::UnityFile file(bundle);
            const unity::PPtr p = file.findTexture("Lut_Amount");
            if (p.isNull()) continue;
            auto tex = file.readTexture(p, why);
            if (!tex) return false;
            Bytes png;
            if (!stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n); }, &png, tex->width, tex->height, 4, tex->rgba.data(), tex->width * 4)) {
                if (why) *why = "could not encode the PNG";
                return false;
            }
            fileio::CreateDirectories(fileio::Join(libraryRoot, "colorgrade"));
            fileio::WriteFile(fileio::Join(libraryRoot, "colorgrade/lut_amount.png"), png.data(), png.size());
            return true;
        } catch (const std::exception&) {
            continue;   // a bundle this reader cannot open is not the one
        }
    }
    if (why) *why = "no map scene bundle in " + dir + " holds the colour-grading table";
    return false;
}

ImportResult ImportLibrary(const ImportOptions& opt, const std::function<void(size_t, size_t)>& progress,
                           const std::function<bool()>& cancelled) {
    ImportResult res;
    std::string srcAbdata = fileio::Join(opt.gameRoot, "abdata");
    if (!fileio::Exists(fileio::Join(srcAbdata, "chara/oo_base.unity3d"))) {
        res.issues.push_back("not a game folder: " + srcAbdata + "/chara/oo_base.unity3d does not exist");
        return res;
    }

    // Files to copy, as abdata-relative paths.
    // oo_base: skeleton and body; etc: the shared "none"/dummy prefabs some mods point their empty items at.
    std::vector<std::string> rels{"chara/oo_base.unity3d"};
    if (fileio::Exists(fileio::Join(srcAbdata, "chara/etc.unity3d"))) rels.push_back("chara/etc.unity3d");
    auto addDir = [&](const std::string& sub, bool recurse) {
        std::string dir = fileio::Join(srcAbdata, sub);
        for (const std::string& f : fileio::ListFiles(dir, {".unity3d"}, &res.issues)) {
            std::string rel = f.substr(srcAbdata.size());
            while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(rel.begin());
            if (!recurse && rel.find('/', sub.size() + 1) != std::string::npos) continue;   // top level of `sub` only
            rels.push_back(rel);
        }
    };
    addDir("list/characustom", false);
    if (fileio::Exists(fileio::Join(srcAbdata, "list/customshape.unity3d"))) rels.push_back("list/customshape.unity3d");
    if (fileio::Exists(fileio::Join(srcAbdata, "list/shapecorrect/shapecorrect.unity3d"))) rels.push_back("list/shapecorrect/shapecorrect.unity3d");
    if (opt.profile == Profile::Full) addDir("chara", false);
    std::sort(rels.begin(), rels.end());
    rels.erase(std::unique(rels.begin(), rels.end()), rels.end());

    const GameInfo game = DetectGameInfo(opt.gameRoot);

    nlohmann::json manifest;
    manifest["schema_version"] = 1;
    manifest["profile"] = opt.profile == Profile::Full ? "full" : "minimal";
    manifest["game"] = game.name();
    manifest["unity_version"] = game.unityVersion;
    manifest["serialized_format"] = game.serializedFormat;
    manifest["created_by"] = "cardbridge library import";
    auto& files = manifest["files"] = nlohmann::json::array();

    fileio::CreateDirectories(AbdataRoot(opt.libraryRoot));
    size_t done = 0;
    for (const std::string& rel : rels) {
        if (cancelled && cancelled()) throw Cancelled{};
        std::string src = fileio::Join(srcAbdata, rel);
        std::string dst = fileio::Join(AbdataRoot(opt.libraryRoot), rel);
        try {
            Bytes data = fileio::ReadFile(src);
            uint64_t h = Fnv1a64(data.data(), data.size());
            bool same = fileio::Exists(dst) && fileio::FileSize(dst) == data.size();
            if (same) {
                Bytes old = fileio::ReadFile(dst);
                same = Fnv1a64(old.data(), old.size()) == h;
            }
            if (same) {
                ++res.unchanged;
            } else {
                std::string dir = dst.substr(0, dst.find_last_of('/'));
                fileio::CreateDirectories(dir);
                fileio::WriteFile(dst, data.data(), data.size());
                ++res.copied;
            }
            res.bytes += data.size();
            files.push_back({{"path", rel}, {"size", data.size()}, {"fnv1a64", std::to_string(h)}});
            ++res.files;
        } catch (const std::exception& e) {
            res.issues.push_back(rel + ": " + e.what());
        }
        if (progress) progress(++done, rels.size());
    }
    {   // the colour grading the maker applies (a tiny table, not a bundle: it is not in library.json)
        std::string why;
        if (!ExtractColourGrade(opt.gameRoot, opt.libraryRoot, &why)) res.issues.push_back("colour grading table: " + why + " (the viewer and the export will skip the grading)");
    }
    fileio::WriteTextFile(fileio::Join(opt.libraryRoot, "library.json"), manifest.dump(2) + "\n");
    return res;
}

CheckResult CheckLibrary(const std::string& libraryRoot, bool verifyHashes) {
    CheckResult r;
    std::string mf = fileio::Join(libraryRoot, "library.json");
    if (!fileio::Exists(mf)) {
        r.issues.push_back("no library.json in " + libraryRoot + " - run 'cardbridge library import' first");
        return r;
    }
    try {
        Bytes raw = fileio::ReadFile(mf);
        auto j = nlohmann::json::parse(raw.begin(), raw.end());
        r.profile = j.value("profile", "");
        r.game = ReadGameInfo(libraryRoot);
        for (const auto& f : j["files"]) {
            std::string rel = f.value("path", "");
            std::string p = fileio::Join(AbdataRoot(libraryRoot), rel);
            ++r.files;
            if (!fileio::Exists(p)) { r.issues.push_back("missing: " + rel); continue; }
            if (fileio::FileSize(p) != f.value("size", uint64_t(0))) { r.issues.push_back("size differs: " + rel); continue; }
            if (verifyHashes) {
                Bytes d = fileio::ReadFile(p);
                if (std::to_string(Fnv1a64(d.data(), d.size())) != f.value("fnv1a64", "")) r.issues.push_back("content differs: " + rel);
            }
        }
        r.ok = r.issues.empty();
    } catch (const std::exception& e) {
        r.issues.push_back(std::string("library.json unreadable: ") + e.what());
    }
    return r;
}

} // namespace cardbridge::library
