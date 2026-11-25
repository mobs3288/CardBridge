// Scratch: render the scene through the GPU view and analyze the chest region for flat grey polygons.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/export/GpuView.h"
#include "../src/core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace cardbridge;

int main() {
    std::string cardPath = "C:/Users/pepsi/Downloads/139306275_p3.png";
    std::string libRoot = "D:/Project K/cardbridge/library_full";
    std::string modsDir = "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, "", &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));
    card::Card card = card::LoadCard(cardPath);
    assemble::AssembleOptions ao; ao.outfit = 0;
    assemble::Scene scene = assemble::Assemble(card, resolver, ao);

    std::printf("GPU init...\n");
    exporter::GpuView gv;
    std::string why;
    if (!gv.Init(&why)) { std::printf("GpuView::Init failed: %s\n", why.c_str()); return 1; }
    std::printf("GPU kind=%d adapter=%s\n", (int)gv.kind(), gv.adapter().c_str());
    if (!gv.Load(scene, &why)) { std::printf("GpuView::Load failed: %s\n", why.c_str()); return 1; }
    std::printf("loaded; skippedMaterials=%d\n", gv.skippedMaterials());

    const int W = 640, H = 640;
    std::vector<uint32_t> bgra;
    exporter::ViewCamera cam;
    if (!gv.Render(cam, W, H, nullptr, bgra)) { std::printf("Render failed\n"); return 1; }
    std::printf("rendered %dx%d\n", W, H);

    // Analyze: find flat-grey polygons in the chest region. Model: Y up; image coords: y up, origin centre-bottom.
    // chest region: y_scene in [0.9, 1.4] (the first ~40% of the image from the top).
    // Map scene y to image row: renderer draws "bounds centre"; we just scan the top 45% of the image.
    // A flat-grey pixel: R,G,B all within ~20 of each other, value in [80, 220], and far from the white diamonds.
    auto getRGB = [&](int x, int y, int& r, int& g, int& b) {
        uint32_t px = bgra[(size_t)y * W + x];
        r = (px >> 16) & 0xff; g = (px >> 8) & 0xff; b = px & 0xff;
    };
    struct Runs { std::vector<std::pair<int, int>> xs; };
    // scan band rows
    for (int bandLo = 0; bandLo < 12; ++bandLo) {
        const int bandHi = bandLo * H / 12;         // upper band (0..11 => top ~8..0 rows)
        if (bandHi <= 20 || bandHi > 260) continue;
        int count = 0; float sum = 0; std::vector<std::array<int, 3>> sample;
        for (int y = bandHi; y < bandHi + 40; ++y)
            for (int x = 0; x < W; ++x) {
                int r, g, b; getRGB(x, y, r, g, b);
                int mx = std::max({r, g, b}), mn = std::min({r, g, b});
                if (mx - mn > 30) continue;          // not flat
                float lum = (r + g + b) / 3.0f;
                if (lum < 80 || lum > 220) continue; // grey-ish range
                // white diamonds would be bright; skin-tone has r>g>b. grey polygons are balanced-ish.
                if (std::abs(r - g) > 12 || std::abs(g - b) > 12) continue;
                ++count; sum += lum;
                if (sample.size() < 400) sample.push_back({r, g, b});
            }
        if (count > 40)
            std::printf("band y[%d,%d): %d flat-grey px, meanLum=%.0f, sample[0]=(%d,%d,%d)\n",
                        bandHi, bandHi + 40, count, count ? sum / count : 0, sample.empty() ? 0 : sample[0][0], sample.empty() ? 0 : sample[0][1], sample.empty() ? 0 : sample[0][2]);
    }
    // Also: count distinct colours in the chest band, print the top few.
    {
        std::map<uint32_t, int> cols;
        for (int y = 30; y < 220; ++y) for (int x = 0; x < W; ++x) {
            int r, g, b; getRGB(x, y, r, g, b);
            cols[(uint32_t(r) << 16) | (uint32_t(g) << 8) | b]++;
        }
        std::printf("chest-band (y 30..220) top colors: ");
        int n = 0;
        for (auto& [c, cnt] : cols) if (cnt > 8 && ++n <= 12) {
            std::printf("(%d,%d,%d)x%d ", (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, cnt);
        }
        std::printf("\n");
    }
    return 0;
}
