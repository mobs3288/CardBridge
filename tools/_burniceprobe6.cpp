// Scratch probe: verify pos == sum w*nw*local for the apron meshes in each scene, using only public data.
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

static void checkMesh(const char* tag, const assemble::Scene& s, const char* meshName) {
    for (const auto& m : s.meshes) {
        if (m.name != meshName) continue;
        const size_t n = m.vertices.size();
        size_t checked = 0, differing = 0;
        double maxDelta = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const auto& v = m.vertices[i];
            Vec3 p{}, nw{};
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= s.bones.size()) continue;
                p = p + s.bones[static_cast<size_t>(v.bone[k])].world.point(v.local[k]) * v.weight[k];
                sum += v.weight[k];
            }
            if (sum <= 1e-6f) continue;
            ++checked;
            p = p * (1.0f / sum);
            const double d = std::fabs((double)p.x - v.pos.x) + std::fabs((double)p.y - v.pos.y) + std::fabs((double)p.z - v.pos.z);
            if (d > 1e-3) ++differing;
            if (d > maxDelta) maxDelta = d;
        }
        std::printf("%s %s: verts=%zu recomputing matches for %zu, differing=%zu, max delta=%.4f\n", tag, meshName, n, checked - differing, differing, maxDelta);
        return;
    }
    std::printf("%s %s: NOT FOUND\n", tag, meshName);
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
    assemble::AssembleOptions ao;
    ao.outfit = 0;

    card::Card cb = card::LoadCard(cardPath);
    card::ApplyClothingOptions opt; opt.outfit = 0;
    card::ApplySelectiveOutfitSource(cb, apronPath, opt, card::SelectiveDecisions{});
    const assemble::Scene merged = assemble::Assemble(cb, resolver, ao);

    card::Card cd = card::LoadCard(cardPath);
    assemble::AssembleLayerSpec spec;
    spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
    assemble::AssembleOptions lao = ao;
    lao.layers = {spec};
    const assemble::Scene layered = assemble::Assemble(cd, resolver, lao);

    checkMesh("merged", merged, "o_top_epron_a");
    checkMesh("merged", merged, "o_bot_epron_a");
    checkMesh("layered", layered, "o_top_epron_a");
    checkMesh("layered", layered, "o_bot_epron_a");
    return 0;
}
