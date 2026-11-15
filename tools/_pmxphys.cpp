// Scratch: dump every rigid body and joint with full physics parameters.
//   usage: pmxphys <file.pmx> [filter]
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <string>
using namespace cardbridge;
using namespace pmxread;

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const char* filter = argc > 2 ? argv[2] : nullptr;
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    std::fprintf(stderr, "-- bodies --\n");
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        if (filter && b.name.find(filter) == std::string::npos) continue;
        std::fprintf(stderr, "body[%3zu] grp=%2d mask=%04x mode=%d bone=%4d shape=%d size=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f) mass=%.4f movD=%.3f rotD=%.3f rest=%.2f fric=%.2f %s\n",
                     i, b.group, b.mask, b.mode, b.bone, b.shape, b.size.x, b.size.y, b.size.z, b.pos.x, b.pos.y, b.pos.z,
                     b.mass, b.moveDamping, b.rotDamping, b.restitution, b.friction, b.name.c_str());
    }
    std::fprintf(stderr, "-- joints --\n");
    for (size_t i = 0; i < m.joints.size(); ++i) {
        const Joint& j = m.joints[i];
        const char* na = j.a >= 0 && j.a < (int)m.bodies.size() ? m.bodies[j.a].name.c_str() : "?";
        const char* nb = j.b >= 0 && j.b < (int)m.bodies.size() ? m.bodies[j.b].name.c_str() : "?";
        if (filter && std::string(na).find(filter) == std::string::npos && std::string(nb).find(filter) == std::string::npos) continue;
        std::fprintf(stderr, "joint[%2zu] type=%d a=%d(%s) b=%d(%s) pos=(%.3f %.3f %.3f) limMove=(%.3f..%.3f) limRot=(%.1f..%.1f) springMove=(%.2f %.2f %.2f) springRot=(%.2f %.2f %.2f)\n",
                     i, j.type, j.a, na, j.b, nb, j.pos.x, j.pos.y, j.pos.z,
                     j.moveLower.x, j.moveUpper.x, j.rotLower.x, j.rotUpper.x,
                     j.springMove.x, j.springMove.y, j.springMove.z, j.springRot.x, j.springRot.y, j.springRot.z);
    }
    return 0;
}
