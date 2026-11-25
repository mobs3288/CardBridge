// Scratch: ASCII map of the GPU view render (color-class per cell).
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

static char classify(int r, int g, int b) {
    if (r > 200 && g > 200 && b > 200) return 'W';        // white-ish
    if (r > 150 && b > 150 && r - b > 60) return 'B';     // blue-ish (hair)
    int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx - mn > 60) return 'x';                          // saturated
    if (mx - mn > 30) {                                    // flat but hue
        if (r > g && g > b) return 'G';                    // skin-ish
        return 'y';
    }
    if (mx < 60) return '#';                               // dark
    float lum = (r + g + b) / 3.0f;
    if (mx - mn <= 20 && lum > 100 && lum < 220 && std::abs(r - g) <= 12 && std::abs(g - b) <= 12) return 'G';  // flat grey
    if (mx - mn <= 20 && lum >= 220) return 'W';           // near-white flat
    if (mx - mn <= 20) return '.';
    return '.';
}

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
    exporter::GpuView gv;
    std::string why;
    if (!gv.Init(&why) || !gv.Load(scene, &why)) { std::printf("fail: %s\n", why.c_str()); return 1; }
    const int W = 640, H = 640;
    std::vector<uint32_t> bgra;
    exporter::ViewCamera cam;
    gv.Render(cam, W, H, nullptr, bgra);
    // ASCII map: cells of 20x20 px, print mean color class
    const int CW = 20, CH = 20;
    for (int y = 0; y + CH <= H; y += CH) {
        for (int x = 0; x + CW <= W; x += CW) {
            // majority-ish color class: average
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y; yy < y + CH; yy += 3) for (int xx = x; xx < x + CW; xx += 3) {
                uint32_t px = bgra[(size_t)yy * W + xx];
                sr += (px >> 16) & 0xff; sg += (px >> 8) & 0xff; sb += px & 0xff; ++n;
            }
            putchar(classify(sr / n, sg / n, sb / n));
        }
        putchar('\n');
    }
    return 0;
}
