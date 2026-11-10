#pragma once

#include "assemble/Scene.h"

#include <cstdint>
#include <vector>

namespace cardbridge::material {

// UV diagnostics (Stage 3): pure CPU functions over a mesh, no window needed,
// mirroring HasCutout/HasInnerLayer in the exporter. Read-only inspection:
// they report, never modify UVs.

struct UvReport {
    // Missing UV: vertices with a non-finite UV (NaN/inf) — they cannot be sampled.
    int missingUvVerts = 0;
    int totalVerts = 0;
    // Out-of-range: UVs outside [-margin, 1+margin].
    float margin = 0.02f;
    int outOfRangeVerts = 0;
    float worstOutOfRange = 0.0f;     // max distance outside the range
    // Stretch: per-triangle texel density (UV area * texel scale vs 3D area),
    // reported as multiples of this mesh's mean density (1.0 = mean; >1 = the
    // UV covers more pixels per unit surface than average). Near-degenerate
    // 3D triangles are skipped. -1 when there are no triangles.
    float stretchMin = -1.0f;
    float stretchMax = -1.0f;
    float stretchMean = 0.0f;
    int worstStretchTri = -1;         // triangle index (submesh-local ordinal)
    double maxStretchRatio = 0.0;     // max/min density
    // Overlap: islands whose UV bounding boxes overlap (Koikatsu bodies do
    // this deliberately — reported, not an error).
    int islands = 0;
    int overlapPairs = 0;
    // Seams: edges used by exactly one triangle (island boundaries).
    int boundaryEdges = 0;
    int totalEdges = 0;
    int seamTriangles = 0;            // triangles touching a boundary edge
    // Totals for display.
    int triangles = 0;
    bool empty() const { return triangles == 0; }
};

// Per-triangle stretch entry (for heat overlays / worst-offender picks).
struct UvStretchTri {
    int submesh = 0;
    int tri = 0;                      // ordinal within the submesh
    float density = 1.0f;             // texel density (see below)
    float edgeRatio = 1.0f;           // max UV edge len / matching 3D edge len (normalised)
};

// Missing-UV count + list of offending vertex indices (for highlight).
int CountMissingUv(const assemble::SceneMesh& mesh);
std::vector<int> FindMissingUv(const assemble::SceneMesh& mesh);

// Out-of-range: any UV outside [-margin, 1+margin]. worst is the max excursion.
int CountOutOfRange(const assemble::SceneMesh& mesh, float margin, float& worstOutOfRange);

// Stretch stats for all triangles in the mesh (all submeshes).
// density = UV area * (texW * texH) / 3D area, in texels^2 per world^2.
// With texW=texH=1024 (the shader-map cap) densities are directly comparable
// across textures. Also fills `worstTri` (first index in the out vector with
// the max density) and optionally every triangle's density + edge ratio.
float UvStretchAll(const assemble::SceneMesh& mesh, int texW, int texH,
                   float& minDensity, float& maxDensity, float& meanDensity, int& worstTri,
                   std::vector<UvStretchTri>* perTri = nullptr);

// UV islands: connected components over triangle adjacency (vertex-index
// adjacency). Returns one list per island, each entry a (submesh, tri) pair.
// Triangles are identified by (submesh index, ordinal within that submesh).
struct UvTriId {
    int submesh = 0;
    int tri = 0;
};
std::vector<std::vector<UvTriId>> UvIslands(const assemble::SceneMesh& mesh);

// Bounding-box overlap between islands (UV space, inclusive). Returns the
// number of overlapping pairs.
int UvOverlapPairs(const assemble::SceneMesh& mesh);

// Seams: boundary edges (used by exactly one triangle). Fills boundaryEdges,
// totalEdges and seamTriangles; when `seamEdges` is given it lists them as
// (vertexA, vertexB) pairs (undirected, min-first).
int CountSeams(const assemble::SceneMesh& mesh, int& totalEdges, int& seamTriangles,
               std::vector<std::pair<uint32_t, uint32_t>>* seamEdges = nullptr);

// Full report over the whole mesh (all submeshes). texW/texH default to the
// 1024 shader-map cap used by the assembler.
UvReport DiagnoseUv(const assemble::SceneMesh& mesh, int texW = 1024, int texH = 1024, float margin = 0.02f);

// Triangles the UV canvas draws for a material: every triangle of every
// submesh naming `materialIndex`, as (mesh, submesh, tri) — used by the viewer
// to pick which triangles to show for the selected material.
struct UvDrawTri {
    int mesh = -1;
    int submesh = -1;
    int tri = -1;
};
std::vector<UvDrawTri> UvTrisForMaterial(const assemble::Scene& scene, int materialIndex);

} // namespace cardbridge::material
