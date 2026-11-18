// LOD coverage: per-vertex occlusion map for every visible mesh (see Occlusion.h).
// Reuses the shared ray-cast grid (assemble/CoverageGrid.h, the body-suppression machinery) so both
// passes answer the same question the same way.

#include "mesh/Occlusion.h"

#include "assemble/BodySuppression.h"
#include "assemble/CoverageGrid.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace cardbridge::mesh {

CoverageResult ComputeSceneCoverage(const assemble::Scene& scene, float maxDist) {
    CoverageResult res;
    res.meshCovered.assign(scene.meshes.size(), {});

    // Cutout shells never occlude (see GatherOpaqueProviders): what shows through a lattice must survive.
    const std::vector<assemble::CoverageProvider> providers = assemble::GatherOpaqueProviders(scene, /*excludeCutout=*/true);
    res.providerTriangles = providers.size();
    if (providers.empty()) return res;

    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& p : providers) {
        lo[0] = std::min(lo[0], p.tri.minX); hi[0] = std::max(hi[0], p.tri.maxX);
        lo[1] = std::min(lo[1], p.tri.minY); hi[1] = std::max(hi[1], p.tri.maxY);
        lo[2] = std::min(lo[2], p.tri.minZ); hi[2] = std::max(hi[2], p.tri.maxZ);
    }
    const assemble::CoverageGrid grid = assemble::CoverageGrid::Build(providers, lo, hi);

    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const assemble::SceneMesh& m = scene.meshes[mi];
        if (m.vertices.empty()) continue;
        // A part that is not drawn contributes no occlusion either (under pieces, explicitly hidden garments).
        if (m.part >= 0 && static_cast<size_t>(m.part) < scene.parts.size() && !scene.parts[static_cast<size_t>(m.part)].visibleInPreview) continue;
        // A shell never covers itself: its own part's providers are excluded from the ray walk.
        const std::string* excludeOwner = nullptr;
        std::string owner;
        if (m.part >= 0 && static_cast<size_t>(m.part) < scene.parts.size()) {
            owner = scene.parts[static_cast<size_t>(m.part)].id;
            excludeOwner = &owner;
        }
        const std::vector<char> flags = assemble::ClassifyWithGrid(grid, providers, scene, {static_cast<int>(mi)}, maxDist, excludeOwner);
        std::size_t covered = 0;
        for (char c : flags) covered += c ? 1 : 0;
        res.coveredVertices += covered;
        res.meshCovered[mi] = flags;
    }
    return res;
}

} // namespace cardbridge::mesh
