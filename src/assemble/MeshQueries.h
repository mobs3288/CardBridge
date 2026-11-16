// Shared mesh-query helpers: closest-point-on-triangle and the triangle structure.
// GarmentFix uses it for the rest-pose penetration correction; the body-suppression fallback uses it for the
// ray-occlusion classification. Header-only: both are small, and both callers want them inlined.
#pragma once

#include "core/Math.h"

#include <cmath>
#include <algorithm>
#include <vector>

namespace cardbridge::assemble {

struct Tri {
    Vec3 a, b, c;
    Vec3 n;         // unit face normal
    float minX, minY, minZ, maxX, maxY, maxZ;
};

// Normal is expected to be the (possibly non-unit) face cross product; it is normalised here.
inline Tri MakeTri(Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
    Tri t;
    t.a = a; t.b = b; t.c = c;
    t.n = normal;
    const float nl = std::sqrt(t.n.x * t.n.x + t.n.y * t.n.y + t.n.z * t.n.z);
    t.n = nl > 1e-12f ? Vec3{t.n.x / nl, t.n.y / nl, t.n.z / nl} : Vec3{0, 1, 0};
    t.minX = std::min({a.x, b.x, c.x}); t.maxX = std::max({a.x, b.x, c.x});
    t.minY = std::min({a.y, b.y, c.y}); t.maxY = std::max({a.y, b.y, c.y});
    t.minZ = std::min({a.z, b.z, c.z}); t.maxZ = std::max({a.z, b.z, c.z});
    return t;
}

// Ericson's closest point on a triangle; never allocates.
inline void closestOnTriangle(Vec3 p, const Tri& t, Vec3& closest, float& distSq) {
    auto dot = [](Vec3 u, Vec3 v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
    auto sub = [](Vec3 u, Vec3 v) { return Vec3{u.x - v.x, u.y - v.y, u.z - v.z}; };
    auto add = [](Vec3 u, Vec3 v) { return Vec3{u.x + v.x, u.y + v.y, u.z + v.z}; };
    auto mul = [](Vec3 u, float s) { return Vec3{u.x * s, u.y * s, u.z * s}; };
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

} // namespace cardbridge::assemble
