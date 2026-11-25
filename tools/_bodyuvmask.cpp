// Diagnostic: rasterise the body mesh's triangle UVs into a mask over its create-result texture.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 6) return 1;
    card::Card c = card::LoadCard(argv[1]);
    std::vector<std::string> issues;
    resolve::ListTable lists; library::CheckLibrary(argv[2], false); lists.loadBaseGame(library::AbdataRoot(argv[2]), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(argv[3], std::string(argv[2]) + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    const int mi = std::atoi(argv[5]);
    const auto& mat = scene.materials[static_cast<size_t>(mi)];
    std::fprintf(stderr, "mat %d '%s' tex=%d\n", mi, mat.name.c_str(), mat.texture);
    const int W = 1024, H = 1024;
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H * 4, 0);
    size_t covered = 0, veinCovered = 0, veinTotal = 0;
    const auto& tex = scene.textures[static_cast<size_t>(mat.texture)];
    for (size_t i = 0; i + 2 < tex.rgba.size(); i += 4) {
        const uint8_t r = tex.rgba[i], g = tex.rgba[i + 1];
        if (r > 120 && r > g + 40) ++veinTotal;
    }
    for (const auto& mesh : scene.meshes) for (const auto& sm : mesh.submeshes) {
        if (sm.material != mi) continue;
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
            const bool useUv1 = std::getenv("USEUV1") != nullptr;
            const auto& a = useUv1 ? mesh.vertices[sm.indices[i]].uv1 : mesh.vertices[sm.indices[i]].uv;
            const auto& b = useUv1 ? mesh.vertices[sm.indices[i + 1]].uv1 : mesh.vertices[sm.indices[i + 1]].uv;
            const auto& c2 = useUv1 ? mesh.vertices[sm.indices[i + 2]].uv1 : mesh.vertices[sm.indices[i + 2]].uv;
            const float ax = a.x * W, ay = (1 - a.y) * H, bx = b.x * W, by = (1 - b.y) * H, cx = c2.x * W, cy = (1 - c2.y) * H;
            const float den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            if (std::fabs(den) < 1e-6f) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
            const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
            const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
            for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
                const float l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
                const float l2 = 1 - l0 - l1;
                if (l0 < -0.001f || l1 < -0.001f || l2 < -0.001f) continue;
                const size_t p = (static_cast<size_t>(y) * W + x) * 4;
                if (mask[p] == 0) ++covered;
                mask[p] = 255; mask[p + 1] = 0; mask[p + 2] = 0; mask[p + 3] = 255;
                const uint8_t r = tex.rgba[p], g = tex.rgba[p + 1];
                if (r > 120 && r > g + 40) ++veinCovered;
            }
        }
    }
    std::fprintf(stderr, "covered=%zu veinTotal=%zu veinCovered=%zu\n", covered, veinTotal, veinCovered);
    stbi_write_png(argv[4], W, H, 4, mask.data(), W * 4);
    std::vector<uint8_t> rgba = tex.rgba;
    for (size_t p = 0; p + 3 < rgba.size(); p += 4) if (mask[p] == 0) { rgba[p] /= 4; rgba[p + 1] /= 4; rgba[p + 2] /= 4; }
    stbi_write_png((std::string(argv[4]) + ".tex.png").c_str(), W, H, 4, rgba.data(), W * 4);
    return 0;
}
