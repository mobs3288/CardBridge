// Scratch: list every MonoBehaviour (class + game object path) under a prefab, with DynamicBone details.
//   usage: mbd <bundle> <prefab-go-name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include "core/Value.h"
#include <cstdio>
#include <string>
#include <functional>
#include <sstream>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: mbd <bundle> <prefab-go-name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) { std::fprintf(stderr, "cannot open bundle\n"); return 1; }
    unity::UnityFile uf(bundle);
    std::string want = argv[2];
    int64_t root = uf.findPrefabRoot(want);
    if (!root) { std::fprintf(stderr, "prefab root '%s' not found\n", want.c_str()); return 1; }
    std::fprintf(stderr, "prefab root: %s\n", uf.pathOf(root).c_str());
    if (const char* lookup = std::getenv("CARDBRIDGE_MB_FIND")) {
        std::string cur;
        std::stringstream ss(lookup);
        int64_t id = 0;
        while (ss >> id) {
            const unity::Xform* x = uf.xform(id);
            std::fprintf(stderr, "id %lld -> %s\n", (long long)id, x ? uf.pathOf(id).c_str() : "(not a transform or GameObject)");
        }
        return 0;
    }
    if (std::getenv("CARDBRIDGE_MB_RAW")) {
        std::function<void(const Value&, int)> dump = [&](const Value& v, int depth) {
            auto indent = [&]() { for (int i = 0; i < depth; ++i) std::fprintf(stderr, "  "); };
            if (v.isMap()) {
                std::fprintf(stderr, "{\n");
                for (const auto& kv : v.asMap()) {
                    indent();
                    if (kv.first.isStr()) std::fprintf(stderr, "%s: ", kv.first.asStr().c_str());
                    else if (kv.first.isInt()) std::fprintf(stderr, "%lld: ", (long long)kv.first.asInt());
                    dump(kv.second, depth + 1);
                }
                indent();
                std::fprintf(stderr, "}\n");
            } else if (v.isArray()) {
                std::fprintf(stderr, "[\n");
                for (const auto& e : v.asArray()) { indent(); dump(e, depth + 1); }
                indent();
                std::fprintf(stderr, "]\n");
            } else if (v.isStr()) {
                std::fprintf(stderr, "'%s'\n", v.asStr().c_str());
            } else if (v.isInt()) {
                std::fprintf(stderr, "%lld\n", (long long)v.asInt());
            } else if (v.isFloat()) {
                std::fprintf(stderr, "%g\n", v.asFloat());
            } else if (v.isBool()) {
                std::fprintf(stderr, "%s\n", v.asBool() ? "true" : "false");
            } else if (v.isBin()) {
                std::fprintf(stderr, "<bin %zu bytes>\n", v.asBin().size());
            } else {
                std::fprintf(stderr, "<nil>\n");
            }
        };
        for (const auto& mb : uf.monoBehavioursUnder(root)) {
            std::fprintf(stderr, "== mb %s go=%lld\n", mb.className.c_str(), (long long)mb.goId);
            dump(mb.fields, 1);
        }
        return 0;
    }
    for (const auto& mb : uf.monoBehavioursUnder(root)) {
        std::fprintf(stderr, "mb %-28s go=%lld path=%s\n", mb.className.c_str(), (long long)mb.goId, uf.pathOf(mb.goId).c_str());
        const Value& f = mb.fields;
        if (mb.className == "DynamicBone") {
            auto v3 = [&](const char* k) { const Value& v = f.at(k); return std::to_string(v.at("x").asFloat()) + "," + std::to_string(v.at("y").asFloat()) + "," + std::to_string(v.at("z").asFloat()); };
            std::fprintf(stderr, "    root=%lld updateRate=%g damping=%g elasticity=%g stiffness=%g inert=%g radius=%g endLength=%g freezeAxis=%lld colliders=%zu\n",
                         (long long)f.at("m_Root").at("m_PathID").asInt(), f.at("m_UpdateRate").asFloat(), f.at("m_Damping").asFloat(),
                         f.at("m_Elasticity").asFloat(), f.at("m_Stiffness").asFloat(), f.at("m_Inert").asFloat(),
                         f.at("m_Radius").asFloat(), f.at("m_EndLength").asFloat(), (long long)f.at("m_FreezeAxis").asInt(),
                         f.at("m_Colliders").asArray().size());
            for (const auto& c : f.at("m_Colliders").asArray()) std::fprintf(stderr, "    collider fileID=%lld pathID=%lld\n", (long long)c.at("m_FileID").asInt(), (long long)c.at("m_PathID").asInt());
            std::fprintf(stderr, "    endOffset=%s gravity=%s\n", v3("m_EndOffset").c_str(), v3("m_Gravity").c_str());
        } else if (mb.className == "DynamicBoneCollider") {
            auto v3 = [&](const char* k) { const Value& v = f.at(k); return std::to_string(v.at("x").asFloat()) + "," + std::to_string(v.at("y").asFloat()) + "," + std::to_string(v.at("z").asFloat()); };
            std::fprintf(stderr, "    radius=%g height=%g direction=%d center=%s\n", f.at("m_Radius").asFloat(), f.at("m_Height").asFloat(), (int)f.at("m_Direction").asInt(), v3("m_Center").c_str());
        } else if (mb.className == "DynamicBoneColliderBase") {
            auto v3 = [&](const char* k) { const Value& v = f.at(k); return std::to_string(v.at("x").asFloat()) + "," + std::to_string(v.at("y").asFloat()) + "," + std::to_string(v.at("z").asFloat()); };
            std::fprintf(stderr, "    radius=%g height=%g direction=%d center=%s\n", f.at("m_Radius").asFloat(), f.at("m_Height").asFloat(), (int)f.at("m_Direction").asInt(), v3("m_Center").c_str());
        }
    }
    return 0;
}
