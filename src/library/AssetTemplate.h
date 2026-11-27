#pragma once

#include "core/Math.h"
#include "library/AssetCatalog.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::library {

// The geometry of one catalog asset, extracted without a card. Skin weights
// refer to `boneNames` (parallel rest-world positions in `bonePositions`).
struct AssetVertex {
    Vec3 pos, normal;
    Vec2 uv;
    int bone[4] = {-1, -1, -1, -1};
    float weight[4] = {0, 0, 0, 0};
};

struct AssetTemplate {
    std::string key, name, attachNode;
    std::vector<AssetVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> boneNames;
    std::vector<Vec3> bonePositions;
    std::vector<int> boneParents;         // renderer bone hierarchy, -1 for roots
    std::vector<std::string> warnings;
};

struct AssetTemplateOptions {
    std::string libraryRoot;
    std::string modsRoot;
    std::string modCachePath;
};

// Opens the asset's prefab from the base library or owning zipmod and returns
// its skinned/rest-pose geometry. Rigid pieces are bound to their parent node.
// No Card and no running game is involved.
bool LoadAssetTemplate(const AssetRecord& record, const AssetTemplateOptions& options,
                       AssetTemplate& out, std::string& error);

} // namespace cardbridge::library
