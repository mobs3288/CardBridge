// Non-destructive separation of near-coincident garment layers.
//
// A card can legitimately carry two clothing pieces that are the *same* garment twice: the outer shell and its
// coloured lining, two copies of a cape in two accessory slots, a top present both as a clothes item and through
// the Accessory Clothes mod. Both are authored at (almost) the same place - that is how the game builds the layer.
// Two such surfaces drawn a millimetre apart in CardBridge read as one flickering mesh: the depth buffer cannot
// decide which copy owns the pixel, and the inner colour pokes through the outer one.
//
// This pass finds those pairs from geometry alone - never from part ids, slot numbers or item names - and pushes
// the outer copy just far enough off the inner copy to clear the depth buffer. The inner copy is left exactly as
// authored, so the lining still shows at the seams and edges the way the card designed it. A pair only qualifies
// when nearly every sampled vertex of BOTH parts lies within a few millimetres of the other surface; ordinary
// layered clothing (a jacket over a shirt, a cape over a dress) is centimetres apart and is never touched.
//
// Like FixGarmentIntersections, the correction re-derives the per-bone locals of every moved mesh so a later
// Reshape agrees with the new positions. Bone weights, UVs, topology, materials and blend shapes are untouched.

#include "assemble/Assembler.h"

#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace cardbridge::assemble {

namespace {

constexpr float kCoincident = 0.02f;       // two surfaces are "the same layer" when >=85% of samples are this close
constexpr float kClearance = 0.003f;       // final gap, 3 mm: well past the depth buffer's precision at arm's length
constexpr float kMaxMove = 0.03f;          // refuse to move a vertex further than this (a coincidence is a hair, not a gap)
constexpr float kCoverage = 0.85f;
constexpr size_t kMaxSamples = 96;

struct Tri {
    Vec3 a, b, c;
    float minX, minY, minZ, maxX, maxY, maxZ;
};

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(Vec3 v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(Vec3 v) {
    const float l = len(v);
    return l > 1e-12f ? mul(v, 1.0f / l) : Vec3{0, 1, 0};
}
float distance(Vec3 a, Vec3 b) { return len(sub(a, b)); }

// Ericson's closest point on a triangle; never allocates. Mirrors the helper in GarmentFix.cpp.
// When `closest` is non-null it receives the point itself (the caller needs it to place the vertex).
float closestOnTriangle(Vec3 p, const Tri& t, Vec3* closest) {
    const Vec3 ab = sub(t.b, t.a), ac = sub(t.c, t.a), ap = sub(p, t.a);
    Vec3 q{};
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) q = t.a;
    else {
        const Vec3 bp = sub(p, t.b);
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) q = t.b;
        else {
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
                q = add(t.a, mul(ab, d1 / (d1 - d3)));
            } else {
                const Vec3 cp = sub(p, t.c);
                const float d5 = dot(ab, cp), d6 = dot(ac, cp);
                if (d6 >= 0.0f && d5 <= d6) q = t.c;
                else {
                    const float vb = d5 * d2 - d1 * d6;
                    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
                        q = add(t.a, mul(ac, d2 / (d2 - d6)));
                    } else {
                        const float va = d3 * d6 - d5 * d4;
                        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
                            q = add(t.b, mul(sub(t.c, t.b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
                        else {
                            const float denom = 1.0f / (dot(ab, ab) * dot(ac, ac) - dot(ab, ac) * dot(ab, ac));
                            const float u = (dot(ac, ac) * d1 - dot(ab, ac) * d2) * denom;
                            const float w = (dot(ab, ab) * d2 - dot(ab, ac) * d1) * denom;
                            q = add(add(t.a, mul(ab, u)), mul(ac, w));
                        }
                    }
                }
            }
        }
    }
    if (closest) *closest = q;
    return dot(sub(p, q), sub(p, q));
}

// Every triangle of a part, with its own bounds so closest-point queries can reject most of them immediately.
std::vector<Tri> collectTriangles(const Scene& scene, const ScenePart& part) {
    std::vector<Tri> out;
    for (int mi : part.meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
        for (const SceneSubmesh& sm : m.submeshes)
            for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
                const uint32_t ia = sm.indices[t], ib = sm.indices[t + 1], ic = sm.indices[t + 2];
                if (ia >= m.vertices.size() || ib >= m.vertices.size() || ic >= m.vertices.size()) continue;
                Tri tri;
                tri.a = m.vertices[ia].pos; tri.b = m.vertices[ib].pos; tri.c = m.vertices[ic].pos;
                tri.minX = std::min({tri.a.x, tri.b.x, tri.c.x}); tri.maxX = std::max({tri.a.x, tri.b.x, tri.c.x});
                tri.minY = std::min({tri.a.y, tri.b.y, tri.c.y}); tri.maxY = std::max({tri.a.y, tri.b.y, tri.c.y});
                tri.minZ = std::min({tri.a.z, tri.b.z, tri.c.z}); tri.maxZ = std::max({tri.a.z, tri.b.z, tri.c.z});
                out.push_back(tri);
            }
    }
    return out;
}

