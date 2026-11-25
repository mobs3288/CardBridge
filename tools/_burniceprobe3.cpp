// Scratch probe: find the DynamicBone chain bones in each scene and compare garment meshes merged vs grafted.
#include "../src/card/CardFile.h"
#include "../src/card/SelectiveImport.h"
#include "../src/card/Abmx.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
using namespace cardbridge;

static void dumpChains(const char* tag, const assemble::Scene& s) {
    size_t n = 0;
    for (const auto& b : s.bones) if (b.name.find("sk_") != std::string::npos || b.name.find("skirt") != std::string::npos || b.name.find("_c") != std::string::npos) ++n;
    std::printf("%s: chain-ish bones = %zu\n", tag, n);
    for (const auto& b : s.bones)
        if (b.name.find("sk_") != std::string::npos || b.name.find("skirt") != std::string::npos) std::printf("   %s pos=(%.3f,%.3f,%.3f) parent=%d\n", b.name.c_str(), b.pos.x, b.pos.y, b.pos.z, b.parent);
    size_t cn = 0;
    for (const auto& c : s.chains) { std::printf("   chain %s part=%s bones=%zu\n", c.name.c_str(), c.part.c_str(), c.bones.size()); ++cn; }
    std::printf("   chains total=%zu\n", cn);
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

    // B: merge pin into the card
    card::Card cb = card::LoadCard(cardPath);
    card::ApplyClothingOptions opt; opt.outfit = 0;
    card::ApplySelectiveOutfitSource(cb, apronPath, opt, card::SelectiveDecisions{});
    const assemble::Scene merged = assemble::Assemble(cb, resolver, ao);
    std::printf("===== merged scene: parts=%zu meshes=%zu bones=%zu\n", merged.parts.size(), merged.meshes.size(), merged.bones.size());
    dumpChains("merged", merged);
    for (const auto& m : merged.meshes) {
        if (m.name.find("epron") == std::string::npos) continue;
        Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
        size_t tris = 0; for (const auto& sm : m.submeshes) tris += sm.indices.size() / 3;
        for (const auto& v : m.vertices) {
            lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
            hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
        }
        std::printf("  mesh %s verts=%zu tris=%zu bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) reshapable=%d\n",
                    m.name.c_str(), m.vertices.size(), tris, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, m.reshapable ? 1 : 0);
        std::printf("    bone[0] name = %d weights after bind: ", m.vertices.empty() ? -1 : m.vertices[0].bone[0]);
        for (int k = 0; k < 4; ++k) if (m.vertices.size() > 0 && m.vertices[0].weight[k] > 0) std::printf("%d:%.2f ", m.vertices[0].bone[k], m.vertices[0].weight[k]);
        std::printf("\n");
    }

    // C: layer graft
    card::Card cd = card::LoadCard(cardPath);
    assemble::AssembleLayerSpec spec;
    spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
    assemble::AssembleOptions lao = ao;
    lao.layers = {spec};
    const assemble::Scene layered = assemble::Assemble(cd, resolver, lao);
    std::printf("\n===== layered scene: parts=%zu meshes=%zu bones=%zu\n", layered.parts.size(), layered.meshes.size(), layered.bones.size());
    dumpChains("layered", layered);
    for (const auto& m : layered.meshes) {
        if (m.name.find("epron") == std::string::npos) continue;
        Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
        size_t tris = 0; for (const auto& sm : m.submeshes) tris += sm.indices.size() / 3;
        for (const auto& v : m.vertices) {
            lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
            hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
        }
        std::printf("  mesh %s verts=%zu tris=%zu bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) reshapable=%d\n",
                    m.name.c_str(), m.vertices.size(), tris, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, m.reshapable ? 1 : 0);
        std::printf("    vertex0 bones: ");
        for (int k = 0; k < 4; ++k) if (m.vertices[0].weight[k] > 0) std::printf("%d:%.2f ", m.vertices[0].bone[k], m.vertices[0].weight[k]);
        std::printf("\n");
    }
    // where do bone indices point in each?
    auto findIdx = [](const assemble::Scene& s, const char* n) { return s.boneIndex(n); };
    for (const char* n : {"cf_j_sk_00_00","cf_j_skt_00","cf_j_spine02","cf_j_f_hips"}) {
        std::printf("%-16s merged=%d layered=%d\n", n, findIdx(merged, n), findIdx(layered, n));
    }
    return 0;
}
