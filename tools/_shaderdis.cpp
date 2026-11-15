// Scratch: disassemble all programs of a shader in a bundle.
//   usage: shaderdis <bundle> <shader name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "shader/UnityShader.h"
#include "shader/Sm4.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: shaderdis <bundle> <shader name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    const std::string want = argv[2];
    for (const auto* o : uf.serialized().ofClass(48)) {
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
        if (n.empty()) n = v.at("m_Name").asStr();
        if (n != want) continue;
        auto blob = uf.readShaderBlob(o->pathId, nullptr);
        if (!blob) { std::fprintf(stderr, "no blob\n"); return 2; }
        for (const auto& d : uf.readShaderTextureDefaults(o->pathId))
            std::fprintf(stderr, "texDefault %s -> %s\n", d.first.c_str(), d.second.c_str());
        Bytes b(blob->begin(), blob->end());
        auto progs = shader::ParseUnityShaderBlob(b, nullptr);
        std::fprintf(stderr, "programs: %zu\n", progs.size());
        for (size_t i = 0; i < progs.size(); ++i) {
            const auto& p = progs[i];
            std::fprintf(stderr, "== program %zu isPixel=%d isVertex=%d tokens=%zu\n", i, p.isPixel() ? 1 : 0, p.isVertex() ? 1 : 0, p.code.tokens.size());
            if (p.isPixel()) {
                shader::Sm4Program ps;
                if (shader::DecodeSm4(p.code.tokens, ps, nullptr)) {
                    std::string dis = shader::DisassembleSm4(ps);
                    std::fprintf(stdout, "%s\n", dis.c_str());
                } else std::fprintf(stderr, "decode failed\n");
            }
        }
        return 0;
    }
    std::fprintf(stderr, "shader '%s' not found\n", want.c_str());
    return 3;
}
