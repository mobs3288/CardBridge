// Scratch: list all Texture2D objects in a bundle with alpha stats.
//   usage: texlist <bundle>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: texlist <bundle>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    for (const auto* o : uf.serialized().ofClass(28)) {
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_Name").asStr();
        unity::PPtr pp; pp.fileId = 0; pp.pathId = o->pathId;
        auto t = uf.readTexture(pp, nullptr);
        if (!t) { std::fprintf(stderr, "%s readTexture failed\n", n.c_str()); continue; }
        size_t n_px = (size_t)t->rgba.size() / 4;
        double sum[4] = {0, 0, 0, 0};
        int hi = 0, lo = 0;
        for (size_t i = 0; i < n_px; ++i) {
            for (int c = 0; c < 4; ++c) sum[c] += t->rgba[i * 4 + c];
            if (t->rgba[i * 4 + 3] >= 250) ++hi;
            if (t->rgba[i * 4 + 3] < 50) ++lo;
        }
        std::fprintf(stderr, "%s %dx%d mean rgba(%.1f %.1f %.1f %.1f) a>=250 %.3f a<50 %.3f\n",
                     n.c_str(), t->width, t->height, sum[0] / n_px, sum[1] / n_px, sum[2] / n_px, sum[3] / n_px,
                     (double)hi / n_px, (double)lo / n_px);
    }
    return 0;
}
