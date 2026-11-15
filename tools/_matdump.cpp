// Scratch: dump the assembled scene's materials and their bindings (names, shaders, alpha/translucent, texture props).
//   usage: matdump <card.png> <library> <mods>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: matdump <card.png> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }

    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists;
    auto chk = library::CheckLibrary(lib, false);
    if (!chk.ok) { for (const auto& i : chk.issues) std::fprintf(stderr, "  library problem: %s\n", i.c_str()); return 3; }
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    std::fprintf(stderr, "indexing mods...\n");
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));

    assemble::AssembleOptions ao;
    ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);

    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const assemble::SceneMaterial& m = scene.materials[i];
        fprintf(stdout, "mat %zu '%s' shader='%s' alpha=%d translucent=%d doubleSided=%d bakedLit=%d toon=%zu texture=%d\n",
                i, m.name.c_str(), m.shader.c_str(), m.alpha ? 1 : 0, m.translucent ? 1 : 0, m.doubleSided ? 1 : 0, m.bakedLit ? 1 : 0, m.toon.size(), m.texture);
        for (const auto& tb : m.texProps) {
            const char* tn = (tb.texture >= 0 && static_cast<size_t>(tb.texture) < scene.textures.size()) ? scene.textures[static_cast<size_t>(tb.texture)].name.c_str() : "?";
            const char* sw = (tb.texture >= 0 && static_cast<size_t>(tb.texture) < scene.textures.size() && scene.textures[static_cast<size_t>(tb.texture)].clamp) ? "clamp" : "wrap";
            fprintf(stdout, "    %s -> %d (%s, %s)\n", tb.name.c_str(), tb.texture, tn, sw);
        }
    }
    fprintf(stdout, "\n%zu textures, %zu shaders\n", scene.textures.size(), scene.shaders.size());
    return 0;
}
