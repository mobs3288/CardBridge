// Scratch probe: full-matrix dump of the apron chain bones in both scenes.
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

static void dumpBoneMat(const assemble::Scene& s, const char* name, const char* tag) {
    const int i = s.boneIndex(name);
    if (i < 0) { std::printf("%s %s: not found\n", tag, name); return; }
    const auto& m = s.bones[static_cast<size_t>(i)].world;
    std::printf("%s %-16s parent=%d pos=(%.4f,%.4f,%.4f)\n  m=(%.5f %.5f %.5f %.5f)(%.5f %.5f %.5f %.5f)(%.5f %.5f %.5f %.5f)(%.5f %.5f %.5f %.5f)\n",
                tag, name, s.bones[static_cast<size_t>(i)].parent,
                s.bones[static_cast<size_t>(i)].pos.x, s.bones[static_cast<size_t>(i)].pos.y, s.bones[static_cast<size_t>(i)].pos.z,
                m.m[0], m.m[4], m.m[8], m.m[12],
                m.m[1], m.m[5], m.m[9], m.m[13],
                m.m[2], m.m[6], m.m[10], m.m[14],
                m.m[3], m.m[7], m.m[11], m.m[15]);
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

    for (const char* n : {"cf_j_sk_04_00", "cf_j_sk_04_01", "cf_j_sk_04_02", "cf_j_sk_05_00", "cf_j_sk_05_01", "cf_d_sk_Top", "cf_d_sk_Top_00", "cf_j_skt_00"}) {
        dumpBoneMat(merged, n, "merged");
        dumpBoneMat(layered, n, "layered");
    }
    return 0;
}
