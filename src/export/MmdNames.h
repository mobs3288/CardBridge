#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <map>
#include <string>
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

// Adds the semi-standard bones the hand-made MMD models have and many motions key, around the named ones: 肩P / 肩C, the twist bones 腕捩 and
// 手捩 (the game's driven skin helpers along the arm and forearm take their twist in proportion), 腰キャンセル, 足IK親 and 操作中心. Bones are
// inserted, not appended (MikuMikuDance evaluates a bone after the ones before it), so every bone index moves: run it last, after the display
// frames exist, and before nothing that still holds bone indices.
void AddMmdStandardBones(pmx::Model& model);

// Adds the standard facial morphs a motion keys and returns their indices (they are listed first in the morph panel): the vowels あ い う え お
// ん, ワ, ∧, ω, ▲, にやり ..., the eyes まばたき, 笑い, ウィンク ..., the brows 真面目, 困る, 怒り .... Each is a copy of the game expression morph
// that plays the pattern of that meaning, found by the pattern's own blend shape name (`shapes`: expression morph name -> SceneExpression::shape,
// from AddExpressionMorphs), so lips, teeth, tongue and nose move together as in the game. A head without those names gets the vowels and
// まばたき from its plain open mouth and closed eyes ("Mouth 0 open", "Eyes 0 closed"). Run after the expression morphs exist.
std::vector<int> AddMmdMorphs(pmx::Model& model, const std::map<std::string, std::string>& shapes = {});

} // namespace cardbridge::exporter
