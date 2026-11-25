// Scratch: inspect assembled materials: shader, queue, alpha flags, texture alpha distribution.
//   usage: matinspect <card.png> <library> <mods> [name-substr]
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: matinspect <card> <library> <mods> [substr]\n"); return 1; }
    const std::string want = argc > 4 ? argv[4] : "";
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists;
    library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao; ao.log = nullptr;
    if (std::getenv("MATINSPECT_OUTFIT")) ao.outfit = std::atoi(std::getenv("MATINSPECT_OUTFIT"));
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (!want.empty() && m.name.find(want) == std::string::npos && m.shader.find(want) == std::string::npos) continue;
        int queue = m.renderQueue, zwrite = -1;
        if (queue < 0 && m.shaderIndex >= 0 && static_cast<size_t>(m.shaderIndex) < scene.shaders.size()) {
            queue = scene.shaders[static_cast<size_t>(m.shaderIndex)].queue;
            zwrite = scene.shaders[static_cast<size_t>(m.shaderIndex)].zWrite ? 1 : 0;
        }
        std::printf("mat %-28s shader=%-34s q=%-5d zw=%-2d alpha=%d trans=%d diff=(%.2f %.2f %.2f %.2f) tex=%d",
                    m.name.c_str(), m.shader.c_str(), queue, zwrite, m.alpha?1:0, m.translucent?1:0,
                    m.diffuse.x,m.diffuse.y,m.diffuse.z,m.diffuse.w, m.texture);
        for (const auto& pr : m.props) if (pr.first == "_Color" || pr.first == "_Cutoff")
            std::printf(" %s=(%.2f %.2f %.2f %.2f)", pr.first.c_str(), pr.second.x,pr.second.y,pr.second.z,pr.second.w);
        if (m.texture >= 0 && static_cast<size_t>(m.texture) < scene.textures.size()) {
            const auto& t = scene.textures[static_cast<size_t>(m.texture)];
            long a0=0,a255=0,amid=0,sum=0;
            for (size_t p=3;p<t.rgba.size();p+=4) { uint8_t a=t.rgba[p]; if(a==0)++a0; else if(a==255)++a255; else {++amid; sum+=a;} }
            std::printf(" texname=%s %dx%d A0=%ld A255=%ld Amid=%ld avgmid=%.1f", t.name.c_str(), t.width,t.height,a0,a255,amid, amid?double(sum)/amid:0.0);
        }
        std::printf("\n");
    }
    return 0;
}
