// Scratch: assemble + BAKE a card, then report material alpha flags and baked-texture alpha statistics.
//   usage: bakedump <card.png> <library> <mods> [name-substr] [dump-dir]
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "core/FileIo.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: bakedump <card> <library> <mods> [substr] [dumpdir]\n"); return 1; }
    const std::string want = argc > 4 ? argv[4] : "";
    const std::string dump = argc > 5 ? argv[5] : "";
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
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (!want.empty() && m.name.find(want) == std::string::npos && m.shader.find(want) == std::string::npos) continue;
        exporter::MmdLook look = exporter::MmdMaterialLook(scene, m, false, 0, false);
        std::printf("mat %-28s shader=%-32s alpha=%d trans=%d lit=%d lookdiff.a=%.3f draw=0x%02x",
            m.name.c_str(), m.shader.c_str(), m.alpha?1:0, m.translucent?1:0, m.bakedLit?1:0, look.diffuse.w, look.drawFlags);
        if (m.texture >= 0 && static_cast<size_t>(m.texture) < scene.textures.size()) {
            const auto& t = scene.textures[static_cast<size_t>(m.texture)];
            long a0=0,a255=0,amid=0,sum=0;
            for (size_t p=3;p<t.rgba.size();p+=4) { uint8_t a=t.rgba[p]; if(a==0)++a0; else if(a==255)++a255; else {++amid; sum+=a;} }
            std::printf(" tex=%s %dx%d A0=%ld A255=%ld Amid=%ld avgmid=%.1f", t.name.c_str(), t.width,t.height,a0,a255,amid, amid?double(sum)/amid:0.0);
            if (!dump.empty()) {
                std::string path = dump + "/" + std::to_string(i) + "_" + m.name + ".png";
                stbi_write_png(path.c_str(), t.width, t.height, 4, t.rgba.data(), t.width*4);
            }
        }
        std::printf("\n");
    }
    return 0;
}
