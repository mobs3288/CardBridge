// Scratch: dump a prefab's transform hierarchy (names, pos, rot, scale) from an asset bundle.
//   usage: ufdump <bundle> <prefab-go-name> [dump renderers too]
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <memory>
#include <string>
using namespace cardbridge;

static void walk(const unity::UnityFile& uf, int64_t id, int depth) {
    const unity::Xform* x = uf.xform(id);
    if (!x) return;
    for (int i = 0; i < depth; ++i) std::fprintf(stderr, "  ");
    std::fprintf(stderr, "%s pos=(%.4f %.4f %.4f) rot=(%.3f %.3f %.3f %.3f) scl=(%.3f %.3f %.3f)\n", x->name.c_str(), x->pos.x, x->pos.y, x->pos.z,
                 x->rot.x, x->rot.y, x->rot.z, x->rot.w, x->scl.x, x->scl.y, x->scl.z);
    for (int64_t cid : uf.subtree(id)) {
        if (cid != id && uf.xform(cid) && uf.xform(cid)->parent == id) walk(uf, cid, depth + 1);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: ufdump <bundle> <prefab-go-name> [dump renderers too]\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) { std::fprintf(stderr, "cannot open bundle\n"); return 1; }
    unity::UnityFile uf(bundle);
    std::string want = argv[2];
    int64_t root = uf.findPrefabRoot(want);
    if (!root) { std::fprintf(stderr, "prefab root '%s' not found\n", want.c_str()); return 1; }
    std::fprintf(stderr, "prefab root: %s\n", uf.pathOf(root).c_str());
    walk(uf, root, 0);
    std::fprintf(stderr, "\n-- renderers --\n");
    for (const auto& r : uf.renderersUnder(root)) {
        std::fprintf(stderr, "renderer '%s' enabled=%d active=%d skinned=%d mesh=%lld materials=%zu\n", r.name.c_str(), r.enabled ? 1 : 0, r.active ? 1 : 0, r.skinned ? 1 : 0, (long long)r.mesh.pathId, r.materials.size());
    }
    return 0;
}
