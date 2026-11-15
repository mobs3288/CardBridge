// Scratch: exact replica of the FixGarmentIntersections parity test against the fixture sphere.
//   usage: raycheck2
#include <cmath>
#include <cstdio>
#include <vector>

struct Vec3 { float x, y, z; };
static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Tri { Vec3 a, b, c; };

static bool rayHitsTri(Vec3 origin, const Tri& t) {
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

int main() {
    const int rings = 14, sectors = 28, stride = sectors + 1;
    std::vector<Vec3> verts;
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings, phi = 3.14159265f * v;
        for (int si = 0; si < stride; ++si) {
            const float u = (float)si / (float)sectors, th = 2.0f * 3.14159265f * u;
            verts.push_back({0.5f * std::sin(phi) * std::cos(th), 0.8f + 0.5f * std::cos(phi), 0.5f * std::sin(phi) * std::sin(th)});
        }
    }
    std::vector<Tri> tris;
    for (int r = 0; r < rings; ++r)
        for (int si = 0; si < sectors; ++si) {
            const int a = r * stride + si, b = a + 1, c = a + stride, d = c + 1;
            tris.push_back({verts[a], verts[c], verts[b]});
            tris.push_back({verts[b], verts[c], verts[d]});
        }
    const Vec3 o{0.0f, 0.8f, 0.0f};
    int hits = 0;
    for (size_t i = 0; i < tris.size(); ++i)
        if (rayHitsTri(o, tris[i])) { ++hits; std::fprintf(stderr, "hit tri %zu a=(%.4f %.4f %.4f)\n", i, tris[i].a.x, tris[i].a.y, tris[i].a.z); }
    std::fprintf(stderr, "hits=%d parity=%d\n", hits, hits % 2);
    return 0;
}
