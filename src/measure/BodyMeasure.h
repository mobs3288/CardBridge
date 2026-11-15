#pragma once

// Real body measurements from the assembled scene, in metres, the scene's own unit.
//
// The maker has no BWH slider and no measurement overlay: the only way to know what a character's bust, waist and hip
// actually are is to measure the skin. This slices the body part's mesh (the nude "body" part, the torso + arms + legs)
// with a plane at each of a set of heights and reports the perimeter of the biggest closed contour at the bust / waist /
// hip heights (the shoulders are covered by the T-pose arms, so arm-bound triangles are excluded from every slice).

#include "assemble/Scene.h"

#include <cstdint>
#include <vector>

namespace cardbridge::measure {

struct Result {
    bool valid = false;         // the scene has a body part and it produced enough contours to compare
    float heightMeters = 0.0f;  // bounding-box height of the body part
    float bust = 0.0f, waist = 0.0f, hip = 0.0f;   // circumferences in metres
    float bustY = 0.0f, waistY = 0.0f, hipY = 0.0f; // the height each was measured at (model space, metres)
    int slices = 0;             // how many slice heights produced a contour
};

// The perimeter (metres) of the largest contour of the torso at height y, or 0 when the slice found none.
// Exposed for the unit tests.
float TorsoPerimeterAt(const assemble::Scene& scene, float y);

// Every contour chain at height y, largest first (a slice of the trunk is one chain; arm and leg tubes make more).
// Exposed for diagnostics and the scratch tooling.
void SliceChainsForDebug(const assemble::Scene& scene, float y, std::vector<float>& chains);

// Measures the character: the contour at the bust (peak above the waist), the waist (the narrowest slice of the
// trunk) and the hip (the peak below it, around the buttocks). Heights are searched inside the body part's bounding
// box, so proportions of very different characters land on the right regions. Invalid (Result::valid == false) when
// there is no body part or the mesh yields too few contours.
Result Measure(const assemble::Scene& scene);

} // namespace cardbridge::measure
