// Scratch: dump FaceBlendShape EyebrowCtrl targets for the head item.
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "unity/UnityScene.h"
#include <cstdio>
#include <memory>
#include <string>
using namespace cardbridge;
static std::string S(const char* s) { std::string o = s; for (auto& c : o) if (c == '\\') c = '/'; return o; }

static void dumpTargets(const char* ctrl, const Value& v, const std::map<int64_t, std::string>& goName) {
    std::printf("  %s FBSTargets=%zu\n", ctrl, v.at("FBSTarget").asArray().size());
    for (const auto& tg : v.at("FBSTarget").asArray()) {
        int64_t go = tg.at("ObjTarget").at("m_PathID").asInt();
        auto it = goName.find(go);
        std::printf("    target %lld '%s' PtnSets=%zu\n", (long long)go, it == goName.end() ? "?" : it->second.c_str(), tg.at("PtnSet").asArray().size());
        const auto& ps = tg.at("PtnSet").asArray();
        for (size_t p = 0; p < ps.size() && p < 3; ++p)
            std::printf("       ptn%zu close=%lld open=%lld\n", p, (long long)ps[p].at("Close").asInt(), (long long)ps[p].at("Open").asInt());
    }
}
int main(int argc, char** argv) {
    if (argc < 3) return 1;
    card::Card c = card::LoadCard(S(argv[1]));
    resolve::ModIndex mi;
    std::vector<std::string> iss;
    if (argc >= 4) mi = resolve::ModIndex::Build(S(argv[3]), std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/fbs_modcache.json", &iss, nullptr, [](size_t, size_t) {});
    resolve::ListTable lists; lists.loadBaseGame(library::AbdataRoot(S(argv[2])), &iss);
    auto r = std::make_unique<resolve::Resolver>(mi, lists, library::AbdataRoot(S(argv[2])));
    int outfit = static_cast<int>(c.status.at("coordinateType").asInt());
    resolve::ItemRef hr;
    for (const auto& it : resolve::ItemsFromCard(c, outfit)) if (it.group == "Head") hr = it;
    auto item = r->resolve(c, hr);
    std::printf("head asset='%s' bundle='%s' status=%s\n", item.assetName().c_str(), item.bundleRel().c_str(), resolve::StatusName(item.status));
    std::string why; auto uf = r->openBundle(item, &why);
    if (!uf) { std::printf("no bundle: %s\n", why.c_str()); return 2; }
    std::map<int64_t, std::string> goName;
    for (const auto& [id, x] : uf->xforms()) goName[x.goId] = x.name;
    int64_t root = uf->findPrefabRoot(item.assetName());
    std::printf("head prefab root=%lld\n", (long long)root);
    for (const auto& mb : uf->monoBehavioursUnder(root))
        if (mb.className == "FaceBlendShape") {
            std::printf("FaceBlendShape on go %lld\n", (long long)mb.goId);
            dumpTargets("EyebrowCtrl", mb.fields.at("EyebrowCtrl"), goName);
            dumpTargets("EyesCtrl", mb.fields.at("EyesCtrl"), goName);
        }
    return 0;
}
