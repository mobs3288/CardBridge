#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace cardbridge::exporter {

struct ExportOptions {
    std::string outDir;            // created; must be empty or absent unless `overwrite`
    std::string baseName;          // file stem; defaults to the character name
    float scale = 12.5f;           // model units per metre (MMD convention)
    bool overwrite = false;
    bool physics = true;           // rigid bodies and joints for hair, skirts and ribbons; false = a rigid PMX with no physics at all
    bool jiggleBust = true;        // breast jiggle physics (the body's own jiggle chains); default on
    bool jiggleButt = true;        // hip / butt jiggle physics; default on (both bounce in MMD)
    float jiggleIntensity = 0.5f;  // how wide the jiggle swings; 0.5 = a gentle default (the game's own is 1)
    bool mmdNames = true;          // the body bones and the blink morph get MikuMikuDance's standard (Japanese) names, so a motion (VMD) finds them
    bool faceMinusZ = true;        // turn the model half a turn about Y so it faces -Z, where MikuMikuDance puts a PMX's front; false = as built (+Z, Unity)
    int outfit = 0;                // recorded in the manifest
    std::string assetLibrary;      // recorded as provenance only
    std::string modsFolder;        // recorded as provenance only
    std::string materialFile;      // --materials provenance ("" = none)
    nlohmann::json materialWorkbench;   // the cardbridge-materials object for the manifest (null = none)
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
MmdLook MmdMaterialLook(const assemble::Scene& scene, const assemble::SceneMaterial& mat, bool outfitGarment = false);
// The material is cut to a shape by its texture the way the game's item shader cuts it (main_item discards where the main texture's
// alpha is below 0.25), sampled where its own triangles lie. `mat` must be one of `scene.materials`.
bool HasCutout(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
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
