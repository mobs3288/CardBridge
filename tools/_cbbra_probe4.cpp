// Scratch: analyze the ASSEMBLED scene's o_bra_a / o_bra_a.under for geometric duplicates.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <map>
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

static TriKey triKey(const assemble::SceneMesh& m, const std::vector<uint32_t>& idxs) {
    TriKey tk{};
    for (int k = 0; k < 3; ++k)
        if (idxs[k] < m.vertices.size()) {
            tk.p[k][0] = m.vertices[idxs[k]].pos.x;
            tk.p[k][1] = m.vertices[idxs[k]].pos.y;
            tk.p[k][2] = m.vertices[idxs[k]].pos.z;
        } else tk.p[k][0] = tk.p[k][1] = tk.p[k][2] = 1e30f;
    return tk;
}

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
    assemble::AssembleOptions ao; ao.outfit = 0;
    assemble::Scene scene = assemble::Assemble(card, resolver, ao);

    // Gather the triangles of every scene mesh named o_bra_a or o_bra_a.under into separate lists,
    // and analyze duplicates within each, and across them.
    std::vector<TriKey> aTris, aUnderTris;
    for (const auto& m : scene.meshes) {
        for (const auto& sm : m.submeshes)
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                TriKey tk = triKey(m, {sm.indices[i], sm.indices[i + 1], sm.indices[i + 2]});
                if (m.name == "o_bra_a") aTris.push_back(tk);
                else if (m.name == "o_bra_a.under") aUnderTris.push_back(tk);
            }
    }
    std::printf("scene o_bra_a tris=%zu  o_bra_a.under tris=%zu\n", aTris.size(), aUnderTris.size());

    auto analyze = [](const char* name, const std::vector<TriKey>& v) {
        std::map<TriKey, int> counts;
        for (const auto& tk : v) ++counts[tk];
        int dups = 0, extra = 0;
        for (const auto& [tk, c] : counts) if (c > 1) { ++dups; extra += c - 1; }
        std::printf("%s: distinct=%zu of %zu; dup groups=%d extra copies=%d\n", name, counts.size(), v.size(), dups, extra);
        for (const auto& [tk, c] : counts) if (c > 1)
            std::printf("  DUP (%f,%f,%f) (%f,%f,%f) (%f,%f,%f) x%d\n",
                        tk.p[0][0], tk.p[0][1], tk.p[0][2], tk.p[1][0], tk.p[1][1], tk.p[1][2], tk.p[2][0], tk.p[2][1], tk.p[2][2], c);
    };
    analyze("o_bra_a", aTris);
    analyze("o_bra_a.under", aUnderTris);

    // Cross-mesh: same geometric triangle present in BOTH o_bra_a and o_bra_a.under?
    std::set<TriKey> inA;
    for (const auto& tk : aTris) inA.insert(tk);
    int cross = 0;
    for (const auto& tk : aUnderTris) if (inA.count(tk)) ++cross;
    std::printf("cross-mesh (o_bra_a triangle also in o_bra_a.under): %d\n", cross);

    // Scene totals around the bra region
    for (const auto& p : scene.parts)
        if (p.id.find("bra") != std::string::npos)
            for (int mi2 : p.meshes) {
                const auto& m = scene.meshes[(size_t)mi2];
                size_t tris = 0; for (const auto& sm : m.submeshes) tris += sm.indices.size() / 3;
                std::printf("part %-28s mesh %-28s tris=%zu\n", p.id.c_str(), m.name.c_str(), tris);
            }
    return 0;
}
