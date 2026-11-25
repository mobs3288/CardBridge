// Diagnostic: enumerate every renderer under a body prefab and its mesh, so the
// extraction filter can be audited against the real source asset.
//   usage: anatomyprobe <oo_base.unity3d> [prefab-root ...]
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "unity/UnitySerialized.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: anatomyprobe <bundle> [prefab-root ...]\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);

    std::vector<std::string> roots;
    if (argc > 2) {
        for (int i = 2; i < argc; ++i) roots.push_back(argv[i]);
    } else {
        for (const auto& [id, x] : uf.xforms())
            if (x.parent == 0 && x.name.find("body") != std::string::npos) roots.push_back(x.name);
    }

    for (const auto& rootName : roots) {
        int64_t root = uf.findPrefabRoot(rootName);
        std::fprintf(stderr, "\n=== PREFAB ROOT '%s' -> xform %lld ===\n", rootName.c_str(), (long long)root);
        if (!root) continue;
        for (const auto& r : uf.renderersUnder(root)) {
            std::string why;
            auto mesh = r.mesh.isNull() ? std::nullopt : uf.readMesh(r.mesh, &why);
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            if (mesh) for (const auto& p : mesh->positions) {
                if (p.x < x0) x0 = p.x; if (p.x > x1) x1 = p.x;
                if (p.y < y0) y0 = p.y; if (p.y > y1) y1 = p.y;
                if (p.z < z0) z0 = p.z; if (p.z > z1) z1 = p.z;
            }
            std::fprintf(stderr, "  renderer='%s' path='%s' enabled=%d active=%d skinned=%d bones=%zu mesh='%s'",
                r.name.c_str(), uf.pathOf(r.xformId).c_str(), (int)r.enabled, (int)r.active,
                (int)r.skinned, r.boneXforms.size(), mesh ? mesh->name.c_str() : "(null)");
            if (mesh) {
                size_t tris = 0; for (const auto& t : mesh->subMeshTriangles) tris += t.size() / 3;
                std::fprintf(stderr, " verts=%zu submeshes=%zu tris=%zu x=[%.3f,%.3f] y=[%.3f,%.3f] z=[%.3f,%.3f]",
                    mesh->positions.size(), mesh->subMeshTriangles.size(), tris, x0, x1, y0, y1, z0, z1);
            } else std::fprintf(stderr, " readMesh failed: %s", why.c_str());
            std::fprintf(stderr, "\n    materials:");
            for (const auto& m : r.materials) {
                auto md = uf.readMaterial(m, &why);
                std::fprintf(stderr, " '%s'%s", md ? md->name.c_str() : "?", md ? "" : "(?)");
                if (md) {
                    std::fprintf(stderr, " shader='%s' queue=%d", md->shaderName.c_str(), md->renderQueue);
                    for (const auto& [prop, pp] : md->textures)
                        std::fprintf(stderr, " %s=(file%d,path%lld)", prop.c_str(), pp.fileId, (long long)pp.pathId);
                    for (const auto& [prop, c] : md->colors)
                        std::fprintf(stderr, " %s=(%.3f %.3f %.3f %.3f)", prop.c_str(), c.x, c.y, c.z, c.w);
                    for (const auto& [prop, f] : md->floats)
                        std::fprintf(stderr, " %s=%.4f", prop.c_str(), f);
                }
            }
            if (mesh && (r.name == "o_mnpa" || r.name == "o_mnpb" || r.name == "o_dankon" || r.name == "o_gomu")) {
                float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                for (const auto& uv : mesh->uv0) { u0 = std::min(u0, uv.x); u1 = std::max(u1, uv.x); v0 = std::min(v0, uv.y); v1 = std::max(v1, uv.y); }
                std::fprintf(stderr, "    uv0=[%.3f,%.3f]x[%.3f,%.3f] uv0count=%zu uv1count=%zu\n", u0, u1, v0, v1, mesh->uv0.size(), mesh->uv1.size());
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "  --- MonoBehaviours ---\n");
        for (const auto& mb : uf.monoBehavioursUnder(root)) {
            std::fprintf(stderr, "  MB class='%s' go=%lld\n", mb.className.c_str(), (long long)mb.goId);
            if (!mb.fields.isMap()) continue;
            for (const auto& [k, v] : mb.fields.asMap()) {
                const std::string key = k.asStr();
                if (key.find("ankon") == std::string::npos && key.find("mnp") == std::string::npos &&
                    key.find("omu") == std::string::npos && key.find("sex") == std::string::npos &&
                    key.find("ender") == std::string::npos && key.find("okan") == std::string::npos &&
                    key.find("tang") == std::string::npos && key.find("kin") == std::string::npos) continue;
                std::fprintf(stderr, "    %s = %s\n", key.c_str(), v.asStr().c_str());
            }
        }
    }
    return 0;
}
