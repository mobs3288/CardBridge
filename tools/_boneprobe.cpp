// Scratch: histogram the bones the mesh vertices weigh onto, by name prefix.
//   usage: boneprobe <file.pmx> [prefix]
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: boneprobe <file.pmx> [prefix]\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    pmxread::Model m = pmxread::Read(raw.data(), raw.size());
    const char* prefix = argc > 2 ? argv[2] : "";
    std::map<std::string, size_t> count;
    std::set<int> used;
    for (const auto& v : m.vertices)
        for (int k = 0; k < 4; ++k) if (v.weight[k] > 0 && v.bone[k] >= 0) { ++count[m.bones[static_cast<size_t>(v.bone[k])].name]; used.insert(v.bone[k]); }
    size_t printed = 0;
    for (const auto& [name, n] : count)
        if (name.rfind(prefix, 0) == 0) { std::fprintf(stderr, "%-24s %zu\n", name.c_str(), n); if (++printed >= 400) break; }
    std::fprintf(stderr, "used bones: %zu, total vert influences with prefix '%s': %zu\n", used.size(), prefix, [&]{size_t s=0; for (auto& [n, c] : count) if (n.rfind(prefix, 0) == 0) s += c; return s;}());
    return 0;
}
