#pragma once

#include <cmath>

namespace cardbridge {

// Just enough linear algebra for skeleton and bind-pose maths - no engine or
// glm dependency, so the core stays a free-standing library.
struct Vec2 { float x = 0, y = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// Row-major 4x4 acting on column vectors: p' = M * p, m[row*4 + col].
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 TRS(Vec3 t, Quat q, Vec3 s) {
        float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
        // A bone whose local scale is exactly zero (e.g. an ABMX mod zeroing an eye-squash axis in the card)
        // makes the world matrix singular; inverse() then returns identity and any vertex captured against
        // that bone blows up on the next re-shape. Keep such bones invertible: flat to a micron is flat.
        const float zeroScale = 1e-6f;
        auto clampScale = [&](float s) { return std::fabs(s) < zeroScale ? std::copysign(zeroScale, s == 0.0f ? 1.0f : s) : s; };
        s.x = clampScale(s.x);
        s.y = clampScale(s.y);
        s.z = clampScale(s.z);
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat4 r;
        r.m[0] = (1 - 2 * (yy + zz)) * s.x; r.m[1] = 2 * (xy - wz) * s.y;       r.m[2] = 2 * (xz + wy) * s.z;       r.m[3] = t.x;
        r.m[4] = 2 * (xy + wz) * s.x;       r.m[5] = (1 - 2 * (xx + zz)) * s.y; r.m[6] = 2 * (yz - wx) * s.z;       r.m[7] = t.y;
        r.m[8] = 2 * (xz - wy) * s.x;       r.m[9] = 2 * (yz + wx) * s.y;       r.m[10] = (1 - 2 * (xx + yy)) * s.z; r.m[11] = t.z;
        return r;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[i * 4 + k] * b.m[k * 4 + j];
                r.m[i * 4 + j] = s;
            }
        return r;
    }

    Vec3 point(Vec3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
                m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
    }
    // Direction (no translation); not re-normalised, see normalize().
    Vec3 dir(Vec3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z,
                m[4] * p.x + m[5] * p.y + m[6] * p.z,
                m[8] * p.x + m[9] * p.y + m[10] * p.z};
    }
    Vec3 translation() const { return {m[3], m[7], m[11]}; }
};

inline Vec3 normalize(Vec3 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-12f ? Vec3{v.x / l, v.y / l, v.z / l} : v;
}

// General 4x4 inverse (cofactor expansion); returns identity if singular.
inline Mat4 inverse(const Mat4& a) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-20f) return r;
    float id = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * id;
    return r;
}

} // namespace cardbridge
