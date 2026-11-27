#pragma once

#include "assemble/KkBodyTemplate.h"
#include "pmx/PmxWriter.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

struct RestoreOptions {
    bool allowUniformScale = true;   // fit scale from matched rest bones, only when it is close to 1
    float maxScaleDeviation = 0.25f; // beyond this the fit is rejected as incompatible
};

struct RestoreResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    size_t addedVertices = 0, addedIndices = 0, skippedVertices = 0;
};

// Appends a Koikatsu body template to an imported PMX document. Append-only:
// every original vertex, index, morph offset, frame entry, rigid body and
// joint reference keeps its index, so nothing in the source model is broken.
// The fit is derived from matched rest-bone positions; an unmatched or
// differently oriented skeleton is reported as incompatible instead of being
// silently reshaped.
RestoreResult RestoreBody(Model& doc, const assemble::BodyTemplate& body, const RestoreOptions& options = {});

} // namespace cardbridge::pmx
