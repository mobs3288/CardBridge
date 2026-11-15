// Scratch: render the card's scene as-is and after mesh simplification (the Optimize feature) to PNG,
// to see what the optimization does to the shading detail.
//   usage: simplifycheck <card.png> <library> <mods> <out-dir>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "export/Preview.h"
#include "mesh/Simplify.h"
#include <stb_image_write.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: simplifycheck <card.png> <library> <mods> <out-dir>\n"); return 1; }
    card::Card c = card::LoadCard(argv[1]);
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists;
    auto chk = library::CheckLibrary(lib, false);
    if (!chk.ok) { for (const auto& i : chk.issues) std::fprintf(stderr, "library problem: %s\n", i.c_str()); return 3; }
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao;
    ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);

    std::fprintf(stderr, "scene: %zu parts, %zu meshes, %zu triangles\n", scene.parts.size(), scene.meshes.size(), mesh::SceneTriangleCount(scene));
    exporter::ViewCamera cam;
    cam.yawDeg = 0;
    cam.pitchDeg = 4;
    exporter::ViewOptions vo;
    vo.shaded = true;           // through the game's shaders, like the viewer's Lit view
    exporter::ViewStatePtr stView = exporter::NewViewState();
    std::vector<uint32_t> px;
    exporter::RenderView(scene, cam, 640, 800, vo, *stView, px);
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) {
        rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255;
    }
    stbi_write_png((std::string(argv[4]) + "/original.png").c_str(), 640, 800, 4, rgba.data(), 640 * 4);

    mesh::SimplifyOptions so;
    so.maxReduction = 0.4f;
    mesh::SceneSimplifyStats stats;
    auto simplified = mesh::SimplifyScene(scene, 0.4f, so, stats, [](float f, const std::string& stage) { std::fprintf(stderr, "  simplify %.0f%%: %s\n", f * 100.0f, stage.c_str()); });
    if (!simplified) { std::fprintf(stderr, "simplify returned null\n"); return 4; }
    std::fprintf(stderr, "simplified: %zu -> %zu triangles, %zu -> %zu vertices\n", stats.originalTriangles, stats.resultTriangles, stats.originalVertices, stats.resultVertices);
    exporter::ViewStatePtr st2 = exporter::NewViewState();
    exporter::RenderView(*simplified, cam, 640, 800, vo, *st2, px);
    for (size_t i = 0; i < px.size(); ++i) {
        rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255;
    }
    stbi_write_png((std::string(argv[4]) + "/simplified40.png").c_str(), 640, 800, 4, rgba.data(), 640 * 4);
    return 0;
}
