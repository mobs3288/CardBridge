// Shared coverage ray-cast: buckets garment triangles into a uniform grid and answers,
// for each vertex of any mesh, "is an opaque garment in front of me?" (one ray along the
// outward normal finds an opaque provider triangle). Used by the body-suppression fallback
// (BodySuppression.cpp) and by the LOD coverage pass (mesh/Occlusion.cpp). Header-only for
// the same reason MeshQueries.h is: two callers, both want inlining, everything is small.
//
// The algorithm is the one the body fallback has always used, moved here verbatim: identical
// behaviour, identical determinism. The only addition is `excludeOwner`: a provider tagged
// with that part id never occludes (a shell never covers itself).
#pragma once

#include "assemble/Scene.h"
#include "assemble/MeshQueries.h"
#include "assemble/BodySuppression.h"   // CoverageProvider

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace cardbridge::assemble {

constexpr float kRayEpsilon = 1e-6f;

inline Vec3 triCentroidG(const SceneMesh& m, uint32_t i0, uint32_t i1, uint32_t i2) {
    const Vec3& a = m.vertices[i0].pos;
    const Vec3& b = m.vertices[i1].pos;
    const Vec3& c = m.vertices[i2].pos;
    return Vec3{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
}

inline Vec3 triNormalG(Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
    return Vec3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
}

// Möller–Trumbore ray/triangle intersection with culling-agnostic hit test; returns true when the ray hits the triangle within [tMin, tMax].
inline bool RayHitsTriangle(Vec3 ro, Vec3 rd, const Tri& t, float tMin, float tMax, float& outT) {
    const Vec3 e1{t.b.x - t.a.x, t.b.y - t.a.y, t.b.z - t.a.z}, e2{t.c.x - t.a.x, t.c.y - t.a.y, t.c.z - t.a.z};
    const Vec3 p{rd.y * e2.z - rd.z * e2.y, rd.z * e2.x - rd.x * e2.z, rd.x * e2.y - rd.y * e2.x};
    const float det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
    if (std::fabs(det) < 1e-9f) return false;
    const float invDet = 1.0f / det;
    const Vec3 s{ro.x - t.a.x, ro.y - t.a.y, ro.z - t.a.z};
    const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * invDet;
    if (u < -kRayEpsilon || u > 1.0f + kRayEpsilon) return false;
    const Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
    const float v = (rd.x * q.x + rd.y * q.y + rd.z * q.z) * invDet;
    if (v < -kRayEpsilon || v + u > 1.0f + kRayEpsilon) return false;
    const float hitT = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * invDet;
    if (hitT <= tMin || hitT >= tMax) return false;
    outT = hitT;
    return true;
}

// Slab test of a ray against an AABB — an inexpensive reject before the precise triangle test.
inline bool RayHitsAabb(Vec3 ro, Vec3 rd, float tMin, float tMax, const Tri& t) {
    float tNear = tMin, tFar = tMax;
    const float o[3] = {ro.x, ro.y, ro.z}, d[3] = {rd.x, rd.y, rd.z};
    const float mn[3] = {t.minX, t.minY, t.minZ}, mx[3] = {t.maxX, t.maxY, t.maxZ};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-12f) {
            if (o[axis] < mn[axis] || o[axis] > mx[axis]) return false;
            continue;
        }
        const float invD = 1.0f / d[axis];
        float t0 = (mn[axis] - o[axis]) * invD, t1 = (mx[axis] - o[axis]) * invD;
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return false;
    }
    return true;
}

// Buckets the provider triangles into a uniform grid so each ray visits only a handful of cells instead of every triangle.
struct ProviderGrid {
    struct Cell { std::vector<size_t> indices; };
    Vec3 origin{0, 0, 0}, cell{1.0f, 1.0f, 1.0f};
    int nx = 0, ny = 0, nz = 0;
    std::vector<Cell> cells;

