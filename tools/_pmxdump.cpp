// Scratch: dump PMX materials (name, diffuse, texture) with the engine's PMX loader.
//   usage: pmxdump <model.pmx>
#include "assets/PmxFile.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: pmxdump <model.pmx>\n"); return 1; }
    engine::pmx::PmxFile m;
    std::string err;
    if (!engine::pmx::Load(argv[1], m, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 2; }
    for (size_t i = 0; i < m.materials.size(); ++i) {
        const auto& mat = m.materials[i];
        const char* tex = mat.textureIndex >= 0 && static_cast<size_t>(mat.textureIndex) < m.texturePaths.size()
                          ? m.texturePaths[static_cast<size_t>(mat.textureIndex)].c_str() : "?";
        std::printf("mat[%zu] '%s' diffuse=(%.3f %.3f %.3f %.3f) tex=%s\n", i, mat.nameLocal.c_str(),
                    mat.diffuse[0], mat.diffuse[1], mat.diffuse[2], mat.diffuse[3], tex);
    }
    return 0;
}
