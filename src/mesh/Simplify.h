#pragma once

// Mesh simplification (quadric error metrics, edge collapse) with CardBridge's
// invariants: open borders, UV / normal / material seams stay intact, bone
// weights blend (top 4 influences, renormalised), blend-shape offsets travel
// with the vertices they belonged to and the result stays reshapable (the
// `local` bind frames of a moved vertex are rebuilt from the new position).
//
// The simplifier only ever sees a *copy* of the scene's meshes; LOD 0 stays
// the untouched original, LOD 1..N are always generated from that original,
// never from the level before.

#include "assemble/Scene.h"
#include "mesh/Occlusion.h"
#include "core/Math.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::mesh {

// ---- one-mesh decimation --------------------------------------------------

struct SimplifyOptions {
    float maxReduction = 0.0f;       // 0..0.95, the fraction of triangles to remove
    bool lockBorders = true;          // keep open borders (uv / normal / weight / material seams, silhouette edges) in place
    float uvWeight = 8.0f;            // attribute quadric weight on the primary UV channel (per component)
    float secondaryUVWeight = 2.0f;   // the TEXCOORD1..3 channels (the body's / face's placement maps)
    float flipDot = 0.1f;             // a candidate is rejected when an adjacent face's normal turns more than acos(flipDot) vs its CURRENT orientation
    float maxFaceTurnDeg = 40.0f;     // the cumulative ceiling per face: no surviving face may rotate more than this from its orientation when the mesh entered (bounds fold-through, invisible faces included); <= 0 disables the guard
    float coveredQuadricScale = 0.25f;   // collapse-budget multiplier for faces whose 3 vertices are occluded by another opaque layer (1.0 = no budget relief)
    bool allowOccludedCull = false;        // drop faces all of whose vertices are occluded (used only for Clothing meshes at LOD 2+, where reduction >= 50%)
    std::size_t hardCapTriangles = 0; // 0 = no cap beyond maxReduction
    std::function<bool()> cancelled;            // polled every few collapses; when it returns true the mesh is left unusable (discard the copy)
    std::function<void(float, const char*)> progress;    // (0..1, stage) - called from the worker thread
};

struct SimplifyMeshStats {
    std::size_t originalVertices = 0, resultVertices = 0;
    std::size_t originalTriangles = 0, resultTriangles = 0;
    std::size_t collapses = 0;              // edges actually collapsed
    std::size_t occludedDropped = 0;        // faces dropped before any collapse because all three vertices are occluded by an opaque outer layer (LOD 2+)
    std::size_t skippedLocked = 0;          // pops rejected (borders, material seams, link condition)
    std::size_t skippedFlip = 0;            // pops rejected (the collapse would fold a face)
    std::size_t skippedDegenerate = 0;      // faces dropped for near-zero area at the end
    std::size_t skippedFoldTurn = 0;        // pops rejected (the candidate would rotate a face more than maxFaceTurnDeg from its original orientation)
    // Exact fold-through measurement: for every surviving face, the angle between its original
    // orientation (when the mesh entered the simplifier) and its final orientation, cumulative
    // across every collapse that moved its vertices. Zero for a mesh that did not move.
    std::size_t foldOver60 = 0;             // surviving faces rotated more than 60 deg from their initial orientation
    std::size_t foldOver84 = 0;             // ... more than 84 deg (past the per-collapse flip budget)
    std::size_t foldOver90 = 0;             // ... more than 90 deg: inverted, culled from outside on single-sided materials
    float foldMaxDeg = 0.0f;                // largest such rotation, in degrees
    double seconds = 0.0;
    bool cancelled = false;
    std::vector<std::string> warnings;
};

// Simplifies `mesh` in place. Open borders and material seams are preserved, every submesh keeps its
// material (an empty one is kept with no faces), surviving vertices carry blended weights and rebuilt
// bind frames, normals are recomputed (area-weighted, seam-aware) and the blend shapes are carried over.
// `bones` (the scene's skeleton) is required when mesh.reshapable, so the bind frames stay consistent;
// without it the mesh's `reshapable` flag is cleared instead of leaving stale frames behind.
// `vertexCovered` (optional, parallel to the mesh's vertices): occlusion flags from ComputeSceneCoverage.
// When set, faces all of whose vertices are occluded cost opt.coveredQuadricScale in budget; when
// opt.allowOccludedCull is also set they are dropped entirely before the collapse starts (LOD 2+).
SimplifyMeshStats SimplifyMesh(assemble::SceneMesh& mesh, const SimplifyOptions& opt,
                               const std::vector<assemble::SceneBone>* bones = nullptr,
                               const std::vector<char>* vertexCovered = nullptr);

// ---- scene-level: what simplification is for what part ---------------------

// The visual class of a mesh; drives how much of the requested reduction the mesh may take. Order:
// more conservative first. Eyes are never simplified; the face only ever loses a sliver; hair is
// removed aggressively (it is mostly planes with open borders that stay locked anyway); the body
// keeps proportions and deformation quality; clothing and accessories simplify the most.
enum class MeshClass { Eye, Face, Hair, Body, Clothing, Accessory, Other };

