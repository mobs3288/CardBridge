// Scratch: dump DXBC of the cf_m_body (main_skin, shaderIdx=1) and cf_m_bra_npl_00/cf_m_top_inner01 (main_opaque, idx=10) pixel shaders.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/shader/UnityShader.h"
#include "../src/shader/Sm4.h"
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
    for (size_t i = 0; i < scene.shaders.size(); ++i)
        std::printf("shader[%zu] name=%s blob=%zu bytes\n", i, scene.shaders[i].name.c_str(), scene.shaders[i].blob.size());
    auto dump = [&](const char* what, int idx) {
        std::string fn = std::string("C:/Users/pepsi/AppData/Local/Temp/cb_shader_") + what + ".txt";
        FILE* f = std::fopen(fn.c_str(), "w");
        if (!f) return;
        if (idx < 0 || (size_t)idx >= scene.shaders.size()) { std::fprintf(f, "<no shader %d>\n", idx); std::fclose(f); return; }
        auto progs = shader::ParseUnityShaderBlob(Bytes(scene.shaders[(size_t)idx].blob.begin(), scene.shaders[(size_t)idx].blob.end()), nullptr);
        std::printf("%s: %zu programs\n", what, progs.size());
        for (size_t pi = 0; pi < progs.size(); ++pi) {
            shader::Sm4Program sp;
            std::string why;
            if (!shader::DecodeSm4(progs[pi].code.tokens, sp, &why)) {
                std::fprintf(f, "== program %zu (%s) DECODE FAIL: %s\n", pi, progs[pi].isPixel() ? "ps" : "vs", why.c_str());
                continue;
            }
            std::string dis = shader::DisassembleSm4(sp);
            std::fprintf(f, "== program %zu (%s, %zu instrs) ==\n%s\n", pi, progs[pi].isPixel() ? "ps" : "vs", sp.code.size(), dis.c_str());
            if (pi == 2 || pi == 3) {
                for (const auto& rb : progs[pi].code.resources)
                    std::printf("%s prog%zu resource: type=%u bind=%u dim=%u name=%s\n", what, pi, rb.type, rb.bindPoint, rb.dimension, rb.name.c_str());
            }
        }
        std::fclose(f);
        std::printf("dumped %s -> %s\n", what, fn.c_str());
    };
    dump("skin", 1);
    dump("opaque", 10);
    return 0;
}
