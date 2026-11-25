// Scratch: dump every property of a named material in the assembled scene.
//   usage: propsdump <card.png> <library> <mods> <material-name>
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
    if (argc < 5) { std::fprintf(stderr, "usage: propsdump <card.png> <library> <mods> <material-name>\n"); return 1; }
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
    assemble::AssembleOptions ao;
    ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    const std::string want = argv[4];
    for (const auto& m : scene.materials) {
        if (m.name != want) continue;
        std::printf("material '%s' shader='%s' diffuse=(%.2f %.2f %.2f %.2f)\n", m.name.c_str(), m.shader.c_str(), m.diffuse.x, m.diffuse.y, m.diffuse.z, m.diffuse.w);
        for (const auto& pr : m.props)
            std::printf("   prop '%s' = (%.3f %.3f %.3f %.3f)\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
        for (const auto& tb : m.texProps)
            std::printf("   tex  '%s' -> %d\n", tb.name.c_str(), tb.texture);
        return 0;
    }
    std::fprintf(stderr, "material '%s' not found\n", want.c_str());
    return 3;
}
