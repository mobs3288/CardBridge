#include "assemble/BodySuppression.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cardbridge::assemble {

bool AllCoverTranslucent(const std::map<std::string, bool>& allTranslucent, const std::vector<std::string>& cover) {
    if (cover.empty()) return false;
    for (const auto& id : cover) {
        if (id.empty()) return false;   // a hidden garment hides the skin the game-way; only visible+translucent ones let it through
        auto it = allTranslucent.find(id);
        if (it == allTranslucent.end() || !it->second) return false;
    }
    return true;
}

std::map<std::string, bool> TranslucencyByPart(const Scene& scene) {
    std::map<std::string, bool> all;
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        bool allTranslucent = true;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes) {
                const SceneMaterial* mat = sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() ? &scene.materials[static_cast<size_t>(sub.material)] : nullptr;
                if (!mat || !mat->translucent) { allTranslucent = false; break; }
            }
            if (!allTranslucent) break;
        }
        all[p.id] = allTranslucent;
    }
    return all;
}

void EmitUnderPieces(Scene& scene, const std::string& what, size_t pi, const UnderPieceSet& pieces, const std::string& tagOverride) {
    auto idTaken = [&](const std::string& id) {
        for (const auto& p : scene.parts) if (p.id == id) return true;
        return false;
    };
    for (const auto& [cover, byMesh] : pieces.byCover) {
        ScenePart piece;
        const ScenePart parent = scene.parts[pi];   // a copy: scene.parts grows below
        std::string tag;
        for (const auto& id : cover) {
            std::string shortName = id.rfind("outfit", 0) == 0 && id.find('.') != std::string::npos ? id.substr(id.find('.') + 1) : id;
            tag += (tag.empty() ? "" : "+") + (shortName.empty() ? std::string("always") : shortName);
        }
        if (!tagOverride.empty()) tag = tagOverride;
        std::string pieceId = parent.id + ".under." + tag;
        if (idTaken(pieceId)) {
            for (int n = 2;; ++n) {
                const std::string candidate = pieceId + "_" + std::to_string(n);
                if (!idTaken(candidate)) { pieceId = candidate; break; }
            }
        }
        piece.id = pieceId;
        piece.label = parent.label + " under " + tag;
        piece.group = parent.group;
        piece.garment = parent.garment;
        piece.fromClothing = parent.fromClothing;
        piece.status = parent.status;
        piece.bundle = parent.bundle;
        piece.detail = "the " + what + " triangles that " + tag + " covers (kept so the nude body can be shown)";
        piece.visibleInPreview = false;
        piece.startHidden = true;
        piece.includeInExport = parent.includeInExport;
        piece.underPiece = true;
        piece.underOf = parent.id;
        piece.coveredBy = cover;
        const int pieceIdx = static_cast<int>(scene.parts.size());
        for (const auto& [mi, byMat] : byMesh) {
            const SceneMesh src = scene.meshes[static_cast<size_t>(mi)];   // a copy: scene.meshes grows below
            SceneMesh nm;
            nm.name = src.name + ".under";
            nm.part = pieceIdx;
            nm.reshapable = src.reshapable;
            std::map<uint32_t, uint32_t> remap;
            for (const auto& [mat, indices] : byMat) {
                SceneSubmesh sm;
                sm.material = mat;
                for (uint32_t i : indices) {
                    auto it = remap.find(i);
                    if (it == remap.end()) { it = remap.emplace(i, static_cast<uint32_t>(nm.vertices.size())).first; nm.vertices.push_back(src.vertices[i]); }
                    sm.indices.push_back(it->second);
                }
                nm.submeshes.push_back(std::move(sm));
            }
            piece.meshes.push_back(static_cast<int>(scene.meshes.size()));
            scene.meshes.push_back(std::move(nm));
        }
        scene.parts.push_back(std::move(piece));
    }
}

