#include "material/UvDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

namespace cardbridge::material {

namespace {
bool Finite(float v) { return std::isfinite(v); }
bool Finite2(const Vec2& v) { return Finite(v.x) && Finite(v.y); }

float TriUvArea(const Vec2& a, const Vec2& b, const Vec2& c) {
    return std::fabs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5f;
}
float Tri3dArea(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 v{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 cr{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z) * 0.5f;
}
float Len2(const Vec2& a, const Vec2& b) { const float x = b.x - a.x, y = b.y - a.y; return std::sqrt(x * x + y * y); }
float Len3(const Vec3& a, const Vec3& b) { const float x = b.x - a.x, y = b.y - a.y, z = b.z - a.z; return std::sqrt(x * x + y * y + z * z); }

// Flattened triangle list (mesh-local ordinal).
struct FT { int submesh; int tri; uint32_t a, b, c; };
std::vector<FT> FlatTris(const assemble::SceneMesh& m) {
    std::vector<FT> out;
    for (int si = 0; si < static_cast<int>(m.submeshes.size()); ++si) {
        const auto& sub = m.submeshes[static_cast<size_t>(si)];
        for (int ti = 0; ti + 2 < static_cast<int>(sub.indices.size()); ti += 3)
            out.push_back({si, ti / 3, sub.indices[static_cast<size_t>(ti)], sub.indices[static_cast<size_t>(ti + 1)], sub.indices[static_cast<size_t>(ti + 2)]});
    }
    return out;
}
} // namespace

int CountMissingUv(const assemble::SceneMesh& mesh) { return static_cast<int>(FindMissingUv(mesh).size()); }

std::vector<int> FindMissingUv(const assemble::SceneMesh& mesh) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(mesh.vertices.size()); ++i)
        if (!Finite2(mesh.vertices[static_cast<size_t>(i)].uv)) out.push_back(i);
    return out;
}

int CountOutOfRange(const assemble::SceneMesh& mesh, float margin, float& worstOutOfRange) {
    int n = 0;
    worstOutOfRange = 0.0f;
    for (const auto& v : mesh.vertices) {
        if (!Finite2(v.uv)) continue;
        float e = 0.0f;
        e = std::max(std::max(e, -margin - v.uv.x), v.uv.x - (1.0f + margin));
        e = std::max(std::max(e, -margin - v.uv.y), v.uv.y - (1.0f + margin));
        if (e > 0.0f) { ++n; worstOutOfRange = std::max(worstOutOfRange, e); }
    }
    return n;
}

float UvStretchAll(const assemble::SceneMesh& mesh, int texW, int texH,
                   float& minDensity, float& maxDensity, float& meanDensity, int& worstTri,
                   std::vector<UvStretchTri>* perTri) {
    minDensity = -1.0f; maxDensity = -1.0f; meanDensity = 0.0f; worstTri = -1;
    const auto tris = FlatTris(mesh);
    if (tris.empty() || texW <= 0 || texH <= 0) return 0.0f;
    const float texelScale = static_cast<float>(texW) * static_cast<float>(texH);
    double sum = 0.0;
    int n = 0;
    for (int fi = 0; fi < static_cast<int>(tris.size()); ++fi) {
        const FT& t = tris[static_cast<size_t>(fi)];
        auto OK = [&](uint32_t vi) { return vi < mesh.vertices.size(); };
        if (!OK(t.a) || !OK(t.b) || !OK(t.c)) continue;
        const Vec2 ua = mesh.vertices[t.a].uv, ub = mesh.vertices[t.b].uv, uc = mesh.vertices[t.c].uv;
        if (!Finite2(ua) || !Finite2(ub) || !Finite2(uc)) continue;
        const Vec3 pa = mesh.vertices[t.a].pos, pb = mesh.vertices[t.b].pos, pc = mesh.vertices[t.c].pos;
        const float uvA = TriUvArea(ua, ub, uc);
        const float dA = Tri3dArea(pa, pb, pc);
        if (dA <= 1e-9f) continue;   // numerically degenerate 3D triangle: texel density is meaningless here
        const float dens = uvA * texelScale / dA;
        float er = 1.0f;
        const float uvEdges[3] = {Len2(ua, ub), Len2(ub, uc), Len2(uc, ua)};
        const float dEdges[3] = {Len3(pa, pb), Len3(pb, pc), Len3(pc, pa)};
        for (int e = 0; e < 3; ++e) if (dEdges[e] > 1e-9f) er = std::max(er, uvEdges[e] / dEdges[e]);
        if (minDensity < 0.0f || dens < minDensity) minDensity = dens;
        if (dens > maxDensity) { maxDensity = dens; worstTri = fi; }
        sum += dens; ++n;
        if (perTri) perTri->push_back({t.submesh, t.tri, dens, er});
    }
    if (n == 0) { minDensity = -1.0f; return 0.0f; }
    meanDensity = static_cast<float>(sum / n);
    // The raw density is unit-dependent (it is uv-area × texel count over 3D area),
    // so report min/max as multiples of this mesh's mean density instead. The ratio
    // is what "stretch" means, and it survives any position-unit choice.
    if (meanDensity > 0.0f) {
        const float inv = 1.0f / meanDensity;
        if (minDensity >= 0.0f) minDensity *= inv;
        if (maxDensity >= 0.0f) maxDensity *= inv;
        meanDensity = 1.0f;
    }
    return meanDensity;
}

