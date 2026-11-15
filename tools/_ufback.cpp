// Scratch: list all GameObjects and materials in a bundle.
//   usage: ufback <bundle>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ufback <bundle>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    for (const auto& kv : uf.xforms()) {
        const unity::Xform& x = kv.second;
        if (x.parent == 0) std::fprintf(stderr, "root %s\n", x.name.c_str());
    }
    std::fprintf(stderr, "\n-- materials --\n");
    for (const auto* o : uf.serialized().ofClass(21)) {   // Material
        Value v = uf.serialized().read(*o);
        std::fprintf(stderr, "mat '%s'\n", v.at("m_Name").asStr().c_str());
    }
    return 0;
}
