// Scratch: project scene chest triangles to screen space; per-mesh stats on the GPU render (grey fraction, mean colour).
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

struct Pt { float x, y; };

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

    // Model bounds -> M matrix (mirrors GpuView::RenderImpl, yaw=pitch=0).
    Pt mn{1e30f, 1e30f}, mx{-1e30f, -1e30f};
    for (const auto& m : scene.meshes)
        for (const auto& v : m.vertices) {
            mn.x = std::min(mn.x, v.pos.x); mx.x = std::max(mx.x, v.pos.x);
            mn.y = std::min(mn.y, v.pos.y); mx.y = std::max(mx.y, v.pos.y);
        }
    const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
    const float extent = std::max(mx.x - mn.x, mx.y - mn.y);
    const float a = 2.0f * H * 0.9f / extent / W;
    const float c = 2.0f * H * 0.9f / extent / H;
    auto sx = [&](float x) { return (1.0f + (-a * (x - cx))) * W / 2.0f; };
    auto sy = [&](float y) { return (1.0f - c * (y - cy)) * H / 2.0f; };
    auto px = [&](int x, int y) -> uint32_t {
        x = std::max(0, std::min(W - 1, x)); y = std::max(0, std::min(H - 1, y));
        return bgra[(size_t)y * W + x];
    };

    struct Agg { size_t tri = 0, grey = 0, tot = 0; double sr = 0, sg = 0, sb = 0; int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1; };
    std::map<std::string, Agg> agg;
    for (const auto& m : scene.meshes) {
        if (!(m.name.rfind("o_inner_a", 0) == 0 || m.name.rfind("o_bra_a", 0) == 0 ||
              m.name.find("misya_jacket") != std::string::npos || m.name.rfind("o_body_a", 0) == 0))
            continue;
        for (const auto& sm : m.submeshes) {
            Agg& A = agg[m.name];
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                Pt ts[3];
                for (int k = 0; k < 3; ++k) { ts[k].x = sx(m.vertices[sm.indices[i + k]].pos.x); ts[k].y = sy(m.vertices[sm.indices[i + k]].pos.y); }
                int x0 = (int)std::max(0.0f, std::floor(std::min({ts[0].x, ts[1].x, ts[2].x})));
                int x1 = (int)std::min((float)(W - 1), std::ceil(std::max({ts[0].x, ts[1].x, ts[2].x})));
                int y0 = (int)std::max(0.0f, std::floor(std::min({ts[0].y, ts[1].y, ts[2].y})));
                int y1 = (int)std::min((float)(H - 1), std::ceil(std::max({ts[0].y, ts[1].y, ts[2].y})));
                if (x1 <= x0 || y1 <= y0) continue;
                ++A.tri;
                for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                    uint32_t v = px(x, y);
                    int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
                    A.sr += r; A.sg += g; A.sb += b; ++A.tot;
                    int mx = std::max({r, g, b}), mn2 = std::min({r, g, b});
                    if (mx - mn2 <= 20 && mn2 > 80 && mx < 220 && std::abs(r - g) <= 12 && std::abs(g - b) <= 12) ++A.grey;
                }
                A.x0 = std::min(A.x0, x0); A.x1 = std::max(A.x1, x1);
                A.y0 = std::min(A.y0, y0); A.y1 = std::max(A.y1, y1);
            }
        }
    }
    for (const auto& [name, A] : agg) {
        double meanR = A.tot ? A.sr / A.tot : 0, meanG = A.tot ? A.sg / A.tot : 0, meanB = A.tot ? A.sb / A.tot : 0;
        std::printf("MESH %-34s tris=%-5zu px=%-7zu bbox=(%d..%d, %d..%d) meanRGB=(%.0f,%.0f,%.0f) greyPx=%zu (%.0f%%)\n",
                    name.c_str(), A.tri, A.tot, A.x0, A.x1, A.y0, A.y1, meanR, meanG, meanB, A.grey, A.tot ? 100.0 * A.grey / A.tot : 0.0);
    }
    return 0;
}
