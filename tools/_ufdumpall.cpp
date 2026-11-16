// Scratch: dump every object in a bundle by class, with names, and the AssetBundle container table.
//   usage: ufdumpall <bundle>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "unity/UnitySerialized.h"
#include "core/FileIo.h"
#include <cstdio>
#include <map>
#include <string>
using namespace cardbridge;

static const char* className(int id) {
    switch (id) {
        case 1: return "GameObject"; case 4: return "Transform"; case 21: return "Material"; case 23: return "MeshRenderer";
        case 28: return "Texture2D"; case 33: return "MeshFilter"; case 43: return "Mesh";
        case 64: return "MeshCollider"; case 81: return "AudioClip";
        case 91: return "Animator"; case 95: return "AnimatorController"; case 108: return "Shader";
        case 114: return "MonoBehaviour"; case 115: return "MonoScript"; case 137: return "SkinnedMeshRenderer";
        case 142: return "AssetBundle"; case 49: return "ComputeShader"; case 198: return "Cloth"; case 82: return "AudioSource";
        default: return "";
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ufdumpall <bundle>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    std::map<int, std::vector<std::string>> byClass;
    for (const auto& o : uf.serialized().objects()) {
        Value v = uf.serialized().read(o);
        std::string n = v.has("m_Name") ? v.at("m_Name").asStr() : std::string();
        byClass[o.classId].push_back(n);
    }
    for (const auto& [id, names] : byClass) {
        const char* cn = className(id);
        std::fprintf(stderr, "class %d %s: %zu\n", id, cn[0] ? cn : "?", names.size());
        for (const auto& n : names) std::fprintf(stderr, "    %s\n", n.c_str());
    }
    return 0;
}
