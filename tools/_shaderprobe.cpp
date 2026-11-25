// Scratch: probe the pixel programs of a named shader in the assembled scene.
//   usage: shaderprobe <card.png> <library> <mods> <shader name>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "shader/UnityShader.h"
#include "shader/Sm4.h"
#include "shader/Dxbc.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: shaderprobe <card.png> <library> <mods> <shader name>\n"); return 1; }
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
    const std::string want = argv[4];
    for (size_t si = 0; si < scene.shaders.size(); ++si) {
        const auto& sh = scene.shaders[si];
        if (sh.name != want) continue;
        std::fprintf(stderr, "shader[%zu] '%s' blob=%zu\n", si, sh.name.c_str(), sh.blob.size());
        Bytes b = sh.blob;
        std::vector<std::string> why;
        auto progs = shader::ParseUnityShaderBlob(b, &why);
        for (const auto& w : why) std::fprintf(stderr, "parse: %s\n", w.c_str());
        for (size_t i = 0; i < progs.size(); ++i) {
            const auto& p = progs[i];
            std::fprintf(stderr, "== program %zu pixel=%d vertex=%d\n", i, p.isPixel()?1:0, p.isVertex()?1:0);
            for (const auto& o : p.code.outputs)
                std::fprintf(stderr, "   OUT semantic='%s' idx=%u reg=%u mask=%u sv=%u\n", o.semantic.c_str(), o.index, o.reg, o.mask, o.systemValue);
            for (const auto& in : p.code.inputs)
                std::fprintf(stderr, "   IN semantic='%s' idx=%u reg=%u mask=%u sv=%u\n", in.semantic.c_str(), in.index, in.reg, in.mask, in.systemValue);
            for (const auto& tb : p.textures)
                std::fprintf(stderr, "   TEX reg=%u name='%s'\n", tb.reg, tb.name.c_str());
            for (const auto& cb : p.cbuffers) {
                std::fprintf(stderr, "   CB slot=%d size=%u params=%zu\n", cb.slot, cb.size, cb.params.size());
                for (const auto& pm : cb.params)
                    std::fprintf(stderr, "      cb[%u] '%s'\n", pm.offset / 16, pm.name.c_str());
            }
            if (p.isPixel()) {
                shader::Sm4Program ps;
                std::string decodeWhy;
                if (shader::DecodeSm4(p.code.tokens, ps, &decodeWhy)) {
                    std::string dis = shader::DisassembleSm4(ps);
                    std::fprintf(stdout, "%s\n", dis.c_str());
                } else std::fprintf(stderr, "   decode failed: %s\n", decodeWhy.c_str());
            }
        }
        return 0;
    }
    std::fprintf(stderr, "shader '%s' not found\n", want.c_str());
    return 3;
}
