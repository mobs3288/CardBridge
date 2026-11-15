// Scratch: list the shaders in an asset bundle (class 48).
//   usage: shaderdump <bundle>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "unity/UnitySerialized.h"
#include "core/FileIo.h"
#include "core/Value.h"
#include "core/MsgPack.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: shaderdump <bundle>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) { std::fprintf(stderr, "cannot open bundle\n"); return 1; }
    unity::UnityFile uf(bundle);
    for (const auto* o : uf.serialized().ofClass(48)) {   // Shader
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
        if (n.empty()) n = v.at("m_Name").asStr();
        std::fprintf(stderr, "shader '%s' pathId=%lld\n", n.c_str(), (long long)o->pathId);
    }
    return 0;
}
