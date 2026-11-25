// Prototype: render with degenerate uv0 replaced by uv1 on garment meshes.
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "export/Exporter.h"
#include "export/GpuView.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: gloveuv <card> <lib> <mods> <out.png> <plain:0|1>\n"); return 1; }
    card::Card c = card::LoadCard(argv[1]);
    std::vector<std::string> issues;
    resolve::ListTable lists; library::CheckLibrary(argv[2], false); lists.loadBaseGame(library::AbdataRoot(argv[2]), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(argv[3], std::string(argv[2]) + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao; ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    for (auto& mesh : scene.meshes) {
        float a = 1e9f, b = -1e9f, c1 = 1e9f, d = -1e9f;
        for (const auto& v : mesh.vertices) { a = std::min(a, v.uv.x); b = std::max(b, v.uv.x); c1 = std::min(c1, v.uv1.x); d = std::max(d, v.uv1.x); }
        if (b - a < 0.1f && d - c1 > 0.5f) {
            std::fprintf(stderr, "uvfix '%s' %.4f -> %.4f\n", mesh.name.c_str(), b - a, d - c1);
            const bool centered = std::getenv("CENTERED") != nullptr;
            for (auto& v : mesh.vertices) {
                v.uv = v.uv1;
                if (centered) v.uv.x = 0.25f + 0.5f * v.uv.x;   // map into the create-job's opaque strip
            }
        }
    }
    if (std::getenv("FILLPAD")) {
        auto& t = scene.textures[static_cast<size_t>(scene.materials[23].texture)];
        size_t filled = 0;
        for (int y = 0; y < t.height; ++y) for (int x = 0; x < t.width; ++x) {
            size_t p = (static_cast<size_t>(y) * t.width + x) * 4;
            if (t.rgba[p + 3] >= 128) continue;
            int sx = std::clamp(x, t.width / 4, t.width * 3 / 4 - 1);
            size_t q = (static_cast<size_t>(y) * t.width + sx) * 4;
            for (int c = 0; c < 4; ++c) t.rgba[p + c] = t.rgba[q + c];
            t.rgba[p + 3] = 255;
            ++filled;
        }
        std::fprintf(stderr, "fillpad: %zu texels\n", filled);
        scene.materials[23].shaderClipsAlpha = false;
        scene.materials[23].alpha = false;
    }
    if (std::getenv("SINGLE")) for (auto& m : scene.materials) if (m.name.find("glove") != std::string::npos) m.doubleSided = false;
    if (std::getenv("KEEPCUT")) for (auto& m : scene.materials) m.fullyCutByAlpha = false;
    if (std::getenv("TRIMFACES")) {
        const int ti = scene.materials[23].texture;
        const auto& t = scene.textures[static_cast<size_t>(ti)];
        static const float bary[4][3] = {{1.f/3,1.f/3,1.f/3},{0.6f,0.2f,0.2f},{0.2f,0.6f,0.2f},{0.2f,0.2f,0.6f}};
        auto& sm = scene.meshes[23].submeshes[0];
        std::vector<uint32_t> keep;
        size_t dropped = 0;
        for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
            size_t opaque = 0;
            for (const auto& w : bary) {
                const auto& a = scene.meshes[23].vertices[sm.indices[i]].uv;
                const auto& b = scene.meshes[23].vertices[sm.indices[i+1]].uv;
                const auto& c = scene.meshes[23].vertices[sm.indices[i+2]].uv;
                float u = a.x*w[0]+b.x*w[1]+c.x*w[2], v = a.y*w[0]+b.y*w[1]+c.y*w[2];
                u -= std::floor(u); v -= std::floor(v);
                int x = std::min(t.width-1, (int)(u*t.width)), y = std::min(t.height-1, (int)((1-v)*t.height));
                if (t.rgba[((size_t)y*t.width+x)*4+3] >= 128) ++opaque;
            }
            if (opaque >= 1) { keep.push_back(sm.indices[i]); keep.push_back(sm.indices[i+1]); keep.push_back(sm.indices[i+2]); }
            else ++dropped;
        }
        std::fprintf(stderr, "trim: kept %zu dropped %zu tris\n", keep.size()/3, dropped);
        sm.indices = std::move(keep);
    }
    if (std::getenv("FALLBACKGLOVE")) {
        for (auto& m : scene.materials) if (m.name.find("glove") != std::string::npos) {
            m.shaderIndex = -1; m.alpha = true; m.shaderClipsAlpha = true; m.fullyCutByAlpha = false;
        }
    }
    if (std::getenv("BAKE")) exporter::BakeShaderTextures(scene, 1024, true, nullptr, nullptr, true, true);
    exporter::GpuOptions go; go.preference = "warp"; go.skin = "cpu";
    exporter::GpuView gpu; std::string why;
    if (!gpu.Init(go, &why) || !gpu.ready()) { std::fprintf(stderr, "gpu: %s\n", why.c_str()); return 3; }
    if (!gpu.Load(scene, &why)) { std::fprintf(stderr, "load: %s\n", why.c_str()); return 4; }
    gpu.SetPlain(std::atoi(argv[5]) != 0);
    std::vector<uint32_t> px;
    gpu.Render(exporter::ViewCamera{}, 640, 800, nullptr, px);
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) { rgba[i*4]=(px[i]>>16)&255; rgba[i*4+1]=(px[i]>>8)&255; rgba[i*4+2]=px[i]&255; rgba[i*4+3]=255; }
    stbi_write_png(argv[4], 640, 800, 4, rgba.data(), 640 * 4);
    return 0;
}
