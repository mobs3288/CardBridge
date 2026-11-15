// Scratch: dump collider groups and chain-body masks so the collision story can be read.
//   usage: pmxgroups <file.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <string>
using namespace cardbridge;
using namespace pmxread;

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    std::fprintf(stderr, "-- bodies by signal order --\n");
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const RigidBody& b = m.bodies[i];
        const bool collider = b.mode == 0 && b.name.rfind("collider ", 0) == 0;
        const bool chain = b.group == 1;
        std::fprintf(stderr, "body[%3zu] grp=%-2d mask=%04x mode=%d bone=%d shape=%d size=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f) %s\n", i, b.group, b.mask, b.mode, b.bone, b.shape, b.size.x, b.size.y, b.size.z, b.pos.x, b.pos.y, b.pos.z, b.name.c_str());
        (void)collider; (void)chain;
    }
    return 0;
}
