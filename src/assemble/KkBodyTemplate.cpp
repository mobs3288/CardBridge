#include "assemble/KkBodyTemplate.h"

#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "unity/UnityScene.h"

#include <algorithm>
#include <optional>

namespace cardbridge::assemble {
namespace {

std::string boneName(const unity::UnityFile& file, int64_t xformId, size_t index) {
    if (const unity::Xform* x = file.xform(xformId)) {
        if (!x->name.empty()) return x->name;
    }
    return "bone_" + std::to_string(index);
}

} // namespace

bool LoadBodyTemplate(const BodyTemplateOptions& options, BodyTemplate& out, std::string& error) {
    out = BodyTemplate{};
    if (options.libraryRoot.empty()) { error = "no asset library configured"; return false; }
    try {
        const library::GameInfo info = library::ReadGameInfo(options.libraryRoot);
        std::vector<std::string> issues;
        resolve::ModIndex mods = resolve::ModIndex::Build(options.modsRoot, options.modCachePath, &issues);
        mods.setGame(info);
        resolve::ListTable lists;
        lists.loadBaseGame(library::AbdataRoot(options.libraryRoot), &issues);
        resolve::Resolver resolver(mods, lists, library::AbdataRoot(options.libraryRoot), info);

        std::string why;
        auto body = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
        if (!body) { error = "cannot open chara/oo_base.unity3d" + (why.empty() ? std::string() : ": " + why); return false; }
        out.source = info.name();

        int64_t root = body->findPrefabRoot("p_cf_body_00");
        if (!root) root = body->findPrefabRoot("p_cf_body");
        if (!root) { error = "oo_base.unity3d has no p_cf_body_00 prefab"; return false; }
        std::vector<unity::RendererInfo> renderers = body->renderersUnder(root);
        const unity::RendererInfo* best = nullptr;
        std::optional<unity::MeshData> bestMesh;
        for (const unity::RendererInfo& r : renderers) {
            if (!r.skinned || !r.active) continue;
            auto mesh = body->readMesh(r.mesh);
            if (!mesh || mesh->positions.empty()) continue;
            if (!bestMesh || mesh->positions.size() > bestMesh->positions.size()) {
                bestMesh = std::move(mesh);
                best = &r;
            }
        }
        if (!best || !bestMesh) { error = "no skinned body mesh under p_cf_body_00"; return false; }
        const unity::MeshData& mesh = *bestMesh;

        for (size_t i = 0; i < best->boneXforms.size(); ++i) {
            out.boneNames.push_back(boneName(*body, best->boneXforms[i], i));
            const Mat4 world = body->worldMatrix(best->boneXforms[i]);
            out.bonePositions.push_back({world.m[3], world.m[7], world.m[11]});
        }

        out.vertices.reserve(mesh.positions.size());
        for (size_t i = 0; i < mesh.positions.size(); ++i) {
            BodyVertex v;
            v.pos = i < mesh.positions.size() ? mesh.positions[i] : Vec3{};
            v.normal = i < mesh.normals.size() ? mesh.normals[i] : Vec3{0, 1, 0};
            v.uv = i < mesh.uv0.size() ? mesh.uv0[i] : Vec2{};
            if (i < mesh.skin.size()) {
                for (int k = 0; k < 4; ++k) {
                    v.bone[k] = mesh.skin[i].idx[k];
                    v.weight[k] = mesh.skin[i].w[k];
                }
            }
            out.vertices.push_back(v);
        }
        for (const auto& tri : mesh.subMeshTriangles)
            out.indices.insert(out.indices.end(), tri.begin(), tri.end());

        if (out.indices.empty()) { error = "body mesh has no triangles"; return false; }
        for (const std::string& issue : issues) out.warnings.push_back(issue);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace cardbridge::assemble
