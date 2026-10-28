#include "assemble/Assembler.h"

#include "core/Bytes.h"

#include <algorithm>
#include <map>
#include <set>

namespace cardbridge::assemble {

namespace {

using unity::MaterialData;
using unity::MeshData;
using unity::PPtr;
using unity::RendererInfo;
using unity::UnityFile;

struct Rig {
    std::map<std::string, Mat4> world;   // bone name -> world matrix (first definition wins)
};

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// Adds every transform of a prefab to the scene skeleton; returns false if the prefab root is missing.
bool addRigPrefab(Scene& scene, Rig& rig, UnityFile& uf, const std::string& prefabName, const Mat4& base,
                  int attachParent, const std::string& source) {
    auto roots = uf.findXformsByName(prefabName);
    if (roots.empty()) return false;
    int64_t rootId = roots[0];
    Mat4 rootInv = inverse(uf.worldMatrix(rootId));
    std::map<int64_t, int> idx;
    for (int64_t id : uf.subtree(rootId)) {
        const unity::Xform* x = uf.xform(id);
        Mat4 w = base * (rootInv * uf.worldMatrix(id));
        int parent = attachParent;
        if (id != rootId) {
            auto p = idx.find(x->parent);
            if (p != idx.end()) parent = p->second;
        }
        int existing = scene.boneIndex(x->name);
        if (existing >= 0 || rig.world.count(x->name)) {   // duplicate name: keep the first, map this id to it
            idx[id] = existing >= 0 ? existing : parent;
            continue;
        }
        SceneBone b;
        b.name = x->name;
        b.parent = parent;
        b.pos = w.translation();
        b.source = source;
        scene.bones.push_back(b);
        idx[id] = static_cast<int>(scene.bones.size()) - 1;
        rig.world[x->name] = w;
    }
    return true;
}

struct Builder {
    Scene& scene;
    Rig& rig;
    std::map<std::string, int> textureCache;   // "<bundle>#<pathId>" -> scene texture
    std::map<std::string, int> accessoryBones; // "<part>#<xformId>" -> scene bone (accessory-owned skeletons)

    int textureFor(UnityFile& uf, const PPtr& p, const std::string& partId) {
        std::string key = uf.name() + "#" + std::to_string(p.pathId);
        auto it = textureCache.find(key);
        if (it != textureCache.end()) return it->second;
        std::string why;
        auto t = uf.readTexture(p, &why);
        int result = -1;
        if (!t) {
            scene.warn(partId, "texture not loaded: " + why);
        } else {
            SceneTexture st;
            st.name = t->name;
            st.source = uf.name();
            st.width = t->width;
            st.height = t->height;
            st.pixelHash = Fnv1a64(t->rgba.data(), t->rgba.size());
            st.rgba = std::move(t->rgba);
            scene.textures.push_back(std::move(st));
            result = static_cast<int>(scene.textures.size()) - 1;
        }
        textureCache[key] = result;
        return result;
    }

    // `defaultTexture`: the texture the item's list row names, used by materials the prefab leaves without one
    // (the game assigns it at run time with SetTexture(_MainTex, ...)).
    int materialFor(UnityFile& uf, const PPtr& p, const std::string& partId, const Vec4* fallbackColor, int defaultTexture = -1) {
        SceneMaterial sm;
        std::string why;
        auto m = uf.readMaterial(p, &why);
        if (!m) {
            scene.warn(partId, "material not loaded (" + why + "); using a plain material");
            sm.name = "material";
            if (fallbackColor) sm.diffuse = *fallbackColor;
        } else {
            sm.name = m->name;
            sm.shader = m->shaderName;
            sm.alpha = contains(m->shaderName, "alpha");
            sm.doubleSided = sm.alpha;
            if (const PPtr* tp = m->texture("_MainTex"))
                if (!tp->isNull()) sm.texture = textureFor(uf, *tp, partId);
            if (sm.texture < 0) sm.texture = defaultTexture;
            if (sm.texture < 0 && fallbackColor) sm.diffuse = *fallbackColor;
        }
        scene.materials.push_back(std::move(sm));
        return static_cast<int>(scene.materials.size()) - 1;
    }