std::vector<std::vector<UvTriId>> UvIslands(const assemble::SceneMesh& mesh) {
    const auto tris = FlatTris(mesh);
    std::vector<std::vector<UvTriId>> islands;
    if (tris.empty()) return islands;
    // vertex -> triangle ordinals (flattened ordinals)
    std::map<uint32_t, std::vector<int>> vertToTris;
    for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
        vertToTris[tris[static_cast<size_t>(i)].a].push_back(i);
        vertToTris[tris[static_cast<size_t>(i)].b].push_back(i);
        vertToTris[tris[static_cast<size_t>(i)].c].push_back(i);
    }
    std::vector<char> seen(tris.size(), 0);
    std::queue<int> q;
    for (int start = 0; start < static_cast<int>(tris.size()); ++start) {
        if (seen[static_cast<size_t>(start)]) continue;
        std::vector<UvTriId> island;
        seen[static_cast<size_t>(start)] = 1;
        q.push(start);
        while (!q.empty()) {
            const int cur = q.front(); q.pop();
            island.push_back({tris[static_cast<size_t>(cur)].submesh, tris[static_cast<size_t>(cur)].tri});
            for (uint32_t v : {tris[static_cast<size_t>(cur)].a, tris[static_cast<size_t>(cur)].b, tris[static_cast<size_t>(cur)].c}) {
                for (int nb : vertToTris[v]) {
                    if (!seen[static_cast<size_t>(nb)]) { seen[static_cast<size_t>(nb)] = 1; q.push(nb); }
                }
            }
        }
        islands.push_back(std::move(island));
    }
    return islands;
}

int UvOverlapPairs(const assemble::SceneMesh& mesh) {
    const auto tris = FlatTris(mesh);
    if (tris.empty()) return 0;
    const auto islands = UvIslands(mesh);
    struct B { float x0, y0, x1, y1; };
    std::vector<B> boxes;
    // Map (submesh, tri) -> flattened index for bbox
    std::map<std::pair<int, int>, int> flatIdx;
    for (int i = 0; i < static_cast<int>(tris.size()); ++i) flatIdx[{tris[static_cast<size_t>(i)].submesh, tris[static_cast<size_t>(i)].tri}] = i;
    for (const auto& isl : islands) {
        B b{1e30f, 1e30f, -1e30f, -1e30f};
        bool any = false;
        for (const auto& id : isl) {
            auto it = flatIdx.find({id.submesh, id.tri});
            if (it == flatIdx.end()) continue;
            const FT& t = tris[static_cast<size_t>(it->second)];
            for (uint32_t vi : {t.a, t.b, t.c}) {
                if (vi >= mesh.vertices.size() || !Finite2(mesh.vertices[vi].uv)) continue;
                const Vec2 u = mesh.vertices[vi].uv;
                b.x0 = std::min(b.x0, u.x); b.y0 = std::min(b.y0, u.y);
                b.x1 = std::max(b.x1, u.x); b.y1 = std::max(b.y1, u.y);
                any = true;
            }
        }
        if (any) boxes.push_back(b);
    }
    int pairs = 0;
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const B& a = boxes[i]; const B& b = boxes[j];
            if (a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1) ++pairs;
        }
    return pairs;
}

