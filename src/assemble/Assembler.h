#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "resolve/Resolver.h"

#include <functional>
#include <string>

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
    float areolaSize = -1.0f;              // the areola size (a property of the body's material: shader _nipsize); negative = the card's
    // Optional progress sink (the viewer's live log): called from the assembling thread with short stage descriptions.
    std::function<void(const std::string&)> log;
};

// The game draws its garment and eye "create" materials in linear light (sRGB textures decoded, linear colours, encoded result); the skin ones
// were measured against the game as stored. Checked against the game's own render textures: the iris result was 16/255 off drawn as stored, 1 in linear.
bool CreateRunsInLinearLight(const std::string& materialName);

// Where an item prefab's root hangs in the model: the game instantiates the prefab and parents it with
// SetParent(parent, worldPositionStays: false) (ChaControl.ChangeAccessoryParent for accessories, ChangeHairAsync for hair),
// so the root keeps its own local position, rotation and scale under the parent. `parent` is the attach or hair node's world
// matrix; the prefab root's local transform comes from the bundle.
Mat4 PrefabRootPlacement(const Mat4& parent, Vec3 position, Quat rotation, Vec3 scale);

// Builds the scene for one outfit: the shared skeleton from the base game,
// the body mesh, then every card item that resolves (head, hair, clothing,
// accessories) skinned onto that skeleton by bone name. An item that cannot be
// loaded becomes a part with a failure status and a scene issue - never a
// silent gap.
Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt);

// Re-shapes an assembled scene in place when only the shape changed: `opt` carries the new body and face sliders, the nipple settings and
// the outfit the scene was assembled with. The skeleton is rebuilt (the game's formulas, KKABMX, the head on the new neck) and every vertex,
// normal, bone position and collider follows through the per-bone frames the assembly stored. A few milliseconds, no bundle is read again
// but the small shape tables. Also follow the new slider values: blend shape offsets and the areola size (a property of the body material).
// Returns false when the scene cannot be re-shaped: no skeleton in the library, or an item's DynamicBone chains were settled for the old shape
// (Scene::chainsSettled, CARDBRIDGE_DYNBONE: off by default) - assemble again then.
bool Reshape(Scene& scene, const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt);

} // namespace cardbridge::assemble
