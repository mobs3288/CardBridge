#include "pmx/PmxTransform.h"

#include <cmath>

namespace cardbridge::pmx {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec3 rotate(Vec3 v, Vec3 deg) {
    if (deg.x == 0 && deg.y == 0 && deg.z == 0) return v;
    const float rx = deg.x * kPi / 180.0f, ry = deg.y * kPi / 180.0f, rz = deg.z * kPi / 180.0f;
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);
    // X
    float y = v.y * cx - v.z * sx, z = v.y * sx + v.z * cx;
    v.y = y; v.z = z;
    // Y
    float x = v.x * cy + v.z * sy; z = -v.x * sy + v.z * cy;
    v.x = x; v.z = z;
    // Z
    x = v.x * cz - v.y * sz; y = v.x * sz + v.y * cz;
    v.x = x; v.y = y;
    return v;
}

Vec3 normalizeVec(Vec3 v) {
    const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (n < 1e-8f) return {0, 1, 0};
    return {v.x / n, v.y / n, v.z / n};
}

} // namespace

Vec3 TransformPoint(Vec3 p, const AssetTransform& t) {
    Vec3 s{p.x * t.scale.x, p.y * t.scale.y, p.z * t.scale.z};
    s = rotate(s, t.rotationDeg);
    return {s.x + t.translation.x, s.y + t.translation.y, s.z + t.translation.z};
}

Vec3 TransformNormal(Vec3 n, const AssetTransform& t) {
    // Inverse-transpose of the scale, then the same rotation as positions.
    Vec3 s{n.x / t.scale.x, n.y / t.scale.y, n.z / t.scale.z};
    return normalizeVec(rotate(s, t.rotationDeg));
}

library::AssetTemplate ApplyTransform(const library::AssetTemplate& source, const AssetTransform& t) {
    library::AssetTemplate out = source;
    if (t.identity()) return out;
    for (auto& v : out.vertices) {
        v.pos = TransformPoint(v.pos, t);
        v.normal = TransformNormal(v.normal, t);
    }
    for (auto& p : out.bonePositions) p = TransformPoint(p, t);
    return out;
}

} // namespace cardbridge::pmx
