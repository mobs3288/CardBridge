#pragma once

#include "pmx/PmxWriter.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

// An explicit, opt-in bone graft. `bone` carries its own references: indices
// below the original bone count point at existing bones, indices at or above
// it point at other grafted bones (in graft order).
struct GraftBone {
    Bone bone;
    int sourceBone = -1;   // >= 0: move half of this bone's vertex weights to the new bone
    bool remapWeights = false;
};

struct GraftSpec {
    std::vector<GraftBone> bones;
    bool confirmed = false;   // explicit opt-in: ordinary asset apply never grafts
};

struct GraftResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    size_t addedBones = 0, remappedVertices = 0;
};

// Pure validation of capacity, names, references, IK links and hierarchy.
GraftResult AnalyzeBoneGraft(const GraftSpec& spec, const Model& doc);

// Appends bones at the end of the document. Existing bone indices, references
// and document sections are never renumbered or rewritten. Weight remapping
// is the only intentional mutation of existing vertices, and only for bones
// explicitly marked `remapWeights`.
GraftResult AppendBones(Model& doc, const GraftSpec& spec);

} // namespace cardbridge::pmx