    // Skin one renderer into the scene. `placement` positions non-skinned geometry
    // (accessories) and `staticBone` is the bone such geometry is bound to.
    // Returns the scene mesh index, or -1 when the renderer is skipped.
    // An accessory's own bones (ribbon chains, tails) are not the body skeleton even when they share names
    // with it: they hang on the attach node, so they get part-prefixed scene bones and follow `placement`.
    int accessoryBone(UnityFile& uf, int64_t xformId, int64_t prefabRoot, const Mat4& placement, int staticBone,
                      const std::string& partId) {
        std::string key = partId + "#" + std::to_string(xformId);
        auto it = accessoryBones.find(key);
        if (it != accessoryBones.end()) return it->second;
        const unity::Xform* x = uf.xform(xformId);
        if (!x) return staticBone;
        int parent = staticBone;
        if (xformId != prefabRoot && x->parent) parent = accessoryBone(uf, x->parent, prefabRoot, placement, staticBone, partId);
        SceneBone nb;
        nb.name = partId + "/" + x->name;
        nb.parent = parent;
        nb.pos = (placement * (inverse(uf.worldMatrix(prefabRoot)) * uf.worldMatrix(xformId))).translation();
        nb.source = partId;
        scene.bones.push_back(nb);
        int idx = static_cast<int>(scene.bones.size()) - 1;
        accessoryBones[key] = idx;
        return idx;
    }