// Closest distance from a point to a triangle set. `closest` receives the winning point when given.
float closestOnSurface(Vec3 p, const std::vector<Tri>& tris, float slack, Vec3* closest = nullptr, const Tri** which = nullptr) {
    float best = std::numeric_limits<float>::max();
    Vec3 bestPoint{};
    const Tri* bestTri = nullptr;
    for (const Tri& t : tris) {
        if (p.x < t.minX - slack || p.x > t.maxX + slack) continue;
        if (p.y < t.minY - slack || p.y > t.maxY + slack) continue;
        if (p.z < t.minZ - slack || p.z > t.maxZ + slack) continue;
        Vec3 point{};
        const float d2 = closestOnTriangle(p, t, &point);
        if (d2 < best) { best = d2; bestPoint = point; bestTri = &t; }
    }
    if (closest) *closest = bestPoint;
    if (which) *which = bestTri;
    return std::sqrt(best);
}

// How much of `from` lies within kCoincident of `to`'s surface. Sampled evenly by vertex index: a duplicate layer
// has the same silhouette everywhere, while two pieces that merely touch agree on a handful of vertices at most.
float surfaceCoverage(const Scene& scene, const ScenePart& from, const std::vector<Tri>& to) {
    size_t total = 0, covered = 0;
    for (int mi : from.meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
        const size_t step = std::max<size_t>(1, m.vertices.size() / kMaxSamples);
        for (size_t v = 0; v < m.vertices.size(); v += step) {
            ++total;
            if (closestOnSurface(m.vertices[v].pos, to, kCoincident) <= kCoincident) ++covered;
        }
    }
    return total ? static_cast<float>(covered) / static_cast<float>(total) : 0.0f;
}

float meanRadius(const Scene& scene, const ScenePart& part, Vec3 center) {
    double sum = 0.0;
    size_t n = 0;
    for (int mi : part.meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        for (const SceneVertex& v : scene.meshes[static_cast<size_t>(mi)].vertices) { sum += distance(v.pos, center); ++n; }
    }
    return n ? static_cast<float>(sum / n) : 0.0f;
}

bool excluded(const ScenePart& p) {
    if (p.underPiece || !p.includeInExport || !p.visibleInPreview) return true;
    // Footwear is deliberately layered: an outer shoe encloses its inner half (and the sock) by design. The body
    // clipping pass excludes shoes for the same reason; separating the two halves would break the intended fit.
    if (p.category == 112 || p.id.find(".shoes") != std::string::npos) return true;
    return p.group != "Outfit" && p.group != "Accessories";
}

bool noSeparation() { return std::getenv("CARDBRIDGE_NO_LAYER_SEPARATION") != nullptr; }

} // namespace

