// Scratch: dump a named material's main texture from the assembled scene to a PNG.
//   usage: dumptex <card.png> <library> <mods> <material-name> <out.png>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "export/Preview.h"
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
    if (argc < 6) { std::fprintf(stderr, "usage: dumptex <card.png> <library> <mods> <material-name> <out.png>\n"); return 1; }
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
    if (std::getenv("DUMPTEX_OUTFIT")) ao.outfit = std::atoi(std::getenv("DUMPTEX_OUTFIT"));
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    if (std::getenv("DUMPTEX_BAKE")) exporter::BakeShaderTextures(scene, 1024, false, nullptr, nullptr, true, true);
    const std::string want = argv[4];
    if (want == "*") {
        for (size_t i = 0; i < scene.materials.size(); ++i)
            std::fprintf(stderr, "[%zu] '%s' tex=%d\n", i, scene.materials[i].name.c_str(), scene.materials[i].texture);
        return 0;
    }
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (m.name != want) continue;
        int tex = m.texture;
        if (!std::getenv("DUMPTEX_BAKE")) for (const auto& tb : m.texProps) if (tb.name == "_MainTex") tex = tb.texture;
        if (tex < 0 || static_cast<size_t>(tex) >= scene.textures.size()) { std::fprintf(stderr, "material '%s' has no main texture\n", want.c_str()); return 4; }
        const auto& t = scene.textures[static_cast<size_t>(tex)];
        std::fprintf(stderr, "material '%s' texture '%s' %dx%d rgba=%zu\n", want.c_str(), t.name.c_str(), t.width, t.height, t.rgba.size());
        stbi_write_png(argv[5], t.width, t.height, 4, t.rgba.data(), t.width * 4);
        return 0;
    }
    std::fprintf(stderr, "material '%s' not found\n", want.c_str());
    return 3;
}
