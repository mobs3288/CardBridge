#include "assemble/Assembler.h"

#include "core/Bytes.h"
#include "shape/Shape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
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

// A local transform the card's shape sliders replace on a named bone.
struct LocalOverride {
    Vec3 pos;
    Quat rot;
    bool hasRot = false;
    Vec3 scl{1, 1, 1};
};
using Overrides = std::map<std::string, LocalOverride>;

// Adds every transform of a prefab to the scene skeleton, with world matrices built from local transforms
// (so a bone's shape override moves its whole subtree); returns false if the prefab root is missing.
bool addRigPrefab(Scene& scene, Rig& rig, UnityFile& uf, const std::string& prefabName, const Mat4& base,
                  int attachParent, const std::string& source, const Overrides* overrides = nullptr) {
    auto roots = uf.findXformsByName(prefabName);
    if (roots.empty()) return false;
    int64_t rootId = roots[0];
    std::map<int64_t, int> idx;
    std::map<int64_t, Mat4> worlds;
    for (int64_t id : uf.subtree(rootId)) {
        const unity::Xform* x = uf.xform(id);
        Mat4 w;
        int parent = attachParent;
        if (id == rootId) {
            w = base;
        } else {
            Vec3 pos = x->pos, scl = x->scl;
            Quat rot = x->rot;
            if (overrides) {
                auto ov = overrides->find(x->name);
                if (ov != overrides->end()) {
                    pos = ov->second.pos;
                    scl = ov->second.scl;
                    if (ov->second.hasRot) rot = ov->second.rot;
                }
            }
            auto pw = worlds.find(x->parent);
            w = (pw != worlds.end() ? pw->second : base) * Mat4::TRS(pos, rot, scl);
            auto p = idx.find(x->parent);
            if (p != idx.end()) parent = p->second;
        }
        worlds[id] = w;
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

// The card's body sliders as local transforms for the skeleton's driven bones (ChaControl.UpdateShapeBodyValueFromCustomInfo
// followed by ShapeBodyInfoFemale.Update/UpdateAlways, copied as generated code in shape/).
Overrides bodyShapeOverrides(const card::Card& card, resolve::Resolver& resolver, UnityFile& base, UnityFile& rigFile,
                            const std::string& rigPrefab, Scene& scene) {
    Overrides out;
    std::string why;
    shape::AnimKeys anm;
    bool haveAnm = false;
    for (const auto& [name, bytes] : base.readTextAssets())
        if (name == "cf_anmShapeBody") { anm = shape::AnimKeys::Parse(bytes); haveAnm = true; }
    auto custom = resolver.openBaseBundle("list/customshape.unity3d", &why);
    std::string catText;
    if (custom)
        for (const auto& [name, bytes] : custom->readTextAssets())
            if (name == "cf_custombody") catText.assign(bytes.begin(), bytes.end());
    if (!haveAnm || catText.empty()) {
        scene.warn("body", "the library has no body shape tables (cf_anmShapeBody / list/customshape.unity3d); default proportions are used");
        return out;
    }
    std::map<std::string, int> srcIds;
    const auto& srcNames = shape::BodySrcNames();
    for (size_t i = 0; i < srcNames.size(); ++i) srcIds[srcNames[i]] = static_cast<int>(i);
    shape::CategoryTable cats = shape::ParseCategories(catText, srcIds);

    // Local transforms of every rig bone, so partial assignments (only pos.x ...) keep the rest.
    std::map<std::string, shape::ShapeBone> bones;
    auto roots = rigFile.findXformsByName(rigPrefab);
    if (roots.empty()) return out;
    for (int64_t id : rigFile.subtree(roots[0])) {
        const unity::Xform* x = rigFile.xform(id);
        if (bones.count(x->name)) continue;
        shape::ShapeBone sb;
        sb.pos = x->pos;
        sb.scl = x->scl;
        bones[x->name] = sb;
    }

    shape::ShapeState S;
    S.typeBone = static_cast<int>(card.body.at("typeBone").asInt());
    if (S.typeBone != 0)
        scene.warn("body", "this body uses an alternative bone type (typeBone " + std::to_string(S.typeBone) +
                           "); its correction table is not loaded, proportions may be slightly off");
    const auto& dstNames = shape::BodyDstNames();
    S.dstFn = [&](int i) -> shape::ShapeBone* {
        if (i < 0 || static_cast<size_t>(i) >= dstNames.size()) return nullptr;
        auto it = bones.find(dstNames[static_cast<size_t>(i)]);
        return it == bones.end() ? nullptr : &it->second;
    };
    auto fc = [&](const char* n) -> shape::ShapeBone* { auto it = bones.find(n); return it == bones.end() ? nullptr : &it->second; };
    S.fixCorrect[0] = fc("cf_d_shoulder_L");
    S.fixCorrect[1] = fc("cf_d_shoulder_R");

    const Value& vals = card.body.at("shapeValueBody");
    std::vector<float> v;
    for (size_t i = 0; i < vals.size(); ++i) v.push_back(static_cast<float>(vals.at(i).asFloat()));
    if (v.size() > 13) {
        float nip = static_cast<float>(card.status.at("nipStandRate").asFloat());
        v[13] = v[13] + (1.0f - v[13]) * std::min(1.0f, std::max(0.0f, nip));
    }
    for (size_t i = 0; i < v.size(); ++i) shape::ChangeValue(S, anm, cats, static_cast<int>(i), v[i]);
    shape::RunBodyUpdate(S, 7);
    shape::RunBodyUpdateAlways(S);

    for (const auto& [name, sb] : bones) {
        LocalOverride o;
        o.pos = sb.pos;
        o.scl = sb.scl;
        o.hasRot = sb.rotSet;
        if (sb.rotSet) o.rot = shape::EulerToQuat(sb.rot);
        out[name] = o;
    }
    return out;
}

// How an item's materials are coloured. The game builds clothing textures at run time from the list row's
// MainTex and ColorMaskTex plus the card's colours and assigns them to the first material of the renderers its
// ChaClothesComponent lists; accessories keep the prefab texture and are tinted through their colour mask.
struct ItemLook {
    int tex1 = -1, tex2 = -1;             // baked scene textures for the clothing main / second texture
    std::set<int64_t> use1, use2;         // renderer component ids that get tex1 / tex2 on material 0
    bool materialTint = false;            // accessories: tint each material's own texture by its _ColorMask
    Vec4 colors[3]{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
};

// main * lerp-by-mask of the three colours: red channel region gets colour 1, green colour 2, blue colour 3,
// everything outside the mask stays as it is. A missing mask is the game's dummy: pure red, so colour 1 everywhere.
std::vector<uint8_t> tintTexture(const unity::TextureData& main, const unity::TextureData* mask, const Vec4 c[3]) {
    std::vector<uint8_t> out(main.rgba.size());
    for (int y = 0; y < main.height; ++y)
        for (int x = 0; x < main.width; ++x) {
            size_t i = (static_cast<size_t>(y) * main.width + x) * 4;
            float mr = 1, mg = 0, mb = 0;
            if (mask && mask->width > 0 && mask->height > 0) {
                int mx = std::min(mask->width - 1, x * mask->width / main.width);
                int my = std::min(mask->height - 1, y * mask->height / main.height);
                const uint8_t* m = &mask->rgba[(static_cast<size_t>(my) * mask->width + mx) * 4];
                mr = m[0] / 255.0f; mg = m[1] / 255.0f; mb = m[2] / 255.0f;
            }
            float col[3] = {main.rgba[i] / 255.0f, main.rgba[i + 1] / 255.0f, main.rgba[i + 2] / 255.0f};
            const float mk[3] = {mr, mg, mb};
            for (int k = 0; k < 3; ++k) {
                const Vec4& cc = c[k];
                float tint[3] = {cc.x, cc.y, cc.z};
                for (int ch = 0; ch < 3; ++ch) col[ch] = col[ch] + (col[ch] * tint[ch] - col[ch]) * mk[k];
            }
            for (int ch = 0; ch < 3; ++ch) out[i + static_cast<size_t>(ch)] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, col[ch] * 255.0f)));
            out[i + 3] = main.rgba[i + 3];
        }
    return out;
}

