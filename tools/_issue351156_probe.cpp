// Diagnostic only: dump the assembled/baked scene material state plus MmdMaterialLook decisions.
// usage: probe <card.png> <library> <mods>
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: probe <card> <library> <mods>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    for (const auto& kv : c.extended.asMap()) {
        const std::string key = kv.first.isStr() ? kv.first.asStr() : "";
        const auto& payload = kv.second.asArray();
        std::string txt;
        if (!payload.empty()) { const auto& b = payload.back().asBin(); txt.assign(b.begin(), b.end()); }
        if (txt.find("bot_skirt05") != std::string::npos || key.find("Material") != std::string::npos) {
            size_t at = txt.find("bot_skirt05");
            std::fprintf(stderr, "EXT key='%s' payload=%zu bytes skirtAt=%zu\n", key.c_str(), txt.size(), at);
            if (at != std::string::npos) std::fprintf(stderr, "   context: %s\n", txt.substr(std::max<size_t>(0, at - 120), 400).c_str());
        }
    }
    {
        const auto& cs = c.status.at("clothesState");
        std::fprintf(stderr, "clothesState: [");
        for (const auto& v : cs.asArray()) std::fprintf(stderr, "%d ", static_cast<int>(v.asInt()));
        std::fprintf(stderr, "]\n");
    }
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    for (const auto& mesh : scene.meshes) {
        if (mesh.name.find("skirt") == std::string::npos) continue;
        float lo = 1e9f, hi = -1e9f;
        for (const auto& v : mesh.vertices) { lo = std::min(lo, v.pos.y); hi = std::max(hi, v.pos.y); }
        std::fprintf(stderr, "MESH '%s' verts=%zu submeshes=%zu y=[%.2f,%.2f]\n", mesh.name.c_str(), mesh.vertices.size(), mesh.submeshes.size(), lo, hi);
    }
    // Pre-bake state: what the live Lit view draws.
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (m.name.find("skirt") == std::string::npos && m.name.find("high_neck") == std::string::npos && m.name.find("hair") == std::string::npos) continue;
        auto preLook = exporter::MmdMaterialLook(scene, m, false, 0, false);
        std::fprintf(stderr, "PRE mat[%zu] '%s' shader='%s' q=%d alpha=%d trans=%d clips=%d ignore=%d double=%d diff.a=%.3f tex=%d lookFlags=0x%02x look.a=%.3f inner=%d\n",
                     i, m.name.c_str(), m.shader.c_str(), assemble::EffectiveRenderQueue(scene, m), m.alpha ? 1 : 0,
                     m.translucent ? 1 : 0, m.shaderClipsAlpha ? 1 : 0, assemble::IgnoresTextureAlpha(scene, m) ? 1 : 0,
                     m.doubleSided ? 1 : 0, m.diffuse.w, m.texture, preLook.drawFlags, preLook.diffuse.w, exporter::HasInnerLayer(scene, m) ? 1 : 0);
        auto dumpTex = [&](int ti, const char* tag) {
            if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size()) return;
            const auto& t = scene.textures[static_cast<size_t>(ti)];
            std::fprintf(stderr, "     %s tex[%d] '%s' %dx%d\n", tag, ti, t.name.c_str(), t.width, t.height);
        };
        dumpTex(m.texture, "main");
        for (const auto& tb : m.texProps) {
            if (tb.name == "_MainTex" || tb.name == "_AlphaMask" || tb.name == "_Texture2" || tb.name == "_Texture3")
                dumpTex(tb.texture, tb.name.c_str());
            std::fprintf(stderr, "     PRE texprop %s = tex %d st=(%.3f %.3f %.3f %.3f)\n", tb.name.c_str(), tb.texture, tb.st.x, tb.st.y, tb.st.z, tb.st.w);
        }
        for (const auto& pr : m.props)
            if (pr.first.find("Cutoff") != std::string::npos || pr.first == "_Color")
                std::fprintf(stderr, "     PRE prop %s = %.3f %.3f %.3f %.3f\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
        // mesh-sampled source alpha cut fraction, ignoring the ignore-alpha predicate
        if (m.texture >= 0 && static_cast<size_t>(m.texture) < scene.textures.size()) {
            const auto& t = scene.textures[static_cast<size_t>(m.texture)];
            size_t sampled = 0, cut = 0;
            for (const auto& mesh : scene.meshes)
                for (const auto& sm : mesh.submeshes) {
                    if (sm.material != static_cast<int>(i)) continue;
                    for (size_t k = 0; k + 2 < sm.indices.size(); k += 3) {
                        const auto& a = mesh.vertices[sm.indices[k]].uv, & b = mesh.vertices[sm.indices[k + 1]].uv, & c = mesh.vertices[sm.indices[k + 2]].uv;
                        float u = (a.x + b.x + c.x) / 3.0f, v = (a.y + b.y + c.y) / 3.0f;
                        u -= std::floor(u); v -= std::floor(v);
                        const int x = std::min(t.width - 1, (int)(u * t.width)), y = std::min(t.height - 1, (int)((1.0f - v) * t.height));
                        ++sampled;
                        if (t.rgba[((size_t)y * t.width + x) * 4 + 3] < 128) ++cut;
                    }
                }
            std::fprintf(stderr, "     PRE source-alpha sampled=%zu cut=%zu (%.1f%%)\n", sampled, cut, sampled ? 100.0 * cut / sampled : 0.0);
        }
    }
    exporter::BakeShaderTextures(scene, 1024, true, nullptr, nullptr, true, true);
    std::fprintf(stderr, "materials=%zu textures=%zu\n", scene.materials.size(), scene.textures.size());
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (m.name.find("skirt") == std::string::npos && m.name.find("goddes") == std::string::npos &&
            m.name.find("inner") == std::string::npos && m.name.find("gloves") == std::string::npos) continue;
        const int q = assemble::EffectiveRenderQueue(scene, m);
        const bool ig = assemble::IgnoresTextureAlpha(scene, m);
        auto look = exporter::MmdMaterialLook(scene, m, false, 0, false);
        std::fprintf(stderr, "mat[%zu] '%s' shader='%s' q=%d alpha=%d trans=%d clips=%d ignore=%d diff.a=%.3f look.a=%.3f tex=%d\n",
                     i, m.name.c_str(), m.shader.c_str(), q, m.alpha ? 1 : 0, m.translucent ? 1 : 0,
                     m.shaderClipsAlpha ? 1 : 0, ig ? 1 : 0, m.diffuse.w, look.diffuse.w, m.texture);
        if (m.texture >= 0 && static_cast<size_t>(m.texture) < scene.textures.size()) {
            const auto& t = scene.textures[static_cast<size_t>(m.texture)];
            size_t a0 = 0, a255 = 0, amid = 0;
            for (size_t p = 3; p < t.rgba.size(); p += 4) {
                if (t.rgba[p] == 0) ++a0; else if (t.rgba[p] == 255) ++a255; else ++amid;
            }
            std::fprintf(stderr, "     tex '%s' %dx%d A0=%zu A255=%zu Amid=%zu\n", t.name.c_str(), t.width, t.height, a0, a255, amid);
        }
        for (const auto& pr : m.props) {
            if (pr.first == "_Cutoff" || pr.first == "_Color" || pr.first.find("MainTex") != std::string::npos || pr.first.find("ST") != std::string::npos)
                std::fprintf(stderr, "     prop %s = %.3f %.3f %.3f %.3f\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
        }
        for (const auto& tb : m.texProps)
            std::fprintf(stderr, "     texprop %s = tex %d st=(%.3f %.3f %.3f %.3f)\n", tb.name.c_str(), tb.texture, tb.st.x, tb.st.y, tb.st.z, tb.st.w);
    }
    return 0;
}
