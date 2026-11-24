#include "library/AssetCatalog.h"

#include "core/FileIo.h"
#include "core/ZipArchive.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "unity/UnityBundle.h"
#include "unity/UnityScene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace cardbridge::library {
namespace {

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isTextureKey(const std::string& key) {
    return key.find("Tex") != std::string::npos || key.find("Mask") != std::string::npos;
}

} // namespace

const char* CategoryName(AssetCategory a) {
    switch (a) {
        case AssetCategory::Clothing: return "Clothing";
        case AssetCategory::Hair: return "Hair";
        case AssetCategory::Accessories: return "Accessories";
        case AssetCategory::Body: return "Body";
        case AssetCategory::Textures: return "Textures";
        default: return "Other";
    }
}

AssetCategory CategoryFromName(const std::string& name) {
    const std::string n = lower(name);
    for (int i = 0; i < static_cast<int>(AssetCategory::Count); ++i)
        if (n == lower(CategoryName(static_cast<AssetCategory>(i)))) return static_cast<AssetCategory>(i);
    return AssetCategory::Other;
}

const char* SubcategoryName(int c) {
    switch (c) {
        case 100: return "Head";
        case 101: return "Back hair";
        case 102: return "Front hair";
        case 103: return "Side hair";
        case 104: return "Hair extension";
        case 105: return "Tops";
        case 106: return "Bottoms";
        case 107: return "Bras";
        case 108: return "Shorts";
        case 109: return "Gloves";
        case 110: return "Pantyhose";
        case 111: return "Socks";
        case 112: return "Shoes";
        case 122: case 123: return "Hair accessories";
        case 200: case 201: case 202: case 210: case 211: case 212: return "Outerwear";
        case 400: case 401: case 402: case 403: case 404: case 405: return "Makeup";
        case 406: case 407: case 408: case 409: case 410: case 411: case 412: case 413: case 414: case 415: return "Face";
        case 420: case 421: case 422: case 423: case 424: return "Body paint";
        case 430: case 432: case 433: return "Hair / clothing textures";
        default: return nullptr;
    }
}

std::string CatalogFingerprint(const CatalogOptions& opt) {
    std::string key = "v1|" + opt.libraryRoot + "|" + opt.modsRoot + "|";
    std::vector<std::string> skipped;
    const std::string lists = fileio::Join(library::AbdataRoot(opt.libraryRoot), "list/characustom");
    for (const std::string& f : fileio::ListFiles(lists, {".unity3d"}, &skipped)) {
        key += fileio::FileName(f) + ":" + std::to_string(fileio::FileSize(f)) + ":" +
               std::to_string(fileio::LastWriteTicks(f)) + ";";
    }
    std::vector<std::string> issues;
    key += "|mods:" + resolve::ModSourceFingerprint(opt.modsRoot, &issues);
    return std::to_string(fnv1a(key)) + "-" + std::to_string(key.size());
}

