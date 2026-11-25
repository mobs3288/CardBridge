// Diagnostic: dump the raw glove mesh UVs, material ST and main texture from a Unity bundle.
#include "core/FileIo.h"
#include "unity/UnityBundle.h"
#include "unity/UnityScene.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <stb_image_write.h>
using namespace cardbridge;
using namespace cardbridge::unity;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: glovedump <bundle> <out-prefix>\n"); return 1; }
    Bytes bytes = fileio::ReadFile(argv[1]);
    auto bundle = UnityBundle::Open(std::move(bytes), argv[1]);
    UnityFile uf(bundle);
    for (const char* name : {"cf_gloves_maid_01_t", "cf_gloves_maid_01_mc"}) {
        PPtr p = uf.findTexture(name);
        std::fprintf(stderr, "texture '%s' -> file=%d path=%lld\n", name, p.fileId, (long long)p.pathId);
        if (p.isNull()) continue;
        std::string why;
        auto t = uf.readTexture(p, &why);
        if (!t) { std::fprintf(stderr, "  read failed: %s\n", why.c_str()); continue; }
        std::fprintf(stderr, "  %dx%d wrap=%d srgb=%d\n", t->width, t->height, t->wrapMode, t->colorSpace);
        std::string out = std::string(argv[2]) + "_" + name + ".png";
        stbi_write_png(out.c_str(), t->width, t->height, 4, t->rgba.data(), t->width * 4);
    }
    for (int64_t xf : uf.findXformsByName("o_gloves_maid_01")) {
        std::fprintf(stderr, "xform %lld path=%s\n", (long long)xf, uf.pathOf(xf).c_str());
        for (const auto& r : uf.renderersUnder(xf)) {
            std::fprintf(stderr, "  renderer '%s' mesh=%lld mats=%zu\n", r.name.c_str(), (long long)r.mesh.pathId, r.materials.size());
            std::string why;
            auto mesh = uf.readMesh(r.mesh, &why);
            if (!mesh) { std::fprintf(stderr, "    mesh failed: %s\n", why.c_str()); continue; }
            float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
            for (const auto& uv : mesh->uv0) { umin = std::min(umin, uv.x); umax = std::max(umax, uv.x); vmin = std::min(vmin, uv.y); vmax = std::max(vmax, uv.y); }
            std::fprintf(stderr, "    mesh uv0 %zu verts uv=[%.5f..%.5f]x[%.5f..%.5f] uv1=%zu uv2=%zu uv3=%zu\n",
                         mesh->uv0.size(), umin, umax, vmin, vmax, mesh->uv1.size(), mesh->uv2.size(), mesh->uv3.size());
            auto bounds = [](const std::vector<Vec2>& uv, const char* tag) {
                if (uv.empty()) return;
                float a = 1e9f, b = -1e9f, c = 1e9f, d = -1e9f;
                for (const auto& p : uv) { a = std::min(a, p.x); b = std::max(b, p.x); c = std::min(c, p.y); d = std::max(d, p.y); }
                std::fprintf(stderr, "      %s=[%.5f..%.5f]x[%.5f..%.5f] first=(%.5f %.5f)\n", tag, a, b, c, d, uv[0].x, uv[0].y);
            };
            bounds(mesh->uv1, "uv1"); bounds(mesh->uv2, "uv2"); bounds(mesh->uv3, "uv3");
            for (size_t i = 0; i < std::min<size_t>(5, mesh->uv0.size()); ++i)
                std::fprintf(stderr, "      uv0[%zu]=(%.5f %.5f)\n", i, mesh->uv0[i].x, mesh->uv0[i].y);
            for (const auto& mp : r.materials) {
                auto m = uf.readMaterial(mp, &why);
                if (!m) { std::fprintf(stderr, "    material failed: %s\n", why.c_str()); continue; }
                std::fprintf(stderr, "    material '%s' shader='%s'\n", m->name.c_str(), m->shaderName.c_str());
                for (const auto& tb : m->textures) std::fprintf(stderr, "      tex %s -> %lld\n", tb.first.c_str(), (long long)tb.second.pathId);
                for (const auto& st : m->texST) std::fprintf(stderr, "      ST %s = (%.5f %.5f %.5f %.5f)\n", st.first.c_str(), st.second.x, st.second.y, st.second.z, st.second.w);
            }
        }
    }
    return 0;
}
