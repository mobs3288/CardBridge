// Scratch: CPU RenderView of the same scene; dump body chest triangles' colors.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/export/Preview.h"
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

    const int W = 640, H = 640;
    exporter::ViewCamera cam;
    exporter::ViewOptions opt;
    exporter::ViewStatePtr state = exporter::NewViewState();
    std::vector<uint32_t> bgra;
    exporter::RenderView(scene, cam, W, H, opt, *state, bgra);

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

    int dumped = 0;
    for (const auto& m : scene.meshes) {
        if (m.name != "o_body_a") continue;
        for (const auto& sm : m.submeshes) {
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                Pt ts[3];
                for (int k = 0; k < 3; ++k) { ts[k].x = sx(m.vertices[sm.indices[i + k]].pos.x); ts[k].y = sy(m.vertices[sm.indices[i + k]].pos.y); }
                int y0 = (int)std::max(0.0f, std::floor(std::min({ts[0].y, ts[1].y, ts[2].y})));
                int y1 = (int)std::min((float)(H - 1), std::ceil(std::max({ts[0].y, ts[1].y, ts[2].y})));
                if (y1 < 350 || y0 > 530) continue;
                float mx_ = (ts[0].x + ts[1].x + ts[2].x) / 3, my_ = (ts[0].y + ts[1].y + ts[2].y) / 3;
                uint32_t v = px((int)mx_, (int)my_);
                int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
                if (dumped++ < 30) std::printf("body tri render=(%d,%d,%d) at screen=(%.0f,%.0f)\n", r, g, b, mx_, my_);
            }
        }
    }
    std::printf("CPU: dumped %d\n", dumped);
    return 0;
}
