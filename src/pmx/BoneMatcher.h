#pragma once

#include "core/Math.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

enum class MatchMethod { None = 0, Exact, Normalized, Alias, Hierarchy, Geometric };
const char* MatchMethodName(MatchMethod m);

struct BoneMapping {
    int source = -1;                  // index into the source bone list
    int target = -1;                  // index into the destination bone list
    MatchMethod method = MatchMethod::None;
    float confidence = 0.0f;
    float deviation = 0.0f;           // rest-position distance / destination scale
    bool ambiguous = false;
    std::string note;
    bool ok() const { return target >= 0 && !ambiguous; }
};

struct BoneMatchOptions {
    bool allowGeometric = true;
    float minConfidence = 0.50f;      // mappings below this are not accepted
    float maxGeometricDeviation = 0.35f;   // normalized by destination scale
    float ambiguityMargin = 0.12f;    // two candidates this close are ambiguous
};

// Deterministic, side/qualifier-aware resolution. Every stage records how the
// mapping was found and how confident it is; ambiguous or weak matches are
// reported, never silently accepted.
std::vector<BoneMapping> ResolveBones(const std::vector<std::string>& sourceNames,
                                      const std::vector<Vec3>& sourcePositions,
                                      const std::vector<int>& sourceParents,
                                      const std::vector<std::string>& targetNames,
                                      const std::vector<Vec3>& targetPositions,
                                      const std::vector<int>& targetParents,
                                      const BoneMatchOptions& options = {});

} // namespace cardbridge::pmx
