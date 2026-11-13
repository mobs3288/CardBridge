#include "mesh/Simplify.h"

#include "core/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cardbridge::mesh {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// Symmetric 4x4 stored as its upper triangle (10 floats): the QEM quadric.
struct Quadric {
    double q[10] = {};

    static Quadric zero() { return {}; }
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) q[i] += o.q[i]; }
    // [x y z w] * Q * [x y z w]^T
    double evaluate(double x, double y, double z, double w) const {
        const double* a = q;
        const double x2 = a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x * w +
                          a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y * w +
                          a[7] * z * z + 2 * a[8] * z * w + a[9] * w * w;
        return x2;
    }
    // The 3x3 block A (q[0..7]) and the right-hand side of A * p = -b (q[3], q[6], q[8]); the optimal point.
    bool optimal(Vec3& out) const {
        const double a00 = q[0], a01 = q[1], a02 = q[2], a03 = q[3];
        const double a11 = q[4], a12 = q[5], a13 = q[6];
        const double a22 = q[7], a23 = q[8];
        const double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) + a02 * (a01 * a12 - a11 * a02);
        if (std::fabs(det) < 1e-12 * (std::fabs(a00) + std::fabs(a11) + std::fabs(a22) + 1e-6)) return false;
        const double inv = 1.0 / det;
        out.x = static_cast<float>(-((a11 * a22 - a12 * a12) * a03 - (a01 * a22 - a12 * a02) * a13 + (a01 * a12 - a11 * a02) * a23) * inv);
        out.y = static_cast<float>(-(-(a01 * a22 - a12 * a02) * a03 + (a00 * a22 - a02 * a02) * a13 - (a00 * a12 - a01 * a02) * a23) * inv);
        out.z = static_cast<float>(-((a01 * a12 - a11 * a02) * a03 - (a00 * a12 - a01 * a02) * a13 + (a00 * a11 - a01 * a01) * a23) * inv);
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }
    static Quadric fromPlane(Vec3 n, float d, double scale = 1.0) {
        Quadric r;
        const double nx = n.x, ny = n.y, nz = n.z, dd = d;
        r.q[0] = nx * nx * scale; r.q[1] = nx * ny * scale; r.q[2] = nx * nz * scale; r.q[3] = nx * dd * scale;
        r.q[4] = ny * ny * scale; r.q[5] = ny * nz * scale; r.q[6] = ny * dd * scale;
        r.q[7] = nz * nz * scale; r.q[8] = nz * dd * scale; r.q[9] = dd * dd * scale;
        return r;
    }
    // Embed a 3x3 scatter (w * g gᵀ) around p0 into homogeneous space: [ [A, -A p0], [-p0ᵀA, p0ᵀ A p0] ].
    static Quadric fromScatter(Vec3 g, double w, Vec3 p0) {
        const double a00 = g.x * g.x * w, a01 = g.x * g.y * w, a02 = g.x * g.z * w;
        const double a11 = g.y * g.y * w, a12 = g.y * g.z * w, a22 = g.z * g.z * w;
        const double px = p0.x, py = p0.y, pz = p0.z;
        const double b0 = -(a00 * px + a01 * py + a02 * pz), b1 = -(a01 * px + a11 * py + a12 * pz), b2 = -(a02 * px + a12 * py + a22 * pz);
        Quadric r;
        r.q[0] = a00; r.q[1] = a01; r.q[2] = a02; r.q[3] = b0;
        r.q[4] = a11; r.q[5] = a12; r.q[6] = b1;
        r.q[7] = a22; r.q[8] = b2;
        r.q[9] = a00 * px * px + 2 * a01 * px * py + 2 * a02 * px * pz + a11 * py * py + 2 * a12 * py * pz + a22 * pz * pz;
        return r;
    }
};

struct WFace {
    int v[3] = {0, 0, 0};
    int submesh = 0;
    bool alive = true;
};

struct WVertex {
    Vec3 pos;
    Vec3 normal;
    Vec2 uv[4];
    int bone[4];
    float weight[4];
    Quadric quadric;
    bool alive = true;
    int stamp = 0;
};

struct CollapseRec { int u, v; float t; };

struct Candidate {
    float cost;
    int u, v;
    int stampU, stampV;
};
struct CandidateGreater {
    bool operator()(const Candidate& a, const Candidate& b) const { return a.cost > b.cost; }
};

