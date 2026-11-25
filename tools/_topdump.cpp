// Scratch: dump the source renderers/meshes of a resolved card item.
//   usage: topdump <card.png> <library> [mods] [outfit]
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

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: topdump <card.png> <library> [mods] [outfit]\n"); return 1; }
    const std::string cardPath = S(argv[1]), library = S(argv[2]);
    const std::string mods = argc >= 4 ? S(argv[3]) : std::string();
    int outfit = argc >= 5 ? std::atoi(argv[4]) : -1;
    card::Card c;
    try { c = card::LoadCard(cardPath); } catch (const std::exception& e) { std::printf("card: %s\n", e.what()); return 2; }
    resolve::ModIndex mi;
    if (!mods.empty()) {
        std::vector<std::string> iss;
        mi = resolve::ModIndex::Build(mods, std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/topdump_modcache.json", &iss, nullptr, [](size_t, size_t) {});
    }
    resolve::ListTable lists;
    std::vector<std::string> iss;
    lists.loadBaseGame(library::AbdataRoot(library), &iss);
    auto resolver = std::make_unique<resolve::Resolver>(mi, lists, library::AbdataRoot(library));
    if (outfit < 0) outfit = static_cast<int>(c.status.at("coordinateType").asInt());
    std::vector<resolve::ItemRef> refs = resolve::ItemsFromCard(c, outfit);
    const Value& subIds = c.outfits[(size_t)outfit].clothes.at("subPartsId");
    for (int k = 0; k < 3; ++k) {
        resolve::ItemRef s;
        s.partId = "outfit" + std::to_string(outfit) + ".top.sub_" + std::string(1, (char)('a'+k));
        s.group = "Outfit"; s.category = 200 + k; s.id = subIds.at((size_t)k).asInt(); s.slot = 0; s.subKind = k; s.outfit = outfit;
        refs.push_back(s);
    }
    for (const auto& ref : refs) {
        if (ref.group != "Outfit") continue;
        auto item = resolver->resolve(c, ref);
        std::printf("\n=== %s cat=%d id=%lld slot=%d status=%s asset='%s' bundle='%s' ===\n", ref.partId.c_str(),
                    ref.category, (long long)ref.id, ref.slot, resolve::StatusName(item.status),
                    item.assetName().c_str(), item.bundleRel().c_str());
        std::string why;
        auto uf = resolver->openBundle(item, &why);
        if (!uf) { std::printf("  no bundle: %s\n", why.c_str()); continue; }

        // all prefab roots
        for (const auto& [id, xf] : uf->xforms()) {
            if (xf.parent != 0) continue;
            auto rs = uf->renderersUnder(id);
            if (rs.empty()) continue;
            std::printf("  ROOT id=%lld name='%s'\n", (long long)id, xf.name.c_str());
            for (const auto& r : rs) {
                std::printf("    RENDERER %-34s skinned=%d enabled=%d active=%d bones=%zu mats=%zu\n", r.name.c_str(),
                            r.skinned ? 1 : 0, r.enabled ? 1 : 0, r.active ? 1 : 0, r.boneXforms.size(), r.materials.size());
                for (size_t i = 0; i < r.boneXforms.size() && i < 24; ++i) {
                    const auto* bx = uf->xform(r.boneXforms[i]);
                    std::printf("       bone[%zu] %lld '%s'\n", i, (long long)r.boneXforms[i], bx ? bx->name.c_str() : "<missing>");
                }
                std::string mwhy;
                auto mesh = uf->readMesh(r.mesh, &mwhy);
                if (!mesh) { std::printf("       mesh: %s\n", mwhy.c_str()); continue; }
                std::printf("       MESH '%s' verts=%zu skin=%zu bindPoses=%zu submeshes=%zu\n", mesh->name.c_str(),
                            mesh->positions.size(), mesh->skin.size(), mesh->bindPoses.size(), mesh->subMeshTriangles.size());
                if (!mesh->skin.empty())
                    for (size_t v = 0; v < 3 && v < mesh->skin.size(); ++v)
                        std::printf("         skin[%zu] idx=(%d %d %d %d) w=(%.2f %.2f %.2f %.2f)\n", v,
                                    mesh->skin[v].idx[0], mesh->skin[v].idx[1], mesh->skin[v].idx[2], mesh->skin[v].idx[3],
                                    mesh->skin[v].w[0], mesh->skin[v].w[1], mesh->skin[v].w[2], mesh->skin[v].w[3]);
                else {
                    std::printf("         (no skin data) -- raw mesh object fields:\n");
                    const auto* oi = uf->serialized().find(r.mesh.pathId);
                    if (oi) {
                        Value v = uf->serialized().read(*oi);
                        for (const auto& kv : v.asMap()) {
                            std::string k = kv.first.isStr() ? kv.first.asStr() : std::to_string(kv.first.asInt());
                            if (k == "m_Skin") std::printf("           m_Skin array len=%zu\n", kv.second.asArray().size());
                            else if (k == "m_VertexData") {
                                const auto& vd = kv.second;
                                std::printf("           m_VertexData channels=%zu count=%lld keys:", vd.at("m_Channels").asArray().size(), (long long)vd.at("m_VertexCount").asInt());
                                for (const auto& ck : vd.asMap()) { std::string cks = ck.first.isStr()?ck.first.asStr():"?"; if(cks!="m_Channels"&&cks!="m_DataSize") std::printf(" %s", cks.c_str()); }
                                std::printf("\n");
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}



