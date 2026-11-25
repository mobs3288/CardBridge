// Diagnostic: apply the viewer's default live pose (A-pose + sag + height scale) to the skirt and report
// its world-space Y range before and after, then render that posed scene in the live view.
#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "assemble/Scene.h"
#include "card/CardFile.h"
#include <cstdlib>
#include "export/GpuView.h"
#include "export/Preview.h"
#include "export/ViewMode.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <stb_image_write.h>
using namespace cardbridge;

static void bounds(const assemble::Scene& s, const char* tag) {
    for (const auto& m : s.meshes) {
        if (m.name.find("skirt") == std::string::npos) continue;
        float lo = 1e9f, hi = -1e9f;
        for (const auto& v : m.vertices) { lo = std::min(lo, v.pos.y); hi = std::max(hi, v.pos.y); }
        std::fprintf(stderr, "%s mesh '%s' y=[%.3f, %.3f]\n", tag, m.name.c_str(), lo, hi);
    }
}
static void render(const assemble::Scene& scene, const char* path, const std::vector<char>* hiddenParts = nullptr) {
    exporter::GpuOptions go; go.preference = "warp"; go.skin = "cpu";
    exporter::GpuView gpu; std::string why;
    if (!gpu.Init(go, &why) || !gpu.ready()) { std::fprintf(stderr, "gpu: %s\n", why.c_str()); return; }
    if (!gpu.Load(scene, &why)) { std::fprintf(stderr, "load: %s\n", why.c_str()); return; }
    gpu.SetPlain(std::getenv("PLAIN") != nullptr);
    if (std::getenv("MATID")) gpu.SetDiagnosticMode(static_cast<int>(exporter::ViewMode::MaterialID), nullptr);
    exporter::ViewCamera cam;
    const int W = 640, H = 800;
    std::vector<uint32_t> px;
    if (!gpu.Render(cam, W, H, hiddenParts, px)) { std::fprintf(stderr, "render failed\n"); return; }
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) { rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255; }
    stbi_write_png(path, W, H, 4, rgba.data(), W * 4);
    std::fprintf(stderr, "wrote %s\n", path);
}
int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: poseskirt <card> <library> <mods> <out_prefix>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    if (std::getenv("CLEAR_ABMX")) {
        std::vector<std::string> notes;
        const bool cleared = card::ClearOutfitCoordinateSlot(c, 0, static_cast<int>(c.outfits.size()), &notes);
        std::fprintf(stderr, "cleared outfit ABMX=%d notes=%zu\n", cleared ? 1 : 0, notes.size());
    }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists; library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t, size_t) {});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao; ao.log = nullptr;
    if (std::getenv("NIP_INSIDE")) ao.nippleInside = true;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    bounds(scene, "rest");
    assemble::Pose p;
    assemble::MakeAPose(scene, 40.0f, p.base);
    assemble::AddSagPose(scene, p, 1.0f);
    const float height = assemble::SceneHeightMeters(scene);
    std::fprintf(stderr, "measured height %.3f m\n", height);
    if (height > 1e-4f) {
        const float f = 1.6f / height;
        if (std::fabs(f - 1.0f) > 1e-4f)
            for (size_t i = 0; i < scene.bones.size(); ++i) {
                if (scene.bones[i].parent != -1) continue;
                auto& e = p.edit(i);
                e.scl.x *= f; e.scl.y *= f; e.scl.z *= f;
            }
    }
    const auto skin = assemble::SkinMatrices(scene, p);
    assemble::BakeSkin(scene, skin);
    bounds(scene, "posed");
    std::vector<int> preTex;
    for (const auto& m : scene.materials) preTex.push_back(m.texture);
    if (std::getenv("BAKE")) exporter::BakeShaderTextures(scene, 1024, false, nullptr, nullptr, true, true);
    if (std::getenv("BODY_MASK") && scene.materials.size() > 0) {
        const int src = preTex[0], dst = scene.materials[0].texture;
        if (src >= 0 && dst >= 0 && static_cast<size_t>(src) < scene.textures.size() && static_cast<size_t>(dst) < scene.textures.size()) {
            const auto& a = scene.textures[static_cast<size_t>(src)];
            auto& b = scene.textures[static_cast<size_t>(dst)];
            if (a.rgba.size() == b.rgba.size())
                for (size_t p = 3; p < b.rgba.size(); p += 4) b.rgba[p] = a.rgba[p];
            std::fprintf(stderr, "restored body coverage alpha from '%s' to '%s'\n", a.name.c_str(), b.name.c_str());
        }
    }
    if (std::getenv("OPAQUE_TOP")) {
        for (size_t mi = 0; mi < scene.materials.size(); ++mi) {
            if (scene.materials[mi].name.find("high_neck") == std::string::npos) continue;
            const int ti = scene.materials[mi].texture;
            if (ti >= 0 && static_cast<size_t>(ti) < scene.textures.size())
                for (size_t p = 3; p < scene.textures[static_cast<size_t>(ti)].rgba.size(); p += 4) scene.textures[static_cast<size_t>(ti)].rgba[p] = 255;
            std::fprintf(stderr, "forced opaque top material %zu\n", mi);
        }
    }
    std::string out = std::string(argv[4]) + (std::getenv("BAKE") ? "_baked.png" : ".png");
    if (std::getenv("HIDE_BODY") || std::getenv("HIDE_TOP")) {
        std::vector<char> hidden(scene.parts.size(), 0);
        for (size_t i = 0; i < scene.parts.size(); ++i) {
            const std::string& id = scene.parts[i].id;
            if ((std::getenv("HIDE_BODY") && (id == "body" || id == "Body")) ||
                (std::getenv("HIDE_TOP") && id.find("top") != std::string::npos)) {
                hidden[i] = 1; std::fprintf(stderr, "hiding part %zu '%s'\n", i, id.c_str());
            }
        }
        render(scene, out.c_str(), &hidden);
    } else render(scene, out.c_str());
    return 0;
}
