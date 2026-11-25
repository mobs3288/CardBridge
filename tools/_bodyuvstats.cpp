// Diagnostic: histogram of a mesh's UV0 coordinates.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
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
    const int me = std::atoi(argv[5]);
    const auto& mesh = scene.meshes[static_cast<size_t>(me)];
    std::map<std::pair<int, int>, size_t> hist;
    size_t outside = 0;
    for (const auto& v : mesh.vertices) {
        if (v.uv.x < 0 || v.uv.x > 1 || v.uv.y < 0 || v.uv.y > 1) ++outside;
        ++hist[{static_cast<int>(v.uv.x * 10), static_cast<int>(v.uv.y * 10)}];
    }
    std::fprintf(stderr, "mesh %d '%s' verts=%zu outside01=%zu bins=%zu\n", me, mesh.name.c_str(), mesh.vertices.size(), outside, hist.size());
    for (const auto& [k, n] : hist) std::fprintf(stderr, "  uv bin %d.%d %d.%d : %zu\n", k.first / 10, k.first % 10, k.second / 10, k.second % 10, n);
    return 0;
}
