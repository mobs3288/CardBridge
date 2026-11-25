#pragma once

#include "assemble/Scene.h"

#include <string>
#include <vector>

namespace cardbridge::assemble {

// Garment-aware physics is deliberately conservative: only a confident "supportive" verdict changes the exported
// breast physics. Everything else - a shirt, an ordinary under-shirt, pasties, a one-sided accessory, a garment whose
// geometry cannot be inspected - leaves the existing physics exactly as it was.
enum class GarmentSupportVerdict {
    Unknown,       // not enough reliable evidence (the baseline is used)
    Other,         // not an upper-body garment at all
    NonSupportive, // an upper-body garment, but nothing that supports the bust
    Supportive     // a supportive bra, confidently identified
};

const char* GarmentSupportVerdictName(GarmentSupportVerdict v);

// The breast region measured from the character's own rig, not from fixed coordinates: ABMX and body sliders move
// these bones, and the region follows them. Both sides are required; a one-sided or missing rig is not classifiable.
struct BreastRegion {
    Vec3 left{0, 0, 0}, right{0, 0, 0};   // centres of the two breast volumes
    Vec3 chest{0, 0, 0};                  // centre of the chest wall between them
    float radius = 0.0f;                  // radius of each side's volume, in scene units (metres)
    float yMin = 0.0f, yMax = 0.0f;       // vertical band the breast bones occupy
    float waistY = 0.0f;                  // height of the waist, for rejecting torso-length garments
    bool valid = false;
};

// Builds the region from the current bone positions. Returns false and fills `why` when the rig lacks the anchors
// (a synthetic scene, a partial body, a male rig without breast bones): the caller must then keep the baseline.
bool BuildBreastRegion(const Scene& scene, BreastRegion& out, std::string* why = nullptr);

// One candidate garment's evidence. `notes` is a human-readable audit trail; `score` is the weighted evidence in
// [0, 1]; `confidence` is the same number, reported separately so a caller can apply an extra threshold without
// having to know the weights. Nothing here touches the scene.
struct GarmentEvidence {
    std::string part;
    GarmentSupportVerdict verdict = GarmentSupportVerdict::Unknown;
    float score = 0.0f;
    float confidence = 0.0f;
    std::vector<std::string> notes;
};

struct GarmentSupportResult {
    bool supportiveBra = false;   // true only when at least one candidate reached Supportive
    std::vector<GarmentEvidence> candidates;
    std::vector<std::string> diagnostics;   // one line per decision, for the manifest and the log
};

// Classifies every exported upper-body candidate once. Geometry is read in place (no mesh is copied or changed) and
// the result is pure data, so an export can reuse it for every LOD it writes. `disregardSlotMetadata` exists for
// testing the geometry-only path: real callers leave it false.
GarmentSupportResult ClassifyGarmentSupport(const Scene& scene, bool disregardSlotMetadata = false);

} // namespace cardbridge::assemble