struct Builder {
    const std::map<int64_t, Mat4>* localOv = nullptr;   // local-matrix overrides for the prefab being added (accessory N_move nodes)
    Mat4 W(UnityFile& uf, int64_t id) const { return uf.worldMatrix(id, localOv); }
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

    std::map<std::string, std::shared_ptr<unity::TextureData>> decoded;   // "<bundle>#<pathId>" -> decoded pixels
    std::map<std::string, int> bakedCache;                                  // tint recipe -> scene texture

    std::shared_ptr<unity::TextureData> decode(UnityFile& uf, const PPtr& p, const std::string& partId) {
        std::string key = uf.name() + "#" + std::to_string(p.pathId);
        auto it = decoded.find(key);
        if (it != decoded.end()) return it->second;
        std::string why;
        auto t = uf.readTexture(p, &why);
        std::shared_ptr<unity::TextureData> out;
        if (t) out = std::make_shared<unity::TextureData>(std::move(*t));
        else scene.warn(partId, "texture not loaded: " + why);
        decoded[key] = out;
        return out;
    }

    int addSceneTexture(const std::string& name, const std::string& source, int w, int h, std::vector<uint8_t>&& rgba) {
        SceneTexture st;
        st.name = name;
        st.source = source;
        st.width = w;
        st.height = h;
        st.pixelHash = Fnv1a64(rgba.data(), rgba.size());
        st.rgba = std::move(rgba);
        scene.textures.push_back(std::move(st));
        return static_cast<int>(scene.textures.size()) - 1;
    }

