#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

namespace cardbridge::exporter {

// The secondary-motion layer for garments: every outfit DynamicBone chain (skirts, capes, kimono panels, skirt hems — never
// the body's own jiggle, never hair) gets one "sway" bone per chain bone, childed at the same rest position, and every vertex
// that is skinned to a skirt bone is re-weighted to blend in the matching sway bone with kGarmentSwayMix of its share.
// MikuMikuDance physics then acts on the sway bones only: the chain root body is kinematic (welded to the first sway bone,
// like a hair root), and each link joint pins its fold axis on the gravity side at exactly zero with a hold spring sized from
// the card's own elasticity — gravity can never pull the skirt away from the authored pose. The wide flare the card draws is
// the rest pose at every frame; in motion the cloth kicks outward and micro-flutters, never a curtain droop, and nothing can
// open the skirt into the deep-undie-showing fold.
void AddGarmentSway(const assemble::Scene& scene, pmx::Model& model, float scale);

} // namespace cardbridge::exporter
