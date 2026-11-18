// LOD coverage: which vertices of which mesh an opaque outer garment shell replaces from outside.
//
// The same ray-cast mechanism the body-suppression fallback uses, applied to every mesh: a vertex is
// "occluded" when a short ray along its outward normal is blocked by an opaque Outfit/Accessory
// triangle (own part excluded — a shell never covers itself). It is occlusion, not proximity and not
// bounding boxes: the dress slit stays open because the ray there exits unobstructed.
//
// The result feeds the LOD pipeline:
//   - triangles whose three vertices are occluded collapse first (a budget, never a removal — LOD 1), and
//   - at LOD 2 and coarser they may be dropped outright instead of carried (whole triangles only —
//     a garment never loses part of a triangle, so no holes can appear).
// Anything uncertain is kept: translucent providers never occlude, cutout (perforated) shells never
// occlude, a ray that exits unobstructed keeps its vertex, and without a reliable coverage map the
// geometry is always retained in full.
#pragma once

#include "assemble/Scene.h"

#include <cstddef>
#include <vector>

namespace cardbridge::mesh {

struct CoverageResult {
    // meshCovered[meshIndex][vertexIndex] == 1 when an opaque outer garment blocks the outward-normal ray.
    // Empty for meshes not classified (parts that are not visibleInPreview are not drawn anyway).
    std::vector<std::vector<char>> meshCovered;
    std::size_t providerTriangles = 0;   // opaque shell triangles considered as occluders
    std::size_t coveredVertices = 0;     // total vertices found occluded across all classified meshes
};

// Computes per-vertex coverage for every visible mesh in the scene. Deterministic: the provider grid is
// built in part-list order, every query is a fixed first-hit ray walk, and no result depends on thread
// timing. Call once on the pristine scene (before any copy is simplified); pass the per-mesh flags to
// SimplifyMesh via its vertexCovered argument. `maxDist` (default 0.4 m) bounds the ray, exactly like
// the body suppression pass.
CoverageResult ComputeSceneCoverage(const assemble::Scene& scene, float maxDist = 0.4f);

} // namespace cardbridge::mesh