    static int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
    static ProviderGrid build(const std::vector<CoverageProvider>& providers, float cellSize, const Vec3& lo, const Vec3& hi) {
        ProviderGrid g;
        g.origin = lo;
        g.cell = Vec3{cellSize, cellSize, cellSize};
        g.nx = std::max(1, static_cast<int>((hi.x - lo.x) / cellSize) + 1);
        g.ny = std::max(1, static_cast<int>((hi.y - lo.y) / cellSize) + 1);
        g.nz = std::max(1, static_cast<int>((hi.z - lo.z) / cellSize) + 1);
        g.cells.assign(static_cast<size_t>(g.nx) * static_cast<size_t>(g.ny) * static_cast<size_t>(g.nz), Cell{});
        for (size_t i = 0; i < providers.size(); ++i) {
            const Tri& t = providers[i].tri;
            const int x0 = clampi(static_cast<int>((t.minX - lo.x) / cellSize), 0, g.nx - 1);
            const int y0 = clampi(static_cast<int>((t.minY - lo.y) / cellSize), 0, g.ny - 1);
            const int z0 = clampi(static_cast<int>((t.minZ - lo.z) / cellSize), 0, g.nz - 1);
            const int x1 = clampi(static_cast<int>((t.maxX - lo.x) / cellSize), 0, g.nx - 1);
            const int y1 = clampi(static_cast<int>((t.maxY - lo.y) / cellSize), 0, g.ny - 1);
            const int z1 = clampi(static_cast<int>((t.maxZ - lo.z) / cellSize), 0, g.nz - 1);
            for (int z = z0; z <= z1; ++z)
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        g.cells[(static_cast<size_t>(z) * g.ny + y) * g.nx + x].indices.push_back(i);
        }
        return g;
    }
    // Visits the cells the ray passes through within [tMin, tMax], running `test` on each triangle in each visited cell.
    template <typename F>
    bool forEachTriangleOnRay(Vec3 ro, Vec3 rd, float tMin, float tMax, F&& test) const {
        if (nx <= 0) return false;
        // DDA over the grid; if the ray starts inside a cell, begin from it.
        int ix = clampi(static_cast<int>((ro.x - origin.x) / cell.x), 0, nx - 1);
        int iy = clampi(static_cast<int>((ro.y - origin.y) / cell.y), 0, ny - 1);
        int iz = clampi(static_cast<int>((ro.z - origin.z) / cell.z), 0, nz - 1);
        const float stepX = rd.x >= 0.0f ? 1.0f : -1.0f, stepY = rd.y >= 0.0f ? 1.0f : -1.0f, stepZ = rd.z >= 0.0f ? 1.0f : -1.0f;
        const float invX = std::fabs(rd.x) < 1e-12f ? 1e12f : 1.0f / rd.x;
        const float invY = std::fabs(rd.y) < 1e-12f ? 1e12f : 1.0f / rd.y;
        const float invZ = std::fabs(rd.z) < 1e-12f ? 1e12f : 1.0f / rd.z;
        // Standard DDA: tNext is the parametric distance at which the ray crosses the next cell boundary on each axis.
        float tNextX = stepX > 0.0f ? ((static_cast<float>(ix) + 1.0f) * cell.x - origin.x) * invX : (static_cast<float>(ix) * cell.x - origin.x) * invX;
        float tNextY = stepY > 0.0f ? ((static_cast<float>(iy) + 1.0f) * cell.y - origin.y) * invY : (static_cast<float>(iy) * cell.y - origin.y) * invY;
        float tNextZ = stepZ > 0.0f ? ((static_cast<float>(iz) + 1.0f) * cell.z - origin.z) * invZ : (static_cast<float>(iz) * cell.z - origin.z) * invZ;
        const float cellDistX = std::fabs(cell.x * invX), cellDistY = std::fabs(cell.y * invY), cellDistZ = std::fabs(cell.z * invZ);
        for (;;) {
            const Cell& cur = cells[(static_cast<size_t>(iz) * ny + iy) * nx + ix];
            for (size_t ti : cur.indices)
                if (test(ti)) return true;
            if (tNextX <= tNextY && tNextX <= tNextZ && tNextX <= tMax) {
                if (tNextX + kRayEpsilon >= tMax) break;
                ix += static_cast<int>(stepX);
                if (ix < 0 || ix >= nx) break;
                tNextX += cellDistX;
            } else if (tNextY <= tNextX && tNextY <= tNextZ && tNextY <= tMax) {
                if (tNextY + kRayEpsilon >= tMax) break;
                iy += static_cast<int>(stepY);
                if (iy < 0 || iy >= ny) break;
                tNextY += cellDistY;
            } else if (tNextZ <= tNextX && tNextZ <= tNextY && tNextZ <= tMax) {
                if (tNextZ + kRayEpsilon >= tMax) break;
                iz += static_cast<int>(stepZ);
                if (iz < 0 || iz >= nz) break;
                tNextZ += cellDistZ;
            } else {
                break;
            }
        }
        return false;
    }
};

