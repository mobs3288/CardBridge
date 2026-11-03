#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <vector>

namespace cardbridge::exporter {

// A motion (VMD) drives a model by bone and morph NAME, and MikuMikuDance's standard names are Japanese (センター, 上半身, 左腕, 左足ＩＫ,
// まばたき ...). The Koikatsu rig's own names (cf_j_arm00_L ...) match nothing, so a motion finds no bone. This gives the rig's
// body bones the standard names, in place (same indices, same weights), and fixes the hierarchy the standard names imply: 全ての親 is the
// root, センター a bone of its own above both 下半身 and 上半身 (so the two halves turn independently, as a motion expects).
//
// Run after AddIk: the foot IK bones it added become 左足ＩＫ / 右足ＩＫ (aimed at 足首, the ankle) and a つま先ＩＫ is added under each.
// Returns the indices of the bones added (the toe IKs), to be listed with the other IK bones. Bones the rig does not have are skipped.
std::vector<int> ApplyMmdBoneNames(const assemble::Scene& scene, pmx::Model& model);

// Adds the standard facial morphs a motion keys (まばたき ...) as copies of the game's expression morphs they correspond to, and returns
// their indices (they are listed first in the morph panel). Run after the expression morphs exist.
std::vector<int> AddMmdMorphs(pmx::Model& model);

} // namespace cardbridge::exporter
