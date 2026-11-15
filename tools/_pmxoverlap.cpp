// Scratch: measure the penetration of every group-1 (chain) body into every collider body at rest,
// exactly the way the exporter's rest-overlap test does (margin included).
//   usage: pmxoverlap <file.pmx> [chain-name-filter]
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
using namespace cardbridge;
using namespace pmxread;
using Vec3 = V3;

struct Core { Vec3 a, b; float r; };

static Vec3 sub3(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float len3(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The axis of a body's capsule from its rotation (the writer's PmxRotationFromYAxis: R = Ry(y) Rx(x) Rz(z), axis = R * +Y).
static Vec3 axisFromRot(const Vec3& r) {
    const float cx = std::cos(r.x), sx = std::sin(r.x), cy = std::cos(r.y), sy = std::sin(r.y), cz = std::cos(r.z), sz = std::sin(r.z);
    // R = Ry * Rx * Rz ; axis = R * (0,1,0)
    const float x0 = 0.0f, y0 = 1.0f, z0 = 0.0f;
    // Rz: (x,y) -> (x cosz - y sinz, x sinz + y cosz)
    float x1 = x0 * cz - y0 * sz, y1 = x0 * sz + y0 * cz, z1 = z0;
    // Rx: (y,z) -> (y cosx - z sinx, y sinx + z cosx)
    float x2 = x1, y2 = y1 * cx - z1 * sx, z2 = y1 * sx + z1 * cx;
    // Ry: (x,z) -> (x cosy + z siny, -x siny + z cosy)
    float x3 = x2 * cy + z2 * sy, y3 = y2, z3 = -x2 * sy + z2 * cy;
    return {x3, y3, z3};
}

static float dot3(const Vec3& u, const Vec3& v) { return u.x * v.x + u.y * v.y + u.z * v.z; }

// The least distance between segments a-b and c-d (a segment may be a point) - same math as the exporter's.
static float segDist(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const Vec3 u = sub3(b, a), v = sub3(d, c), w = sub3(a, c);
    const float uu = dot3(u, u), vv = dot3(v, v), uv = dot3(u, v), uw = dot3(u, w), vw = dot3(v, w);
    float s = 0.0f, t = 0.0f;
    const float den = uu * vv - uv * uv;
    auto clamp01 = [](float x) { return std::min(1.0f, std::max(0.0f, x)); };
    if (uu < 1e-12f && vv < 1e-12f) return len3(w);
    if (uu < 1e-12f) t = clamp01(vw / vv);
    else if (vv < 1e-12f) s = clamp01(-uw / uu);
    else {
        s = den > 1e-12f ? clamp01((uv * vw - vv * uw) / den) : 0.0f;
        t = (uv * s + vw) / vv;
        if (t < 0.0f) { t = 0.0f; s = clamp01(-uw / uu); }
        else if (t > 1.0f) { t = 1.0f; s = clamp01((uv - uw) / uu); }
    }
    const Vec3 p = {w.x + u.x * s - v.x * t, w.y + u.y * s - v.y * t, w.z + u.z * s - v.z * t};
    return len3(p);
}
static Core coreOf(const RigidBody& b) {
    Core c;
    c.r = b.size.x;
    const float cyl = b.size.y;
    if (b.shape == 2 && cyl > 1e-6f) {
        const Vec3 ax = axisFromRot(b.rot);
        const float half = 0.5f * cyl;
        c.a = {b.pos.x - ax.x * half, b.pos.y - ax.y * half, b.pos.z - ax.z * half};
        c.b = {b.pos.x + ax.x * half, b.pos.y + ax.y * half, b.pos.z + ax.z * half};
    } else {
        c.a = c.b = b.pos;
    }
    return c;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const char* filter = argc > 2 ? argv[2] : nullptr;
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    const float margin = 0.02f * 12.5f;   // the export scale
    std::vector<Core> colliders;
    std::vector<std::string> colliderName;
    std::vector<int> colliderGroup;
    for (const auto& b : m.bodies) {
        if (b.mode != 0 || b.name.rfind("collider ", 0) != 0) continue;
        colliders.push_back(coreOf(b));
        colliderName.push_back(b.name);
        colliderGroup.push_back(b.group);
    }
    for (const auto& b : m.bodies) {
        if (b.group != 1) continue;   // chain bodies
        if (filter && b.name.find(filter) == std::string::npos) continue;
        const Core cb = coreOf(b);
        int buried = 0;
        for (size_t ci = 0; ci < colliders.size(); ++ci) {
            const int g = colliderGroup[ci];
            if (!(b.mask & (1u << g))) continue;   // this body never meets that group: MMD ignores the contact
            const float dist = segDist(colliders[ci].a, colliders[ci].b, cb.a, cb.b);
            const float want = colliders[ci].r + cb.r + margin;
            if (dist < want) {
                ++buried;
                std::fprintf(stderr, "body[%s] vs %s (group %d, IN MASK): dist %.3f want %.3f  penetration %.3f\n",
                             b.name.c_str(), colliderName[ci].c_str(), g, dist, want, want - dist);
            }
        }
        if (buried == 0) std::fprintf(stderr, "body[%s]: spawn-safe (mask meets colliders, none overlapping)\n", b.name.c_str());
    }
    return 0;
}
