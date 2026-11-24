#pragma once

#include "assemble/Scene.h"
#include "assemble/JiggleSettings.h"
#include "assemble/Proportions.h"
#include "card/CardFile.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// Lining verdicts from ColocatedLinings: 0 none, 1 outward-facing twin (reverse winding at export), 2 inward-facing twin (keep
// winding), 3 ambiguous (preserve original exactly). Callers treat 1 and 2 as the same interior-lining treatment (single-sided,
// culled from outside); 3 is not treated as a lining at all.
constexpr char kLiningNone = 0;
constexpr char kLiningOutward = 1;
constexpr char kLiningInward = 2;
constexpr char kLiningAmbiguous = 3;
// The two interior verdicts: a twin wound the opposite way to its opaque sibling (the interior lining as
// authored) or wound the same way (the cover, flipped at export). Both face into the garment.
constexpr bool IsInteriorLining(char code) { return code == kLiningOutward || code == kLiningInward; }


struct ExportOptions {
    std::string outDir;            // created; must be empty or absent unless `overwrite`
    std::string baseName;          // file stem; defaults to the character name
    float scale = 12.5f;           // model units per metre (MMD convention)
    bool overwrite = false;
    // Optional post-export upscaling of colour textures through waifu2x-ncnn-vulkan (open source, Windows).
    // Off by default; an unresolvable tool keeps the normal export unchanged.
    bool upscaleTextures = false;
    int upscaleScale = 2;
    std::string upscalerExe;                       // explicit exe path; auto-detected when empty
    std::string upscalerModel = "waifu2x";         // "waifu2x" or "realesrgan" (x4plus-anime)
    bool physics = true;           // rigid bodies and joints for hair, skirts and ribbons; false = a rigid PMX with no physics at all
    bool jiggleBust = true;        // breast jiggle physics (the body's own jiggle chains); default on
    bool jiggleButt = false;       // hip / butt jiggle physics; default off - the card's butt sway reads as the back of the skirt vibrating. [--jiggle-butt] enables it
    assemble::JiggleSettings jiggle = {};   // the physical breast / hip jiggle settings, shared with the live preview (see assemble/JiggleSettings.h)
    bool mmdNames = true;          // the body bones and the blink morph get MikuMikuDance's standard (Japanese) names, so a motion (VMD) finds them
    bool faceMinusZ = true;        // turn the model half a turn about Y so it faces -Z, where MikuMikuDance puts a PMX's front; false = as built (+Z, Unity)
    int outfit = 0;                // recorded in the manifest
    std::string assetLibrary;      // recorded as provenance only
    std::string modsFolder;        // recorded as provenance only
    std::string materialFile;      // --materials provenance ("" = none)
    nlohmann::json materialWorkbench;   // the cardbridge-materials object for the manifest (null = none)
    // Triangle reduction for the written PMX (0 = the original geometry, the default; 80 = aggressive). The reduction is applied
    // per part class (face / eyes / hair / body / clothes / accessories), always from this scene's own vertices - the exported
    // PMX never comes from an already simplified intermediate. 0 disables the pass entirely (the default).
    float simplifyPercent = 0.0f;
    // One level per file to write beside the main PMX. `remaining` is the fraction of the original triangles to keep (0.6 = 60%).
    // Each LOD is generated from this scene's own (untouched) vertices too, never from another level. Empty = none.
    // The files share the main export's textures (identical bytes) and are listed in the manifest as "lods".
    struct LodTarget {
        std::string label;         // "LOD 1" .. "LOD 4": names the file (<stem>_lod1.pmx) and shows in the manifest
        float remaining = 1.0f;
    };
    std::vector<LodTarget> lodTargets;
    // Optional, non-destructive proportion correction layer active for this export (the viewer passes the same layer the
    // previewed scene was re-shaped with). Written into the manifest as "proportions": {"active": bool, "ops": [...]}.
    assemble::ProportionLayer correctionLayer;
    // Optional measured body-transfer spec (JSON from assemble::transfer::ToJson) actually applied to the
    // scene before export. Written verbatim as "bodyTransfer" so an export can be audited against its source.
    std::string bodyTransferJson;
    // Optional progress sink (the viewer's live log): called with short stage descriptions.
    std::function<void(const std::string&)> log;
    // Optional determinate progress: (fraction 0..1, stage label). A negative
    // fraction means "indeterminate, just show the stage".
    std::function<void(double, const std::string&)> progress;
    // Optional cooperative cancellation: true stops the export at the next
    // part / preview boundary with cardbridge::Cancelled.
    std::function<bool()> cancelled;
};

struct ExportResult {
    std::string folder, pmxPath, manifestPath, previewPath;
    size_t vertices = 0, triangles = 0, materials = 0, bones = 0, textures = 0, parts = 0;
    // One entry per --lods level, in the order given: the file name (inside `folder`), the label and how much of the original it keeps.
    struct Lod {
        std::string file, label;
        float remaining = 1.0f;
        size_t triangles = 0, vertices = 0;
    };
    std::vector<Lod> lods;
};

