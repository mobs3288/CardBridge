// Optional, non-destructive clipping correction for outfit garments that cross into the body.
//
// Koikatsu outfit meshes are modelled a hair apart from the body; a card whose body sliders reshape the
// body by a millimetre past the garment's authored surface buries garment vertices in the skin (the skin
// shows through the cloth). The pass below closes exactly that gap and nothing else:
//   - only vertices that pierce the body's surface, or lie within a fingernail of it, are moved;
//   - every one is pushed out along the body's own surface normal by its own penetration depth, plus a small clearance;
//   - everything else (bone weights, UVs, topology, blend shapes, every vertex further away) is untouched.
//
// The correction re-derives the per-bone locals Reshape consumes for the moved vertices, so re-shaping afterwards agrees.
//
// Exclusions (documented, by role rather than by asset name): the body itself, the hidden "under pieces"
// (the skin kept for the nude morphs), footwear (a shoe encloses the foot), the Head and the Hair group.
// The pass is opt-in; CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=0 keeps it off even for a caller that turns it on.

#include "assemble/Assembler.h"

#include "core/Math.h"
#include "core/Timing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cardbridge::assemble {

namespace {

struct Tri {
    Vec3 a, b, c;
    Vec3 n;         // unit face normal, oriented away from the body centroid
    float minX, minY, minZ, maxX, maxY, maxZ;
};

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(Vec3 v) { return std::sqrt(dot(v, v)); }

// Ericson's closest point on a triangle; never allocates.
void closestOnTriangle(Vec3 p, const Tri& t, Vec3& closest, float& distSq) {
    const Vec3 ab = sub(t.b, t.a), ac = sub(t.c, t.a), ap = sub(p, t.a);
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { closest = t.a; distSq = dot(sub(p, t.a), sub(p, t.a)); return; }
    const Vec3 bp = sub(p, t.b);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { closest = t.b; distSq = dot(sub(p, t.b), sub(p, t.b)); return; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        closest = add(t.a, mul(ab, v));
        distSq = dot(sub(p, closest), sub(p, closest));
        return;
    }
    const Vec3 cp = sub(p, t.c);
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) { closest = t.c; distSq = dot(sub(p, t.c), sub(p, t.c)); return; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        closest = add(t.a, mul(ac, w));
        distSq = dot(sub(p, closest), sub(p, closest));
        return;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        closest = add(t.b, mul(sub(t.c, t.b), w));
        distSq = dot(sub(p, closest), sub(p, closest));
        return;
    }
    const float denom = 1.0f / (dot(ab, ab) * dot(ac, ac) - dot(ab, ac) * dot(ab, ac));
    const float v = (dot(ac, ac) * d1 - dot(ab, ac) * d2) * denom;
    const float w = (dot(ab, ab) * d2 - dot(ab, ac) * d1) * denom;
    closest = add(add(t.a, mul(ab, v)), mul(ac, w));
    distSq = dot(sub(p, closest), sub(p, closest));
}

// Moller's ray-triangle intersection for the inside/outside parity test. The direction is deliberately tilted off-axis: a ray
// straight up passes exactly through pole vertices shared by many triangles (the parity count doubles and the vertex reads as
// outside). Any fixed generic direction is measure-zero against meshes that are not built on a lattice.
bool rayHitsTri(Vec3 origin, const Tri& t) {
    const Vec3 e1 = sub(t.b, t.a), e2 = sub(t.c, t.a);
    const Vec3 dir{0.213271f, 1.0f, 0.371903f};
    const Vec3 p = cross(dir, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vec3 tv = sub(origin, t.a);
    const float u = dot(tv, p) * inv;
    const Vec3 q = cross(tv, e1);
    const float v = dot(dir, q) * inv;
    const float tDist = dot(e2, q) * inv;
    const bool hit = u >= -1e-6f && v >= -1e-6f && (u + v) <= 1.0f + 1e-6f && tDist > 1e-6f;
    return hit;
}

bool pointInsideBody(Vec3 p, const std::vector<Tri>& body) {
    int hits = 0;
    for (const Tri& t : body) if (rayHitsTri(p, t)) ++hits;
    return (hits % 2) == 1;
}

bool partExcluded(const ScenePart& p) {
    if (p.underPiece) return true;                 // hidden skin kept for the nude morphs
    if (p.id == "body") return true;               // the body itself is the reference
    if (p.id.find(".shoes") != std::string::npos) return true;   // a shoe encloses the foot by design
    if (p.group == "Head" || p.group == "Hair") return true;     // the face is not an outfit seam
    return false;
}

} // namespace

