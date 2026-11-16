// Scratch: verify the exported PMX's masks against a from-file replication of the exporter's rest-overlap
// assignment (same formula, same margin). Reports every dynamic chain body that may meet a collider its own
// core overlaps at rest, and every collider whose group a body meets that it overlaps.
//   usage: overlapcheck <file.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include "../src/export/PmxPhysics.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
using namespace cardbridge;
using namespace pmxread;

struct Seg { Vec3 a, b; float r; };

static Vec3 capsuleAxisFromEuler(Vec3 e) {
    Vec3 v{0, 1, 0};
    v = {v.x * std::cos(e.z) - v.y * std::sin(e.z), v.x * std::sin(e.z) + v.y * std::cos(e.z), v.z};
    v = {v.x, v.y * std::cos(e.x) - v.z * std::sin(e.x), v.y * std::sin(e.x) + v.z * std::cos(e.x)};
    v = {v.x * std::cos(e.y) + v.z * std::sin(e.y), v.y, -v.x * std::sin(e.y) + v.z * std::cos(e.y)};
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: overlapcheck <file.pmx>\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    const float margin = 0.02f * 12.5f;

    struct Entry { Seg s; int body = -1; };
    std::vector<Entry> colliders;
    std::vector<Entry> chain;
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        Entry e;
        e.body = (int)i;
        e.s.r = b.size.x;
        if (b.shape == 2) {
            const Vec3 v = capsuleAxisFromEuler({b.rot.x, b.rot.y, b.rot.z});
            const float half = 0.5f * std::max(0.0f, b.size.y);
            e.s.a = {b.pos.x - v.x * half, b.pos.y - v.y * half, b.pos.z - v.z * half};
            e.s.b = {b.pos.x + v.x * half, b.pos.y + v.y * half, b.pos.z + v.z * half};
        } else {
            e.s.a = e.s.b = {b.pos.x, b.pos.y, b.pos.z};
        }
        if (b.group == 1) {
            if (b.mode != 0) chain.push_back(e);
        } else {
            colliders.push_back(e);
        }
    }
    int bad = 0;
    for (const Entry& c : chain) {
        for (const Entry& col : colliders) {
            if (exporter::SegmentDistance(col.s.a, col.s.b, c.s.a, c.s.b) < col.s.r + c.s.r + margin) {
                const int g = (int)m.bodies[(size_t)col.body].group;
                if (g == 0 || g == 15) continue;
                const uint16_t mask = m.bodies[(size_t)c.body].mask;
                if ((mask >> g) & 1) {
                    std::fprintf(stderr, "body %d '%s' may meet collider %d '%s' (group %d) but starts inside it\n", c.body, m.bodies[(size_t)c.body].name.c_str(), col.body, m.bodies[(size_t)col.body].name.c_str(), g);
                    ++bad;
                }
            }
        }
    }
    std::fprintf(stderr, "bad overlaps: %d\n", bad);
    return bad ? 1 : 0;
}
