#include "assemble/Assembler.h"

#include "core/Bytes.h"
#include "shape/Shape.h"
#include "card/MaterialEdits.h"
#include "core/MsgPack.h"
#include "export/Preview.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
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


// Card colours are used as stored. An earlier "colour conversion" (a saturation boost fitted to two samples) tinted
// near-white clothes green and skin-like colours brown; the game's own shader does whatever conversion there is.
Vec4 kkColor(Vec4 c) { return c; }

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
    if (std::getenv("CARDBRIDGE_DEBUG_SHAPE"))
        for (int id : {0, 1, 26, 27})
            std::fprintf(stderr, "src %d %s scl %g %g %g\n", id, srcNames[static_cast<size_t>(id)].c_str(), S.src(id).scl.x, S.src(id).scl.y, S.src(id).scl.z);
    if (std::getenv("CARDBRIDGE_DEBUG_SHAPE")) { std::fprintf(stderr, "body values:"); for (size_t i = 0; i < v.size(); ++i) std::fprintf(stderr, " %zu=%.3f", i, v[i]); std::fprintf(stderr, "\n"); }
    shape::RunBodyUpdate(S, 7);
    shape::RunBodyUpdateAlways(S);

    for (const auto& [name, sb] : bones) {
        LocalOverride o;
        o.pos = sb.pos;
        o.scl = sb.scl;
        o.hasRot = sb.rotSet;
        if (sb.rotSet) o.rot = shape::EulerToQuat(sb.rot);
        if (std::getenv("CARDBRIDGE_DEBUG_SHAPE"))
            std::fprintf(stderr, "shape %s pos %g %g %g scl %g %g %g\n", name.c_str(), o.pos.x, o.pos.y, o.pos.z, o.scl.x, o.scl.y, o.scl.z);
        out[name] = o;
    }
    return out;
}