GarmentFixStats FixGarmentIntersections(Scene& scene, float clearance) {
    GarmentFixStats stats;
    const char* env = std::getenv("CARDBRIDGE_FIX_GARMENT_INTERSECTIONS");
    if (env && std::string(env) == "0") return stats;   // a caller may force it off even when it enables it on purpose

    // --- the body's triangles are the reference ---
    std::vector<int> bodyParts;
    for (size_t i = 0; i < scene.parts.size(); ++i)
        if (scene.parts[i].id == "body" && !scene.parts[i].meshes.empty()) bodyParts.push_back(static_cast<int>(i));
    std::vector<Tri> body;
    body.reserve(16000);
    Vec3 centroid{0, 0, 0};
    for (int pi : bodyParts) {
        for (int mi : scene.parts[static_cast<size_t>(pi)].meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (const SceneSubmesh& sm : m.submeshes) {
                for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
                    const uint32_t ia = sm.indices[t], ib = sm.indices[t + 1], ic = sm.indices[t + 2];
                    if (ia >= m.vertices.size() || ib >= m.vertices.size() || ic >= m.vertices.size()) continue;
                    Tri tri;
                    tri.a = m.vertices[ia].pos;
                    tri.b = m.vertices[ib].pos;
                    tri.c = m.vertices[ic].pos;
                    tri.n = cross(sub(tri.b, tri.a), sub(tri.c, tri.a));
                    const float nl = len(tri.n);
                    tri.n = nl > 1e-12f ? mul(tri.n, 1.0f / nl) : Vec3{0, 1, 0};
                    tri.minX = std::min({tri.a.x, tri.b.x, tri.c.x}); tri.maxX = std::max({tri.a.x, tri.b.x, tri.c.x});
                    tri.minY = std::min({tri.a.y, tri.b.y, tri.c.y}); tri.maxY = std::max({tri.a.y, tri.b.y, tri.c.y});
                    tri.minZ = std::min({tri.a.z, tri.b.z, tri.c.z}); tri.maxZ = std::max({tri.a.z, tri.b.z, tri.c.z});
                    body.push_back(tri);
                    centroid = add(centroid, add(add(tri.a, tri.b), tri.c));
                }
            }
        }
    }
    if (body.empty()) return stats;
    centroid = mul(centroid, 1.0f / (3.0f * static_cast<float>(body.size())));
    // Orient the face normals away from the body centroid: the body is a closed skin, outward is away from the middle.
    for (Tri& t : body) {
        const Vec3 mid = mul(add(add(t.a, t.b), t.c), 1.0f / 3.0f);
        if (dot(t.n, sub(mid, centroid)) < 0.0f) t.n = mul(t.n, -1.0f);
    }

    float bodyMinX = 1e30f, bodyMinY = 1e30f, bodyMinZ = 1e30f, bodyMaxX = -1e30f, bodyMaxY = -1e30f, bodyMaxZ = -1e30f;
    for (const Tri& t : body) {
        bodyMinX = std::min(bodyMinX, t.minX); bodyMaxX = std::max(bodyMaxX, t.maxX);
        bodyMinY = std::min(bodyMinY, t.minY); bodyMaxY = std::max(bodyMaxY, t.maxY);
        bodyMinZ = std::min(bodyMinZ, t.minZ); bodyMaxZ = std::max(bodyMaxZ, t.maxZ);
    }

    // --- candidate garments and accessories ---
    std::vector<int> candidates;
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const ScenePart& p = scene.parts[i];
        if (partExcluded(p)) continue;
        if (!(p.group == "Outfit" || p.group == "Accessories")) continue;
        candidates.push_back(static_cast<int>(i));
    }

    std::vector<std::pair<int, std::pair<size_t, float>>> partStats;   // part, (moved, maxMove)
    for (int pi : candidates) {
        ScenePart& part = scene.parts[static_cast<size_t>(pi)];
        size_t partMoved = 0;
        float partMax = 0.0f;
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
            for (size_t vi = 0; vi < m.vertices.size(); ++vi) {
                SceneVertex& v = m.vertices[vi];
                // quick reject: the body's overall bounds, expanded by the clearance
                const float slack = clearance + 1e-4f;
                if (v.pos.x < bodyMinX - slack || v.pos.x > bodyMaxX + slack) continue;
                if (v.pos.y < bodyMinY - slack || v.pos.y > bodyMaxY + slack) continue;
                if (v.pos.z < bodyMinZ - slack || v.pos.z > bodyMaxZ + slack) continue;
                Vec3 bestClosest;
                float bestDistSq = std::numeric_limits<float>::max();
                const Tri* bestTri = nullptr;
                for (const Tri& t : body) {
                    if (v.pos.x < t.minX - slack || v.pos.x > t.maxX + slack) continue;
                    if (v.pos.y < t.minY - slack || v.pos.y > t.maxY + slack) continue;
                    if (v.pos.z < t.minZ - slack || v.pos.z > t.maxZ + slack) continue;
                    Vec3 closest;
                    float dsq;
                    closestOnTriangle(v.pos, t, closest, dsq);
                    if (dsq < bestDistSq) { bestDistSq = dsq; bestClosest = closest; bestTri = &t; }
                }
                if (!bestTri) continue;
                const float dist = std::sqrt(bestDistSq);
                // a vertex deep inside the body is far from the surface; only a parity test can tell it from the air outside
                const bool inside = pointInsideBody(v.pos, body);
                if (dist >= clearance && !inside) continue;   // outside with clearance to spare
                Vec3 dir;
                if (inside) {
                    dir = bestTri->n;                          // straight out of the body along the surface
                } else {
                    dir = sub(v.pos, bestClosest);
                    const float l = len(dir);
                    dir = l > 1e-12f ? mul(dir, 1.0f / l) : bestTri->n;
                }
                Vec3 target = add(bestClosest, mul(dir, clearance));
                if (!inside && dist < clearance) target = add(bestClosest, mul(dir, clearance));
                Vec3 moveVec = sub(target, v.pos);
                const float moveLen = len(moveVec);
                if (moveLen <= 1e-9f) continue;
                if (moveLen > 0.5f) { ++stats.skipped; continue; }   // something is badly off; leave it alone and report nothing moved there
                v.pos = target;
                v.normal = normalize(v.normal);
                partMoved += 1;
                partMax = std::max(partMax, moveLen);
                stats.verticesMoved += 1;
                stats.maxMove = std::max(stats.maxMove, moveLen);
            }
            // re-derive the per-bone locals Reshape consumes, so the moved vertices agree with their bones after a re-shape
            if (partMoved > 0 && m.reshapable) {
                std::vector<Mat4> inv(scene.bones.size());
                for (size_t i = 0; i < scene.bones.size(); ++i) inv[i] = inverse(scene.bones[i].world);
                for (SceneVertex& v : m.vertices) {
                    for (int k = 0; k < 4; ++k) {
                        if (v.weight[k] <= 0.0f || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= inv.size()) continue;
                        v.local[k] = inv[static_cast<size_t>(v.bone[k])].point(v.pos);
                        v.nlocal[k] = inv[static_cast<size_t>(v.bone[k])].dir(v.normal);
                    }
                }
            }
        }
        if (partMoved > 0) {
            stats.partsFixed += 1;
            partStats.emplace_back(pi, std::make_pair(partMoved, partMax));
        }
    }

    if (stats.verticesMoved > 0) {
        char buf[256];
        for (const auto& [pi, moved] : partStats) {
            std::snprintf(buf, sizeof buf, "garment intersection correction: %zu vertices moved out of the body (largest %.1f mm)", moved.first, moved.second * 1000.0f);
            scene.warn(scene.parts[static_cast<size_t>(pi)].id, buf);
        }
        std::snprintf(buf, sizeof buf, "garment intersection correction: %zu vertices in %zu part(s) moved out of the body", stats.verticesMoved, stats.partsFixed);
        scene.warn("garment", buf);
    } else if (stats.skipped > 0) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "garment intersection correction: %zu vertices needed more than the displacement cap and were left alone", stats.skipped);
        scene.warn("garment", buf);
    }
    return stats;
}

} // namespace cardbridge::assemble

