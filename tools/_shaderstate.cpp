// Scratch: list shader render state (queue / zWrite) for shader names matching a filter.
//   usage: shaderstate <abdata-dir> [substr]
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: shaderstate <abdata-dir> [substr]\n"); return 1; }
    const std::string want = argc > 2 ? argv[2] : "";
    for (const auto& f : fileio::ListFiles(argv[1], {".unity3d"})) {
        auto bundle = unity::UnityBundle::Open(fileio::ReadFile(f), f);
        if (!bundle) continue;
        try {
            unity::UnityFile uf(bundle);
            for (const auto* o : uf.serialized().ofClass(48)) {   // Shader
                unity::ShaderRenderState st;
                if (!uf.readShaderRenderState(o->pathId, st)) continue;
                Value v = uf.serialized().read(*o);
                std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
                if (n.empty()) n = v.at("m_Name").asStr();
                if (!want.empty() && n.find(want) == std::string::npos) continue;
                std::printf("%-60s queue=%d zWrite=%d cull=%d  (%s)\n", n.c_str(), st.queue, st.zWrite ? 1 : 0, st.cull, fileio::FileName(f).c_str());
            }
        } catch (...) {}
    }
    return 0;
}
