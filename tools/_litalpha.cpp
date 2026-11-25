// Diagnostic: render the same assembled scene twice in the live (game-shader) view, once as-is and once
// with every scene texture's alpha forced to 255. If the skirt becomes long in the second frame, its
// short silhouette comes from the alpha test, not from geometry/skinning.
#include "assemble/Assembler.h"
#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "export/GpuView.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

static void render(const assemble::Scene& scene, const char* path) {
    exporter::GpuOptions go; go.preference = "warp"; go.skin = "cpu";
    exporter::GpuView gpu; std::string why;
    if (!gpu.Init(go, &why) || !gpu.ready()) { std::fprintf(stderr, "gpu: %s\n", why.c_str()); return; }
    if (!gpu.Load(scene, &why)) { std::fprintf(stderr, "load: %s\n", why.c_str()); return; }
    gpu.SetPlain(false);
    exporter::ViewCamera cam;
    const int W = 640, H = 800;
    std::vector<uint32_t> px;
    if (!gpu.Render(cam, W, H, nullptr, px)) { std::fprintf(stderr, "render failed\n"); return; }
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) { rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255; }
    stbi_write_png(path, W, H, 4, rgba.data(), W * 4);
    std::fprintf(stderr, "wrote %s\n", path);
}

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: litalpha <card> <library> <mods> <out_prefix>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    std::string a = std::string(argv[4]) + "_orig.png", b = std::string(argv[4]) + "_opaque.png";
    render(scene, a.c_str());
    for (auto& t : scene.textures)
        for (size_t p = 3; p < t.rgba.size(); p += 4) t.rgba[p] = 255;
    render(scene, b.c_str());
    return 0;
}
