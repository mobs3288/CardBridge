// Scratch: minimal FixGarmentIntersections repro — prints stats + where each vertex landed.
//   usage: fixcheck
#include "assemble/Assembler.h"
#include "assemble/Scene.h"
#include <cstdio>
using namespace cardbridge;
using namespace cardbridge::assemble;

int main() {
    Scene s;
    s.name = "fixcheck";
    auto addBone = [&](const char* name, int parent, Vec3 pos) {
        SceneBone b; b.name = name; b.parent = parent; b.pos = pos; b.source = "rig"; b.world = Mat4{}; s.bones.push_back(b);
    };
    addBone("root", -1, {0, 0, 0});

    // body sphere at y=0.8, r=0.5
    SceneMesh body;
    body.name = "o_body";
    body.reshapable = true;
    const int rings = 14, sectors = 28;
    const int stride = sectors + 1;
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings, phi = 3.14159265f * v;
        for (int si = 0; si < stride; ++si) {
            const float u = (float)si / (float)sectors, th = 2.0f * 3.14159265f * u;
            SceneVertex vert;
            vert.pos = {0.5f * std::sin(phi) * std::cos(th), 0.8f + 0.5f * std::cos(phi), 0.5f * std::sin(phi) * std::sin(th)};
            vert.normal = normalize(Vec3{vert.pos.x, vert.pos.y - 0.8f, vert.pos.z});
            vert.uv = {u, 1.0f - v};
            vert.uv1 = vert.uv; vert.uv2 = vert.uv; vert.uv3 = vert.uv;
            vert.bone[0] = 0; vert.weight[0] = 1.0f;
            for (int k = 0; k < 4; ++k) { vert.local[k] = vert.pos; vert.nlocal[k] = vert.normal; }
            body.vertices.push_back(vert);
        }
    }
    SceneSubmesh bsm; bsm.material = 0;
    for (int r = 0; r < rings; ++r)
        for (int si = 0; si < sectors; ++si) {
            const int a = r * stride + si, b = a + 1, c = a + stride, d = c + 1;
            bsm.indices.insert(bsm.indices.end(), {(uint32_t)a, (uint32_t)c, (uint32_t)b, (uint32_t)b, (uint32_t)c, (uint32_t)d});
        }
    body.submeshes.push_back(bsm);
    s.meshes.push_back(std::move(body));
    ScenePart bp; bp.id = "body"; bp.group = "Body"; bp.meshes = {0};
    s.parts.push_back(bp);

    SceneMesh cloth;
    cloth.name = "o_top";
    cloth.reshapable = true;
    auto mk = [&](float y) {
        SceneVertex v;
        v.pos = {0.0f, y, 0.0f};
        v.normal = {0, 1, 0}; v.uv = {0, 0}; v.uv1 = v.uv; v.uv2 = v.uv; v.uv3 = v.uv;
        v.bone[0] = 0; v.weight[0] = 1.0f;
        for (int k = 0; k < 4; ++k) { v.local[k] = v.pos; v.nlocal[k] = v.normal; }
        return v;
    };
    for (float y : {1.295f, 1.301f, 1.31f, 5.0f, 0.8f}) cloth.vertices.push_back(mk(y));
    SceneSubmesh csm; csm.material = 0;
    cloth.submeshes.push_back(csm);
    s.meshes.push_back(std::move(cloth));
    ScenePart tp; tp.id = "outfit0.top"; tp.group = "Outfit"; tp.meshes = {1};
    s.parts.push_back(tp);

    const auto stats = FixGarmentIntersections(s);
    std::fprintf(stderr, "stats: moved=%zu parts=%zu skipped=%zu maxMove=%.6f\n", stats.verticesMoved, stats.partsFixed, stats.skipped, stats.maxMove);
    for (size_t i = 0; i < s.meshes[1].vertices.size(); ++i)
        std::fprintf(stderr, "  v[%zu] pos=(%.6f %.6f %.6f)\n", i, s.meshes[1].vertices[i].pos.x, s.meshes[1].vertices[i].pos.y, s.meshes[1].vertices[i].pos.z);
    for (const auto& is : s.issues) std::fprintf(stderr, "  issue[%s] (%s) %s\n", is.severity.c_str(), is.part.c_str(), is.message.c_str());
    return 0;
}