Overrides headShapeOverrides(const card::Card& card, resolve::Resolver& resolver, const resolve::ResolvedItem& headItem,
                             UnityFile& headBundle, UnityFile& rigFile, const std::string& rigPrefab, const Mat4& headParentWorld,
                             Scene& scene) {
    Overrides out;
    std::string why;
    shape::AnimKeys anm;
    const std::string anmName = headItem.listItem.get("ShapeAnime");
    bool haveAnm = false;
    if (!anmName.empty() && anmName != "0")
        for (const auto& [name, bytes] : headBundle.readTextAssets())
            if (name == anmName) { anm = shape::AnimKeys::Parse(bytes); haveAnm = true; }
    auto custom = resolver.openBaseBundle("list/customshape.unity3d", &why);
    std::string catText;
    if (custom)
        for (const auto& [name, bytes] : custom->readTextAssets())
            if (name == "cf_customhead") catText.assign(bytes.begin(), bytes.end());
    if (!haveAnm || catText.empty()) {
        scene.warn("head", "face shape tables not found (ShapeAnime '" + anmName + "' / cf_customhead); the default face shape is used");
        return out;
    }
    std::map<std::string, int> srcIds;
    const auto& srcNames = shape::HeadSrcNames();
    for (size_t i = 0; i < srcNames.size(); ++i) srcIds[srcNames[i]] = static_cast<int>(i);
    shape::CategoryTable cats = shape::ParseCategories(catText, srcIds);

    std::map<std::string, shape::ShapeBone> bones;
    std::map<std::string, std::string> parentOf;
    std::map<const shape::ShapeBone*, std::string> nameOf;
    auto roots = rigFile.findXformsByName(rigPrefab);
    if (roots.empty()) return out;
    for (int64_t id : rigFile.subtree(roots[0])) {
        const unity::Xform* x = rigFile.xform(id);
        if (bones.count(x->name)) continue;
        shape::ShapeBone sb;
        sb.pos = x->pos;
        sb.scl = x->scl;
        bones[x->name] = sb;
        if (id != roots[0]) if (const unity::Xform* par = rigFile.xform(x->parent)) parentOf[x->name] = par->name;
    }
    for (auto& [n, b] : bones) nameOf[&b] = n;

    // World scale of the node the head rig hangs from (cf_s_head: the body shape scales it).
    auto colLen = [&](int c) { return std::sqrt(headParentWorld.m[0 * 4 + c] * headParentWorld.m[0 * 4 + c] + headParentWorld.m[1 * 4 + c] * headParentWorld.m[1 * 4 + c] + headParentWorld.m[2 * 4 + c] * headParentWorld.m[2 * 4 + c]); };
    const Vec3 attachScale{colLen(0), colLen(1), colLen(2)};

    shape::ShapeState S;
    S.typeBone = static_cast<int>(card.body.at("typeBone").asInt());
    const auto& dstNames = shape::HeadDstNames();
    S.dstFn = [&](int i) -> shape::ShapeBone* {
        if (i < 0 || static_cast<size_t>(i) >= dstNames.size()) return nullptr;
        auto it = bones.find(dstNames[static_cast<size_t>(i)]);
        return it == bones.end() ? nullptr : &it->second;
    };
    S.parentScaleFn = [&](shape::ShapeBone* b) -> Vec3 {
        Vec3 r = attachScale;
        auto nm = nameOf.find(b);
        std::string cur = nm == nameOf.end() ? std::string() : nm->second;
        for (auto pi = parentOf.find(cur); pi != parentOf.end(); pi = parentOf.find(cur)) {
            const shape::ShapeBone& pb = bones[pi->second];
            r = {r.x * pb.scl.x, r.y * pb.scl.y, r.z * pb.scl.z};
            cur = pi->second;
        }
        return r;
    };
    const Value& vals = card.face.at("shapeValueFace");
    const bool neutral = std::getenv("CARDBRIDGE_FACE_NEUTRAL") != nullptr;   // debug: every slider at 0.5
    for (size_t i = 0; i < vals.size(); ++i) shape::ChangeValue(S, anm, cats, static_cast<int>(i), neutral ? 0.5f : static_cast<float>(vals.at(i).asFloat()));
    if (std::getenv("CARDBRIDGE_DEBUG_SHAPE")) {
        std::fprintf(stderr, "face values:");
        for (size_t i = 0; i < vals.size(); ++i) std::fprintf(stderr, " %zu=%.3f", i, vals.at(i).asFloat());
        std::fprintf(stderr, "\n");
    }
    shape::RunHeadUpdate(S);
    shape::RunHeadUpdateAlways(S);

    for (const auto& [name, sb] : bones) {
        LocalOverride o;
        o.pos = sb.pos;
        o.scl = sb.scl;
        o.hasRot = sb.rotSet;
        if (sb.rotSet) o.rot = shape::EulerToQuat(sb.rot);
        if (std::getenv("CARDBRIDGE_DEBUG_SHAPE"))
            std::fprintf(stderr, "headshape %s pos %g %g %g scl %g %g %g rot%s %g %g %g\n", name.c_str(), o.pos.x, o.pos.y, o.pos.z, o.scl.x, o.scl.y, o.scl.z,
                         sb.rotSet ? "*" : "", sb.rot.x, sb.rot.y, sb.rot.z);
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

// Source-over of an embedded image onto `dst` (nearest resampling when the sizes differ).
void alphaOver(std::vector<uint8_t>& dst, int dw, int dh, const card::MaterialEdits::Image& ov) {
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            int ox = std::min(ov.width - 1, x * ov.width / dw), oy = std::min(ov.height - 1, y * ov.height / dh);
            const uint8_t* o = &ov.rgba[(static_cast<size_t>(oy) * ov.width + ox) * 4];
            float a = o[3] / 255.0f;
            if (a <= 0.0f) continue;
            uint8_t* d = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            float da = d[3] / 255.0f, oa = a + da * (1.0f - a);
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((o[c] * a + d[c] * da * (1.0f - a)) / std::max(oa, 1e-6f));
            d[3] = static_cast<uint8_t>(oa * 255.0f);
        }
}

struct Builder {
    const std::map<int64_t, Mat4>* localOv = nullptr;   // local-matrix overrides for the prefab being added (accessory N_move nodes)
    Mat4 W(UnityFile& uf, int64_t id) const { return uf.worldMatrix(id, localOv); }
    Scene& scene;
    Rig& rig;
    std::map<std::string, int> textureCache;   // "<bundle>#<pathId>" -> scene texture
    std::map<std::string, int> accessoryBones; // "<part>#<xformId>" -> scene bone (accessory-owned skeletons)

    // MaterialEditor texture replacements stored in the card, for the item being added (-1 type = none).
    // Colours the game's ChaControl writes into the item's materials at run time (ChangeAccessoryColor, ChangeSettingHairColor ...),
    // by Unity property; the shader reads them. Set per renderer before it is added.
    std::vector<std::pair<std::string, Vec4>> runtime;
    bool glossForThisRenderer = false;
    int hairGlossTexture = -1;   // the card's hair gloss map (_HairGloss of hair renderers)
    // Head only: blend-shape weights (0..100) of the character's resting expression, by renderer GameObject id and shape index.
    const std::map<int64_t, std::vector<std::pair<int, float>>>* blend = nullptr;
    // Textures the game puts on a material at run time besides its main one (face lips and eye shadow, body nipples and underhair):
    // property -> scene texture, bound for the renderer being added. Colours for them go through `runtime`.
    std::vector<std::pair<std::string, int>> extraTex;
    std::vector<std::pair<std::string, Vec4>> stOverride;   // texture property -> scale.xy offset.xy (the eye look controller's iris and highlight placement)
    float blushAlpha = -1;       // face material only: alpha of _overcolor2, the emotion blush (ChaControl.ChangeHohoAkaRate); < 0 = leave
    const Mat4* hairParent = nullptr;   // world matrix of the hair parent node while a hair item is added
    const card::MaterialEdits* edits = nullptr;
    int meCoordinate = 0, meType = -1, meSlot = 0;
    std::map<int, int> meTextures;   // embedded TexID -> scene texture (-1 when unreadable)

    int embeddedTexture(int texId, const std::string& partId) {
        auto it = meTextures.find(texId);
        if (it != meTextures.end()) return it->second;
        card::MaterialEdits::Image img;
        int result = -1;
        if (edits && edits->decode(texId, img)) result = addSceneTexture("card_tex_" + std::to_string(texId), "card (MaterialEditor)", img.width, img.height, std::move(img.rgba));
        else scene.warn(partId, "MaterialEditor texture " + std::to_string(texId) + " stored in the card could not be decoded");
        meTextures[texId] = result;
        return result;
    }

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

    std::map<std::string, int> shaderCache;    // "<bundle>#<pathId>" -> scene shader (-1 = unreadable)
    std::map<std::string, int> shaderTexCache; // "<bundle>#<pathId>" -> scene texture, at most 1024 px on a side

    resolve::Resolver* resolverPtr = nullptr;
    std::map<std::string, int> shaderByNameCache;

    // A shader by its Unity name, for MaterialEditor shader overrides: one already in the scene, else found in the base bundles
    // that carry the character shaders.
    int shaderByName(const std::string& name) {
        auto c = shaderByNameCache.find(name);
        if (c != shaderByNameCache.end()) return c->second;
        int result = -1;
        for (size_t i = 0; i < scene.shaders.size() && result < 0; ++i)
            if (scene.shaders[i].name == name) result = static_cast<int>(i);
        static const char* bundles[] = {"chara/bo_head_00.unity3d", "chara/oo_base.unity3d", "chara/mm_base.unity3d", "chara/co_top_00.unity3d", "chara/ao_hair_00.unity3d"};
        for (const char* bp : bundles) {
            if (result >= 0 || !resolverPtr) break;
            std::string why;
            auto f = resolverPtr->openBaseBundle(bp, &why);
            if (!f) continue;
            for (const auto* o : f->serialized().ofClass(48)) {   // Shader
                Value v = f->serialized().read(*o);
                std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
                if (n.empty()) n = v.at("m_Name").asStr();
                if (n == name) { result = shaderFor(*f, o->pathId, name); break; }
            }
        }
        shaderByNameCache[name] = result;
        return result;
    }

    int shaderFor(UnityFile& uf, int64_t pathId, const std::string& name) {
        std::string key = uf.name() + "#" + std::to_string(pathId);
        auto it = shaderCache.find(key);
        if (it != shaderCache.end()) return it->second;
        int result = -1;
        for (size_t i = 0; i < scene.shaders.size(); ++i)
            if (scene.shaders[i].name == name) { result = static_cast<int>(i); break; }   // the same shader from another bundle
        if (result < 0) {
            std::string why;
            if (auto blob = uf.readShaderBlob(pathId, &why)) {
                SceneShader sh;
                sh.name = name;
                sh.blob = std::move(*blob);
                sh.texDefaults = uf.readShaderTextureDefaults(pathId);
                sh.propDefaults = uf.readShaderPropDefaults(pathId);
                scene.shaders.push_back(std::move(sh));
                result = static_cast<int>(scene.shaders.size()) - 1;
            }
        }
        shaderCache[key] = result;
        return result;
    }

    // A material texture for the shader: decoded once and shrunk to 1024 px (box filter) so a card's dozen maps stay small.
    std::function<std::shared_ptr<UnityFile>(const std::string&)> openCab;   // library bundle for an external file name

    int shaderTexture(UnityFile& uf, const PPtr& p, const std::string& partId) {
        if (p.isNull()) return -1;
        UnityFile* src = &uf;
        std::shared_ptr<UnityFile> hold;
        PPtr lp = p;
        if (!p.isLocal()) {   // a shared texture (ramp, gloss, normal map) lives in another bundle of the library
            if (!openCab) return -1;
            hold = openCab(uf.externalNameOf(p));
            if (!hold) { if (std::getenv("CARDBRIDGE_DEBUG_MAT")) std::fprintf(stderr, "ext texture %s in %s: no library bundle\n", uf.externalNameOf(p).c_str(), uf.name().c_str()); return -1; }
            src = hold.get();
            lp.fileId = 0;
        }
        std::string key = src->name() + "#" + std::to_string(lp.pathId);
        auto it = shaderTexCache.find(key);
        if (it != shaderTexCache.end()) return it->second;
        int result = -1;
        auto t = decode(*src, lp, partId);
        if (t && t->width > 0 && t->height > 0) {
            int sx = 1, sy = 1;
            while (t->width / sx > 1024) sx *= 2;
            while (t->height / sy > 1024) sy *= 2;
            int w = t->width / sx, h = t->height / sy;
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 4; ++c) {
                        int sum = 0;
                        for (int dy = 0; dy < sy; ++dy)
                            for (int dx = 0; dx < sx; ++dx) sum += t->rgba[(static_cast<size_t>(y * sy + dy) * t->width + (x * sx + dx)) * 4 + c];
                        px[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(sum / (sx * sy));
                    }
            result = addSceneTexture(t->name, src->name(), w, h, std::move(px));
            scene.textures[static_cast<size_t>(result)].clamp = t->wrapMode == 1;
        }
        shaderTexCache[key] = result;
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
            sm.name = "material";
            if (look && matIndex == 0) {   // a renderer whose material lives elsewhere still gets the texture the game assigns it
                if (look->use1.count(r.componentId) && look->tex1 >= 0) sm.texture = look->tex1;
                else if (look->use2.count(r.componentId) && look->tex2 >= 0) sm.texture = look->tex2;
            }
            if (sm.texture < 0) scene.warn(partId, "material not loaded (" + why + "); using a plain material");
            if (sm.texture >= 0) sm.alpha = true;
            if (sm.texture < 0 && fallbackColor) sm.diffuse = *fallbackColor;
        } else {
            sm.name = m->name;
            sm.shader = m->shaderName;
            if (m->shaderPathId) sm.shaderIndex = shaderFor(uf, m->shaderPathId, m->shaderName);
            for (const auto& c : m->colors) sm.props.emplace_back(c.first, c.second);
            for (const auto& rp : runtime) {
                bool found = false;
                for (auto& pr : sm.props) if (pr.first == rp.first) { pr.second = rp.second; found = true; }
                if (!found) sm.props.push_back(rp);
            }
            if (blushAlpha >= 0)
                for (auto& pr : sm.props) if (pr.first == "_overcolor2") pr.second.w = blushAlpha;
            for (const auto& f : m->floats) sm.props.emplace_back(f.first, Vec4{f.second, 0, 0, 0});
            for (const auto& tb : m->textures) {
                SceneTexBinding b;
                b.name = tb.first;
                for (const auto& st : m->texST) if (st.first == tb.first) b.st = st.second;
                b.texture = shaderTexture(uf, tb.second, partId);
                sm.texProps.push_back(std::move(b));
            }
            for (const auto& [name, tex] : extraTex) {
                bool set = false;
                for (auto& tb : sm.texProps) if (tb.name == name) { tb.texture = tex; set = true; }
                if (!set) { SceneTexBinding nb; nb.name = name; nb.texture = tex; sm.texProps.push_back(nb); }
            }
            for (const auto& [name, st] : stOverride)
                for (auto& tb : sm.texProps) if (tb.name == name) tb.st = st;
            if (hairGlossTexture >= 0 && glossForThisRenderer) {
                bool set = false;
                for (auto& tb : sm.texProps) if (tb.name == "_HairGloss") { tb.texture = hairGlossTexture; set = true; }
                if (!set) { SceneTexBinding g; g.name = "_HairGloss"; g.texture = hairGlossTexture; sm.texProps.push_back(g); }
            }
            if (const Vec4* c = m->color("_ShadowColor")) sm.shadow = {c->x, c->y, c->z};
            if (const Vec4* c = m->color("_LineColor")) sm.outlineColor = {c->x, c->y, c->z};
            float specPow = 0.0f;
            for (const auto& f : m->floats) {
                if (f.first == "_SpecularPower") specPow = f.second;
                else if (f.first == "_LineWidthS") sm.outlineWidth = f.second;
                else if (f.first == "_LineWidth") sm.outlineWidth = f.second;
            }
            if (const Vec4* c = m->color("_SpecularColor")) {
                float k = std::min(1.0f, std::max(0.0f, specPow));   // _SpecularPower is the highlight strength in these shaders
                sm.specular = {c->x * k, c->y * k, c->z * k};
            }
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
        if (sm.texture >= 0) {   // the texture this exporter assigns (baked, tinted or replaced) is what the shader gets as _MainTex
            bool found = false;
            for (auto& b : sm.texProps) if (b.name == "_MainTex") { b.texture = sm.texture; found = true; }
            if (!found) { SceneTexBinding b; b.name = "_MainTex"; b.texture = sm.texture; sm.texProps.push_back(std::move(b)); }
        }
        if (edits && meType >= 0) {
            // MaterialEditor's shader, float and colour overrides for this material.
            const unity::MaterialData* m_original = m ? &*m : nullptr;
            for (const auto* e : edits->editsFor(edits->shaders, meCoordinate, meType, meSlot, sm.name)) {
                int si = shaderByName(e->property);
                if (si < 0) continue;
                // Changing a material's shader keeps the values of properties both shaders have; the new shader's own
                // properties start at its declared defaults (the card's run-time colours are for the old shader).
                if (m_original) {
                    for (const auto& d : scene.shaders[static_cast<size_t>(si)].propDefaults) {
                        bool had = false;
                        for (const auto& c : m_original->colors) if (c.first == d.first) had = true;
                        for (const auto& f : m_original->floats) if (f.first == d.first) had = true;
                        if (had) continue;
                        bool set = false;
                        for (auto& pr : sm.props) if (pr.first == d.first) { pr.second = d.second; set = true; }
                        if (!set) sm.props.emplace_back(d.first, d.second);
                    }
                }
                sm.shaderIndex = si;
                sm.shader = e->property;
                sm.alpha = contains(e->property, "alpha");
                sm.doubleSided = sm.alpha || contains(e->property, "hair");
            }
            auto setProp = [&](const std::string& name, Vec4 v) {
                for (auto& pr : sm.props) if (pr.first == name) { pr.second = v; return; }
                sm.props.emplace_back(name, v);
            };
            for (const auto* e : edits->editsFor(edits->floats, meCoordinate, meType, meSlot, sm.name)) setProp("_" + e->property, Vec4{e->value[0], 0, 0, 0});
            for (const auto* e : edits->editsFor(edits->colors, meCoordinate, meType, meSlot, sm.name)) setProp("_" + e->property, Vec4{e->value[0], e->value[1], e->value[2], e->value[3]});
            int id = edits->mainTexFor(meCoordinate, meType, meSlot, sm.name);
            if (id >= 0) {
                int t = embeddedTexture(id, partId);
                if (t >= 0) { sm.texture = t; sm.diffuse = {1, 1, 1, 1}; }
            }
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
        if (blend) {
            auto bw = blend->find(r.goId);
            if (bw != blend->end())
                for (const auto& [shapeIndex, weight] : bw->second) mesh->applyBlendShape(static_cast<size_t>(shapeIndex), weight);
        }

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
                    bool anchored = false;
                    for (const unity::Xform* a = bx ? uf.xform(bx->parent) : nullptr; a; a = uf.xform(a->parent)) {
                        auto ar = rig.world.find(a->name);
                        if (ar == rig.world.end()) continue;
                        bw = ar->second * (inverse(rootInv * W(uf, a->id)) * (rootInv * W(uf, r.boneXforms[i])));
                        anchored = true;
                        break;
                    }
                    // A hair item's prefab hangs under the head's hair parent (cf_J_FaceUp_ty) in the game.
                    if (!anchored && hairParent) bw = *hairParent * (rootInv * W(uf, r.boneXforms[i]));
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
            out.uv1 = v < mesh->uv1.size() ? mesh->uv1[v] : out.uv;
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

// True when the path ("/a/b/") runs through a clothes-state node other than the worn one: n_top_b, n_bot_c ...
bool hiddenClothesState(const std::string& path) {
    for (const char* kind : {"/n_top_", "/n_bot_"}) {
        for (size_t at = path.find(kind); at != std::string::npos; at = path.find(kind, at + 1)) {
            size_t state = at + std::strlen(kind);
            if (state + 1 < path.size() && path[state + 1] == '/' && path[state] != 'a') return true;
        }
    }
    return false;
}

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
    b.resolverPtr = &resolver;
    b.openCab = [&resolver](const std::string& n) { std::string w; return resolver.openByCab(n, &w); };

    int outfit = opt.outfit;
    if (outfit < 0) outfit = static_cast<int>(card.status.at("coordinateType").asInt());
    if (outfit < 0 || static_cast<size_t>(outfit) >= card.outfits.size()) outfit = 0;
    // Shader globals as Character.UpdateGlobalShader / ChaShader set them at their defaults (Config.EtcData: rampId 1,
    // shadowDepth 0.26, lineDepth 1, lineWidth 0.307).
    scene.globalProps.emplace_back("_ambientshadowG", Vec4{0.019f, 0.027f, 0.039f, 0.26f});
    scene.globalProps.emplace_back("_LineColorG", Vec4{0.5f, 0.5f, 0.5f, 0.0f});
    scene.globalProps.emplace_back("_linewidthG", Vec4{0.307f, 0, 0, 0});
    scene.globalProps.emplace_back("_FaceShadowG", Vec4{std::getenv("FS") ? static_cast<float>(std::atof(std::getenv("FS"))) : 0.0f, 0, 0, 0});
    scene.globalProps.emplace_back("_FaceNormalG", Vec4{std::getenv("FN") ? static_cast<float>(std::atof(std::getenv("FN"))) : 0.0f, 0, 0, 0});
    auto loadListTexture = [&](int category, int64_t id) -> int {
        const resolve::ListItem* row = resolver.baseRow(category, id);
        if (!row) return -1;
        std::string ab = row->get("MainTexAB"), name = row->get("MainTex");
        if (ab.empty() || ab == "0" || name.empty() || name == "0") return -1;
        std::string w;
        auto bu = resolver.openBaseBundle(ab, &w);
        if (!bu) return -1;
        PPtr tp = bu->findAsset(name);
        if (tp.isNull()) tp = bu->findTexture(name);
        if (tp.isNull()) return -1;
        auto t = b.decode(*bu, tp, "globals");
        if (!t) return -1;
        std::vector<uint8_t> px = t->rgba;
        int idx = b.addSceneTexture(t->name, bu->name(), t->width, t->height, std::move(px));
        scene.textures[static_cast<size_t>(idx)].clamp = t->wrapMode == 1;
        return idx;
    };
    {
        SceneTexBinding ramp;
        ramp.name = "_RampG";
        ramp.texture = loadListTexture(432, 1);
        scene.globalTextures.push_back(ramp);
        int64_t gloss = card.hair.at("parts").at(size_t(0)).at("glossId").asInt();
        b.hairGlossTexture = loadListTexture(433, gloss);
    }
    card::MaterialEdits materialEdits = card::LoadMaterialEdits(card.extended, outfit);
    if (!materialEdits.empty()) b.edits = &materialEdits;

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
    Overrides headOv;
    if (opt.applyShape) {
        resolve::ItemRef hr;
        for (const auto& it : resolve::ItemsFromCard(card, outfit))
            if (it.group == "Head") hr = it;
        if (hr.group == "Head") {
            resolve::ResolvedItem hi = resolver.resolve(card, hr);
            if (hi.ok()) {
                std::string hwhy;
                auto hb = resolver.openBundle(hi, &hwhy);
                if (hb) headOv = headShapeOverrides(card, resolver, hi, *hb, *base, opt.headRigPrefab, headBase, scene);
            }
        }
    }
    if (!addRigPrefab(scene, rig, *base, opt.headRigPrefab, headBase, headAttach, "rig", headOv.empty() ? nullptr : &headOv))
        scene.warn("", "prefab '" + opt.headRigPrefab + "' not found in oo_base.unity3d");

    auto vec4Of = [](const Value& v) {
        return Vec4{static_cast<float>(v.at(size_t(0)).asFloat()), static_cast<float>(v.at(size_t(1)).asFloat()),
                    static_cast<float>(v.at(size_t(2)).asFloat()), static_cast<float>(v.size() > 3 ? v.at(size_t(3)).asFloat() : 1.0)};
    };
    // The texture a list row names for a card id (base or mod), decoded; null for "none" or when missing.
    auto rowTextureFor = [&](const std::string& partId, int category, int64_t id, const std::string& property, const char* texKey, const std::string& what,
                          std::string* keyOut = nullptr, const char* abKey = "MainAB") -> std::shared_ptr<unity::TextureData> {
        resolve::ItemRef fr;
        fr.partId = partId + "." + what;
        fr.group = "Face";
        fr.property = property;
        fr.category = category;
        fr.id = id;
        resolve::ResolvedItem fi = resolver.resolve(card, fr);
        if (fi.status == resolve::ItemStatus::Empty) return nullptr;
        if (!fi.ok()) {
            scene.warn(partId, what + ": " + (fi.detail.empty() ? "no texture item for this id" : fi.detail));
            return nullptr;
        }
        std::string texName = fi.listItem.get(texKey), ab = fi.listItem.get(abKey);
        if (texName.empty() || texName == "0" || ab.empty() || ab == "0") return nullptr;   // "none"
        std::string w;
        auto tb = resolver.openItemBundle(fi, ab, &w);
        if (!tb) { scene.warn(partId, what + ": texture bundle not loaded: " + w); return nullptr; }
        PPtr tp = tb->findAsset(texName);
        if (tp.isNull()) tp = tb->findTexture(texName);
        if (tp.isNull()) { scene.warn(partId, what + ": texture '" + texName + "' not found in " + ab); return nullptr; }
        if (keyOut) *keyOut = tb->name() + "#" + std::to_string(tp.pathId);
        return b.decode(*tb, tp, partId);
    };

    // A texture the game builds by drawing a quad through a "create" material of mm_base (CustomTextureCreate): queues the job
    // (run by export::RunCreateJobs at the end of assembly) and returns the placeholder texture that will hold the result.
    using TexInput = std::pair<std::string, std::shared_ptr<unity::TextureData>>;
    std::map<const unity::TextureData*, int> inputTexCache;
    auto createJobFor = [&](const std::string& partId, const char* matName, const std::vector<TexInput>& texs, const std::vector<std::pair<std::string, Vec4>>& props, int size) -> int {
        std::string w;
        auto mm = resolver.openBaseBundle("chara/mm_base.unity3d", &w);
        if (!mm) { scene.warn(partId, std::string("create material ") + matName + ": mm_base.unity3d not in the library (" + w + ")"); return -1; }
        PPtr mp = mm->findAsset(matName);
        if (mp.isNull()) { scene.warn(partId, std::string("create material ") + matName + " not found in mm_base.unity3d"); return -1; }
        std::vector<std::pair<std::string, Vec4>> savedRuntime = std::move(b.runtime);
        b.runtime.clear();
        RendererInfo none;
        b.materialFor(*mm, mp, partId, nullptr, nullptr, none, 0);
        b.runtime = std::move(savedRuntime);
        SceneCreateJob job;
        job.material = std::move(scene.materials.back());
        scene.materials.pop_back();
        for (const auto& [name, td] : texs) {
            int idx = -1;
            if (td) {
                auto it = inputTexCache.find(td.get());
                if (it == inputTexCache.end()) {
                    auto copy = td->rgba;
                    int made = b.addSceneTexture(td->name, mm->name(), td->width, td->height, std::move(copy));
                    scene.textures[static_cast<size_t>(made)].clamp = td->wrapMode == 1;   // gradation masks and ramps clamp
                    it = inputTexCache.emplace(td.get(), made).first;
                }
                idx = it->second;
            }
            bool set = false;
            for (auto& tb : job.material.texProps) if (tb.name == name) { tb.texture = idx; set = true; }
            if (!set) { SceneTexBinding nb; nb.name = name; nb.texture = idx; job.material.texProps.push_back(nb); }
        }
        for (const auto& [name, v] : props) {
            bool set = false;
            for (auto& pr : job.material.props) if (pr.first == name) { pr.second = v; set = true; }
            if (!set) job.material.props.emplace_back(name, v);
        }
        job.width = job.height = size;
        job.texture = b.addSceneTexture(std::string(matName) + "_result", std::string("create material ") + matName, 1, 1, std::vector<uint8_t>{255, 255, 255, 255});
        scene.createJobs.push_back(std::move(job));
        return scene.createJobs.back().texture;
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
            std::vector<std::pair<std::string, int>> bodyOverlayTex;
            std::vector<std::pair<std::string, Vec4>> bodyOverlayColors;
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
                        // The game draws the skin through cf_m_body_create (base, colour mask, two skin colours, paint 1 and 2 placed by
                        // their layout rows, sunburn, nail colour); nipples and underhair are overlays on the body material.
                        const Value& bd = card.body;
                        auto layoutRow = [&](int64_t id, const Value& l) {
                            Vec4 v = vec4Of(l), out{0, 0, 0, 0};
                            resolve::ItemRef fr;
                            fr.partId = "body.paintlayout";
                            fr.group = "Face";
                            fr.property = "ChaFileBody.paintLayoutId";
                            fr.category = 3;
                            fr.id = id;
                            resolve::ResolvedItem fi = resolver.resolve(card, fr);
                            if (!fi.ok()) return out;   // layout 0 / none: the paint has no placement and shows nothing
                            auto num = [&](const char* k) { return static_cast<float>(std::atof(fi.listItem.get(k).c_str())); };
                            auto lerp = [](float a, float bb, float t) { return a + (bb - a) * t; };
                            out.x = lerp(num("CenterX") + num("MoveX"), num("CenterX") - num("MoveX"), v.x);
                            out.y = lerp(num("CenterY") + num("MoveY"), num("CenterY") - num("MoveY"), v.y);
                            out.z = lerp(-1.0f, 1.0f, v.z);
                            out.w = lerp(num("CenterScale") - num("AddScale"), num("CenterScale") + num("AddScale"), v.w);
                            return out;
                        };
                        std::vector<TexInput> texs = {{"_MainTex", b.decode(*base, bt, "body")}, {"_ColorMask", b.decode(*base, bm, "body")}};
                        std::vector<std::pair<std::string, Vec4>> props = {{"_Color", vec4Of(sk)}, {"_Color2", c[1]}, {"_Color4", vec4Of(bd.at("sunburnColor"))}, {"_Color5", vec4Of(bd.at("nailColor"))}};
                        const char* paintTex[2] = {"_Texture3", "_Texture6"};
                        const char* paintCol[2] = {"_Color3", "_Color6"};
                        const char* paintVec[2] = {"_paint1", "_paint2"};
                        for (size_t k = 0; k < 2; ++k) {
                            texs.push_back({paintTex[k], rowTextureFor("body", 421, bd.at("paintId").at(k).asInt(), "ChaFileBody.paintId", "PaintTex", "paint" + std::to_string(k + 1))});
                            props.emplace_back(paintCol[k], vec4Of(bd.at("paintColor").at(k)));
                            props.emplace_back(paintVec[k], layoutRow(bd.at("paintLayoutId").at(k).asInt(), bd.at("paintLayout").at(k)));
                        }
                        texs.push_back({"_Texture4", rowTextureFor("body", 422, bd.at("sunburnId").asInt(), "ChaFileBody.sunburnId", "SunburnTex", "sunburn")});
                        int job = createJobFor("body", "cf_m_body_create", texs, props, 1024);
                        if (job >= 0) {
                            bodyLook.tex1 = job;
                            card::MaterialEdits::Image ov;
                            if (b.edits && b.edits->decodeSkinOverlay(card::MaterialEdits::OverlayBody, ov)) {
                                scene.createJobs.back().overlay = ov.rgba;
                                scene.createJobs.back().overlayWidth = ov.width;
                                scene.createJobs.back().overlayHeight = ov.height;
                            }
                            auto addOverlay = [&](const char* texName, const char* colName, int category, int64_t id, const char* prop, const char* key, const char* what, Vec4 col) {
                                auto t = rowTextureFor("body", category, id, prop, key, what);
                                int idx = -1;
                                if (t) { auto copy = t->rgba; idx = b.addSceneTexture(t->name, "body overlay", t->width, t->height, std::move(copy)); }
                                bodyOverlayTex.emplace_back(texName, idx);
                                bodyOverlayColors.emplace_back(colName, col);
                            };
                            addOverlay("_overtex1", "_overcolor1", 423, bd.at("nipId").asInt(), "ChaFileBody.nipId", "NipTex", "nipple", vec4Of(bd.at("nipColor")));
                            addOverlay("_overtex2", "_overcolor2", 424, bd.at("underhairId").asInt(), "ChaFileBody.underhairId", "UnderhairTex", "underhair", vec4Of(bd.at("underhairColor")));
                            bodyOverlayColors.emplace_back("_nipsize", Vec4{static_cast<float>(bd.at("areolaSize").asFloat()), 0, 0, 0});
                        } else {
                            bodyLook.tex1 = b.bake(b.decode(*base, bt, "body"), "body_t", b.decode(*base, bm, "body"), "body_mc", c, base->name());
                            card::MaterialEdits::Image ov;
                            if (bodyLook.tex1 >= 0 && b.edits && b.edits->decodeSkinOverlay(card::MaterialEdits::OverlayBody, ov)) {
                                auto& st = scene.textures[static_cast<size_t>(bodyLook.tex1)];
                                alphaOver(st.rgba, st.width, st.height, ov);
                                st.pixelHash = Fnv1a64(st.rgba.data(), st.rgba.size());
                            }
                        }
                        bodyLook.use1.insert(r.componentId);
                        bodyLookPtr = &bodyLook;
                    }
                }
                b.extraTex = bodyOverlayTex;
                b.runtime = bodyOverlayColors;
                int mi = b.addRenderer(*base, r, roots[0], partIdx, "body", Mat4{}, 0, &skin, false, bodyLookPtr);
                b.extraTex.clear();
                b.runtime.clear();
                if (mi >= 0) part.meshes.push_back(mi);
            }
            if (scene.createJobs.empty())   // the body's create material did not run
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
    std::vector<std::shared_ptr<unity::TextureData>> bodyMasks, braMasks;
    for (size_t wi = 0; wi < work.size(); ++wi) {
        const resolve::ItemRef ref = work[wi];
        resolve::ResolvedItem item = resolver.resolve(card, ref);
        if (item.status == resolve::ItemStatus::Empty) continue;
        if (const char* sp = std::getenv("CARDBRIDGE_SKIP_PART")) {   // debug aid: leave whole parts out
            if ((std::string(",") + sp + ",").find("," + ref.partId + ",") != std::string::npos) continue;
        }

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
        bool garmentAsAccessory = false;
        if (accessoryMode) {
            // A garment prefab (cf_o_root with the body's own bone names) worn through an accessory slot is bound to the
            // character's skeleton by bone name like any garment; it does not hang on the slot's attach node, whose offset
            // would lift a vest authored in body space by the node's height.
            for (int64_t xid : uf->subtree(roots[0]))
                if (const unity::Xform* x = uf->xform(xid); x && x->name == "cf_o_root") {
                    accessoryMode = false;
                    garmentAsAccessory = true;
                    placement = Mat4();
                    staticBone = 0;
                    break;
                }
        }

        // ---- colouring: what the game does at run time with the card's colours ----
        ItemLook look;
        bool haveLook = false;
        if ((ref.group == "Outfit" || garmentAsAccessory) && ref.outfit >= 0) {
            Vec4 c[3];
            if (garmentAsAccessory) {   // a garment in an accessory slot is coloured by that slot's three colours
                const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(std::max(0, ref.accIndex))).at("color");
                for (size_t k = 0; k < 3; ++k) c[k] = kkColor(vec4Of(col.at(k)));
            } else {
                // Sub-parts use the top's colours; part C's first colour is the fourth slot (ChaControl.ChangeCustomClothes).
                size_t colorSlot = ref.subKind >= 0 ? 0 : static_cast<size_t>(std::max(0, ref.slot));
                const Value& ci = card.outfits[static_cast<size_t>(ref.outfit)].clothes.at("parts").at(colorSlot).at("colorInfo");
                for (size_t k = 0; k < 3; ++k) c[k] = kkColor(vec4Of(ci.at(k).at("baseColor")));
                if (ref.subKind == 2) c[0] = kkColor(vec4Of(ci.at(size_t(3)).at("baseColor")));
            }

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
                        const Value& fv = mb.fields.at(field);
                        if (fv.isMap()) {   // a single renderer reference (rendAccessory)
                            if (fv.at("m_FileID").asInt() == 0 && fv.at("m_PathID").asInt() != 0) into.insert(fv.at("m_PathID").asInt());
                            return;
                        }
                        for (const auto& pp : fv.asArray())
                            if (pp.at("m_FileID").asInt() == 0 && pp.at("m_PathID").asInt() != 0) into.insert(pp.at("m_PathID").asInt());
                    };
                    collect("rendNormal01", look.use1);
                    collect("rendAlpha01", look.use1);
                    collect("rendNormal02", look.use2);
                    collect("rendAlpha02", look.use2);
                    collect("rendAccessory", look.use1);
                }
            }
        } else if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("color");
            if (col.size() >= 3) {
                for (int k = 0; k < 3; ++k) look.colors[k] = kkColor(vec4Of(col.at(static_cast<size_t>(k))));
                look.materialTint = true;
                haveLook = true;
            }
        }

        // Face parts: the game builds each one's texture from the list row the card's face ids select,
        // tinted with the card's colours (ChaControl.ChangeSettingEyebrow / Eyeline / Eye ...).
        const Value& faceV = card.face;

        auto rowTexture = [&](int category, int64_t id, const std::string& property, const char* texKey, const std::string& what,
                              std::string* keyOut = nullptr, const char* abKey = "MainAB") {
            return rowTextureFor(ref.partId, category, id, property, texKey, what, keyOut, abKey);
        };

        auto createJob = [&](const char* matName, const std::vector<TexInput>& texs, const std::vector<std::pair<std::string, Vec4>>& props, int size) {
            return createJobFor(ref.partId, matName, texs, props, size);
        };

        // Face parts: the game builds each one's texture from the list row the card's face ids select,
        // tinted with the card's colours (ChaControl.ChangeSettingEyebrow / Eyeline / Eye ...).
        auto faceTextureFor = [&](const std::string& rendererName) -> int {
            struct Spec { const char* renderer; int category; int64_t id; const char* property; const char* texKey; Vec4 color; };
            auto colorOf = [&](const char* field) { return kkColor(vec4Of(faceV.at(field))); };
            std::vector<Spec> specs;
            specs.push_back({"cf_O_mayuge", 406, faceV.at("eyebrowId").asInt(), "ChaFileFace.eyebrowId", "EyebrowTex", colorOf("eyebrowColor")});
            specs.push_back({"cf_O_eyeline", 412, faceV.at("eyelineUpId").asInt(), "ChaFileFace.eyelineUpId", "EyelineUpTex", colorOf("eyelineColor")});
            specs.push_back({"cf_O_eyeline_low", 413, faceV.at("eyelineDownId").asInt(), "ChaFileFace.eyelineDownId", "EyelineDownTex", colorOf("eyelineColor")});
            specs.push_back({"cf_O_noseline", 414, faceV.at("noseId").asInt(), "ChaFileFace.noseId", "NoseTex", Vec4{0.25f, 0.2f, 0.2f, 1.0f}});
            specs.push_back({"cf_Ohitomi_L", 407, faceV.at("whiteId").asInt(), "ChaFileFace.whiteId", "EyeWhiteTex", colorOf("whiteBaseColor")});
            specs.push_back({"cf_Ohitomi_R", 407, faceV.at("whiteId").asInt(), "ChaFileFace.whiteId", "EyeWhiteTex", colorOf("whiteBaseColor")});
            const Value& pupils = faceV.at("pupil");
            specs.push_back({"cf_Ohitomi_L02", 408, pupils.at(size_t(0)).at("id").asInt(), "ChaFileFace.Pupil1", "EyeTex", kkColor(vec4Of(pupils.at(size_t(0)).at("baseColor")))});
            specs.push_back({"cf_Ohitomi_R02", 408, pupils.at(size_t(1)).at("id").asInt(), "ChaFileFace.Pupil2", "EyeTex", kkColor(vec4Of(pupils.at(size_t(1)).at("baseColor")))});
            for (const Spec& sp : specs) {
                if (rendererName != sp.renderer) continue;
                std::string key;
                auto tex = rowTexture(sp.category, sp.id, sp.property, sp.texKey, sp.renderer, &key);
                Vec4 c[3] = {sp.color, Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}};
                // KoiSkinOverlayX paints the iris over the eye part (cards that use it often pair it with a "no iris" item).
                int overlayType = rendererName == "cf_Ohitomi_L02" ? card::MaterialEdits::OverlayEyeL
                                  : rendererName == "cf_Ohitomi_R02" ? card::MaterialEdits::OverlayEyeR : -1;
                card::MaterialEdits::Image ov;
                if (overlayType >= 0 && b.edits && b.edits->decodeSkinOverlay(overlayType, ov)) {
                    unity::TextureData out;
                    if (tex) { out = *tex; out.rgba = tintTexture(*tex, nullptr, c); }
                    else { out.width = ov.width; out.height = ov.height; out.rgba.assign(static_cast<size_t>(ov.width) * ov.height * 4, 0); }
                    out.name = std::string(sp.renderer) + "_iris";
                    alphaOver(out.rgba, out.width, out.height, ov);
                    return b.addSceneTexture(out.name, "card (KoiSkinOverlayX)", out.width, out.height, std::move(out.rgba));
                }
                if (!tex) return -1;
                // The iris and the white of the eye are drawn through the game's own create shaders (iris: gradient between the two
                // colours by the gradation mask; white: base and sub colour).
                const bool isWhite = rendererName == "cf_Ohitomi_L" || rendererName == "cf_Ohitomi_R";
                const bool isIris = rendererName == "cf_Ohitomi_L02" || rendererName == "cf_Ohitomi_R02";
                if (isWhite) {
                    int job = createJob("cf_m_eyewhite_create", {{"_MainTex", tex}}, {{"_Color", colorOf("whiteBaseColor")}, {"_Color2", colorOf("whiteSubColor")}}, 512);
                    if (job >= 0) return job;
                } else if (isIris) {
                    const Value& pu = pupils.at(rendererName == "cf_Ohitomi_L02" ? size_t(0) : size_t(1));
                    auto grad = rowTexture(409, pu.at("gradMaskId").asInt(), "ChaFileFace.PupilGrad", "ColorMaskTex", std::string(sp.renderer) + ".gradation", nullptr, "ColorMaskAB");
                    const float gy = static_cast<float>(pu.at("gradOffsetY").asFloat()), gs = static_cast<float>(pu.at("gradScale").asFloat());
                    int job = createJob("cf_m_eye_create", {{"_MainTex", tex}, {"_ColorMask", grad}},
                                        {{"_Color", kkColor(vec4Of(pu.at("baseColor")))}, {"_Color2", kkColor(vec4Of(pu.at("subColor")))},
                                         {"_Blend", Vec4{static_cast<float>(pu.at("gradBlend").asFloat()), 0, 0, 0}},
                                         {"_grad", Vec4{0.0f, -0.5f + gy, 0.0f, -1.0f + 2.0f * gs}}}, 512);
                    if (job >= 0) return job;
                }
                return b.bake(tex, key, nullptr, "-", c, uf->name());
            }
            return -1;
        };

        // The face skin: the head's own base texture tinted by its colour mask with the card's two skin colours,
        // then the makeup layers (cheek, lip line, lips, eye shadow) blended on top in the same UV layout
        // (ChaControl.ChangeSettingFaceBaseTex/Color and the overlay textures of the face material).
        std::vector<std::pair<std::string, int>> faceOverlayTex;        // lips, eye shadow: bound on the face material
        std::vector<std::pair<std::string, Vec4>> faceOverlayColors;
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
            const auto& oc = card.outfits[static_cast<size_t>(std::max(0, ref.outfit < 0 ? 0 : ref.outfit))];
            bool coordMakeup = oc.enableMakeup;
            const Value& mk = coordMakeup ? oc.makeup : faceV.at("baseMakeup");
            std::string pre = coordMakeup ? "outfit" + std::to_string(ref.outfit < 0 ? 0 : ref.outfit) + "." : "";

            // The game draws the skin through its cf_m_face_create material (base, colour mask, two skin colours, paint 1 and 2,
            // cheek, lip line, mole); lips and eye shadow are overlays on the face material itself (_overtex1 / _overtex3).
            {
                const std::string which = ref.partId + ".face";
                auto layout = [&](const Value& l, bool mole) {
                    auto lerp = [](float a, float bb, float t) { return a + (bb - a) * t; };
                    Vec4 v = vec4Of(l);
                    return mole ? Vec4{lerp(0.25f, -0.25f, v.x), lerp(0.3f, -0.3f, v.y), 0.0f, lerp(0.0f, 0.7f, v.w)}
                                : Vec4{lerp(0.25f, -0.25f, v.x), lerp(0.3f, -0.3f, v.y), lerp(1.0f, -1.0f, v.z), lerp(-8.0f, 0.7f, v.w)};
                };
                std::vector<TexInput> texs = {{"_MainTex", base}, {"_ColorMask", mask}};
                std::vector<std::pair<std::string, Vec4>> props = {{"_Color", skinColor}, {"_Color2", vec4Of(card.body.at("skinSubColor"))}};
                const char* paintTex[2] = {"_Texture3", "_Texture7"};
                const char* paintCol[2] = {"_Color3", "_Color7"};
                const char* paintVec[2] = {"_paint1", "_paint2"};
                for (size_t k = 0; k < 2; ++k) {
                    texs.push_back({paintTex[k], rowTexture(405, mk.at("paintId").at(k).asInt(), pre + "ChaFileMakeup.paintId", "PaintTex", "paint" + std::to_string(k + 1))});
                    props.emplace_back(paintCol[k], vec4Of(mk.at("paintColor").at(k)));
                    props.emplace_back(paintVec[k], layout(mk.at("paintLayout").at(k), false));
                }
                texs.push_back({"_Texture4", rowTexture(402, mk.at("cheekId").asInt(), pre + "ChaFileMakeup.cheekId", "CheekTex", "cheek")});
                props.emplace_back("_Color4", vec4Of(mk.at("cheekColor")));
                texs.push_back({"_Texture5", rowTexture(404, faceV.at("lipLineId").asInt(), "ChaFileFace.lipLineId", "LiplineTex", "lipline")});
                props.emplace_back("_Color5", vec4Of(faceV.at("lipLineColor")));
                texs.push_back({"_Texture6", rowTexture(415, faceV.at("moleId").asInt(), "ChaFileFace.moleId", "MoleTex", "mole")});
                props.emplace_back("_Color6", vec4Of(faceV.at("moleColor")));
                props.emplace_back("_hokuro", layout(faceV.at("moleLayout"), true));
                int job = createJobFor(which, "cf_m_face_create", texs, props, 1024);
                if (job >= 0) {
                    card::MaterialEdits::Image fov;
                    if (b.edits && b.edits->decodeSkinOverlay(card::MaterialEdits::OverlayFace, fov)) {
                        scene.createJobs.back().overlay = fov.rgba;
                        scene.createJobs.back().overlayWidth = fov.width;
                        scene.createJobs.back().overlayHeight = fov.height;
                    }
                    faceOverlayTex.clear();
                    faceOverlayColors.clear();
                    auto addOverlay = [&](const char* texName, const char* colName, int category, int64_t id, const std::string& prop, const char* key, const char* what, Vec4 col) {
                        auto t = rowTexture(category, id, prop, key, what);
                        int idx = -1;   // "none": the game clears the slot
                        if (t) { auto copy = t->rgba; idx = b.addSceneTexture(t->name, "face overlay", t->width, t->height, std::move(copy)); }
                        faceOverlayTex.emplace_back(texName, idx);
                        faceOverlayColors.emplace_back(colName, col);
                    };
                    addOverlay("_overtex1", "_overcolor1", 403, mk.at("lipId").asInt(), pre + "ChaFileMakeup.lipId", "LipTex", "lip", vec4Of(mk.at("lipColor")));
                    addOverlay("_overtex3", "_overcolor3", 401, mk.at("eyeshadowId").asInt(), pre + "ChaFileMakeup.eyeshadowId", "EyeshadowTex", "eyeshadow", vec4Of(mk.at("eyeshadowColor")));
                    return job;
                }
            }

            Vec4 c[3] = {skinColor, vec4Of(card.body.at("skinSubColor")), Vec4{1, 1, 1, 1}};
            unity::TextureData out = *base;
            out.name = base->name + "_skin";
            out.rgba = tintTexture(*base, mask.get(), c);
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
            card::MaterialEdits::Image fov;
            if (b.edits && b.edits->decodeSkinOverlay(card::MaterialEdits::OverlayFace, fov)) alphaOver(out.rgba, out.width, out.height, fov);
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
        // Hair length: a hair prefab's "_s" bones sit at baseLength + Lerp(0, addLength, lengthRate) along y (ChaCustomHairComponent.Update).
        // The rate is the hair part's length slider for a hair item and, for a hair accessory, the HairAccessoryCustomizer slot setting.
        if (accessoryMode || ref.group == "Hair") {
            for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
                if (mb.className != "ChaCustomHairComponent") continue;
                double rate = mb.fields.at("lengthRate").asFloat();
                if (ref.group == "Hair") {
                    size_t kind = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
                    rate = card.hair.at("parts").at(kind).at("length").asFloat(rate);
                } else if (ref.outfit >= 0 && ref.accIndex >= 0) {
                    const Value& payload = card.extended.at("com.deathweasel.bepinex.hairaccessorycustomizer").at(size_t(1)).at("HairAccessories");
                    if (payload.isBin()) {
                        Value all = MsgPackParse(payload.asBin());
                        const Value& info = all.atInt(ref.outfit).atInt(ref.accIndex);
                        if (info.isMap()) rate = info.at("HairLength").asFloat(rate);
                    }
                }
                const double add = mb.fields.at("addLength").asFloat();
                if (std::getenv("CARDBRIDGE_DEBUG_HAIRLEN")) std::fprintf(stderr, "hairlen %s rate %g add %g trf %zu base %zu\n", ref.partId.c_str(), rate, add, mb.fields.at("trfLength").size(), mb.fields.at("baseLength").size());
                const auto& trf = mb.fields.at("trfLength").asArray();
                const auto& baseLen = mb.fields.at("baseLength").asArray();
                for (size_t i = 0; i < trf.size() && i < baseLen.size(); ++i) {
                    int64_t id = trf[i].at("m_PathID").asInt();
                    const unity::Xform* x = id ? uf->xform(id) : nullptr;
                    if (!x) continue;
                    Vec3 p = x->pos;
                    p.y = static_cast<float>(baseLen[i].asFloat() + add * rate);
                    moveOv[id] = Mat4::TRS(p, x->rot, x->scl);
                }
            }
        }
        b.localOv = moveOv.empty() ? nullptr : &moveOv;

        // The garment's body mask: the game hides the body under clothing with an alpha mask in body UV space.
        if (ref.group == "Outfit") {
            std::string mab = item.listItem.get("OverBodyMaskAB"), mname = item.listItem.get("OverBodyMask");
            if (!mname.empty() && mname != "0" && !mab.empty() && mab != "0") {
                std::string w;
                auto mb = resolver.openItemBundle(item, mab, &w);
                if (mb) {
                    PPtr mp = mb->findAsset(mname);
                    if (mp.isNull()) mp = mb->findTexture(mname);
                    if (!mp.isNull()) { if (auto mt = b.decode(*mb, mp, ref.partId)) bodyMasks.push_back(mt); }
                    else scene.warn(ref.partId, "body mask '" + mname + "' not found in " + mab);
                }
            }
            std::string bab = item.listItem.get("OverBraMaskAB"), bname = item.listItem.get("OverBraMask");
            if (!bname.empty() && bname != "0" && !bab.empty() && bab != "0") {
                std::string w;
                auto bb = resolver.openItemBundle(item, bab, &w);
                if (bb) {
                    PPtr bp = bb->findAsset(bname);
                    if (bp.isNull()) bp = bb->findTexture(bname);
                    if (!bp.isNull()) { if (auto bt = b.decode(*bb, bp, ref.partId)) braMasks.push_back(bt); }
                }
            }
        }

        // The game always plays the face's blend shapes (FaceBlendShape on the head prefab): eyes open, mouth closed, the
        // eyebrow in its pattern. The sculpted mesh alone is a different face; every Head renderer a control targets gets the
        // resting-state weights (FBSBase.CalculateBlendShape at blink open-rate 1 and voice 0, no gaze offset).
        std::map<int64_t, std::vector<std::pair<int, float>>> blendWeights;
        b.blend = nullptr;
        if (ref.group == "Head") {
            std::map<int64_t, std::map<int, float>> sum;
            for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
                if (mb.className != "FaceBlendShape") continue;
                auto status = [&](const char* key, double def) { return card.status.at(key).asFloat(def); };
                const float lookUp = static_cast<float>(mb.fields.at("EyeLookUpCorrect").asFloat(0.1));
                struct Ctrl { const char* field; int ptn; float openRate, openMax; };
                const Ctrl ctrls[3] = {
                    {"EyebrowCtrl", static_cast<int>(status("eyebrowPtn", 0)), 1.0f, std::min(1.0f, std::max(0.0f, static_cast<float>(status("eyebrowOpenMax", 1)))) },
                    {"EyesCtrl", static_cast<int>(status("eyesPtn", 0)), 1.0f, std::min(0.92f, std::max(0.0f, static_cast<float>(status("eyesOpenMax", 1)))) },
                    {"MouthCtrl", static_cast<int>(status("mouthPtn", 0)), 0.0f, std::min(1.0f, std::max(0.0f, static_cast<float>(status("mouthOpenMax", 1)))) }};
                for (const Ctrl& c : ctrls) {
                    const Value& ctl = mb.fields.at(c.field);
                    float openMax = c.openMax;
                    if (std::string(c.field) == "EyesCtrl") openMax = std::min(openMax, 1.0f - lookUp);   // EyeLookController.SetCorrectOpenMax, gaze straight ahead
                    float num = static_cast<float>(ctl.at("OpenMin").asFloat(0.0)) + (openMax - static_cast<float>(ctl.at("OpenMin").asFloat(0.0))) * c.openRate;
                    int num3 = static_cast<int>(std::min(100.0f, std::max(0.0f, num * 100.0f)));
                    for (const auto& tg : ctl.at("FBSTarget").asArray()) {
                        int64_t go = tg.at("ObjTarget").at("m_PathID").asInt();
                        const auto& ptns = tg.at("PtnSet").asArray();
                        if (go == 0 || ptns.empty()) continue;
                        const Value& ps = ptns[static_cast<size_t>(c.ptn >= 0 && static_cast<size_t>(c.ptn) < ptns.size() ? c.ptn : 0)];
                        int close = static_cast<int>(ps.at("Close").asInt()), open = static_cast<int>(ps.at("Open").asInt());
                        if (close >= 0) sum[go][close] += static_cast<float>(100 - num3);
                        if (open >= 0) sum[go][open] += static_cast<float>(num3);
                    }
                }
            }
            for (const auto& [go, shapes] : sum)
                for (const auto& [idx, w] : shapes) {
                    blendWeights[go].emplace_back(idx, std::min(100.0f, w));
                    if (std::getenv("CARDBRIDGE_DEBUG_BLEND")) std::fprintf(stderr, "blend go %lld shape %d weight %g\n", static_cast<long long>(go), idx, w);
                }
            if (!blendWeights.empty()) b.blend = &blendWeights;
        }

        b.hairParent = nullptr;
        if (ref.group == "Hair") {
            auto hp = rig.world.find("cf_J_FaceUp_ty");
            if (hp != rig.world.end()) b.hairParent = &hp->second;
            else scene.warn(ref.partId, "hair parent node cf_J_FaceUp_ty not found; the hair is placed at the model origin");
        }
        b.meCoordinate = outfit;
        b.meType = -1;
        b.meSlot = 0;
        if (ref.group == "Outfit") { b.meType = card::MaterialEdits::Clothing; b.meSlot = ref.subKind >= 0 ? 0 : std::max(0, ref.slot); }
        else if (ref.group == "Accessories") { b.meType = card::MaterialEdits::Accessory; b.meSlot = std::max(0, ref.accIndex); }
        else if (ref.group == "Hair") {
            b.meType = card::MaterialEdits::Hair;
            b.meSlot = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
        } else if (ref.group == "Head") { b.meType = card::MaterialEdits::Character; }

        // Which renderers get which run-time colours (ChaAccessoryComponent: rendNormal/rendAlpha/rendHair; ChaCustomHairComponent: rendHair).
        std::set<int64_t> rtNormal, rtAlpha, rtHair, rtHairItem, rtHairAcs;
        bool useC[3] = {true, true, true};
        for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
            auto ids = [&](const char* field, std::set<int64_t>& into) {
                const Value& fv = mb.fields.at(field);
                if (fv.isMap()) { if (fv.at("m_FileID").asInt() == 0 && fv.at("m_PathID").asInt() != 0) into.insert(fv.at("m_PathID").asInt()); return; }
                for (const auto& pp : fv.asArray()) if (pp.at("m_FileID").asInt() == 0 && pp.at("m_PathID").asInt() != 0) into.insert(pp.at("m_PathID").asInt());
            };
            if (std::getenv("CARDBRIDGE_DEBUG_MB")) std::fprintf(stderr, "mb %s on %s\n", mb.className.c_str(), ref.partId.c_str());
            if (mb.className == "ChaAccessoryComponent") {
                ids("rendNormal", rtNormal); ids("rendAlpha", rtAlpha); ids("rendHair", rtHair);
                useC[0] = mb.fields.at("useColor01").asInt() != 0;
                useC[1] = mb.fields.at("useColor02").asInt() != 0;
                useC[2] = mb.fields.at("useColor03").asInt() != 0;
            } else if (mb.className == "ChaCustomHairComponent") {
                ids("rendHair", rtHairItem);
                ids("rendAccessory", rtHairAcs);
            }
        }
        Vec4 hairColor{1, 1, 1, 1};
        Value hacInfo;   // HairAccessoryCustomizer data of this accessory slot (a map when the card has it)
        if (accessoryMode && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& payload = card.extended.at("com.deathweasel.bepinex.hairaccessorycustomizer").at(size_t(1)).at("HairAccessories");
            if (payload.isBin()) hacInfo = MsgPackParse(payload.asBin()).atInt(ref.outfit).atInt(ref.accIndex);
        }
        std::vector<std::string> skipped, skippedByEditor;
        size_t before = scene.meshes.size();
        for (const auto& r : uf->renderersUnder(roots[0])) {
            if (std::getenv("CARDBRIDGE_DEBUG_PATHS"))
                std::fprintf(stderr, "renderer %s: %s enabled=%d active=%d\n", ref.partId.c_str(), uf->pathOf(r.xformId).c_str(), r.enabled ? 1 : 0, r.active ? 1 : 0);
            if (!r.enabled) continue;
            if (b.edits && b.meType >= 0) {   // MaterialEditor can switch a renderer off
                bool off = false;
                for (const auto* e : b.edits->editsFor(b.edits->rendererEnabled, b.meCoordinate, b.meType, b.meSlot, r.name))
                    if (e->property == "0" && e->value[0] == 0.0f) off = true;
                if (off) { skippedByEditor.push_back(r.name); continue; }
            }
            if (onlyHelperMaterials(*uf, r)) continue;
            std::string path = "/" + uf->pathOf(r.xformId) + "/";
            // Variants the game keeps switched off in the default state: the pulled-off ("nuge") top and
            // bottom, and the face's tears, gag eyes and tongue.
            // A garment prefab holds one node per clothes state: n_top_a / n_bot_a is the worn state, b and c the pulled-off
            // ones (c = shorts or skirt hanging from one leg); an object inactive in the prefab is off in every state.
            bool hiddenByDefault = !r.active || hiddenClothesState(path) ||
                                   (ref.group == "Head" && (contains(r.name, "namida") || contains(r.name, "gag_eye") || contains(r.name, "o_tang")));
            if (hiddenByDefault) { skipped.push_back(r.name); continue; }
            // Debug aid: CARDBRIDGE_SKIP=name1,name2 leaves those renderers out (to find which mesh draws an artefact).
            if (const char* sk = std::getenv("CARDBRIDGE_SKIP")) {
                std::string list = std::string(",") + sk + ",";
                if (list.find("," + r.name + ",") != std::string::npos) { skipped.push_back(r.name + " (CARDBRIDGE_SKIP)"); continue; }
            }
            const Vec4* fb = (ref.group == "Head" && contains(r.name, "face")) ? &skinColor : nullptr;
            if (ref.group == "Hair") {   // hair items: the card's hair colour (shading and root-to-tip gradient are not modelled)
                size_t kind = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
                const Value& hp = card.hair.at("parts").at(kind);
                if (hp.at("baseColor").size() >= 3) { hairColor = kkColor(vec4Of(hp.at("baseColor"))); fb = &hairColor; }
            }
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
            b.runtime.clear();
            b.extraTex.clear();
            if (ref.group == "Head" && contains(r.name, "cf_O_face") && !faceOverlayTex.empty()) {
                b.extraTex = faceOverlayTex;
                b.runtime = faceOverlayColors;
            }
            // Iris renderers: highlights (_overtex1 up, _overtex2 down), their colours, the tilt, and the iris / highlight placement the
            // EyeLookMaterialControll computes from the card's pupil sliders with the gaze straight ahead.
            b.stOverride.clear();
            if (ref.group == "Head" && (r.name == "cf_Ohitomi_L02" || r.name == "cf_Ohitomi_R02")) {
                const bool left = r.name == "cf_Ohitomi_L02";
                auto lerp = [](float a, float bb, float t) { return a + (bb - a) * t; };
                auto F = [&](const char* k, double def) { return static_cast<float>(faceV.at(k).asFloat(def)); };
                const Value* ctl = nullptr;
                std::vector<unity::MonoInfo> mbs = uf->monoBehavioursUnder(roots[0]);
                for (const auto& mb : mbs) if (mb.className == "EyeLookMaterialControll" && mb.goId == r.goId) { ctl = &mb.fields; break; }
                int hlUp = -1, hlDown = -1;
                if (auto t = rowTexture(410, faceV.at("hlUpId").asInt(), "ChaFileFace.hlUpId", "EyeHiUpTex", "highlight up")) { auto c2 = t->rgba; hlUp = b.addSceneTexture(t->name, "eye highlight", t->width, t->height, std::move(c2)); }
                if (auto t = rowTexture(411, faceV.at("hlDownId").asInt(), "ChaFileFace.hlDownId", "EyeHiDownTex", "highlight down")) { auto c2 = t->rgba; hlDown = b.addSceneTexture(t->name, "eye highlight", t->width, t->height, std::move(c2)); }
                b.extraTex = {{"_overtex1", hlUp}, {"_overtex2", hlDown}};
                b.runtime = {{"_overcolor1", vec4Of(faceV.at("hlUpColor"))}, {"_overcolor2", vec4Of(faceV.at("hlDownColor"))},
                             {"_rotation", Vec4{(left ? 1.0f : -1.0f) * lerp(0.02f, -0.02f, static_cast<float>(faceV.at("shapeValueFace").at(size_t(33)).asFloat(0.5))), 0, 0, 0}}};
                if (ctl) {
                    float offX = lerp(0.2f, -0.6f, F("pupilX", 0.5)) * (left ? 1.0f : -1.0f), offY = lerp(-0.5f, 0.5f, F("pupilY", 0.5));
                    float sx = lerp(1.8f, -0.2f, F("pupilWidth", 0.9)), sy = lerp(1.8f, -0.2f, F("pupilHeight", 0.9));
                    float hx = offX, hy = offY, mag = std::sqrt(hx * hx + hy * hy);
                    if (mag > 1.0f) { hx /= mag; hy /= mag; }
                    auto invLerp = [](float v) { return (v + 1.0f) * 0.5f; };
                    float inW = static_cast<float>(ctl->at("InsideWait").asFloat()), outW = static_cast<float>(ctl->at("OutsideWait").asFloat());
                    float upW = static_cast<float>(ctl->at("UpWait").asFloat()), dnW = static_cast<float>(ctl->at("DownWait").asFloat());
                    float power = static_cast<float>(ctl->at("power").asFloat(0.001));
                    float num = lerp(inW, outW, invLerp(hx)), num2 = lerp(dnW, upW, invLerp(hy));
                    float num3 = lerp(1.0f, 5.0f, sx), num4 = lerp(1.0f, 5.0f, sy);
                    auto clampf = [](float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); };
                    float vx = clampf(num * (power * num3), static_cast<float>(ctl->at("InsideLimit").asFloat()), static_cast<float>(ctl->at("OutsideLimit").asFloat()));
                    float vy = clampf(num2 * (power * num4), static_cast<float>(ctl->at("UpLimit").asFloat()), static_cast<float>(ctl->at("DownLimit").asFloat()));
                    float hlUpY = lerp(0.1f, -0.1f, F("hlUpY", 0.5)), hlDownY = lerp(0.1f, -0.1f, F("hlDownY", 0.5));
                    b.stOverride = {{"_MainTex", Vec4{1 + sx, 1 + sy, vx - sx * 0.5f, vy - sy * 0.5f}},
                                    {"_overtex1", Vec4{1 + sx, 1 + sy, vx - sx * 0.5f, vy + hlUpY - sy * 0.5f}},
                                    {"_overtex2", Vec4{1 + sx, 1 + sy, vx - sx * 0.5f, vy + hlDownY - sy * 0.5f}}};
                    if (std::getenv("CARDBRIDGE_DEBUG_EYE")) std::fprintf(stderr, "eye %s main_st %g %g %g %g up_st y %g down_st y %g\n", r.name.c_str(), 1 + sx, 1 + sy, vx - sx * 0.5f, vy - sy * 0.5f, vy + hlUpY - sy * 0.5f, vy + hlDownY - sy * 0.5f);
                }
            }
            // The face material's _overcolor2 alpha is the blush of the character's emotion state: 0 at rate 0, 0.2 at rate 1.
            b.blushAlpha = (ref.group == "Head" && contains(r.name, "cf_O_face"))
                               ? 0.2f * std::min(1.0f, std::max(0.0f, static_cast<float>(card.status.at("hohoAkaRate").asFloat(0.0))))
                               : -1.0f;
            // Hair accessories: the HairAccessoryCustomizer slot setting decides gloss and colours (ColorMatch = the character's hair colours); a slot
            // without that data keeps the prefab's own.
            b.glossForThisRenderer = rtHairItem.count(r.componentId) > 0 && (ref.group == "Hair" || (hacInfo.isMap() && hacInfo.at("HairGloss").asBool()));
            auto rawColor = [&](const Value& c) { Vec4 v = vec4Of(c); if (v.w < 1.0f) v.w = 1.0f; return v; };
            if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0) {
                const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("color");
                if (rtNormal.count(r.componentId)) {
                    if (useC[0]) b.runtime.emplace_back("_Color", vec4Of(col.at(size_t(0))));
                    if (useC[1]) b.runtime.emplace_back("_Color2", vec4Of(col.at(size_t(1))));
                    if (useC[2]) b.runtime.emplace_back("_Color3", vec4Of(col.at(size_t(2))));
                }
                if (rtAlpha.count(r.componentId)) b.runtime.emplace_back("_Color4", vec4Of(col.at(size_t(3))));
                if (hacInfo.isMap()) {
                    const Value& hp0 = card.hair.at("parts").at(size_t(0));
                    if (rtHairItem.count(r.componentId)) {
                        if (hacInfo.at("ColorMatch").asBool()) {
                            b.runtime.emplace_back("_Color", rawColor(hp0.at("baseColor")));
                            b.runtime.emplace_back("_Color2", rawColor(hp0.at("startColor")));
                            b.runtime.emplace_back("_Color3", rawColor(hp0.at("endColor")));
                            b.runtime.emplace_back("_LineColor", vec4Of(hp0.at("outlineColor")));
                        } else {
                            b.runtime.emplace_back("_LineColor", vec4Of(hacInfo.at("OutlineColor")));
                        }
                    }
                    if (rtHairAcs.count(r.componentId)) {
                        Vec4 ac = vec4Of(hacInfo.at("AccessoryColor"));
                        b.runtime.emplace_back("_Color", ac); b.runtime.emplace_back("_Color2", ac); b.runtime.emplace_back("_Color3", ac);
                    }
                }
            } else if (ref.group == "Hair" && rtHairItem.count(r.componentId)) {
                size_t kind = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
                const Value& hp = card.hair.at("parts").at(kind);
                b.runtime.emplace_back("_Color", rawColor(hp.at("baseColor")));
                b.runtime.emplace_back("_Color2", rawColor(hp.at("startColor")));
                b.runtime.emplace_back("_Color3", rawColor(hp.at("endColor")));
                b.runtime.emplace_back("_LineColor", vec4Of(hp.at("outlineColor")));
            }
            int mi = b.addRenderer(*uf, r, roots[0], partIdx, ref.partId, placement, staticBone, fb, accessoryMode, lookPtr);
            if (mi >= 0) part.meshes.push_back(mi);
        }
        if (!skippedByEditor.empty()) {
            std::string list;
            for (const auto& n : skippedByEditor) list += (list.empty() ? "" : ", ") + n;
            scene.warn(ref.partId, "left out, switched off in the card's MaterialEditor data: " + list);
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
        b.hairParent = nullptr;
        b.blushAlpha = -1;
        b.extraTex.clear();
        b.stOverride.clear();
        b.blend = nullptr;
        b.meType = -1;
        scene.parts.push_back(std::move(part));
    }

    // Hide the triangles the worn garments cover. Masks live in the covered mesh's own UV space (red channel 255 = keep):
    // the game masks the body this way, and the bra the same way (OverBraMask).
    auto applyMasks = [&](const std::string& what, const std::function<bool(const ScenePart&)>& pick,
                          const std::vector<std::shared_ptr<unity::TextureData>>& masks) {
        if (masks.empty()) return;
        size_t dropped = 0, total = 0;
        for (auto& part : scene.parts) {
            if (!pick(part)) continue;
            for (int mi : part.meshes) {
                SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
                for (auto& sub : m.submeshes) {
                    std::vector<uint32_t> keep;
                    for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                        ++total;
                        float u = 0, v = 0;
                        for (int k = 0; k < 3; ++k) { u += m.vertices[sub.indices[t + static_cast<size_t>(k)]].uv.x; v += m.vertices[sub.indices[t + static_cast<size_t>(k)]].uv.y; }
                        u /= 3; v /= 3;
                        u -= std::floor(u); v -= std::floor(v);
                        bool hidden = false;
                        for (const auto& mk : masks) {
                            int x = std::min(mk->width - 1, static_cast<int>(u * mk->width)), y = std::min(mk->height - 1, static_cast<int>((1.0f - v) * mk->height));
                            if (mk->rgba[(static_cast<size_t>(y) * mk->width + x) * 4] < 128) { hidden = true; break; }
                        }
                        if (hidden) { ++dropped; continue; }
                        keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                    }
                    sub.indices = std::move(keep);
                }
            }
        }
        scene.warn(what, what + " mask: " + std::to_string(dropped) + " of " + std::to_string(total) + " triangles hidden under clothing (as the game does)");
    };
    applyMasks("body", [](const ScenePart& p) { return p.id == "body"; }, bodyMasks);
    applyMasks("bra", [](const ScenePart& p) { return p.group == "Outfit" && p.id.size() >= 4 && p.id.compare(p.id.size() - 4, 4, ".bra") == 0; }, braMasks);

    // A top whose list row says NotBra hides the bra in the game (it would only show through the garment).
    if (topCoversBra)
        for (auto& p : scene.parts)
            if (p.group == "Outfit" && p.id.size() >= 4 && p.id.compare(p.id.size() - 4, 4, ".bra") == 0 && !p.meshes.empty()) {
                p.includeInExport = false;
                p.visibleInPreview = false;
                p.detail = "hidden: the top covers the bra (list NotBra=1); the game does not draw it either";
            }
    exporter::RunCreateJobs(scene);
    if (std::getenv("CARDBRIDGE_DEBUG_MAT"))
        for (const auto& m : scene.materials) {
            std::fprintf(stderr, "mat %s shader=%s idx=%d tex:", m.name.c_str(), m.shader.c_str(), m.shaderIndex);
            for (const auto& t : m.texProps) std::fprintf(stderr, " %s=%d", t.name.c_str(), t.texture);
            std::fprintf(stderr, " | props:");
            for (const auto& pr : m.props) if (pr.first.rfind("_Color", 0) == 0) std::fprintf(stderr, " %s=(%.2f %.2f %.2f %.2f)", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
            std::fprintf(stderr, "\n");
        }
    return scene;
}

} // namespace cardbridge::assemble
