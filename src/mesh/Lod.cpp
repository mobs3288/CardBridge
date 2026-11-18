// Scene-level simplification: per-part classification, presets, and non-destructive
// multi-level LOD generation (every level is generated from the untouched original).

#include "mesh/Simplify.h"
#include "mesh/Occlusion.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <memory>
#include <mutex>
#include <thread>

namespace cardbridge::mesh {
namespace {

std::string toLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool containsAny(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (hay.find(n) != std::string::npos) return true;
    return false;
}

std::size_t trianglesOf(const assemble::SceneMesh& m) {
    std::size_t t = 0;
    for (const auto& sm : m.submeshes) t += sm.indices.size() / 3;
    return t;
}

} // namespace

const SimplifyPreset kSimplifyPresets[5] = {
    {"Original", 0.0f},
    {"High Quality", 0.20f},
    {"Balanced", 0.40f},
    {"Performance", 0.60f},
    {"Aggressive", 0.80f},
};

const MeshClassProfile& ProfileForClass(MeshClass cls) {
    static const MeshClassProfile kProfiles[] = {
        {"Eyes", 0.0f, 0.0f},         // Eye: never simplified
        {"Face", 0.25f, 0.15f},       // Face: a sliver at most
        {"Hair", 1.0f, 0.45f},        // Hair: keep the silhouette and the strand planes readable (the face-front strands carry the look)
        {"Body", 0.75f, 0.60f},       // Body: keep proportions and deformation
        {"Clothing", 1.0f, 0.80f},    // Clothing: up to a lot, by silhouette
        {"Accessories", 1.0f, 0.90f}, // Accessories: the most
        {"Other", 1.0f, 0.80f},       // Other
    };
    return kProfiles[static_cast<int>(cls)];
}

float ClassReduction(MeshClass cls, float reduction) {
    const MeshClassProfile& p = ProfileForClass(cls);
    if (reduction <= 0.0f) return 0.0f;
    return std::clamp(reduction * p.weight, 0.0f, p.cap);
}

MeshClass ClassifyMesh(const assemble::Scene& scene, std::size_t meshIndex) {
    const assemble::SceneMesh& m = scene.meshes[meshIndex];
    std::string hay = toLower(m.name);
    for (const auto& sm : m.submeshes)
        if (sm.material >= 0 && static_cast<size_t>(sm.material) < scene.materials.size())
            hay += ' ' + toLower(scene.materials[static_cast<size_t>(sm.material)].name);

    // Eyes first: whatever part they belong to, they are never simplified.
    if (containsAny(hay, {"hitomi", "iris", "pupil", "namida", "gag_eye", "eye_light", "cf_o_eye", "eyeshadow"})) return MeshClass::Eye;

    const assemble::ScenePart* part = nullptr;
    if (m.part >= 0 && static_cast<size_t>(m.part) < scene.parts.size()) part = &scene.parts[static_cast<size_t>(m.part)];
    if (part) {
        const std::string group = toLower(part->group);
        if (group == "head") {
            if (containsAny(hay, {"tooth", "canine", "tang", "mayu", "brow", "eyelash", "namida", "gag", "face"})) return MeshClass::Face;
            return MeshClass::Face;   // the head model is a facial surface; treat it as such
        }
        if (group == "body") return MeshClass::Body;
        if (group == "hair") return MeshClass::Hair;
        if (group == "outfit") return MeshClass::Clothing;
        if (group == "accessories") return MeshClass::Accessory;
    }
    // Best-effort when the part is unknown (a bare mesh).
    if (containsAny(hay, {"hair", "ke"})) return MeshClass::Hair;
    if (containsAny(hay, {"body", "o_body", "skin"})) return MeshClass::Body;
    if (containsAny(hay, {"face", "head"})) return MeshClass::Face;
    if (containsAny(hay, {"cloth", "top", "skirt", "shoe", "sock", "bra"})) return MeshClass::Clothing;
    return MeshClass::Other;
}

std::size_t SceneTriangleCount(const assemble::Scene& scene) {
    std::size_t t = 0;
    for (const auto& m : scene.meshes) t += trianglesOf(m);
    return t;
}

std::size_t SceneVertexCount(const assemble::Scene& scene) {
    std::size_t v = 0;
    for (const auto& m : scene.meshes) v += m.vertices.size();
    return v;
}

std::vector<LodLevelSpec> DefaultLodLevels() {
    return {{"lod1", "LOD 1", 0.60f}, {"lod2", "LOD 2", 0.30f}, {"lod3", "LOD 3", 0.15f}, {"lod4", "LOD 4", 0.05f}};
}

std::shared_ptr<assemble::Scene> SimplifyScene(const assemble::Scene& pristine, float reduction,
                                                const SimplifyOptions& base, SceneSimplifyStats& stats,
                                                const std::function<void(float, const std::string&)>& progress, const mesh::CoverageResult* precomputedCoverage) {
    stats = {};
    const auto t0 = std::chrono::steady_clock::now();
    auto out = std::make_shared<assemble::Scene>(pristine);   // deep copy; the caller's scene is never simplified
    stats.originalTriangles = SceneTriangleCount(pristine);
    stats.originalVertices = SceneVertexCount(pristine);
    stats.resultTriangles = stats.originalTriangles;
    stats.resultVertices = stats.originalVertices;
    if (reduction <= 0.0f || out->meshes.empty()) return out;

    // Occlusion coverage, computed once on the pristine scene before any mesh is simplified: every visible
    // mesh gets per-vertex flags ("an opaque outer garment shell replaces me from outside"). The copy indexes
    // the same way, so the flags pass straight through. Without it, all geometry is kept exactly as before.
    const mesh::CoverageResult coverageStorage = precomputedCoverage ? mesh::CoverageResult{} : mesh::ComputeSceneCoverage(pristine);
    const mesh::CoverageResult& coverage = precomputedCoverage ? *precomputedCoverage : coverageStorage;
    const bool cullOk = reduction >= 0.5f;   // LOD 2 and coarser: faces hidden by another opaque layer are dropped outright

    const std::size_t n = out->meshes.size();
    std::vector<float> meshReduction(n, 0.0f);
    std::vector<MeshClass> meshClass(n, MeshClass::Other);
    for (std::size_t i = 0; i < n; ++i) {
        meshClass[i] = ClassifyMesh(*out, i);
        meshReduction[i] = ClassReduction(meshClass[i], reduction);
    }
    stats.providerTriangles = coverage.providerTriangles;
    stats.coveredVertices = coverage.coveredVertices;

    std::atomic<std::size_t> nextMesh{0};
    std::atomic<std::size_t> doneMeshes{0};
    std::atomic<bool> anyCancelled{false};
    std::mutex warnMu;
    std::vector<std::pair<int, MeshFoldStats>> meshFolds;   // filled under warnMu

    const unsigned threads = std::max(1u, std::min(std::thread::hardware_concurrency(), static_cast<unsigned>(n)));
    std::atomic<std::size_t> occludedDropped{0};
    auto worker = [&]() {
        for (;;) {
            const std::size_t i = nextMesh.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            if (anyCancelled.load(std::memory_order_relaxed)) break;
            const float red = meshReduction[i];
            if (red <= 0.0f) {   // nothing to do for this mesh (eyes, or a tiny mesh at a gentle preset)
                const std::size_t done = doneMeshes.fetch_add(1) + 1;
                if (progress) progress(static_cast<float>(done) / static_cast<float>(n), "keeping mesh");
                continue;
            }
            SimplifyOptions o = base;
            o.maxReduction = red;
            o.progress = {};   // the scene-level progress is per mesh; per-collapse progress would be noise
            o.allowOccludedCull = cullOk && meshClass[i] == MeshClass::Clothing;   // only clothing drops hidden faces, only at LOD 2+
            const std::vector<char>* covered = i < coverage.meshCovered.size() && !coverage.meshCovered[i].empty() ? &coverage.meshCovered[i] : nullptr;
            SimplifyMeshStats st = SimplifyMesh(out->meshes[i], o, &out->bones, covered);
            if (st.cancelled) { anyCancelled = true; continue; }
            occludedDropped.fetch_add(st.occludedDropped, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(warnMu);
                meshFolds.emplace_back(static_cast<int>(i), MeshFoldStats{st.foldOver60, st.foldOver84, st.foldOver90, st.foldMaxDeg, st.skippedFoldTurn, st.occludedDropped});
                for (auto& w : st.warnings) stats.warnings.push_back(std::move(w));
            }
            const std::size_t done = doneMeshes.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress) progress(static_cast<float>(done) / static_cast<float>(n), out->meshes[i].name);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(threads > 0 ? threads - 1 : 0);
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();

    if (anyCancelled) {
        stats.cancelled = true;
        stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return nullptr;
    }
    stats.occludedFacesDropped = occludedDropped.load(std::memory_order_relaxed);
    std::sort(meshFolds.begin(), meshFolds.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    stats.meshFolds = std::move(meshFolds);
    if (stats.occludedFacesDropped > 0)
        stats.warnings.push_back("coverage: " + std::to_string(stats.occludedFacesDropped) +
                                 " triangles completely concealed by another opaque layer were dropped (faces hidden from every outward direction; visible clothing is untouched)");

    std::size_t resultTris = 0, resultVerts = 0;
    for (std::size_t i = 0; i < n; ++i) {
        resultTris += trianglesOf(out->meshes[i]);
        resultVerts += out->meshes[i].vertices.size();
        if (meshReduction[i] > 0.0f) stats.meshTriangles.emplace_back(static_cast<int>(i), trianglesOf(out->meshes[i]));
    }
    stats.resultTriangles = resultTris;
    stats.resultVertices = resultVerts;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

std::vector<LodResult> GenerateLods(const assemble::Scene& pristine, const std::vector<LodLevelSpec>& levels,
                                    const SimplifyOptions& base,
                                    const std::function<void(float, const std::string&)>& progress) {
    std::vector<LodResult> out;
    out.reserve(levels.size());
    // The occlusion map is identical for every level generated from this scene: compute it once here instead of
    // once per level (and pass it down). When nothing reduces, the coverage pass is skipped entirely.
    mesh::CoverageResult coverage;
    bool coverageComputed = false;
    for (const auto& l : levels) if (l.remaining < 1.0f) { coverage = mesh::ComputeSceneCoverage(pristine); coverageComputed = true; break; }
    const mesh::CoverageResult* coverageArg = coverageComputed ? &coverage : nullptr;
    for (std::size_t li = 0; li < levels.size(); ++li) {
        const LodLevelSpec& spec = levels[li];
        LodResult r;
        r.spec = spec;
        if (spec.remaining >= 1.0f) {   // LOD 0: never regenerated by this path
            r.scene = std::make_shared<assemble::Scene>(pristine);
            r.triangles = SceneTriangleCount(pristine);
            r.vertices = SceneVertexCount(pristine);
        } else {
            SceneSimplifyStats st;
            const float reduction = 1.0f - spec.remaining;
            auto scene = SimplifyScene(pristine, reduction, base, st, [&](float f, const std::string& stage) {
                if (progress) progress((static_cast<float>(li) + f) / static_cast<float>(levels.size()), spec.label + ": " + stage);
            }, coverageArg);
            if (!scene) {
                r.cancelled = true;
            } else {
                r.scene = std::move(scene);
                r.triangles = st.resultTriangles;
                r.vertices = st.resultVertices;
                r.seconds = st.seconds;
                r.warnings = std::move(st.warnings);
            }
        }
        if (progress) progress((static_cast<float>(li) + 1.0f) / static_cast<float>(levels.size()), spec.label);
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace cardbridge::mesh
