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
    float flipDot = 0.1f;             // a candidate is rejected when an adjacent face's normal turns more than acos(flipDot)
    std::size_t hardCapTriangles = 0; // 0 = no cap beyond maxReduction
    std::function<bool()> cancelled;            // polled every few collapses; when it returns true the mesh is left unusable (discard the copy)
    std::function<void(float, const char*)> progress;    // (0..1, stage) - called from the worker thread
};

struct SimplifyMeshStats {
    std::size_t originalVertices = 0, resultVertices = 0;
    std::size_t originalTriangles = 0, resultTriangles = 0;
    std::size_t collapses = 0;              // edges actually collapsed
    std::size_t skippedLocked = 0;          // pops rejected (borders, material seams, link condition)
    std::size_t skippedFlip = 0;            // pops rejected (the collapse would fold a face)
    std::size_t skippedDegenerate = 0;      // faces dropped for near-zero area at the end
    double seconds = 0.0;
    bool cancelled = false;
    std::vector<std::string> warnings;
};

// Simplifies `mesh` in place. Open borders and material seams are preserved, every submesh keeps its
// material (an empty one is kept with no faces), surviving vertices carry blended weights and rebuilt
// bind frames, normals are recomputed (area-weighted, seam-aware) and the blend shapes are carried over.
// `bones` (the scene's skeleton) is required when mesh.reshapable, so the bind frames stay consistent;
// without it the mesh's `reshapable` flag is cleared instead of leaving stale frames behind.
SimplifyMeshStats SimplifyMesh(assemble::SceneMesh& mesh, const SimplifyOptions& opt,
                               const std::vector<assemble::SceneBone>* bones = nullptr);

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

struct SceneSimplifyStats {
    std::size_t originalTriangles = 0, resultTriangles = 0;
    std::size_t originalVertices = 0, resultVertices = 0;
    double seconds = 0.0;
    bool cancelled = false;
    std::vector<std::string> warnings;
    std::vector<std::pair<int, std::size_t>> meshTriangles;   // (mesh index, triangles after) - only for meshes that were allowed to reduce
};

// A deep copy of `pristine` with every mesh simplified by `reduction` (0..0.95), per
// ClassifyMesh's caps. Always reads from `pristine`; the copy is returned. Null when
// cancelled (or when nothing could be done, e.g. an empty scene: the copy is returned then).
// `parallel` runs the independent meshes over hardware_concurrency threads.
std::shared_ptr<assemble::Scene> SimplifyScene(const assemble::Scene& pristine, float reduction,
                                               const SimplifyOptions& base, SceneSimplifyStats& stats,
                                               const std::function<void(float, const std::string&)>& progress = {});

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
