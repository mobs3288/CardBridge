// Scratch: for each scene shader, report whether the forward pixel pass has parsed cbuffers.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "shader/UnityShader.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: cbaudit <card.png> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists;
    library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao;
    ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    size_t empty = 0;
    for (size_t si = 0; si < scene.shaders.size(); ++si) {
        const auto& sh = scene.shaders[si];
        Bytes b = sh.blob;
        auto progs = shader::ParseUnityShaderBlob(b, nullptr);
        size_t pi = 0, vi = 0;
        if (!shader::PickForwardPass(progs, pi, vi)) { std::printf("shader[%zu] '%s' NO PASS\n", si, sh.name.c_str()); continue; }
        const auto& pp = progs[pi];
        if (pp.cbuffers.empty()) ++empty;
        std::printf("shader[%zu] '%.60s' cb=%zu tex=%zu instr=%zu%s\n", si, sh.name.c_str(), pp.cbuffers.size(), pp.textures.size(), pp.code.tokens.size(), pp.cbuffers.empty() ? "  <-- NO CB" : "");
    }
    std::printf("TOTAL %zu shaders, %zu with no parsed cbuffers\n", scene.shaders.size(), empty);
    return 0;
}
