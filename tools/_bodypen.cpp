// Scratch: measure how deeply every PMX group-1 chain rigid body penetrates the body mesh at rest.
// The body mesh is the vertices of the first material (body/o_body).
//   usage: bodypen <file.pmx>
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

struct Tri { Vec3 a, b, c; };

static Vec3 sub3(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 cross3(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float len3(const Vec3& v) { return std::sqrt(dot3(v, v)); }

// closest point on triangle to point p (Ericson)
static float pointTriDist(const Vec3& p, const Tri& t) {
    const Vec3 a = t.a, b = t.b, c = t.c;
    const Vec3 ab = sub3(b, a), ac = sub3(c, a), ap = sub3(p, a);
    const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return len3(sub3(p, a));
    const Vec3 bp = sub3(p, b);
    const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return len3(sub3(p, b));
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return len3(sub3(p, Vec3{a.x + v * ab.x, a.y + v * ab.y, a.z + v * ab.z}));
    }
    const Vec3 cp = sub3(p, c);
    const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return len3(sub3(p, c));
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float v = d2 / (d2 - d6);
        return len3(sub3(p, Vec3{a.x + v * ac.x, a.y + v * ac.y, a.z + v * ac.z}));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const Vec3 bc = sub3(c, b);
        const float v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return len3(sub3(p, Vec3{b.x + v * bc.x, b.y + v * bc.y, b.z + v * bc.z}));
    }
    const Vec3 n = cross3(ab, ac);
    const float nn = len3(n);
    return std::fabs(dot3(n, ap)) / (nn > 1e-12f ? nn : 1e-12f);
}

// closest distance between a sphere and a triangle soup: distance from sphere center to each triangle minus radius
static float sphereTriDist(const Vec3& c, float r, const std::vector<Tri>& tris, float margin) {
    float best = 1e30f;
    for (const auto& t : tris) best = std::min(best, pointTriDist(c, t));
    return best - r;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    // the first material is the body: its index range is [0, materials[0].indexCount)
    const pmxread::Material& bodyMat = m.materials[0];
    std::vector<Tri> bodytris;
    bodytris.reserve(bodyMat.indexCount / 3);
    for (uint32_t f = 0; f + 2 < bodyMat.indexCount; f += 3) {
        Tri t;
        t.a = m.vertices[m.indices[f]].pos;
        t.b = m.vertices[m.indices[f + 1]].pos;
        t.c = m.vertices[m.indices[f + 2]].pos;
        bodytris.push_back(t);
    }
    fprintf(stderr, "body tris %zu\n", bodytris.size());
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        if (b.group != 1) continue;   // chain bodies only
        // capsule proxy: use sphere test at the two ends for cylinders, sphere for spheres/boxes
        const Vec3 pos{b.pos.x, b.pos.y, b.pos.z};
        float r = b.size.x;
        float d = 1e30f;
        if (b.shape == 2 && b.size.y > 1e-6f) {   // cylinder: sample the two caps as spheres of size.x and midpoints
            const float half = 0.5f * b.size.y;
            // rotation maps +Y to the cylinder axis (R = Ry*Rx*Rz as PMX)
            const float cx = std::cos(b.rot.x), sx = std::sin(b.rot.x), cy = std::cos(b.rot.y), sy = std::sin(b.rot.y), cz = std::cos(b.rot.z), sz = std::sin(b.rot.z);
            // axis = R * (0,1,0)
            float x1 = 0.f * cz - 1.f * sz, y1 = 0.f * sz + 1.f * cz, z1 = 0.f;
            float x2 = x1, y2 = y1 * cx - z1 * sx, z2 = y1 * sx + z1 * cx;
            float x3 = x2 * cy + z2 * sy, y3 = y2, z3 = -x2 * sy + z2 * cy;
            const Vec3 ax{x3, y3, z3};
            for (int s = -1; s <= 1; ++s) {
                const Vec3 p{pos.x + ax.x * half * s, pos.y + ax.y * half * s, pos.z + ax.z * half * s};
                d = std::min(d, sphereTriDist(p, r, bodytris, 0.0f));
            }
        } else {
            d = sphereTriDist(pos, r, bodytris, 0.0f);
        }
        const char* flag = d < 0.0f ? " INSIDE" : (d < 0.05f ? " TOUCH" : "");
        fprintf(stdout, "body[%3zu] %-12s shape=%d r=%.3f pos=(%.2f %.2f %.2f)  clearance=%+.4f%s\n", i, b.name.c_str(), b.shape, r, pos.x, pos.y, pos.z, d, flag);
    }
    return 0;
}