namespace {

constexpr float kRayEpsilon = 1e-6f;

Vec3 triCentroid(const SceneMesh& m, uint32_t i0, uint32_t i1, uint32_t i2) {
    const Vec3& a = m.vertices[i0].pos;
    const Vec3& b = m.vertices[i1].pos;
    const Vec3& c = m.vertices[i2].pos;
    return Vec3{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
}

Vec3 triNormal(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
    return Vec3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
}

// Möller–Trumbore ray/triangle intersection with culling-agnostic hit test; returns true when the ray hits the triangle within [tMin, tMax].
bool RayHitsTriangle(Vec3 ro, Vec3 rd, const Tri& t, float tMin, float tMax, float& outT) {
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
bool RayHitsAabb(Vec3 ro, Vec3 rd, float tMin, float tMax, const Tri& t) {
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

} // namespace

std::vector<CoverageProvider> GatherOpaqueProviders(const Scene& scene) {
    std::vector<CoverageProvider> providers;
    providers.reserve(40960);
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        if (!(p.group == "Outfit" || p.group == "Accessories")) continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : m.submeshes) {
                const SceneMaterial* mat = sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() ? &scene.materials[static_cast<size_t>(sub.material)] : nullptr;
                if (mat && mat->translucent) continue;   // translucent garments do not shield
                for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                    const uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                    if (i0 >= m.vertices.size() || i1 >= m.vertices.size() || i2 >= m.vertices.size()) continue;
                    const Vec3& a = m.vertices[i0].pos;
                    const Vec3& b = m.vertices[i1].pos;
                    const Vec3& c = m.vertices[i2].pos;
                    providers.push_back({MakeTri(a, b, c, triNormal(a, b, c)), p.id});
                }
            }
        }
    }
    return providers;
}

std::vector<char> ClassifyBodyOcclusion(const Scene& scene, const std::vector<int>& meshIndices, const std::vector<CoverageProvider>& providers, float maxDist) {
    std::vector<char> covered;
    // compute total body vertex count only for the referenced meshes
    size_t totalVerts = 0;
    for (int mi : meshIndices)
        if (mi >= 0 && static_cast<size_t>(mi) < scene.meshes.size()) totalVerts += scene.meshes[static_cast<size_t>(mi)].vertices.size();
    covered.assign(totalVerts, 0);
    if (providers.empty() || totalVerts == 0) return covered;

    // gather the provider bounds to size the grid
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (const auto& p : providers) {
        lo[0] = std::min(lo[0], p.tri.minX); hi[0] = std::max(hi[0], p.tri.maxX);
        lo[1] = std::min(lo[1], p.tri.minY); hi[1] = std::max(hi[1], p.tri.maxY);
        lo[2] = std::min(lo[2], p.tri.minZ); hi[2] = std::max(hi[2], p.tri.maxZ);
    }
    const float span = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
    const float cell = std::max(0.05f, span / 32.0f);   // ~4-8 cm cells
    const ProviderGrid grid = ProviderGrid::build(providers, cell, Vec3{lo[0], lo[1], lo[2]}, Vec3{hi[0], hi[1], hi[2]});

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
                float t = 0.0f;
                return RayHitsAabb(ro, n, 1e-5f, maxDist, providers[ti].tri) && RayHitsTriangle(ro, n, providers[ti].tri, 1e-5f, maxDist, t);
            });
            covered[at++] = hid ? 1 : 0;
        }
    }
    return covered;
}

