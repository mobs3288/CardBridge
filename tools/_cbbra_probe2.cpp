// Scratch: dump the chest-region triangle list of o_inner_a / o_bra_a / o_bra_a.under / body parts.
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
#include <string>
#include <vector>
using namespace cardbridge;

static void dumpTris(const assemble::Scene& s, const assemble::SceneMesh& m, const char* tag, float ymin) {
    for (size_t si = 0; si < m.submeshes.size(); ++si) {
        const auto& sm = m.submeshes[si];
        int mat = sm.material;
        const char* mname = "<bad>";
        if (mat >= 0 && (size_t)mat < s.materials.size()) mname = s.materials[(size_t)mat].name.c_str();
        std::printf("MESH %-28s SUB %zu mat=%-28s tris=%zu\n", tag, si, mname, sm.indices.size() / 3);
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
            Vec3 p[3];
            for (int k = 0; k < 3; ++k) p[k] = m.vertices[sm.indices[i + k]].pos;
            float ymax = std::max({p[0].y, p[1].y, p[2].y});
            if (ymax < ymin) continue;
            float area = std::fabs((p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x));
            float area3 = area + (p[2].y - p[1].y) * (p[0].x - p[1].x) - (p[2].x - p[1].x) * (p[0].y - p[1].y);
            (void)area3;
            std::printf("  tri idx=%zu v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)  tri=(%.3f,%.3f,%.3f) area2d=%.5f\n",
                        sm.indices[i] / 1, p[0].x, p[0].y, p[0].z, p[1].x, p[1].y, p[1].z, p[2].x, p[2].y, p[2].z,
                        p[0].x, p[0].y, p[0].z, area);
        }
    }
}

int main(int argc, char** argv) {
    std::string cardPath = "C:/Users/pepsi/Downloads/139306275_p3.png";
    std::string libRoot  = "D:/Project K/cardbridge/library_full";
    std::string modsDir  = "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, "", &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));
    card::Card card = card::LoadCard(cardPath);
    assemble::AssembleOptions ao; ao.outfit = 0;
    assemble::Scene scene = assemble::Assemble(card, resolver, ao);
    std::printf("SCENE %zu parts %zu meshes %zu materials %zu textures\n", scene.parts.size(), scene.meshes.size(), scene.materials.size(), scene.textures.size());
    for (const auto& p : scene.parts) {
        for (int meshIdx : p.meshes) {
            const auto& m = scene.meshes[(size_t)meshIdx];
            if (m.name.rfind("o_inner_a", 0) == 0 || m.name.rfind("o_bra_a", 0) == 0) {
                std::printf("PART %s MESH %s\n", p.id.c_str(), m.name.c_str());
                dumpTris(scene, m, p.id.c_str(), 1.0f);
            }
        }
    }
    return 0;
}
