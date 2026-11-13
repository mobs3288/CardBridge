#pragma once

#include "assemble/Scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// One vertex morph of the exported PMX as a *recipe* over the scene's blend shapes, in scene units. The viewer plays
// morphs live by accumulating (weight * recipe weight * shape offset) into the rest vertices before skinning - exactly
// how the game applies blend shapes to the rendered mesh and how MMD applies PMX vertex morphs - so what the Morphs tab
// moves is what the exported file moves, not an approximation of it.
//
// The recipe is re-evaluated against the *current* scene every frame (a re-shape moves the shape offsets with the bones),
// which is what keeps the live preview correct while a body or face slider is being dragged; building the recipes (names,
// panels, which pattern of the game's FaceBlendShape the morph plays) happens once, at load.
struct LiveMorph {
    std::string name, nameEn;             // the PMX name (the MMD standard ones carry the Japanese name and its English form)
    int panel = 4;                        // MMD's control panel: 1 brows, 2 eyes, 3 mouth, 4 other
    // (mesh, shape, weight) - weight already includes (target weight - rest)*0.01 for the expressions (and the raw shape's
    // (100 - rest)*0.01 for the raw shapes), and the gain of the MMD standard ones that are copies of another morph.
    struct Recipe { int mesh = -1, shape = -1; float weight = 0.0f; };
    std::vector<Recipe> recipe;
    // The one-eyed versions of the closed eyes (ウィンク / ｳｨﾝｸ２右): only the vertices on one side of the face's middle are moved.
    int winkSide = -1;                 // -1: all vertices; 0: the left eye (the closed eye kept on the rig's left side), 1: the right
    float winkMid = 0.0f, winkSign = 1.0f;   // where the middle is, and which side is the rig's left (the sign of the left eye bone's x)
};

// The eye sets the iris follow needs: when a morph squeezes the eye white (the lid), the iris takes the mean offset of the
// white's, one rigid offset per shape - the same compensation the exporter adds to every morph in the PMX (the game moves
// the white with the lid, the iris would otherwise stay in front of it).
struct LiveMorphEyes {
    int whiteMesh = -1, irisMesh = -1;      // scene mesh indices of the white and the iris of this eye (-1: side not present)
    std::vector<uint32_t> whiteVerts, irisVerts;   // mesh-local vertex indices
};

// The catalog: the morph list and the two eye sets the iris follow is made from. Same morphs the PMX writes, in the
// same order (expressions, then the raw per-mesh shapes, then the MMD standard ones, then the one-eyed versions) - the
// outfit and nude toggles are material morphs in the PMX, so they are not here; the Parts tab hides and shows meshes.
struct LiveMorphCatalog {
    std::vector<LiveMorph> morphs;
    LiveMorphEyes eyes[2];                 // 0 = left (+x in the as-built scene), 1 = right
};

// Builds the catalog from a scene's structure (expressions, shapes and names), like ExportScene picks the PMX's morphs.
// Recipes reference the scene's blend shapes; vertex positions and offsets are NOT copied, so the caller accumulates with
// AccumulateLiveMorph against whatever the scene currently holds.
LiveMorphCatalog BuildLiveMorphs(const assemble::Scene& scene);

// Accumulates one catalog morph at weight `weight` (0..1) into per-mesh dense offsets (scene units), adding to whatever
// `dst` already holds (mesh-parallel, scene-vertex-parallel; empty vectors are sized). The iris follow runs at the end
// from the accumulated white offsets, so the iris slides with the lid even mid-drag.
void AccumulateLiveMorph(const assemble::Scene& scene, const LiveMorphCatalog& catalog, const LiveMorph& morph, float weight,
                         std::vector<std::vector<Vec3>>& dst);

} // namespace cardbridge::exporter
