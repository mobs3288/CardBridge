// Scratch: print the shader names used by o_body_a/cf_m_body, o_bra_a/cf_m_bra_npl_00, o_inner_a/cf_m_top_inner01.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/unity/UnityScene.h"
#include "../src/unity/UnityBundle.h"
#include "../src/core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main() {
    std::string cardPath = "C:/Users/pepsi/Downloads/139306275_p3.png";
    std::string libRoot = "D:/Project K/cardbridge/library_full";
    std::string modsDir = "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, "", &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));
    card::Card card = card::LoadCard(cardPath);
    assemble::AssembleOptions ao; ao.outfit = 0;
    assemble::Scene scene = assemble::Assemble(card, resolver, ao);
    for (const char* want : {"cf_m_body", "cf_m_bra_npl_00", "cf_m_top_inner01", "cf_m_top_sailor05_02", "cf_m_misya_jacket_top"}) {
        const assemble::SceneMaterial* found = nullptr;
        for (const auto& m : scene.materials) if (m.name == want) { found = &m; break; }
        if (!found) { std::printf("%-24s not found in scene materials\n", want); continue; }
        std::printf("%-24s shader=%s shaderIdx=%d\n", want, found->shader.c_str(), found->shaderIndex);
    }
    return 0;
}