BodySuppressionStats SuppressBodyUnderGarments(Scene& scene, const std::vector<std::string>& kokanHiders, bool applyCoverage) {
    BodySuppressionStats stats;
    if (scene.parts.empty()) return stats;
    const std::map<std::string, bool> allTranslucent = TranslucencyByPart(scene);
    (void)allTranslucent;   // fallback doesn't use it anymore: opacity handled at the provider level

    // --- layer 2: occlusion-based suppression ---
    const std::vector<CoverageProvider> providers = applyCoverage ? GatherOpaqueProviders(scene) : std::vector<CoverageProvider>{};
    stats.providers = providers.size();
    const size_t partCount = scene.parts.size();
    for (size_t pi = 0; pi < partCount; ++pi) {
        if (scene.parts[pi].id != "body") continue;
        if (providers.empty()) break;
        const std::vector<int> meshIndices = scene.parts[pi].meshes;
        const std::vector<char> covered = ClassifyBodyOcclusion(scene, meshIndices, providers, 0.4f);
        if (const char* dbg = std::getenv("CARDBRIDGE_DEBUG_BODY_OCCLUSION")) {
            size_t cov = 0; for (char c : covered) cov += c ? 1 : 0;
            std::fprintf(stderr, "body occlusion: %zu/%zu vertices concealed\n", cov, covered.size());
        }
        size_t vertexOffset = 0;
        std::map<int, std::map<int, std::vector<uint32_t>>> byMesh;   // mesh -> material -> indices
        size_t total = 0, dropped = 0;
        for (int mi : meshIndices) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            const size_t mOff = vertexOffset;
            vertexOffset += m.vertices.size();
            for (auto& sub : m.submeshes) {
                std::vector<uint32_t> keep;
                for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                    ++total;
                    const uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                    const bool inRange = i0 < m.vertices.size() && i1 < m.vertices.size() && i2 < m.vertices.size();
                    const bool allCovered = inRange && covered[mOff + i0] && covered[mOff + i1] && covered[mOff + i2];
                    if (allCovered) {
                        ++dropped;
                        auto& dst = byMesh[mi][sub.material];
                        dst.insert(dst.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                        continue;
                    }
                    keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                }
                sub.indices = std::move(keep);
            }
        }
        UnderPieceSet pieces;
        pieces.byCover[std::vector<std::string>{"always"}] = std::move(byMesh);
        EmitUnderPieces(scene, "coverage", pi, pieces, "always");
        stats.coverageTraits += dropped;
        stats.coverageTotal += total;
        if (dropped > 0)
            scene.warn("body", "coverage: " + std::to_string(dropped) + " of " + std::to_string(total) + " body triangles are concealed by opaque clothing (kept as under pieces)");
    }

    // --- layer 3: KokanHide ---
    if (!kokanHiders.empty()) {
        int kokan = scene.boneIndex("cf_d_kokan");
        if (kokan < 0) kokan = scene.boneIndex("cf_j_kokan");
        if (kokan < 0) {
            scene.warn("kokan", "KokanHide was requested by a worn bottom item, but this rig has no cf_d_kokan/cf_j_kokan bone; the crotch skin is left visible");
        } else {
            constexpr float kRx = 0.105f, kRy = 0.095f, kRz = 0.125f;   // metres
            const Vec3 c = scene.bones[static_cast<size_t>(kokan)].pos;
            auto kokanTest = [&](Vec3 centroid) {
                const float dx = (centroid.x - c.x) / kRx, dy = (centroid.y - c.y) / kRy, dz = (centroid.z - c.z) / kRz;
                return dx * dx + dy * dy + dz * dz <= 1.0f;
            };
            for (size_t pi = 0; pi < partCount; ++pi) {
                if (scene.parts[pi].id != "body") continue;
                const std::vector<int> meshIndices = scene.parts[pi].meshes;
                UnderPieceSet pieces;
                size_t total = 0, dropped = 0;
                for (int mi : meshIndices) {
                    if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
                    SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
                    for (auto& sub : m.submeshes) {
                        std::vector<uint32_t> keep;
                        for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                            ++total;
                            const uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                            if (i0 >= m.vertices.size() || i1 >= m.vertices.size() || i2 >= m.vertices.size()) {
                                keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                                continue;
                            }
                            if (kokanTest(triCentroid(m, i0, i1, i2))) {
                                ++dropped;
                                auto& dst = pieces.byCover[std::vector<std::string>{"kokan"}][mi][sub.material];
                                dst.insert(dst.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                                continue;
                            }
                            keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                        }
                        sub.indices = std::move(keep);
                    }
                }
                EmitUnderPieces(scene, "KokanHide", pi, pieces, "kokan");
                stats.kokanTraits += dropped;
                if (dropped > 0)
                    scene.warn("kokan", "KokanHide: " + std::to_string(dropped) + " crotch triangles are hidden (a bottom with KokanHide=1 is worn)");
            }
        }
    }
    stats.fallbackUsed = stats.coverageTraits > 0;
    return stats;
}

} // namespace cardbridge::assemble
