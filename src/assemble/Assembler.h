#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "resolve/Resolver.h"

#include <functional>

namespace cardbridge::assemble {

struct AssembleOptions {
    int outfit = -1;                       // -1: the card's current outfit (Status.coordinateType)
    std::string bodyPrefab = "p_cf_body_00";
    std::string bodyRigPrefab = "p_cf_body_bone";
    std::string headRigPrefab = "p_cf_head_bone";
    bool applyShape = true;                // apply the card's body sliders to the skeleton
    std::string headAttachBone = "cf_s_head";   // where the head rig hangs (ChaReference HeadParent)
    // Tweaks from the viewer. A slider vector replaces the card's own values index by index (shorter = the rest stay the card's);
    // empty = the card's. Body has 44 sliders, face 52 (shape::BodySliders / FaceSliders say what each one is).
    std::vector<float> bodyShape, faceShape;
    // Nipples: nippleOffset moves the nipple bones along the chest normal in metres (negative = into the body, positive = out).
    // nippleInside pushes them in so they do not show through clothes: the nipple stand rate and the nipple depth slider (13)
    // drop to 0 and the offset is at most nippleInsideOffset.
    float nippleOffset = 0.0f;
    bool nippleInside = false;
    float nippleInsideOffset = -0.008f;
    float areolaSize = -1.0f;              // the areola texture size (shader _nipsize); negative = the card's
    // Optional progress sink (the viewer's live log): called from the assembling thread with short stage descriptions.
    std::function<void(const std::string&)> log;
};

// Builds the scene for one outfit: the shared skeleton from the base game,
// the body mesh, then every card item that resolves (head, hair, clothing,
// accessories) skinned onto that skeleton by bone name. An item that cannot be
// loaded becomes a part with a failure status and a scene issue - never a
// silent gap.
Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt);

// Re-shapes an assembled scene in place when only the shape changed: `opt` carries the new body and face sliders, the nipple settings and
// the outfit the scene was assembled with. The skeleton is rebuilt (the game's formulas, KKABMX, the head on the new neck) and every vertex,
// normal, bone position and collider follows through the per-bone frames the assembly stored. A few milliseconds, no bundle is read again
// but the small shape tables. Not redone: hair and accessory chains that were settled by a DynamicBone (off by default), blend shape offsets,
// and the areola texture size, which is a texture, not a bone. Returns false when the scene cannot be re-shaped (no skeleton in the library).
bool Reshape(Scene& scene, const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt);

} // namespace cardbridge::assemble
