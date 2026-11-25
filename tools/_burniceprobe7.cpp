// Scratch probe: recompute the rebind sum for the layered apron directly from public data.
#include "../src/card/CardFile.h"
#include "../src/card/SelectiveImport.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/core/Math.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
using namespace cardbridge;

static void rebindCheck(const assemble::Scene& s, const char* meshName) {
    for (const auto& m : s.meshes) {
        if (m.name != meshName) continue;
        const size_t n = m.vertices.size();
        size_t zeroWeights = 0;
        for (size_t i = 0; i < n; ++i) {
            const auto& v = m.vertices[i];
            float sw = 0;
            for (int k = 0; k < 4; ++k) sw += v.weight[k];
            if (sw <= 1e-6f) ++zeroWeights;
        }
        // recompute vertex 0 by hand
        const auto& v = m.vertices[0];
        Vec3 p{};
        float sum = 0;
        for (int k = 0; k < 4; ++k) {
            const int bi = v.bone[k];
            const float w = v.weight[k];
            if (w <= 0 || bi < 0 || static_cast<size_t>(bi) >= s.bones.size()) continue;
            p = p + s.bones[static_cast<size_t>(bi)].world.point(v.local[k]) * w;
            sum += w;
        }
        p = p * (1.0f / sum);
        std::printf("mesh %s: reshapable=%d zero-weight verts=%zu\n  v0 stored=(%.4f,%.4f,%.4f) rebound=(%.4f,%.4f,%.4f) sum=%.4f\n",
                    meshName, m.reshapable ? 1 : 0, zeroWeights, v.pos.x, v.pos.y, v.pos.z, p.x, p.y, p.z, sum);
        // the lowest bone world at rest: the CF_t heights
        std::printf("  bones: root y=%.4f, cf_j_hips y=%.4f, cf_j_leg01_L y=%.4f\n",
                    s.bones[static_cast<size_t>(s.boneIndex("cf_j_root"))].pos.y,
                    s.boneIndex("cf_j_spine02") >= 0 ? s.bones[static_cast<size_t>(s.boneIndex("cf_j_spine02"))].pos.y : -1.0f,
                    s.boneIndex("cf_j_leg01_L") >= 0 ? s.bones[static_cast<size_t>(s.boneIndex("cf_j_leg01_L"))].pos.y : -1.0f);
        return;
    }
    std::printf("mesh %s: NOT FOUND\n", meshName);
}

int main() {
    const std::string libRoot = "D:/Project K/cardbridge/library_full";
    const std::string modsDir = "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    const std::string cardPath = modsDir + "/Burnice/[Owl] Burnice.png";
    const std::string apronPath = "C:/Users/pepsi/Downloads/Oosuki Mamako - Sleeping Over.png";
    const char* la = std::getenv("LOCALAPPDATA");
    const std::string cache = la ? std::string(la) + "/CardBridge/modindex.json" : std::string();

    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, cache, &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));

    card::Card cd = card::LoadCard(cardPath);
    assemble::AssembleLayerSpec spec;
    spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
    assemble::AssembleOptions lao;
    lao.outfit = 0;
    lao.layers = {spec};
    const assemble::Scene layered = assemble::Assemble(cd, resolver, lao);
    rebindCheck(layered, "o_top_epron_a");
    rebindCheck(layered, "o_bot_epron_a");

    card::Card cb = card::LoadCard(cardPath);
    card::ApplyClothingOptions opt; opt.outfit = 0;
    card::ApplySelectiveOutfitSource(cb, apronPath, opt, card::SelectiveDecisions{});
    assemble::AssembleOptions pao;
    pao.outfit = 0;
    const assemble::Scene merged = assemble::Assemble(cb, resolver, pao);
    rebindCheck(merged, "o_top_epron_a");
    rebindCheck(merged, "o_bot_epron_a");
    return 0;
}
