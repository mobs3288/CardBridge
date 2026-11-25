// Diagnostic only: dump the skirt mesh's dominant bones, the ABMX table entries and the bone
// world scales, so the live viewer's short-skirt vs. the long PMX geometry can be explained.
// usage: probeskirt <card.png> <library> <mods>
#include "assemble/Assembler.h"
#include "card/Abmx.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

static float colLen(const Mat4& m, int c) { return std::sqrt(m.m[c] * m.m[c] + m.m[4 + c] * m.m[4 + c] + m.m[8 + c] * m.m[8 + c]); }

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: probeskirt <card> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    const int coordType = static_cast<int>(c.status.at("coordinateType").asInt());
    std::fprintf(stderr, "coordinateType=%d\n", coordType);

    const card::AbmxTable abmx = card::LoadAbmx(c.extended);
    std::fprintf(stderr, "ABMX bones=%zu\n", abmx.bones.size());
    for (const auto& kv : abmx.bones) {
        if (kv.first.rfind("cf_d_sk", 0) != 0 && kv.first.rfind("cf_j_sk", 0) != 0) continue;
        std::fprintf(stderr, "  ABMX '%s' entries=%zu\n", kv.first.c_str(), kv.second.size());
        for (size_t i = 0; i < kv.second.size(); ++i) {
            const auto& m = kv.second[i];
            std::fprintf(stderr, "     coord %zu scale=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f) len=%.3f\n", i,
                         m.scale.x, m.scale.y, m.scale.z, m.position.x, m.position.y, m.position.z, m.rotation.x, m.rotation.y, m.rotation.z,
                         m.length);
        }
    }

    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));

    for (int outfit : {coordType, 0}) {
        assemble::AssembleOptions ao; ao.log = nullptr; ao.outfit = outfit;
        assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
        std::fprintf(stderr, "\n=== outfit %d: bones=%zu meshes=%zu ===\n", outfit, scene.bones.size(), scene.meshes.size());
        for (const auto& mesh : scene.meshes) {
            if (mesh.name.find("skirt") == std::string::npos) continue;
            // dominant bone per vertex, grouped by that bone
            struct Group { size_t n = 0; double w = 0; float ylo = 1e9f, yhi = -1e9f; };
            std::map<int, Group> groups;
            float ylo = 1e9f, yhi = -1e9f;
            for (const auto& v : mesh.vertices) {
                ylo = std::min(ylo, v.pos.y); yhi = std::max(yhi, v.pos.y);
                int best = 0;
                for (int k = 1; k < 4; ++k) if (v.weight[k] > v.weight[best]) best = k;
                const int b = v.bone[best] >= 0 && static_cast<size_t>(v.bone[best]) < scene.bones.size() ? v.bone[best] : -1;
                auto& g = groups[b];
                ++g.n; g.w += v.weight[best];
                g.ylo = std::min(g.ylo, v.pos.y); g.yhi = std::max(g.yhi, v.pos.y);
            }
            std::fprintf(stderr, "mesh '%s' verts=%zu y=[%.2f,%.2f]\n", mesh.name.c_str(), mesh.vertices.size(), ylo, yhi);
            std::vector<std::pair<size_t, int>> order;
            for (const auto& kv : groups) order.push_back({kv.second.n, kv.first});
            std::sort(order.rbegin(), order.rend());
            for (size_t i = 0; i < order.size() && i < 14; ++i) {
                const int b = order[i].second;
                const auto& g = groups[b];
                const char* name = b >= 0 && static_cast<size_t>(b) < scene.bones.size() ? scene.bones[static_cast<size_t>(b)].name.c_str() : "(none)";
                float bx = 0, by = 0, bz = 0, sx = 0, sy = 0, sz = 0;
                if (b >= 0 && static_cast<size_t>(b) < scene.bones.size()) {
                    const auto& sb = scene.bones[static_cast<size_t>(b)];
                    bx = sb.pos.x; by = sb.pos.y; bz = sb.pos.z;
                    sx = colLen(sb.world, 0); sy = colLen(sb.world, 1); sz = colLen(sb.world, 2);
                }
                std::fprintf(stderr, "  bone[%3d] %-28s verts=%5zu meanW=%.2f vy=[%6.2f,%6.2f] world=(%.2f,%.2f,%.2f) wscale=(%.3f,%.3f,%.3f)\n",
                             b, name, g.n, g.w / g.n, g.ylo, g.yhi, bx, by, bz, sx, sy, sz);
            }
        }
    }
    return 0;
}
