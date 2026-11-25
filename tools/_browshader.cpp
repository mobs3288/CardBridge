// Scratch: disassemble the eyebrow material's forward pixel shader from an assembled scene.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "shader/Sm4.h"
#include "shader/UnityShader.h"
#include <cstdio>
#include <memory>
#include <string>
using namespace cardbridge;
static std::string S(const char* s) { std::string o = s; for (auto& c : o) if (c == '\\') c = '/'; return o; }

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    card::Card c = card::LoadCard(S(argv[1]));
    resolve::ModIndex mi;
    std::vector<std::string> iss;
    if (argc >= 4) mi = resolve::ModIndex::Build(S(argv[3]), std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/browshader_modcache.json", &iss, nullptr, [](size_t, size_t) {});
    resolve::ListTable lists; lists.loadBaseGame(library::AbdataRoot(S(argv[2])), &iss);
    auto r = std::make_unique<resolve::Resolver>(mi, lists, library::AbdataRoot(S(argv[2])));
    assemble::AssembleOptions ao;
    assemble::Scene scene = assemble::Assemble(c, *r, ao);
    int si = -1;
    for (const auto& m : scene.materials) if (m.name.find("mayuge") != std::string::npos || m.name.find("brow") != std::string::npos) { si = m.shaderIndex; std::printf("material %s shaderIndex %d\n", m.name.c_str(), si); }
    if (si < 0 || (size_t)si >= scene.shaders.size()) { std::printf("no shader\n"); return 2; }
    const auto& sh = scene.shaders[(size_t)si];
    std::printf("shader %s blob=%zu cull=%d zWrite=%d queue=%d\n", sh.name.c_str(), sh.blob.size(), sh.cull, sh.zWrite ? 1 : 0, sh.queue);
    std::vector<std::string> why;
    auto progs = shader::ParseUnityShaderBlob(sh.blob, &why);
    std::printf("programs=%zu\n", progs.size());
    for (const auto& w : why) std::printf("  why: %s\n", w.c_str());
    size_t pi = 0, vi = 0;
    bool pf = shader::PickForwardPass(progs, pi, vi);
    std::printf("forward=%d pixel=%zu vertex=%zu\n", pf ? 1 : 0, pi, vi);
    if (pi < progs.size()) {
        const auto& p = progs[pi];
        std::printf("pixel keywords:"); for (const auto& k : p.keywords) std::printf(" %s", k.c_str()); std::printf("\n");
        for (const auto& cb : p.cbuffers) {
            std::printf("cbuffer %s slot=%d size=%u\n", cb.name.c_str(), cb.slot, cb.size);
            for (const auto& pa : cb.params) std::printf("   %-24s off=%u rows=%u cols=%u mat=%d\n", pa.name.c_str(), pa.offset, pa.rows, pa.cols, pa.isMatrix ? 1 : 0);
        }
        for (const auto& t : p.textures) std::printf("texture %-20s t%u s%u dim=%u\n", t.name.c_str(), t.reg, t.sampler, t.dimension);
        shader::Sm4Program sm; std::string sw;
        if (shader::DecodeSm4(p.code.tokens, sm, &sw)) {
            std::string dis = shader::DisassembleSm4(sm);
            std::printf("--- DISASM ---\n%s\n--- END ---\n", dis.c_str());
        } else std::printf("sm4 decode failed: %s\n", sw.c_str());
    }
    return 0;
}
