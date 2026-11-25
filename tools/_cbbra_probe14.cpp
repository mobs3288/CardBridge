// Scratch: dump the cf_m_body_create_result skin texture's chest-region colors.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <map>
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
    for (const auto& t : scene.textures) {
        if (t.name != "cf_m_body_create_result" && t.name != "cf_m_body_create_result") continue;
        std::printf("texture %s: %dx%d, %zu bytes, fmt=%s\n", t.name.c_str(), t.width, t.height, t.rgba.size(), t.format.c_str());
        // print mean colors over rows
        int W = t.width, H = t.height;
        for (int band = 0; band < 8; ++band) {
            int y0 = band * H / 8, y1 = (band + 1) * H / 8;
            long sr = 0, sg = 0, sb = 0; int n = 0; int greyN = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    size_t i = ((size_t)y * W + x) * 4;
                    if (i + 3 >= t.rgba.size()) continue;
                    int r = t.rgba[i], g = t.rgba[i + 1], b = t.rgba[i + 2];
                    sr += r; sg += g; sb += b; ++n;
                    if (std::max({r, g, b}) - std::min({r, g, b}) <= 20) ++greyN;
                }
            if (n) std::printf("  band y[%3d,%3d): mean=(%ld,%ld,%ld), flat=%d (%.1f%%)\n", y0, y1, sr / n, sg / n, sb / n, greyN, n ? 100.0 * greyN / n : 0);
        }
        // Also dump raw pixel sample at the centre.
        int yc = H / 2, xc = W / 2;
        size_t i = ((size_t)yc * W + xc) * 4;
        if (i + 3 < t.rgba.size())
            std::printf("  centre pixel (%d,%d): (%d,%d,%d,%d)\n", xc, yc, t.rgba[i], t.rgba[i + 1], t.rgba[i + 2], t.rgba[i + 3]);
    }
    return 0;
}
