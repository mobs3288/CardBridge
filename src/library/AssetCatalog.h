#pragma once

#include "library/Library.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::library {

// Searchable categories for the character-maker asset catalog. The numbers
// are the game's ChaListDefine.CategoryNo; there is no "dress" category in
// the game (a dress is a top) and outerwear lives in the cpo_* sub-parts.
enum class AssetCategory { Clothing = 0, Hair, Accessories, Body, Textures, Other, Count };
const char* CategoryName(AssetCategory a);
AssetCategory CategoryFromName(const std::string& name);
const char* SubcategoryName(int categoryNo);

struct AssetRecord {
    std::string key;              // game|category|id|guid (stable, deduplicated)
    SourceGame game = SourceGame::Unknown;
    AssetCategory category = AssetCategory::Other;
    int categoryNo = 0;
    int64_t id = 0;
    std::string name;             // the list row's display name
    std::string subcategory;
    bool mod = false;
    std::string modGuid, modName, modVersion, modAuthor, modPath;
    std::string bundle;           // list row MainAB (abdata-relative)
    std::string asset;            // list row MainData
    std::string thumbBundle;      // ThumbAB / ThumbTex, when the list row has them
    std::string thumbAsset;
    std::string parent;           // list row Parent: the bone/node an accessory hangs on
    std::vector<std::string> textureDeps;   // non-empty texture/mask column names
    std::string flags;            // comma-separated authored flags (HideShorts, MabUV, ...)
};

struct AssetCatalog {
    std::vector<AssetRecord> records;
    size_t deduplicated = 0;      // rows dropped as duplicates
    size_t size() const { return records.size(); }
};

struct CatalogOptions {
    std::string libraryRoot;
    std::string modsRoot;
    std::string cachePath;        // on-disk catalog cache (empty = no cache)
    std::string modCachePath;     // the existing mod-index cache
};

// Cheap change key: library list-table files plus the mods-dir fingerprint.
// Equal fingerprints mean the catalog cache can be reused.
std::string CatalogFingerprint(const CatalogOptions& opt);

bool LoadCatalog(const std::string& cachePath, const std::string& fingerprint, AssetCatalog& out);
bool SaveCatalog(const std::string& cachePath, const std::string& fingerprint, const AssetCatalog& cat);

// Builds from base list tables + every compatible installed mod. Never needs
// a card, a running game, or a character to be loaded.
AssetCatalog BuildCatalog(const CatalogOptions& opt, std::vector<std::string>* issues = nullptr,
                          const std::function<void(size_t, size_t)>& progress = {},
                          const std::function<bool()>& cancelled = {});

std::vector<const AssetRecord*> Query(const AssetCatalog& cat, AssetCategory category,
                                      const std::string& search, size_t limit = 0);
std::map<AssetCategory, size_t> CategoryCounts(const AssetCatalog& cat);

// Decodes the list row's ThumbAB/ThumbTex thumbnail, when the installed base
// library or zipmod has it. Returns false (leaving the outputs empty) for rows
// without a thumbnail or when the asset cannot be decoded.
bool LoadAssetThumbnail(const AssetRecord& record, const std::string& libraryRoot,
                        std::vector<uint8_t>& rgba, int& width, int& height);

} // namespace cardbridge::library
