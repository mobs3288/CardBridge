#include "assemble/BodySuppression.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

Vec3 triCentroid(const SceneMesh& m, uint32_t i0, uint32_t i1, uint32_t i2) {
    const Vec3& a = m.vertices[i0].pos;
    const Vec3& b = m.vertices[i1].pos;
    const Vec3& c = m.vertices[i2].pos;
    return Vec3{(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
}

// Moves the triangles of part `pi` that `test(centroid)` marks as covered to under pieces (shifting their indices in place),
// filling `pieces`; returns nothing. `test` may add ids to `cover`; an empty result keeps the triangle.
void MoveCoveredTriangles(Scene& scene, const std::string& what, size_t pi, size_t& total, size_t& dropped, UnderPieceSet& pieces,
                          const std::function<bool(Vec3 centroid, std::vector<std::string>& cover)>& test) {
    const std::vector<int> meshIndices = scene.parts[pi].meshes;
    for (int mi : meshIndices) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
        for (auto& sub : m.submeshes) {
            std::vector<uint32_t> keep;
            for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                ++total;
                const uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                const bool inRange = i0 < m.vertices.size() && i1 < m.vertices.size() && i2 < m.vertices.size();
                std::vector<std::string> cover;
                if (inRange && test(triCentroid(m, i0, i1, i2), cover)) {
                    // a garment the game itself hides still covers: it counts as "" (always)
                    for (auto& id : cover)
                        for (const auto& gp : scene.parts)
                            if (gp.id == id && !gp.visibleInPreview) id.clear();
                    std::sort(cover.begin(), cover.end());
                    cover.erase(std::unique(cover.begin(), cover.end()), cover.end());
                    ++dropped;
                    auto& dst = pieces.byCover[cover][mi][sub.material];
                    dst.insert(dst.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                    continue;
                }
                keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
            }
            sub.indices = std::move(keep);
        }
    }
    (void)what;
}

} // namespace

BodySuppressionStats SuppressBodyUnderGarments(Scene& scene, const std::vector<std::string>& kokanHiders, float eps) {
    BodySuppressionStats stats;
    if (scene.parts.empty()) return stats;
    const std::map<std::string, bool> allTranslucent = TranslucencyByPart(scene);

    // --- layer 2: spatial coverage ------------------------------------------------
    // The opaque triangles of every worn Outfit/Accessory part. Each provider carries its owner's part id so a triangle
    // shared by several garments is tagged with every coverer (layered clothing). A garment switched off leaves no
    // provider behind: the assembly that excluded it never built its meshes.
    std::vector<Tri> providers;
    std::vector<std::string> owner;
    providers.reserve(32768);
    owner.reserve(32768);
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        if (!(p.group == "Outfit" || p.group == "Accessories")) continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : m.submeshes) {
                const SceneMaterial* mat = sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() ? &scene.materials[static_cast<size_t>(sub.material)] : nullptr;
                if (mat && mat->translucent) continue;   // translucent garments mask nothing: the skin shows through on purpose
                for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                    const uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                    if (i0 >= m.vertices.size() || i1 >= m.vertices.size() || i2 >= m.vertices.size()) continue;
                    const Vec3& a = m.vertices[i0].pos;
                    const Vec3& b = m.vertices[i1].pos;
                    const Vec3& c = m.vertices[i2].pos;
                    const Vec3 n = {(b.y - c.y) * (a.z - c.z) - (b.z - c.z) * (a.y - c.y),
                                    (b.z - c.z) * (a.x - c.x) - (b.x - c.x) * (a.z - c.z),
                                    (b.x - c.x) * (a.y - c.y) - (b.y - c.y) * (a.x - c.x)};
                    providers.push_back(MakeTri(a, b, c, n));
                    owner.push_back(p.id);
                }
            }
        }
    }
    stats.providers = providers.size();
    const float epsSq = eps * eps;
    auto coverageTest = [&](Vec3 centroid, std::vector<std::string>& cover) {
        for (size_t qi = 0; qi < providers.size(); ++qi) {
            const Tri& tri = providers[qi];
            if (centroid.x < tri.minX - eps || centroid.x > tri.maxX + eps) continue;
            if (centroid.y < tri.minY - eps || centroid.y > tri.maxY + eps) continue;
            if (centroid.z < tri.minZ - eps || centroid.z > tri.maxZ + eps) continue;
            Vec3 closest;
            float distSq = 0.0f;
            closestOnTriangle(centroid, tri, closest, distSq);
            if (distSq <= epsSq && std::find(cover.begin(), cover.end(), owner[qi]) == cover.end()) cover.push_back(owner[qi]);
        }
        return !cover.empty();
    };

    // the body part(s) only — clothing hides nothing of itself here
    const size_t partCount = scene.parts.size();
    for (size_t pi = 0; pi < partCount; ++pi) {
        if (scene.parts[pi].id != "body") continue;
        UnderPieceSet pieces;
        size_t total = 0, dropped = 0;
        MoveCoveredTriangles(scene, "coverage", pi, total, dropped, pieces, coverageTest);
        EmitUnderPieces(scene, "coverage", pi, pieces);
        stats.coverageTraits += dropped;
        stats.coverageTotal += total;
        if (dropped > 0)
            scene.warn("body", "coverage: " + std::to_string(dropped) + " of " + std::to_string(total) + " body triangles are hidden under opaque clothing (kept as under pieces)");
    }

    // --- layer 3: KokanHide -------------------------------------------------
    if (!kokanHiders.empty()) {
        int kokan = scene.boneIndex("cf_d_kokan");
        if (kokan < 0) kokan = scene.boneIndex("cf_j_kokan");
        if (kokan < 0) {
            scene.warn("kokan", "KokanHide was requested by a worn bottom item, but this rig has no cf_d_kokan/cf_j_kokan bone; the crotch skin is left visible");
        } else {
            constexpr float kRx = 0.105f, kRy = 0.095f, kRz = 0.125f;   // metres
            const Vec3 c = scene.bones[static_cast<size_t>(kokan)].pos;
            auto kokanTest = [&](Vec3 centroid, std::vector<std::string>& cover) {
                const float dx = (centroid.x - c.x) / kRx, dy = (centroid.y - c.y) / kRy, dz = (centroid.z - c.z) / kRz;
                if (dx * dx + dy * dy + dz * dz > 1.0f) return false;
                cover = kokanHiders;
                return true;
            };
            for (size_t pi = 0; pi < partCount; ++pi) {
                if (scene.parts[pi].id != "body") continue;
                UnderPieceSet pieces;
                size_t total = 0, dropped = 0;
                MoveCoveredTriangles(scene, "KokanHide", pi, total, dropped, pieces, kokanTest);
                EmitUnderPieces(scene, "KokanHide", pi, pieces, "kokan");
                stats.kokanTraits += dropped;
                if (dropped > 0)
                    scene.warn("kokan", "KokanHide: " + std::to_string(dropped) + " crotch triangles are hidden (a bottom with KokanHide=1 is worn)");
            }
        }
    }
    return stats;
}

} // namespace cardbridge::assemble
