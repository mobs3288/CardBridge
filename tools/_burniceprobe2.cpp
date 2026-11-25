// Scratch probe: bone-frame comparison between plain and layer scenes for the real Burnice card.
#include "../src/card/CardFile.h"
#include "../src/card/SelectiveImport.h"
#include "../src/card/Abmx.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <cmath>
using namespace cardbridge;

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

    auto assembleWithLayers = [&](const card::Card& card, std::vector<assemble::AssembleLayerSpec> layers) {
        assemble::AssembleOptions ao;
        ao.outfit = 0;
        ao.layers = std::move(layers);
        return assemble::Assemble(card, resolver, ao);
    };

    card::Card c0 = card::LoadCard(cardPath);
    const assemble::Scene plain = assembleWithLayers(c0, {});

    card::Card cly = c0;    // layer only
    assemble::AssembleLayerSpec spec;
    spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
    std::vector<assemble::AssembleLayerSpec> layers{spec};
    std::map<std::string, Vec3> plainPos;
    for (const auto& b : plain.bones) plainPos[b.name] = b.pos;
    const assemble::Scene layerScene = assembleWithLayers(cly, std::move(layers));

    size_t shared = 0, mismatch = 0;
    double maxDelta = 0.0;
    std::string worst;
    for (const auto& b : layerScene.bones) {
        const auto it = plainPos.find(b.name);
        if (it == plainPos.end()) continue;
        ++shared;
        const double d = std::fabs((double)it->second.y - b.pos.y) + std::fabs((double)it->second.x - b.pos.x) * 0.5;
        if (d > 1e-4) { ++mismatch; if (d > maxDelta) { maxDelta = d; worst = b.name; } }
    }
    std::printf("plain bones=%zu layer bones=%zu shared=%zu mismatch=%zu maxDelta=%.4f worst=%s\n",
                plain.bones.size(), layerScene.bones.size(), shared, mismatch, maxDelta, worst.c_str());

    const char* named[] = {"cf_j_root","cf_j_head","cf_j_spine01","cf_j_spine02","cf_j_spine03","cf_j_f_hips",
                           "cf_j_thigh00_L","cf_j_thigh00_R","cf_j_leg01_L","cf_j_neck","cf_j_f_bust00","cf_j_f_an1","cf_j_shoulder_L"};
    for (const char* n : named) {
        const int ip = plain.boneIndex(n), il = layerScene.boneIndex(n);
        const Vec3* pv = ip >= 0 ? &plain.bones[static_cast<size_t>(ip)].pos : nullptr;
        const Vec3* lv = il >= 0 ? &layerScene.bones[static_cast<size_t>(il)].pos : nullptr;
        std::printf("%-16s plain=(%.3f,%.3f,%.3f) layer=(%.3f,%.3f,%.3f)\n", n,
                    pv ? pv->x : -1.0f, pv ? pv->y : -1.0f, pv ? pv->z : -1.0f,
                    lv ? lv->x : -1.0f, lv ? lv->y : -1.0f, lv ? lv->z : -1.0f);
    }
    const int lp = layerScene.boneIndex("cf_j_s_waist_00"), lp2 = layerScene.boneIndex("cf_j_sk_00_00");
    std::printf("layer cf_j_sk_00_00 = %d\n", lp);
    if (lp >= 0) std::printf("  pos=(%.3f,%.3f,%.3f) parent=%d\n", layerScene.bones[static_cast<size_t>(lp)].pos.x,
        layerScene.bones[static_cast<size_t>(lp)].pos.y, layerScene.bones[static_cast<size_t>(lp)].pos.z,
        layerScene.bones[static_cast<size_t>(lp)].parent);
    (void)lp2;
    return 0;
}
