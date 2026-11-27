#pragma once

#include "library/AssetTemplate.h"
#include "pmx/BoneMatcher.h"
#include "pmx/PmxWriter.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

enum class FitStatus { Compatible = 0, CompatibleWithWarnings, RequiresManualMapping, Incompatible };
const char* FitStatusName(FitStatus s);

struct AssetFitReport {
    // `compatible` is kept for older callers; `status` carries the full result.
    bool compatible = false;
    FitStatus status = FitStatus::Incompatible;
    std::string reason;
    std::vector<std::string> warnings;
    std::vector<BoneMapping> mappings;
    int requiredBones = 0, matchedBones = 0, ambiguousBones = 0, missingBones = 0;
    float hierarchyCompatibility = 0.0f;
    bool indexCapacityOk = true;
    float scale = 1.0f;
    Vec3 translation{};
    std::string materialCompatibility;
};

// Full compatibility analysis: bone resolution (exact/normalized/alias/
// hierarchy/geometric), confidence and ambiguity reporting, hierarchy
// agreement, index capacity and material notes. Nothing is modified.
AssetFitReport AnalyzeAssetFit(const library::AssetTemplate& asset, const Model& doc,
                               const BoneMatchOptions& options = {});

struct AssetApplyResult {
    bool ok = false;
    std::string error;
    size_t addedVertices = 0, addedIndices = 0;
};

// Append-only application: original vertices, indices, morphs, frames, physics
// and source format stay untouched. Only bones resolved in the report are used;
// ambiguous or unresolved mappings are never guessed at apply time.
AssetApplyResult ApplyAsset(Model& doc, const library::AssetTemplate& asset, const AssetFitReport& fit);

} // namespace cardbridge::pmx
