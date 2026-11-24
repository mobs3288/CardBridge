#include "measure/BodyMeasure.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace cardbridge::measure {
namespace {

bool IsArmBoneName(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    static const char* const kKeys[] = {"arm", "hand", "shoulder", "elbow", "wrist", "clav"};
    for (const char* k : kKeys) if (s.find(k) != std::string::npos) return true;
    return false;
}

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Length(Vec3 d) { return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

constexpr uint32_t kInvalidEdge = std::numeric_limits<uint32_t>::max();

// The skin surface as one soup of welded triangles. Coincident vertices (the seam duplicates a shaped rig leaves
// behind) are unioned first; edges then pair up by cluster id, so the slice junction works everywhere, not just on a
// 2-manifold. The body part holds the skin not covered by clothes; the triangles the garments hide are on "body.under.*"
// copies (same shape, same weights), so those are included too.
struct Soup {
    std::vector<Vec3> v;
    std::vector<uint8_t> arm;                       // 1 where the vertex is arm-dominant (T-pose arms ruin horizontal slices)
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<std::array<uint32_t, 3>> triEdges;  // the three (undirected, sorted) edge indices of each triangle
    std::unordered_map<uint64_t, uint32_t> edgeMap;
};

struct UnionFind {
    std::vector<uint32_t> parent;
    explicit UnionFind(size_t n) { parent.resize(n); for (size_t i = 0; i < n; ++i) parent[i] = static_cast<uint32_t>(i); }
    uint32_t find(uint32_t i) { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; }
    void unite(uint32_t a, uint32_t b) { a = find(a); b = find(b); if (a != b) parent[a] = b; }
};

bool Gather(const assemble::Scene& scene, Soup& soup) {
    std::vector<const assemble::SceneMesh*> meshes;
    for (const auto& part : scene.parts) {
        const bool bodySkin = part.id == "body";
        const bool chestUnder = part.underPiece && part.underOf == "body";
        if (!bodySkin && !chestUnder) continue;   // the neck is in the garment copy: it is where the chest lives
        for (int mi : part.meshes)
            if (mi >= 0 && static_cast<size_t>(mi) < scene.meshes.size()) meshes.push_back(&scene.meshes[static_cast<size_t>(mi)]);
    }
    if (meshes.empty()) return false;
    uint32_t total = 0;
    for (const auto* m : meshes) total += static_cast<uint32_t>(m->vertices.size());
    soup.v.reserve(total);
    soup.arm.reserve(total);
    // Vertices of the separate meshes are unpacked into one table; triangle indices are remapped.
    std::vector<uint32_t> base(meshes.size(), 0);
    uint32_t running = 0;
    for (size_t mi = 0; mi < meshes.size(); ++mi) { base[mi] = running; running += static_cast<uint32_t>(meshes[mi]->vertices.size()); }
    const auto& bones = scene.bones;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const assemble::SceneMesh& m = *meshes[mi];
        for (size_t vi = 0; vi < m.vertices.size(); ++vi) {
            const assemble::SceneVertex& sv = m.vertices[vi];
            soup.v.push_back(sv.pos);
            // Arm dominance: the arm shares of the vertex's skin weights, of all valid weight.
            double arm = 0.0, all = 0.0;
            for (int k = 0; k < 4; ++k) {
                const int b = sv.bone[k];
                const float w = sv.weight[k];
                if (b < 0 || static_cast<size_t>(b) >= bones.size() || !(w > 0.0f) || !std::isfinite(w)) continue;
                all += w;
                if (IsArmBoneName(bones[static_cast<size_t>(b)].name)) arm += w;
            }
            soup.arm.push_back(all > 0.0 && arm / all >= 0.5 ? 1 : 0);
        }
    }
    uint32_t index = 0;
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const assemble::SceneMesh& m = *meshes[mi];
        for (const auto& sub : m.submeshes) {
            for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
                const uint32_t a = sub.indices[i], b = sub.indices[i + 1], c = sub.indices[i + 2];
                if (a >= m.vertices.size() || b >= m.vertices.size() || c >= m.vertices.size()) continue;
                const std::array<uint32_t, 3> t{base[mi] + a, base[mi] + b, base[mi] + c};
                soup.tris.push_back(t);
                ++index;
            }
        }
    }
    // Weld coincident vertices. Bone scaling moves duplicates of the same rig vertex deterministically, so exact-
    // position equality is enough; anything looser collapses dense regions into one cluster.
    UnionFind uf(soup.v.size());
    constexpr float kCell = 0.0002f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    grid.reserve(soup.v.size() * 2);
    auto cellKey = [&](const Vec3& p) {
        const int64_t cx = static_cast<int64_t>(std::floor(p.x / kCell));
        const int64_t cy = static_cast<int64_t>(std::floor(p.y / kCell));
        const int64_t cz = static_cast<int64_t>(std::floor(p.z / kCell));
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 42) ^ (static_cast<uint64_t>(static_cast<uint32_t>(cy)) << 21) ^
               static_cast<uint64_t>(static_cast<uint32_t>(cz));
    };
    for (uint32_t i = 0; i < static_cast<uint32_t>(soup.v.size()); ++i) {
        const int64_t cx = static_cast<int64_t>(std::floor(soup.v[i].x / kCell));
        const int64_t cy = static_cast<int64_t>(std::floor(soup.v[i].y / kCell));
        const int64_t cz = static_cast<int64_t>(std::floor(soup.v[i].z / kCell));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx + dx)) << 42) ^
                                         (static_cast<uint64_t>(static_cast<uint32_t>(cy + dy)) << 21) ^
                                         static_cast<uint64_t>(static_cast<uint32_t>(cz + dz));
                    auto it = grid.find(key);
                    if (it != grid.end())
                        for (uint32_t j : it->second)
                            if (soup.v[i].x == soup.v[j].x && soup.v[i].y == soup.v[j].y && soup.v[i].z == soup.v[j].z) uf.unite(i, j);
                }
        grid[cellKey(soup.v[i])].push_back(i);
    }
    std::vector<uint32_t> cluster(soup.v.size(), std::numeric_limits<uint32_t>::max());
    uint32_t nClusters = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(soup.v.size()); ++i) {
        const uint32_t root = uf.find(i);
        if (cluster[root] == std::numeric_limits<uint32_t>::max()) cluster[root] = nClusters++;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(soup.v.size()); ++i) cluster[i] = cluster[uf.find(i)];
    // The triangle indices move to cluster ids; the vertex table goes with them (a representative vertex keeps the spot).
    std::vector<std::vector<uint32_t>> byCluster(nClusters);
    for (uint32_t i = 0; i < static_cast<uint32_t>(soup.v.size()); ++i) byCluster[cluster[i]].push_back(i);
    std::vector<Vec3> cv; cv.reserve(nClusters);
    std::vector<uint8_t> ca; ca.reserve(nClusters);
    for (uint32_t c = 0; c < nClusters; ++c) {
        const uint32_t rep = byCluster[c].empty() ? 0 : byCluster[c].front();
        cv.push_back(soup.v[rep]);
        ca.push_back(soup.arm[rep]);
    }
    soup.v = std::move(cv);
    soup.arm = std::move(ca);
    // Re-index the triangles onto the clusters; a sliver left with duplicate corners is dropped.
    for (auto& t : soup.tris) {
        t[0] = cluster[t[0]]; t[1] = cluster[t[1]]; t[2] = cluster[t[2]];
    }
    soup.tris.erase(std::remove_if(soup.tris.begin(), soup.tris.end(), [](const std::array<uint32_t, 3>& t) {
        return t[0] == t[1] || t[1] == t[2] || t[2] == t[0];
    }), soup.tris.end());
    // The global edge table (undirected, a<b) so a slice junction can be matched by edge, not by position tolerance.
    for (const auto& t : soup.tris) {
        std::array<uint32_t, 3> e{};
        for (int k = 0; k < 3; ++k) {
            uint32_t a = t[k], b = t[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            const uint64_t key = (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
            auto it = soup.edgeMap.find(key);
            if (it == soup.edgeMap.end()) { const uint32_t id = static_cast<uint32_t>(soup.edgeMap.size()); soup.edgeMap[key] = id; e[k] = id; }
            else e[k] = it->second;
        }
        soup.triEdges.push_back(e);
    }
    // Keep only the largest connected component of the skin: helper shells, islands and torn patches never feed the
    // measurement. Weld clusters (uv seams) already group the vertices; the edge table groups the triangles.
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> edgeTris;
        edgeTris.reserve(soup.triEdges.size() * 3 / 2);
        for (size_t ti = 0; ti < soup.tris.size(); ++ti)
            for (int k = 0; k < 3; ++k)
                edgeTris[(static_cast<uint64_t>(soup.triEdges[ti][k]))].push_back(static_cast<uint32_t>(ti));
        UnionFind triUf(soup.tris.size());
        for (const auto& kv : edgeTris)
            for (size_t i = 0; i + 1 < kv.second.size(); ++i) triUf.unite(kv.second[i], kv.second[i + 1]);
        std::unordered_map<uint32_t, size_t> sizeOf;
        size_t bestSize = 0;
        uint32_t bestRoot = 0;
        for (size_t i = 0; i < soup.tris.size(); ++i) {
            const uint32_t root = triUf.find(static_cast<uint32_t>(i));
            const size_t s = ++sizeOf[root];
            if (s > bestSize) { bestSize = s; bestRoot = root; }
        }
        if (bestSize > 0 && bestSize != soup.tris.size()) {
            std::vector<std::array<uint32_t, 3>> keptTris, keptEdges;
            std::unordered_map<uint64_t, uint32_t> keptMap;
            keptTris.reserve(bestSize);
            keptEdges.reserve(bestSize);
            for (size_t i = 0; i < soup.tris.size(); ++i) {
                if (triUf.find(static_cast<uint32_t>(i)) != bestRoot) continue;
                keptTris.push_back(soup.tris[i]);
                // edges are re-mapped compactly: every kept edge gets a fresh id (remapped from the table built above)
                const auto& e = soup.triEdges[i];
                std::array<uint32_t, 3> ne{};
                for (int k = 0; k < 3; ++k) {
                    const auto it = keptMap.find(e[k]);
                    if (it == keptMap.end()) { const uint32_t id = static_cast<uint32_t>(keptMap.size()); keptMap[e[k]] = id; ne[k] = id; }
                    else ne[k] = it->second;
                }
                keptEdges.push_back(ne);
            }
            soup.tris = std::move(keptTris);
            soup.triEdges = std::move(keptEdges);
            soup.edgeMap = std::move(keptMap);
        }
    }
    (void)index;
    return true;
}

