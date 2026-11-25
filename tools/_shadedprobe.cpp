// Diagnostic: headless shaded render of a card with shader tracing support.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "export/Preview.h"
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

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: shadedprobe <card> <library> <mods> <out.png>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib, false); lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    if (std::getenv("SHADEBAKE")) exporter::BakeShaderTextures(scene, 1024, true, nullptr, nullptr, true, true);
    exporter::ViewCamera cam; cam.yawDeg = 0; cam.pitchDeg = 4;
    exporter::ViewOptions vo; vo.shaded = true;
    auto vs = exporter::NewViewState();
    std::vector<uint32_t> px;
    exporter::RenderView(scene, cam, 640, 800, vo, *vs, px);
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) { rgba[i*4] = (px[i] >> 16) & 255; rgba[i*4+1] = (px[i] >> 8) & 255; rgba[i*4+2] = px[i] & 255; rgba[i*4+3] = 255; }
    stbi_write_png(argv[4], 640, 800, 4, rgba.data(), 640 * 4);
    std::fprintf(stderr, "wrote %s\n", argv[4]);
    return 0;
}
