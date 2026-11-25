// Scratch: report the direct children of the swayed skirt-chain bones in the exported PMX.
//   usage: childprobe <file.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"

#include <cstdio>
#include <set>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: childprobe <file.pmx>\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    pmxread::Model m = pmxread::Read(raw.data(), raw.size());
    std::set<int> skirt;
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const std::string& n = m.bones[i].name;
        if (n.rfind("cf_j_sk_", 0) == 0) skirt.insert(static_cast<int>(i));
    }
    std::fprintf(stderr, "skirt bones: %zu\n", skirt.size());
    size_t directChildren = 0, allDesc = 0;
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const pmxread::Bone& b = m.bones[i];
        if (skirt.count(b.parent)) {
            std::fprintf(stderr, "child %3zu '%s' (parent %s)\n", i, b.name.c_str(), m.bones[static_cast<size_t>(b.parent)].name.c_str());
            ++directChildren;
        }
    }
    // all descendants: walk up
    int guard = 0;
    for (size_t i = 0; i < m.bones.size(); ++i) {
        int p = m.bones[i].parent;
        guard = 0;
        while (p >= 0 && guard++ < 64) { if (skirt.count(p)) { ++allDesc; break; } p = m.bones[static_cast<size_t>(p)].parent; }
    }
    std::fprintf(stderr, "direct children: %zu; all descendants: %zu\n", directChildren, allDesc);
    return 0;
}