// One slice: every triangle whose y-interval contains y, cut by the plane, joined back into contour chains. Arms and
// legs are left in the soup on purpose: the trunk encloses the spine, so chains off its axis lose to it. Outputs:
// `allChains` gets every contour's length (unordered), `trunkLen` the trunk's (0 when the slice found none), `picked`
// the trunk's points. Chains are joined on the mesh's edges (topological), not by position tolerance.
void SliceChains(const Soup& soup, float y, std::vector<float>* allChains, float* trunkLen, std::vector<Vec3>* picked,
                 std::vector<SliceChain>* allInfo = nullptr) {
    if (allChains) allChains->clear();
    if (trunkLen) *trunkLen = 0.0f;
    if (picked) picked->clear();
    if (allInfo) allInfo->clear();
    struct Seg {
        Vec3 p, q;             // p = the endpoint on edge ea, q = the endpoint on edge eb
        uint32_t ea = kInvalidEdge, eb = kInvalidEdge;
    };
    std::vector<Seg> segs;
    for (size_t ti = 0; ti < soup.tris.size(); ++ti) {
        const auto& t = soup.tris[ti];
        // Arms excluded from the contour: the trunk ring closes without them (the armpit is rooted in the torso),
        // and the T-pose arms would otherwise read as part of the same slice.
        int armVerts = 0;
        for (uint32_t i : t) armVerts += soup.arm[i] ? 1 : 0;
        if (armVerts >= 2) continue;
        const Vec3& va = soup.v[t[0]];
        const Vec3& vb = soup.v[t[1]];
        const Vec3& vc = soup.v[t[2]];
        const float ya = va.y - y, yb = vb.y - y, yc = vc.y - y;
        if ((ya < 0.0f && yb < 0.0f && yc < 0.0f) || (ya >= 0.0f && yb >= 0.0f && yc >= 0.0f)) continue;   // entirely on one side
        if (ya == 0.0f && yb == 0.0f && yc == 0.0f) continue;                                               // coplanar
        // Intersect with the plane; each of the (up to two) crossing points belongs to one of the triangle's edges.
        Vec3 hit[3]; uint32_t hitEdge[3]; int nHit = 0;
        const Vec3* vp[3] = {&va, &vb, &vc};
        const float vy[3] = {ya, yb, yc};
        for (int e = 0; e < 3; ++e) {
            const int f = (e + 1) % 3;
            if ((vy[e] < 0.0f) == (vy[f] < 0.0f)) continue;
            const float t2 = vy[e] / (vy[e] - vy[f]);
            hit[nHit] = *vp[e] + (*vp[f] - *vp[e]) * t2;
            hitEdge[nHit] = soup.triEdges[ti][e];   // edge (vertex e, vertex f)
            ++nHit;
        }
        if (nHit != 2) continue;
        if (Length(hit[0] - hit[1]) < 1e-6f) continue;
        Seg s;
        s.p = hit[0]; s.q = hit[1]; s.ea = hitEdge[0]; s.eb = hitEdge[1];
        segs.push_back(s);
    }
    if (segs.empty()) return;
    // Junction map: each mesh edge crossing the plane appears in the two triangles that share it; link those endpoints.
    std::unordered_map<uint32_t, std::vector<uint32_t>> byEdge;   // edgeId -> seg indices (each edge yields one endpoint per seg)
    byEdge.reserve(segs.size() * 2);
    for (uint32_t i = 0; i < static_cast<uint32_t>(segs.size()); ++i) {
        if (segs[i].ea != kInvalidEdge) byEdge[segs[i].ea].push_back(i);
        if (segs[i].eb != kInvalidEdge) byEdge[segs[i].eb].push_back(i);
    }
    // Endpoint id: 2*seg+side (0 = p/ea, 1 = q/eb). Link the two endpoints that share an edge.
    const size_t nPort = segs.size() * 2;
    std::vector<uint32_t> mate(nPort, kInvalidEdge);
    for (const auto& kv : byEdge) {
        const auto& list = kv.second;
        for (size_t i = 0; i + 1 < list.size(); i += 2) {
            const uint32_t a = list[i], b = list[i + 1];
            // side of each seg at the shared edge: where the seg's ea/eb equals this edgeId
            const int sideA = segs[a].ea == kv.first ? 0 : 1;
            const int sideB = segs[b].ea == kv.first ? 0 : 1;
            mate[2 * a + static_cast<size_t>(sideA)] = 2 * b + static_cast<size_t>(sideB);
            mate[2 * b + static_cast<size_t>(sideB)] = 2 * a + static_cast<size_t>(sideA);
        }
    }
    std::vector<uint8_t> used(segs.size(), 0);
    struct Chain { std::vector<Vec3> pts; float len; float cx, cz; float area; };
    std::vector<Chain> found;
    for (size_t s0 = 0; s0 < segs.size(); ++s0) {
        if (used[s0]) continue;
        used[s0] = 1;
        std::vector<Vec3> pts;
        pts.push_back(segs[s0].p);
        pts.push_back(segs[s0].q);
        float len = Length(segs[s0].q - segs[s0].p);
        int cur = static_cast<int>(s0);
        int side = 1;   // we entered s0 from its p; leave through its q
        for (;;) {
            const uint32_t next = mate[static_cast<size_t>(2 * cur + side)];
            if (next == kInvalidEdge) break;
            const uint32_t seg = next >> 1;
            const int sideOut = static_cast<int>(next & 1);
            if (seg == s0) break;   // closed back to the start
            if (used[seg]) break;   // shouldn't happen on a 2-manifold; stop rather than loop forever
            used[seg] = 1;
            pts.push_back(sideOut == 0 ? segs[seg].q : segs[seg].p);
            len += Length(segs[seg].q - segs[seg].p);
            cur = static_cast<int>(seg);
            side = 1 - sideOut;     // leave through the other end of that segment
        }
        Chain c;
        c.pts = std::move(pts);
        c.len = len;
        double cx = 0.0, cz = 0.0;
        for (const Vec3& p : c.pts) { cx += p.x; cz += p.z; }
        c.cx = static_cast<float>(cx / static_cast<double>(c.pts.size()));
        c.cz = static_cast<float>(cz / static_cast<double>(c.pts.size()));
        double area = 0.0;
        for (size_t i = 0; i < c.pts.size(); ++i) {
            const Vec3& a = c.pts[i];
            const Vec3& b = c.pts[(i + 1) % c.pts.size()];
            area += static_cast<double>(a.x) * b.z - static_cast<double>(b.x) * a.z;
        }
        c.area = static_cast<float>(std::fabs(area) * 0.5);
        found.push_back(std::move(c));
    }
    // The trunk encloses the spine: its centroid is on the axis. Arms (T-pose, horizontal at shoulder height) and
    // thighs sit off to the sides, so candidates off the axis lose regardless of how big they are.
    float bestLen = 0.0f, bestArea = -1.0f;
    size_t bestIdx = SIZE_MAX;
    for (size_t i = 0; i < found.size(); ++i) {
        const Chain& c = found[i];
        if (c.pts.size() < 8) continue;
        if (std::fabs(c.cx) > 0.10f || std::fabs(c.cz) > 0.35f) continue;
        if (c.area > bestArea) { bestArea = c.area; bestLen = c.len; bestIdx = i; }
    }
    if (bestIdx == SIZE_MAX && !found.empty()) {   // degenerate case: nothing qualifies; the longest chain wins
        for (size_t i = 0; i < found.size(); ++i) if (found[i].len > bestLen) { bestLen = found[i].len; bestIdx = i; }
    }
    if (allChains) {
        allChains->reserve(found.size());
        for (const Chain& c : found) allChains->push_back(c.len);
    }
    if (allInfo) {
        allInfo->reserve(found.size());
        for (const Chain& c : found)
            allInfo->push_back({c.len, c.cx, c.cz, static_cast<int>(c.pts.size())});
    }
    if (bestIdx == SIZE_MAX) return;
    if (trunkLen) *trunkLen = bestLen;
    if (picked) *picked = found[bestIdx].pts;
}

} // namespace