struct CoverageGrid {
    ProviderGrid grid;
    static CoverageGrid Build(const std::vector<CoverageProvider>& providers, float lo[3], float hi[3]) {
        const float span = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
        const float cell = std::max(0.05f, span / 32.0f);   // ~4-8 cm cells
        CoverageGrid g;
        g.grid = ProviderGrid::build(providers, cell, Vec3{lo[0], lo[1], lo[2]}, Vec3{hi[0], hi[1], hi[2]});
        return g;
    }
};

// Classifies every vertex of the given meshes: 1 when an opaque provider triangle blocks the ray
// cast from a hair above the vertex along its outward normal, within `maxDist`. `excludeOwner`
// (when non-null) names a part id whose provider triangles never block (a shell never covers itself).
// Parallel to the concatenated vertex lists of `meshIndices`. The grid must have been built from the
// same `providers` vector, indexes included (ProviderGrid stores provider indices, not copies).
inline std::vector<char> ClassifyWithGrid(const CoverageGrid& coverage, const std::vector<CoverageProvider>& providers, const Scene& scene,
                                          const std::vector<int>& meshIndices, float maxDist,
                                          const std::string* excludeOwner = nullptr) {
    std::vector<char> covered;
    size_t totalVerts = 0;
    for (int mi : meshIndices)
        if (mi >= 0 && static_cast<size_t>(mi) < scene.meshes.size()) totalVerts += scene.meshes[static_cast<size_t>(mi)].vertices.size();
    covered.assign(totalVerts, 0);
    if (totalVerts == 0) return covered;

    const ProviderGrid& grid = coverage.grid;
    size_t at = 0;
    for (int mi : meshIndices) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
        for (size_t vi = 0; vi < m.vertices.size(); ++vi) {
            Vec3 n = m.vertices[vi].normal;
            float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (nl > 1e-8f) n = {n.x / nl, n.y / nl, n.z / nl};
            else n = {0.0f, 1.0f, 0.0f};
            const Vec3& o = m.vertices[vi].pos;
            // cast the ray from a hair inside the surface to avoid self-grazing on co-located triangles
            const Vec3 ro{o.x + n.x * 1e-4f, o.y + n.y * 1e-4f, o.z + n.z * 1e-4f};
            const bool hid = grid.forEachTriangleOnRay(ro, n, 1e-5f, maxDist, [&](size_t ti) {
                const CoverageProvider& p = providers[ti];
                if (excludeOwner && p.owner == *excludeOwner) return false;   // a shell never covers itself
                float t = 0.0f;
                return RayHitsAabb(ro, n, 1e-5f, maxDist, p.tri) && RayHitsTriangle(ro, n, p.tri, 1e-5f, maxDist, t);
            });
            covered[at++] = hid ? 1 : 0;
        }
    }
    return covered;
}

} // namespace cardbridge::assemble