    int addRenderer(UnityFile& uf, const RendererInfo& r, int64_t prefabRoot, int part, const std::string& partId,
                    const Mat4& placement, int staticBone, const Vec4* fallbackColor, bool accessoryMode = false,
                    int defaultTexture = -1) {
        std::string why;
        auto mesh = uf.readMesh(r.mesh, &why);
        if (!mesh) {
            scene.warn(partId, "renderer '" + r.name + "' skipped: " + why);
            return -1;
        }
        if (mesh->positions.empty() || mesh->subMeshTriangles.empty()) return -1;

        SceneMesh sm;
        sm.name = r.name;
        sm.part = part;
        size_t n = mesh->vertexCount();
        sm.vertices.resize(n);

        Mat4 rendererW = uf.worldMatrix(r.xformId);
        Mat4 rootInv = inverse(uf.worldMatrix(prefabRoot));
        bool skinned = r.skinned && !r.boneXforms.empty() && !mesh->skin.empty() && mesh->bindPoses.size() >= r.boneXforms.size();
        if (r.skinned && !skinned && !r.boneXforms.empty())
            scene.warn(partId, "renderer '" + r.name + "' has bones but no usable skin data; placed rigidly");

        // Per bone: the PMX bone index and the matrix that takes bind-pose vertices to model space.
        std::vector<int> boneIdx;
        std::vector<Mat4> boneMat;
        if (skinned && accessoryMode) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                int64_t bid = r.boneXforms[i];
                if (!uf.xform(bid)) { boneIdx.push_back(staticBone); boneMat.push_back(placement * (rootInv * rendererW)); continue; }
                boneIdx.push_back(accessoryBone(uf, bid, prefabRoot, placement, staticBone, partId));
                boneMat.push_back(placement * (rootInv * uf.worldMatrix(bid)) * mesh->bindPoses[i]);
            }
        } else if (skinned) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                const unity::Xform* bx = uf.xform(r.boneXforms[i]);
                std::string bname = bx ? bx->name : std::string();
                int bi = bname.empty() ? -1 : scene.boneIndex(bname);
                Mat4 bw;
                auto rw = rig.world.find(bname);
                if (rw != rig.world.end()) {
                    bw = rw->second;
                } else {
                    bw = rootInv * uf.worldMatrix(r.boneXforms[i]);
                    if (bx && bi < 0) {
                        // A bone only this item has (a mod's necklace joint ...): add it, parented to the
                        // nearest ancestor the scene already knows.
                        int parent = 0;
                        for (const unity::Xform* a = uf.xform(bx->parent); a; a = uf.xform(a->parent)) {
                            int pi = scene.boneIndex(a->name);
                            if (pi >= 0) { parent = pi; break; }
                        }
                        SceneBone nb;
                        nb.name = bname;
                        nb.parent = parent;
                        nb.pos = bw.translation();
                        nb.source = partId;
                        scene.bones.push_back(nb);
                        bi = static_cast<int>(scene.bones.size()) - 1;
                        rig.world[bname] = bw;
                    }
                }
                if (bi < 0) bi = 0;
                boneIdx.push_back(bi);
                boneMat.push_back(bw * mesh->bindPoses[i]);
            }
        }

        Mat4 staticM = placement * (rootInv * rendererW);
        for (size_t v = 0; v < n; ++v) {
            SceneVertex& out = sm.vertices[v];
            Vec3 p = mesh->positions[v];
            Vec3 nrm = v < mesh->normals.size() ? mesh->normals[v] : Vec3{0, 1, 0};
            out.uv = v < mesh->uv0.size() ? mesh->uv0[v] : Vec2{};
            if (skinned) {
                const unity::BoneInfluence& bi = mesh->skin[v];
                Vec3 pos{}, nr{};
                float wsum = 0;
                int slot = 0;
                int bones[4] = {0, 0, 0, 0};
                float ws[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    if (bi.w[k] <= 0.0f) continue;
                    if (bi.idx[k] < 0 || static_cast<size_t>(bi.idx[k]) >= boneMat.size()) continue;
                    const Mat4& m = boneMat[static_cast<size_t>(bi.idx[k])];
                    pos = pos + m.point(p) * bi.w[k];
                    nr = nr + m.dir(nrm) * bi.w[k];
                    wsum += bi.w[k];
                    bones[slot] = boneIdx[static_cast<size_t>(bi.idx[k])];
                    ws[slot] = bi.w[k];
                    ++slot;
                }
                if (wsum > 1e-6f) {
                    out.pos = pos * (1.0f / wsum);
                    out.normal = normalize(nr);
                    for (int k = 0; k < 4; ++k) { out.bone[k] = bones[k]; out.weight[k] = ws[k] / wsum; }
                } else {
                    out.pos = staticM.point(p);
                    out.normal = normalize(staticM.dir(nrm));
                    out.bone[0] = staticBone;
                    out.weight[0] = 1.0f;
                }
            } else {
                out.pos = staticM.point(p);
                out.normal = normalize(staticM.dir(nrm));
                out.bone[0] = staticBone;
                out.weight[0] = 1.0f;
            }
        }

        for (size_t s = 0; s < mesh->subMeshTriangles.size(); ++s) {
            if (mesh->subMeshTriangles[s].empty()) continue;
            SceneSubmesh ss;
            ss.indices = mesh->subMeshTriangles[s];
            size_t mi = s < r.materials.size() ? s : (r.materials.empty() ? 0 : r.materials.size() - 1);
            if (r.materials.empty()) {
                scene.warn(partId, "renderer '" + r.name + "' has no materials");
                SceneMaterial plain;
                plain.name = r.name;
                scene.materials.push_back(plain);
                ss.material = static_cast<int>(scene.materials.size()) - 1;
            } else {
                ss.material = materialFor(uf, r.materials[mi], partId, fallbackColor, defaultTexture);
            }
            sm.submeshes.push_back(std::move(ss));
        }
        scene.meshes.push_back(std::move(sm));
        return static_cast<int>(scene.meshes.size()) - 1;
    }
};

