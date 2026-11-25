// Scratch: GPU render -> find flat-grey blocks -> unproject to scene -> name the triangle/material/diffuse/texture there.
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

struct Pt { float x, y, z; };

static bool contains(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& bary) {
    // barycentric in XY; Z via projection
    float ax = a.x, ay = a.y, bx = b.x, by = b.y, cx = c.x, cy = c.y;
    float v0x = bx - ax, v0y = by - ay, v1x = cx - ax, v1y = cy - ay, v2x = p.x - ax, v2y = p.y - ay;
    float d00 = v0x * v0x + v0y * v0y, d01 = v0x * v1x + v0y * v1y, d11 = v1x * v1x + v1y * v1y;
    float d20 = v2x * v0x + v2y * v0y, d21 = v2x * v1x + v2y * v1y;
    float den = d00 * d11 - d01 * d01;
    if (std::fabs(den) < 1e-12f) return false;
    float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
    float u = 1.0f - v - w;
    if (u < -1e-3f || v < -1e-3f || w < -1e-3f) return false;
    bary = {u, v, w};
    return true;
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
    auto px = [&](int x, int y) -> uint32_t {
        x = std::max(0, std::min(W - 1, x)); y = std::max(0, std::min(H - 1, y));
        return bgra[(size_t)y * W + x];
    };

    Pt mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
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
    auto unx = [&](int sx_) { return cx - (2.0f * sx_ / W - 1.0f) / a; };
    auto uny = [&](int sy_) { return cy + (1.0f - 2.0f * sy_ / H) / c; };

    // Collect grey pixels (flat-grey render) in the chest band.
    std::map<std::string, int> hits;
    for (int y = 320; y < 520; ++y) for (int x = 100; x < 540; ++x) {
        uint32_t v = px(x, y);
        int r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
        int mxv = std::max({r, g, b}), mnv = std::min({r, g, b});
        if (mxv - mnv <= 20 && mnv > 80 && mxv < 220 && std::abs(r - g) <= 12 && std::abs(g - b) <= 12) {
            Vec3 w{unx(x), uny(y), 0.0f};
            // find the triangle
            for (const auto& m : scene.meshes) {
                if (!(m.name.rfind("o_inner_a", 0) == 0 || m.name.rfind("o_bra_a", 0) == 0 ||
                      m.name.find("misya_jacket") != std::string::npos || m.name.rfind("o_body_a", 0) == 0))
                    continue;
                for (const auto& sm : m.submeshes)
                    for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                        Vec3 bary;
                        if (contains(w, m.vertices[sm.indices[i]].pos, m.vertices[sm.indices[i + 1]].pos, m.vertices[sm.indices[i + 2]].pos, bary)) {
                            const assemble::SceneMaterial& mat = scene.materials[(size_t)sm.material];
                            std::string key = m.name + "/" + mat.name + "/tex=" + (mat.texture >= 0 ? scene.textures[(size_t)mat.texture].name : std::string("none"));
                            hits[key]++;
                            goto done;
                        }
                    }
            }
            done:;
        }
    }
    int tot = 0; for (auto& [k, c] : hits) tot += c;
    std::printf("total grey px (chest, all meshes): %d\n", tot);
    for (const auto& [k, c] : hits) if (c > 30) std::printf("%-80s grey=%d\n", k.c_str(), c);
    return 0;
}
