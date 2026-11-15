// Scratch: dump the assembled scene's materials for a card with an outfit file applied.
//   usage: matdumpcloth <card.png> <outfit.png> <library> <mods>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "card/MaterialEdits.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: matdumpcloth <card.png> <outfit.png> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }

    card::ApplyClothingOptions opt;
    // default target: the card's on-screen outfit (CLI semantics)
    opt.keepHairAccessories = true;
    {
        // dump the SOURCE outfit file's own MaterialEditor edits (before merge)
        try {
            card::ClothingFile src = card::LoadClothingFile(argv[2]);
            fprintf(stdout, "SRC clothes.srcCoordinate=%d singleCoordinateSource=%d extendedMap=%d extendedNil=%d\n", src.srcCoordinate, src.singleCoordinateSource ? 1 : 0, src.extended.isMap() ? 1 : 0, src.extended.isNil() ? 1 : 0);
            for (int coord : {0}) {
                card::MaterialEdits meSrc = card::LoadMaterialEdits(src.extended, coord);
                fprintf(stdout, "SRC-ME outfit=%d shaders=%zu floats=%zu colors=%zu mainTex=%zu\n", coord, meSrc.shaders.size(), meSrc.floats.size(), meSrc.colors.size(), meSrc.mainTex.size());
                for (const auto& e : meSrc.shaders) fprintf(stdout, "SRC-shader coord=%d slot=%d obj=%d mat=%s -> %s\n", e.coordinate, e.slot, e.objectType, e.material.c_str(), e.property.c_str());
                for (const auto& e : meSrc.floats) fprintf(stdout, "SRC-float  coord=%d slot=%d obj=%d mat=%s %s = %.4f\n", e.coordinate, e.slot, e.objectType, e.material.c_str(), e.property.c_str(), e.value[0]);
                for (const auto& e : meSrc.colors) fprintf(stdout, "SRC-color  coord=%d slot=%d obj=%d mat=%s %s = (%.4f, %.4f, %.4f, %.4f)\n", e.coordinate, e.slot, e.objectType, e.material.c_str(), e.property.c_str(), e.value[0], e.value[1], e.value[2], e.value[3]);
                for (const auto& m : meSrc.mainTex) fprintf(stdout, "SRC-mainTex slot=%d obj=%d mat=%s prop=%s texId=%d scale=%.3f,%.3f offset=%.3f,%.3f\n", m.slot, m.objectType, m.material.c_str(), m.property.c_str(), m.texId, m.scale[0], m.scale[1], m.offset[0], m.offset[1]);
            }
        } catch (const std::exception& e) {
            fprintf(stdout, "SRC-ME failed: %s\n", e.what());
        }
    }
    card::OutfitSourceReport rep = card::ApplyOutfitSource(c, argv[2], opt);
    {
        // dump the merged card's MaterialEditor shader/float/color edits for the target outfit
        int outfit = rep.apply.outfit >= 0 ? rep.apply.outfit : 0;
        card::MaterialEdits me = card::LoadMaterialEdits(c.extended, outfit);
        fprintf(stdout, "ME outfit=%d shaders=%zu floats=%zu colors=%zu mainTex=%zu\n", outfit, me.shaders.size(), me.floats.size(), me.colors.size(), me.mainTex.size());
        for (const auto& e : me.shaders) fprintf(stdout, "ME-shader slot=%d obj=%d mat=%s -> %s\n", e.slot, e.objectType, e.material.c_str(), e.property.c_str());
        for (const auto& e : me.floats) fprintf(stdout, "ME-float  slot=%d obj=%d mat=%s %s = %.4f\n", e.slot, e.objectType, e.material.c_str(), e.property.c_str(), e.value[0]);
        for (const auto& e : me.colors) fprintf(stdout, "ME-color  slot=%d obj=%d mat=%s %s = (%.4f, %.4f, %.4f, %.4f)\n", e.slot, e.objectType, e.material.c_str(), e.property.c_str(), e.value[0], e.value[1], e.value[2], e.value[3]);
    }

    std::vector<std::string> issues;
    std::string lib = argv[3], modsDir = argv[4];
    resolve::ListTable lists;
    auto chk = library::CheckLibrary(lib, false);
    if (!chk.ok) { for (const auto& i : chk.issues) std::fprintf(stderr, "  library problem: %s\n", i.c_str()); return 3; }
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    std::fprintf(stderr, "indexing mods...\n");
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));

    assemble::AssembleOptions ao;
    ao.log = [](const std::string& s) { std::fprintf(stdout, "LOG %s\n", s.c_str()); };
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);

    // parts so we can see which part each material belongs to
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const assemble::SceneMaterial& m = scene.materials[i];
        fprintf(stdout, "mat %zu '%s' shader='%s' alpha=%d translucent=%d doubleSided=%d bakedLit=%d toon=%zu texture=%d\n",
                i, m.name.c_str(), m.shader.c_str(), m.alpha ? 1 : 0, m.translucent ? 1 : 0, m.doubleSided ? 1 : 0, m.bakedLit ? 1 : 0, m.toon.size(), m.texture);
        for (const auto& tb : m.texProps) {
            const char* tn = (tb.texture >= 0 && static_cast<size_t>(tb.texture) < scene.textures.size()) ? scene.textures[static_cast<size_t>(tb.texture)].name.c_str() : "?";
            const char* sw = (tb.texture >= 0 && static_cast<size_t>(tb.texture) < scene.textures.size() && scene.textures[static_cast<size_t>(tb.texture)].clamp) ? "clamp" : "wrap";
            fprintf(stdout, "    %s -> %d (%s, %s)\n", tb.name.c_str(), tb.texture, tn, sw);
        }
        for (const auto& pr : m.props) {
            if (pr.first.rfind("_Cutoff", 0) == 0 || pr.first == "_Color" || pr.first == "_Mode" || pr.first == "_SpecularPower" || pr.first == "_Glossiness" || pr.first == "_Metallic" || pr.first == "_rimV")
                fprintf(stdout, "    prop %s = (%.3f, %.3f, %.3f, %.3f)\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
        }
        if (m.shaderIndex >= 0 && static_cast<size_t>(m.shaderIndex) < scene.shaders.size()) {
            const auto& sh = scene.shaders[static_cast<size_t>(m.shaderIndex)];
            fprintf(stdout, "    shaderIndex=%d name='%s' queue=%d zWrite=%d cull=%d cullProp='%s'\n", m.shaderIndex, sh.name.c_str(), sh.queue, sh.zWrite ? 1 : 0, sh.cull, sh.cullProperty.c_str());
        } else {
            fprintf(stdout, "    shaderIndex=%d\n", m.shaderIndex);
        }
        fprintf(stdout, "    renderQueue=%d shaderIndex=%d translucent=%d\n", m.renderQueue, m.shaderIndex, m.translucent ? 1 : 0);
    }
    fprintf(stdout, "\nmeshes:\n");
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const auto& m = scene.meshes[i];
        fprintf(stdout, "mesh %zu part=%d name=%s submeshes=", i, m.part, m.name.c_str());
        for (const auto& sm : m.submeshes) fprintf(stdout, "[mat=%d n=%zu]", sm.material, sm.indices.size() / 3);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "\nparts:\n");
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const auto& p = scene.parts[i];
        fprintf(stdout, "part %zu id=%s meshes=", i, p.id.c_str());
        for (int mi : p.meshes) fprintf(stdout, "%d ", mi);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "\n%zu textures, %zu shaders\n", scene.textures.size(), scene.shaders.size());
    return 0;
}
