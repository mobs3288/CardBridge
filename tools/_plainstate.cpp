// Diagnostic: state of the glove material after the viewer-style (lit=false) bake.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

static void dump(assemble::Scene& scene, const char* tag) {
    const auto& m = scene.materials[23];
    auto look = exporter::MmdMaterialLook(scene, m, false, 0, false);
    std::fprintf(stderr, "%s: mat[23] '%s' shader='%s' hidden=%d clips=%d alpha=%d trans=%d diff.a=%.3f tex=%d look.flags=0x%02x look.a=%.3f\n",
                 tag, m.name.c_str(), m.shader.c_str(), m.hidden ? 1 : 0, m.shaderClipsAlpha ? 1 : 0, m.alpha ? 1 : 0,
                 m.translucent ? 1 : 0, m.diffuse.w, m.texture, look.drawFlags, look.diffuse.w);
    if (m.texture >= 0) {
        const auto& t = scene.textures[static_cast<size_t>(m.texture)];
        size_t a0 = 0, a255 = 0;
        for (size_t p = 3; p + 3 < t.rgba.size(); p += 4) { if (t.rgba[p] == 0) ++a0; else if (t.rgba[p] == 255) ++a255; }
        std::fprintf(stderr, "  tex '%s' %dx%d A0=%zu A255=%zu\n", t.name.c_str(), t.width, t.height, a0, a255);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) return 1;
    card::Card c = card::LoadCard(argv[1]);
    std::vector<std::string> issues;
    resolve::ListTable lists; library::CheckLibrary(argv[2], false); lists.loadBaseGame(library::AbdataRoot(argv[2]), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(argv[3], std::string(argv[2]) + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    dump(scene, "pre-bake");
    std::string note;
    exporter::BakeShaderTextures(scene, 1024, false, &note, nullptr, true, true);
    std::fprintf(stderr, "note: %s\n", note.c_str());
    dump(scene, "post-bake(lit=false)");
    for (const auto& i : scene.issues)
        if (i.part == "materials" || i.message.find("glove") != std::string::npos || i.message.find("empty") != std::string::npos)
            std::fprintf(stderr, "issue [%s] %s: %s\n", i.severity.c_str(), i.part.c_str(), i.message.c_str());
    return 0;
}
