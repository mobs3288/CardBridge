// Diagnostic only: scene materials/textures/UVs, sampled source alpha and RGB.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

static void sampleTex(const assemble::Scene& scene, int ti, const char* tag) {
    if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size()) return;
    const auto& t = scene.textures[static_cast<size_t>(ti)];
    size_t a0 = 0, a255 = 0, amid = 0;
    double rgb[3] = {0, 0, 0};
    for (size_t p = 0; p + 3 < t.rgba.size(); p += 4) {
        const uint8_t a = t.rgba[p + 3];
        if (a == 0) ++a0; else if (a == 255) ++a255; else ++amid;
        rgb[0] += t.rgba[p]; rgb[1] += t.rgba[p + 1]; rgb[2] += t.rgba[p + 2];
    }
    const double n = static_cast<double>(t.width) * t.height;
    std::fprintf(stderr, "  %s tex[%d] '%s' %dx%d A0=%zu A255=%zu Amid=%zu meanRGB=%.0f,%.0f,%.0f\n",
                 tag, ti, t.name.c_str(), t.width, t.height, a0, a255, amid, rgb[0] / n, rgb[1] / n, rgb[2] / n);
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: uvprobe <card> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    resolve::ListTable lists;
    library::CheckLibrary(argv[2], false);
    lists.loadBaseGame(library::AbdataRoot(argv[2]), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(argv[3], std::string(argv[2]) + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    std::fprintf(stderr, "materials=%zu textures=%zu meshes=%zu parts=%zu\n", scene.materials.size(), scene.textures.size(), scene.meshes.size(), scene.parts.size());
    {
        std::fprintf(stderr, "--- UV channels ---\n");
        for (size_t me = 0; me < scene.meshes.size(); ++me) {
            const auto& m = scene.meshes[me];
            float mn0 = 1e9f, mx0 = -1e9f, mn1 = 1e9f, mx1 = -1e9f;
            for (const auto& x : m.vertices) { mn0 = std::min(mn0, x.uv.x); mx0 = std::max(mx0, x.uv.x); mn1 = std::min(mn1, x.uv1.x); mx1 = std::max(mx1, x.uv1.x); }
            std::fprintf(stderr, "mesh[%zu] '%s' uv0spanX=%.4f uv1spanX=%.4f\n", me, m.name.c_str(), mx0 - mn0, mx1 - mn1);
        }
    }

    // Map material -> meshes/submeshes and UV bounds.
    for (size_t mi = 0; mi < scene.materials.size(); ++mi) {
        const auto& m = scene.materials[mi];
        if (m.name.find("glove") == std::string::npos && m.name.find("body") == std::string::npos &&
            m.name.find("skirt") == std::string::npos && m.name.find("high_neck") == std::string::npos) continue;
        std::fprintf(stderr, "mat[%zu] '%s' shader='%s' alpha=%d trans=%d clips=%d ignore=%d q=%d tex=%d\n",
                     mi, m.name.c_str(), m.shader.c_str(), m.alpha ? 1 : 0, m.translucent ? 1 : 0,
                     m.shaderClipsAlpha ? 1 : 0, assemble::IgnoresTextureAlpha(scene, m) ? 1 : 0,
                     assemble::EffectiveRenderQueue(scene, m), m.texture);
        sampleTex(scene, m.texture, "main");
        for (const auto& tb : m.texProps) if (tb.name == "_MainTex" && tb.texture != m.texture) sampleTex(scene, tb.texture, "_MainTex");
        if (m.texture < 0 || static_cast<size_t>(m.texture) >= scene.textures.size()) continue;
        const auto& t = scene.textures[static_cast<size_t>(m.texture)];
        for (size_t me = 0; me < scene.meshes.size(); ++me) {
            const auto& mesh = scene.meshes[me];
            for (const auto& sm : mesh.submeshes) {
                if (sm.material != static_cast<int>(mi)) continue;
                float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
                for (uint32_t idx : sm.indices) {
                    const auto& uv = mesh.vertices[idx].uv;
                    umin = std::min(umin, uv.x); umax = std::max(umax, uv.x);
                    vmin = std::min(vmin, uv.y); vmax = std::max(vmax, uv.y);
                }
                size_t sampled = 0, cut = 0;
                double sr = 0, sg = 0, sb = 0;
                for (size_t k = 0; k + 2 < sm.indices.size(); k += 3) {
                    const auto& a = mesh.vertices[sm.indices[k]].uv;
                    const auto& b = mesh.vertices[sm.indices[k + 1]].uv;
                    const auto& c2 = mesh.vertices[sm.indices[k + 2]].uv;
                    float u = (a.x + b.x + c2.x) / 3.0f, v = (a.y + b.y + c2.y) / 3.0f;
                    u -= std::floor(u); v -= std::floor(v);
                    const int x = std::min(t.width - 1, static_cast<int>(u * t.width));
                    const int y = std::min(t.height - 1, static_cast<int>((1.0f - v) * t.height));
                    const size_t p = (static_cast<size_t>(y) * t.width + x) * 4;
                    ++sampled;
                    if (t.rgba[p + 3] < 128) ++cut;
                    sr += t.rgba[p]; sg += t.rgba[p + 1]; sb += t.rgba[p + 2];
                }
                std::fprintf(stderr, "   mesh[%zu] '%s' tris=%zu uv=[%.3f..%.3f]x[%.3f..%.3f] sampled=%zu cut=%zu RGB=%.0f,%.0f,%.0f\n",
                             me, mesh.name.c_str(), sm.indices.size() / 3, umin, umax, vmin, vmax, sampled, cut,
                             sampled ? sr / sampled : 0, sampled ? sg / sampled : 0, sampled ? sb / sampled : 0);
            }
        }
    }
    std::fprintf(stderr, "--- parts ---\n");
    for (const auto& p : scene.parts) {
        std::fprintf(stderr, "part '%s' group='%s' meshes=%zu preview=%d export=%d hidden=%d under=%d\n",
                     p.id.c_str(), p.group.c_str(), p.meshes.size(), p.visibleInPreview ? 1 : 0,
                     p.includeInExport ? 1 : 0, p.startHidden ? 1 : 0, p.underPiece ? 1 : 0);
        for (int me : p.meshes) {
            const auto& mesh = scene.meshes[static_cast<size_t>(me)];
            std::fprintf(stderr, "    mesh '%s' verts=%zu subs=%zu\n", mesh.name.c_str(), mesh.vertices.size(), mesh.submeshes.size());
        }
    }
    return 0;
}
