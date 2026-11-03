#pragma once

#include "assemble/Scene.h"

#include <string>
#include <vector>

namespace cardbridge::exporter {

// Name guesses for the PMX bone table. The PMX keeps the game's own bone name as the primary name (it is the stable identity: the
// shape sliders, the card's bone modifiers and every mod refer to bones by it); these give an English label, a display group for the
// bone panel of a PMX editor, and a hidden-helper flag. They only read names - an unknown bone gets a plain label and the "Other" group.

// "cf_J_Mayu_L" -> "Left Brow"; a part-owned bone ("outfit0.acc03/joint 4") keeps its part prefix.
std::string BoneLabelEn(const std::string& name);

// The display frame a bone is listed in ("Face: Eyes", "Arms", "Hair", "Accessories", ...).
std::string BoneGroup(const assemble::Scene& scene, size_t bone);

// Driven, shape and attach nodes the user does not pose directly; they are written hidden and not operable.
bool IsHelperBone(const std::string& name);

// The PMX display frames: group name -> bone indices, groups in a fixed order (Root first), every bone in exactly one.
std::vector<std::pair<std::string, std::vector<int>>> BoneFrames(const assemble::Scene& scene);

} // namespace cardbridge::exporter