bool onlyHelperMaterials(UnityFile& uf, const RendererInfo& r) {
    if (r.materials.empty()) return false;
    for (const auto& mp : r.materials) {
        auto m = uf.readMaterial(mp, nullptr);
        if (!m) return false;
        if (!(contains(m->shaderName, "shadowcast") || m->shaderName == "Shader Forge/mnpb")) return false;
    }
    return true;
}

} // namespace

Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt) {
    Scene scene;
    scene.name = card.displayName();
    Rig rig;
    Builder b{scene, rig, {}};

    int outfit = opt.outfit;
    if (outfit < 0) outfit = static_cast<int>(card.status.at("coordinateType").asInt());
    if (outfit < 0 || static_cast<size_t>(outfit) >= card.outfits.size()) outfit = 0;

    // --- shared skeleton and body ---------------------------------------
    std::string why;
    auto base = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
    if (!base) {
        scene.error("", "the library has no chara/oo_base.unity3d (" + why + ") - cannot build a skeleton");
        return scene;
    }
    if (!addRigPrefab(scene, rig, *base, opt.bodyRigPrefab, Mat4{}, -1, "rig")) {
        scene.error("", "prefab '" + opt.bodyRigPrefab + "' not found in oo_base.unity3d");
        return scene;
    }
    int headAttach = scene.boneIndex(opt.headAttachBone);
    Mat4 headBase;
    if (headAttach >= 0) {
        auto hw = rig.world.find(opt.headAttachBone);
        if (hw != rig.world.end()) headBase = hw->second;
    } else {
        scene.warn("", "bone '" + opt.headAttachBone + "' not found; the head rig is placed at the origin");
    }
    if (!addRigPrefab(scene, rig, *base, opt.headRigPrefab, headBase, headAttach, "rig"))
        scene.warn("", "prefab '" + opt.headRigPrefab + "' not found in oo_base.unity3d");

    {
        ScenePart part;
        part.id = "body";
        part.label = "Body";
        part.group = "Body";
        part.status = "resolved-base";
        part.bundle = "chara/oo_base.unity3d";
        int partIdx = static_cast<int>(scene.parts.size());
        auto roots = base->findXformsByName(opt.bodyPrefab);
        if (roots.empty()) {
            scene.error("body", "prefab '" + opt.bodyPrefab + "' not found in oo_base.unity3d");
            part.status = "missing-bundle";
        } else {
            // The skin colour from the card stands in for the runtime-built skin texture.
            const Value& sk = card.body.at("skinMainColor");
            Vec4 skin{1, 1, 1, 1};
            if (sk.size() >= 3) skin = {static_cast<float>(sk.at(size_t(0)).asFloat()), static_cast<float>(sk.at(size_t(1)).asFloat()),
                                         static_cast<float>(sk.at(size_t(2)).asFloat()), 1.0f};
            for (const auto& r : base->renderersUnder(roots[0])) {
                if (!r.enabled) continue;
                if (r.name != "o_body_a") {   // helpers, shadow casters, anatomy meshes that are hidden in the character maker
                    continue;
                }
                int mi = b.addRenderer(*base, r, roots[0], partIdx, "body", Mat4{}, 0, &skin);
                if (mi >= 0) part.meshes.push_back(mi);
            }
            scene.warn("body", "body skin texture is built at runtime by the game from the card's skin settings; "
                               "the card's base skin colour is used instead");
        }
        scene.parts.push_back(std::move(part));
    }

    // --- card items ------------------------------------------------------
    for (const auto& ref : resolve::ItemsFromCard(card, outfit)) {
        resolve::ResolvedItem item = resolver.resolve(card, ref);
        if (item.status == resolve::ItemStatus::Empty) continue;

        ScenePart part;
        part.id = ref.partId;
        part.label = ref.label;
        part.group = ref.group;
        part.property = ref.property;
        part.category = ref.category;
        part.sourceItemId = ref.id;
        part.status = resolve::StatusName(item.status);
        part.detail = item.detail;
        part.modGuid = item.modGuid;
        part.modName = item.modName;
        part.modVersion = item.modVersion;
        part.modAuthor = item.modAuthor;
        part.modPath = item.modPath;
        int partIdx = static_cast<int>(scene.parts.size());

        if (item.ambiguous) scene.warn(ref.partId, "more than one installed file matches mod '" + item.modGuid + "'; the first was used");
        if (!item.ok()) {
            scene.warn(ref.partId, item.detail);
            scene.parts.push_back(std::move(part));
            continue;
        }

        std::string whyOpen;
        auto uf = resolver.openBundle(item, &whyOpen);
        if (!uf) {
            part.status = "missing-bundle";
            part.detail = whyOpen;
            scene.warn(ref.partId, "bundle not loaded: " + whyOpen);
            scene.parts.push_back(std::move(part));
            continue;
        }
        part.bundle = item.bundleRel();
        auto roots = uf->findXformsByName(item.assetName());
        if (roots.empty()) {
            part.status = "missing-bundle";
            part.detail = "prefab '" + item.assetName() + "' not found in " + item.bundleRel();
            scene.warn(ref.partId, part.detail);
            scene.parts.push_back(std::move(part));
            continue;
        }

        // Accessories hang on a named body node; everything else is skinned.
        Mat4 placement;
        int staticBone = 0;
        if (ref.group == "Accessories" && !ref.attachNode.empty()) {
            auto aw = rig.world.find(ref.attachNode);
            int ai = scene.boneIndex(ref.attachNode);
            if (aw != rig.world.end() && ai >= 0) {
                placement = aw->second;
                staticBone = ai;
            } else {
                scene.warn(ref.partId, "attach node '" + ref.attachNode + "' not found on the skeleton; placed at the origin");
            }
        }

        const Value& sk2 = card.body.at("skinMainColor");
        Vec4 skinColor{1, 1, 1, 1};
        if (sk2.size() >= 3) skinColor = {static_cast<float>(sk2.at(size_t(0)).asFloat()), static_cast<float>(sk2.at(size_t(1)).asFloat()),
                                           static_cast<float>(sk2.at(size_t(2)).asFloat()), 1.0f};
        bool accessoryMode = ref.group == "Accessories";

        // The list row's own texture (base-game garments carry none in the prefab).
        int listTexture = -1;
        std::string texRel = item.listItem.get("MainTexAB"), texName = item.listItem.get("MainTex");
        if (!texName.empty() && texName != "0" && !texRel.empty() && texRel != "0") {
            std::string whyTex;
            auto tb = resolver.openItemBundle(item, texRel, &whyTex);
            if (!tb) {
                scene.warn(ref.partId, "texture bundle '" + texRel + "' not loaded: " + whyTex);
            } else {
                PPtr tp = tb->findTexture(texName);
                if (tp.isNull()) scene.warn(ref.partId, "texture '" + texName + "' not found in " + texRel);
                else listTexture = b.textureFor(*tb, tp, ref.partId);
            }
        }
        std::vector<std::string> skipped;
        size_t before = scene.meshes.size();
        for (const auto& r : uf->renderersUnder(roots[0])) {
            if (!r.enabled) continue;
            if (onlyHelperMaterials(*uf, r)) continue;
            std::string path = "/" + uf->pathOf(r.xformId) + "/";
            // Variants the game keeps switched off in the default state: the pulled-off ("nuge") top and
            // bottom, and the face's tears, gag eyes and tongue.
            bool hiddenByDefault = path.find("/n_top_b/") != std::string::npos || path.find("/n_bot_b/") != std::string::npos ||
                                   (ref.group == "Head" && (contains(r.name, "namida") || contains(r.name, "gag_eye") || contains(r.name, "o_tang")));
            if (hiddenByDefault) { skipped.push_back(r.name); continue; }
            const Vec4* fb = (ref.group == "Head" && contains(r.name, "face")) ? &skinColor : nullptr;
            int mi = b.addRenderer(*uf, r, roots[0], partIdx, ref.partId, placement, staticBone, fb, accessoryMode, listTexture);
            if (mi >= 0) part.meshes.push_back(mi);
        }
        if (!skipped.empty()) {
            std::string list;
            for (const auto& n : skipped) list += (list.empty() ? "" : ", ") + n;
            scene.warn(ref.partId, "left out, hidden by default in the game: " + list);
        }
        if (scene.meshes.size() == before) {
            if (item.assetName() == "p_dummy") part.detail = "intentionally empty item (p_dummy)";
            else scene.warn(ref.partId, "no renderable geometry found in the prefab");
        }
        scene.parts.push_back(std::move(part));
    }
    return scene;
}

} // namespace cardbridge::assemble
