// Scratch: dump a texture's alpha histogram + mean from a bundle by pathId or name.
//   usage: texstat <bundle> <texture name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: texstat <bundle> <texture name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    const std::string want = argv[2];
    for (const auto* o : uf.serialized().ofClass(28)) {   // Texture2D
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_Name").asStr();
        if (n != want) continue;
        unity::PPtr pp;
        pp.fileId = 0;
        pp.pathId = o->pathId;
        auto t = uf.readTexture(pp, nullptr);
        if (!t) { std::fprintf(stderr, "readTexture failed for %s\n", n.c_str()); return 2; }
        size_t n_px = (size_t)t->rgba.size() / 4;
        double sum[4] = {0, 0, 0, 0};
        int bins[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < n_px; ++i) {
            for (int c = 0; c < 4; ++c) sum[c] += t->rgba[i * 4 + c];
            const int a = t->rgba[i * 4 + 3];
            ++bins[a < 50 ? 0 : a < 200 ? 1 : a < 250 ? 2 : 3];
        }
        std::fprintf(stderr, "%s %dx%d mean rgba(%.1f %.1f %.1f %.1f) alpha bins a<50=%.2f 50-200=%.2f 200-250=%.2f >=250=%.2f\n",
                     n.c_str(), t->width, t->height, sum[0] / n_px, sum[1] / n_px, sum[2] / n_px, sum[3] / n_px,
                     (double)bins[0] / n_px, (double)bins[1] / n_px, (double)bins[2] / n_px, (double)bins[3] / n_px);
        return 0;
    }
    std::fprintf(stderr, "texture %s not found in bundle\n", want.c_str());
    return 3;
}
