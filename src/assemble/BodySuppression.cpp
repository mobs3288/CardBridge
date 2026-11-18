#include "assemble/BodySuppression.h"
#include "assemble/CoverageGrid.h"

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

inline Vec3 triCentroid(const SceneMesh& m, uint32_t i0, uint32_t i1, uint32_t i2) { return triCentroidG(m, i0, i1, i2); }
inline Vec3 triNormal(const Vec3& a, const Vec3& b, const Vec3& c) { return triNormalG(a, b, c); }

} // namespace

namespace {

// True when the material's faces sample any texel the game's shader discards (alpha below 64 — the same threshold the
// exporter's HasCutout uses). Such a shell is a lattice, not a cover: it must not conceal what is behind it. Computed
// once per call over every candidate part; the result is cached per material index.
std::vector<char> CutoutMaterials(const Scene& scene) {
    std::vector<char> cutout(scene.materials.size(), 0);
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        if (!(p.group == "Outfit" || p.group == "Accessories")) continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : m.submeshes) {
                const int miMat = sub.material;
                if (miMat < 0 || static_cast<size_t>(miMat) >= scene.materials.size()) continue;
                const size_t msi = static_cast<size_t>(miMat);
                if (cutout[msi]) continue;   // already flagged; no need to sample further
                const SceneMaterial& mat = scene.materials[msi];
                if (mat.translucent) continue;
                if (mat.texture < 0 || static_cast<size_t>(mat.texture) >= scene.textures.size()) continue;
                const SceneTexture& tex = scene.textures[static_cast<size_t>(mat.texture)];
                if (tex.width <= 0 || tex.height <= 0 || tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) continue;
                const int W = tex.width, H = tex.height;
                for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                    const uint32_t i0 = sub.indices[f], i1 = sub.indices[f + 1], i2 = sub.indices[f + 2];
                    if (i0 >= m.vertices.size() || i1 >= m.vertices.size() || i2 >= m.vertices.size()) continue;
                    float u = (m.vertices[i0].uv.x + m.vertices[i1].uv.x + m.vertices[i2].uv.x) / 3.0f;
                    float v = (m.vertices[i0].uv.y + m.vertices[i1].uv.y + m.vertices[i2].uv.y) / 3.0f;
                    u -= std::floor(u); v -= std::floor(v);
                    const int x = std::min(W - 1, static_cast<int>(u * W)), y = std::min(H - 1, static_cast<int>((1.0f - v) * H));
                    if (tex.rgba[(static_cast<size_t>(y) * W + x) * 4 + 3] < 64) { cutout[msi] = 1; break; }
                }
            }
        }
    }
    return cutout;
}

} // namespace

std::vector<CoverageProvider> GatherOpaqueProviders(const Scene& scene, bool excludeCutout) {
    std::vector<CoverageProvider> providers;
    providers.reserve(40960);
    // A cutout shell (lace, mesh socks that really have holes) is not a solid cover: with excludeCutout set, materials
    // whose faces sample a discard texel are not providers, so what is behind them is always kept. The default keeps the
    // historical (assemble-time) behaviour unchanged.
    const std::vector<char> cutout = excludeCutout ? CutoutMaterials(scene) : std::vector<char>{};
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        if (!(p.group == "Outfit" || p.group == "Accessories")) continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : m.submeshes) {
                const SceneMaterial* mat = sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() ? &scene.materials[static_cast<size_t>(sub.material)] : nullptr;
                if (mat && mat->translucent) continue;   // translucent garments do not shield
                if (!cutout.empty() && sub.material >= 0 && static_cast<size_t>(sub.material) < cutout.size() && cutout[static_cast<size_t>(sub.material)]) continue;   // perforated shell: does not shield
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

std::vector<char> ClassifyBodyOcclusion(const Scene& scene, const std::vector<int>& meshIndices, const std::vector<CoverageProvider>& providers, float maxDist, const std::string* excludeOwner) {
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
    const CoverageGrid grid = CoverageGrid::Build(providers, lo, hi);
    return ClassifyWithGrid(grid, providers, scene, meshIndices, maxDist, excludeOwner);
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
