#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "resolve/Resolver.h"

namespace cardbridge::assemble {

struct AssembleOptions {
    int outfit = -1;                       // -1: the card's current outfit (Status.coordinateType)
    std::string bodyPrefab = "p_cf_body_00";
    std::string bodyRigPrefab = "p_cf_body_bone";
    std::string headRigPrefab = "p_cf_head_bone";
    bool heelPose = false;                 // pose the feet for heeled shoes (opt-in; the game leaves that to plugins)
    bool applyShape = true;                // apply the card's body sliders to the skeleton
    std::string headAttachBone = "cf_s_head";   // where the head rig hangs (ChaReference HeadParent)
};

// Builds the scene for one outfit: the shared skeleton from the base game,
// the body mesh, then every card item that resolves (head, hair, clothing,
// accessories) skinned onto that skeleton by bone name. An item that cannot be
// loaded becomes a part with a failure status and a scene issue - never a
// silent gap.
Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt);

} // namespace cardbridge::assemble
