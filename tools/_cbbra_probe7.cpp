// Scratch: render the GPU view; variants that hide suspected mesh names; ASCII maps + grey-region pixel counts.
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
    if (r > 200 && g > 200 && b > 200) return 'W';
    int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx - mn > 60) return 'x';
    if (mx - mn > 30) return r > g && g > b ? 'G' : 'y';
    if (mx < 60) return '#';
    float lum = (r + g + b) / 3.0f;
    if (mx - mn <= 20 && lum > 100 && lum < 220 && std::abs(r - g) <= 12 && std::abs(g - b) <= 12) return 'G';
    if (mx - mn <= 20 && lum >= 220) return 'W';
    if (mx - mn <= 20) return '.';
    return '.';
}

static void asciiMap(const std::vector<uint32_t>& bgra, int W, int H) {
    const int CW = 20, CH = 20;
    for (int y = 0; y + CH <= H; y += CH) {
        for (int x = 0; x + CW <= W; x += CW) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y; yy < y + CH; yy += 3) for (int xx = x; xx < x + CW; xx += 3) {
                uint32_t px = bgra[(size_t)yy * W + xx];
                sr += (px >> 16) & 0xff; sg += (px >> 8) & 0xff; sb += px & 0xff; ++n;
            }
            putchar(classify(sr / n, sg / n, sb / n));
        }
        putchar('\n');
    }
}

static int countGrey(const std::vector<uint32_t>& bgra, int W, int H, int yLo, int yHi) {
    int n = 0;
    for (int y = yLo; y < yHi; ++y)
        for (int x = 0; x < W; ++x) {
            uint32_t px = bgra[(size_t)y * W + x];
            int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
            int mx = std::max({r, g, b}), mn = std::min({r, g, b});
            if (mx - mn <= 20 && mn > 80 && mx < 220 && std::abs(r - g) <= 12 && std::abs(g - b) <= 12) ++n;
        }
    return n;
}

int main(int argc, char** argv) {
    std::string hide = argc > 1 ? argv[1] : "";   // substring of mesh names to hide, "" = none
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
    if (!gv.Init(&why)) { std::printf("Init failed: %s\n", why.c_str()); return 1; }
    if (!gv.Load(scene, &why)) { std::printf("Load failed: %s\n", why.c_str()); return 1; }
    const int W = 640, H = 640;
    std::vector<uint32_t> bgra;
    exporter::ViewCamera cam;

    // hiddenParts: per part. Hide parts whose mesh name contains `hide` (if non-empty).
    std::vector<char> hidden(scene.parts.size(), 0);
    if (!hide.empty()) {
        for (size_t pi = 0; pi < scene.parts.size(); ++pi)
            for (int mi : scene.parts[pi].meshes)
                if (scene.meshes[(size_t)mi].name.find(hide) != std::string::npos) hidden[pi] = 1;
    }
    const std::vector<char>* hp = hide.empty() ? nullptr : &hidden;
    if (!gv.Render(cam, W, H, hp, bgra)) { std::printf("Render failed\n"); return 1; }

    std::printf("== variant: hide '%s' ==\n", hide.c_str());
    asciiMap(bgra, W, H);
    std::printf("grey px in chest band y[80,300): %d\n", countGrey(bgra, W, H, 80, 300));
    std::printf("grey px overall (all): %d\n", countGrey(bgra, W, H, 0, H));
    return 0;
}
