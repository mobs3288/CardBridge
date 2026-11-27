#include "library/AssetTemplate.h"

#include "core/FileIo.h"
#include "export/FormatCommon.h"
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
    int parent = -1;
    if (const unity::Xform* child = file.xform(xformId))
        if (child->parent)
            if (const unity::Xform* px = file.xform(child->parent))
                for (size_t i = 0; i < out.boneNames.size(); ++i)
                    if (out.boneNames[i] == px->name) { parent = static_cast<int>(i); break; }
    out.boneParents.push_back(parent);
    return static_cast<int>(out.boneNames.size()) - 1;
}

Vec4 colorOf(const unity::MaterialData& md, const char* property, Vec4 fallback) {
    const Vec4* c = md.color(property);
    return c ? *c : fallback;
}

float floatOf(const unity::MaterialData& md, const char* property, float fallback) {
    for (const auto& f : md.floats) if (f.first == property) return f.second;
    return fallback;
}

// Deterministic mapping from the source Unity material to the properties PMX
// can express. Unsupported shader features are reported by the caller.
AssetMaterial mapMaterial(const unity::MaterialData& md) {
    AssetMaterial m;
    m.name = md.name;
    m.shaderName = md.shaderName;
    m.diffuse = colorOf(md, "_Color", {1, 1, 1, 1});
    m.ambient = Vec3{colorOf(md, "_ShadowColor", {0.5f, 0.5f, 0.5f, 1}).x,
                     colorOf(md, "_ShadowColor", {0.5f, 0.5f, 0.5f, 1}).y,
                     colorOf(md, "_ShadowColor", {0.5f, 0.5f, 0.5f, 1}).z};
    m.specular = Vec3{colorOf(md, "_SpecularColor", {0, 0, 0, 1}).x,
                      colorOf(md, "_SpecularColor", {0, 0, 0, 1}).y,
                      colorOf(md, "_SpecularColor", {0, 0, 0, 1}).z};
    m.specularPower = floatOf(md, "_SpecularPower", 5.0f);
    m.edgeColor = Vec3{colorOf(md, "_LineColor", {0, 0, 0, 1}).x,
                       colorOf(md, "_LineColor", {0, 0, 0, 1}).y,
                       colorOf(md, "_LineColor", {0, 0, 0, 1}).z};
    m.edgeSize = floatOf(md, "_LineWidthS", floatOf(md, "_LineWidth", 1.0f));
    const std::string shader = md.shaderName;
    m.translucent = m.diffuse.w < 0.999f || md.renderQueue >= 3000 ||
                    shader.find("alpha") != std::string::npos ||
                    shader.find("transparent") != std::string::npos;
    m.drawFlags = static_cast<uint8_t>(0x1E | (m.translucent ? 0x00 : 0x00));
    return m;
}

std::string textureRole(const std::string& property) {
    const std::string p = property;
    if (p.find("Toon") != std::string::npos || p.find("Ramp") != std::string::npos) return "toon";
    if (p.find("Sphere") != std::string::npos || p.find("Env") != std::string::npos) return "sphere";
    if (p.find("Mask") != std::string::npos) return "mask";
    return "diffuse";
}

int resolveTexture(unity::UnityFile& file, resolve::Resolver& resolver, const std::string& keyPrefix,
                   const unity::PPtr& ref, const std::string& role, AssetTemplate& out) {
    if (ref.isNull()) return -1;
    const std::string key = keyPrefix + "|" + std::to_string(ref.pathId);
    for (size_t i = 0; i < out.textures.size(); ++i)
        if (out.textures[i].key == key) return static_cast<int>(i);

    auto tex = file.readTexture(ref);
    if (!tex && ref.fileId > 0) {
        const std::string external = file.externalNameOf(ref);
        if (!external.empty()) {
            std::string why2;
            auto other = resolver.openByCab(external, &why2);
            if (other) tex = other->readTexture(ref);
        }
    }
    if (!tex || tex->rgba.empty()) return -1;

    assemble::SceneTexture st;
    st.name = std::to_string(ref.pathId);
    st.width = tex->width; st.height = tex->height; st.rgba = tex->rgba;
    Bytes png;
    try { png = exportcommon::EncodePng(st); } catch (const std::exception&) { return -1; }
    if (png.empty()) return -1;
    AssetTexture at;
    at.key = key;
    at.name = std::to_string(ref.pathId) + ".png";
    at.role = role;
    at.source = file.source().empty() ? keyPrefix : file.source();
    at.bytes = std::move(png);
    at.width = tex->width; at.height = tex->height;
    out.textures.push_back(std::move(at));
    return static_cast<int>(out.textures.size()) - 1;
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

        int defaultMaterial = -1;
        auto ensureDefault = [&]() {
            if (defaultMaterial >= 0) return defaultMaterial;
            AssetMaterial m;
            m.name = "default";
            m.shaderName = "MMD";
            out.materials.push_back(m);
            defaultMaterial = static_cast<int>(out.materials.size()) - 1;
            return defaultMaterial;
        };

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

            // Keep every submesh's material association and index range. PMX
            // requires indices grouped by material, which this order provides.
            for (size_t s = 0; s < mesh->subMeshTriangles.size(); ++s) {
                const size_t first = out.indices.size();
                for (uint32_t idx : mesh->subMeshTriangles[s])
                    if (idx < mesh->positions.size()) out.indices.push_back(static_cast<uint32_t>(base + idx));
                const size_t count = out.indices.size() - first;
                if (!count) continue;

                int matIndex = -1;
                const size_t slot = unity::SubmeshMaterialSlot(*mesh, s, r);
                if (slot < r.materials.size()) {
                    std::string why;
                    auto md = file->readMaterial(r.materials[slot], &why);
                    if (md) {
                        AssetMaterial am = mapMaterial(*md);
                        for (const auto& [prop, ptr] : md->textures) {
                            const std::string role = textureRole(prop);
                            const int ti = resolveTexture(*file, resolver, record.key, ptr, role, out);
                            if (ti < 0) {
                                out.warnings.push_back("texture '" + prop + "' of material '" + md->name + "' could not be extracted");
                                continue;
                            }
                            if (role == "toon") am.toonTexture = ti;
                            else if (role == "sphere") { am.sphereTexture = ti; am.sphereMode = 2; }
                            else am.diffuseTexture = ti;
                        }
                        if (md->textures.empty())
                            out.warnings.push_back("material '" + md->name + "' has no textures");
                        out.materials.push_back(std::move(am));
                        matIndex = static_cast<int>(out.materials.size()) - 1;
                    } else {
                        out.warnings.push_back("material could not be read (" + why + ")");
                    }
                }
                if (matIndex < 0) matIndex = ensureDefault();
                out.submeshRanges.emplace_back(first, count);
                out.submeshMaterials.push_back(matIndex);
            }
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
