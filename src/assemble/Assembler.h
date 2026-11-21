#pragma once

#include "assemble/Scene.h"
#include "assemble/Proportions.h"
#include "card/CardFile.h"
#include "resolve/Resolver.h"
#include "unity/UnityScene.h"

#include <functional>
#include <string>
#include <memory>

namespace cardbridge::assemble {

// The automatic nipple / areola colour: the reference tone the user picked for the default (#fda797, an HSL(9, 96%, 79%)
// warm pastel salmon-pink). An explicit colour overrides it (see AssembleOptions); w <= 0 keeps the card's own colour.
inline constexpr Vec4 kDefaultNippleColour{0.992f, 0.655f, 0.592f, 1.0f};

// Whether a renderer's mesh carries skinning we can use. A SkinnedMeshRenderer's bone list may be longer than
// the mesh's inverse-bind table (KK/KKS garments ship bones the mesh never weighs); that is not a reason to
// drop the skin wholesale. Influences naming a bone past the table are discarded during assembly, the rest
// keep their bind. Only an empty skin or an empty bind table makes a renderer truly rigid. Exposed for tests.
inline bool HasUsableSkin(const unity::RendererInfo& r, const unity::MeshData& mesh) {
    return r.skinned && !r.boneXforms.empty() && !mesh.skin.empty() && !mesh.bindPoses.empty();
}

// An outer top sub-part's OverInnerMask applies to the inner garment of the same top: the sibling sub-part created
// from the card's own subPartsId with a lower list category (e.g. the shirt under a jacket), never an unrelated
// garment and never the cover itself. Exposed for tests so the rule can be checked without a real card/library.
inline bool IsInnerGarmentOf(const ScenePart& cover, const ScenePart& candidate) {
    return !cover.subGroup.empty() && candidate.group == "Outfit" && !candidate.underPiece &&
           candidate.subGroup == cover.subGroup && candidate.category < cover.category && candidate.id != cover.id;
}

// One layer to build on top of the outfit: a garment source (outfit file, or a Coordinate PNG) the assembly
// grafts onto the scene after the outfit itself is built. The source is assembled against a disposable copy of
// the card (ONLY the chosen clothes slots, the rest is discarded), and the parts land in the scene with
// slot ids like "outfit0.top.l2" so the PMX carries both garments of one slot. The card is never told
// two garments occupy one slot: the extra garment exists only in the assembled scene.
struct AssembleLayerSpec {
    std::string path;          // the source file (outfit file or Coordinate PNG)
    int srcOutfit = -1;        // coordinate source: which coordinate of the source (-1 = the source's current one)
    int dstOutfit = -1;        // the outfit slot to layer into (-1 = the shown outfit)
    std::vector<int> slots;    // clothes slot indices (0..8) whose parts get grafted; empty = none
    std::string name;          // label shown by logs/UI
    int layerIndex = 1;        // the suffix for the grafted part ids ("outfit0.top.l1"); must differ between layers of the scene
};

struct AssembleOptions {
    int outfit = -1;                       // -1: the card's current outfit (Status.coordinateType)
    // Source library profile from library.json. Format detection inside each
    // bundle still wins for mixed-engine mods; this is the library's base game.
    library::GameInfo sourceGame;
    std::string bodyPrefab = "p_cf_body_00";
    std::string bodyRigPrefab = "p_cf_body_bone";
    std::string headRigPrefab = "p_cf_head_bone";
    bool applyShape = true;                // apply the card's body sliders to the skeleton
    bool settleHeels = true;               // the foot-lift pass (only the heel pose moves the character, not the bind); false for the disposable layer assemblies whose vertices are grafted away
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
    // Koikatsu's own skin look, baked into the body's skin texture on top of the cleavage shading: a local-curvature
    // (cavity) factor from the base body normal map (form shading from the game's data, orientation-independent, never
    // re-authoring the lighting MMD does) plus the ink lines of the line
    // mask the game binds to _LineMask for the card's body detailId (cf_body_00_line at the default). The bake yields to
    // SkinShade where skinShade darkened (no stacking); 0 = off, 1 = default, up to 2. Both are multiplied into the same
    // texture the PMX exporter references, so every viewer of the exported model sees them.
    float skinAo = 0.85f;
    bool autoCloth = false;                // DISABLED by default (revert to last good look): synthesised chains were rebinding hanging cloth
                                           // onto new bones and collapsing it toward the body. Opt-in with --auto-cloth / CARDBRIDGE_AUTO_CLOTH=1;
                                           // CARDBRIDGE_NO_AUTO_CLOTH=1 disables
    // Optional, non-destructive proportion correction (see Proportions.h). Empty or disabled = faithful pipeline, bit-identical.
    ProportionLayer proportionLayer;
    // Extra garments layered on top of the outfit (see AssembleLayerSpec): assembled against a disposable copy of this card
    // and grafted onto the scene. Empty = the classic outfit.
    std::vector<AssembleLayerSpec> layers;
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

// True for both KK (Shader Forge/main_hair[_front]) and KKS
// (Koikano/hair_main_sun_front, hair_main_low_sun) hair shader families.
// Exposed so the tint rule can be regression-tested without game files.
bool IsHairShaderFamily(const std::string& shaderName);

// Softens the nose-line overlay stroke the same way the assembly does for the "cf_O_noseline" texture (see the .cpp):
// two gaussian taps (a tight one keeps the stroke soft but present, a wide one feathers it out into the surrounding skin),
// RGB untouched. Alpha blending darkens toward the skin hue underneath, whatever the skin tone or the nose texture's
// authored style, so the same pass works on the stock black strokes and on mods' grey arcs alike. Exposed for tests.
void SoftenNoseLineAlpha(std::vector<uint8_t>& rgba, int w, int h);

// Where an item prefab's root hangs in the model: the game instantiates the prefab and parents it with
// SetParent(parent, worldPositionStays: false) (ChaControl.ChangeAccessoryParent for accessories, ChangeHairAsync for hair),
// so the root keeps its own local position, rotation and scale under the parent. `parent` is the attach or hair node's world
// matrix; the prefab root's local transform comes from the bundle.
Mat4 PrefabRootPlacement(const Mat4& parent, Vec3 position, Quat rotation, Vec3 scale);

// Resolves a garment's body/underwear mask texture by name (the list row's OverBodyMask/OverBraMask + *AB fields). The game performs
// a typed lookup (LoadAsset<Texture2D>(name)): a same-named GameObject or prefab does NOT count as the mask, and an asset that decodes
// to something else is not a mask either. A mask is a full-screen body texture in body UV space (red channel 255 = keep the skin).
// `decode` is the caller's asset decoder (nullptr when the caller has none); it must decode the candidate as a texture or return
// nullptr. whyOut names what was tried when nothing decodes, so a missing mask is never silent. Returns nullptr when no mask can be
// produced; callers then keep the covered geometry visible instead of silently hiding nothing.
struct MaskLookup {
    virtual ~MaskLookup() = default;
    virtual unity::PPtr findTexture(const std::string& name) const = 0;
    virtual unity::PPtr findAsset(const std::string& name) const = 0;
};
using MaskDecode = std::function<std::shared_ptr<unity::TextureData>(const unity::PPtr&)>;
std::shared_ptr<unity::TextureData> ResolveBodyMask(const MaskLookup& lookup, const std::string& maskName,
                                                    const MaskDecode& decode, std::string* whyOut);

struct GarmentFixStats {
    size_t verticesMoved = 0;   // total vertices pushed off the body's surface
    size_t partsFixed = 0;      // parts with at least one vertex moved
    size_t skipped = 0;         // vertices that needed more than the displacement cap were left alone
    float maxMove = 0.0f;       // largest push, in the scene's units (the scene is in metres; x1000 = millimetres)
};
// Optional, non-destructive clipping correction for outfit garments that cross into the body. Koikatsu outfit pieces are
// modelled a hair apart from the body; a card whose body sliders move the body a millimetre too far for the garment's
// authored surface buries garment vertices in the skin (the skin shows through the cloth). This pass moves ONLY the
// vertices that pierce the body's surface or lie within `clearance` of it; they are pushed out along the body's own
// surface by (penetration depth + clearance), or by (clearance - distance) for the ones merely touching. Everything else
// - bone weights, UVs, topology, blend shapes, every vertex further than the clearance from the skin - is bit-identical.
// The silhouette is preserved up to the penetration depth itself; it is the minimal correction possible. Exclusions,
// documented: the body itself, the hidden "under pieces" (skin kept for the nude morphs), footwear (a shoe encloses the
// foot by design), and anything in the Head or Hair groups. The pass is OFF by default (the export carries the card's own
// garment vertices); --fix-garment-intersections and the viewer's "Original garment" switch turn it on, and
// CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=0 keeps it off everywhere. Diagnostics: one issue per part that had vertices
// moved (count + largest move), one summary issue, and the counts in the return value for the caller's manifest. A scene
// without a body part is left untouched (stats all zero).
GarmentFixStats FixGarmentIntersections(Scene& scene, float clearance = 0.002f);

struct LayerSeparationStats {
    size_t pairsFound = 0;      // pairs of near-coincident garment parts detected from geometry
    size_t partsFixed = 0;      // outer parts with at least one vertex moved
    size_t verticesMoved = 0;
    size_t skipped = 0;         // vertices whose required move exceeded the safety cap
    float maxMove = 0.0f;       // largest push, in metres
};
// Separates clothing pieces that are the same layer twice - an outer shell and its coloured lining, two copies of a
// cape in two accessory slots, a garment worn both as clothes and through Accessory Clothes. Such pieces are authored
// a hair apart and z-fight when both are drawn. A pair qualifies only when >=85% of sampled vertices of the inner
// copy lie within 2 cm of the outer surface, and both parts come from the same card item with matching topology and
// silhouette. The outer copy is shifted a few millimetres off the inner one as one rigid piece; the inner copy, the
// bone weights, UVs, topology, materials and blend shapes stay exactly as authored. Per-bone locals are re-derived
// for moved meshes, so a later Reshape agrees. Derived from geometry and card metadata only - no slot numbers, item
// names or per-card data. On by default. CARDBRIDGE_NO_LAYER_SEPARATION=1 disables it for before/after comparison.
LayerSeparationStats SeparateCoincidentLayers(Scene& scene);

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
