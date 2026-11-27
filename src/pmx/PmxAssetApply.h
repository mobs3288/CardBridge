#pragma once

#include "library/AssetTemplate.h"
#include "pmx/PmxWriter.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

struct AssetFitReport {
    bool compatible = false;
    std::string reason;
    std::vector<std::string> warnings;
    int requiredBones = 0, matchedBones = 0;
    float scale = 1.0f;
    Vec3 translation{};
};

// Reports whether an asset can bind to this PMX's existing skeleton, and the
// conservative translation/scale derived from matched rest bones. Nothing is
// modified. Missing required bones, index-size overflow and a bad fit are
// reported as incompatible, never silently worked around.
AssetFitReport AnalyzeAssetFit(const library::AssetTemplate& asset, const Model& doc);

struct AssetApplyResult {
    bool ok = false;
    std::string error;
    size_t addedVertices = 0, addedIndices = 0;
};

// Append-only application: original vertices, indices, morphs, frames, physics
// and source format stay untouched. All asset vertices must bind to bones that
// already exist in the document.
AssetApplyResult ApplyAsset(Model& doc, const library::AssetTemplate& asset, const AssetFitReport& fit);

} // namespace cardbridge::pmx