float TorsoPerimeterAt(const assemble::Scene& scene, float y) {
    Soup soup;
    if (!Gather(scene, soup)) return 0.0f;
    float trunk = 0.0f;
    SliceChains(soup, y, nullptr, &trunk, nullptr);
    return trunk;
}
void SliceChainsForDebug(const assemble::Scene& scene, float y, std::vector<float>& chains) {
    Soup soup;
    chains.clear();
    if (!Gather(scene, soup)) return;
    SliceChains(soup, y, &chains, nullptr, nullptr);
}

void SliceChainsAt(const assemble::Scene& scene, float y, std::vector<SliceChain>& chains) {
    Soup soup;
    chains.clear();
    if (!Gather(scene, soup)) return;
    SliceChains(soup, y, nullptr, nullptr, nullptr, &chains);
    std::sort(chains.begin(), chains.end(), [](const SliceChain& a, const SliceChain& b) { return a.length > b.length; });
}

float LimbPerimeterAt(const assemble::Scene& scene, float y, float cx, float cz, float minLength) {
    std::vector<SliceChain> chains;
    SliceChainsAt(scene, y, chains);
    float best = 0.0f, bestDist = std::numeric_limits<float>::max();
    for (const SliceChain& c : chains) {
        if (c.points < 8 || c.length < minLength) continue;   // seam rings and helper shells
        const float dx = c.cx - cx, dz = c.cz - cz;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < bestDist) { bestDist = dist; best = c.length; }
    }
    return best;
}

