// Scratch: dump o_bra_a triangles; report exact and near duplicates (max corner distance per pair).
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
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace cardbridge;

struct Tri { int idx; Vec3 p[3]; };

static float triDist(const Tri& a, const Tri& b) {
    float d = 0;
    for (int i = 0; i < 3; ++i) {
        float dd = std::sqrt(std::max({a.p[i].x - b.p[i].x, a.p[i].y - b.p[i].y, a.p[i].z - b.p[i].z}) * 0); (void)dd;
        float best = 1e30f;
        for (int j = 0; j < 3; ++j) {
            float dx = a.p[i].x - b.p[j].x, dy = a.p[i].y - b.p[j].y, dz = a.p[i].z - b.p[j].z;
            best = std::min(best, std::sqrt(dx*dx+dy*dy+dz*dz));
        }
        d = std::max(d, best);
    }
    return d;
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
    for (const auto& m : scene.meshes) {
        if (m.name != "o_bra_a") continue;
        std::printf("mesh %s: %zu submeshes\n", m.name.c_str(), m.submeshes.size());
        std::vector<Tri> tris;
        for (const auto& sm : m.submeshes)
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                Tri t; t.idx = i;
                for (int k = 0; k < 3; ++k) t.p[k] = m.vertices[sm.indices[i + k]].pos;
                tris.push_back(t);
            }
        std::printf("o_bra_a: %zu triangles\n", tris.size());
        int exactDups = 0;
        for (size_t i = 0; i < tris.size(); ++i)
            for (size_t j = i + 1; j < tris.size(); ++j) {
                float d = triDist(tris[i], tris[j]);
                if (d < 1e-6f) {
                    ++exactDups;
                    if (exactDups <= 30)
                        std::printf("EXACT-DUP tri[%zu] vs tri[%zu]: (%.3f,%.3f,%.3f) d=%.6f\n",
                                    i, j, tris[i].p[0].x, tris[i].p[0].y, tris[i].p[0].z, d);
                }
            }
        std::printf("exact dups: %d\n", exactDups);
        int nearDups = 0;
        for (size_t i = 0; i < tris.size(); ++i)
            for (size_t j = i + 1; j < tris.size(); ++j) {
                float d = triDist(tris[i], tris[j]);
                if (d > 1e-6f && d < 5e-4f) {
                    ++nearDups;
                    if (nearDups <= 30)
                        std::printf("NEAR-DUP tri[%zu] vs tri[%zu]: d=%.6f triA=(%.3f,%.3f,%.3f) triB=(%.3f,%.3f,%.3f)\n",
                                    i, j, d,
                                    tris[i].p[0].x, tris[i].p[0].y, tris[i].p[0].z,
                                    tris[j].p[0].x, tris[j].p[0].y, tris[j].p[0].z);
                }
            }
        std::printf("near dups (<5e-4): %d\n", nearDups);
    }
    return 0;
}