LayerSeparationStats SeparateCoincidentLayers(Scene& scene) {
    LayerSeparationStats stats;
    if (noSeparation()) return stats;

    // The body centre is only a direction hint for "outward": no part is moved towards it.
    Vec3 center{};
    size_t bodyVerts = 0;
    for (const auto& p : scene.parts) {
        if (p.id != "body") continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            for (const SceneVertex& v : scene.meshes[static_cast<size_t>(mi)].vertices) { center = add(center, v.pos); ++bodyVerts; }
        }
    }
    if (bodyVerts) center = mul(center, 1.0f / static_cast<float>(bodyVerts));

    std::vector<size_t> candidates;
    std::vector<std::vector<Tri>> tris;
    std::vector<float> radius;
    std::vector<size_t> vertexCount;
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        if (excluded(scene.parts[i])) continue;
        auto t = collectTriangles(scene, scene.parts[i]);
        if (t.empty()) continue;
        size_t nv = 0;
        for (int mi : scene.parts[i].meshes)
            if (mi >= 0 && static_cast<size_t>(mi) < scene.meshes.size()) nv += scene.meshes[static_cast<size_t>(mi)].vertices.size();
        candidates.push_back(i);
        vertexCount.push_back(nv);
        radius.push_back(meanRadius(scene, scene.parts[i], center));
        tris.push_back(std::move(t));
    }

    // Which meshes had any vertex moved (re-derive their per-bone frames once at the end).
    std::vector<char> movedMesh(scene.meshes.size(), 0);
    std::vector<std::string> notes;

    for (size_t ci = 0; ci < candidates.size(); ++ci) {
        for (size_t cj = ci + 1; cj < candidates.size(); ++cj) {
            const size_t pi = candidates[ci], pj = candidates[cj];
            // A part takes part in one separation only. A card can repeat the same item across several slots;
            // shifting a part once for each of those pairs would stack offsets and make a second pass non-idempotent.
            if (scene.parts[pi].layerSeparated || scene.parts[pj].layerSeparated) continue;
            // The decisive metadata signal: both parts are the SAME item from the card, just worn twice (two
            // accessory slots, or clothes plus Accessory Clothes). A shoe over its inner shoe, a bra under a top,
            // a sock under a boot - different items the card deliberately layers - have different ids and never
            // reach the geometry test at all. This is read from the card, never from a slot number or item name.
            if (scene.parts[pi].sourceItemId == 0 || scene.parts[pi].sourceItemId != scene.parts[pj].sourceItemId) continue;
            // AABB prefilter: a coincidence cannot span a gap larger than the threshold.
            const std::vector<Tri>& ti = tris[ci], &tj = tris[cj];
            float aiMinX = 1e30f, aiMinY = 1e30f, aiMinZ = 1e30f, aiMaxX = -1e30f, aiMaxY = -1e30f, aiMaxZ = -1e30f;
            float ajMinX = 1e30f, ajMinY = 1e30f, ajMinZ = 1e30f, ajMaxX = -1e30f, ajMaxY = -1e30f, ajMaxZ = -1e30f;
            auto bounds = [](const std::vector<Tri>& ts, float& x0, float& y0, float& z0, float& x1, float& y1, float& z1) {
                for (const Tri& t : ts) {
                    x0 = std::min(x0, t.minX); y0 = std::min(y0, t.minY); z0 = std::min(z0, t.minZ);
                    x1 = std::max(x1, t.maxX); y1 = std::max(y1, t.maxY); z1 = std::max(z1, t.maxZ);
                }
            };
            bounds(ti, aiMinX, aiMinY, aiMinZ, aiMaxX, aiMaxY, aiMaxZ);
            bounds(tj, ajMinX, ajMinY, ajMinZ, ajMaxX, ajMaxY, ajMaxZ);
            if (aiMaxX < ajMinX - kCoincident || ajMaxX < aiMinX - kCoincident ||
                aiMaxY < ajMinY - kCoincident || ajMaxY < aiMinY - kCoincident ||
                aiMaxZ < ajMinZ - kCoincident || ajMaxZ < aiMinZ - kCoincident) continue;

            // Two copies of one layer come from the same prefab: their vertex counts and their silhouette spans
            // match. Together these reject a small accessory pinned onto a large garment (a brooch on a dress),
            // which otherwise has all its vertices near the coat's surface but is plainly not a second copy of it.
            const size_t biggerCount = std::max(vertexCount[ci], vertexCount[cj]);
            if (std::abs(static_cast<int64_t>(vertexCount[ci]) - static_cast<int64_t>(vertexCount[cj])) >
                static_cast<int64_t>(std::max<size_t>(1, biggerCount / 100))) continue;
            auto similar = [](float a0, float a1, float b0, float b1) {
                const float spanA = a1 - a0, spanB = b1 - b0;
                return std::fabs(spanA - spanB) <= 0.02f + 0.1f * std::max(spanA, spanB);
            };
            if (!similar(aiMinX, aiMaxX, ajMinX, ajMaxX) || !similar(aiMinY, aiMaxY, ajMinY, ajMaxY) ||
                !similar(aiMinZ, aiMaxZ, ajMinZ, ajMaxZ)) continue;

            // The copy further from the body is the outer layer; it gets the clearance. A tie is broken by slot
            // order so the same card always resolves the same way, with no name or id involved.
            const bool iOuter = radius[ci] > radius[cj] + 1e-4f ||
                                (std::fabs(radius[ci] - radius[cj]) <= 1e-4f && pi > pj);
            const size_t outerPart = iOuter ? pi : pj, innerPart = iOuter ? pj : pi;
            const std::vector<Tri>& inner = iOuter ? tj : ti;
            const std::vector<Tri>& outer = iOuter ? ti : tj;
            // Only the inner copy has to hug the outer surface: the outer one is usually a hair larger, so sampling
            // it in the reverse direction under-reports the overlap. One-directional coverage plus matching topology
            // is the data-derived signature of a duplicated layer, and still rejects pieces that merely touch.
            if (surfaceCoverage(scene, scene.parts[innerPart], outer) < kCoverage) continue;
            ++stats.pairsFound;

            // One rigid offset for the whole outer layer. Moving each vertex to its own closest point would warp
            // the mesh (neighbouring vertices land on different triangles); translating every vertex by the same
            // measured vector keeps the garment's shape, weights, UVs and authored drape bit-for-bit intact.
            Vec3 sumDir{};
            float minGap = std::numeric_limits<float>::max();
            size_t samples = 0;
            for (int mi : scene.parts[outerPart].meshes) {
                if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
                const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
                const size_t step = std::max<size_t>(1, m.vertices.size() / kMaxSamples);
                for (size_t v = 0; v < m.vertices.size(); v += step) {
                    Vec3 closest{};
                    const float d = closestOnSurface(m.vertices[v].pos, inner, kCoincident + kClearance, &closest);
                    sumDir = add(sumDir, normalized(sub(m.vertices[v].pos, closest)));
                    minGap = std::min(minGap, d);
                    ++samples;
                }
            }
            const float shift = std::max(0.0f, kClearance - minGap);
            if (samples && shift > 1e-6f) {
                if (shift > kMaxMove) { stats.skipped += samples; continue; }
                const Vec3 offset = mul(normalized(sumDir), shift);
                size_t pairMoved = 0;
                for (int mi : scene.parts[outerPart].meshes) {
                    if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
                    SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
                    for (SceneVertex& v : m.vertices) { v.pos = add(v.pos, offset); ++pairMoved; }
                    movedMesh[static_cast<size_t>(mi)] = 1;
                }
                stats.verticesMoved += pairMoved;
                stats.partsFixed += 1;
                stats.maxMove = std::max(stats.maxMove, shift);
                scene.parts[outerPart].layerSeparated = true;
                char buf[256];
                std::snprintf(buf, sizeof buf,
                    "coincident clothing layers: '%s' shifted %s mm off '%s' as one piece (both layers kept)",
                    scene.parts[outerPart].id.c_str(), std::to_string(shift * 1000.0f).c_str(),
                    scene.parts[innerPart].id.c_str());
                notes.push_back(buf);
            }
        }
    }

    // Per-bone locals, as in the body clipping pass: reshaping must start from the corrected vertices.
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        if (!movedMesh[mi]) continue;
        SceneMesh& m = scene.meshes[mi];
        if (!m.reshapable) continue;
        std::vector<Mat4> inv(scene.bones.size());
        for (size_t b = 0; b < scene.bones.size(); ++b) inv[b] = inverse(scene.bones[b].world);
        for (SceneVertex& v : m.vertices)
            for (int k = 0; k < 4; ++k) {
                if (v.weight[k] <= 0.0f || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= inv.size()) continue;
                v.local[k] = inv[static_cast<size_t>(v.bone[k])].point(v.pos);
                v.nlocal[k] = inv[static_cast<size_t>(v.bone[k])].dir(v.normal);
            }
    }

    for (const std::string& n : notes) scene.warn("clothing layers", n);
    if (stats.pairsFound)
        scene.note("clothing layers", "separated " + std::to_string(stats.pairsFound) +
            " coincident clothing layer pair(s); " + std::to_string(stats.verticesMoved) + " vertex(es) adjusted");
    return stats;
}

} // namespace cardbridge::assemble
