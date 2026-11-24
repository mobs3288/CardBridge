#pragma once

// Body measurements for the reference-transfer feature. The old BodyMeasure module measures only
// height and bust/waist/hip; this adds the rest of the transferable measurements from the assembled
// scene: head extent, shoulder/neck/arm/leg geometry and limb circumferences. Every value comes from
// the actual skinned mesh or from the assembled skeleton — never from an assumed uniform bone scale.

#include "assemble/Scene.h"
#include "measure/BodyMeasure.h"

#include <array>
#include <string>
#include <vector>

namespace cardbridge::measure {

enum TransferId {
    kTransferHeight = 0,
    kTransferHeadSize,
    kTransferHeadBodyRatio,
    kTransferBust,
    kTransferWaist,
    kTransferHip,
    kTransferShoulderWidth,
    kTransferNeck,
    kTransferArmLength,
    kTransferUpperArm,
    kTransferLowerArm,
    kTransferLegLength,
    kTransferThigh,
    kTransferCalf,
    kTransferCount
};

enum TransferConfidence {
    kTransferMesh = 0,       // measured from the skin surface
    kTransferBone = 1,       // measured from assembled bone geometry
    kTransferDerived = 2     // a ratio of two measured values
};

struct TransferValue {
    float value = 0.0f;                  // centimetres, except HeadBodyRatio which is a factor
    bool valid = false;
    int confidence = kTransferBone;
    std::string source;                  // what was actually measured, for the UI tooltip
};

using TransferSet = std::array<TransferValue, kTransferCount>;

const char* TransferName(int id);
const char* TransferUnit(int id);        // "cm" or "x"
bool TransferIsRatio(int id);

// All transferable measurements from one assembled scene. Invalid entries have value 0.
TransferSet MeasureTransfer(const assemble::Scene& scene);

// Length of a bone's local offset from its parent, and of the world-space segment to its parent.
// The local length is the one ABMX "length" multiplies; the world length is what the user sees.
float BoneLocalLength(const assemble::Scene& scene, const std::string& bone);
float BoneWorldLength(const assemble::Scene& scene, const std::string& bone);

// Named chains (the first name is the chain's parent, whose own offset does not belong to the chain).
std::vector<std::string> ArmChain(bool left);
std::vector<std::string> UpperArmChain(bool left);
std::vector<std::string> LowerArmChain(bool left);
std::vector<std::string> LegChain(bool left);

// Sum of the world segments of a chain, averaged over both sides. 0 when the bones are missing.
float ChainWorldLength(const assemble::Scene& scene, const std::vector<std::string>& chain);

} // namespace cardbridge::measure
