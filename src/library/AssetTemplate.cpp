#include "library/AssetTemplate.h"

#include "core/FileIo.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "unity/UnityScene.h"

#include <algorithm>
#include <map>

namespace cardbridge::library {
namespace {

// Adds a renderer bone to the template's global bone list, deduplicated by name.
int globalBone(const unity::UnityFile& file, int64_t xformId, AssetTemplate& out) {
    std::string name;
    if (const unity::Xform* x = file.xform(xformId)) name = x->name;
    if (name.empty()) name = "bone_" + std::to_string(xformId);
    for (size_t i = 0; i < out.boneNames.size(); ++i)
        if (out.boneNames[i] == name) return static_cast<int>(i);
    const Mat4 world = file.worldMatrix(xformId);
    out.boneNames.push_back(name);
    out.bonePositions.push_back({world.m[3], world.m[7], world.m[11]});
    return static_cast<int>(out.boneNames.size()) - 1;
}

} // namespace

bool LoadAssetTemplate(const AssetRecord& record, const AssetTemplateOptions& options,
                       AssetTemplate& out, std::string& error) {
    out = AssetTemplate{};
    if (options.libraryRoot.empty()) { error = "no asset library configured"; return false; }
    try {
        const GameInfo info = ReadGameInfo(options.libraryRoot);
        std::vector<std::string> issues;
        resolve::ModIndex mods = resolve::ModIndex::Build(options.modsRoot, options.modCachePath, &issues);
        mods.setGame(info);
        resolve::ListTable lists;
        lists.loadBaseGame(AbdataRoot(options.libraryRoot), &issues);
        resolve::Resolver resolver(mods, lists, AbdataRoot(options.libraryRoot), info);

        resolve::ResolvedItem item;
        item.ref.category = record.categoryNo;
        item.ref.id = record.id;
        item.ref.attachNode = record.parent;
        item.status = record.mod ? resolve::ItemStatus::ResolvedMod : resolve::ItemStatus::ResolvedBase;
        item.modGuid = record.modGuid;
        item.modName = record.modName;
        item.modPath = record.modPath;
        item.listItem.keys["MainAB"] = record.bundle;
        item.listItem.keys["MainData"] = record.asset;
        if (!record.parent.empty()) item.listItem.keys["Parent"] = record.parent;
        for (const std::string& dep : record.textureDeps) {
            const size_t eq = dep.find('=');
            if (eq == std::string::npos) continue;
            item.listItem.keys[dep.substr(0, eq)] = dep.substr(eq + 1);
        }
        if (record.mod) resolver.registerBundleProvider(item);

        std::string why;
        auto file = resolver.openBundle(item, &why);
        if (!file && !record.bundle.empty()) file = resolver.openItemBundle(item, record.bundle, &why);
        if (!file) { error = "cannot open asset bundle '" + record.bundle + "'" + (why.empty() ? std::string() : ": " + why); return false; }

        out.key = record.key;
        out.name = record.name;
        out.attachNode = record.parent;
        int64_t root = file->findPrefabRoot(record.asset);
        if (!root && !record.name.empty()) root = file->findPrefabRoot(record.name);
        if (!root) { error = "asset prefab '" + record.asset + "' not found in " + record.bundle; return false; }
        const auto renderers = file->renderersUnder(root);
        if (renderers.empty()) { error = "asset prefab has no renderers"; return false; }

        for (const unity::RendererInfo& r : renderers) {
            if (!r.active) continue;
            auto mesh = file->readMesh(r.mesh);
            if (!mesh || mesh->positions.empty()) continue;
            std::map<int, int> localBone;
            for (size_t b = 0; b < r.boneXforms.size(); ++b)
                localBone[static_cast<int>(b)] = globalBone(*file, r.boneXforms[b], out);

            int rigidBone = -1;
            if (mesh->skin.empty() || mesh->skin.size() != mesh->positions.size()) {
                const unity::Xform* xf = file->xform(r.xformId);
                const int64_t parent = xf ? xf->parent : 0;
                rigidBone = globalBone(*file, parent ? parent : r.xformId, out);
            }
            const size_t base = out.vertices.size();
            for (size_t i = 0; i < mesh->positions.size(); ++i) {
                AssetVertex v;
                v.pos = mesh->positions[i];
                v.normal = i < mesh->normals.size() ? mesh->normals[i] : Vec3{0, 1, 0};
                v.uv = i < mesh->uv0.size() ? mesh->uv0[i] : Vec2{};
                if (rigidBone >= 0) {
                    v.bone[0] = rigidBone; v.weight[0] = 1.0f;
                } else if (i < mesh->skin.size()) {
                    for (int k = 0; k < 4; ++k) {
                        auto it = localBone.find(mesh->skin[i].idx[k]);
                        if (it == localBone.end()) continue;
                        v.bone[k] = it->second;
                        v.weight[k] = mesh->skin[i].w[k];
                    }
                }
                out.vertices.push_back(v);
            }
            for (const auto& tri : mesh->subMeshTriangles)
                for (uint32_t idx : tri)
                    if (idx < mesh->positions.size()) out.indices.push_back(static_cast<uint32_t>(base + idx));
        }
        if (out.vertices.empty() || out.indices.empty()) {
            error = "asset has no usable geometry";
            return false;
        }
        out.warnings = std::move(issues);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace cardbridge::library
