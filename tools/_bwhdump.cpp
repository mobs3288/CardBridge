// Scratch: assemble a card and dump the torso perimeter at every height + the BWH measurement.
//   usage: bwhdump <card.png> <library> [mods]
#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "measure/BodyMeasure.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
std::string Slashes(const char* s) {
    std::string out = s;
    for (auto& c : out) if (c == '\\') c = '/';
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bwhdump <card.png> <library> [mods]\n"); return 1; }
    const std::string cardPath = Slashes(argv[1]);
    const std::string library = Slashes(argv[2]);
    const std::string mods = argc >= 4 ? Slashes(argv[3]) : std::string();
    using namespace cardbridge;
    card::Card c;
    try {
        c = card::LoadCard(cardPath);
    } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    resolve::ModIndex mindex;
    if (!mods.empty()) {
        const std::string cache = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/bwhdump_modcache.json";
        std::vector<std::string> issues;
        mindex = resolve::ModIndex::Build(mods, cache, &issues, nullptr, [](size_t, size_t) {});
    }
    const auto chk = library::CheckLibrary(library, false);
    if (!chk.ok) { std::fprintf(stderr, "library problems:\n"); for (const auto& i : chk.issues) std::fprintf(stderr, "  %s\n", i.c_str()); return 3; }
    const std::string abdata = library::AbdataRoot(library);
    resolve::ListTable lists;
    std::vector<std::string> issues;
    lists.loadBaseGame(abdata, &issues);
    auto resolver = std::make_unique<resolve::Resolver>(mindex, lists, abdata);
    assemble::AssembleOptions ao;
    ao.outfit = 0;
    std::fprintf(stderr, "assembling %s\n", cardPath.c_str());
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    for (const auto& p : scene.parts)
        for (int mi : p.meshes) {
            const auto& m = scene.meshes[static_cast<size_t>(mi)];
            float mlo = 1e9f, mhi = -1e9f;
            for (const auto& v : m.vertices) { mlo = std::min(mlo, v.pos.y); mhi = std::max(mhi, v.pos.y); }
            size_t nidx = 0;
            for (const auto& sub : m.submeshes) nidx += sub.indices.size();
            std::fprintf(stderr, "part %-24s mesh %d '%s'  verts %6zu  tris %5zu  y %.3f..%.3f\n", p.id.c_str(), mi, m.name.c_str(), m.vertices.size(), nidx / 3, mlo, mhi);
        }
    {   // where the skeleton actually is
        std::fprintf(stderr, "bone y heights (m):\n");
        for (const char* want : {"cf_j_head", "cf_s_head", "cf_j_shoulder_L", "cf_s_spine03", "cf_s_spine02", "cf_s_spine01", "cf_s_spine00", "cf_j_chest", "cf_j_root", "cf_n_height", "cf_j_ankle_L", "cf_j_ankle_R", "cf_j_knee_L", "cf_j_thigh01_L", "cf_s_waist01", "cf_s_waist02", "cf_j_waist01", "cf_j_waist02", "cf_j_dan", "cf_s_dan", "cf_s_siri", "cf_j_siri"}) {
            const int i = scene.boneIndex(want);
            if (i < 0) continue;
            std::fprintf(stderr, "  %-16s pos y %.4f\n", want, scene.bones[static_cast<size_t>(i)].pos.y);
        }
    }
    {   // triangle count per coarse band
        size_t band[40] = {};
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const auto& vs = m.vertices;
                    for (const auto& sub : m.submeshes)
                        for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                            const int b = static_cast<int>(vs[sub.indices[f]].pos.y * 20.0f);
                            if (b >= 0 && b < 40) ++band[b];
                        }
                }
        for (int i = 0; i < 40; ++i)
            if (band[i]) std::fprintf(stderr, "  tris %4.2f..%4.2f  %6zu\n", i * 0.05f, (i + 1) * 0.05f, band[i]);
    }
    {   // index sanity: triangles referencing vertices beyond the shared vertex buffer
        size_t oob = 0, total = 0;
        uint32_t maxIdx = 0;
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const size_t nv = m.vertices.size();
                    for (const auto& sub : m.submeshes)
                        for (uint32_t idx : sub.indices) {
                            ++total;
                            maxIdx = std::max(maxIdx, idx);
                            if (idx >= nv) ++oob;
                        }
                }
        std::fprintf(stderr, "body indices total %zu  out-of-buffer %zu  maxIndex %u  (buffer fits 8302?)\n", total, oob, maxIdx);
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    for (const auto& sub : m.submeshes) {
                        size_t subOob = 0;
                        for (uint32_t idx : sub.indices) if (idx >= m.vertices.size()) ++subOob;
                        if (subOob) std::fprintf(stderr, "  submesh material %d  indices %zu  oob %zu\n", sub.material, sub.indices.size(), subOob);
                    }
                }
    }
    {   // how many triangles live above y = 1.15 at all, and the y-extent of the mesh's different islands
        size_t trisAbove = 0, trisAny = 0, vertsAbove = 0;
        float ymin = 1e9f, ymax = -1e9f;
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const auto& vs = m.vertices;
                    for (const auto& v : vs) { ymin = std::min(ymin, v.pos.y); ymax = std::max(ymax, v.pos.y); if (v.pos.y > 1.15f) ++vertsAbove; }
                    for (const auto& sub : m.submeshes)
                        for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                            const float a = vs[sub.indices[f]].pos.y, b = vs[sub.indices[f + 1]].pos.y, c = vs[sub.indices[f + 2]].pos.y;
                            ++trisAny;
                            if (std::max({a, b, c}) > 1.15f) ++trisAbove;
                        }
                }
        std::fprintf(stderr, "verts with y>1.15: %zu   triangles with any vertex above 1.15: %zu / %zu   mesh y %.4f..%.4f\n", vertsAbove, trisAbove, trisAny, ymin, ymax);
    }
    {   // how many vertices each submesh carries, and their y range
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    float mlo = 1e9f, mhi = -1e9f;
                    for (const auto& v : m.vertices) { mlo = std::min(mlo, v.pos.y); mhi = std::max(mhi, v.pos.y); }
                    std::fprintf(stderr, "mesh %d '%s'  verts %zu  y %.4f..%.4f\n", mi, m.name.c_str(), m.vertices.size(), mlo, mhi);
                }
    }
    {   // every triangle whose lowest vertex is in [1.10, 1.31]: print its overall y-range, plus a summary
        size_t n = 0, below116 = 0, inNeck = 0, belowHalf = 0;
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const auto& vs = m.vertices;
                    for (const auto& sub : m.submeshes)
                        for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                            const float a = vs[sub.indices[f]].pos.y, b = vs[sub.indices[f + 1]].pos.y, c = vs[sub.indices[f + 2]].pos.y;
                            const float mn = std::min({a, b, c}), mx = std::max({a, b, c});
                            if (mn > 1.10f && mn < 1.31f) {
                                ++n;
                                if (mx <= 1.16f) ++below116;
                                if (mn >= 1.28f) ++n; // no-op: keep symmetry
                                if (n < 40) std::fprintf(stderr, "tri#%zu mn %.4f  mx %.4f\n", f / 3, mn, mx);
                            }
                        }
                }
        std::fprintf(stderr, "triangles with lowest vertex in (1.10, 1.31): %zu, of which fully below 1.16: %zu\n", n, below116);
    }
    for (float y : {1.10f, 1.12f, 1.15f, 1.18f, 1.21f, 1.24f, 1.28f, 1.30f, 1.35f, 1.40f, 1.45f}) {
        size_t straddle = 0, below = 0, above = 0, inBand = 0, spanAbove = 0;
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const auto& vs = m.vertices;
                    for (const auto& sub : m.submeshes)
                        for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                            const float a = vs[sub.indices[f]].pos.y - y, b = vs[sub.indices[f + 1]].pos.y - y, c = vs[sub.indices[f + 2]].pos.y - y;
                            if (a < 0.0f && b < 0.0f && c < 0.0f) ++below;
                            else if (a >= 0.0f && b >= 0.0f && c >= 0.0f) ++above;
                            else ++straddle;
                            const float mn = std::min({vs[sub.indices[f]].pos.y, vs[sub.indices[f + 1]].pos.y, vs[sub.indices[f + 2]].pos.y});
                            const float mx = std::max({vs[sub.indices[f]].pos.y, vs[sub.indices[f + 1]].pos.y, vs[sub.indices[f + 2]].pos.y});
                            if (mn > 1.15f && mx <= 1.28f) ++inBand;
                            if (mx > 1.28f) ++spanAbove;
                        }
                }
        std::fprintf(stderr, "y %.2f  straddle %zu  below %zu  above %zu   band(1.15-1.28) %zu   spanAbove1.28 %zu\n", y, straddle, below, above, inBand, spanAbove);
    }
    {   // vertex count in coarse bands across the whole figure
        int band[40] = {};
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes)
                    for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                        const int b = static_cast<int>(v.pos.y * 20.0f);
                        if (b >= 0 && b < 40) ++band[b];
                    }
        for (int i = 0; i < 40; ++i)
            if (band[i]) std::fprintf(stderr, "  %5.3f..%5.3f  %6d verts\n", i * 0.05f, (i + 1) * 0.05f, band[i]);
    }
    {   // where the triangles in the neck gap truly lie: histogram of each triangle's lowest vertex
        int band[20] = {};
        for (const auto& p : scene.parts)
            if (p.id == "body")
                for (int mi : p.meshes) {
                    const auto& m = scene.meshes[static_cast<size_t>(mi)];
                    const auto& vs = m.vertices;
                    for (const auto& sub : m.submeshes)
                        for (size_t f = 0; f + 2 < sub.indices.size(); f += 3) {
                            const float mn = std::min({vs[sub.indices[f]].pos.y, vs[sub.indices[f + 1]].pos.y, vs[sub.indices[f + 2]].pos.y});
                            const int b = static_cast<int>(mn * 10.0f);   // 10 cm bins
                            if (b >= 0 && b < 20) ++band[b];
                        }
                }
        for (int i = 0; i < 20; ++i)
            if (band[i]) std::fprintf(stderr, "  tris with lowest y in [%.1f, %.1f)  %6d\n", i * 0.1f, (i + 1) * 0.1f, band[i]);
    }
    for (float y : {0.85f, 0.90f, 0.95f, 1.00f, 1.05f, 1.10f, 1.13f, 1.16f, 1.18f, 1.21f, 1.25f, 1.30f}) {
        std::vector<float> chains;
        measure::SliceChainsForDebug(scene, y, chains);
        std::sort(chains.begin(), chains.end(), [](float a, float b) { return a > b; });
        std::fprintf(stderr, "y %.2f  chains(%zu):", y, chains.size());
        for (size_t i = 0; i < std::min<size_t>(chains.size(), 8); ++i) std::fprintf(stderr, " %.1f", chains[i] * 100.0f);
        std::fprintf(stderr, "\n");
    }
    for (float y : {0.85f, 0.90f, 0.95f, 1.00f, 1.05f, 1.10f, 1.13f, 1.16f, 1.18f, 1.21f, 1.25f, 1.30f}) {
        float xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
        for (const auto& p : scene.parts)
            if (p.id == "body" && !p.underPiece)
                for (int mi : p.meshes)
                    for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices)
                        if (std::fabs(v.pos.y - y) < 0.015f) {
                            xmin = std::min(xmin, v.pos.x); xmax = std::max(xmax, v.pos.x);
                            zmin = std::min(zmin, v.pos.z); zmax = std::max(zmax, v.pos.z);
                        }
        std::fprintf(stderr, "y %.2f  x %+.3f..%+.3f (width %.2f cm)   z %+.3f..%+.3f (depth %.2f cm)\n", y, xmin, xmax, (xmax - xmin) * 100.0f, zmin, zmax, (zmax - zmin) * 100.0f);
    }
    const auto pr = measure::Measure(scene);
    std::fprintf(stderr, "heightMeters %.4f valid %d\n", pr.heightMeters, pr.valid ? 1 : 0);
    std::fprintf(stderr, "bust  %.1f cm at y %.4f\n", pr.bust * 100.0f, pr.bustY);
    std::fprintf(stderr, "waist %.1f cm at y %.4f\n", pr.waist * 100.0f, pr.waistY);
    std::fprintf(stderr, "hip   %.1f cm at y %.4f\n", pr.hip * 100.0f, pr.hipY);
    std::fprintf(stderr, "slices %d\n", pr.slices);
    for (float y = 0.45f; y < 1.25f; y += 0.004f) {
        const float p = measure::TorsoPerimeterAt(scene, y);
        std::fprintf(stderr, "slice %.3f  %7.1f\n", y, p * 100.0f);
    }
    return 0;
}
