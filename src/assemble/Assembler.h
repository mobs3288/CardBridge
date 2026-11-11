#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "resolve/Resolver.h"

#include <functional>
#include <string>

namespace cardbridge::assemble {

// The automatic nipple / areola colour: the reference tone the user picked for the default (#fda797, an HSL(9, 96%, 79%)
// warm pastel salmon-pink). An explicit colour overrides it (see AssembleOptions); w <= 0 keeps the card's own colour.
inline constexpr Vec4 kDefaultNippleColour{0.992f, 0.655f, 0.592f, 1.0f};

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
    // Nipple / areola colour. w > 0: use rgb; w <= 0: keep the card's own nipColor. The default is a natural, muted dusty rose:
    // the card's pale skin tone leaves a flat, nearly invisible areola, and a fully saturated pink reads as an anime cue rather than a body.
    Vec4 nippleColour = kDefaultNippleColour;
    // Automatic skin shading baked from the body mesh, applied to the body skin texture in UV space. cleavage is the strength
    // of the crease between the breasts (0 = off, 1 = default, up to 2); underboob the strength of the fold under the breasts;
    // top is how far up the chest the cleavage reaches, measured against the bust region itself: 0 = nothing, 1 = the full
    // breast line (top of the bust), 2 = a full bust length higher, toward the base of the neck. The three are independent.
    // width scales how wide the cleavage line runs (1 = the ~4 cm default half-width, below that tighter, above that a broad
    // gradient); the fold under the breasts is ray-gated and is not affected.
    float skinShade = 1.5f;
    float skinShadeUnder = 1.0f;
    float skinShadeTop = 1.2f;
    float skinShadeWidth = 0.6f;
    bool autoCloth = false;                // DISABLED by default (revert to last good look): synthesised chains were rebinding hanging cloth
                                           // onto new bones and collapsing it toward the body. Opt-in with --auto-cloth / CARDBRIDGE_AUTO_CLOTH=1;
                                           // CARDBRIDGE_NO_AUTO_CLOTH=1 disables
    // Optional progress sink (the viewer's live log): called from the assembling thread with short stage descriptions.
    std::function<void(const std::string&)> log;
    // Optional determinate progress: (fraction 0..1, stage label). A negative
    // fraction means "indeterminate, just show the stage". Called from the
    // assembling thread, as `log` is.
    std::function<void(double, const std::string&)> progress;
    // Optional cooperative cancellation: when it returns true the assembly
    // stops at the next item boundary and throws cardbridge::Cancelled.
    std::function<bool()> cancelled;
};

// The game draws its garment and eye "create" materials in linear light (sRGB textures decoded, linear colours, encoded result); the skin ones
// were measured against the game as stored. Checked against the game's own render textures: the iris result was 16/255 off drawn as stored, 1 in linear.
bool CreateRunsInLinearLight(const std::string& materialName);

// Where an item prefab's root hangs in the model: the game instantiates the prefab and parents it with
// SetParent(parent, worldPositionStays: false) (ChaControl.ChangeAccessoryParent for accessories, ChangeHairAsync for hair),
// so the root keeps its own local position, rotation and scale under the parent. `parent` is the attach or hair node's world
// matrix; the prefab root's local transform comes from the bundle.
Mat4 PrefabRootPlacement(const Mat4& parent, Vec3 position, Quat rotation, Vec3 scale);

// Bakes the heeled-footwear rest pose (Stiletto: foot pitched about cf_j_foot,
// toes about cf_j_toes, lift onto y=0) for the heeled shoe(s) in the scene.
// Runs at the end of Assemble/Reshape; exported for tests so the flat/heeled
// thresholds can be exercised without game files. CARDBRIDGE_NO_HEEL_POSE=1
// makes it a no-op (before/after comparison).
void SettleHeels(Scene& scene);

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
