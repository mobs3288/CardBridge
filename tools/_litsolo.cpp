// Diagnostic: unbaked GPU Lit render of selected scene parts.
// usage: litsolo <card> <library> <mods> <out.png> <keep-prefixes-comma>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/GpuView.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <cstdlib>
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
    std::vector<char> hidden(scene.parts.size(), 1);
    std::string keep = argv[6];
    size_t start = 0;
    while (start <= keep.size()) {
        size_t end = keep.find(',', start);
        std::string p = keep.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (size_t i = 0; i < scene.parts.size(); ++i) if (scene.parts[i].id == p) hidden[i] = 0;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    exporter::GpuOptions go; go.preference = "warp"; go.skin = "cpu";
    exporter::GpuView gpu; std::string why;
    if (!gpu.Init(go, &why) || !gpu.ready()) { std::fprintf(stderr, "gpu: %s\n", why.c_str()); return 3; }
    if (!gpu.Load(scene, &why)) { std::fprintf(stderr, "load: %s\n", why.c_str()); return 4; }
    gpu.SetPlain(false);
    exporter::ViewCamera cam; cam.yawDeg = 0; cam.pitchDeg = 4;
    std::vector<uint32_t> px;
    gpu.Render(cam, 640, 800, &hidden, px);
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) { rgba[i*4]=(px[i]>>16)&255; rgba[i*4+1]=(px[i]>>8)&255; rgba[i*4+2]=px[i]&255; rgba[i*4+3]=255; }
    stbi_write_png(argv[4], 640, 800, 4, rgba.data(), 640 * 4);
    std::fprintf(stderr, "wrote %s keep='%s'\n", argv[4], keep.c_str());
    return 0;
}