bool LoadCatalog(const std::string& cachePath, const std::string& fingerprint, AssetCatalog& out) {
    if (cachePath.empty() || fingerprint.empty()) return false;
    try {
        const Bytes bytes = fileio::ReadFile(cachePath);
        nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_discarded() || j.value("version", 0) != 1 || j.value("fingerprint", std::string()) != fingerprint) return false;
        out.records.clear();
        out.deduplicated = j.value("deduplicated", size_t(0));
        for (const auto& r : j.value("records", nlohmann::json::array())) {
            AssetRecord a;
            a.key = r.value("key", std::string());
            a.game = r.value("game", std::string()) == "koikatsu-sunshine" ? SourceGame::KoikatsuSunshine
                   : r.value("game", std::string()) == "koikatsu" ? SourceGame::Koikatsu : SourceGame::Unknown;
            a.category = CategoryFromName(r.value("category", std::string()));
            a.categoryNo = r.value("categoryNo", 0);
            a.id = r.value("id", int64_t(0));
            a.name = r.value("name", std::string());
            a.subcategory = r.value("subcategory", std::string());
            a.mod = r.value("mod", false);
            a.modGuid = r.value("modGuid", std::string());
            a.modName = r.value("modName", std::string());
            a.modVersion = r.value("modVersion", std::string());
            a.modAuthor = r.value("modAuthor", std::string());
            a.modPath = r.value("modPath", std::string());
            a.bundle = r.value("bundle", std::string());
            a.asset = r.value("asset", std::string());
            a.thumbBundle = r.value("thumbBundle", std::string());
            a.thumbAsset = r.value("thumbAsset", std::string());
            a.parent = r.value("parent", std::string());
            a.textureDeps = r.value("textureDeps", std::vector<std::string>{});
            a.flags = r.value("flags", std::string());
            out.records.push_back(std::move(a));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool SaveCatalog(const std::string& cachePath, const std::string& fingerprint, const AssetCatalog& cat) {
    if (cachePath.empty()) return false;
    try {
        nlohmann::json j;
        j["version"] = 1;
        j["fingerprint"] = fingerprint;
        j["deduplicated"] = cat.deduplicated;
        auto& rows = j["records"] = nlohmann::json::array();
        for (const AssetRecord& a : cat.records) {
            nlohmann::json r;
            r["key"] = a.key;
            r["game"] = a.game == SourceGame::KoikatsuSunshine ? "koikatsu-sunshine"
                      : a.game == SourceGame::Koikatsu ? "koikatsu" : "unknown";
            r["category"] = CategoryName(a.category);
            r["categoryNo"] = a.categoryNo;
            r["id"] = a.id;
            r["name"] = a.name;
            r["subcategory"] = a.subcategory;
            r["mod"] = a.mod;
            r["modGuid"] = a.modGuid;
            r["modName"] = a.modName;
            r["modVersion"] = a.modVersion;
            r["modAuthor"] = a.modAuthor;
            r["modPath"] = a.modPath;
            r["bundle"] = a.bundle;
            r["asset"] = a.asset;
            r["thumbBundle"] = a.thumbBundle;
            r["thumbAsset"] = a.thumbAsset;
            r["parent"] = a.parent;
            r["textureDeps"] = a.textureDeps;
            r["flags"] = a.flags;
            rows.push_back(std::move(r));
        }
        fileio::WriteTextFile(cachePath, j.dump());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

AssetCatalog BuildCatalog(const CatalogOptions& opt, std::vector<std::string>* issues,
                          const std::function<void(size_t, size_t)>& progress,
                          const std::function<bool()>& cancelled) {
    AssetCatalog cat;
    const std::string fingerprint = CatalogFingerprint(opt);
    if (LoadCatalog(opt.cachePath, fingerprint, cat)) {
        if (progress) progress(1, 1);
        return cat;   // cached: no game data is read at all
    }

    if (cancelled && cancelled()) return cat;
    const GameInfo info = ReadGameInfo(opt.libraryRoot);
    resolve::ModIndex mods = resolve::ModIndex::Build(opt.modsRoot, opt.modCachePath, issues, nullptr,
        [&](size_t done, size_t total) { if (progress) progress(done, total ? total + 1 : 0); }, cancelled);
    mods.setGame(info);
    if (cancelled && cancelled()) return cat;

    resolve::ListTable lists;
    lists.loadBaseGame(AbdataRoot(opt.libraryRoot), issues);
    for (const resolve::ModEntry* e : mods.allEntries()) {
        if (!e->compatibleWith(info)) continue;
        lists.addMod(e->path, e->guid, issues);
        if (cancelled && cancelled()) return cat;
    }
    if (progress) progress(1, 1);

    std::vector<AssetRecord> rows;
    rows.reserve(lists.baseCount() + lists.modRowCount());
    for (const resolve::ListItem& item : lists.all()) {
        AssetRecord a;
        const std::string gk = info.name();
        a.key = gk + "|" + std::to_string(item.category) + "|" + std::to_string(item.id) + "|" +
                (item.origin == resolve::ListItem::Origin::BaseGame ? std::string("base") : item.modGuid);
        a.game = info.game;
        a.categoryNo = item.category;
        a.id = item.id;
        a.name = item.name;
        a.mod = item.origin == resolve::ListItem::Origin::Mod;
        a.modGuid = item.modGuid;
        a.bundle = item.get("MainAB");
        a.asset = item.get("MainData");
        a.thumbBundle = item.get("ThumbAB");
        a.thumbAsset = item.get("ThumbTex");
        a.parent = item.get("Parent");
        if (const char* sub = SubcategoryName(item.category)) a.subcategory = sub;
        if (a.mod) {
            const std::vector<resolve::ModEntry>* matches = mods.find(item.modGuid);
            if (matches && !matches->empty()) {
                a.modName = matches->front().name;
                a.modVersion = matches->front().version;
                a.modAuthor = matches->front().author;
                a.modPath = matches->front().path;
            }
        }
        for (const auto& [k, v] : item.keys) {
            if (isTextureKey(k) && !v.empty() && v != "0") a.textureDeps.push_back(k + "=" + v);
            if ((k == "HideShorts" || k == "MabUV" || k == "KokanHide" || k == "NotBra") &&
                !v.empty() && v != "0") {
                if (!a.flags.empty()) a.flags += ",";
                a.flags += k;
            }
        }
        std::sort(a.textureDeps.begin(), a.textureDeps.end());
        switch (item.category) {
            case 100: case 420: case 421: case 422: case 423: case 424:
                a.category = AssetCategory::Body; break;
            case 101: case 102: case 103: case 104: case 122: case 123: case 432: case 433:
                a.category = AssetCategory::Hair; break;
            case 105: case 106: case 107: case 108: case 109: case 110: case 111: case 112:
            case 200: case 201: case 202: case 210: case 211: case 212: case 430:
                a.category = AssetCategory::Clothing; break;
            case 400: case 401: case 402: case 403: case 404: case 405:
                a.category = AssetCategory::Textures; break;
            case 121: case 124: case 125: case 126: case 127: case 128: case 129: case 130:
                a.category = AssetCategory::Accessories; break;
            default:
                if (item.category >= 121 && item.category <= 130) a.category = AssetCategory::Accessories;
                else if (item.category >= 400 && item.category <= 433) a.category = AssetCategory::Textures;
                else a.category = AssetCategory::Other;
                break;
        }
        if (a.subcategory.empty()) {
            if (a.category == AssetCategory::Accessories) {
                const size_t ab = a.bundle.find("ao_");
                a.subcategory = ab == std::string::npos ? "Accessory " + std::to_string(item.category)
                                                        : "Accessory (" + a.bundle.substr(ab, a.bundle.find('/', ab) == std::string::npos ? std::string::npos : a.bundle.find('/', ab) - ab) + ")";
            } else {
                a.subcategory = "Category " + std::to_string(item.category);
            }
        }
        rows.push_back(std::move(a));
    }
    std::sort(rows.begin(), rows.end(), [](const AssetRecord& a, const AssetRecord& b) {
        if (a.category != b.category) return a.category < b.category;
        if (a.subcategory != b.subcategory) return a.subcategory < b.subcategory;
        if (a.name != b.name) return a.name < b.name;
        return a.key < b.key;
    });
    std::vector<std::string> seen;
    seen.reserve(rows.size());
    for (AssetRecord& r : rows) {
        if (std::find(seen.begin(), seen.end(), r.key) != seen.end()) { ++cat.deduplicated; continue; }
        seen.push_back(r.key);
        cat.records.push_back(std::move(r));
    }
    SaveCatalog(opt.cachePath, fingerprint, cat);
    return cat;
}

std::vector<const AssetRecord*> Query(const AssetCatalog& cat, AssetCategory category,
                                      const std::string& search, size_t limit) {
    std::vector<const AssetRecord*> out;
    const std::string needle = lower(search);
    for (const AssetRecord& r : cat.records) {
        if (category != AssetCategory::Count && r.category != category) continue;
        if (!needle.empty()) {
            const std::string hay = lower(r.name + " " + r.subcategory + " " + std::to_string(r.id) + " " +
                                          r.modGuid + " " + r.modName + " " + r.key);
            if (hay.find(needle) == std::string::npos) continue;
        }
        out.push_back(&r);
        if (limit && out.size() >= limit) break;
    }
    return out;
}

bool LoadAssetThumbnail(const AssetRecord& record, const std::string& libraryRoot,
                        std::vector<uint8_t>& rgba, int& width, int& height) {
    rgba.clear(); width = height = 0;
    if (record.thumbBundle.empty() || record.thumbAsset.empty() || record.thumbAsset == "0") return false;
    std::string rel = record.thumbBundle;
    for (char& c : rel) if (c == '\\') c = '/';
    try {
        Bytes bytes;
        if (record.mod && !record.modPath.empty()) {
            ZipArchive zip(record.modPath);
            const std::string ab = rel.rfind("abdata/", 0) == 0 ? rel : "abdata/" + rel;
            const ZipArchive::Entry* e = zip.find(ab);
            if (!e) {
                const std::string want = lower(ab);
                for (const ZipArchive::Entry& candidate : zip.entries())
                    if (lower(candidate.name) == want) { e = &candidate; break; }
            }
            if (!e) return false;
            bytes = zip.read(*e);
        } else {
            bytes = fileio::ReadFile(fileio::Join(AbdataRoot(libraryRoot), rel));
        }
        auto bundle = unity::UnityBundle::Open(std::move(bytes), record.thumbBundle);
        unity::UnityFile file(bundle);
        const unity::PPtr texRef = file.findTexture(record.thumbAsset);
        if (texRef.isNull()) return false;
        const auto tex = file.readTexture(texRef);
        if (!tex || tex->rgba.empty() || tex->width <= 0 || tex->height <= 0) return false;
        rgba = tex->rgba; width = tex->width; height = tex->height;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::map<AssetCategory, size_t> CategoryCounts(const AssetCatalog& cat) {
    std::map<AssetCategory, size_t> counts;
    for (const AssetRecord& r : cat.records) ++counts[r.category];
    return counts;
}

} // namespace cardbridge::library