// Writes <outDir>/<name>.pmx, <outDir>/textures/*.png and
// <outDir>/<name>.cardbridge.json - one self-contained folder with relative
// paths only, the contract the engine importer reads (plan section 16.1).
// Parts with includeInExport == false are left out of the PMX but stay listed
// in the manifest. Callers first run ApplyPartStateForExport with the viewer's
// checkboxes: an unticked item becomes includeInExport == false, so it (and
// the bones only it used) leaves no trace in the PMX, and the preview PNGs
// are the viewer's picture at export time. Throws FormatError on IO or
// consistency errors.
ExportResult ExportScene(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt);

// Structural checks on an assembled scene, each finding one line: indices outside their arrays, non-finite positions, vertex
// weights that do not sum to 1, bones, materials or textures that do not exist, and parts whose geometry lies far outside the
// body (a transform applied twice or not at all). Empty = nothing found. Tolerances: weights 1e-3, a part may not reach more
// than 25 % above the head's top or 15 % of the body height below the floor.
std::vector<std::string> ValidateScene(const assemble::Scene& scene);

// A material as MikuMikuDance shows it: the colours, draw flags and edge the exporter writes into its PMX material (no texture,
// toon or sphere indices). The viewer's "as exported" view draws with the same numbers, so the two cannot drift apart.
struct MmdLook {
    Vec4 diffuse{1, 1, 1, 1};
    Vec3 ambient{1, 1, 1}, specular{0, 0, 0};
    float specularPower = 5.0f;
    uint8_t drawFlags = 0;   // 0x01 double-sided, 0x02 ground shadow, 0x04 casts self-shadow, 0x08 receives self-shadow, 0x10 edge
    Vec4 edgeColor{0, 0, 0, 1};
    float edgeSize = 0.0f;
    int sphere = 0;              // 0 none, 1 the teeth's roundness (multiply), 2 a soft gloss (add), 3 the hair's ring highlight (add)
    float sphereStrength = 0.0f; // the add spheres' peak
};
// `outfitGarment` marks a material of an outfit garment (ScenePart::garment): it keeps the ground shadow and casts into the self-shadow
// map, but takes no self-shadow (inverted cloth bands where it clips) and gets no edge (back faces pushed through it).
// `innerLayer` is a lining verdict from ColocatedLinings (kLiningNone..kLiningAmbiguous): kLiningOutward/kLiningInward are
// written single-sided; an ambiguous verdict keeps the authored draw flags untouched.
MmdLook MmdMaterialLook(const assemble::Scene& scene, const assemble::SceneMaterial& mat, bool outfitGarment = false, char innerLayer = kLiningNone, bool accessory = false);
// A material the game draws SOLID even though its shader is an alpha-test family: its texture alpha is a
// baked tint (no texel the mesh covers is discarded), not translucency. Shared by the PMX writer and the
// viewer so both read the same material semantics instead of re-deriving them from the texture.
bool IsAlphaTestSolid(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
// Make the scene's own texture agree with that decision: copy the affected texture with alpha solidified
// (the authored RGB, sheen and UVs are untouched) and repoint the material's main texture at it. The PMX
// writer already does this at export time, so the interactive viewer then sees the same pixels it exports.
// Idempotent: a texture that is already opaque is left alone.
void SolidifyAlphaTestSolidTextures(assemble::Scene& scene);
// One flag per scene material: it belongs to an outfit garment (ScenePart::garment, or group Outfit in a hand-built scene). The exporter
// and the viewer's as-exported pass both use this, so the written PMX flags and the preview cannot drift apart.
std::vector<char> MmdGarmentMaterials(const assemble::Scene& scene);
// One flag per scene material: it is the translucent, co-located twin of an opaque material in the same mesh (the
// base layer and its "studio" lining share nearly all of their triangles). Such a layer is authored as the
// garment's interior; its winding relative to the opaque sibling decides the verdict: wound the same way it is the
// cover shell (reversed at export so it faces into the garment), wound the opposite way it is already the interior
// lining (kept as authored), mixed - kLiningAmbiguous - never guessed, drawn exactly as authored. Interior twins
// are single-sided and culled from outside.
std::vector<char> ColocatedLinings(const assemble::Scene& scene);
// The material is cut to a shape by its texture the way the game's item shader cuts it (main_item discards where the main texture's
// alpha is below 0.25), sampled where its own triangles lie. `mat` must be one of `scene.materials`.
bool HasCutout(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
// True when a meaningful share of the texels the material's own triangles cover are alpha-discarded.
// Empty baked-atlas padding or one stray texel is not enough to classify an otherwise opaque material
// as a cutout. `mat` must be one of `scene.materials`.
bool HasAlphaCutout(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
// The material's mesh is modelled as two co-located layers (an outer and an inner surface sharing one vertex grid), the triangle winding
// of a large share of them running opposite to their vertex normals. `mat` must be one of `scene.materials`.
bool HasInnerLayer(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
// The sphere map image of a look (64 x 64, its name says kind and strength), the same for the PMX file and the viewer.
assemble::SceneTexture MmdSphereImage(const MmdLook& look);

// The manifest as JSON (exposed for tests).
nlohmann::json BuildManifest(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt,
                             const std::string& pmxFile, const std::vector<std::string>& textureFiles,
                             const std::vector<std::pair<int, int>>& partMaterialRange,
                             const std::vector<std::string>& morphNames = {});

} // namespace cardbridge::exporter