    // A texture tinted by a colour mask, cached by (main, mask, colours).
    int bake(const std::shared_ptr<unity::TextureData>& main, const std::string& mainKey, const std::shared_ptr<unity::TextureData>& mask,
             const std::string& maskKey, const Vec4 c[3], const std::string& source) {
        if (!main) return -1;
        char buf[160];
        std::snprintf(buf, sizeof buf, "|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z);
        std::string key = mainKey + "|" + maskKey + buf;
        auto it = bakedCache.find(key);
        if (it != bakedCache.end()) return it->second;
        int idx = addSceneTexture(main->name + "_tint", source, main->width, main->height, tintTexture(*main, mask.get(), c));
        bakedCache[key] = idx;
        return idx;
    }

    // `look` (may be null) says how this item's materials are coloured; `matIndex` is the material's slot on its renderer.
    int materialFor(UnityFile& uf, const PPtr& p, const std::string& partId, const Vec4* fallbackColor, const ItemLook* look,
                    const RendererInfo& r, size_t matIndex) {
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
            sm.doubleSided = sm.alpha || contains(m->shaderName, "hair");   // hair cards are drawn from both sides in the game; a culled PMX loses half of them
            if (look && matIndex == 0) {
                if (look->use1.count(r.componentId) && look->tex1 >= 0) sm.texture = look->tex1;
                else if (look->use2.count(r.componentId) && look->tex2 >= 0) sm.texture = look->tex2;
            }
            if (sm.texture < 0) {
                const PPtr* tp = m->texture("_MainTex");
                if (tp && !tp->isNull()) {
                    if (look && look->materialTint) {
                        auto main = decode(uf, *tp, partId);
                        const PPtr* mp = m->texture("_ColorMask");
                        std::shared_ptr<unity::TextureData> mask;
                        std::string maskKey = "-";
                        if (mp && !mp->isNull() && mp->isLocal()) { mask = decode(uf, *mp, partId); maskKey = uf.name() + "#" + std::to_string(mp->pathId); }
                        sm.texture = bake(main, uf.name() + "#" + std::to_string(tp->pathId), mask, maskKey, look->colors, uf.name());
                    } else {
                        sm.texture = textureFor(uf, *tp, partId);
                    }
                }
            }
            if (sm.texture < 0 && look && look->materialTint) sm.diffuse = {look->colors[0].x, look->colors[0].y, look->colors[0].z, 1.0f};
            else if (sm.texture < 0 && fallbackColor) sm.diffuse = *fallbackColor;
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
        nb.pos = (placement * (inverse(W(uf, prefabRoot)) * W(uf, xformId))).translation();
        nb.source = partId;
        scene.bones.push_back(nb);
        int idx = static_cast<int>(scene.bones.size()) - 1;
        accessoryBones[key] = idx;
        return idx;
    }

    int addRenderer(UnityFile& uf, const RendererInfo& r, int64_t prefabRoot, int part, const std::string& partId,
                    const Mat4& placement, int staticBone, const Vec4* fallbackColor, bool accessoryMode = false,
                    const ItemLook* look = nullptr) {
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

        Mat4 rendererW = W(uf, r.xformId);
        Mat4 rootInv = inverse(W(uf, prefabRoot));
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
                boneMat.push_back(placement * (rootInv * W(uf, bid)) * mesh->bindPoses[i]);
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
                    // A bone only this item has (a mod's necklace joint ...): keep its offset from the nearest ancestor the
                    // shared skeleton knows, so it moves with the (possibly reshaped) body instead of staying at its authored place.
                    bw = rootInv * W(uf, r.boneXforms[i]);
                    for (const unity::Xform* a = bx ? uf.xform(bx->parent) : nullptr; a; a = uf.xform(a->parent)) {
                        auto ar = rig.world.find(a->name);
                        if (ar == rig.world.end()) continue;
                        bw = ar->second * (inverse(rootInv * W(uf, a->id)) * (rootInv * W(uf, r.boneXforms[i])));
                        break;
                    }
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
                ss.material = materialFor(uf, r.materials[mi], partId, fallbackColor, look, r, mi);
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
    Builder b{nullptr, scene, rig, {}};

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
    Overrides shapeOv;
    if (opt.applyShape) shapeOv = bodyShapeOverrides(card, resolver, *base, *base, opt.bodyRigPrefab, scene);
    if (!addRigPrefab(scene, rig, *base, opt.bodyRigPrefab, Mat4{}, -1, "rig", shapeOv.empty() ? nullptr : &shapeOv)) {
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

    auto vec4Of = [](const Value& v) {
        return Vec4{static_cast<float>(v.at(size_t(0)).asFloat()), static_cast<float>(v.at(size_t(1)).asFloat()),
                    static_cast<float>(v.at(size_t(2)).asFloat()), static_cast<float>(v.size() > 3 ? v.at(size_t(3)).asFloat() : 1.0)};
    };
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
                // Skin: the body's base texture tinted by its colour mask with the card's two skin colours (the game does
                // the same at run time; overlays for paint, sunburn, nipples and underhair are not applied).
                ItemLook bodyLook;
                const ItemLook* bodyLookPtr = nullptr;
                {
                    PPtr bt = base->findTexture("cf_body_00_t"), bm = base->findTexture("cf_body_00_mc");
                    if (!bt.isNull() && !bm.isNull()) {
                        Vec4 c[3] = {skin, vec4Of(card.body.at("skinSubColor")), Vec4{1, 1, 1, 1}};
                        bodyLook.tex1 = b.bake(b.decode(*base, bt, "body"), "body_t", b.decode(*base, bm, "body"), "body_mc", c, base->name());
                        bodyLook.use1.insert(r.componentId);
                        bodyLookPtr = &bodyLook;
                    }
                }
                int mi = b.addRenderer(*base, r, roots[0], partIdx, "body", Mat4{}, 0, &skin, false, bodyLookPtr);
                if (mi >= 0) part.meshes.push_back(mi);
            }
            scene.warn("body", "body skin texture is built at runtime by the game from the card's skin settings; "
                               "the card's base skin colour is used instead");
        }
        scene.parts.push_back(std::move(part));
    }

