#pragma once

#include "card/CardFile.h"
#include "library/AssetCatalog.h"

#include <string>
#include <vector>

namespace cardbridge::card {

// One usable item from an outfit/coordinate file, already resolved to a
// catalog record that library::LoadAssetTemplate can consume. No Card and no
// running game is involved.
struct ClothingAssetItem {
    std::string key;              // "clothes.0", "acc.03", ...
    std::string label;
    library::AssetRecord record;
};

struct ClothingAssetOptions {
    std::string libraryRoot;
    std::string modsRoot;
    std::string modCachePath;
};

struct ClothingAssetResult {
    std::vector<ClothingAssetItem> items;
    std::vector<std::string> warnings;
};

// Enumerates a clothing file's clothing/accessory slots and resolves each to
// its base-library or zipmod asset. Empty slots and unresolved items are
// reported in `warnings`, never silently applied.
bool EnumerateClothingAssets(const ClothingFile& clothing, const ClothingAssetOptions& options,
                             ClothingAssetResult& out, std::string& error);

} // namespace cardbridge::card
