// Scratch: check that every texture the PMX references exists on disk next to it.
//   usage: texaudit <file.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <string>
using namespace cardbridge;
using namespace pmxread;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: texaudit <file.pmx>\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    std::string dir = argv[1];
    const size_t cut = dir.find_last_of("/\\");
    dir = dir.substr(0, cut);
    int missing = 0;
    for (size_t i = 0; i < m.textures.size(); ++i) {
        const std::string& p = m.textures[i];
        std::string sub = p;
        // PMX textures are stored with forward slashes; some are absolute paths from the author's machine
        // (absolute references in a PMX are broken on every machine but the author's).
        std::string base = sub;
        const size_t sl = base.find_last_of("/\\");
        if (sl != std::string::npos) base = base.substr(sl + 1);
        std::string rel = dir + "/textures/" + base;
        Bytes probe;
        try { probe = fileio::ReadFile(rel); } catch (...) { probe; }
        if (probe.empty()) { std::fprintf(stderr, "missing texture [%zu] \"%s\" (looked for %s)\n", i, p.c_str(), rel.c_str()); ++missing; }
    }
    std::fprintf(stderr, "%zu textures listed, %d missing\n", m.textures.size(), missing);
    // per-material reference report
    int refs = 0;
    for (const auto& mat : m.materials) {
        if (mat.texture < 0 || static_cast<size_t>(mat.texture) >= m.textures.size()) { if (mat.texture >= 0) { std::fprintf(stderr, "material \"%s\": texture index %d out of range\n", mat.name.c_str(), mat.texture); ++missing; } continue; }
        ++refs;
    }
    std::fprintf(stderr, "%d materials reference %d textures\n", (int)m.materials.size(), refs);
    return missing ? 2 : 0;
}