    // --- card items ------------------------------------------------------
    std::vector<resolve::ItemRef> work = resolve::ItemsFromCard(card, outfit);
    const Value& sk2 = card.body.at("skinMainColor");
    Vec4 skinColor{1, 1, 1, 1};
    if (sk2.size() >= 3) skinColor = vec4Of(sk2);

    bool topCoversBra = false;
    for (size_t wi = 0; wi < work.size(); ++wi) {
        const resolve::ItemRef ref = work[wi];
        resolve::ResolvedItem item = resolver.resolve(card, ref);
        if (item.status == resolve::ItemStatus::Empty) continue;

        // A jacket-type (2) or sailor-type (1) top is mostly empty itself: the garments are its sub-parts A/B/C,
        // categories 210+k / 200+k, with ids from the coordinate's subPartsId (ChaControl.ChangeClothesSubParts).
        if (ref.slot == 0 && ref.subKind < 0 && item.ok()) {
            int topKind = std::atoi(item.listItem.get("Kind").c_str());
            if (topKind == 1 || topKind == 2) {
                static const char* letter[3] = {"A", "B", "C"};
                static const char* sname[3] = {"a", "b", "c"};
                const Value& ids = card.outfits[static_cast<size_t>(outfit)].clothes.at("subPartsId");
                for (int k = 0; k < 3; ++k) {
                    resolve::ItemRef sub;
                    sub.partId = ref.partId + ".sub_" + sname[k];
                    sub.label = std::string("top part ") + letter[k];
                    sub.group = "Outfit";
                    sub.property = "outfit" + std::to_string(outfit) + ".ChaFileClothes.Clothes" + (topKind == 1 ? "Sailor" : "Jacket") + "Sub" + letter[k];
                    sub.category = (topKind == 1 ? 200 : 210) + k;
                    sub.id = ids.at(static_cast<size_t>(k)).asInt();
                    sub.slot = 0;
                    sub.subKind = k;
                    sub.outfit = outfit;
                    work.push_back(sub);
                }
            }
        }

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
        bool accessoryMode = ref.group == "Accessories";

        // ---- colouring: what the game does at run time with the card's colours ----
        ItemLook look;
        bool haveLook = false;
        if (ref.group == "Outfit" && ref.outfit >= 0) {
            // Sub-parts use the top's colours; part C's first colour is the fourth slot (ChaControl.ChangeCustomClothes).
            size_t colorSlot = ref.subKind >= 0 ? 0 : static_cast<size_t>(std::max(0, ref.slot));
            const Value& ci = card.outfits[static_cast<size_t>(ref.outfit)].clothes.at("parts").at(colorSlot).at("colorInfo");
            Vec4 c[3] = {vec4Of(ci.at(size_t(0)).at("baseColor")), vec4Of(ci.at(size_t(1)).at("baseColor")), vec4Of(ci.at(size_t(2)).at("baseColor"))};
            if (ref.subKind == 2) c[0] = vec4Of(ci.at(size_t(3)).at("baseColor"));

            // The list row's textures; a list bundle of "0" means the item's own bundle.
            auto listTex = [&](const char* abKey, const char* nameKey, std::string& keyOut) -> std::shared_ptr<unity::TextureData> {
                std::string ab = item.listItem.get(abKey), name = item.listItem.get(nameKey);
                if (name.empty() || name == "0") return nullptr;
                std::shared_ptr<UnityFile> tb = uf;
                if (!ab.empty() && ab != "0") {
                    std::string w;
                    tb = resolver.openItemBundle(item, ab, &w);
                    if (!tb) { scene.warn(ref.partId, "texture bundle '" + ab + "' not loaded: " + w); return nullptr; }
                }
                PPtr tp = tb->findAsset(name);
                if (tp.isNull()) tp = tb->findTexture(name);
                if (tp.isNull()) { scene.warn(ref.partId, "texture '" + name + "' not found in " + tb->name()); return nullptr; }
                keyOut = tb->name() + "#" + std::to_string(tp.pathId);
                return b.decode(*tb, tp, ref.partId);
            };
            std::string k1, k2, k3, k4;
            auto main1 = listTex("MainTexAB", "MainTex", k1);
            auto mask1 = listTex("ColorMaskAB", "ColorMaskTex", k2);
            auto main2 = listTex("MainTex02AB", "MainTex02", k3);
            auto mask2 = listTex("ColorMask02AB", "ColorMask02Tex", k4);
            // The game only builds a texture when it has both the main texture and its mask.
            if (main1 && mask1) { look.tex1 = b.bake(main1, k1, mask1, k2, c, uf->name()); haveLook = true; }
            if (main2 && mask2) { look.tex2 = b.bake(main2, k3, mask2, k4, c, uf->name()); haveLook = true; }
            if (haveLook) {
                for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
                    if (mb.className != "ChaClothesComponent") continue;
                    auto collect = [&](const char* field, std::set<int64_t>& into) {
                        for (const auto& pp : mb.fields.at(field).asArray())
                            if (pp.at("m_FileID").asInt() == 0 && pp.at("m_PathID").asInt() != 0) into.insert(pp.at("m_PathID").asInt());
                    };
                    collect("rendNormal01", look.use1);
                    collect("rendAlpha01", look.use1);
                    collect("rendNormal02", look.use2);
                    collect("rendAlpha02", look.use2);
                }
            }
        } else if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("color");
            if (col.size() >= 3) {
                for (int k = 0; k < 3; ++k) look.colors[k] = vec4Of(col.at(static_cast<size_t>(k)));
                look.materialTint = true;
                haveLook = true;
            }
        }

        // Face parts: the game builds each one's texture from the list row the card's face ids select,
        // tinted with the card's colours (ChaControl.ChangeSettingEyebrow / Eyeline / Eye ...).
        const Value& faceV = card.face;

        // The texture a list row names for a card id (base or mod), decoded; null for "none" or when missing.
        auto rowTexture = [&](int category, int64_t id, const std::string& property, const char* texKey, const std::string& what,
                              std::string* keyOut = nullptr) -> std::shared_ptr<unity::TextureData> {
            resolve::ItemRef fr;
            fr.partId = ref.partId + "." + what;
            fr.group = "Face";
            fr.property = property;
            fr.category = category;
            fr.id = id;
            resolve::ResolvedItem fi = resolver.resolve(card, fr);
            if (fi.status == resolve::ItemStatus::Empty) return nullptr;
            if (!fi.ok()) {
                scene.warn(ref.partId, what + ": " + (fi.detail.empty() ? "no texture item for this id" : fi.detail));
                return nullptr;
            }
            std::string texName = fi.listItem.get(texKey), ab = fi.listItem.get("MainAB");
            if (texName.empty() || texName == "0" || ab.empty() || ab == "0") return nullptr;   // "none"
            std::string w;
            auto tb = resolver.openItemBundle(fi, ab, &w);
            if (!tb) { scene.warn(ref.partId, what + ": texture bundle not loaded: " + w); return nullptr; }
            PPtr tp = tb->findAsset(texName);
            if (tp.isNull()) tp = tb->findTexture(texName);
            if (tp.isNull()) { scene.warn(ref.partId, what + ": texture '" + texName + "' not found in " + ab); return nullptr; }
            if (keyOut) *keyOut = tb->name() + "#" + std::to_string(tp.pathId);
            return b.decode(*tb, tp, ref.partId);
        };

        // Face parts: the game builds each one's texture from the list row the card's face ids select,
        // tinted with the card's colours (ChaControl.ChangeSettingEyebrow / Eyeline / Eye ...).
        auto faceTextureFor = [&](const std::string& rendererName) -> int {
            struct Spec { const char* renderer; int category; int64_t id; const char* property; const char* texKey; Vec4 color; };
            auto colorOf = [&](const char* field) { return vec4Of(faceV.at(field)); };
            std::vector<Spec> specs;
            specs.push_back({"cf_O_mayuge", 406, faceV.at("eyebrowId").asInt(), "ChaFileFace.eyebrowId", "EyebrowTex", colorOf("eyebrowColor")});
            specs.push_back({"cf_O_eyeline", 412, faceV.at("eyelineUpId").asInt(), "ChaFileFace.eyelineUpId", "EyelineUpTex", colorOf("eyelineColor")});
            specs.push_back({"cf_O_eyeline_low", 413, faceV.at("eyelineDownId").asInt(), "ChaFileFace.eyelineDownId", "EyelineDownTex", colorOf("eyelineColor")});
            specs.push_back({"cf_O_noseline", 414, faceV.at("noseId").asInt(), "ChaFileFace.noseId", "NoseTex", Vec4{0.25f, 0.2f, 0.2f, 1.0f}});
            specs.push_back({"cf_Ohitomi_L", 407, faceV.at("whiteId").asInt(), "ChaFileFace.whiteId", "EyeWhiteTex", colorOf("whiteBaseColor")});
            specs.push_back({"cf_Ohitomi_R", 407, faceV.at("whiteId").asInt(), "ChaFileFace.whiteId", "EyeWhiteTex", colorOf("whiteBaseColor")});
            const Value& pupils = faceV.at("pupil");
            specs.push_back({"cf_Ohitomi_L02", 408, pupils.at(size_t(0)).at("id").asInt(), "ChaFileFace.Pupil1", "EyeTex", vec4Of(pupils.at(size_t(0)).at("baseColor"))});
            specs.push_back({"cf_Ohitomi_R02", 408, pupils.at(size_t(1)).at("id").asInt(), "ChaFileFace.Pupil2", "EyeTex", vec4Of(pupils.at(size_t(1)).at("baseColor"))});
            for (const Spec& sp : specs) {
                if (rendererName != sp.renderer) continue;
                std::string key;
                auto tex = rowTexture(sp.category, sp.id, sp.property, sp.texKey, sp.renderer, &key);
                if (!tex) return -1;
                Vec4 c[3] = {sp.color, Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}};
                return b.bake(tex, key, nullptr, "-", c, uf->name());
            }
            return -1;
        };

        // The face skin: the head's own base texture tinted by its colour mask with the card's two skin colours,
        // then the makeup layers (cheek, lip line, lips, eye shadow) blended on top in the same UV layout
        // (ChaControl.ChangeSettingFaceBaseTex/Color and the overlay textures of the face material).
        auto faceSkinTexture = [&]() -> int {
            auto fromHead = [&](const char* abKey, const char* nameKey, std::string& key) -> std::shared_ptr<unity::TextureData> {
                std::string ab = item.listItem.get(abKey), name = item.listItem.get(nameKey);
                if (name.empty() || name == "0") return nullptr;
                std::shared_ptr<UnityFile> tb = uf;
                if (!ab.empty() && ab != "0") {
                    std::string w;
                    tb = resolver.openItemBundle(item, ab, &w);
                    if (!tb) { scene.warn(ref.partId, "face skin: texture bundle '" + ab + "' not loaded: " + w); return nullptr; }
                }
                PPtr tp = tb->findAsset(name);
                if (tp.isNull()) tp = tb->findTexture(name);
                if (tp.isNull()) { scene.warn(ref.partId, "face skin: texture '" + name + "' not found in " + tb->name()); return nullptr; }
                key = tb->name() + "#" + std::to_string(tp.pathId);
                return b.decode(*tb, tp, ref.partId);
            };
            std::string k1, k2;
            auto base = fromHead("MainTexAB", "MainTex", k1);
            if (!base) return -1;
            auto mask = fromHead("ColorMaskAB", "ColorMaskTex", k2);
            Vec4 c[3] = {skinColor, vec4Of(card.body.at("skinSubColor")), Vec4{1, 1, 1, 1}};
            unity::TextureData out = *base;
            out.name = base->name + "_skin";
            out.rgba = tintTexture(*base, mask.get(), c);

            const auto& oc = card.outfits[static_cast<size_t>(std::max(0, ref.outfit < 0 ? 0 : ref.outfit))];
            bool coordMakeup = oc.enableMakeup;
            const Value& mk = coordMakeup ? oc.makeup : faceV.at("baseMakeup");
            std::string pre = coordMakeup ? "outfit" + std::to_string(ref.outfit < 0 ? 0 : ref.outfit) + "." : "";
            auto blend = [&](const std::shared_ptr<unity::TextureData>& ov, Vec4 col) {
                if (!ov) return;
                for (int y = 0; y < out.height; ++y)
                    for (int x = 0; x < out.width; ++x) {
                        int ox = std::min(ov->width - 1, x * ov->width / out.width), oy = std::min(ov->height - 1, y * ov->height / out.height);
                        const uint8_t* o = &ov->rgba[(static_cast<size_t>(oy) * ov->width + ox) * 4];
                        float a = o[3] / 255.0f * std::min(1.0f, col.w);
                        if (a <= 0.0f) continue;
                        uint8_t* d = &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4];
                        d[0] = static_cast<uint8_t>(d[0] + (col.x * 255.0f - d[0]) * a);
                        d[1] = static_cast<uint8_t>(d[1] + (col.y * 255.0f - d[1]) * a);
                        d[2] = static_cast<uint8_t>(d[2] + (col.z * 255.0f - d[2]) * a);
                    }
            };
            blend(rowTexture(402, mk.at("cheekId").asInt(), pre + "ChaFileMakeup.cheekId", "CheekTex", "cheek"), vec4Of(mk.at("cheekColor")));
            blend(rowTexture(404, faceV.at("lipLineId").asInt(), "ChaFileFace.lipLineId", "LiplineTex", "lipline"), vec4Of(faceV.at("lipLineColor")));
            blend(rowTexture(403, mk.at("lipId").asInt(), pre + "ChaFileMakeup.lipId", "LipTex", "lip"), vec4Of(mk.at("lipColor")));
            blend(rowTexture(401, mk.at("eyeshadowId").asInt(), pre + "ChaFileMakeup.eyeshadowId", "EyeshadowTex", "eyeshadow"), vec4Of(mk.at("eyeshadowColor")));
            return b.addSceneTexture(out.name, uf->name(), out.width, out.height, std::move(out.rgba));
        };
        int faceSkin = -2;   // computed once, on the first face renderer that needs it

        // The card's per-accessory move/rotate/scale (ChaFileAccessory.PartsInfo.addMove): the game sets these on the
        // prefab's N_move / N_move2 nodes (position in centimetres). Without them duplicated pieces stack in one place.
        std::map<int64_t, Mat4> moveOv;
        if (accessoryMode && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& am = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("addMove");
            const Value& flat = am.at(size_t(2));
            for (int k = 0; k < 2; ++k) {
                auto nodes = uf->findXformsByName(k == 0 ? "N_move" : "N_move2");
                if (nodes.empty() || flat.size() < static_cast<size_t>(k * 3 + 3)) continue;
                Vec3 pos = {static_cast<float>(flat.at(static_cast<size_t>(k * 3)).at(size_t(0)).asFloat()) * 0.01f,
                            static_cast<float>(flat.at(static_cast<size_t>(k * 3)).at(size_t(1)).asFloat()) * 0.01f,
                            static_cast<float>(flat.at(static_cast<size_t>(k * 3)).at(size_t(2)).asFloat()) * 0.01f};
                const Value& rv = flat.at(static_cast<size_t>(k * 3 + 1));
                Vec3 rot = {static_cast<float>(rv.at(size_t(0)).asFloat()), static_cast<float>(rv.at(size_t(1)).asFloat()), static_cast<float>(rv.at(size_t(2)).asFloat())};
                const Value& sv = flat.at(static_cast<size_t>(k * 3 + 2));
                Vec3 scl = {static_cast<float>(sv.at(size_t(0)).asFloat()), static_cast<float>(sv.at(size_t(1)).asFloat()), static_cast<float>(sv.at(size_t(2)).asFloat())};
                for (int64_t n : nodes) moveOv[n] = Mat4::TRS(pos, shape::EulerToQuat(rot), scl);
            }
        }
        b.localOv = moveOv.empty() ? nullptr : &moveOv;

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
            // Debug aid: CARDBRIDGE_SKIP=name1,name2 leaves those renderers out (to find which mesh draws an artefact).
            if (const char* sk = std::getenv("CARDBRIDGE_SKIP")) {
                std::string list = std::string(",") + sk + ",";
                if (list.find("," + r.name + ",") != std::string::npos) { skipped.push_back(r.name + " (CARDBRIDGE_SKIP)"); continue; }
            }
            const Vec4* fb = (ref.group == "Head" && contains(r.name, "face")) ? &skinColor : nullptr;
            ItemLook faceLook;
            const ItemLook* lookPtr = haveLook ? &look : nullptr;
            if (ref.group == "Head") {
                int ft = -1;
                if (contains(r.name, "cf_O_face")) {
                    if (faceSkin == -2) faceSkin = faceSkinTexture();
                    ft = faceSkin;
                } else {
                    ft = faceTextureFor(r.name);
                    // These parts are drawn only through their texture; with none (a "none" item, or a mod texture that
                    // could not be loaded) the game shows nothing, but an unlit white mesh would be a white smear.
                    static const char* textureOnly[] = {"cf_O_mayuge", "cf_O_eyeline", "cf_O_eyeline_low", "cf_O_noseline"};
                    for (const char* n : textureOnly)
                        if (ft < 0 && r.name == n) { skipped.push_back(r.name + " (no texture)"); ft = -3; }
                    if (ft == -3) continue;
                }
                if (ft >= 0) {
                    faceLook.tex1 = ft;
                    faceLook.use1.insert(r.componentId);
                    lookPtr = &faceLook;
                }
            }
            int mi = b.addRenderer(*uf, r, roots[0], partIdx, ref.partId, placement, staticBone, fb, accessoryMode, lookPtr);
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
        if (ref.group == "Outfit" && ref.slot == 0 && item.listItem.get("NotBra") == "1") topCoversBra = true;
        b.localOv = nullptr;
        scene.parts.push_back(std::move(part));
    }

    // A top whose list row says NotBra hides the bra in the game (it would only show through the garment).
    if (topCoversBra)
        for (auto& p : scene.parts)
            if (p.group == "Outfit" && p.id.size() >= 4 && p.id.compare(p.id.size() - 4, 4, ".bra") == 0 && !p.meshes.empty()) {
                p.includeInExport = false;
                p.visibleInPreview = false;
                p.detail = "hidden: the top covers the bra (list NotBra=1); the game does not draw it either";
            }
    return scene;
}

} // namespace cardbridge::assemble
