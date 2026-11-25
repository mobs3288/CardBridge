// Diagnostic: assemble a real card and dump the extracted anatomy meshes/materials.
//   usage: anatomymat <card.png> <library> <mods|-> [--no-mods]
#include "assemble/Assembler.h"
#include "card/CardFile.h"
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
    if (argc < 3) { std::fprintf(stderr, "usage: anatomymat <card.png> <library> <mods|->\n"); return 1; }
    card::Card c = card::LoadCard(argv[1]);
    std::vector<std::string> issues;
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(argv[2]), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(argc > 3 ? argv[3] : "", "", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const auto& p = scene.parts[i];
        if (p.id != "body.anatomy") continue;
        std::fprintf(stderr, "part %s meshes=%zu\n", p.id.c_str(), p.meshes.size());
        for (int mi : p.meshes) {
            const auto& m = scene.meshes[static_cast<size_t>(mi)];
            std::fprintf(stderr, "  mesh %s verts=%zu submeshes=%zu\n", m.name.c_str(), m.vertices.size(), m.submeshes.size());
            for (const auto& s : m.submeshes) {
                const auto& mat = scene.materials[static_cast<size_t>(s.material)];
                std::fprintf(stderr, "    material[%d] name='%s' shader='%s' texture=%d alpha=%d translucent=%d hidden=%d diffuse=(%.3f %.3f %.3f %.3f) tris=%zu\n",
                    s.material, mat.name.c_str(), mat.shader.c_str(), mat.texture, (int)mat.alpha, (int)mat.translucent,
                    (int)mat.hidden, mat.diffuse.x, mat.diffuse.y, mat.diffuse.z, mat.diffuse.w, s.indices.size() / 3);
                if (mat.texture >= 0) {
                    const auto& t = scene.textures[static_cast<size_t>(mat.texture)];
                    std::fprintf(stderr, "      texture %d '%s' %dx%d src='%s'\n", mat.texture, t.name.c_str(), t.width, t.height, t.source.c_str());
                }
                for (const auto& pr : mat.props)
                    if (pr.first == "_Color" || pr.first == "_MainTex" || pr.first == "_WhiteOff" || pr.first == "_WhiteOn")
                        std::fprintf(stderr, "      prop %s=(%.3f %.3f %.3f %.3f)\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
            }
        }
    }
    return 0;
}