struct MeshClassProfile {
    const char* label;    // "Eyes", "Face", ...
    float weight;         // multiplier on the requested reduction
    float cap;            // hard cap on this class's reduction, whatever the preset says
};
const MeshClassProfile& ProfileForClass(MeshClass cls);
// The class of scene.meshes[meshIndex]; reads the owning part's group and the mesh's own and its
// materials' names.
MeshClass ClassifyMesh(const assemble::Scene& scene, std::size_t meshIndex);
// The reduction this mesh gets for a scene-wide `reduction` (before its own locks are considered).
float ClassReduction(MeshClass cls, float reduction);

// ---- presets and LOD --------------------------------------------------------

struct SimplifyPreset {
    const char* name;
    float reduction;   // fraction of triangles to remove
};
// Original, High Quality, Balanced, Performance, Aggressive - in that order.
extern const SimplifyPreset kSimplifyPresets[5];
constexpr int kPresetCustom = 5;   // the preset index of a user-typed slider value

struct LodLevelSpec {
    std::string id;        // "lod1".."lod4"
    std::string label;     // "LOD 1"
    float remaining = 1.0f;   // fraction of the triangles to keep
};
// The stock levels: 60 / 30 / 15 / 5 percent remaining.
std::vector<LodLevelSpec> DefaultLodLevels();

struct MeshFoldStats {
    std::size_t foldOver60 = 0;       // surviving faces rotated more than 60 deg from their original orientation
    std::size_t foldOver84 = 0;       // ... more than 84 deg
    std::size_t foldOver90 = 0;       // ... more than 90 deg (inverted; single-sided culling erases them)
    float foldMaxDeg = 0.0f;          // largest rotation of any surviving face
    std::size_t skippedFoldTurn = 0;  // collapses rejected because a face would cross maxFaceTurnDeg
    std::size_t occludedDropped = 0;  // faces removed outright because all three vertices are occluded (LOD 2+)
};

struct SceneSimplifyStats {
    std::size_t originalTriangles = 0, resultTriangles = 0;
    std::size_t originalVertices = 0, resultVertices = 0;
    double seconds = 0.0;
    bool cancelled = false;
    std::vector<std::string> warnings;
    std::vector<std::pair<int, std::size_t>> meshTriangles;   // (mesh index, triangles after) - only for meshes that were allowed to reduce
    std::vector<std::pair<int, MeshFoldStats>> meshFolds;     // (mesh index, fold/occlusion stats) - only for meshes that were allowed to reduce
    // Occlusion bookkeeping: what the coverage pass saw and what it dropped (LOD 2+ budget weighting + culling).
    // Zero when the requested reduction is 0 or the scene has no opaque outfit/accessory layer.
    std::size_t providerTriangles = 0;    // opaque shell triangles considered as occluders
    std::size_t coveredVertices = 0;      // vertices found occluded by another opaque layer across all meshes
    std::size_t occludedFacesDropped = 0; // faces removed outright at LOD 2+ (all three vertices occluded)
};

// A deep copy of `pristine` with every mesh simplified by `reduction` (0..0.95), per
// ClassifyMesh's caps. Always reads from `pristine`; the copy is returned. Null when
// cancelled (or when nothing could be done, e.g. an empty scene: the copy is returned then).
// `parallel` runs the independent meshes over hardware_concurrency threads.
// `precomputedCoverage`: the occlusion map for `pristine` (from ComputeSceneCoverage). When null and reduction > 0,
// the coverage pass is run on `pristine` first — pass a shared result in to avoid recomputing it for every LOD level.
std::shared_ptr<assemble::Scene> SimplifyScene(const assemble::Scene& pristine, float reduction,
                                               const SimplifyOptions& base, SceneSimplifyStats& stats,
                                               const std::function<void(float, const std::string&)>& progress = {},
                                               const CoverageResult* precomputedCoverage = nullptr);

// Total triangle count of a scene.
std::size_t SceneTriangleCount(const assemble::Scene& scene);
// Total vertex count of a scene.
std::size_t SceneVertexCount(const assemble::Scene& scene);

struct LodResult {
    LodLevelSpec spec;
    std::size_t triangles = 0;
    std::size_t vertices = 0;
    double seconds = 0.0;
    bool cancelled = false;
    std::vector<std::string> warnings;
    std::shared_ptr<assemble::Scene> scene;   // null when cancelled
};

// LOD 1..N, each generated from `pristine` alone (never from another level). LOD 0 is the
// untouched original and is not produced here. A level that fails (cancel / mesh errors) is
// skipped with its warnings; the rest are still produced.
std::vector<LodResult> GenerateLods(const assemble::Scene& pristine, const std::vector<LodLevelSpec>& levels,
                                    const SimplifyOptions& base,
                                    const std::function<void(float, const std::string&)>& progress = {});

} // namespace cardbridge::mesh
