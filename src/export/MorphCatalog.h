#pragma once

#include "assemble/Scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// One vertex morph of the exported PMX, in scene units, as per-mesh offset lists. The viewer plays these live (a slider
// adds w * offset to the rest vertices; the skin then moves them like any other bind-space offset), which is exactly how
// the game applies blend shapes to the rendered mesh and how MMD applies PMX vertex morphs to the model - so what the
// Morphs tab moves is what the exported file moves, not an approximation of it.
struct LiveMorph {
    std::string name, nameEn;             // the PMX name (the MMD standard ones carry the Japanese name and its English form)
    int panel = 4;                        // MMD's control panel: 1 brows, 2 eyes, 3 mouth, 4 other
    // Per scene mesh, parallel to Scene::meshes: the morph's offsets as (vertex, offset) in scene units at rest.
    // Empty for the meshes the morph does not touch.
    std::vector<std::vector<std::pair<uint32_t, Vec3>>> byMesh;
};

// Builds the morph list from a scene the same way ExportScene builds the PMX's: one vertex morph per scene expression
// (relative to the resting expression built into the vertices), one per raw blend shape, the MMD standard morphs
// (the vowels, まばたき ...) as copies of the game pattern each meaning maps to, and the iris follow the exporter adds
// (the blink squeezes the eye white; the iris takes the white's mean offset so it does not show in front of the lid).
// The outfit / nude toggles are material and group morphs in the PMX - they hide and show meshes, not move vertices -
// so they are not here; the viewer's Parts tab does that job.
// Throws nothing into the caller's face: a scene without a face still yields the raw shapes it has.
std::vector<LiveMorph> BuildLiveMorphs(const assemble::Scene& scene);

} // namespace cardbridge::exporter