Result Measure(const assemble::Scene& scene) {
    Result r;
    Soup soup;
    if (!Gather(scene, soup) || soup.tris.empty() || soup.v.empty()) return r;
    float yBot = std::numeric_limits<float>::max(), yTop = -std::numeric_limits<float>::max();
    for (const Vec3& p : soup.v) { yBot = std::min(yBot, p.y); yTop = std::max(yTop, p.y); }
    if (!(yTop > yBot)) return r;
    r.heightMeters = yTop - yBot;

    // Anatomical anchors from the rig: the waist sits between the hip bone and the mid-chest bone; the bust is the
    // peak above that; the shoulders sit above the chest top and stay out of the bust search. Falls back to body
    // fractions when the bones are not named as expected.
    const float h = r.heightMeters;
    auto boneY = [&](const char* name) {
        const int i = scene.boneIndex(name);
        return i >= 0 ? scene.bones[static_cast<size_t>(i)].pos.y : -std::numeric_limits<float>::max();
    };
    float waistLo = boneY("cf_s_waist02"), waistHi = boneY("cf_s_spine02"), chestTop = boneY("cf_s_spine03");
    if (waistLo <= -1e8f || waistHi <= -1e8f) { waistLo = yBot + 0.55f * h; waistHi = yBot + 0.80f * h; }   // no anchors: fraction fallback
    if (chestTop <= -1e8f) chestTop = yBot + 0.90f * h;
    if (chestTop < waistHi) chestTop = waistHi + 0.5f * (yTop - waistHi);
    waistLo = std::max(waistLo, yBot + 0.30f * h);
    chestTop = std::min(chestTop, yBot + 0.95f * h);
    const float yLo = yBot + 0.15f * h, yHi = yBot + 0.97f * h;
    const int steps = 160;
    std::vector<float> ys(static_cast<size_t>(steps)), perim(static_cast<size_t>(steps));
    for (int i = 0; i < steps; ++i) ys[static_cast<size_t>(i)] = yLo + (yHi - yLo) * (static_cast<float>(i) / static_cast<float>(steps - 1));
    int good = 0;
    for (int i = 0; i < steps; ++i) {
        float trunk = 0.0f;
        SliceChains(soup, ys[static_cast<size_t>(i)], nullptr, &trunk, nullptr);
        perim[static_cast<size_t>(i)] = trunk;
        if (perim[static_cast<size_t>(i)] > 0.15f) ++good;
    }
    if (std::getenv("CARDBRIDGE_BWH_DEBUG")) {
        for (int i = 0; i < steps; ++i) std::fprintf(stderr, "  perim[%3d] y=%.4f  %.1f cm\n", i, ys[static_cast<size_t>(i)], perim[static_cast<size_t>(i)] * 100.0f);
    }
    if (good < 24) return r;
    // Waist: the narrowest contour of the trunk, searched between the hip bone and the mid-chest bone.
    int waistIdx = -1;
    float waistMin = std::numeric_limits<float>::max();
    for (int i = 0; i < steps; ++i) {
        const float y = ys[static_cast<size_t>(i)];
        if (y < waistLo || y > waistHi) continue;
        const float p = perim[static_cast<size_t>(i)];
        if (p > 0.15f && p < waistMin) { waistMin = p; waistIdx = i; }
    }
    if (waistIdx < 0) return r;
    const float waistY = ys[static_cast<size_t>(waistIdx)];
    // Hip: the widest contour below the waist (the buttocks); bust: the widest above it.
    int hipIdx = -1;
    float hipMax = 0.0f;
    for (int i = 0; i < steps; ++i) {
        const float y = ys[static_cast<size_t>(i)];
        if (y >= waistY || y < yBot + 0.18f * h) continue;
        const float p = perim[static_cast<size_t>(i)];
        if (p > hipMax) { hipMax = p; hipIdx = i; }
    }
    int bustIdx = -1;
    float bustMax = 0.0f;
    for (int i = 0; i < steps; ++i) {
        const float y = ys[static_cast<size_t>(i)];
        if (y < waistY || y > chestTop) continue;
        const float p = perim[static_cast<size_t>(i)];
        if (p > bustMax) { bustMax = p; bustIdx = i; }
    }
    if (bustIdx < 0 || hipIdx < 0) return r;
    r.valid = true;
    r.waist = waistMin;
    r.hip = hipMax;
    r.bust = bustMax;
    r.waistY = waistY;
    r.hipY = ys[static_cast<size_t>(hipIdx)];
    r.bustY = ys[static_cast<size_t>(bustIdx)];
    r.slices = good;
    // Plausibility: cards whose skin has torn regions (a missing neck, a disassembled bust) can produce absurd
    // contours; a "waist" a fraction of the bust or the hips is not a waist. In that case the measurement is not
    // used at all — the caller falls back to the plain slider view.
    const bool plausible = r.waist >= 0.18f && r.waist >= 0.35f * r.bust && r.waist >= 0.35f * r.hip && r.hip >= 0.45f && r.bust >= 0.35f;
    if (!plausible) r = Result{};
    return r;
}

} // namespace cardbridge::measure
