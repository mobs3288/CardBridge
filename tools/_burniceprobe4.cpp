// Scratch probe: per-vertex comparison of the apron mesh between the merged scene and the grafted one.
#include "../src/card/CardFile.h"
#include "../src/card/SelectiveImport.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

static void dumpMesh(const char* tag, const assemble::Scene& s, const char* meshName) {
    for (const auto& m : s.meshes) {
        if (m.name != meshName) continue;
        std::printf("== %s mesh %s verts=%zu reshapable=%d\n", tag, m.name.c_str(), m.vertices.size(), m.reshapable ? 1 : 0);
        for (int i = 0; i < 5; ++i) {
            const auto& v = m.vertices[static_cast<size_t>(i)];
            std::printf("  v[%d] pos=(%.4f,%.4f,%.4f) bones=(%d,%d,%d,%d) w=(%.3f,%.3f,%.3f,%.3f) local0min=(%.4f,%.4f,%.4f)\n",
                        i, v.pos.x, v.pos.y, v.pos.z, v.bone[0], v.bone[1], v.bone[2], v.bone[3], v.weight[0], v.weight[1], v.weight[2], v.weight[3], v.local[0].x, v.local[0].y, v.local[0].z);
        }
        // where do those bone indices point?
        std::map<int, std::string> seen;
        for (int i = 0; i < 5; ++i)
            for (int k = 0; k < 4; ++k)
                if (m.vertices[static_cast<size_t>(i)].weight[k] > 0) {
                    const int bi = m.vertices[static_cast<size_t>(i)].bone[k];
                    std::string n = (bi >= 0 && static_cast<size_t>(bi) < s.bones.size()) ? s.bones[static_cast<size_t>(bi)].name : "<oob>";
                    std::printf("      weight k=%d bone %d (%s) = %.3f\n", k, bi, n.c_str(), m.vertices[static_cast<size_t>(i)].weight[k]);
                }
        return;
    }
    std::printf("== %s: mesh %s NOT FOUND\n", tag, meshName);
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

    // merged: apron replaces the top
    card::Card cb = card::LoadCard(cardPath);
    card::ApplyClothingOptions opt; opt.outfit = 0;
    card::ApplySelectiveOutfitSource(cb, apronPath, opt, card::SelectiveDecisions{});
    assemble::AssembleOptions ao; ao.outfit = 0;
    const assemble::Scene merged = assemble::Assemble(cb, resolver, ao);
    dumpMesh("merged", merged, "o_top_epron_a");

    // layered: apron grafted
    card::Card cd = card::LoadCard(cardPath);
    assemble::AssembleLayerSpec spec;
    spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
    assemble::AssembleOptions lao = ao;
    lao.layers = {spec};
    const assemble::Scene layered = assemble::Assemble(cd, resolver, lao);
    dumpMesh("layered", layered, "o_top_epron_a");
    // where do the mapped-in bones of vertex0 land relative to the shared ones
    std::map<std::string, Vec3> plainPos;
    assemble::AssembleOptions pao = ao;
    const assemble::Scene plain = assemble::Assemble(card::LoadCard(cardPath), resolver, pao);
    for (const auto& b : plain.bones) plainPos[b.name] = b.pos;
    for (const char* want : {"cf_j_sk_00_00", "cf_j_sp_breast00", "cf_j_spine02", "cf_j_spinesk_00"}) {
        const int ip = plain.boneIndex(want), iq = layered.boneIndex(want);
        std::printf("bone %-16s plain idx=%d layered idx=%d\n", want, ip, iq);
    }
    return 0;
}
