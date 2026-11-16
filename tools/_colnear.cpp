// Scratch: report the least distance from each dynamic chain body to every collider (from the file's bytes).
//   usage: colnear <file.pmx> [--limit F]   (only prints pairs closer than limit*scale, default 1.1)
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include "../src/export/PmxPhysics.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
using namespace cardbridge;
using namespace pmxread;

static Vec3 capsuleAxisFromEuler(Vec3 e) {
    Vec3 v{0, 1, 0};
    v = {v.x * std::cos(e.z) - v.y * std::sin(e.z), v.x * std::sin(e.z) + v.y * std::cos(e.z), v.z};
    v = {v.x, v.y * std::cos(e.x) - v.z * std::sin(e.x), v.y * std::sin(e.x) + v.z * std::cos(e.x)};
    v = {v.x * std::cos(e.y) + v.z * std::sin(e.y), v.y, -v.x * std::sin(e.y) + v.z * std::cos(e.y)};
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: colnear <file.pmx> [--limit F]\n"); return 1; }
    float limit = 1.1f;
    for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = (float)atof(argv[++i]);
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    struct Seg { Vec3 a, b; float r; int body; };
    std::vector<Seg> colliders;
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        if (b.group == 1) continue;
        Seg s; s.r = b.size.x; s.body = (int)i;
        if (b.shape == 2) {
            const Vec3 v = capsuleAxisFromEuler({b.rot.x, b.rot.y, b.rot.z});
            const float half = 0.5f * std::max(0.0f, b.size.y);
            s.a = {b.pos.x - v.x * half, b.pos.y - v.y * half, b.pos.z - v.z * half};
            s.b = {b.pos.x + v.x * half, b.pos.y + v.y * half, b.pos.z + v.z * half};
        } else { s.a = s.b = {b.pos.x, b.pos.y, b.pos.z}; }
        colliders.push_back(s);
    }
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        if (b.group != 1 || b.mode == 0) continue;
        Seg s; s.r = b.size.x; s.body = (int)i;
        if (b.shape == 2) {
            const Vec3 v = capsuleAxisFromEuler({b.rot.x, b.rot.y, b.rot.z});
            const float half = 0.5f * std::max(0.0f, b.size.y);
            s.a = {b.pos.x - v.x * half, b.pos.y - v.y * half, b.pos.z - v.z * half};
            s.b = {b.pos.x + v.x * half, b.pos.y + v.y * half, b.pos.z + v.z * half};
        } else { s.a = s.b = {b.pos.x, b.pos.y, b.pos.z}; }
        for (const Seg& c : colliders) {
            const float d = exporter::SegmentDistance(s.a, s.b, c.a, c.b) - (s.r + c.r);
            if (d < limit) std::fprintf(stderr, "%-26s  touches/%-24s  gap %+.4f\n", b.name.c_str(), m.bodies[(size_t)c.body].name.c_str(), d);
        }
    }
    return 0;
}