Vec3 lerp(Vec3 a, Vec3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
Vec2 lerp2(Vec2 a, Vec2 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 sub3(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float lenSquared(Vec3 a) { return dot3(a, a); }

// The gradient of attribute values c0..c2 over the triangle (in the plane span, so orthogonal to the normal).
Vec3 attributeGradient(Vec3 p0, Vec3 p1, Vec3 p2, float c0, float c1, float c2, bool& ok) {
    const Vec3 e1 = sub3(p1, p0), e2 = sub3(p2, p0);
    const float d1 = c1 - c0, d2 = c2 - c0;
    const float aa = dot3(e1, e1), ab = dot3(e1, e2), bb = dot3(e2, e2);
    const float det = aa * bb - ab * ab;
    if (det <= 1e-20f * (aa * bb + 1e-18f)) { ok = false; return {}; }
    const float inv = 1.0f / det;
    const float f1 = (d1 * bb - d2 * ab) * inv, f2 = (d2 * aa - d1 * ab) * inv;
    const Vec3 g{e1.x * f1 + e2.x * f2, e1.y * f1 + e2.y * f2, e1.z * f1 + e2.z * f2};
    ok = true;
    return g;
}

// Blend two influence sets (u dies into v's destination) into one renormalised set of 4.
void blendWeights(const int ub[4], const float uw[4], const int vb[4], const float vw[4], float t, int outB[4], float outW[4]) {
    struct Acc { int bone; float w; } acc[8];
    int n = 0;
    auto add = [&acc, &n](int bone, float w) {
        if (w <= 0.0f || bone < 0) return;
        for (int i = 0; i < n; ++i)
            if (acc[i].bone == bone) { acc[i].w += w; return; }
        acc[n].bone = bone; acc[n].w = w; ++n;
    };
    for (int k = 0; k < 4; ++k) add(ub[k], uw[k] * (1.0f - t));
    for (int k = 0; k < 4; ++k) add(vb[k], vw[k] * t);
    // top 4 by weight
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (acc[j].w > acc[i].w) std::swap(acc[i], acc[j]);
    float sum = 0;
    const int keep = std::min(4, n);
    for (int k = 0; k < keep; ++k) sum += acc[k].w;
    if (sum <= 1e-6f) { outB[0] = ub[0] >= 0 ? ub[0] : (vb[0] >= 0 ? vb[0] : 0); outW[0] = 1.0f; for (int k = 1; k < 4; ++k) { outB[k] = 0; outW[k] = 0.0f; } return; }
    for (int k = 0; k < 4; ++k) { outB[k] = 0; outW[k] = 0.0f; }
    for (int k = 0; k < keep; ++k) { outB[k] = acc[k].bone; outW[k] = acc[k].w / sum; }
}

} // namespace

SimplifyMeshStats SimplifyMesh(assemble::SceneMesh& mesh, const SimplifyOptions& opt,
                               const std::vector<assemble::SceneBone>* bones) {
    SimplifyMeshStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    stats.originalVertices = mesh.vertices.size();
    stats.originalTriangles = 0;
    for (const auto& sm : mesh.submeshes) stats.originalTriangles += sm.indices.size() / 3;
    stats.resultVertices = stats.originalVertices;
    stats.resultTriangles = stats.originalTriangles;
    if (mesh.vertices.empty() || stats.originalTriangles == 0 || opt.maxReduction <= 0.0f) {
        if (opt.maxReduction <= 0.0f) stats.warnings.clear();
        stats.seconds = 0.0;
        return stats;
    }
    const double clampedReduction = std::clamp(static_cast<double>(opt.maxReduction), 0.0, 0.95);
    const std::size_t totalFaces = stats.originalTriangles;
    const std::size_t targetFaces = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(totalFaces * (1.0 - clampedReduction))));

    // ---- working copy ---------------------------------------------------------
    std::vector<WFace> faces;
    faces.reserve(totalFaces);
    for (const auto& sm : mesh.submeshes) {
        const int sub = static_cast<int>(&sm - mesh.submeshes.data());
        for (std::size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
            WFace f;
            f.v[0] = static_cast<int>(sm.indices[i]);
            f.v[1] = static_cast<int>(sm.indices[i + 1]);
            f.v[2] = static_cast<int>(sm.indices[i + 2]);
            f.submesh = sub;
            faces.push_back(f);
        }
    }
    std::vector<WVertex> verts;
    verts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        WVertex w;
        w.pos = v.pos;
        w.normal = v.normal;
        for (int c = 0; c < 4; ++c) { w.uv[c] = c == 0 ? v.uv : c == 1 ? v.uv1 : c == 2 ? v.uv2 : v.uv3; }
        for (int k = 0; k < 4; ++k) { w.bone[k] = v.bone[k]; w.weight[k] = v.weight[k]; }
        verts.push_back(w);
    }
    std::vector<std::vector<int>> vertFaces(verts.size());
    for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
        faces[static_cast<size_t>(i)].alive = true;
        for (int k = 0; k < 3; ++k) vertFaces[static_cast<size_t>(faces[static_cast<size_t>(i)].v[k])].push_back(i);
    }

    // ---- twinned border chains (split UV / normal / material seams in the index topology) ----
    // A chain edge is used by exactly one face. Two chains at the same positions are two index-space sides of one geometric seam;
    // moving either side's vertices cracks it. Lock every border vertex that has a coincident twin somewhere else (twinned chains).
    // True open borders (no twin anywhere: a garment hem) stay collapsible - they cannot split from anything.
    std::unordered_map<uint64_t, int> boundaryEdgeCount;
    boundaryEdgeCount.reserve(faces.size() * 2);
    auto edgeKey2 = [](int a, int b) { if (a > b) std::swap(a, b); return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(b)); };
    for (const auto& f : faces) {
        ++boundaryEdgeCount[edgeKey2(f.v[0], f.v[1])];
        ++boundaryEdgeCount[edgeKey2(f.v[1], f.v[2])];
        ++boundaryEdgeCount[edgeKey2(f.v[2], f.v[0])];
    }
    std::vector<char> isBorder(verts.size(), 0);
    for (const auto& [key, count] : boundaryEdgeCount) {
        if (count != 1) continue;
        const int a = static_cast<int>(key >> 32), b = static_cast<int>(key & 0xffffffffull);
        isBorder[static_cast<size_t>(a)] = 1;
        isBorder[static_cast<size_t>(b)] = 1;
    }
    std::unordered_map<uint64_t, std::vector<int>> byCell;
    byCell.reserve(verts.size());
    const float quantise = 1e-4f;   // 0.1 mm: coincident split vertices land in one cell
    const auto cellKey = [&](Vec3 p) {
        const int64_t x = static_cast<int64_t>(std::floor(p.x / quantise));
        const int64_t y = static_cast<int64_t>(std::floor(p.y / quantise));
        const int64_t z = static_cast<int64_t>(std::floor(p.z / quantise));
        return (static_cast<uint64_t>(static_cast<uint32_t>(x * 73856093)) ^ (static_cast<uint64_t>(static_cast<uint32_t>(y * 19349663)) << 1) ^ (static_cast<uint64_t>(static_cast<uint32_t>(z * 83492791)) << 2));
    };
    for (int i = 0; i < static_cast<int>(isBorder.size()); ++i)
        if (isBorder[static_cast<size_t>(i)]) byCell[cellKey(verts[static_cast<size_t>(i)].pos)].push_back(i);
    // boundary cells shared by two or more distinct positions are conservative; what matters is size > 1
    std::vector<char> locked(verts.size(), 0);
    for (const auto& [cell, members] : byCell) {
        if (members.size() < 2) continue;
        for (int i : members) locked[static_cast<size_t>(i)] = 1;
    }

    // ---- initial quadrics -----------------------------------------------------
    const float secondary = opt.secondaryUVWeight;
    const float primary = opt.uvWeight;
    for (std::size_t fi = 0; fi < faces.size(); ++fi) {
        const WFace& f = faces[fi];
        const WVertex& a = verts[static_cast<size_t>(f.v[0])];
        const WVertex& b = verts[static_cast<size_t>(f.v[1])];
        const WVertex& c = verts[static_cast<size_t>(f.v[2])];
        const Vec3 e1 = sub3(b.pos, a.pos), e2 = sub3(c.pos, a.pos);
        const Vec3 n = cross3(e1, e2);
        const float area2 = lenSquared(n);
        if (area2 <= 1e-18f) continue;   // degenerate input face: carries no metric
        const float invLen = 1.0f / std::sqrt(area2);
        const Vec3 nn{n.x * invLen, n.y * invLen, n.z * invLen};
        const float d = -dot3(nn, a.pos);
        Quadric q = Quadric::fromPlane(nn, d);
        // attribute gradients: 8 components (4 UV sets x u,v), the face's own UV area bounding the error
        for (int set = 0; set < 4; ++set) {
            const double w = set == 0 ? primary : secondary;
            for (int comp = 0; comp < 2; ++comp) {
                const float c0 = comp == 0 ? a.uv[set].x : a.uv[set].y;
                const float c1 = comp == 0 ? b.uv[set].x : b.uv[set].y;
                const float c2 = comp == 0 ? c.uv[set].x : c.uv[set].y;
                bool ok = false;
                const Vec3 g = attributeGradient(a.pos, b.pos, c.pos, c0, c1, c2, ok);
                if (!ok) continue;
                q.add(Quadric::fromScatter(g, w, a.pos));
            }
        }
        for (int k = 0; k < 3; ++k) verts[static_cast<size_t>(f.v[k])].quadric.add(q);
    }

    // ---- the edge queue -------------------------------------------------------
    // Entries are never removed while queued (heap + stamps pick the live ones), so re-queueing
    // after a collapse just pushes fresh copies; stale pops are dropped via the stamp check.
    std::priority_queue<Candidate, std::vector<Candidate>, CandidateGreater> queue;
    auto pushEdge = [&](int u, int v) {
        if (u == v) return;
        if (u > v) std::swap(u, v);
        const WVertex& a = verts[static_cast<size_t>(u)];
        const WVertex& b = verts[static_cast<size_t>(v)];
        Quadric sum = a.quadric;
        sum.add(b.quadric);
        // The best collapse point of the three candidates Garland suggests, kept as the entry's cost.
        float best = kInf;
        const Vec3 mid{(a.pos.x + b.pos.x) * 0.5f, (a.pos.y + b.pos.y) * 0.5f, (a.pos.z + b.pos.z) * 0.5f};
        best = std::min(best, static_cast<float>(sum.evaluate(mid.x, mid.y, mid.z, 1.0)));
        best = std::min(best, static_cast<float>(sum.evaluate(a.pos.x, a.pos.y, a.pos.z, 1.0)));
        best = std::min(best, static_cast<float>(sum.evaluate(b.pos.x, b.pos.y, b.pos.z, 1.0)));
        Vec3 opt;
        if (sum.optimal(opt)) best = std::min(best, static_cast<float>(sum.evaluate(opt.x, opt.y, opt.z, 1.0)));
        queue.push({best, u, v, a.stamp, b.stamp});
    };
    for (const auto& f : faces) {
        pushEdge(f.v[0], f.v[1]);
        pushEdge(f.v[1], f.v[2]);
        pushEdge(f.v[2], f.v[0]);
    }

    std::size_t aliveFaces = totalFaces;
    std::size_t collapses = 0;
    std::size_t skippedLocked = 0, skippedFlip = 0, skippedDegenerate = 0;
    std::vector<CollapseRec> shapeLog;
    shapeLog.reserve(1 << 16);
    std::size_t sinceProgress = 0;

    auto materialMask = [&](int vi) {
        uint64_t mask = 0;
        for (int fi : vertFaces[static_cast<size_t>(vi)])
            if (faces[static_cast<size_t>(fi)].alive) {
                const int s = faces[static_cast<size_t>(fi)].submesh;
                if (s >= 0 && s < 63) mask |= 1ull << s;
                else mask |= 1ull << 63;
            }
        return mask;
    };
    auto edgeFaceCount = [&](int u, int v) {
        int count = 0;
        for (int fi : vertFaces[static_cast<size_t>(u)]) {
            const WFace& f = faces[static_cast<size_t>(fi)];
            if (!f.alive) continue;
            if ((f.v[0] == v || f.v[1] == v || f.v[2] == v)) ++count;
        }
        return count;
    };

    auto flipBroken = [&](int u, int v, const Vec3& x) {
        // Every face touching u or v must not turn more than flipDot away from its current normal.
        const auto check = [&](int vi) {
            const int other = vi == u ? v : u;
            for (int fi : vertFaces[static_cast<size_t>(vi)]) {
                const WFace& f = faces[static_cast<size_t>(fi)];
                if (!f.alive) continue;
                bool hasBoth = false;
                for (int k = 0; k < 3; ++k) if (f.v[k] == other) hasBoth = true;
                if (hasBoth) continue;   // this face is destroyed by the collapse; its normal is meaningless here
                Vec3 o[3];
                for (int k = 0; k < 3; ++k) o[k] = (f.v[k] == vi) ? x : verts[static_cast<size_t>(f.v[k])].pos;
                const Vec3 n = cross3(sub3(o[1], o[0]), sub3(o[2], o[0]));
                const Vec3 oldN = cross3(sub3(verts[static_cast<size_t>(f.v[1])].pos, verts[static_cast<size_t>(f.v[0])].pos),
                                         sub3(verts[static_cast<size_t>(f.v[2])].pos, verts[static_cast<size_t>(f.v[0])].pos));
                const float ll = lenSquared(n), ol = lenSquared(oldN);
                // A degenerate face (old or new) carries no orientation to protect: skip it instead of rejecting the collapse.
                if (ll <= 1e-18f || ol <= 1e-18f) continue;
                const float d = (n.x * oldN.x + n.y * oldN.y + n.z * oldN.z) / (std::sqrt(ll) * std::sqrt(ol));
                if (d < opt.flipDot) return true;
            }
            return false;
        };
        return check(u) || check(v);
    };

    auto collapsedQuadricSum = [&](int u, int v, Quadric& out) { out = verts[static_cast<size_t>(u)].quadric; out.add(verts[static_cast<size_t>(v)].quadric); };

    auto candidateCost = [&](const Quadric& q, Vec3 p) { return static_cast<float>(q.evaluate(p.x, p.y, p.z, 1.0)); };

    auto tryCollapse = [&](int u, int v, const Vec3& x, float& tOut) {
        const Vec3 seg = sub3(verts[static_cast<size_t>(v)].pos, verts[static_cast<size_t>(u)].pos);
        const float len2 = lenSquared(seg);
        float t = 0.0f;
        if (len2 > 1e-12f) t = std::clamp(dot3(sub3(x, verts[static_cast<size_t>(u)].pos), seg) / len2, 0.0f, 1.0f);
        tOut = t;
        return true;
    };

    while (!queue.empty() && aliveFaces > targetFaces) {
        Candidate c = queue.top();
        queue.pop();
        int u = c.u, v = c.v;
        if (!verts[static_cast<size_t>(u)].alive || !verts[static_cast<size_t>(v)].alive) continue;
        if (locked[static_cast<size_t>(u)] || locked[static_cast<size_t>(v)]) { ++skippedLocked; continue; }   // twinned border chain: pinned
        // A topology changed since this entry was pushed: recompute (and let a worse cost re-queue).
        if (verts[static_cast<size_t>(u)].stamp != c.stampU || verts[static_cast<size_t>(v)].stamp != c.stampV) {
            Quadric sumCheck;
            collapsedQuadricSum(u, v, sumCheck);
            const WVertex& aChk = verts[static_cast<size_t>(u)], & bChk = verts[static_cast<size_t>(v)];
            const Vec3 midChk{(aChk.pos.x + bChk.pos.x) * 0.5f, (aChk.pos.y + bChk.pos.y) * 0.5f, (aChk.pos.z + bChk.pos.z) * 0.5f};
            float bestChk = std::min({candidateCost(sumCheck, midChk), candidateCost(sumCheck, aChk.pos), candidateCost(sumCheck, bChk.pos)});
            Vec3 oChk;
            if (sumCheck.optimal(oChk)) bestChk = std::min(bestChk, candidateCost(sumCheck, oChk));
            if (bestChk > c.cost * 1.001f + 1e-9f) { queue.push({bestChk, u, v, aChk.stamp, bChk.stamp}); continue; }
        }
        const int edgeCount = edgeFaceCount(u, v);
        if (edgeCount == 0) continue;   // the edge is gone
        // Material seams: one material across the union, or the edge is on a boundary between materials.
        const uint64_t maskU = materialMask(u), maskV = materialMask(v);
        if ((maskU | maskV) != (maskU & maskV) && (maskU != 0 && maskV != 0)) { ++skippedLocked; continue; }
        if (opt.lockBorders && edgeCount == 1) { ++skippedLocked; continue; }   // open border in index space (a seam): keep it paired
        if (opt.lockBorders && (maskU != maskV)) { ++skippedLocked; continue; }  // a boundary between different submeshes
        // Duplicate-face rule (the link condition for surfaces): the collapse is invalid iff some pair {a, b} already forms
        // a face with u and would also form one with v — the result would carry two identical triangles.
        {
            auto faceKey = [&](const WFace& f, int own) {
                // unordered pair of the two vertices that are not `own`, packed
                int a = -1, b = -1;
                for (int k = 0; k < 3; ++k) if (f.v[k] != own) { if (a < 0) a = f.v[k]; else b = f.v[k]; }
                if (a > b) std::swap(a, b);
                return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(b));
            };
            std::unordered_set<uint64_t> pairs;
            for (int fi : vertFaces[static_cast<size_t>(u)]) {
                const WFace& f = faces[static_cast<size_t>(fi)];
                if (f.alive) pairs.insert(faceKey(f, u));
            }
            bool dup = false;
            for (int fi : vertFaces[static_cast<size_t>(v)]) {
                const WFace& f = faces[static_cast<size_t>(fi)];
                if (f.alive && pairs.count(faceKey(f, v))) { dup = true; break; }
            }
            if (dup) { ++skippedLocked; continue; }
        }
        // The collapse point: endpoints, midpoint and the optimal; cheapest wins; all must pass the flip test.
        Quadric sum;
        collapsedQuadricSum(u, v, sum);
        const WVertex& a = verts[static_cast<size_t>(u)], & b = verts[static_cast<size_t>(v)];
        const Vec3 mid{(a.pos.x + b.pos.x) * 0.5f, (a.pos.y + b.pos.y) * 0.5f, (a.pos.z + b.pos.z) * 0.5f};
        Vec3 optimal{};
        const bool hasOptimal = sum.optimal(optimal);
        Vec3 chosen = mid;
        float bestCost = candidateCost(sum, mid);
        for (const Vec3& cand : {a.pos, b.pos}) {
            const float cc = candidateCost(sum, cand);
            if (cc < bestCost) { bestCost = cc; chosen = cand; }
        }
        if (hasOptimal) {
            const float cc = candidateCost(sum, optimal);
            if (cc < bestCost) { bestCost = cc; chosen = optimal; }
        }
        if (flipBroken(u, v, chosen)) {
            // A cheaper candidate exists but is deforming; try the plainer ones before giving up.
            bool placed = false;
            for (const Vec3& cand : {mid, a.pos, b.pos})
                if (!flipBroken(u, v, cand)) { chosen = cand; placed = true; break; }
            if (!placed) { ++skippedFlip; continue; }
        }
        // ---- the collapse itself ---------------------------------------------------
        float t = 0.5f;
        tryCollapse(u, v, chosen, t);
        shapeLog.push_back({u, v, t});
        WVertex& A = verts[static_cast<size_t>(u)];
        const WVertex& B = verts[static_cast<size_t>(v)];
        A.pos = chosen;
        A.normal = normalize(lerp(A.normal, B.normal, t));
        for (int set = 0; set < 4; ++set) A.uv[set] = lerp2(A.uv[set], B.uv[set], t);
        blendWeights(A.bone, A.weight, B.bone, B.weight, t, A.bone, A.weight);
        A.quadric.add(B.quadric);
        A.stamp += 1;
        A.alive = true;
        verts[static_cast<size_t>(v)].alive = false;
        // Faces: v's become u's; faces holding both die; degenerate ones are dropped at the end.
        const std::vector<int> fu = vertFaces[static_cast<size_t>(u)];
        const std::vector<int> fv = vertFaces[static_cast<size_t>(v)];
        vertFaces[static_cast<size_t>(u)].clear();
        auto touch = [&](int fi) {
            WFace& f = faces[static_cast<size_t>(fi)];
            if (!f.alive) return;
            const bool hadU = (f.v[0] == u || f.v[1] == u || f.v[2] == u);
            const bool hadV = (f.v[0] == v || f.v[1] == v || f.v[2] == v);
            if (hadU && hadV) { f.alive = false; --aliveFaces; return; }
            if (hadV) for (int k = 0; k < 3; ++k) if (f.v[k] == v) f.v[k] = u;
            vertFaces[static_cast<size_t>(u)].push_back(fi);
        };
        for (int fi : fu) touch(fi);
        for (int fi : fv) touch(fi);
        // Re-queue the edges that changed. The v entries are skipped by the stamp check.
        for (int fi : vertFaces[static_cast<size_t>(u)]) {
            const WFace& f = faces[static_cast<size_t>(fi)];
            pushEdge(u, f.v[1]);
            pushEdge(u, f.v[2]);
            pushEdge(f.v[2], u);
        }
        ++collapses;
        ++sinceProgress;
        if (sinceProgress >= 256) {
            sinceProgress = 0;
            if (opt.cancelled && opt.cancelled()) { stats.cancelled = true; break; }
            if (opt.progress) {
                const float frac = 1.0f - static_cast<float>(aliveFaces) / static_cast<float>(totalFaces);
                opt.progress(std::clamp(frac, 0.0f, 1.0f), mesh.name.c_str());
            }
        }
    }

    stats.collapses = collapses;
    stats.skippedLocked = skippedLocked;
    stats.skippedFlip = skippedFlip;
    stats.skippedDegenerate = skippedDegenerate;

    // ---- final normals (area-weighted, seam-aware: split vertices stay split) ----
    std::vector<Vec3> normalAcc(verts.size(), Vec3{});
    for (const auto& f : faces) {
        if (!f.alive) continue;
        const Vec3 p0 = verts[static_cast<size_t>(f.v[0])].pos, p1 = verts[static_cast<size_t>(f.v[1])].pos, p2 = verts[static_cast<size_t>(f.v[2])].pos;
        const Vec3 n = cross3(sub3(p1, p0), sub3(p2, p0));
        for (int k = 0; k < 3; ++k) normalAcc[static_cast<size_t>(f.v[k])] = normalAcc[static_cast<size_t>(f.v[k])] + n;
    }
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const Vec3 n = normalAcc[i];
        if (lenSquared(n) > 1e-18f) verts[i].normal = normalize(n);
    }

    // ---- collapse log replay onto the blend shapes -----------------------------
    std::vector<std::unordered_map<int, Vec3>> shapeOffsets;
    shapeOffsets.resize(mesh.shapes.size());
    for (std::size_t si = 0; si < mesh.shapes.size(); ++si) {
        auto& off = shapeOffsets[si];
        for (const auto& [vi, d] : mesh.shapes[si].offsets) off[static_cast<int>(vi)] = d;
        for (const CollapseRec& rec : shapeLog) {
            const auto itV = off.find(rec.v);
            Vec3 dv{};
            if (itV != off.end()) { dv = itV->second; off.erase(itV); }
            const auto itU = off.find(rec.u);
            const Vec3 du = itU == off.end() ? Vec3{} : itU->second;
            const Vec3 merged{du.x * (1.0f - rec.t) + dv.x * rec.t, du.y * (1.0f - rec.t) + dv.y * rec.t, du.z * (1.0f - rec.t) + dv.z * rec.t};
            if (lenSquared(merged) > 1e-12f) off[rec.u] = merged;
            else if (itU != off.end()) off.erase(itU);
        }
    }

    // ---- compaction -------------------------------------------------------------
    std::vector<int> remap(verts.size(), -1);
    std::size_t newCount = 0;
    for (std::size_t i = 0; i < verts.size(); ++i) if (verts[i].alive) remap[i] = static_cast<int>(newCount++);
    if (stats.cancelled) { stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); return stats; }   // caller discards the copy

    // rebuild the vertices: positions, skinning, bind frames and normals
    mesh.vertices.clear();
    mesh.vertices.reserve(newCount);
    const bool wantLocal = bones && mesh.reshapable;   // non-reshapable meshes carry no bind frames
    std::unordered_map<int, Mat4> invBone;   // lazy per-bone inverses of the rest worlds
    static const Mat4 kIdentity;
    const auto invWorld = [&](int bone) -> const Mat4& {
        if (!bones || bone < 0 || static_cast<size_t>(bone) >= bones->size()) return kIdentity;
        auto it = invBone.find(bone);
        if (it == invBone.end()) it = invBone.emplace(bone, inverse((*bones)[static_cast<size_t>(bone)].world)).first;
        return it->second;
    };
    std::vector<assemble::SceneVertex> outVerts;
    outVerts.reserve(newCount);
    for (std::size_t i = 0; i < verts.size(); ++i) {
        if (!verts[i].alive) continue;
        const WVertex& w = verts[i];
        assemble::SceneVertex v;
        v.pos = w.pos;
        v.normal = w.normal;
        v.uv = w.uv[0];
        v.uv1 = w.uv[1];
        v.uv2 = w.uv[2];
        v.uv3 = w.uv[3];
        float wsum = 0;
        for (int k = 0; k < 4; ++k) wsum += w.weight[k];
        if (wsum <= 1e-6f) { for (int k = 0; k < 4; ++k) { v.bone[k] = 0; v.weight[k] = 0; } v.weight[0] = 1.0f; }
        else for (int k = 0; k < 4; ++k) { v.bone[k] = w.bone[k]; v.weight[k] = w.weight[k] / wsum; }
        if (wantLocal) {
            for (int k = 0; k < 4; ++k) {
                const Mat4& inv = invWorld(v.bone[k]);
                v.local[k] = inv.point(v.pos);
                v.nlocal[k] = inv.dir(v.normal);
            }
        }
        outVerts.push_back(std::move(v));
    }
    mesh.vertices = std::move(outVerts);
    const bool keepReshapable = wantLocal;
    for (auto& m : mesh.submeshes) m.indices.clear();
    auto statsSkipped = [&]() { ++skippedDegenerate; };
    for (const auto& f : faces) {
        if (!f.alive) continue;
        const Vec3 p0 = verts[static_cast<size_t>(f.v[0])].pos, p1 = verts[static_cast<size_t>(f.v[1])].pos, p2 = verts[static_cast<size_t>(f.v[2])].pos;
        if (lenSquared(cross3(sub3(p1, p0), sub3(p2, p0))) <= 1e-16f) { statsSkipped(); continue; }
        const int a = remap[static_cast<size_t>(f.v[0])], b = remap[static_cast<size_t>(f.v[1])], c = remap[static_cast<size_t>(f.v[2])];
        if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) { statsSkipped(); continue; }
        mesh.submeshes[static_cast<size_t>(f.submesh)].indices.push_back(static_cast<uint32_t>(a));
        mesh.submeshes[static_cast<size_t>(f.submesh)].indices.push_back(static_cast<uint32_t>(b));
        mesh.submeshes[static_cast<size_t>(f.submesh)].indices.push_back(static_cast<uint32_t>(c));
    }
    // blend shapes back onto the compacted vertices
    for (std::size_t si = 0; si < mesh.shapes.size(); ++si) {
        assemble::SceneBlendShape& sh = mesh.shapes[si];
        std::vector<std::pair<uint32_t, Vec3>> offsets;
        for (const auto& [oldVi, d] : shapeOffsets[si]) {
            const int ni = remap[static_cast<size_t>(oldVi)];
            if (ni < 0) continue;   // merged into a survivor and recorded there instead
            offsets.emplace_back(static_cast<uint32_t>(ni), d);
        }
        std::sort(offsets.begin(), offsets.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        sh.offsets = std::move(offsets);
        if (wantLocal) {
            std::vector<std::array<Vec3, 4>> local;
            local.reserve(sh.offsets.size());
            for (const auto& [ni, d] : sh.offsets) {
                const assemble::SceneVertex& v = mesh.vertices[ni];
                std::array<Vec3, 4> slot{};
                for (int k = 0; k < 4; ++k) if (v.weight[k] > 0) slot[static_cast<size_t>(k)] = invWorld(v.bone[k]).dir(d);
                local.push_back(slot);
            }
            sh.local = std::move(local);
        } else {
            sh.local.clear();
        }
    }

    // indices are consistent; recount and finish
    std::size_t outTris = 0;
    for (const auto& sm : mesh.submeshes) outTris += sm.indices.size() / 3;
    mesh.reshapable = keepReshapable;
    stats.resultTriangles = outTris;
    stats.resultVertices = mesh.vertices.size();
    stats.skippedDegenerate = skippedDegenerate;
    if (aliveFaces > targetFaces && !stats.cancelled)
        stats.warnings.push_back("mesh '" + mesh.name + "': could not reduce below " + std::to_string(aliveFaces) +
                                 " triangles (open UV / normal / material boundaries are locked); the requested " + std::to_string(targetFaces) + " was not reachable");
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (opt.progress && !stats.cancelled) opt.progress(1.0f, mesh.name.c_str());
    return stats;
}

} // namespace cardbridge::mesh
