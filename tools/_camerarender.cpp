// Scratch: assemble + bake a card, render the plain/MMD view from a given camera angle (WARP).
//   usage: camerarender <card.png> <library> <mods> <out.png> <yawDeg> <pitchDeg>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "export/GpuView.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "core/FileIo.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 8) { std::fprintf(stderr, "usage: camerarender <card> <library> <mods> <out.png> <yaw> <pitch> <plain:0|1>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr,"card: %s\n",e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib=argv[2], modsDir=argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib,false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods=resolve::ModIndex::Build(modsDir, lib+"/modindex.json", &issues, &st, [](size_t,size_t){});
    auto resolver=std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao; ao.log=nullptr;
    assemble::Scene scene=assemble::Assemble(c,*resolver,ao);
    exporter::BakeShaderTextures(scene, 1024, true, nullptr, nullptr, true, true);
    exporter::GpuOptions go; go.preference="warp"; go.skin="cpu";
    exporter::GpuView gpu; std::string why;
    if (!gpu.Init(go,&why) || !gpu.ready()) { std::fprintf(stderr,"gpu: %s\n",why.c_str()); return 3; }
    if (!gpu.Load(scene,&why)) { std::fprintf(stderr,"load: %s\n",why.c_str()); return 4; }
    const bool plain = std::atoi(argv[7]) != 0;
    gpu.SetPlain(plain);
    exporter::ViewCamera cam;
    cam.yawDeg = static_cast<float>(std::atof(argv[5]));
    cam.pitchDeg = static_cast<float>(std::atof(argv[6]));
    const int W=640,H=800;
    std::vector<uint32_t> px;
    if (!gpu.Render(cam, W,H, nullptr, px)) { std::fprintf(stderr,"render failed\n"); return 5; }
    std::vector<uint8_t> rgba(px.size()*4);
    for (size_t i=0;i<px.size();++i) { rgba[i*4]=(px[i]>>16)&255; rgba[i*4+1]=(px[i]>>8)&255; rgba[i*4+2]=px[i]&255; rgba[i*4+3]=255; }
    stbi_write_png(argv[4], W,H,4, rgba.data(), W*4);
    std::fprintf(stderr,"wrote %s yaw=%.1f pitch=%.1f plain=%d\n",argv[4],cam.yawDeg,cam.pitchDeg,plain?1:0);
    return 0;
}
