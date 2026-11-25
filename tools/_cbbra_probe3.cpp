// Scratch: read the Unity mesh for the bra (o_bra_a) directly and analyze source triangles for duplicates.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/unity/UnityScene.h"
#include "../src/core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace cardbridge;

struct TriKey {
    float p[3][3];
    bool operator<(const TriKey& o) const {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                if (p[i][j] < o.p[i][j] - 1e-6f) return true;
                if (p[i][j] > o.p[i][j] + 1e-6f) return false;
            }
        return false;
    }
};

int main() {
    std::string cardPath = "C:/Users/pepsi/Downloads/139306275_p3.png";
    std::string libRoot = "D:/Project K/cardbridge/library_full";
    std::string modsDir = "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, "", &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));
    card::Card card = card::LoadCard(cardPath);

    for (const auto& ref : resolve::ItemsFromCard(card, 0)) {
        if (ref.partId != "outfit0.bra") continue;
        std::printf("ref: id=%s group=%s\n", ref.partId.c_str(), ref.group.c_str());
        resolve::ResolvedItem ri = resolver.resolve(card, ref);
        std::printf("resolved ok=%d status=%d asset=%s bundle=%s\n", ri.ok(), (int)ri.status, ri.assetName().c_str(), ri.bundleRel().c_str());
        std::string why;
        auto uf = resolver.openBundle(ri, &why);
        if (!uf) { std::printf("openBundle failed: %s\n", why.c_str()); continue; }
        const int64_t rootId = uf->findPrefabRoot(ri.assetName());
        std::printf("prefab root=%lld\n", (long long)rootId);
        for (const auto& r : uf->renderersUnder(rootId)) {
            std::printf("renderer name=%s mesh=%lld nMats=%zu enabled=%d active=%d\n", r.name.c_str(), (long long)r.mesh.pathId, r.materials.size(), r.enabled, r.active);
            auto m = uf->readMesh(r.mesh, &why);
            if (!m) { std::printf("  readMesh failed: %s\n", why.c_str()); continue; }
            std::printf("  mesh %s: positions=%zu subMeshTriangles=%zu subMeshMaterials=%zu\n",
                        m->name.c_str(), m->positions.size(), m->subMeshTriangles.size(), m->subMeshMaterials.size());
            for (size_t s = 0; s < m->subMeshTriangles.size(); ++s) {
                const auto& tris = m->subMeshTriangles[s];
                std::printf("  sub %zu: tris=%zu matIdx=%d\n", s, tris.size() / 3, s < m->subMeshMaterials.size() ? m->subMeshMaterials[s] : -1);
                // per-triangle corner analysis with tight tolerance
                std::map<TriKey, int> counts;
                for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                    TriKey tk{};
                    for (int k = 0; k < 3; ++k) {
                        if (tris[i + k] < m->positions.size()) {
                            tk.p[k][0] = m->positions[tris[i + k]].x;
                            tk.p[k][1] = m->positions[tris[i + k]].y;
                            tk.p[k][2] = m->positions[tris[i + k]].z;
                        } else tk.p[k][0] = tk.p[k][1] = tk.p[k][2] = 1e30f;
                    }
                    ++counts[tk];
                }
                int dups = 0, extra = 0;
                for (const auto& [tk, c] : counts) if (c > 1) { ++dups; extra += c - 1; }
                std::printf("  sub %zu duplicate-analysis: %zu distinct of %zu tris; dup groups=%d extra copies=%d\n",
                            s, counts.size(), tris.size() / 3, dups, extra);
                // dump the duplicated triangles
                for (const auto& [tk, c] : counts) if (c > 1) {
                    std::printf("    DUP tri xyz: (%f,%f,%f) (%f,%f,%f) (%f,%f,%f) count=%d\n",
                                tk.p[0][0], tk.p[0][1], tk.p[0][2], tk.p[1][0], tk.p[1][1], tk.p[1][2], tk.p[2][0], tk.p[2][1], tk.p[2][2], c);
                }
                // count how many of the mesh's triangles are near-degenerate
                size_t degen = 0;
                for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                    Vec3 a = m->positions[tris[i]], b = m->positions[tris[i + 1]], c2 = m->positions[tris[i + 2]];
                    float ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
                    float fx = c2.x - a.x, fy = c2.y - a.y, fz = c2.z - a.z;
                    float l = std::sqrt(ex*ex+ey*ey+ez*ez);
                    (void)l; (void)fx; (void)fy; (void)fz;
                    if (tris[i] == tris[i+1] || tris[i+1] == tris[i+2] || tris[i] == tris[i+2]) ++degen;
                }
                std::printf("  sub %zu: index-identical (degenerate) triangles=%zu\n", s, degen);
            }
        }
    }
    return 0;
}
