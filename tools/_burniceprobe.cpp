// Scratch probe: reproduce the two reported Apply bugs against the real Burnice files,
// offline, and print part-by-part geometry stats. Not committed.
#include "../src/card/CardFile.h"
#include "../src/card/SelectiveImport.h"
#include "../src/card/Abmx.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/core/FileIo.h"
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;


static void dumpScene(const char* tag, const assemble::Scene& s) {
    std::printf("==== %s: %zu parts, %zu meshes, %zu bones\n", tag, s.parts.size(), s.meshes.size(), s.bones.size());
    for (const auto& p : s.parts) {
        size_t tris = 0, verts = 0;
        Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= s.meshes.size()) continue;
            const auto& m = s.meshes[static_cast<size_t>(mi)];
            verts += m.vertices.size();
            size_t st = 0;
            for (const auto& sm : m.submeshes) st += sm.indices.size() / 3;
            tris += st;
            for (const auto& v : m.vertices) {
                lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
                hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
            }
        }
        std::printf("  part %-22s group=%-11s tris=%zu verts=%zu bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) fromClothing=%d underPiece=%d\n",
                    p.id.c_str(), p.group.c_str(), tris, verts,
                    lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                    p.fromClothing ? 1 : 0, p.underPiece ? 1 : 0);
    }
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
    std::fprintf(stderr, "mods issues: %zu\n", issues.size());
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));

    // ABMX tables of interest
    auto abmxInfo = [](const char* tag, const card::Card& c) {
        const card::AbmxTable t = card::LoadAbmx(c.extended);
        size_t mods = 0;
        for (const auto& kv : t.bones) mods += kv.second.size();
        std::printf("%s: ABMX bones=%zu entries=%zu\n", tag, t.bones.size(), mods);
    };
    card::Card c0 = card::LoadCard(cardPath);
    abmxInfo("target card (Owl Burnice)", c0);
    {
        card::ClothingFile cl = card::LoadClothingFile(apronPath);
        const card::AbmxTable t = card::LoadAbmx(cl.extended);
        size_t mods = 0;
        for (const auto& kv : t.bones) mods += kv.second.size();
        std::printf("apron file (Mamako SleepingOver): CLOTHES ABMX bones=%zu entries=%zu name=%s clothesCount=%zu\n",
                    t.bones.size(), mods, cl.name.c_str(), cl.outfit.clothes.isMap() ? cl.outfit.clothes.at("parts").asArray().size() : 0);
    }

    auto assembleWithLayers = [&](const card::Card& card, std::vector<assemble::AssembleLayerSpec> layers) {
        assemble::AssembleOptions ao;
        ao.outfit = 0;
        ao.layers = std::move(layers);
        try { return assemble::Assemble(card, resolver, ao); }
        catch (const std::exception& e) { std::fprintf(stderr, "assemble failed: %s\n", e.what()); throw; }
    };

    // A: the vanilla character, no changes
    dumpScene("A vanilla outfit 0", assembleWithLayers(c0, {}));

    // B: the empty merge pin (decisions = {}) — what SelApply pushed for an AddLayer-only selective import
    {
        card::Card cb = c0;
        card::ApplyClothingOptions opt; opt.outfit = 0;
        try {
            card::ApplySelectiveOutfitSource(cb, apronPath, opt, card::SelectiveDecisions{});
            abmxInfo("after empty pin apply (copy)", cb);
        } catch (const std::exception& e) { std::printf("empty-pin apply threw: %s\n", e.what()); }
        dumpScene("B after empty merge pin (the bug)", assembleWithLayers(cb, {}));
    }

    // C: the correct apply — layer pin only (what the user wants)
    {
        card::Card cd = c0;   // pin loops reload from disk each time; the applied pins on the vanilla card are the empty pin + the layer spec
        assemble::AssembleLayerSpec spec;
        spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
        std::vector<assemble::AssembleLayerSpec> layers{spec};
        dumpScene("C layer only, target ABMX kept (the fix)", assembleWithLayers(cd, std::move(layers)));
    }

    // D: both pins in the order StartLoad applies them (empty merge pin first, then the layer pin) — what the user actually rendered
    {
        card::Card cd = c0;
        card::ApplyClothingOptions opt; opt.outfit = 0;
        try { card::ApplySelectiveOutfitSource(cd, apronPath, opt, card::SelectiveDecisions{}); } catch (...) {}
        assemble::AssembleLayerSpec spec;
        spec.path = apronPath; spec.dstOutfit = 0; spec.name = "apron"; spec.slots = {0}; spec.layerIndex = 1;
        std::vector<assemble::AssembleLayerSpec> layers{spec};
        dumpScene("D empty merge pin + layer pin (what the user got)", assembleWithLayers(cd, std::move(layers)));
    }
    return 0;
}
