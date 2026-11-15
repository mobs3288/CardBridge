// Scratch: decode every texture in a bundle to <outdir>/<name>.png.
//   usage: texpng <bundle> <outdir>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: texpng <bundle> <outdir>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    for (const auto* o : uf.serialized().ofClass(28)) {
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_Name").asStr();
        unity::PPtr pp; pp.fileId = 0; pp.pathId = o->pathId;
        auto t = uf.readTexture(pp, nullptr);
        if (!t) continue;
        std::string out = std::string(argv[2]) + "/" + n + ".png";
        stbi_write_png(out.c_str(), t->width, t->height, 4, t->rgba.data(), t->width * 4);
        std::fprintf(stderr, "wrote %s\n", out.c_str());
    }
    return 0;
}
