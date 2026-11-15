// Scratch: dump a shader pass's raw state map (m_ParsedForm.m_SubShaders[0].m_Passes[i].m_State).
//   usage: state <bundle> <shader name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include "core/Value.h"
#include "core/MsgPack.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

static void dumpValue(const Value& v, int depth) {
    for (int i = 0; i < depth; ++i) std::fprintf(stderr, "  ");
    if (v.isMap()) {
        std::fprintf(stderr, "{\n");
        for (const auto& kv : v.asMap()) {
            for (int i = 0; i < depth + 1; ++i) std::fprintf(stderr, "  ");
            std::fprintf(stderr, "%s:\n", kv.first.asStr().c_str());
            dumpValue(kv.second, depth + 1);
        }
        for (int i = 0; i < depth; ++i) std::fprintf(stderr, "  ");
        std::fprintf(stderr, "}\n");
        return;
    }
    if (v.isArray()) { std::fprintf(stderr, "[%zu elements]\n", v.asArray().size()); return; }
    if (v.isInt()) { std::fprintf(stderr, "%lld\n", (long long)v.asInt()); return; }
    if (v.isFloat()) { std::fprintf(stderr, "%.3f\n", (double)v.asFloat()); return; }
    if (v.isBool()) { std::fprintf(stderr, "%s\n", v.asBool() ? "true" : "false"); return; }
    if (v.isStr()) { std::fprintf(stderr, "\"%s\"\n", v.asStr().c_str()); return; }
    if (v.isNil()) { std::fprintf(stderr, "nil\n"); return; }
    if (v.isBin()) { std::fprintf(stderr, "<bin %zu bytes>\n", v.asBin().size()); return; }
    std::fprintf(stderr, "<other>\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: state <bundle> <shader name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    const std::string want = argv[2];
    for (const auto* o : uf.serialized().ofClass(48)) {   // Shader
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
        if (n.empty()) n = v.at("m_Name").asStr();
        if (n != want) continue;
        const auto& subs = v.at("m_ParsedForm").at("m_SubShaders").asArray();
        std::fprintf(stderr, "shader '%s' subshaders %zu\n", want.c_str(), subs.size());
        for (const auto& sub : subs) {
            const auto& passes = sub.at("m_Passes").asArray();
            std::fprintf(stderr, "passes %zu\n", passes.size());
            for (const auto& p : passes) {
                std::fprintf(stderr, "== pass %s\n", p.at("m_Name").asStr().c_str());
                dumpValue(p.at("m_State"), 0);
                std::fprintf(stderr, "\n");
            }
        }
        return 0;
    }
    std::fprintf(stderr, "shader '%s' not found\n", want.c_str());
    return 3;
}
