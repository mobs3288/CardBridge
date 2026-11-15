// Scratch: dump a mesh's vertex bounds + triangles count from a bundle.
//   usage: meshstat <bundle> <mesh name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: meshstat <bundle> <mesh name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    const std::string want = argv[2];
    for (const auto* o : uf.serialized().ofClass(43)) {   // Mesh
        Value v = uf.serialized().read(*o);
        if (v.at("m_Name").asStr() != want) continue;
        unity::PPtr pp; pp.fileId = 0; pp.pathId = o->pathId;
        auto m = uf.readMesh(pp, nullptr);
        if (!m) { std::fprintf(stderr, "readMesh failed\n"); return 2; }
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        float umn[2] = {1e30f, 1e30f}, umx[2] = {-1e30f, -1e30f};
        for (const auto& p : m->positions) { mn[0] = std::min(mn[0], p.x); mn[1] = std::min(mn[1], p.y); mn[2] = std::min(mn[2], p.z); mx[0] = std::max(mx[0], p.x); mx[1] = std::max(mx[1], p.y); mx[2] = std::max(mx[2], p.z); }
        size_t tris = 0; for (const auto& sm : m->subMeshTriangles) tris += sm.size() / 3;
        for (const auto& uv : m->uv0) { umn[0] = std::min(umn[0], uv.x); umn[1] = std::min(umn[1], uv.y); umx[0] = std::max(umx[0], uv.x); umx[1] = std::max(umx[1], uv.y); }
        std::fprintf(stderr, "verts=%zu tris=%zu submeshes=%zu\nmin (%.3f %.3f %.3f)\nmax (%.3f %.3f %.3f)\nuv min (%.3f %.3f) uv max (%.3f %.3f)\n",
                     m->positions.size(), tris, m->subMeshTriangles.size(), mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], umn[0], umn[1], umx[0], umx[1]);
        return 0;
    }
    std::fprintf(stderr, "mesh %s not found\n", want.c_str());
    return 3;
}