int CountSeams(const assemble::SceneMesh& mesh, int& totalEdges, int& seamTriangles,
               std::vector<std::pair<uint32_t, uint32_t>>* seamEdges) {
    totalEdges = 0; seamTriangles = 0;
    const auto tris = FlatTris(mesh);
    if (tris.empty()) return 0;
    std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
    for (const auto& t : tris) {
        const uint32_t vs[3] = {t.a, t.b, t.c};
        for (int e = 0; e < 3; ++e) {
            uint32_t a = vs[e], b = vs[(e + 1) % 3];
            if (a > b) std::swap(a, b);
            edgeCount[{a, b}]++;
        }
    }
    totalEdges = static_cast<int>(edgeCount.size());
    std::set<std::pair<uint32_t, uint32_t>> seamSet;
    for (const auto& [e, n] : edgeCount) if (n == 1) {
        seamSet.insert(e);
        if (seamEdges) seamEdges->push_back(e);
    }
    // Triangles touching a seam edge.
    std::set<int> seamTriSet;
    for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
        const FT& t = tris[static_cast<size_t>(i)];
        const uint32_t vs[3] = {t.a, t.b, t.c};
        for (int e = 0; e < 3; ++e) {
            uint32_t a = vs[e], b = vs[(e + 1) % 3];
            if (a > b) std::swap(a, b);
            if (seamSet.count({a, b})) { seamTriSet.insert(i); break; }
        }
    }
    seamTriangles = static_cast<int>(seamTriSet.size());
    return static_cast<int>(seamSet.size());
}

UvReport DiagnoseUv(const assemble::SceneMesh& mesh, int texW, int texH, float margin) {
    UvReport r;
    r.margin = margin;
    r.totalVerts = static_cast<int>(mesh.vertices.size());
    r.missingUvVerts = CountMissingUv(mesh);
    r.outOfRangeVerts = CountOutOfRange(mesh, margin, r.worstOutOfRange);
    float mn, mx, mean;
    int worst;
    UvStretchAll(mesh, texW, texH, mn, mx, mean, worst, nullptr);
    r.stretchMin = mn; r.stretchMax = mx; r.stretchMean = mean; r.worstStretchTri = worst;
    if (mn >= 0.0f && mx >= 0.0f && mn > 1e-9f) r.maxStretchRatio = mx / mn;
    else r.maxStretchRatio = 0.0;
    const auto islands = UvIslands(mesh);
    r.islands = static_cast<int>(islands.size());
    r.overlapPairs = UvOverlapPairs(mesh);
    r.boundaryEdges = CountSeams(mesh, r.totalEdges, r.seamTriangles, nullptr);
    r.triangles = static_cast<int>(FlatTris(mesh).size());
    return r;
}

std::vector<UvDrawTri> UvTrisForMaterial(const assemble::Scene& scene, int materialIndex) {
    std::vector<UvDrawTri> out;
    for (int mi = 0; mi < static_cast<int>(scene.meshes.size()); ++mi) {
        const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
        for (int si = 0; si < static_cast<int>(mesh.submeshes.size()); ++si) {
            const auto& sub = mesh.submeshes[static_cast<size_t>(si)];
            if (sub.material != materialIndex) continue;
            for (int ti = 0; ti + 2 < static_cast<int>(sub.indices.size()); ti += 3)
                out.push_back({mi, si, ti / 3});
        }
    }
    return out;
}

} // namespace cardbridge::material
