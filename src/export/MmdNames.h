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
// The eyes get their pivot at the eyeball's centre (a sphere fitted to the eye white) and a 両目 bone under the head turns both (the eyes
// follow it by append rotation). `scale` is the model scale the scene's positions were multiplied by. Bones the rig does not have are skipped.
struct MmdBones {
    std::vector<int> ik;     // the toe IKs added, to be listed with the other IK bones
    std::vector<int> extra;  // 両目
};
MmdBones ApplyMmdBoneNames(const assemble::Scene& scene, pmx::Model& model, float scale);

// Adds the standard facial morphs a motion keys and returns their indices (they are listed first in the morph panel): まばたき is a copy of
// the game's closed eyes; あ is the game's wide open mouth, and い う え お are built from it (a smaller opening, the mouth corners spread or
// drawn in, the lips pushed forward) because which of the game's mouth patterns is which vowel is not recorded anywhere. Run after the
// expression morphs exist (they and the model's vertices are what these are made from).
std::vector<int> AddMmdMorphs(pmx::Model& model);

} // namespace cardbridge::exporter
