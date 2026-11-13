#include "assemble/Assembler.h"
#include "assemble/AutoCloth.h"
#include "assemble/DynamicBone.h"
#include "assemble/HeelConfig.h"
#include "unity/TextureDecode.h"

#include "core/Bytes.h"
#include "shape/Shape.h"
#include "card/Abmx.h"
#include "card/MaterialEdits.h"
#include "material/MaterialWorkbench.h"
#include "core/MemoryProbe.h"
#include "core/MsgPack.h"
#include "core/Timing.h"
#include "export/Preview.h"
#include "export/SkinAoBake.h"
#include "export/SkinShade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <list>
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

// A card's hair tint multiplies a light, almost flat hair texture (the game's hair shader draws the strand pattern
// through _ColorMask). A base colour near black multiplies that texture to flat black and loses both the strands and
// the shade the card picture shows, so lift the tint to a visible brightness while keeping the hue of the card's own
// darkest hair colour (the start/end colours when the base carries none). The value is sRGB: the standard views
// linearise it (0.68 -> 0.42 linear) and encode the frame back, so they show ~0.61; the bake feeds it raw in its
// gamma space (see BakeShaderTextures), so the PMX texture shows the same ~0.61 instead of the linear 0.42 product,
// which crushed saturated hair colours to near black in MikuMikuDance.
// Only a near-black base is lifted: a card whose base is already visible must keep it raw, or _Color ends up brighter
// than the untouched _Color2/_Color3 and the _ColorMask's R regions fade against the G/B regions (Megumin's crown).
Vec4 hairTint(Vec4 base, Vec4 start, Vec4 end) {
    auto peakOf = [](const Vec4& c) { return std::max(c.x, std::max(c.y, c.z)); };
    Vec4 hue = base;
    float peak = peakOf(hue);
    if (peak >= 0.10f) return hue;   // visible card colour: keep it, so it agrees with _Color2/_Color3
    if (peak < 0.02f) { hue = end; peak = peakOf(hue); }
    if (peak < 0.02f) { hue = start; peak = peakOf(hue); }
    if (peak < 0.02f) { hue = {1, 1, 1, 1}; peak = 1; }
    const float target = std::getenv("CARDBRIDGE_HAIR_TINT") ? static_cast<float>(std::atof(std::getenv("CARDBRIDGE_HAIR_TINT"))) : 0.68f;   // test aid
    if (peak < target) {
        const float s = target / peak;
        hue.x *= s;
        hue.y *= s;
        hue.z *= s;
    }
    return {std::min(1.0f, hue.x), std::min(1.0f, hue.y), std::min(1.0f, hue.z), 1};
}

// A local transform the card's shape sliders replace on a named bone.
struct LocalOverride {
    Vec3 pos;
    Quat rot;
    bool hasRot = false;
    Vec3 scl{1, 1, 1};
};
using Overrides = std::map<std::string, LocalOverride>;

Quat quatMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// KKABMX on top of the game's shape: for every bone of the prefab the table names, the local scale, rotation and position the
// plugin writes (BoneModifier.Apply) from the bone's own values at that moment (the shape override when there is one).
void applyAbmx(Overrides& ov, UnityFile& uf, const std::string& prefabName, const card::AbmxTable& table, int coordinate) {
    if (table.empty()) return;
    const int64_t root = uf.findPrefabRoot(prefabName);
    if (!root) return;
    std::set<std::string> done;
    for (int64_t id : uf.subtree(root)) {
        const unity::Xform* x = uf.xform(id);
        if (!x || !done.insert(x->name).second) continue;
        const card::AbmxMod* m = table.find(x->name, coordinate);
        if (!m || m->empty()) continue;
        LocalOverride o;
        o.pos = x->pos; o.scl = x->scl; o.rot = x->rot;
        auto it = ov.find(x->name);
        if (it != ov.end()) o = it->second;
        Quat baseRot = it != ov.end() && it->second.hasRot ? it->second.rot : x->rot;
        Vec3 basePos = o.pos, baseScl = o.scl;
        if (m->hasScale()) o.scl = {baseScl.x * m->scale.x, baseScl.y * m->scale.y, baseScl.z * m->scale.z};
        if (m->hasRotation()) { o.rot = quatMul(baseRot, shape::EulerToQuat(m->rotation)); o.hasRot = true; }
        if (m->hasLength()) o.pos = basePos * m->length + m->position;
        else if (m->hasPosition()) o.pos = basePos + m->position;
        ov[x->name] = o;
    }
}

// The accessory attach nodes (a_n_*: hair pin, pony, twin tails, head top, earrings ...) of a character's head come from the head model's
// own prefab, which carries them placed for that head (the pony node of the default head points its ponytail down the back; the generic
// p_cf_head_bone only has placeholders). Local transforms by node name, for the nodes `ov` does not already replace.
void headAttachOverrides(Overrides& ov, UnityFile& head, const std::string& prefabName) {
    const int64_t root = head.findPrefabRoot(prefabName);
    if (!root) return;
    for (int64_t id : head.subtree(root)) {
        const unity::Xform* x = head.xform(id);
        if (!x || x->name.rfind("a_n_", 0) != 0 || ov.count(x->name)) continue;
        LocalOverride o;
        o.pos = x->pos;
        o.rot = x->rot;
        o.hasRot = true;
        o.scl = x->scl;
        ov[x->name] = o;
    }
}

// Adds every transform of a prefab to the scene skeleton, with world matrices built from local transforms
// (so a bone's shape override moves its whole subtree); returns false if the prefab root is missing.
bool addRigPrefab(Scene& scene, Rig& rig, UnityFile& uf, const std::string& prefabName, const Mat4& base,
                  int attachParent, const std::string& source, const Overrides* overrides = nullptr) {
    const int64_t rootId = uf.findPrefabRoot(prefabName);
    if (!rootId) return false;
    std::map<int64_t, int> idx;
    std::map<int64_t, Mat4> worlds;
    for (int64_t id : uf.subtree(rootId)) {
        const unity::Xform* x = uf.xform(id);
        Mat4 w;
        int parent = attachParent;
        if (id == rootId) {
            // SetParent(parent, false): a rig prefab's own root transform stays under the node it is hung on.
            w = base * Mat4::TRS(x->pos, x->rot, x->scl);
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
        b.world = w;
        scene.bones.push_back(b);
        idx[id] = static_cast<int>(scene.bones.size()) - 1;
        rig.world[x->name] = w;
    }
    return true;
}

// The correction table of the alternative bone type (typeBone != 0); a library imported before it was copied lacks it.
void loadCorrectTable(resolve::Resolver& resolver, shape::ShapeState& S, Scene& scene, const char* what) {
    std::string why;
    auto bundle = resolver.openBaseBundle("list/shapecorrect/shapecorrect.unity3d", &why);
    bool ok = false;
    if (bundle)
        for (const auto& [name, bytes] : bundle->readTextAssets())
            if (name == "shapecorrect") ok = shape::ParseCorrectTable(bytes, S);
    if (!ok)
        scene.warn(what, "this body uses an alternative bone type (typeBone " + std::to_string(S.typeBone) +
                         ") but the library has no list/shapecorrect/shapecorrect.unity3d; import the library again, proportions may be slightly off");
}

// The card's body sliders as local transforms for the skeleton's driven bones (ChaControl.UpdateShapeBodyValueFromCustomInfo
// followed by ShapeBodyInfoFemale.Update/UpdateAlways, copied as generated code in shape/).
Overrides bodyShapeOverrides(const card::Card& card, resolve::Resolver& resolver, UnityFile& base, UnityFile& rigFile,
                            const std::string& rigPrefab, Scene& scene, const AssembleOptions& opt) {
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
    const int64_t rigRoot = rigFile.findPrefabRoot(rigPrefab);
    if (!rigRoot) return out;
    for (int64_t id : rigFile.subtree(rigRoot)) {
        const unity::Xform* x = rigFile.xform(id);
        if (bones.count(x->name)) continue;
        shape::ShapeBone sb;
        sb.pos = x->pos;
        sb.scl = x->scl;
        bones[x->name] = sb;
    }

    shape::ShapeState S;
    S.typeBone = static_cast<int>(card.body.at("typeBone").asInt());
    if (S.typeBone != 0) loadCorrectTable(resolver, S, scene, "body");
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
    // Viewer sliders: a value the user set replaces the card's own.
    for (size_t i = 0; i < v.size() && i < opt.bodyShape.size(); ++i) v[i] = opt.bodyShape[i];
    if (v.size() > 13) {
        float nip = opt.nippleInside ? 0.0f : static_cast<float>(card.status.at("nipStandRate").asFloat());
        v[13] = v[13] + (1.0f - v[13]) * std::min(1.0f, std::max(0.0f, nip));
        if (opt.nippleInside) v[13] = std::min(v[13], 0.0f);
        if (opt.nippleInside) v[11] = 0.0f;   // the areola dome flat too: it is what shows as a pale disc through thin clothes
    }
    for (size_t i = 0; i < v.size(); ++i) shape::ChangeValue(S, anm, cats, static_cast<int>(i), v[i]);
    if (std::getenv("CARDBRIDGE_DEBUG_SHAPE"))
        for (int id : {0, 1, 26, 27})
            std::fprintf(stderr, "src %d %s scl %g %g %g\n", id, srcNames[static_cast<size_t>(id)].c_str(), S.src(id).scl.x, S.src(id).scl.y, S.src(id).scl.z);
    if (std::getenv("CARDBRIDGE_DEBUG_SHAPE")) { std::fprintf(stderr, "body values:"); for (size_t i = 0; i < v.size(); ++i) std::fprintf(stderr, " %zu=%.3f", i, v[i]); std::fprintf(stderr, "\n"); }
    shape::RunBodyUpdate(S, 7);
    shape::RunBodyUpdateAlways(S);
    // Nipple placement along the chest normal (the nipple bones' local z): the sliders only reach the shape the maker allows, which still
    // stands out of the skin and shows through thin clothes; this moves the tip below or above it.
    {
        const float off = opt.nippleInside ? std::min(opt.nippleOffset, opt.nippleInsideOffset) : opt.nippleOffset;
        if (off != 0.0f)
            for (const char* nm : {"cf_s_bnip02_L", "cf_s_bnip02_R", "cf_s_bnipacc_L", "cf_s_bnipacc_R", "cf_s_bnip015_L", "cf_s_bnip015_R",
                              "cf_s_bnip025_L", "cf_s_bnip025_R", "cf_s_bnip01_L", "cf_s_bnip01_R"}) {
                auto it = bones.find(nm);
                if (it != bones.end()) it->second.pos.z += off;
            }
    }

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
                             Scene& scene, const AssembleOptions& opt) {
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
    const int64_t rigRoot = rigFile.findPrefabRoot(rigPrefab);
    if (!rigRoot) return out;
    for (int64_t id : rigFile.subtree(rigRoot)) {
        const unity::Xform* x = rigFile.xform(id);
        if (bones.count(x->name)) continue;
        shape::ShapeBone sb;
        sb.pos = x->pos;
        sb.scl = x->scl;
        bones[x->name] = sb;
        if (id != rigRoot) if (const unity::Xform* par = rigFile.xform(x->parent)) parentOf[x->name] = par->name;
    }
    for (auto& [n, b] : bones) nameOf[&b] = n;

    // World scale of the node the head rig hangs from (cf_s_head: the body shape scales it).
    auto colLen = [&](int c) { return std::sqrt(headParentWorld.m[0 * 4 + c] * headParentWorld.m[0 * 4 + c] + headParentWorld.m[1 * 4 + c] * headParentWorld.m[1 * 4 + c] + headParentWorld.m[2 * 4 + c] * headParentWorld.m[2 * 4 + c]); };
    const Vec3 attachScale{colLen(0), colLen(1), colLen(2)};

    shape::ShapeState S;
    S.typeBone = static_cast<int>(card.body.at("typeBone").asInt());
    if (S.typeBone != 0) {
        // ChaControl.UpdateShapeFace: the head is divided by the body's neck-base height correction
        shape::ShapeState corr;
        corr.typeBone = S.typeBone;
        loadCorrectTable(resolver, corr, scene, "head");
        S.headCorrectValue = 1.0f / (1.0f + corr.correct(2).scl.y);
    }
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
    for (size_t i = 0; i < vals.size(); ++i) {
        float fv = i < opt.faceShape.size() ? opt.faceShape[i] : static_cast<float>(vals.at(i).asFloat());
        shape::ChangeValue(S, anm, cats, static_cast<int>(i), neutral ? 0.5f : fv);
    }
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
    std::set<int64_t> tintOnly;           // accessories: the renderers the game colours (rendNormal / rendHair / the custom-hair component); empty = all
    // accessories: the colour triple the game applies to that renderer (ChaControl.ChangeAccessoryColor, then HairAccessoryCustomizer at run time;
    // last write wins). A channel whose w is < 0 means the game leaves the prefab's own value for it.
    std::map<int64_t, std::array<Vec4, 3>> tintByRenderer;
    bool bakedOutfitColors = false;       // the outfit's slot colours are already baked into the clothing texture
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

// The KSOX "under" layers (BodyUnder/FaceUnder): full-coverage skin repaints the card paints beneath the skin's own detail.
// They belong on the base texture before the create shader runs (source-over onto the flat base); drawn over the finished
// skin they would bury the mask-derived shading, the paint and (for the face) the makeup the create shader draws. `composited`,
// when given, reports whether an image was actually laid over the base.
std::shared_ptr<unity::TextureData> skinWithUnder(const std::shared_ptr<unity::TextureData>& baseTex, int overlayType,
                                                  const card::MaterialEdits* edits, bool* composited = nullptr) {
    if (composited) *composited = false;
    if (!baseTex || !edits) return baseTex;
    card::MaterialEdits::Image ov;
    if (!edits->decodeSkinOverlay(overlayType, ov)) return baseTex;
    auto comp = std::make_shared<unity::TextureData>(*baseTex);
    alphaOver(comp->rgba, comp->width, comp->height, ov);
    if (composited) *composited = true;
    return comp;
}

// Runs the DynamicBone components of an item's prefab to rest, as the game's still character does, and returns the world matrices (in the
// prefab's own space, like UnityFile::worldMatrix) of every transform they move. `P` is the world matrix the prefab's root hangs on.
bool settleItem(UnityFile& uf, int64_t prefabRoot, const Mat4& P, const Mat4* P0, const std::vector<DbCollider>& colliders,
                const std::map<int64_t, Mat4>* localOv, std::map<int64_t, Mat4>& simW) {
    std::vector<unity::MonoInfo> dbs;
    for (auto& mb : uf.monoBehavioursUnder(prefabRoot))
        if (mb.className == "DynamicBone") dbs.push_back(std::move(mb));
    if (dbs.empty()) return false;

    std::vector<int64_t> ids = uf.subtree(prefabRoot);
    std::map<int64_t, int> index;
    for (size_t i = 0; i < ids.size(); ++i) index[ids[i]] = static_cast<int>(i);
    std::map<int64_t, int64_t> xformOfGo;
    for (const auto& [id, x] : uf.xforms()) xformOfGo[x.goId] = id;
    std::vector<Mat4> locals(ids.size());
    std::vector<int> parents(ids.size(), -1);
    for (size_t i = 1; i < ids.size(); ++i) {
        const unity::Xform* x = uf.xform(ids[i]);
        auto ov = localOv ? localOv->find(ids[i]) : std::map<int64_t, Mat4>::const_iterator{};
        locals[i] = (localOv && ov != localOv->end()) ? ov->second : Mat4::TRS(x->pos, x->rot, x->scl);
        auto pi = index.find(x->parent);
        parents[i] = pi != index.end() ? pi->second : 0;
    }
    DbTree tree;
    tree.Init(locals, parents, P);
    DbTree before = tree;   // the same item where the game first put it, on the character before its shape sliders and bone modifiers
    int updates = 0;
    if (P0) {
        before.rootWorld = *P0;
        updates = std::getenv("CARDBRIDGE_DB_STEPS") ? std::atoi(std::getenv("CARDBRIDGE_DB_STEPS")) : 10;
    }

    auto vec3 = [](const Value& v) { return Vec3{static_cast<float>(v.at("x").asFloat()), static_cast<float>(v.at("y").asFloat()), static_cast<float>(v.at("z").asFloat())}; };
    auto curve = [](const Value& v) {
        DbCurve c;
        for (const auto& k : v.at("m_Curve").asArray())
            c.keys.push_back({static_cast<float>(k.at("time").asFloat()), static_cast<float>(k.at("value").asFloat()),
                              static_cast<float>(k.at("inSlope").asFloat()), static_cast<float>(k.at("outSlope").asFloat())});
        return c;
    };
    auto nodeOf = [&](const Value& pptr) -> int {
        if (pptr.at("m_FileID").asInt() != 0) return -1;
        auto it = index.find(pptr.at("m_PathID").asInt());
        return it == index.end() ? -1 : it->second;
    };
    std::set<int> moved;
    for (const auto& mb : dbs) {
        const Value& f = mb.fields;
        DbParams p;
        p.root = nodeOf(f.at("m_Root"));
        if (p.root < 0) continue;
        auto go = xformOfGo.find(mb.goId);
        if (go != xformOfGo.end()) { auto oi = index.find(go->second); if (oi != index.end()) p.owner = oi->second; }
        p.updateRate = static_cast<float>(f.at("m_UpdateRate").asFloat(60.0));
        p.damping = static_cast<float>(f.at("m_Damping").asFloat());
        p.elasticity = static_cast<float>(f.at("m_Elasticity").asFloat());
        p.stiffness = static_cast<float>(f.at("m_Stiffness").asFloat());
        p.inert = static_cast<float>(f.at("m_Inert").asFloat());
        p.radius = static_cast<float>(f.at("m_Radius").asFloat());
        p.endLength = static_cast<float>(f.at("m_EndLength").asFloat());
        p.endOffset = vec3(f.at("m_EndOffset"));
        p.gravity = vec3(f.at("m_Gravity"));
        p.force = vec3(f.at("m_Force"));
        p.freezeAxis = static_cast<int>(f.at("m_FreezeAxis").asInt());
        p.dampingC = curve(f.at("m_DampingDistrib"));
        p.elasticityC = curve(f.at("m_ElasticityDistrib"));
        p.stiffnessC = curve(f.at("m_StiffnessDistrib"));
        p.inertC = curve(f.at("m_InertDistrib"));
        p.radiusC = curve(f.at("m_RadiusDistrib"));
        for (const auto& e : f.at("m_Exclusions").asArray()) if (int n = nodeOf(e); n >= 0) p.exclusions.push_back(n);
        for (const auto& e : f.at("m_notRolls").asArray()) if (int n = nodeOf(e); n >= 0) p.notRolls.push_back(n);
        int steps = SettleDynamicBone(tree, p, colliders, updates > 0 ? &before : nullptr, updates);
        if (std::getenv("CARDBRIDGE_DEBUG_DB")) {
            Mat4 ow = p.owner >= 0 ? tree.WorldOf(p.owner, true) : P;
            std::fprintf(stderr, "dbsettle root=%s owner=%s scale.x=%.3f steps=%d radius=%g\n", uf.xform(ids[static_cast<size_t>(p.root)])->name.c_str(),
                         p.owner >= 0 ? uf.xform(ids[static_cast<size_t>(p.owner)])->name.c_str() : "?", std::sqrt(ow.m[0] * ow.m[0] + ow.m[4] * ow.m[4] + ow.m[8] * ow.m[8]), steps, p.radius);
        }
        std::vector<int> stack{p.root};
        while (!stack.empty()) {
            int n = stack.back();
            stack.pop_back();
            moved.insert(n);
            for (int c : tree.children[static_cast<size_t>(n)]) stack.push_back(c);
        }
    }
    if (moved.empty()) return false;
    const Mat4 rootW = uf.worldMatrix(prefabRoot, localOv);
    const Mat4 toPrefab = rootW * inverse(P);
    for (int n : moved) simW[ids[static_cast<size_t>(n)]] = toPrefab * tree.WorldOf(n);
    if (std::getenv("CARDBRIDGE_DEBUG_DB")) {
        auto cl = [&](int c) { return std::sqrt(P.m[c] * P.m[c] + P.m[4 + c] * P.m[4 + c] + P.m[8 + c] * P.m[8 + c]); };
        std::fprintf(stderr, "dbanchor pos %.3f %.3f %.3f scale %.3f %.3f %.3f\n", P.m[3], P.m[7], P.m[11], cl(0), cl(1), cl(2));
    }
    if (std::getenv("CARDBRIDGE_DEBUG_DB"))
        for (int n : moved) {
            Vec3 a = tree.WorldOf(n, true).translation(), c = tree.WorldOf(n).translation();
            std::fprintf(stderr, "dbnode %s rest %.4f %.4f %.4f sim %.4f %.4f %.4f\n", uf.pathOf(ids[static_cast<size_t>(n)]).c_str(), a.x, a.y, a.z, c.x, c.y, c.z);
        }
    return true;
}

struct Builder {
    const std::map<int64_t, Mat4>* localOv = nullptr;   // local-matrix overrides for the prefab being added (accessory N_move nodes)
    Mat4 W(UnityFile& uf, int64_t id) const { return uf.worldMatrix(id, localOv); }
    // The same with the DynamicBone settle applied (prefab space): the pose a hair or accessory chain ends in.
    Mat4 Wp(UnityFile& uf, int64_t id) const {
        if (simW) {
            auto it = simW->find(id);
            if (it != simW->end()) return it->second;
        }
        return W(uf, id);
    }
    Scene& scene;
    Rig& rig;
    std::map<std::string, int> textureCache;   // "<bundle>#<pathId>" -> scene texture
    std::map<std::string, int> accessoryBones; // "<part>#<xformId>" -> scene bone (accessory-owned skeletons)
    std::map<int64_t, int> goMesh;             // renderer GameObject id -> scene mesh, for the item being added

    // MaterialEditor texture replacements stored in the card, for the item being added (-1 type = none).
    // Colours the game's ChaControl writes into the item's materials at run time (ChangeAccessoryColor, ChangeSettingHairColor ...),
    // by Unity property; the shader reads them. Set per renderer before it is added.
    std::vector<std::pair<std::string, Vec4>> runtime;
    bool glossForThisRenderer = false;
    bool stripHairGloss = false;   // HairAccessoryCustomizer HairGloss=false: clear the prefab's _HairGloss like the game does
    int hairGlossTexture = -1;   // the card's hair gloss map (_HairGloss of hair renderers)
    // Head only: blend-shape weights (0..100) of the character's resting expression, by renderer GameObject id and shape index.
    const std::map<int64_t, std::vector<std::pair<int, float>>>* blend = nullptr;
    // Textures the game puts on a material at run time besides its main one (face lips and eye shadow, body nipples and underhair):
    // property -> scene texture, bound for the renderer being added. Colours for them go through `runtime`.
    std::vector<std::pair<std::string, int>> extraTex;
    std::vector<std::pair<std::string, Vec4>> stOverride;   // texture property -> scale.xy offset.xy (the eye look controller's iris and highlight placement)
    float blushAlpha = -1;       // face material only: alpha of _overcolor2, the emotion blush (ChaControl.ChangeHohoAkaRate); < 0 = leave
    const std::map<int64_t, Mat4>* simW = nullptr;   // settled world matrices (prefab space) of the item being added
    std::vector<DbCollider> dbColliders;   // the body's DynamicBoneColliders, in world space
    const Rig* rigDefault = nullptr;       // the skeleton without shape sliders and bone modifiers: where the game's hair started
    const Mat4* hairParent = nullptr;   // world matrix of the hair parent node while a hair item is added
    const card::MaterialEdits* edits = nullptr;
    int meCoordinate = 0, meType = -1, meSlot = 0;
    std::map<int, int> meTextures;   // embedded TexID -> scene texture (-1 when unreadable)
    // Stiletto shoe warp for the footwear item being added (bind delta for cf_j_foot binds).
    HeelConfig heelWarp;
    bool heelWarpOn = false;

    // The embedded images are linear dumps, not sRGB authoring images (the game displays them unconverted); `colour` picks the
    // cache slot only (colour maps are the main textures the ME edit points at).
    // `forceSrgb` overrides the default (linear): an edit on `MainTex` that replaces a texture the prefab material
    // already binds inherits that texture's colour space, because the game restores the editor's texture with it.
    // A replaced albedo (sRGB in the bundle) must decode on sample and round-trip in the frame; treating it as a
    // linear dump brightens it to a wash. -1 = keep the linear default (no original to inherit from, or a data map).
    int embeddedTexture(int texId, const std::string& partId, bool colour = true, int forceSrgb = -1) {
        const int key = colour ? texId : texId + (1 << 20);
        const int slot = key * 4 + (forceSrgb + 1);   // forceSrgb -1/0/1 stay separate cache entries
        auto it = meTextures.find(slot);
        if (it != meTextures.end()) return it->second;
        card::MaterialEdits::Image img;
        int result = -1;
        if (edits && edits->decode(texId, img)) {
            result = addSceneTexture("card_tex_" + std::to_string(texId), "card (MaterialEditor)", img.width, img.height, std::move(img.rgba));
            // A replaced main texture keeps the colour space of the material's original main texture when the call site
            // knows it (forceSrgb 1/0). Otherwise the image's own colour-space hints decide: the character maker's PNGs are
            // sRGB authoring images (they carry sRGB/gAMA/iCCP chunks, as does any JPEG), and treating them as linear lifts
            // every dark tone. Only a file with no colour-space chunk stays on the old linear default.
            scene.textures[static_cast<size_t>(result)].srgb = forceSrgb >= 0 ? forceSrgb > 0 : img.srgb;
        } else scene.warn(partId, "MaterialEditor texture " + std::to_string(texId) + " stored in the card could not be decoded");
        meTextures[slot] = result;
        return result;
    }

    int textureFor(UnityFile& uf, const PPtr& p, const std::string& partId) {
        std::string key = uf.name() + "#" + std::to_string(p.pathId);
        auto it = textureCache.find(key);
        if (it != textureCache.end()) return it->second;
        std::string why;
        std::shared_ptr<unity::TextureData> t;
        if (resolverPtr && !uf.source().empty())
            if (auto bound = resolverPtr->cachedDecodedTexture(uf.source(), uf.statPath(), p.pathId))
                t = std::make_shared<unity::TextureData>(*bound);
        if (!t) {
            if (auto fresh = uf.readTexture(p, &why)) {
                t = std::make_shared<unity::TextureData>(std::move(*fresh));
                if (resolverPtr && !uf.source().empty()) resolverPtr->cacheDecodedTexture(uf.source(), uf.statPath(), p.pathId, t);
            }
        }
        int result = -1;
        if (!t) {
            scene.warn(partId, "texture not loaded: " + why);
        } else {
            SceneTexture st;
            st.name = t->name;
            st.source = uf.name();
            st.format = unity::FormatName(t->unityFormat);
            st.width = t->width;
            st.height = t->height;
            st.pixelHash = Fnv1a64(t->rgba.data(), t->rgba.size());
            st.rgba = std::move(t->rgba);
            scene.textures.push_back(std::move(st));
            scene.textures.back().clamp = t->wrapMode == 1;   // the game clamps what the bundle marks as clamp
            scene.textures.back().srgb = t->colorSpace == 1;
            result = static_cast<int>(scene.textures.size()) - 1;
        }
        textureCache[key] = result;
        return result;
    }

    std::map<std::string, int> shaderCache;    // "<bundle>#<pathId>" -> scene shader (-1 = unreadable)
    std::map<std::string, int> shaderTexCache; // "<bundle>#<pathId>" -> scene texture, at most 1024 px on a side

    resolve::Resolver* resolverPtr = nullptr;
    std::map<std::string, int> shaderByNameCache;

    // Whether a material is drawn from both sides: alpha and hair shaders (hair cards are drawn from both sides in the game; a culled PMX
    // loses half of them), and whatever the shader itself draws unculled (Cull Off, or the material float it reads its culling from at 0).
    bool twoSided(int shaderIndex, const std::string& shaderName, const unity::MaterialData& m) const {
        if (contains(shaderName, "alpha") || contains(shaderName, "hair")) return true;
        if (shaderIndex < 0) return false;
        const SceneShader& sh = scene.shaders[static_cast<size_t>(shaderIndex)];
        int cull = sh.cull;
        for (const auto& f : m.floats) if (!sh.cullProperty.empty() && f.first == sh.cullProperty) cull = static_cast<int>(f.second);
        return cull == 0;
    }

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
                unity::ShaderRenderState rs;
                if (uf.readShaderRenderState(pathId, rs)) {
                    sh.cull = rs.cull;
                    sh.cullProperty = rs.cullProperty;
                    sh.zWrite = rs.zWrite;
                    sh.queue = rs.queue;
                }
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
            scene.textures[static_cast<size_t>(result)].srgb = t->colorSpace == 1;
            scene.textures[static_cast<size_t>(result)].format = unity::FormatName(t->unityFormat);
        }
        shaderTexCache[key] = result;
        return result;
    }

    // "<bundle>#<pathId>" -> decoded pixels, bounded LRU. Previously every
    // texture decoded during assembly was kept until the Builder died, so a
    // character's decoded textures (hundreds of MB at full resolution) sat
    // beside the scene textures they were copied into - the same pixels
    // twice. 64 MB keeps the shared ramps, masks and the texture a whole
    // item's materials touch, and evicts the rest (re-decoded from the still
    // cached bundle on demand; the scene copy is what is kept).
    struct DecodedEntry { std::string key; std::shared_ptr<unity::TextureData> tex; size_t bytes = 0; };
    std::list<DecodedEntry> decoded;
    std::map<std::string, std::list<DecodedEntry>::iterator> decodedByKey;
    size_t decodedBytes = 0;
    static constexpr size_t kDecodedCap = 64ull * 1024 * 1024;
    void decodedEvict() {
        while (decodedBytes > kDecodedCap && decoded.size() > 1) {
            auto& back = decoded.back();
            decodedBytes -= back.bytes;
            decodedByKey.erase(back.key);
            decoded.pop_back();
        }
    }

    std::map<std::string, int> bakedCache;                                  // tint recipe -> scene texture

    std::shared_ptr<unity::TextureData> decode(UnityFile& uf, const PPtr& p, const std::string& partId) {
        std::string key = uf.name() + "#" + std::to_string(p.pathId);
        auto it = decodedByKey.find(key);
        if (it != decodedByKey.end()) {
            decoded.splice(decoded.begin(), decoded, it->second);
            return it->second->tex;
        }
        std::string why;
        std::shared_ptr<unity::TextureData> out;
        // The process-wide decoded-texture cache outlives this assembly (an outfit switch or a
        // reload in the viewer decodes the same bundles again): copy out of it, because the
        // caller may consume the shared TextureData.
        if (resolverPtr && !uf.source().empty()) {
            if (auto bound = resolverPtr->cachedDecodedTexture(uf.source(), uf.statPath(), p.pathId))
                out = std::make_shared<unity::TextureData>(*bound);   // private copy: the caller may consume it, the cache object must stay intact
        }
        if (!out) {
            auto t = uf.readTexture(p, &why);
            if (t) {
                out = std::make_shared<unity::TextureData>(std::move(*t));
                if (resolverPtr && !uf.source().empty()) resolverPtr->cacheDecodedTexture(uf.source(), uf.statPath(), p.pathId, out);
            } else {
                scene.warn(partId, "texture not loaded: " + why);
            }
        }
        if (out) {
            DecodedEntry e;
            e.key = key;
            e.tex = out;
            e.bytes = out->rgba.size();
            decoded.push_front(e);
            decodedByKey[key] = decoded.begin();
            decodedBytes += decoded.front().bytes;
            decodedEvict();
        }
        return out;
    }

    int addSceneTexture(const std::string& name, const std::string& source, int w, int h, std::vector<uint8_t>&& rgba) {
        SceneTexture st;
        st.name = name;
        st.source = source;
        st.format = "RGBA8";
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
        scene.textures[static_cast<size_t>(idx)].clamp = main->wrapMode == 1;   // the tint copy samples exactly like its source
        scene.textures[static_cast<size_t>(idx)].srgb = main->colorSpace == 1;
        bakedCache[key] = idx;
        return idx;
    }

    // `look` (may be null) says how this item's materials are coloured; `matIndex` is the material's slot on its renderer.
    // `hairTintColor` (may be null) is the character's hair colour for a hair material (the game overwrites its _Color).
    int materialFor(UnityFile& uf0, const PPtr& p0, const std::string& partId, const Vec4* fallbackColor, const ItemLook* look,
                    const RendererInfo& r, size_t matIndex, const Vec4* hairTintColor = nullptr) {
        SceneMaterial sm;
        std::string why;
        // A material the prefab's bundle only points at (the body's cf_m_body in mm_base) is read from the library bundle that holds it;
        // its textures and shader then resolve against that bundle.
        UnityFile* ufp = &uf0;
        PPtr p = p0;
        std::shared_ptr<UnityFile> external;
        if (!p0.isNull() && !p0.isLocal() && openCab) {
            external = openCab(uf0.externalNameOf(p0));
            if (external) { ufp = external.get(); p.fileId = 0; }
        }
        UnityFile& uf = *ufp;
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
            sm.renderQueue = m->renderQueue;
            if (m->shaderPathId) sm.shaderIndex = shaderFor(uf, m->shaderPathId, m->shaderName);
            // The game's own shader lerps the card's colour into the texture through the
            // mask at draw time; tinting the copy we feed the bake as well multiplies the
            // colour in twice and crushes it towards black. Only tint on the CPU when the
            // shader provably ignores _Color (ADR-009).
            sm.readsColor = cardbridge::material::ShaderReadsColor(scene, sm.shaderIndex);
            for (const auto& c : m->colors) sm.props.emplace_back(c.first, c.second);
            for (const auto& rp : runtime) {
                bool found = false;
                for (auto& pr : sm.props) if (pr.first == rp.first) { pr.second = rp.second; found = true; }
                if (!found) sm.props.push_back(rp);
                if (rp.first == "_nipsize") sm.cardAreola = true;
            }
            // The outfit's slot colours are already baked into the clothing texture the create step built: applying them a
            // second time through the prefab's stored _Color (the outfit's default colour) crushes the outfit to black.
            if (look && look->bakedOutfitColors && matIndex == 0 && ((look->use1.count(r.componentId) && look->tex1 >= 0) || (look->use2.count(r.componentId) && look->tex2 >= 0)))
                for (auto& pr : sm.props)
                    if (pr.first == "_Color" || pr.first == "_Color2" || pr.first == "_Color3") pr.second = Vec4{1, 1, 1, 1};
            if (const Vec4* tint = hairTintColor ? hairTintColor
                                                 : (fallbackColor && (sm.shader == "Shader Forge/main_hair" || sm.shader == "Shader Forge/main_hair_front")
                                                        ? fallbackColor
                                                        : nullptr)) {
                // In the game the character maker's hair colour overwrites the _Color of every hair material
                // (ChaController). Accessories follow their own rules instead (see the colour block further down). The tint is
                // already lifted off flat black (see hairTint) so the texture keeps its strand gradient.
                bool set = false;
                for (auto& pr : sm.props) if (pr.first == "_Color") { pr.second = *tint; set = true; }
                if (!set) sm.props.emplace_back("_Color", *tint);
                sm.diffuse = {tint->x, tint->y, tint->z, 0.0f};
                sm.hairTinted = true;
            }
            if (blushAlpha >= 0)
                for (auto& pr : sm.props) if (pr.first == "_overcolor2") pr.second.w = blushAlpha;
            for (const auto& f : m->floats) {   // the material's own floats; a run-time value of the same name (the card's _nipsize) is not overridden by it
                bool set = false;
                for (const auto& rp : runtime) set = set || rp.first == f.first;
                if (!set) sm.props.emplace_back(f.first, Vec4{f.second, 0, 0, 0});
            }
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
            if (stripHairGloss) {   // HairAccessoryCustomizer with HairGloss off: the game sets the gloss texture to null
                auto& tp = sm.texProps;
                tp.erase(std::remove_if(tp.begin(), tp.end(), [](const SceneTexBinding& tb) { return tb.name == "_HairGloss"; }), tp.end());
            } else if (hairGlossTexture >= 0 && glossForThisRenderer) {
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
            // A property the material does not store has the shader's declared default, which is what the game draws with.
            if (sm.shaderIndex >= 0) {
                bool storedWidth = false, storedColor = m->color("_LineColor") != nullptr;
                for (const auto& f : m->floats) storedWidth = storedWidth || f.first == "_LineWidthS" || f.first == "_LineWidth";
                for (const auto& d : scene.shaders[static_cast<size_t>(sm.shaderIndex)].propDefaults) {
                    if (!storedWidth && (d.first == "_LineWidthS" || d.first == "_LineWidth")) sm.outlineWidth = d.second.x;
                    if (!storedColor && d.first == "_LineColor") sm.outlineColor = {d.second.x, d.second.y, d.second.z};
                }
            }
            for (const auto& rp : runtime)   // the card's own outline colour (hair)
                if (rp.first == "_LineColor") sm.outlineColor = {rp.second.x, rp.second.y, rp.second.z};
            if (const Vec4* c = m->color("_SpecularColor")) {
                float k = std::min(1.0f, std::max(0.0f, specPow));   // _SpecularPower is the highlight strength in these shaders
                sm.specular = {c->x * k, c->y * k, c->z * k};
            }
            sm.alpha = contains(m->shaderName, "alpha");
            sm.doubleSided = twoSided(sm.shaderIndex, m->shaderName, *m);
            if (look && matIndex == 0) {
                if (look->use1.count(r.componentId) && look->tex1 >= 0) sm.texture = look->tex1;
                else if (look->use2.count(r.componentId) && look->tex2 >= 0) sm.texture = look->tex2;
            }
            // The colour triple the game applies to this renderer (accessory rules; a channel with w < 0 keeps the prefab's own value).
            auto resolveTint = [&]() -> std::array<Vec4, 3> {
                if (!look) return {Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}, Vec4{1, 1, 1, 1}};
                Vec4 t[3] = {look->colors[0], look->colors[1], look->colors[2]};
                if (const auto it = look->tintByRenderer.find(r.componentId); it != look->tintByRenderer.end()) {
                    const char* kNames[3] = {"_Color", "_Color2", "_Color3"};
                    for (int k = 0; k < 3; ++k) {
                        Vec4 v = it->second[k];
                        if (v.w < 0.0f) v = (m && m->color(kNames[k])) ? *m->color(kNames[k]) : Vec4{1, 1, 1, 1};
                        t[k] = v;
                    }
                }
                return {t[0], t[1], t[2]};
            };
            if (sm.texture < 0) {
                const PPtr* tp = m->texture("_MainTex");
                if (tp && !tp->isNull()) {
                    if (look && look->materialTint && (look->tintOnly.empty() || look->tintOnly.count(r.componentId)) && sm.readsColor != 1) {
                        std::array<Vec4, 3> tintArr = resolveTint();
                        Vec4* tint = tintArr.data();
                        if (edits && meType >= 0)   // MaterialEditor colour overrides replace the card's slot colours in the tint
                            for (const auto* e : edits->editsFor(edits->colors, meCoordinate, meType, meSlot, sm.name)) {
                                int k = e->property == "Color" ? 0 : e->property == "Color2" ? 1 : e->property == "Color3" ? 2 : -1;
                                if (k >= 0) tint[k] = Vec4{e->value[0], e->value[1], e->value[2], e->value[3]};
                            }
                        // The tint recipe is known before any pixels are decoded: a renderer that shares it with one
                        // already baked (the same accessory texture and colours) reuses the scene texture instead of
                        // decoding the main and mask textures again only to discard them.
                        const PPtr* mp = m->texture("_ColorMask");
                        const std::string mainKey = uf.name() + "#" + std::to_string(tp->pathId);
                        const std::string maskKey = (mp && !mp->isNull() && mp->isLocal()) ? uf.name() + "#" + std::to_string(mp->pathId) : "-";
                        char buf[160];
                        std::snprintf(buf, sizeof buf, "|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f", tint[0].x, tint[0].y, tint[0].z, tint[1].x, tint[1].y, tint[1].z, tint[2].x, tint[2].y, tint[2].z);
                        auto cached = bakedCache.find(mainKey + "|" + maskKey + buf);
                        if (cached != bakedCache.end()) {
                            sm.texture = cached->second;
                        } else {
                            auto main = decode(uf, *tp, partId);
                            std::shared_ptr<unity::TextureData> mask;
                            if (mp && !mp->isNull() && mp->isLocal()) mask = decode(uf, *mp, partId);
                            sm.texture = bake(main, mainKey, mask, maskKey, tint, uf.name());
                        }
                    } else {
                        sm.texture = textureFor(uf, *tp, partId);
                    }
                }
            }
            if (sm.texture < 0 && look && look->materialTint && (look->tintOnly.empty() || look->tintOnly.count(r.componentId))) { auto t = resolveTint(); sm.diffuse = Vec4{t[0].x, t[0].y, t[0].z, 1.0f}; }
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
                // Some shaders the card switches on only exist inside the card's own
                // bundle (the author's translucent fabric shader IBL_Shader_alpha / the
                // IBL set for the same purpose). Falling back to the opaque twin turns
                // a cutout transparent shape into a solid black lump behind the top.
                // Prefer a library twin that keeps the alpha semantics instead;
                // anything without a workable twin stays on the original shader.
                std::string fallback;
                if (si < 0) {
                    if (contains(e->property, "alpha")) fallback = "Shader Forge/main_alpha";
                    else if (contains(e->property, "item")) fallback = "Shader Forge/main_item";
                    if (!fallback.empty()) si = shaderByName(fallback);
                }
                if (si < 0) { scene.warn(partId, "MaterialEditor shader " + e->property + " for " + sm.name + " is not in the library: kept " + sm.shader); continue; }
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
                // Any shader carrying "alpha" is a translucency shader for the purpose of the export: it stays
                // blended (not cut out), keeps its own texture as authored, the body behind it stays visible
                // and MikuMikuDance gets an alpha below 1 so it blends - like the game draws it.
                if (contains(e->property, "alpha")) sm.translucent = true;
                if (m_original) sm.doubleSided = twoSided(si, e->property, *m_original);
            }
            auto setProp = [&](const std::string& name, Vec4 v) {
                for (auto& pr : sm.props) if (pr.first == name) { pr.second = v; return; }
                sm.props.emplace_back(name, v);
            };
            for (const auto* e : edits->editsFor(edits->floats, meCoordinate, meType, meSlot, sm.name)) setProp("_" + e->property, Vec4{e->value[0], 0, 0, 0});
            for (const auto* e : edits->editsFor(edits->colors, meCoordinate, meType, meSlot, sm.name)) setProp("_" + e->property, Vec4{e->value[0], e->value[1], e->value[2], e->value[3]});
            // Texture overrides: the main texture (also what the PMX shows) and the other maps the shader reads, each with its tiling.
            for (const auto* e : edits->texEditsFor(meCoordinate, meType, meSlot, sm.name)) {
                const std::string prop = "_" + e->property;
                const bool main = e->property == "MainTex";
                int orig = -1, forceSrgb = -1;   // MainTex edits inheriting an sRGB original: round-trip, don't encode like a linear dump
                for (auto& tb : sm.texProps) if (tb.name == prop) orig = tb.texture;
                if (main && orig >= 0 && static_cast<size_t>(orig) < scene.textures.size() && scene.textures[static_cast<size_t>(orig)].srgb)
                    forceSrgb = 1;
                if (std::getenv("CARDBRIDGE_DEBUG_ME"))
                    std::fprintf(stderr, "ME %s %s prop=%s texId=%d origTex=%d name=%s srgb=%d force=%d\n",
                                 partId.c_str(), sm.name.c_str(), prop.c_str(), e->texId, orig,
                                 (orig >= 0 && static_cast<size_t>(orig) < scene.textures.size()) ? scene.textures[static_cast<size_t>(orig)].name.c_str() : "-",
                                 (orig >= 0 && static_cast<size_t>(orig) < scene.textures.size()) ? (scene.textures[static_cast<size_t>(orig)].srgb ? 1 : 0) : -1,
                                 forceSrgb);
                int t = e->texId >= 0 ? embeddedTexture(e->texId, partId, main, forceSrgb) : -1;
                if (main && t >= 0) {
                    sm.texture = t;
                    sm.diffuse = {1, 1, 1, 1};
                    // The embedded image is the complete authored look; where the outfit colours are already composed
                    // into the texture the same ones must not be applied again (same rule as the outfit create path).
                    // Face paints keep their run-time colours (brow/lash), only outfit materials whiten here.
                    if (look && look->bakedOutfitColors)
                        for (auto& pr : sm.props)
                            if (pr.first == "_Color" || pr.first == "_Color2" || pr.first == "_Color3") pr.second = Vec4{1, 1, 1, 1};
                }
                SceneTexBinding* bind = nullptr;
                for (auto& tb : sm.texProps) if (tb.name == prop) bind = &tb;
                if (!bind) {
                    if (t < 0) continue;   // only the tiling of a map this material does not have
                    sm.texProps.emplace_back();
                    bind = &sm.texProps.back();
                    bind->name = prop;
                }
                if (t >= 0) bind->texture = t;
                if (e->hasScale) { bind->st.x = e->scale[0]; bind->st.y = e->scale[1]; }
                if (e->hasOffset) { bind->st.z = e->offset[0]; bind->st.w = e->offset[1]; }
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
        for (int n = 2; scene.boneIndex(nb.name) >= 0; ++n) nb.name = partId + "/" + x->name + "#" + std::to_string(n);   // names stay unique
        nb.parent = parent;
        nb.world = placement * (inverse(W(uf, prefabRoot)) * Wp(uf, xformId));
        nb.pos = nb.world.translation();
        nb.source = partId;
        scene.bones.push_back(nb);
        int idx = static_cast<int>(scene.bones.size()) - 1;
        accessoryBones[key] = idx;
        return idx;
    }

    // The world matrix (model space) of a transform only this item has (a mod's necklace joint, a hair chain joint): its offset from
    // the nearest ancestor the shared skeleton knows, so it moves with the (possibly reshaped) body instead of staying at its authored place.
    // `placement` is where the prefab root hangs (the attach node for accessories, the hair parent for hair), root transform included.
    Mat4 itemBoneWorld(UnityFile& uf, int64_t id, int64_t prefabRoot, const Mat4& placement) {
        const Mat4 rootInv = inverse(W(uf, prefabRoot));
        Mat4 bw = rootInv * Wp(uf, id);
        const unity::Xform* bx = uf.xform(id);
        bool anchored = false;
        for (const unity::Xform* a = bx ? uf.xform(bx->parent) : nullptr; a; a = uf.xform(a->parent)) {
            auto ar = rig.world.find(a->name);
            if (ar == rig.world.end()) continue;
            bw = ar->second * (inverse(rootInv * W(uf, a->id)) * (rootInv * Wp(uf, id)));
            anchored = true;
            break;
        }
        // Not anchored to the shared skeleton: hang the whole prefab on the node the game parents it to.
        if (!anchored) bw = placement * bw;
        return bw;
    }

    // The scene bone of one of the item's own transforms: found by name, else added under the nearest ancestor the scene already has.
    int itemBone(UnityFile& uf, int64_t id, int64_t prefabRoot, const Mat4& placement, const std::string& partId) {
        const unity::Xform* bx = uf.xform(id);
        if (!bx) return -1;
        int bi = scene.boneIndex(bx->name);
        if (bi >= 0) return bi;
        const Mat4 bw = itemBoneWorld(uf, id, prefabRoot, placement);
        // A hair item's own top bone (cf_J_hairB_top ...) hangs on the head's hair parent, not on the model root: left on the root it stays
        // behind when the head moves (a floating cap of hair in MikuMikuDance once a motion turns the head).
        int parent = hairParent ? std::max(0, scene.boneIndex("cf_J_FaceUp_ty")) : 0;
        for (const unity::Xform* a = uf.xform(bx->parent); a; a = uf.xform(a->parent)) {
            int pi = scene.boneIndex(a->name);
            if (pi >= 0) { parent = pi; break; }
        }
        SceneBone nb;
        nb.name = bx->name;
        nb.parent = parent;
        nb.world = bw;
        nb.pos = bw.translation();
        nb.source = partId;
        scene.bones.push_back(nb);
        rig.world[bx->name] = bw;
        return static_cast<int>(scene.bones.size()) - 1;
    }

    // Every transform of an item that works as a bone becomes a scene bone, not only those a mesh is skinned to: the loose joints and
    // end nodes of hair and accessory chains (the DynamicBone chains) and the helper nodes under skin bones. A transform that carries a
    // renderer is geometry, not a bone.
    void addItemBones(UnityFile& uf, int64_t prefabRoot, const std::string& partId, const Mat4& placement, int staticBone, bool accessoryMode, bool noShake = false) {
        std::set<int64_t> geometry, seeds, bones;
        for (const auto& r : uf.renderersUnder(prefabRoot)) {
            geometry.insert(r.xformId);
            for (int64_t id : r.boneXforms) seeds.insert(id);
        }
        for (const auto& mb : uf.monoBehavioursUnder(prefabRoot)) {
            if (mb.className.rfind("DynamicBone", 0) != 0 || mb.className == "DynamicBoneCollider") continue;
            for (const char* key : {"m_Root", "Root", "m_Roots", "Roots"}) {
                const Value& v = mb.fields.at(key);
                if (v.isMap()) { if (v.at("m_FileID").asInt() == 0 && v.at("m_PathID").asInt() != 0) seeds.insert(v.at("m_PathID").asInt()); continue; }
                for (const auto& pp : v.asArray()) if (pp.at("m_FileID").asInt() == 0 && pp.at("m_PathID").asInt() != 0) seeds.insert(pp.at("m_PathID").asInt());
            }
        }
        for (int64_t id : uf.subtree(prefabRoot)) {
            const unity::Xform* x = uf.xform(id);
            if (!x || geometry.count(id)) continue;
            if (!seeds.count(id) && !bones.count(x->parent)) continue;
            bones.insert(id);
            if (accessoryMode) accessoryBone(uf, id, prefabRoot, placement, staticBone, partId);
            else itemBone(uf, id, prefabRoot, placement, partId);
        }
        addChains(uf, prefabRoot, partId, accessoryMode, noShake);
    }

    // The DynamicBone chains of an item (hair strands, skirt and ribbon joints, the body's breast and hip jiggle) as scene chains, for
    // the PMX rigid bodies and joints. Handles the classic component (a root and everything under it) and the character's Ver02 one
    // (a root, a list of bones, per-bone parameters in the selected pattern).
    void addChains(UnityFile& uf, int64_t prefabRoot, const std::string& partId, bool accessoryMode, bool noShake = false) {
        // ChaFile noShake: the maker marked this accessory/hair item rigid (no DynamicBone sway).
        // The game creates no chains for it; keep it rigid so hair does not droop in PMX.
        if (noShake) return;
        auto sceneBoneOf = [&](int64_t id) -> int {
            const unity::Xform* x = uf.xform(id);
            if (!x) return -1;
            if (accessoryMode) {
                auto it = accessoryBones.find(partId + "#" + std::to_string(id));
                return it == accessoryBones.end() ? -1 : it->second;
            }
            return scene.boneIndex(x->name);
        };
        auto pathId = [](const Value& v) { return v.at("m_FileID").asInt() == 0 ? v.at("m_PathID").asInt() : 0; };
        auto vec3Of = [](const Value& v) { return Vec3{static_cast<float>(v.at("x").asFloat()), static_cast<float>(v.at("y").asFloat()), static_cast<float>(v.at("z").asFloat())}; };
        for (const auto& mb : uf.monoBehavioursUnder(prefabRoot)) {
            const bool classic = mb.className == "DynamicBone", ver2 = mb.className == "DynamicBone_Ver02";
            if (!classic && !ver2) continue;
            if (mb.fields.at("m_Enabled").asInt(1) == 0) continue;
            const int64_t rootId = pathId(mb.fields.at(classic ? "m_Root" : "Root"));
            if (!uf.xform(rootId)) continue;
            SceneChain ch;
            ch.part = partId;
            ch.characterOwn = ver2;
            std::vector<int64_t> ids;
            if (classic) {
                std::set<int64_t> excluded;
                for (const auto& e : mb.fields.at("m_Exclusions").asArray()) excluded.insert(pathId(e));
                std::set<int64_t> geometry, inChain;
                for (const auto& r : uf.renderersUnder(prefabRoot)) geometry.insert(r.xformId);
                for (int64_t id : uf.subtree(rootId)) {
                    const unity::Xform* x = uf.xform(id);
                    if (!x || geometry.count(id) || excluded.count(id) || (id != rootId && !inChain.count(x->parent))) continue;
                    inChain.insert(id);
                    ids.push_back(id);
                }
                // A root with no bone under it still moves: the component adds an end particle past it (m_EndLength / m_EndOffset) and swings
                // the bone towards it (a ribbon's tail bone). The bone then hangs from its parent, which is the chain's anchor.
                if (ids.size() == 1) {
                    const Vec3 endOff = vec3Of(mb.fields.at("m_EndOffset"));
                    const bool endParticle = mb.fields.at("m_EndLength").asFloat(0.0) > 0.0 || endOff.x != 0.0f || endOff.y != 0.0f || endOff.z != 0.0f;
                    const unity::Xform* rx = uf.xform(rootId);
                    if (endParticle && rx && rx->parent && uf.xform(rx->parent)) ids.insert(ids.begin(), rx->parent);
                }
                ch.damping = static_cast<float>(mb.fields.at("m_Damping").asFloat(0.1));
                ch.elasticity = static_cast<float>(mb.fields.at("m_Elasticity").asFloat(0.1));
                ch.stiffness = static_cast<float>(mb.fields.at("m_Stiffness").asFloat(0.1));
                ch.inert = static_cast<float>(mb.fields.at("m_Inert").asFloat(0.0));
                ch.radius = static_cast<float>(mb.fields.at("m_Radius").asFloat(0.0));
                ch.gravity = vec3Of(mb.fields.at("m_Gravity"));
                ch.force = vec3Of(mb.fields.at("m_Force"));
            } else {
                ids.push_back(rootId);
                for (const auto& e : mb.fields.at("Bones").asArray())
                    if (int64_t id = pathId(e); id && id != rootId && uf.xform(id)) ids.push_back(id);
                const Value& ptns = mb.fields.at("Patterns");
                const Value& ptn = ptns.at(static_cast<size_t>(std::max<int64_t>(0, mb.fields.at("PtnNo").asInt())));
                const auto& params = ptn.at("Params").asArray();
                for (const auto& p : params) {
                    ch.damping += static_cast<float>(p.at("Damping").asFloat());
                    ch.elasticity += static_cast<float>(p.at("Elasticity").asFloat());
                    ch.stiffness += static_cast<float>(p.at("Stiffness").asFloat());
                    ch.inert += static_cast<float>(p.at("Inert").asFloat());
                    ch.radius += static_cast<float>(p.at("CollisionRadius").asFloat());
                }
                if (!params.empty()) { const float k = 1.0f / static_cast<float>(params.size()); ch.damping *= k; ch.elasticity *= k; ch.stiffness *= k; ch.inert *= k; ch.radius *= k; }
                ch.gravity = vec3Of(ptn.at("Gravity"));
                ch.force = vec3Of(mb.fields.at("Force"));
            }
            for (int64_t id : ids)
                if (int bi = sceneBoneOf(id); bi >= 0 && std::find(ch.bones.begin(), ch.bones.end(), bi) == ch.bones.end()) ch.bones.push_back(bi);
            if (ch.bones.size() < 2) continue;
            // The curves only for hair and accessory chains (a ponytail stiff where it is tied, free at its tip). A garment's chains keep the
            // component's own values all along: with its curves this card's skirt was rigid down to the hem and looked frozen in MMD.
            const bool garment = partId.rfind("outfit", 0) == 0 && partId.find(".acc") == std::string::npos;
            if (classic && !garment) {
                auto curveOf = [&](const char* key) {
                    DbCurve c;
                    for (const auto& k : mb.fields.at(key).at("m_Curve").asArray())
                        c.keys.push_back({static_cast<float>(k.at("time").asFloat()), static_cast<float>(k.at("value").asFloat()),
                                          static_cast<float>(k.at("inSlope").asFloat()), static_cast<float>(k.at("outSlope").asFloat())});
                    return c;
                };
                const DbCurve dc = curveOf("m_DampingDistrib"), ec = curveOf("m_ElasticityDistrib"), sc = curveOf("m_StiffnessDistrib");
                if (!dc.keys.empty() || !ec.keys.empty() || !sc.keys.empty()) {
                    std::vector<float> dist(ch.bones.size(), 0.0f);   // each bone's distance from the chain root along the chain
                    float total = 0.0f;
                    for (size_t k = 1; k < ch.bones.size(); ++k) {
                        const SceneBone& sb = scene.bones[static_cast<size_t>(ch.bones[k])];
                        auto pi = std::find(ch.bones.begin(), ch.bones.end(), sb.parent);
                        const size_t pk = pi == ch.bones.end() ? 0 : static_cast<size_t>(pi - ch.bones.begin());
                        const Vec3 up = scene.bones[static_cast<size_t>(sb.parent >= 0 ? sb.parent : ch.bones[0])].pos;
                        const float dx = sb.pos.x - up.x, dy = sb.pos.y - up.y, dz = sb.pos.z - up.z;
                        dist[k] = dist[pk] + std::sqrt(dx * dx + dy * dy + dz * dz);
                        total = std::max(total, dist[k]);
                    }
                    for (size_t k = 0; k < ch.bones.size(); ++k) {
                        const float t = total > 1e-6f ? dist[k] / total : 0.0f;
                        auto at = [&](float base, const DbCurve& c) { return std::min(1.0f, std::max(0.0f, base * (c.keys.empty() ? 1.0f : c.eval(t)))); };
                        ch.boneDamping.push_back(at(ch.damping, dc));
                        ch.boneElasticity.push_back(at(ch.elasticity, ec));
                        ch.boneStiffness.push_back(at(ch.stiffness, sc));
                    }
                }
            }
            const std::string comment = mb.fields.at("Comment").asStr();
            ch.name = comment.empty() ? scene.bones[static_cast<size_t>(ch.bones[0])].name : comment;
            // A Ver02 component's bone list can be a forest, not a line: the hip jiggle lists both cheeks under one component. One chain
            // per root, so a bone never hangs from another subtree's joint (bridging subtrees across the pelvis yanked the butt inward).
            const std::set<int> inSet(ch.bones.begin(), ch.bones.end());
            std::vector<std::vector<int>> groups;
            std::map<int, size_t> groupOf;
            for (int b : ch.bones) {
                int root = b;
                while (true) {
                    const int p = scene.bones[static_cast<size_t>(root)].parent;
                    if (p >= 0 && p != root && inSet.count(p)) root = p;
                    else break;
                }
                auto it = groupOf.find(root);
                size_t g;
                if (it == groupOf.end()) { g = groups.size(); groupOf[root] = g; groups.push_back({}); }
                else g = it->second;
                groups[g].push_back(b);
            }
            if (groups.size() <= 1) {
                scene.chains.push_back(std::move(ch));
            } else {
                for (auto& g : groups) {
                    if (g.size() < 2) continue;
                    SceneChain sub = ch;
                    sub.bones = std::move(g);
                    scene.chains.push_back(std::move(sub));
                }
            }
        }
    }

    int addRenderer(UnityFile& uf, const RendererInfo& r, int64_t prefabRoot, int part, const std::string& partId,
                    const Mat4& placement, int staticBone, const Vec4* fallbackColor, bool accessoryMode = false,
                    const ItemLook* look = nullptr, const Vec4* hairTintColor = nullptr) {
        std::string why;
        auto mesh = uf.readMesh(r.mesh, &why);
        if (!mesh) {
            scene.warn(partId, "renderer '" + r.name + "' skipped: " + why);
            return -1;
        }
        if (mesh->positions.empty() || mesh->subMeshTriangles.empty()) return -1;
        // The blend shapes themselves (mesh space), kept for the PMX morphs; the resting expression is applied to the vertices below.
        std::vector<std::vector<std::pair<uint32_t, Vec3>>> shapeDeltas(mesh->blendShapes.size());
        std::vector<float> shapeRest(mesh->blendShapes.size(), 0.0f);
        for (size_t s = 0; s < mesh->blendShapes.size(); ++s) {
            const auto& bsh = mesh->blendShapes[s];
            for (size_t i = bsh.first; i < static_cast<size_t>(bsh.first) + bsh.count && i < mesh->blendVertices.size(); ++i)
                if (mesh->blendVertices[i].index < mesh->positions.size()) shapeDeltas[s].emplace_back(mesh->blendVertices[i].index, mesh->blendVertices[i].dPos);
        }
        if (blend) {
            auto bw = blend->find(r.goId);
            if (bw != blend->end())
                for (const auto& [shapeIndex, weight] : bw->second) {
                    mesh->applyBlendShape(static_cast<size_t>(shapeIndex), weight);
                    if (shapeIndex >= 0 && static_cast<size_t>(shapeIndex) < shapeRest.size()) shapeRest[static_cast<size_t>(shapeIndex)] = weight;
                }
        }

        SceneMesh sm;
        sm.name = r.name;
        sm.part = part;
        size_t n = mesh->vertexCount();
        sm.vertices.resize(n);

        Mat4 rendererW = Wp(uf, r.xformId);
        Mat4 rootInv = inverse(W(uf, prefabRoot));
        bool skinned = r.skinned && !r.boneXforms.empty() && !mesh->skin.empty() && mesh->bindPoses.size() >= r.boneXforms.size();
        if (r.skinned && !skinned && !r.boneXforms.empty())
            scene.warn(partId, "renderer '" + r.name + "' has bones but no usable skin data; placed rigidly");

        // Per bone: the PMX bone index and the matrix that takes bind-pose vertices to model space.
        // Stiletto UpdateShoeWarp: foot binds pre-multiply by R(-ankle-leg)*Shear*T*S*R(ankle+leg+
        // shoeAngle) in bind space, so the same warp lands in SceneVertex::local and survives
        // Reshape without double application.
        bool warpOn = heelWarpOn && !ShoeWarpIsIdentity(heelWarp);
        Mat4 warpMat;
        if (warpOn) {
            warpMat = ShoeWarpMatrix(heelWarp);
            for (float f : warpMat.m)
                if (!std::isfinite(f)) { warpOn = false; break; }   // never let NaN/inf reach PMX
        }
        std::vector<int> boneIdx;
        std::vector<Mat4> boneMat;
        if (skinned && accessoryMode) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                int64_t bid = r.boneXforms[i];
                if (!uf.xform(bid)) { boneIdx.push_back(staticBone); boneMat.push_back(placement * (rootInv * rendererW)); continue; }
                const unity::Xform* bx = uf.xform(bid);
                const std::string bname = bx ? bx->name : std::string();
                const bool isFoot = bname == "cf_j_foot_L" || bname == "cf_j_foot_R";
                Mat4 bp = mesh->bindPoses[i];
                if (warpOn && isFoot) bp = warpMat * bp;
                // A garment worn through an accessory slot is bound to the body skeleton: Unity ignores the accessory node's
                // transform for skinned meshes, so the attach anchor must not move it. Only a bone this item creates of its own
                // hangs on the anchor; a bone the body rig already has is the rig's (like the clothing slots' path).
                const int rigBi = bname.empty() ? -1 : scene.boneIndex(bname);
                if (rigBi >= 0) {
                    const auto rw = rig.world.find(bname);
                    const Mat4 bw = rw != rig.world.end() ? rw->second : itemBoneWorld(uf, bid, prefabRoot, placement);
                    boneIdx.push_back(rigBi);
                    boneMat.push_back(bw * bp);
                    continue;
                }
                boneIdx.push_back(accessoryBone(uf, bid, prefabRoot, placement, staticBone, partId));
                boneMat.push_back(placement * (rootInv * Wp(uf, bid)) * bp);
            }
        } else if (skinned) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                const unity::Xform* bx = uf.xform(r.boneXforms[i]);
                std::string bname = bx ? bx->name : std::string();
                int bi = bname.empty() ? -1 : scene.boneIndex(bname);
                auto rw = rig.world.find(bname);
                if (rw == rig.world.end() && bx && bi < 0) {   // a bone only this item has (a mod's necklace joint ...)
                    bi = itemBone(uf, r.boneXforms[i], prefabRoot, placement, partId);
                    rw = rig.world.find(bname);
                }
                const Mat4 bw = rw != rig.world.end() ? rw->second : itemBoneWorld(uf, r.boneXforms[i], prefabRoot, placement);
                if (bi < 0) bi = 0;
                boneIdx.push_back(bi);
                Mat4 bp = mesh->bindPoses[i];
                if (warpOn && (bname == "cf_j_foot_L" || bname == "cf_j_foot_R")) bp = warpMat * bp;
                boneMat.push_back(bw * bp);
            }
        }

        Mat4 staticM = placement * (rootInv * rendererW);
        // Non-skinned geometry follows the node it hangs on, as it does in the game: a hair accessory's rigid pieces stay under N_move,
        // a static child of a DynamicBone chain swings with that chain. Binding every such piece to the attach node instead would leave
        // it behind the moment the chain moved. The item's own node becomes a scene bone, so it also follows the (possibly reshaped) body.
        int staticBind = staticBone;
        if (accessoryMode) {
            const unity::Xform* rx = uf.xform(r.xformId);
            if (rx && rx->parent && uf.xform(rx->parent))
                staticBind = accessoryBone(uf, rx->parent, prefabRoot, placement, staticBone, partId);
        }
        // Inverse rest worlds of the bones that move this mesh: a vertex is also stored in each bone's own frame (SceneVertex::local), so a
        // re-shape that moves the bones moves the vertices too. Taking the frame from the scene's bone makes the identity re-shape exact.
        std::vector<Mat4> invBone(boneIdx.size());
        for (size_t i = 0; i < boneIdx.size(); ++i) invBone[i] = inverse(scene.bones[static_cast<size_t>(boneIdx[i])].world);
        const Mat4 invStatic = inverse(scene.bones[static_cast<size_t>(staticBind)].world);
        sm.reshapable = true;
        for (size_t v = 0; v < n; ++v) {
            SceneVertex& out = sm.vertices[v];
            Vec3 p = mesh->positions[v];
            Vec3 nrm = v < mesh->normals.size() ? mesh->normals[v] : Vec3{0, 1, 0};
            out.uv = v < mesh->uv0.size() ? mesh->uv0[v] : Vec2{};
            out.uv1 = v < mesh->uv1.size() ? mesh->uv1[v] : out.uv;
            out.uv2 = v < mesh->uv2.size() ? mesh->uv2[v] : out.uv;
            out.uv3 = v < mesh->uv3.size() ? mesh->uv3[v] : out.uv;
            bool placed = false;
            if (skinned) {
                const unity::BoneInfluence& bi = mesh->skin[v];
                Vec3 pos{}, nr{};
                float wsum = 0;
                int slot = 0;
                int bones[4] = {0, 0, 0, 0};
                float ws[4] = {0, 0, 0, 0};
                Vec3 lp[4], ln[4];
                for (int k = 0; k < 4; ++k) {
                    if (bi.w[k] <= 0.0f) continue;
                    if (bi.idx[k] < 0 || static_cast<size_t>(bi.idx[k]) >= boneMat.size()) continue;
                    const size_t bk = static_cast<size_t>(bi.idx[k]);
                    const Mat4& m = boneMat[bk];
                    pos = pos + m.point(p) * bi.w[k];
                    nr = nr + m.dir(nrm) * bi.w[k];
                    wsum += bi.w[k];
                    bones[slot] = boneIdx[bk];
                    ws[slot] = bi.w[k];
                    lp[slot] = invBone[bk].point(m.point(p));
                    ln[slot] = invBone[bk].dir(m.dir(nrm));
                    ++slot;
                }
                if (wsum > 1e-6f) {
                    out.pos = pos * (1.0f / wsum);
                    out.normal = normalize(nr);
                    for (int k = 0; k < 4; ++k) { out.bone[k] = bones[k]; out.weight[k] = ws[k] / wsum; out.local[k] = lp[k]; out.nlocal[k] = ln[k]; }
                    placed = true;
                }
            }
            if (!placed) {
                out.pos = staticM.point(p);
                out.normal = normalize(staticM.dir(nrm));
                out.bone[0] = staticBind;
                out.weight[0] = 1.0f;
                out.local[0] = invStatic.point(out.pos);
                out.nlocal[0] = invStatic.dir(out.normal);
            }
        }

        {   // blend shape offsets in model space: the mesh-space delta goes through the same skinning (rotation and scale part) as the vertex
            auto modelDelta = [&](size_t v, Vec3 d, std::array<Vec3, 4>& local) {
                local = {};
                if (skinned) {
                    const unity::BoneInfluence& bi = mesh->skin[v];
                    Vec3 acc{};
                    float wsum = 0;
                    int slot = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (bi.w[k] <= 0.0f || bi.idx[k] < 0 || static_cast<size_t>(bi.idx[k]) >= boneMat.size()) continue;
                        const size_t bk = static_cast<size_t>(bi.idx[k]);
                        const Vec3 md = boneMat[bk].dir(d);
                        acc = acc + md * bi.w[k];
                        wsum += bi.w[k];
                        local[static_cast<size_t>(slot++)] = invBone[bk].dir(md);   // the same slots the vertex keeps
                    }
                    if (wsum > 1e-6f) return acc * (1.0f / wsum);
                    local = {};
                }
                const Vec3 md = staticM.dir(d);
                local[0] = invStatic.dir(md);
                return md;
            };
            for (size_t s = 0; s < shapeDeltas.size(); ++s) {
                if (shapeDeltas[s].empty()) continue;
                SceneBlendShape bsh;
                bsh.name = mesh->blendShapes[s].name.empty() ? "shape" + std::to_string(s) : mesh->blendShapes[s].name;
                bsh.rest = shapeRest[s];
                for (const auto& [vi, d] : shapeDeltas[s]) {
                    std::array<Vec3, 4> local;
                    bsh.offsets.emplace_back(vi, modelDelta(vi, d, local));
                    bsh.local.push_back(local);
                }
                sm.shapes.resize(s + 1);   // keeps the unity shape index as the position
                sm.shapes[s] = std::move(bsh);
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
                ss.material = materialFor(uf, r.materials[mi], partId, fallbackColor, look, r, mi, hairTintColor);
            }
            sm.submeshes.push_back(std::move(ss));
        }
        scene.meshes.push_back(std::move(sm));
        goMesh[r.goId] = static_cast<int>(scene.meshes.size()) - 1;
        return static_cast<int>(scene.meshes.size()) - 1;
    }
};

// True when the path ("/a/b/") runs through a clothes-state node other than the worn one: n_top_b, n_bot_c, n_panst_b ...
bool hiddenClothesState(const std::string& path) {
    for (const char* kind : {"/n_top_", "/n_bot_", "/n_panst_"}) {
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

// Heeled footwear (see Stiletto "HeelInfo" in the game): shoes are modelled for a flat foot
// (level sole, heel stack hanging below it), but when worn the foot points down at the ankle
// and the whole character is lifted by the heel height, so the toes and the heel stack stand
// on the same floor and the stack's own forward lean stands upright. This bakes that pose into
// the rest pose: the foot (cf_j_foot) is pitched about its own origin by the stack's lean angle
// and the toes bend back by the same angle at the ball, so the forefoot stays flat on the floor
// (closed heels stand on the ball, not on pointed toes), verts weighted to the leg bones stay
// with the leg, and the character is lifted onto y = 0. The angle and the height come from the shoe's own geometry
// (the retargeting bind keeps the source weights: rear heel/stack verts are 100% cf_j_foot,
// forefoot verts are cf_j_toes, shaft verts are cf_s_leg02/03), so flat shoes (level sole,
// upright stack, heel below 4 mm) are left alone. CARDBRIDGE_NO_HEEL_POSE=1 disables it for
// before/after comparison.
void settleHeels(Scene& scene) {
    if (std::getenv("CARDBRIDGE_NO_HEEL_POSE")) return;
    const bool dbg = std::getenv("CARDBRIDGE_DEBUG_HEELS") != nullptr;
    if (dbg) std::fprintf(stderr, "settleHeels: bones=%zu parts=%zu meshes=%zu\n", scene.bones.size(), scene.parts.size(), scene.meshes.size());
    // A re-shape (viewer slider drag, CLI --hide re-pose) re-derives the pitch from the flat
    // locals: drop the previous heel-pose notes so the manifest states the current pose only.
    scene.issues.erase(std::remove_if(scene.issues.begin(), scene.issues.end(), [](const SceneIssue& is) {
                           return is.part.empty() && is.message.find("posed for a") != std::string::npos && is.message.find("heel") != std::string::npos;
                       }), scene.issues.end());
    bool posed = false;
    for (const char* side : {"L", "R"}) {
        int foot = scene.boneIndex(std::string("cf_j_foot_") + side);
        int toes = scene.boneIndex(std::string("cf_j_toes_") + side);
        if (foot < 0 || toes < 0) continue;
        const int other = scene.boneIndex(std::string("cf_j_foot_") + (side[0] == 'L' ? "R" : "L"));
        std::vector<char> set(scene.bones.size(), 0);   // 1 = under the foot, 2 = under the toes
        set[static_cast<size_t>(foot)] = 1;
        set[static_cast<size_t>(toes)] = 2;
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < scene.bones.size(); ++i) {
                int par = scene.bones[i].parent;
                if (!set[i] && par >= 0 && set[static_cast<size_t>(par)]) { set[i] = set[static_cast<size_t>(par)]; changed = true; }
            }
        }
        const Vec3 ankle = scene.bones[static_cast<size_t>(foot)].pos;
        const Vec3 ball = scene.bones[static_cast<size_t>(toes)].pos;
        const Vec3 otherAnkle = other >= 0 ? scene.bones[static_cast<size_t>(other)].pos : Vec3{1000.0f, 1000.0f, 1000.0f};
        auto weights = [&](const SceneVertex& v, float& wFoot, float& wToes) {
            wFoot = wToes = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.weight[k] <= 0 || v.bone[k] < 0) continue;
                char c = set[static_cast<size_t>(v.bone[k])];
                if (c == 1) wFoot += v.weight[k];
                else if (c == 2) wToes += v.weight[k];
            }
        };
        auto sideOf = [&](Vec3 p) {
            float d0 = (p.x - ankle.x) * (p.x - ankle.x) + (p.z - ankle.z) * (p.z - ankle.z);
            float d1 = (p.x - otherAnkle.x) * (p.x - otherAnkle.x) + (p.z - otherAnkle.z) * (p.z - otherAnkle.z);
            return d0 <= d1;
        };
        auto rot = [](Vec3 p, Vec3 c, float ang) {   // about the X axis through c; positive points +Z (the toes) down
            float cs = std::cos(ang), sn = std::sin(ang), dy = p.y - c.y, dz = p.z - c.z;
            return Vec3{p.x, c.y + dy * cs - dz * sn, c.z + dz * cs + dy * sn};
        };
        auto rotDir = [](Vec3 d, float ang) {
            float cs = std::cos(ang), sn = std::sin(ang);
            return Vec3{d.x, d.y * cs - d.z * sn, d.z * cs + d.y * sn};
        };
        // Stiletto chain (HeelInfoAnimation): leg about cf_j_leg03, ankle about cf_j_foot,
        // toe about cf_j_toes, each as a local X-rotation on top of its parent (hierarchy).
        // Baked here as rest-pose rotations about the rest pivots: leg first, then ankle
        // about the moved foot pivot, then toe about the moved ball. IKCorrect/FrameCorrect
        // do not touch cf_s/cf_d/cf_j leg bones (Assembly-CSharp verified), so cf_s_leg02/03
        // stay with the leg by hierarchy only - no PMX append is added (it would deviate).
        int leg03 = scene.boneIndex(std::string("cf_j_leg03_") + side);
        const Vec3 leg03Pos = leg03 >= 0 ? scene.bones[static_cast<size_t>(leg03)].pos : ankle;
        auto legM = [&](Vec3 p, float legRad) { return rot(p, leg03Pos, legRad); };
        auto footM = [&](Vec3 p, float legRad, float ankleRad) {
            return legM(rot(p, ankle, ankleRad), legRad);
        };
        auto toesM = [&](Vec3 p, float legRad, float ankleRad, float toeRad) {
            return legM(rot(rot(p, ball, toeRad), ankle, ankleRad), legRad);
        };
        auto weights3 = [&](const SceneVertex& v, float& wLeg, float& wFoot, float& wToes) {
            wLeg = wFoot = wToes = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.weight[k] <= 0 || v.bone[k] < 0) continue;
                char c = set[static_cast<size_t>(v.bone[k])];
                if (c == 1) wFoot += v.weight[k];
                else if (c == 2) wToes += v.weight[k];
                else if (v.bone[k] == leg03) wLeg += v.weight[k];   // cf_j_leg03 joint itself (cf_s_leg03 stays with the leg)
            }
        };
        auto blend3 = [&](const SceneVertex& v, Vec3 p, float legRad, float ankleRad, float toeRad) {
            float wl, wf, wt;
            weights3(v, wl, wf, wt);
            Vec3 l = legM(p, legRad), f = footM(p, legRad, ankleRad), t = toesM(p, legRad, ankleRad, toeRad);
            float wr = 1.0f - wl - wf - wt;
            return Vec3{p.x * wr + l.x * wl + f.x * wf + t.x * wt, p.y * wr + l.y * wl + f.y * wf + t.y * wt, p.z * wr + l.z * wl + f.z * wf + t.z * wt};
        };

        // Inner and outer shoes differ: evaluate each shoe part on its own and pose for the
        // heeled one(s) only, so a flat shoe (e.g. an uwabaki worn under a heeled outer) never
        // triggers or dilutes the pose, and a changed shoe (new outfit, hidden heeled part)
        // re-evaluates at once. The foot carries a single pitch, so when both slots are heeled
        // the taller one wins (the worn, outer shoe is taller in practice); a flat slot is
        // ignored. Hidden or excluded parts are skipped (the viewer's hide/show updates
        // visibleInPreview, so hiding the heeled shoe goes flat again on the next re-shape).
        // Priority (per part): Stiletto config (zipmod txt, known table) > geometry > flat.
        struct HeelCandidate {
            float height = 0;   // metres
            float ankle = 0, leg = 0, toe = 0;   // radians
            std::string partId;
            std::string from;  // "zipmod ..." / "known ..." / "geometry"
            int verts = 0;     // geometry confidence: shoe verts counted (0 for config)
            bool wedge = false;   // geometry: insole-slope path taken (diagonal strut)
            bool contact = false;   // geometry: sole-contact pitch solved (heel/toe flush)
        };
        HeelCandidate chosen;
        for (const auto& part : scene.parts) {
            if (dbg && part.id.find(".shoes") != std::string::npos)
                std::fprintf(stderr, "  side %s part %s include=%d visible=%d group=%s hasCfg=%d listName=[%s] meshes=%zu\n", side, part.id.c_str(), (int)part.includeInExport, (int)part.visibleInPreview, part.group.c_str(), (int)part.hasHeelConfig, part.listName.c_str(), part.meshes.size());
            if (!part.includeInExport || !part.visibleInPreview || part.group != "Outfit" || part.id.find(".shoes") == std::string::npos) continue;
            HeelCandidate cand;
            cand.partId = part.id;
            if (part.hasHeelConfig) {
                cand.height = part.heelHeightMeters;
                cand.ankle = part.heelAnkleDeg * 3.14159265f / 180.0f;
                cand.leg = part.heelLegDeg * 3.14159265f / 180.0f;
                cand.toe = part.heelToeDeg * 3.14159265f / 180.0f;
                cand.from = part.heelConfigSource;
                if (!std::isfinite(cand.height) || !std::isfinite(cand.ankle) || !std::isfinite(cand.leg) || !std::isfinite(cand.toe)) continue;
                // Height-only platforms (Geta 0/0/30mm): lift without pitching.
                bool heeled = cand.height >= 0.004f || std::fabs(cand.ankle) > 1e-6f || std::fabs(cand.leg) > 1e-6f || std::fabs(cand.toe) > 1e-6f;
                if (!heeled) continue;   // explicitly flat in config (most base shoes)
            } else {
                std::vector<const SceneVertex*> shoe;
                float maxZ = -1e30f;
                for (int mi : part.meshes)
                    for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                        float wf, wt;
                        weights(v, wf, wt);
                        if (wf + wt > 0.5f && sideOf(v.pos)) { shoe.push_back(&v); maxZ = std::max(maxZ, v.pos.z - ankle.z); }
                    }
                if (shoe.size() < 50 || maxZ <= 0) continue;
                float toeLow0 = 1e30f, heelLow0 = 1e30f;
                const SceneVertex* toeLowVert = nullptr;
                const SceneVertex* heelLowVert = nullptr;
                for (const SceneVertex* v : shoe) {
                    float z = v->pos.z - ankle.z;
                    // Heel contact is the shoe's lowest point overall (the heel post under the
                    // arch, not the rear-most sole edge): a stiletto tip sits ~0.3-0.5 maxZ
                    // forward of the ankle, so a rear-only cut misses it and under-reads the heel.
                    if (v->pos.y < heelLow0) { heelLow0 = v->pos.y; heelLowVert = v; }
                    if (z > 0.6f * maxZ) {
                        if (v->pos.y < toeLow0) { toeLow0 = v->pos.y; toeLowVert = v; }
                    }
                }
                float height = toeLow0 - heelLow0;
                if (dbg)
                    std::fprintf(stderr, "    geom side %s part %s verts=%zu maxZ=%.4f toeLow=%.4f heelLow=%.4f height=%.4f ankle=(%.3f,%.3f,%.3f) ball=(%.3f,%.3f,%.3f)\n",
                                 side, part.id.c_str(), shoe.size(), maxZ, toeLow0, heelLow0, height, ankle.x, ankle.y, ankle.z, ball.x, ball.y, ball.z);
                if (!std::isfinite(height) || height < 0.002f) continue;   // no heel (2 mm: below sole noise)
                const float soleY = toeLow0;
                // The angle: the shoe is modelled for the pose it is worn in, so its heel stack
                // leans forward by the foot's pointing angle. Pointing the foot by that much stands
                // the stack upright, which is what a worn heel does.
                float topZ = 0, topY = 0, botZ = 0, botY = 0;
                int topN = 0, botN = 0;
                for (const SceneVertex* v : shoe) {
                    if (v->pos.z - ankle.z > 0.4f * maxZ || v->pos.y > soleY - 0.003f) continue;   // the stack: below the sole, behind the arch
                    if (v->pos.y > soleY - 0.015f) { topZ += v->pos.z; topY += v->pos.y; ++topN; }
                    else if (v->pos.y < heelLow0 + 0.01f) { botZ += v->pos.z; botY += v->pos.y; ++botN; }
                }
                // Pitch that puts the sole flush with the ground plane (both contact points at the
                // same height after the pose, before the global lift). The heel-stack lean alone
                // is not this angle for flat-modelled shoes: a vertical stack has lean ~0 but still
                // needs ~15-25deg of foot pitch to bring its heel block to the toe's level, and a
                // slanted stiletto over-reads to 50deg. So the pitch is solved from the contact
                // points themselves (heel lowest vs toe lowest, through the same leg/ankle/toe
                // blend the pose uses). The stack lean / insole slope below stay as a fallback when
                // a contact point is missing.
                float best = 0;
                bool wedge = false;
                bool contact = false;
                float contactTheta = 0;
                if (heelLowVert && toeLowVert) {
                    float bestGap = 1e30f;
                    for (int deg10 = 0; deg10 <= 550; ++deg10) {
                        float th = deg10 * 0.1f * 3.14159265f / 180.0f;
                        Vec3 hy = blend3(*heelLowVert, heelLowVert->pos, 0, th, -th);
                        Vec3 ty = blend3(*toeLowVert, toeLowVert->pos, 0, th, -th);
                        float gap = std::fabs(ty.y - hy.y);
                        if (gap < bestGap) { bestGap = gap; contactTheta = th; }
                    }
                    // Refine at 0.01deg around the coarse best (contact must be sub-mm to look closed).
                    float c = contactTheta;
                    for (int k = -20; k <= 20; ++k) {
                        float th = c + k * 0.01f * 3.14159265f / 180.0f;
                        if (th < 0) continue;
                        Vec3 hy = blend3(*heelLowVert, heelLowVert->pos, 0, th, -th);
                        Vec3 ty = blend3(*toeLowVert, toeLowVert->pos, 0, th, -th);
                        float gap = std::fabs(ty.y - hy.y);
                        if (gap < bestGap) { bestGap = gap; contactTheta = th; }
                    }
                    if (dbg)
                        std::fprintf(stderr, "    geom side %s part %s contact theta=%.2f gap=%.4f\n",
                                     side, part.id.c_str(), contactTheta * 180.0f / 3.14159265f, bestGap);
                    if (bestGap < 0.006f && contactTheta > 1e-6f) { best = contactTheta; contact = true; }
                }
                if (!contact) {
                    if (topN > 3 && botN > 3) {
                        float dz = botZ / botN - topZ / topN, dy = topY / topN - botY / botN;   // forward lean, drop
                        if (dy > 0.005f) best = std::min(std::atan2(dz, dy), 50.0f * 3.14159265f / 180.0f);
                    }
                    // Wedges and platforms: the heel strut is diagonal by design, so its lean is not the
                    // pitch (it would read as 50 degrees for a wedge). Their pitch is the insole slope
                    // instead: the footbed just above the outsole, rear higher than front by the heel
                    // height. Use it when the stack lean is implausible for an ankle (>35 degrees).
                    if (best > 35.0f * 3.14159265f / 180.0f) {
                        float fY = 0, fZ = 0, rY = 0, rZ = 0;
                        int fN = 0, rN = 0;
                        for (const SceneVertex* v : shoe) {
                            float z = v->pos.z - ankle.z;
                            if (v->pos.y < soleY - 0.001f || v->pos.y > soleY + 0.05f) continue;   // just above the outsole: the insole
                            if (z > 0.5f * maxZ) { fY += v->pos.y; fZ += v->pos.z; ++fN; }
                            else if (z < 0.3f * maxZ) { rY += v->pos.y; rZ += v->pos.z; ++rN; }
                        }
                        if (fN > 10 && rN > 10) {
                            float dz = fZ / fN - rZ / rN, dy = rY / rN - fY / fN;
                            if (dz > 0.02f && dy > 0.0f) {
                                float slope = std::atan2(dy, dz);
                                if (slope > 2.0f * 3.14159265f / 180.0f && slope < 35.0f * 3.14159265f / 180.0f) { best = slope; wedge = true; }
                            }
                        }
                    }
                }
                if (dbg)
                    std::fprintf(stderr, "    geom side %s part %s stack topN=%d botN=%d best=%.2f wedge=%d\n",
                                 side, part.id.c_str(), topN, botN, best * 180.0f / 3.14159265f, (int)wedge);
                if (!std::isfinite(best) || best < 2.0f * 3.14159265f / 180.0f) continue;   // the sole already stands flat
                cand.height = height;
                cand.ankle = best;
                cand.leg = 0.0f;
                cand.toe = -best;   // geometry fallback: forefoot flat (Stiletto default toe is 0 when data exists)
                cand.from = "geometry";
                cand.verts = static_cast<int>(shoe.size());
                cand.wedge = wedge;
                cand.contact = contact;
            }
            if (cand.height > chosen.height) chosen = cand;
        }
        if (chosen.height == 0) continue;   // no heeled shoe on this side
        float legRad = chosen.leg, ankleRad = chosen.ankle, toeRad = chosen.toe;
        for (auto& mesh : scene.meshes)
            for (auto& v : mesh.vertices) {
                float wl, wf, wt;
                weights3(v, wl, wf, wt);
                if (wl + wf + wt <= 0) continue;
                Vec3 np = blend3(v, v.pos, legRad, ankleRad, toeRad);
                SceneVertex nv = v;
                nv.pos = {v.pos.x + v.normal.x, v.pos.y + v.normal.y, v.pos.z + v.normal.z};
                Vec3 tip = blend3(nv, nv.pos, legRad, ankleRad, toeRad);
                Vec3 base = blend3(v, v.pos, legRad, ankleRad, toeRad);
                Vec3 n{tip.x - base.x, tip.y - base.y, tip.z - base.z};
                float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (len > 1e-6f) v.normal = {n.x / len, n.y / len, n.z / len};
                v.pos = np;
            }
        for (size_t i = 0; i < scene.bones.size(); ++i) {
            if (static_cast<int>(i) == foot) continue;   // pivot: the origin itself stays (rotation about it)
            if (set[i] == 1) scene.bones[i].pos = footM(scene.bones[i].pos, legRad, ankleRad);
            else if (set[i] == 2) scene.bones[i].pos = toesM(scene.bones[i].pos, legRad, ankleRad, toeRad);
            else if (static_cast<int>(i) == leg03 && std::fabs(legRad) > 1e-9f) scene.bones[i].pos = legM(scene.bones[i].pos, legRad);
        }
        // cf_j_foot itself moves with the leg rotation (its pivot for the ankle is stationary).
        if (std::fabs(legRad) > 1e-9f) scene.bones[static_cast<size_t>(foot)].pos = legM(ankle, legRad);
        for (auto& c : scene.colliders) {
            if (c.bone < 0 || static_cast<size_t>(c.bone) >= set.size()) continue;
            char s = set[static_cast<size_t>(c.bone)];
            float total = 0;
            if (s == 1) { c.center = footM(c.center, legRad, ankleRad); total = legRad + ankleRad; }
            else if (s == 2) { c.center = toesM(c.center, legRad, ankleRad, toeRad); total = legRad + ankleRad + toeRad; }
            else if (c.bone == leg03 && std::fabs(legRad) > 1e-9f) { c.center = legM(c.center, legRad); total = legRad; }
            else continue;
            c.axis = normalize(rotDir(c.axis, total));
        }
        // Shoe warp is baked in addRenderer (bind space) before this pose; note it here for provenance.
        {
            const ScenePart* src = nullptr;
            for (const auto& p : scene.parts) if (p.id == chosen.partId) { src = &p; break; }
            if (src && src->hasHeelConfig) {
                bool warped = src->heelShoeScaleX != 1 || src->heelShoeScaleY != 1 || src->heelShoeScaleZ != 1 ||
                              src->heelShoeAngleDeg != 0 || src->heelShoeTranslateY != 0 || src->heelShoeTranslateZ != 0 ||
                              src->heelShoeShearY != 0 || src->heelShoeShearZ != 0;
                if (warped)
                    scene.warn("", "foot " + std::string(side) + " shoe warp baked from " + src->heelConfigSource +
                                    " (scale/angle/translate/shear in bind space)");
            }
        }
        char buf[340];
        std::snprintf(buf, sizeof buf, "foot %s posed for a %.1f cm heel from %s (%s): leg %.1f, ankle %.1f, toe %.1f degrees", side,
                      chosen.height * 100.0f, chosen.partId.c_str(), chosen.from.c_str(),
                      legRad * 180.0f / 3.14159265f, ankleRad * 180.0f / 3.14159265f, toeRad * 180.0f / 3.14159265f);
        std::string msg = buf;
        if (chosen.from == "geometry") {
            msg += ", geometry from " + std::to_string(chosen.verts) + " shoe verts" +
                   (chosen.contact ? ", sole contact" : chosen.wedge ? ", wedge insole slope" : ", heel-stack lean");
        }
        scene.warn("", msg);
        posed = true;
    }
    if (!posed) return;
    if (dbg) {
        float lo2 = 1e30f;
        for (const auto& mesh : scene.meshes) for (const auto& v : mesh.vertices) lo2 = std::min(lo2, v.pos.y);
        std::fprintf(stderr, "  settleHeels: lowest before lift = %.4f\n", lo2);
    }
    float lowest = 1e30f;
    for (const auto& mesh : scene.meshes)
        for (const auto& v : mesh.vertices) lowest = std::min(lowest, v.pos.y);
    if (lowest < 1e29f && std::fabs(lowest) > 1e-5f) {
        for (auto& mesh : scene.meshes)
            for (auto& v : mesh.vertices) v.pos.y -= lowest;
        for (auto& b : scene.bones) b.pos.y -= lowest;
        for (auto& c : scene.colliders) c.center.y -= lowest;
    }
}


} // namespace

void SettleHeels(Scene& scene) { settleHeels(scene); }

bool CreateRunsInLinearLight(const std::string& materialName) {
    return materialName == "cf_m_clothesN_create" || materialName == "cf_m_eye_create" || materialName == "cf_m_eyewhite_create";
}

Mat4 PrefabRootPlacement(const Mat4& parent, Vec3 position, Quat rotation, Vec3 scale) {
    return parent * Mat4::TRS(position, rotation, scale);
}

Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt) {
    Scene scene;
    scene.name = card.displayName();
    auto log = [&](const std::string& s) { if (opt.log) opt.log(s); };
    auto report = [&](double frac, const std::string& stage) { if (opt.progress) opt.progress(frac, stage); };
    auto checkCancel = [&]() { if (opt.cancelled && opt.cancelled()) throw Cancelled{}; };
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
    if (const char* ov = std::getenv("CARDBRIDGE_GLOBALS")) {   // test aid: "name=x,y,z,w;name=..." replaces shader globals
        std::string spec = ov;
        for (size_t at = 0; at < spec.size();) {
            size_t end = spec.find(';', at);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(at, end - at);
            at = end + 1;
            size_t eq = item.find('=');
            float v[4] = {0, 0, 0, 0};
            if (eq == std::string::npos || std::sscanf(item.c_str() + eq + 1, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) < 1) continue;
            for (auto& g : scene.globalProps) if (g.first == item.substr(0, eq)) g.second = Vec4{v[0], v[1], v[2], v[3]};
        }
    }
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
        scene.textures[static_cast<size_t>(idx)].srgb = t->colorSpace == 1;
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
    cbtime::Scope skeletonTimer("skeleton + head + body");
    log("loading the shared skeleton and body");
    report(0.05, "skeleton and body");
    std::string why;
    auto base = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
    if (!base) {
        scene.error("", "the library has no chara/oo_base.unity3d (" + why + ") - cannot build a skeleton");
        return scene;
    }
    Overrides shapeOv;
    if (opt.applyShape) shapeOv = bodyShapeOverrides(card, resolver, *base, *base, opt.bodyRigPrefab, scene, opt);
    const card::AbmxTable abmx = std::getenv("CARDBRIDGE_NO_ABMX") ? card::AbmxTable{} : card::LoadAbmx(card.extended);
    applyAbmx(shapeOv, *base, opt.bodyRigPrefab, abmx, outfit);
    if (!addRigPrefab(scene, rig, *base, opt.bodyRigPrefab, Mat4{}, -1, "rig", shapeOv.empty() ? nullptr : &shapeOv)) {
        scene.error("", "prefab '" + opt.bodyRigPrefab + "' not found in oo_base.unity3d");
        return scene;
    }
    {   // the colliders every DynamicBone of the character is given (ChaControl.LoadCharaFbxData: the body bone prefab's DynamicBoneColliders)
        std::map<int64_t, std::string> goName;
        for (const auto& [id, x] : base->xforms()) goName[x.goId] = x.name;
        const int64_t bodyRigRoot = base->findPrefabRoot(opt.bodyRigPrefab);
        if (bodyRigRoot)
            for (const auto& mb : base->monoBehavioursUnder(bodyRigRoot)) {
                if (mb.className != "DynamicBoneCollider") continue;
                auto nm = goName.find(mb.goId);
                if (nm == goName.end()) continue;
                auto w = rig.world.find(nm->second);
                if (w == rig.world.end()) continue;
                DbCollider c;
                c.center = {static_cast<float>(mb.fields.at("m_Center").at("x").asFloat()), static_cast<float>(mb.fields.at("m_Center").at("y").asFloat()), static_cast<float>(mb.fields.at("m_Center").at("z").asFloat())};
                c.radius = static_cast<float>(mb.fields.at("m_Radius").asFloat(0.5));
                c.height = static_cast<float>(mb.fields.at("m_Height").asFloat());
                c.direction = static_cast<int>(mb.fields.at("m_Direction").asInt());
                c.inside = mb.fields.at("m_Bound").asInt() == 1;
                c.world = w->second;
                b.dbColliders.push_back(c);
                if (mb.fields.at("m_Enabled").asInt(1) != 0) {   // the PMX carries it as a kinematic rigid body on its bone
                    SceneCollider sc;
                    sc.name = nm->second;
                    sc.bone = scene.boneIndex(nm->second);
                    sc.center = c.world.point(c.center);
                    const Vec3 axisLocal{c.direction == 0 ? 1.0f : 0.0f, c.direction == 1 ? 1.0f : 0.0f, c.direction == 2 ? 1.0f : 0.0f};
                    sc.axis = normalize(c.world.dir(axisLocal));
                    const float wscale = std::sqrt(c.world.m[2] * c.world.m[2] + c.world.m[6] * c.world.m[6] + c.world.m[10] * c.world.m[10]);
                    sc.radius = c.radius * wscale;
                    sc.height = c.height * wscale;
                    sc.inside = c.inside;
                    if (sc.bone >= 0) scene.colliders.push_back(sc);
                }
                if (std::getenv("CARDBRIDGE_DEBUG_DB")) {
                    Vec3 cw = c.world.point(c.center);
                    float sc = std::sqrt(c.world.m[2] * c.world.m[2] + c.world.m[6] * c.world.m[6] + c.world.m[10] * c.world.m[10]);
                    std::fprintf(stderr, "dbcollider enabled=%lld %s center %.3f %.3f %.3f r %.3f h %.3f dir %d inside %d\n", static_cast<long long>(mb.fields.at("m_Enabled").asInt(-1)), nm->second.c_str(), cw.x, cw.y, cw.z, c.radius * sc, c.height * sc, c.direction, c.inside ? 1 : 0);
                }
            }
        if (std::getenv("CARDBRIDGE_DEBUG_DB")) std::fprintf(stderr, "dbcolliders %zu\n", b.dbColliders.size());
        if (bodyRigRoot) b.addChains(*base, bodyRigRoot, "rig", false);   // the body's own jiggle chains
    }
    int headAttach = scene.boneIndex(opt.headAttachBone);
    Mat4 headBase;
    if (headAttach >= 0) {
        auto hw = rig.world.find(opt.headAttachBone);
        if (hw != rig.world.end()) headBase = hw->second;
    } else {
        scene.warn("", "bone '" + opt.headAttachBone + "' not found; the head rig is placed at the origin");
    }
    // The head model is parented to the head rig prefab's root (objHeadBone). Keep that root's own transform, not only the
    // cf_s_head world it hangs on (the base rig's root is identity, a replacement rig's need not be).
    Mat4 headRoot = headBase;
    if (const int64_t headRigRoot = base->findPrefabRoot(opt.headRigPrefab); headRigRoot)
        if (const unity::Xform* hx = base->xform(headRigRoot))
            headRoot = PrefabRootPlacement(headBase, hx->pos, hx->rot, hx->scl);
    Overrides headOv, attachOv;
    {
        resolve::ItemRef hr;
        for (const auto& it : resolve::ItemsFromCard(card, outfit))
            if (it.group == "Head") hr = it;
        if (hr.group == "Head") {
            resolve::ResolvedItem hi = resolver.resolve(card, hr);
            if (hi.ok()) {
                std::string hwhy;
                auto hb = resolver.openBundle(hi, &hwhy);
                if (hb) {
                    if (opt.applyShape) headOv = headShapeOverrides(card, resolver, hi, *hb, *base, opt.headRigPrefab, headRoot, scene, opt);
                    headAttachOverrides(attachOv, *hb, hi.assetName());
                    for (const auto& [name, o] : attachOv) headOv[name] = o;   // not slider-driven: the head model's own placement wins
                }
            }
        }
    }
    Rig rigDefault;   // the same skeleton as the game first built it, before sliders and bone modifiers moved anything
    {
        Scene scratch;
        Overrides none;
        addRigPrefab(scratch, rigDefault, *base, opt.bodyRigPrefab, Mat4{}, -1, "rig", nullptr);
        Mat4 headBase0;
        auto hw0 = rigDefault.world.find(opt.headAttachBone);
        if (hw0 != rigDefault.world.end()) headBase0 = hw0->second;
        addRigPrefab(scratch, rigDefault, *base, opt.headRigPrefab, headBase0, -1, "rig", attachOv.empty() ? nullptr : &attachOv);
        b.rigDefault = &rigDefault;
    }
    applyAbmx(headOv, *base, opt.headRigPrefab, abmx, outfit);
    if (!addRigPrefab(scene, rig, *base, opt.headRigPrefab, headBase, headAttach, "rig", headOv.empty() ? nullptr : &headOv))
        scene.warn("", "prefab '" + opt.headRigPrefab + "' not found in oo_base.unity3d");

    auto vec4Of = [](const Value& v) {
        return Vec4{static_cast<float>(v.at(size_t(0)).asFloat()), static_cast<float>(v.at(size_t(1)).asFloat()),
                    static_cast<float>(v.at(size_t(2)).asFloat()), static_cast<float>(v.size() > 3 ? v.at(size_t(3)).asFloat() : 1.0)};
    };
    // A texture a list row names for a card id (base or mod), decoded; null for "none" or when missing.
    auto rowTextureFor = [&](const std::string& partId, int category, int64_t id, const std::string& property, const char* texKey, const std::string& what,
                          std::string* keyOut = nullptr, const char* abKey = "MainAB") -> std::shared_ptr<unity::TextureData> {
        resolve::ItemRef fr;
        fr.partId = partId + "." + what;
        fr.group = "Face";
        fr.property = property;
        fr.category = category;
        fr.id = id;
        resolve::ResolvedItem fi = resolver.resolve(card, fr);
        if (fi.status == resolve::ItemStatus::Empty) {
            if (category >= 406 && category <= 414)
                scene.warn(partId, what + ": face item id " + std::to_string(id) + " has no row in the lists (library missing or a mod row that no longer exists); the item is left out");
            return nullptr;
        }
        // The pupil is the one face item whose colour lives entirely in a texture: flag the mods that ship a blank one
        // (a white eyeball, an empty png), so the issues tab says why the eyes render like that instead of looking broken.
        auto blankPupil = [&](const std::shared_ptr<unity::TextureData>& t, const std::string& texName) {
            if (!t || category != 408 || t->rgba.empty()) return;
            const unity::FillSummary fs = unity::AnalyzeFill(t->rgba.data(), t->width, t->height);
            const std::string where = fi.modName.empty() ? ("the base lists (" + fi.listItem.get("Name") + ", row " + std::to_string(id) + ")")
                                                         : ("mod " + fi.modName + (fi.modGuid.empty() ? "" : " (" + fi.modGuid + ")") + ", row " + std::to_string(id) + " '" + fi.listItem.get("Name") + "'");
            if (fs.opaqueFraction == 0.0f) {
                scene.warn(partId, what + ": iris texture '" + texName + "' from " + where + " is fully transparent — the eyes render with no pupil at all");
                return;
            }
            if (fs.achromatic >= 0.55f && fs.value >= 0.59f) {   // base game's flat-white skin (0.66 achromatic); every shipped hitomi <= 0.31
                scene.warn(partId, what + ": iris texture '" + texName + "' from " + where + " is achromatic (a white/grey fill with no iris colour) — the eyes render washed out (the card's picture likely used a coloured iris; pick another in the maker or restore a coloured mod)");
            }
        };
        if (!fi.ok()) {
            scene.warn(partId, what + ": " + (fi.detail.empty() ? "no texture item for this id" : fi.detail));
            return nullptr;
        }
        std::string texName = fi.listItem.get(texKey), ab = fi.listItem.get(abKey);
        if (std::getenv("CARDBRIDGE_DEBUG_DB") && category == 423)
            std::fprintf(stderr, "nip-row %s id=%lld texKey=%s texName=%s ab=%s\n", partId.c_str(), static_cast<long long>(id), texKey, texName.c_str(), ab.c_str());
        if (texName.empty() || texName == "0" || ab.empty() || ab == "0") return nullptr;   // "none"
        std::string w;
        auto tb = resolver.openItemBundle(fi, ab, &w);
        if (!tb) {   // a mod can ship the texture as a loose image instead of a bundle (Sideloader)
            if (auto loose = resolver.openLooseTexture(fi, ab, texName, nullptr)) { if (keyOut) *keyOut = "loose:" + ab + "/" + texName; blankPupil(loose, texName); return loose; }
            scene.warn(partId, what + ": texture bundle not loaded: " + w);
            return nullptr;
        }
        PPtr tp = tb->findAsset(texName);
        if (tp.isNull()) tp = tb->findTexture(texName);
        if (tp.isNull()) {   // not in the bundle of that name: a mod may ship the image loose next to where the bundle would be
            if (auto loose = resolver.openLooseTexture(fi, ab, texName, nullptr)) { if (keyOut) *keyOut = "loose:" + ab + "/" + texName; blankPupil(loose, texName); return loose; }
            scene.warn(partId, what + ": texture '" + texName + "' not found in " + ab);
            return nullptr;
        }
        if (keyOut) *keyOut = tb->name() + "#" + std::to_string(tp.pathId);
        auto tex = b.decode(*tb, tp, partId);
        blankPupil(tex, texName);
        return tex;
    };

    // A texture the game builds by drawing a quad through a "create" material of mm_base (CustomTextureCreate): queues the job
    // (run by export::RunCreateJobs at the end of assembly) and returns the placeholder texture that will hold the result.
    using TexInput = std::pair<std::string, std::shared_ptr<unity::TextureData>>;
    // Keyed by raw pointer, so the cache must keep the TextureData alive: after the decoded-texture LRU evicts an
    // entry, a new texture can be allocated at the freed address and would hit a stale entry (e.g. the maid apron's
    // colour mask reusing the shoes mask's scene texture, tinting it wrong and flipping between runs).
    std::map<const unity::TextureData*, std::pair<int, std::shared_ptr<unity::TextureData>>> inputTexCache;
    auto createJobFor = [&](const std::string& partId, const char* matName, const std::vector<TexInput>& texs, const std::vector<std::pair<std::string, Vec4>>& props, int size) -> int {
        std::string w;
        auto mm = resolver.openBaseBundle("chara/mm_base.unity3d", &w);
        if (!mm) { scene.warn(partId, std::string("create material ") + matName + ": mm_base.unity3d not in the library (" + w + ")"); return -1; }
        PPtr mp = mm->findAsset(matName);
        if (mp.isNull()) { scene.warn(partId, std::string("create material ") + matName + " not found in mm_base.unity3d"); return -1; }
        std::vector<std::pair<std::string, Vec4>> savedRuntime = std::move(b.runtime);
        b.runtime.clear();
        // the create material is the game's own, not the renderer being built: the eye's highlight textures and placement belong to the renderer
        auto savedExtra = std::move(b.extraTex);
        auto savedSt = std::move(b.stOverride);
        b.extraTex.clear();
        b.stOverride.clear();
        RendererInfo none;
        b.materialFor(*mm, mp, partId, nullptr, nullptr, none, 0);
        b.runtime = std::move(savedRuntime);
        b.extraTex = std::move(savedExtra);
        b.stOverride = std::move(savedSt);
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
                    scene.textures[static_cast<size_t>(made)].srgb = td->colorSpace == 1;
                    it = inputTexCache.emplace(td.get(), std::make_pair(made, td)).first;
                }
                idx = it->second.first;
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
        if (std::getenv("CARDBRIDGE_DEBUG_CREATE")) {
            std::fprintf(stderr, "createjob %s part %s size %d props:", matName, partId.c_str(), size);
            for (const auto& pr : job.material.props) std::fprintf(stderr, " %s=(%.3f %.3f %.3f %.3f)", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
            std::fprintf(stderr, " tex:");
            for (const auto& tb : job.material.texProps) std::fprintf(stderr, " %s=%d", tb.name.c_str(), tb.texture);
            std::fprintf(stderr, "\n");
        }
        job.linear = CreateRunsInLinearLight(matName);
        job.width = job.height = size;
        job.texture = b.addSceneTexture(std::string(matName) + "_result", std::string("create material ") + matName, 1, 1, std::vector<uint8_t>{255, 255, 255, 255});
        for (const auto& [name, td] : texs) if (td && name == "_MainTex") scene.textures[static_cast<size_t>(job.texture)].clamp = true;
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
        const int64_t bodyRoot = base->findPrefabRoot(opt.bodyPrefab);
        if (!bodyRoot) {
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
            for (const auto& r : base->renderersUnder(bodyRoot)) {
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
                        // KSOX BodyUnder: the card's full-coverage skin repaint goes under the create shader's own detail,
                        // onto the base (source-over on the flat white base the game's base texture is), not on top of the result.
                        std::shared_ptr<unity::TextureData> bodyBase = skinWithUnder(b.decode(*base, bt, "body"), card::MaterialEdits::OverlayBodyUnder, b.edits);
                        std::vector<TexInput> texs = {{"_MainTex", bodyBase}, {"_ColorMask", b.decode(*base, bm, "body")}};
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
                            auto addOverlayTexture = [&](const char* texName, const char* colName, const std::shared_ptr<unity::TextureData>& t, Vec4 col) {
                                int idx = -1;
                                if (t) {
                                    auto copy = t->rgba;
                                    idx = b.addSceneTexture(t->name, "body overlay", t->width, t->height, std::move(copy));
                                    scene.textures[static_cast<size_t>(idx)].clamp = t->wrapMode == 1;   // the overlay is placed by _nipsize: outside its square it must not repeat
                                    scene.textures[static_cast<size_t>(idx)].srgb = t->colorSpace == 1;
                                }
                                bodyOverlayTex.emplace_back(texName, idx);
                                bodyOverlayColors.emplace_back(colName, col);
                                return idx;
                            };
                            auto addOverlay = [&](const char* texName, const char* colName, int category, int64_t id, const char* prop, const char* key, const char* what, Vec4 col) {
                                return addOverlayTexture(texName, colName, rowTextureFor("body", category, id, prop, key, what), col);
                            };
                            // The automatic pink nipple is the Clochette mt_nipple03 bump (the nashi "Clochette Nipple" mod — the one the Matou Sakura
                            // card renders; soft and natural-looking), independent of the card's own NipTex choice. Without that mod installed,
                            // fall back to the card's own NipTex row so the export still looks as the card author intended.
                            const bool automaticPink = opt.nippleColour.w > 0.0f;
                            const int64_t nipId = bd.at("nipId").asInt();
                            auto nipOverlay = std::shared_ptr<unity::TextureData>{};
                            if (automaticPink) nipOverlay = rowTextureFor("body", 423, 743, "ChaFileBody.nipId", "NipTex", "nipple");
                            if (!nipOverlay) {
                                if (automaticPink)
                                    scene.warn("body", "default nipple overlay (mt_nipple03, nashi Clochette Nipple mod) not found; fell back to the card's NipTex id " + std::to_string(nipId));
                                nipOverlay = rowTextureFor("body", 423, nipId, "ChaFileBody.nipId", "NipTex", "nipple");
                            }
                            const Vec4 nipColour = automaticPink ? Vec4{opt.nippleColour.x, opt.nippleColour.y, opt.nippleColour.z, 1.0f}
                                                                   : vec4Of(bd.at("nipColor"));
                            addOverlayTexture("_overtex1", "_overcolor1", nipOverlay, nipColour);
                            addOverlay("_overtex2", "_overcolor2", 424, bd.at("underhairId").asInt(), "ChaFileBody.underhairId", "UnderhairTex", "underhair", vec4Of(bd.at("underhairColor")));
                            bodyOverlayColors.emplace_back("_nipsize", Vec4{opt.areolaSize >= 0 ? opt.areolaSize : static_cast<float>(bd.at("areolaSize").asFloat()), 0, 0, 0});
                        } else {
                            bool underComposited = false;
                            std::shared_ptr<unity::TextureData> bodyBase = skinWithUnder(b.decode(*base, bt, "body"), card::MaterialEdits::OverlayBodyUnder, b.edits, &underComposited);
                            bodyLook.tex1 = b.bake(bodyBase, underComposited ? "body_t_under" : "body_t", b.decode(*base, bm, "body"), "body_mc", c, base->name());
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
                int mi = b.addRenderer(*base, r, bodyRoot, partIdx, "body", Mat4{}, 0, &skin, false, bodyLookPtr);
                b.extraTex.clear();
                b.runtime.clear();
                if (mi >= 0) {
                    part.meshes.push_back(mi);
                    // The automatic skin shade is baked into the body's skin texture; the mesh's own geometry is its source.
                    if (std::find(scene.skinShade.meshes.begin(), scene.skinShade.meshes.end(), mi) == scene.skinShade.meshes.end())
                        scene.skinShade.meshes.push_back(mi);
                    if (scene.skinShade.texture < 0 && bodyLook.tex1 >= 0) scene.skinShade.texture = bodyLook.tex1;
                    for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes) {
                        if (sub.material < 0 || static_cast<size_t>(sub.material) >= scene.materials.size()) continue;
                        auto& sm = scene.materials[static_cast<size_t>(sub.material)];
                        if (sm.name != "cf_m_body" && sm.name.rfind("cf_m_body", 0) != 0) continue;
                        sm.cardSkin = true;
                        sm.cardNippleColour = vec4Of(card.body.at("nipColor"));
                    }
                }
            }
            if (scene.createJobs.empty())   // the body's create material did not run
                scene.warn("body", "body skin texture is built at runtime by the game from the card's skin settings; "
                                   "the card's base skin colour is used instead");
        }
        scene.parts.push_back(std::move(part));
        log("body: " + std::to_string(scene.parts.back().meshes.size()) + " mesh(es)");
        LogMemory("assemble after body");
    }

    // --- card items ------------------------------------------------------
    skeletonTimer.Stop();
    cbtime::Scope itemsTimer("items");
    std::vector<resolve::ItemRef> work = resolve::ItemsFromCard(card, outfit);
    log("assembling " + std::to_string(work.size()) + " card item(s)");
    auto itemWhere = [](const resolve::ItemRef& ref, const resolve::ResolvedItem& item) {
        // A name of where this item is supposed to come from, with the mod identity when
        // we know it (the card names a GUID; the mod index gives the display name when installed):
        // "outfit top (id 100106001; mod 'Heavy Ribbons' by hnrocemii v1.0 (com.foo.bar))"
        std::string s = ref.label.empty() ? ref.partId : ref.label;
        s += " (id " + std::to_string(ref.id);
        if (!item.modGuid.empty()) {
            s += "; mod '";
            if (!item.modName.empty()) {
                s += item.modName;
                if (!item.modVersion.empty()) s += " v" + item.modVersion;
                if (!item.modAuthor.empty()) s += " by " + item.modAuthor;
                if (!item.modGuid.empty()) s += " (" + item.modGuid + ")";
            } else {
                s += item.modGuid;
            }
            s += ')';
        } else {
            s += ')';
        }
        return s;
    };
    report(0.15, "resolving items");
    for (const auto& wr : work) {   // which installed mods ship the same bundle path (the one loaded last is what the game uses)
        resolve::ResolvedItem probe = resolver.resolve(card, wr);
        resolver.registerBundleProvider(probe);
    }
    const Value& sk2 = card.body.at("skinMainColor");
    Vec4 skinColor{1, 1, 1, 1};
    if (sk2.size() >= 3) skinColor = vec4Of(sk2);

    bool topCoversBra = false;
    struct MaskSource { std::shared_ptr<unity::TextureData> tex; std::string owner; };   // a garment's mask and the garment's part id
    std::vector<MaskSource> bodyMasks, braMasks;
    for (size_t wi = 0; wi < work.size(); ++wi) {
        cbtime::Scope itemTimer("item " + work[wi].partId);
        checkCancel();
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
        part.fromClothing = card.clothingPartIds.count(ref.partId) != 0;
        if (!part.fromClothing && ref.subKind >= 0) {   // top sub-parts inherit the top's provenance
            const size_t at = ref.partId.find(".sub_");
            if (at != std::string::npos) part.fromClothing = card.clothingPartIds.count(ref.partId.substr(0, at)) != 0;
        }
        part.property = ref.property;
        part.category = ref.category;
        part.attachNode = ref.attachNode;
        part.sourceItemId = ref.id;
        part.status = resolve::StatusName(item.status);
        part.detail = item.detail;
        part.modGuid = item.modGuid;
        part.modName = item.modName;
        part.modVersion = item.modVersion;
        part.modAuthor = item.modAuthor;
        part.modPath = item.modPath;
        int partIdx = static_cast<int>(scene.parts.size());

        if (item.ambiguous) scene.warn(ref.partId, "more than one installed file matches " + itemWhere(ref, item) + "; the first was used");
        if (!item.ok()) {
            // The card names a mod we cannot find, or one we installed that does not provide this item:
            // say exactly what is missing (which item, which id, and which mod) and why it is out.
            scene.warn(ref.partId, "missing: " + itemWhere(ref, item) + "; the item was skipped (" + item.detail + ")");
            part.detail = "missing: " + itemWhere(ref, item) + "; the item was skipped";
            scene.parts.push_back(std::move(part));
            continue;
        }

        std::string whyOpen;
        auto uf = resolver.openBundle(item, &whyOpen);
        if (!uf) {
            part.status = "missing-bundle";
            part.detail = "missing data for " + itemWhere(ref, item) + ": " + whyOpen;
            scene.warn(ref.partId, part.detail);
            scene.parts.push_back(std::move(part));
            continue;
        }
        part.bundle = item.bundleRel();
        // Heel configuration (priority 1: the mod's own Stiletto txt; priority 2: the known
        // table). The txt lives in the mod as BepInEx/config/Stiletto/Heels/<Name>.txt
        // (Stiletto v2.4.2) or BepInEx/Stiletto/<Name>.txt (legacy installs / shipped mods),
        // where Name is the list row's display Name (the game's heelName, trailing spaces and
        // unicode preserved, lookup case-insensitive). Only footwear slots carry it;
        // hasHeelConfig stays false otherwise so settleHeels uses geometry.
        part.listName = item.listItem.get("Name");
        if (ref.slot == 7 || ref.slot == 8) {
            HeelConfig hc;
            bool found = false;
            if (item.status == resolve::ItemStatus::ResolvedMod && !part.listName.empty()) {
                auto txt = resolver.openModText(item, "BepInEx/config/Stiletto/Heels/" + part.listName + ".txt");
                std::string entry;
                if (txt) entry = "config/Stiletto/Heels";
                else {
                    txt = resolver.openModText(item, "BepInEx/Stiletto/" + part.listName + ".txt");
                    if (txt) entry = "Stiletto";
                }
                if (txt) {
                    std::string text(txt->begin(), txt->end());
                    if (ParseStilettoHeel(text, hc)) {
                        hc.source = "zipmod " + item.modGuid + " (" + entry + ")";
                        found = true;
                    } else {
                        // A present file with no recognized heel keys reads as an all-zero
                        // config in DynamicFileProvider, i.e. an explicit flat: it suppresses
                        // the known table and geometry (issue #4 separation).
                        HeelConfig flat;
                        flat.hasConfig = true;
                        flat.source = "zipmod " + item.modGuid + " (" + entry + " flat: empty/no heel keys)";
                        hc = flat;
                        found = true;
                        bool nonEmpty = false;
                        for (unsigned char c : text) if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && !(c == 0xEF || c == 0xBB || c == 0xBF)) { nonEmpty = true; break; }
                        if (nonEmpty)
                            scene.warn(ref.partId, "Stiletto heel file BepInEx/" + entry + "/" + part.listName +
                                                   ".txt from zipmod " + item.modGuid + " has no recognized heel keys; treated as flat");
                    }
                }
            }
            if (!found && KnownHeelConfig(part.listName, hc)) found = true;
            if (found) {
                part.hasHeelConfig = true;
                part.heelAnkleDeg = hc.ankleDeg; part.heelLegDeg = hc.legDeg;
                part.heelToeDeg = hc.toeDeg; part.heelHeightMeters = hc.heightMeters;
                part.heelShoeScaleX = hc.shoeScaleX; part.heelShoeScaleY = hc.shoeScaleY; part.heelShoeScaleZ = hc.shoeScaleZ;
                part.heelShoeAngleDeg = hc.shoeAngleDeg; part.heelShoeTranslateY = hc.shoeTranslateY;
                part.heelShoeTranslateZ = hc.shoeTranslateZ; part.heelShoeShearY = hc.shoeShearY;
                part.heelShoeShearZ = hc.shoeShearZ; part.heelConfigSource = hc.source;
            }
        }
        const int64_t itemRoot = uf->findPrefabRoot(item.assetName());
        if (!itemRoot) {
            part.status = "missing-bundle";
            part.detail = "prefab '" + item.assetName() + "' not found in " + item.bundleRel() + " (" + itemWhere(ref, item) + ")";
            scene.warn(ref.partId, part.detail);
            scene.parts.push_back(std::move(part));
            continue;
        }

        // Accessories hang on a named body node; everything else is skinned.
        const unity::Xform* rootXf = uf->xform(itemRoot);
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
        // ChaFile noShake (accessories and hair parts): rigid in the game, no DynamicBone sway.
        bool itemNoShake = false;
        if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& ap = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex));
            if (ap.isMap()) itemNoShake = ap.at("noShake").asBool(false);
        } else if (ref.group == "Hair") {
            size_t kind = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
            itemNoShake = card.hair.at("parts").at(kind).at("noShake").asBool(false);
        }
        if (accessoryMode) {
            // A garment prefab worn through an accessory slot is packed that way by DeathWeasel's "Accessory
            // Clothes" mod, which tags it with a ChaAccessoryClothes component: the game rebinds its skinned
            // meshes to the body bones. Plain accessories routinely carry a cf_o_root transform without being
            // garments (every Koikatsu accessory prefab does), so the component - not the transform name - marks
            // one. Anything else stays a rigid piece hanging on its attach node, as the game has it.
            bool boundAsGarment = false;
            for (const auto& mb : uf->monoBehavioursUnder(itemRoot))
                if (mb.className == "ChaAccessoryClothes" || mb.className == "AccessoryClothes") { boundAsGarment = true; break; }
            if (boundAsGarment) {
                accessoryMode = false;
                garmentAsAccessory = true;
                placement = Mat4();
                staticBone = 0;
            }
        }
        if ((ref.group == "Outfit" || ref.group == "Head") && rootXf) {
            // A garment hangs on objTop (the body-space origin); the head model on the head rig root (objHeadBone). The game
            // parents both with SetParent(parent, false). Their skinned meshes are retargeted to the character's bones by name
            // (AssignedAnotherWeights), so the root transform moves the prefab's static geometry and own bones.
            const Mat4 parent = ref.group == "Head" ? headRoot : Mat4{};
            placement = PrefabRootPlacement(parent, rootXf->pos, rootXf->rot, rootXf->scl);
            if (ref.group == "Head") staticBone = std::max(0, scene.boneIndex(opt.headRigPrefab));
        }
        // The game instantiates the item prefab and parents it with SetParent(parent, false), so the prefab root keeps its own
        // local transform under the parent (a cape authored facing back is turned 180 degrees by its root). `rootInv` cancels
        // it again when a transform is expressed relative to the prefab root.
        if (accessoryMode && rootXf) placement = PrefabRootPlacement(placement, rootXf->pos, rootXf->rot, rootXf->scl);
        if (const char* accDbg = std::getenv("CARDBRIDGE_DEBUG_ACC")) {
            if (std::strcmp(accDbg, "1") == 0 || std::strstr(ref.partId.c_str(), accDbg)) {
                Vec3 a = (ref.group == "Accessories" && !ref.attachNode.empty()) ? [&]() -> Vec3 { auto it = rig.world.find(ref.attachNode); return it == rig.world.end() ? Vec3{-999, -999, -999} : it->second.translation(); }() : Vec3{0, 0, 0};
                std::fprintf(stderr, "accdbg %s attach=%s attachT=(%.3f %.3f %.3f) rootT=(%.4f %.4f %.4f) rootR=(%.1f %.1f %.1f %.1f) rootS=(%.3f %.3f %.3f) finalT=(%.4f %.4f %.4f)\n",
                             ref.partId.c_str(), ref.attachNode.c_str(), a.x, a.y, a.z,
                             rootXf ? rootXf->pos.x : 0.0f, rootXf ? rootXf->pos.y : 0.0f, rootXf ? rootXf->pos.z : 0.0f,
                             rootXf ? rootXf->rot.x : 0.0f, rootXf ? rootXf->rot.y : 0.0f, rootXf ? rootXf->rot.z : 0.0f, rootXf ? rootXf->rot.w : 1.0f,
                             rootXf ? rootXf->scl.x : 0.0f, rootXf ? rootXf->scl.y : 0.0f, rootXf ? rootXf->scl.z : 0.0f,
                             placement.translation().x, placement.translation().y, placement.translation().z);
            }
        }

        // ---- colouring: what the game does at run time with the card's colours ----
        ItemLook look;
        bool haveLook = false;
        if ((ref.group == "Outfit" || garmentAsAccessory) && ref.outfit >= 0) {
            Vec4 c[3];
            const Value* clothesInfo = nullptr;   // the garment's colorInfo (patterns and tiling), absent for a garment worn as an accessory
            if (garmentAsAccessory) {   // a garment in an accessory slot is coloured by that slot's three colours
                const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(std::max(0, ref.accIndex))).at("color");
                for (size_t k = 0; k < 3; ++k) c[k] = kkColor(vec4Of(col.at(k)));
            } else {
                // Sub-parts use the top's colours; part C's first colour is the fourth slot (ChaControl.ChangeCustomClothes).
                size_t colorSlot = ref.subKind >= 0 ? 0 : static_cast<size_t>(std::max(0, ref.slot));
                const Value& ci = card.outfits[static_cast<size_t>(ref.outfit)].clothes.at("parts").at(colorSlot).at("colorInfo");
                clothesInfo = &ci;
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
                    if (!tb) {
                        if (auto loose = resolver.openLooseTexture(item, ab, name, nullptr)) { keyOut = "loose:" + ab + "/" + name; return loose; }
                        scene.warn(ref.partId, "texture bundle '" + ab + "' not loaded: " + w);
                        return nullptr;
                    }
                }
                PPtr tp = tb->findAsset(name);
                if (tp.isNull()) tp = tb->findTexture(name);
                if (tp.isNull()) {
                    if (auto loose = resolver.openLooseTexture(item, ab, name, nullptr)) { keyOut = "loose:" + ab + "/" + name; return loose; }
                    scene.warn(ref.partId, "texture '" + name + "' not found in " + tb->name());
                    return nullptr;
                }
                keyOut = tb->name() + "#" + std::to_string(tp.pathId);
                if (std::getenv("CARDBRIDGE_DEBUG_CLOTHES")) std::fprintf(stderr, "clothes texture %s %s = %s\n", ref.partId.c_str(), name.c_str(), keyOut.c_str());
                return b.decode(*tb, tp, ref.partId);
            };
            std::string k1, k2, k3, k4;
            auto main1 = listTex("MainTexAB", "MainTex", k1);
            auto mask1 = listTex("ColorMaskAB", "ColorMaskTex", k2);
            auto main2 = listTex("MainTex02AB", "MainTex02", k3);
            auto mask2 = listTex("ColorMask02AB", "ColorMask02Tex", k4);
            // The game only builds a texture when it has both the main texture and its mask.
            // Garments are built by the game's cf_m_clothesN_create material: the main texture and colour mask, the three colours, and a
            // pattern (mt_pattern list texture, its colour and tiling) per colour slot.
            auto clothesTexture = [&](const std::shared_ptr<unity::TextureData>& mainT, const std::string& mainKey, const std::shared_ptr<unity::TextureData>& maskT, const std::string& maskKey) -> int {
                if (clothesInfo) {
                    const Value& ci = *clothesInfo;
                    const bool subC = ref.subKind == 2;
                    auto tile = [&](size_t k, int axis) { const Value& t = ci.at(k).at("tiling"); double v = t.isArray() ? t.at(static_cast<size_t>(axis)).asFloat(1.0) : t.at(axis == 0 ? "x" : "y").asFloat(1.0); return static_cast<float>(v); };
                    std::vector<TexInput> texs = {{"_MainTex", mainT}, {"_ColorMask", maskT}};
                    std::vector<std::pair<std::string, Vec4>> props;
                    const char* patTex[3] = {"_PatternMask1", "_PatternMask2", "_PatternMask3"};
                    const char* col2[3] = {"_Color1_2", "_Color2_2", "_Color3_2"};
                    const char* colN[3] = {"_Color", "_Color2", "_Color3"};
                    for (size_t k = 0; k < 3; ++k) {
                        const size_t src = (k == 0 && subC) ? 3 : k;   // sub part C takes the fourth colour slot as its first
                        texs.push_back({patTex[k], rowTextureFor(ref.partId, 430, ci.at(src).at("pattern").asInt(), "ChaFileClothes.pattern", "MainTex", "pattern" + std::to_string(k + 1), nullptr, "MainTexAB")});
                        props.emplace_back(colN[k], kkColor(vec4Of(ci.at(src).at("baseColor"))));
                        props.emplace_back(col2[k], kkColor(vec4Of(ci.at(src).at("patternColor"))));
                        props.emplace_back("_PatternScale" + std::to_string(k + 1) + "u", Vec4{tile(src, 0), 0, 0, 0});
                        props.emplace_back("_PatternScale" + std::to_string(k + 1) + "v", Vec4{tile(src, 1), 0, 0, 0});
                    }
                    if (std::getenv("CARDBRIDGE_DEBUG_CLOTHES")) { std::fprintf(stderr, "clothes %s main %s mask %s:", ref.partId.c_str(), mainT->name.c_str(), maskT->name.c_str()); for (const auto& pr : props) std::fprintf(stderr, " %s=(%.2f %.2f %.2f %.2f)", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w); std::fprintf(stderr, "\n"); }
                    if (std::getenv("CARDBRIDGE_DEBUG_CLOTHES")) {
                        auto mean = [](const unity::TextureData& t, double out[4]) { double sum[4] = {0, 0, 0, 0}; size_t n = static_cast<size_t>(t.width) * t.height; for (size_t i = 0; i < n; ++i) for (int c = 0; c < 4; ++c) sum[c] += t.rgba[i * 4 + c]; for (int c = 0; c < 4; ++c) out[c] = n ? sum[c] / n : 0; };
                        double a[4], bm[4]; mean(*mainT, a); mean(*maskT, bm);
                        std::fprintf(stderr, "  main %dx%d mean %.0f %.0f %.0f %.0f  mask %dx%d mean %.0f %.0f %.0f %.0f\n", mainT->width, mainT->height, a[0], a[1], a[2], a[3], maskT->width, maskT->height, bm[0], bm[1], bm[2], bm[3]);
                    }
                    int size = std::min(1024, std::max(mainT->width, mainT->height));
                    int job = createJobFor(ref.partId, "cf_m_clothesN_create", texs, props, size);
                    if (job >= 0) {
                        // KoiClothesOverlayX paints an image over the garment's texture (named by the garment's object in the game).
                        static const char* const kGarmentNames[] = {"ct_clothesTop", "ct_clothesBot", "ct_bra", "ct_shorts", "ct_gloves", "ct_panst", "ct_socks", "ct_shoes_inner", "ct_shoes_outer"};
                        static const char* const kSubNames[] = {"ct_top_parts_A", "ct_top_parts_B", "ct_top_parts_C"};
                        const char* garment = ref.subKind >= 0 && ref.subKind < 3 ? kSubNames[ref.subKind] : ref.slot >= 0 && ref.slot < 9 ? kGarmentNames[ref.slot] : nullptr;
                        card::MaterialEdits::Image ov;
                        bool replace = false;
                        if (garment && b.edits && b.edits->decodeClothesOverlay(garment, ov, replace)) {
                            SceneCreateJob& cj = scene.createJobs.back();
                            cj.overlay = std::move(ov.rgba);
                            cj.overlayWidth = ov.width;
                            cj.overlayHeight = ov.height;
                            cj.overlayReplaces = replace;
                        }
                        return job;
                    }
                }
                return b.bake(mainT, mainKey, maskT, maskKey, c, uf->name());
            };
            if (main1 && mask1) { look.tex1 = clothesTexture(main1, k1, mask1, k2); haveLook = true; }
            if (main2 && mask2) { look.tex2 = clothesTexture(main2, k3, mask2, k4); haveLook = true; }
            if (look.tex1 >= 0 || look.tex2 >= 0) look.bakedOutfitColors = true;
            if (haveLook) {
                for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
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
                    if (std::getenv("CARDBRIDGE_DEBUG_EYE")) std::fprintf(stderr, "iris blend %g offY %g scale %g main %s %dx%d grad %s %dx%d\n", pu.at("gradBlend").asFloat(), gy, gs, tex ? tex->name.c_str() : "-", tex ? tex->width : 0, tex ? tex->height : 0, grad ? grad->name.c_str() : "-", grad ? grad->width : 0, grad ? grad->height : 0);
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
                    if (!tb) {
                        if (auto loose = resolver.openLooseTexture(item, ab, name, nullptr)) { key = "loose:" + ab + "/" + name; return loose; }
                        scene.warn(ref.partId, "face skin: texture bundle '" + ab + "' not loaded: " + w);
                        return nullptr;
                    }
                }
                PPtr tp = tb->findAsset(name);
                if (tp.isNull()) tp = tb->findTexture(name);
                if (tp.isNull()) {
                    if (auto loose = resolver.openLooseTexture(item, ab, name, nullptr)) { key = "loose:" + ab + "/" + name; return loose; }
                    scene.warn(ref.partId, "face skin: texture '" + name + "' not found in " + tb->name());
                    return nullptr;
                }
                key = tb->name() + "#" + std::to_string(tp.pathId);
                return b.decode(*tb, tp, ref.partId);
            };
            std::string k1, k2;
            std::shared_ptr<unity::TextureData> base = fromHead("MainTexAB", "MainTex", k1);
            // KSOX FaceUnder: the card's full-coverage face repaint goes under the create shader's own detail.
            base = skinWithUnder(base, card::MaterialEdits::OverlayFaceUnder, b.edits);
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
                        if (t) { auto copy = t->rgba; idx = b.addSceneTexture(t->name, "face overlay", t->width, t->height, std::move(copy));
                            scene.textures[static_cast<size_t>(idx)].clamp = t->wrapMode == 1;
                            scene.textures[static_cast<size_t>(idx)].srgb = t->colorSpace == 1; }
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
            for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
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
                if (!mb) { if (auto loose = resolver.openLooseTexture(item, mab, mname, nullptr)) bodyMasks.push_back({loose, ref.partId}); }
                if (mb) {
                    PPtr mp = mb->findAsset(mname);
                    if (mp.isNull()) mp = mb->findTexture(mname);
                    if (!mp.isNull()) { if (auto mt = b.decode(*mb, mp, ref.partId)) bodyMasks.push_back({mt, ref.partId}); }
                    else if (auto loose = resolver.openLooseTexture(item, mab, mname, nullptr)) bodyMasks.push_back({loose, ref.partId});
                    else scene.warn(ref.partId, "body mask '" + mname + "' not found in " + mab);
                }
            }
            std::string bab = item.listItem.get("OverBraMaskAB"), bname = item.listItem.get("OverBraMask");
            if (!bname.empty() && bname != "0" && !bab.empty() && bab != "0") {
                std::string w;
                auto bb = resolver.openItemBundle(item, bab, &w);
                if (!bb) { if (auto loose = resolver.openLooseTexture(item, bab, bname, nullptr)) braMasks.push_back({loose, ref.partId}); }
                if (bb) {
                    PPtr bp = bb->findAsset(bname);
                    if (bp.isNull()) bp = bb->findTexture(bname);
                    if (!bp.isNull()) { if (auto bt = b.decode(*bb, bp, ref.partId)) braMasks.push_back({bt, ref.partId}); }
                    else if (auto loose = resolver.openLooseTexture(item, bab, bname, nullptr)) braMasks.push_back({loose, ref.partId});
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
            for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
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
            if (hp != rig.world.end()) {
                b.hairParent = &hp->second;
                placement = rootXf ? PrefabRootPlacement(hp->second, rootXf->pos, rootXf->rot, rootXf->scl) : hp->second;
                staticBone = std::max(0, scene.boneIndex("cf_J_FaceUp_ty"));
            } else scene.warn(ref.partId, "hair parent node cf_J_FaceUp_ty not found; the hair is placed at the model origin");
        }
        std::map<int64_t, Mat4> simW;   // hair and accessory chains settle under their DynamicBones
        b.simW = nullptr;
        // Off by default: against the game dumps the settle helped one card in four and hurt the others (docs: sidecar notes), so hair and accessory chains
        // stay at their authored pose unless CARDBRIDGE_DYNBONE=1.
        if (!itemNoShake && std::getenv("CARDBRIDGE_DYNBONE") && ((ref.group == "Hair" && b.hairParent) || accessoryMode)) {
            const Mat4 anchor = placement;
            Mat4 start;
            const Mat4* startPtr = nullptr;
            if (b.rigDefault) {
                auto sw = b.rigDefault->world.find(ref.group == "Hair" ? std::string("cf_J_FaceUp_ty") : ref.attachNode);
                if (sw != b.rigDefault->world.end()) {
                    start = rootXf ? PrefabRootPlacement(sw->second, rootXf->pos, rootXf->rot, rootXf->scl) : sw->second;
                    startPtr = &start;
                }
            }
            if (settleItem(*uf, itemRoot, anchor, startPtr, b.dbColliders, b.localOv, simW)) { b.simW = &simW; scene.chainsSettled = true; }
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
        // Shoe warp for this item's foot binds (Stiletto UpdateShoeWarp). Identity for all
        // shipped data; set from the preloaded config so addRenderer can warp bindposes.
        b.heelWarpOn = part.hasHeelConfig;
        b.heelWarp.ankleDeg = part.heelAnkleDeg; b.heelWarp.legDeg = part.heelLegDeg;
        b.heelWarp.toeDeg = part.heelToeDeg; b.heelWarp.heightMeters = part.heelHeightMeters;
        b.heelWarp.shoeScaleX = part.heelShoeScaleX; b.heelWarp.shoeScaleY = part.heelShoeScaleY;
        b.heelWarp.shoeScaleZ = part.heelShoeScaleZ; b.heelWarp.shoeAngleDeg = part.heelShoeAngleDeg;
        b.heelWarp.shoeTranslateY = part.heelShoeTranslateY; b.heelWarp.shoeTranslateZ = part.heelShoeTranslateZ;
        b.heelWarp.shoeShearY = part.heelShoeShearY; b.heelWarp.shoeShearZ = part.heelShoeShearZ;
        b.heelWarp.hasConfig = part.hasHeelConfig; b.heelWarp.source = part.heelConfigSource;

        // Which renderers get which run-time colours (ChaAccessoryComponent: rendNormal/rendAlpha/rendHair; ChaCustomHairComponent: rendHair).
        std::set<int64_t> rtNormal, rtAlpha, rtHair, rtHairItem, rtHairAcs;
        bool haveAccComponent = false;
        bool useC[3] = {true, true, true};
        for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
            auto ids = [&](const char* field, std::set<int64_t>& into) {
                const Value& fv = mb.fields.at(field);
                if (fv.isMap()) { if (fv.at("m_FileID").asInt() == 0 && fv.at("m_PathID").asInt() != 0) into.insert(fv.at("m_PathID").asInt()); return; }
                for (const auto& pp : fv.asArray()) if (pp.at("m_FileID").asInt() == 0 && pp.at("m_PathID").asInt() != 0) into.insert(pp.at("m_PathID").asInt());
            };
            if (std::getenv("CARDBRIDGE_DEBUG_MB")) std::fprintf(stderr, "mb %s on %s\n", mb.className.c_str(), ref.partId.c_str());
            if (mb.className == "DynamicBone" && std::getenv("CARDBRIDGE_DEBUG_DB")) {
                const Value& f = mb.fields;
                auto v3 = [&](const char* k) { const Value& v = f.at(k); return std::to_string(v.at("x").asFloat()) + "," + std::to_string(v.at("y").asFloat()) + "," + std::to_string(v.at("z").asFloat()); };
                std::fprintf(stderr, "db %s go=%lld root=%lld rate=%g damp=%g elas=%g stiff=%g inert=%g rad=%g endlen=%g endoff=%s grav=%s force=%s freeze=%lld coll=%zu excl=%zu norolls=%zu curves=%zu/%zu/%zu\n", ref.partId.c_str(), static_cast<long long>(mb.goId),
                             static_cast<long long>(f.at("m_Root").at("m_PathID").asInt()), f.at("m_UpdateRate").asFloat(), f.at("m_Damping").asFloat(), f.at("m_Elasticity").asFloat(), f.at("m_Stiffness").asFloat(), f.at("m_Inert").asFloat(),
                             f.at("m_Radius").asFloat(), f.at("m_EndLength").asFloat(), v3("m_EndOffset").c_str(), v3("m_Gravity").c_str(), v3("m_Force").c_str(), static_cast<long long>(f.at("m_FreezeAxis").asInt()),
                             f.at("m_Colliders").asArray().size(), f.at("m_Exclusions").asArray().size(), f.at("m_notRolls").asArray().size(),
                             f.at("m_DampingDistrib").at("m_Curve").asArray().size(), f.at("m_ElasticityDistrib").at("m_Curve").asArray().size(), f.at("m_StiffnessDistrib").at("m_Curve").asArray().size());
            }
            if (mb.className == "ChaAccessoryComponent") {
                haveAccComponent = true;
                ids("rendNormal", rtNormal); ids("rendAlpha", rtAlpha); ids("rendHair", rtHair);
                useC[0] = mb.fields.at("useColor01").asInt() != 0;
                useC[1] = mb.fields.at("useColor02").asInt() != 0;
                useC[2] = mb.fields.at("useColor03").asInt() != 0;
                if (std::getenv("CARDBRIDGE_DEBUG_MB")) {
                    auto dumpIds = [&](const char* label, const std::set<int64_t>& s) {
                        std::string out;
                        for (int64_t id : s) { if (!out.empty()) out += ","; out += std::to_string(id); }
                        return label + std::string("=[") + out + "]";
                    };
                    std::fprintf(stderr, "acccmp %s useColor01=%d useColor02=%d useColor03=%d %s %s %s\n", ref.partId.c_str(),
                                 mb.fields.at("useColor01").asInt(), mb.fields.at("useColor02").asInt(), mb.fields.at("useColor03").asInt(),
                                 dumpIds("rendNormal", rtNormal).c_str(), dumpIds("rendAlpha", rtAlpha).c_str(), dumpIds("rendHair", rtHair).c_str());
                }
            } else if (mb.className == "ChaCustomHairComponent") {
                ids("rendHair", rtHairItem);
                ids("rendAccessory", rtHairAcs);
                if (std::getenv("CARDBRIDGE_DEBUG_MB")) {
                    auto dumpIds = [&](const char* label, const std::set<int64_t>& s) {
                        std::string out;
                        for (int64_t id : s) { if (!out.empty()) out += ","; out += std::to_string(id); }
                        return label + std::string("=[") + out + "]";
                    };
                    std::fprintf(stderr, "haircmp %s %s %s\n", ref.partId.c_str(), dumpIds("rendHair", rtHairItem).c_str(), dumpIds("rendAccessory", rtHairAcs).c_str());
                }
            }
        }
        if (look.materialTint && haveAccComponent) {   // every renderer with an effective colour (the game's rules: the colour block below)
            look.tintOnly = rtNormal;
            look.tintOnly.insert(rtHair.begin(), rtHair.end());
            look.tintOnly.insert(rtHairItem.begin(), rtHairItem.end());
            look.tintOnly.insert(rtHairAcs.begin(), rtHairAcs.end());
            if (look.tintOnly.empty()) look.tintOnly.insert(-1);   // a component that lists none: nothing is tinted
        }
        Vec4 hairColor{1, 1, 1, 1};
        bool haveHairColor = false;
        size_t hairKind = contains(ref.partId, "front") ? 1 : contains(ref.partId, "side") ? 2 : contains(ref.partId, "ext") ? 3 : 0;
        if (ref.group == "Hair") {   // hair items: the card's hair colour (accessories use the run-time rules below)
            const Value& hp = card.hair.at("parts").at(hairKind);
            if (hp.at("baseColor").size() >= 3) {
                hairColor = hairTint(kkColor(vec4Of(hp.at("baseColor"))), kkColor(vec4Of(hp.at("startColor"))), kkColor(vec4Of(hp.at("endColor"))));
                haveHairColor = true;
            }
        }
        Value hacInfo;   // HairAccessoryCustomizer data of this accessory slot (a map when the card has it)
        if (accessoryMode && ref.outfit >= 0 && ref.accIndex >= 0) {
            const Value& payload = card.extended.at("com.deathweasel.bepinex.hairaccessorycustomizer").at(size_t(1)).at("HairAccessories");
            if (payload.isBin()) hacInfo = MsgPackParse(payload.asBin()).atInt(ref.outfit).atInt(ref.accIndex);
        }
        std::vector<std::string> skipped, skippedByEditor;
        // A garment's optional pieces (ChaClothesComponent objOpt01 / objOpt02): the maker's two option switches, which the card keeps as
        // the garment's hideOpt; a switched-off option's objects are inactive in the game.
        std::vector<int64_t> hiddenOptions;
        if (ref.group == "Outfit" && ref.outfit >= 0 && ref.slot >= 0) {
            const Value& hide = card.outfits[static_cast<size_t>(ref.outfit)].clothes.at("parts").at(static_cast<size_t>(ref.subKind >= 0 ? 0 : ref.slot)).at("hideOpt");
            for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
                if (mb.className != "ChaClothesComponent") continue;
                for (size_t k = 0; k < 2; ++k) {
                    if (!hide.at(k).asBool()) continue;
                    for (const auto& go : mb.fields.at(k == 0 ? "objOpt01" : "objOpt02").asArray())
                        if (go.at("m_FileID").asInt() == 0 && go.at("m_PathID").asInt() != 0) hiddenOptions.push_back(go.at("m_PathID").asInt());
                }
            }
        }
        size_t before = scene.meshes.size();
        for (const auto& r : uf->renderersUnder(itemRoot)) {
            if (std::getenv("CARDBRIDGE_DEBUG_PATHS"))
                std::fprintf(stderr, "renderer %s: %s enabled=%d active=%d cid=%lld\n", ref.partId.c_str(), uf->pathOf(r.xformId).c_str(), r.enabled ? 1 : 0, r.active ? 1 : 0, r.componentId);
            if (!r.enabled) continue;
            // ChangeAccessoryColor deactivates the accessory's GameObject when its slot colour alpha is 0: the renderer is not drawn at all.
            if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0 && rtAlpha.count(r.componentId)) {
                const Value& ac3 = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("color").at(size_t(3));
                if (ac3.size() >= 4 && ac3.at(size_t(3)).asFloat(1.0f) == 0.0f) { skipped.push_back(r.name + " (accessory colour alpha 0; the game hides this renderer)"); continue; }
            }
            if (b.edits && b.meType >= 0) {   // MaterialEditor can switch a renderer off
                bool off = false;
                for (const auto* e : b.edits->editsFor(b.edits->rendererEnabled, b.meCoordinate, b.meType, b.meSlot, r.name))
                    if (e->property == "0" && e->value[0] == 0.0f) off = true;
                if (off) { skippedByEditor.push_back(r.name); continue; }
            }
            if (onlyHelperMaterials(*uf, r)) continue;
            std::string path = "/" + uf->pathOf(r.xformId) + "/";
            // Variants the game keeps switched off in the default state: the pulled-off ("nuge") top and
            // bottom, and the face's tears and gag eyes. (The tongue, o_tang, stays: it sits in the mouth, and an open mouth without it is
            // an empty black hole.)
            // A garment prefab holds one node per clothes state: n_top_a / n_bot_a is the worn state, b and c the pulled-off
            // ones (c = shorts or skirt hanging from one leg); an object inactive in the prefab is off in every state.
            bool hiddenByDefault = !r.active || hiddenClothesState(path) || (!hiddenOptions.empty() && uf->isUnderGameObject(r.xformId, hiddenOptions)) ||
                                   (ref.group == "Head" && (contains(r.name, "namida") || contains(r.name, "gag_eye")));
            if (hiddenByDefault) { skipped.push_back(r.name); continue; }
            // Debug aid: CARDBRIDGE_SKIP=name1,name2 leaves those renderers out (to find which mesh draws an artefact).
            if (const char* sk = std::getenv("CARDBRIDGE_SKIP")) {
                std::string list = std::string(",") + sk + ",";
                if (list.find("," + r.name + ",") != std::string::npos) { skipped.push_back(r.name + " (CARDBRIDGE_SKIP)"); continue; }
            }
            const Vec4* fb = (ref.group == "Head" && contains(r.name, "face")) ? &skinColor : nullptr;
            if (ref.group == "Hair" && haveHairColor) fb = &hairColor;   // the card's hair tint (see hairTint)
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
                std::vector<unity::MonoInfo> mbs = uf->monoBehavioursUnder(itemRoot);
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
            // The brow and lash shaders (toon_eyew) paint with the material's _Color, the texture only a mask, and the game's
            // ChangeSettingEyebrow/Eyeline puts the card's colour there at run time; the bundle's value is white, which baked the
            // strokes white. Set it from the card so the bake (and the live shader) paint the strokes in the card's colour.
            if (ref.group == "Head") {
                if (r.name == "cf_O_mayuge") { b.runtime.emplace_back("_Color", kkColor(vec4Of(faceV.at("eyebrowColor")))); if (std::getenv("CARDBRIDGE_DEBUG_FACE")) { Vec4 c = kkColor(vec4Of(faceV.at("eyebrowColor"))); std::fprintf(stderr, "face-runtime mayuge _Color=(%.2f %.2f %.2f)\n", c.x, c.y, c.z); } }
                else if (r.name == "cf_O_eyeline" || r.name == "cf_O_eyeline_low") { b.runtime.emplace_back("_Color", kkColor(vec4Of(faceV.at("eyelineColor")))); if (std::getenv("CARDBRIDGE_DEBUG_FACE")) { Vec4 c = kkColor(vec4Of(faceV.at("eyelineColor"))); std::fprintf(stderr, "face-runtime %s _Color=(%.2f %.2f %.2f)\n", r.name.c_str(), c.x, c.y, c.z); } }
            }
            // The face material's _overcolor2 alpha is the blush of the character's emotion state: 0 at rate 0, 0.2 at rate 1.
            b.blushAlpha = (ref.group == "Head" && contains(r.name, "cf_O_face"))
                               ? 0.2f * std::min(1.0f, std::max(0.0f, static_cast<float>(card.status.at("hohoAkaRate").asFloat(0.0))))
                               : -1.0f;
            // Hair accessories: the HairAccessoryCustomizer slot setting decides gloss and colours (ColorMatch = the character's hair colours); a slot
            // without that data keeps the prefab's own.
            b.glossForThisRenderer = rtHairItem.count(r.componentId) > 0 && (ref.group == "Hair" || (hacInfo.isMap() && hacInfo.at("HairGloss").asBool()));
            b.stripHairGloss = false;
            auto rawColor = [&](const Value& c) { Vec4 v = vec4Of(c); if (v.w < 1.0f) v.w = 1.0f; return v; };
            const int64_t cid = r.componentId;
            if (ref.group == "Accessories" && ref.outfit >= 0 && ref.accIndex >= 0) {
                // The game's colouring for accessories, in the same order the run-time paths apply it (last write wins):
                // ChaControl.ChangeAccessoryColor - rendNormal takes the slot colours (only the useColor0N channels the prefab asks for), rendAlpha takes
                // _Color4, rendHair pins all three to the back-hair start colour; then the HairAccessoryCustomizer slot rules at run time - rendHair of the
                // custom-hair component keeps the hair colours when ColorMatch is on (else the plugin stays silent), rendAccessory takes AccessoryColor.
                const char* kNames[3] = {"_Color", "_Color2", "_Color3"};
                const Value& col = card.outfits[static_cast<size_t>(ref.outfit)].accessory.at("parts").at(static_cast<size_t>(ref.accIndex)).at("color");
                const Value& hp0 = card.hair.at("parts").at(size_t(0));
                const Vec4 base0 = rawColor(hp0.at("baseColor")), start0 = rawColor(hp0.at("startColor")), end0 = rawColor(hp0.at("endColor"));
                const bool hac = hacInfo.isMap();
                const bool hacMatch = hac && hacInfo.at("ColorMatch").asBool();
                Vec4 eff[3] = {{-1.0f, 0, 0, -1.0f}, {-1.0f, 0, 0, -1.0f}, {-1.0f, 0, 0, -1.0f}};   // w < 0: the game leaves the prefab's own value
                if (rtNormal.count(cid)) {
                    if (useC[0]) eff[0] = rawColor(col.at(size_t(0)));
                    if (useC[1]) eff[1] = rawColor(col.at(size_t(1)));
                    if (useC[2]) eff[2] = rawColor(col.at(size_t(2)));
                }
                if (rtHair.count(cid)) { eff[0] = eff[1] = eff[2] = start0; }
                if (hac && rtHairItem.count(cid)) {
                    if (hacMatch) { eff[0] = base0; eff[1] = start0; eff[2] = end0; }
                    b.runtime.emplace_back("_LineColor", hacMatch ? rawColor(hp0.at("outlineColor")) : vec4Of(hacInfo.at("OutlineColor")));
                    b.stripHairGloss = !hacInfo.at("HairGloss").asBool();
                }
                if (hac && rtHairAcs.count(cid)) {
                    const Vec4 ac = vec4Of(hacInfo.at("AccessoryColor"));
                    eff[0] = eff[1] = eff[2] = ac;
                }
                for (int k = 0; k < 3; ++k)
                    if (eff[k].w >= 0.0f) b.runtime.emplace_back(kNames[k], eff[k]);
                look.tintByRenderer[cid] = {eff[0], eff[1], eff[2]};
                if (rtAlpha.count(cid)) b.runtime.emplace_back("_Color4", vec4Of(col.at(size_t(3))));
            } else if (ref.group == "Hair" && rtHairItem.count(r.componentId)) {
                const Value& hp = card.hair.at("parts").at(hairKind);
                b.runtime.emplace_back("_Color", haveHairColor ? hairColor : rawColor(hp.at("baseColor")));
                b.runtime.emplace_back("_Color2", rawColor(hp.at("startColor")));
                b.runtime.emplace_back("_Color3", rawColor(hp.at("endColor")));
                b.runtime.emplace_back("_LineColor", vec4Of(hp.at("outlineColor")));
            }
            const Vec4* hairTintColorForRenderer = ref.group == "Hair" && haveHairColor ? &hairColor : nullptr;
            int mi = b.addRenderer(*uf, r, itemRoot, partIdx, ref.partId, placement, staticBone, fb, accessoryMode, lookPtr, hairTintColorForRenderer);
            if (mi >= 0) part.meshes.push_back(mi);
        }
        b.addItemBones(*uf, itemRoot, ref.partId, placement, staticBone, accessoryMode, itemNoShake);
        if (ref.group == "Head") {   // the expressions the face component can play: every pattern of brows, eyes and mouth, closed and open
            for (const auto& mb : uf->monoBehavioursUnder(itemRoot)) {
                if (mb.className != "FaceBlendShape") continue;
                static const struct { const char* field; const char* label; int panel; } ctrls[3] = {{"EyebrowCtrl", "Brow", 1}, {"EyesCtrl", "Eyes", 2}, {"MouthCtrl", "Mouth", 3}};
                for (const auto& c : ctrls) {
                    const Value& ctl = mb.fields.at(c.field);
                    size_t patterns = 0;
                    for (const auto& tg : ctl.at("FBSTarget").asArray()) patterns = std::max(patterns, tg.at("PtnSet").asArray().size());
                    const double openMin = ctl.at("OpenMin").asFloat(0.0);
                    for (size_t ptn = 0; ptn < patterns; ++ptn)
                        for (int open = 0; open < 2; ++open) {
                            const int num3 = static_cast<int>(std::min(100.0, std::max(0.0, (openMin + (1.0 - openMin) * open) * 100.0)));
                            std::map<std::pair<int, int>, float> weights;
                            for (const auto& tg : ctl.at("FBSTarget").asArray()) {
                                const auto& ptns = tg.at("PtnSet").asArray();
                                auto gm = b.goMesh.find(tg.at("ObjTarget").at("m_PathID").asInt());
                                if (ptns.empty() || gm == b.goMesh.end()) continue;
                                const Value& ps = ptns[ptn < ptns.size() ? ptn : 0];
                                const int close = static_cast<int>(ps.at("Close").asInt()), opn = static_cast<int>(ps.at("Open").asInt());
                                if (close >= 0) weights[{gm->second, close}] += static_cast<float>(100 - num3);
                                if (opn >= 0) weights[{gm->second, opn}] += static_cast<float>(num3);
                            }
                            if (weights.empty()) continue;
                            SceneExpression ex;
                            ex.name = std::string(c.label) + " " + std::to_string(ptn) + (open ? " open" : " closed");
                            ex.panel = c.panel;
                            for (const auto& [key, w] : weights) ex.targets.push_back({key.first, key.second, std::min(100.0f, w)});
                            {   // the pattern's own name: the strongest shape on the biggest mesh it moves ("kuti_face.f00_a_l_op" -> "a_l_op")
                                size_t best = 0, bestVerts = 0;
                                float bestW = -1.0f;
                                for (size_t ti = 0; ti < ex.targets.size(); ++ti) {
                                    const auto& t = ex.targets[ti];
                                    const size_t nv = scene.meshes[static_cast<size_t>(t.mesh)].vertices.size();
                                    if (nv > bestVerts || (nv == bestVerts && t.weight > bestW)) { best = ti; bestVerts = nv; bestW = t.weight; }
                                }
                                const auto& t = ex.targets[best];
                                const auto& shapes = scene.meshes[static_cast<size_t>(t.mesh)].shapes;
                                if (t.shape >= 0 && static_cast<size_t>(t.shape) < shapes.size()) {
                                    std::string n = shapes[static_cast<size_t>(t.shape)].name;
                                    if (const size_t dot = n.rfind('.'); dot != std::string::npos) n = n.substr(dot + 1);
                                    if (const size_t us = n.find('_'); us != std::string::npos) n = n.substr(us + 1);
                                    ex.shape = n;
                                }
                            }
                            scene.expressions.push_back(std::move(ex));
                        }
                }
            }
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
        b.goMesh.clear();
        b.simW = nullptr;
        b.hairParent = nullptr;
        b.blushAlpha = -1;
        b.extraTex.clear();
        b.stOverride.clear();
        b.blend = nullptr;
        b.meType = -1;
        std::string logId = part.id, logStatus = part.status;
        size_t logMeshes = part.meshes.size();
        scene.parts.push_back(std::move(part));
        log(logId + ": " + (logStatus.empty() ? std::string("ok") : logStatus) + ", " + std::to_string(logMeshes) + " mesh(es)");
        report(0.15 + 0.6 * (static_cast<double>(wi + 1) / static_cast<double>(work.size())),
               "item " + std::to_string(wi + 1) + " / " + std::to_string(work.size()) + ": " + logId);
    }
    itemsTimer.Stop();
    LogMemory("assemble after items");

    // A top whose list row says NotBra hides the bra in the game (it would only show through the garment).
    if (topCoversBra)
        for (auto& p : scene.parts)
            if (p.group == "Outfit" && p.id.size() >= 4 && p.id.compare(p.id.size() - 4, 4, ".bra") == 0 && !p.meshes.empty()) {
                p.includeInExport = false;
                p.visibleInPreview = false;
                p.detail = "hidden: the top covers the bra (list NotBra=1); the game does not draw it either";
            }

    // The triangles the worn garments cover. Masks live in the covered mesh's own UV space (red channel 255 = keep): the game masks
    // the body this way, and the bra the same way (OverBraMask). The covered triangles leave the part but are kept as "under pieces"
    // (one part per set of garments covering them) so the nude body can be shown again, in the viewer and through PMX morphs.
    log("applying clothing masks");
    report(0.78, "clothing masks");
    cbtime::Scope masksTimer("clothing masks");
    // A garment whose materials are all authored translucency does not mask what is behind it: the
    // skin underneath shows through the fabric (that is the point of it), so its triangles stay.
    std::map<std::string, bool> allTranslucentByPart;   // part id -> true when every material on it blends
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.meshes.empty()) continue;
        bool all = true;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes) {
                const SceneMaterial* mat = sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() ? &scene.materials[static_cast<size_t>(sub.material)] : nullptr;
                if (!mat || !mat->translucent) { all = false; break; }
            }
            if (!all) break;
        }
        allTranslucentByPart[p.id] = all;
    }
    auto allCoverTranslucent = [&](const std::vector<std::string>& cover) {
        if (cover.empty()) return false;
        for (const auto& id : cover) {
            if (id.empty()) return false;   // a hidden garment hides the skin the game-way; only visible+translucent ones let it through
            auto it = allTranslucentByPart.find(id);
            if (it == allTranslucentByPart.end() || !it->second) return false;
        }
        return true;
    };
    auto applyMasks = [&](const std::string& what, const std::function<bool(const ScenePart&)>& pick,
                          const std::vector<MaskSource>& masks, const std::function<bool(const SceneMesh&)>& meshOk = {}) {
        if (masks.empty()) return;
        size_t dropped = 0, total = 0;
        const size_t partCount = scene.parts.size();
        for (size_t pi = 0; pi < partCount; ++pi) {
            if (!pick(scene.parts[pi])) continue;
            const std::vector<int> meshIndices = scene.parts[pi].meshes;
            // garments covering a triangle -> per mesh, per material, the triangles' indices
            std::map<std::vector<std::string>, std::map<int, std::map<int, std::vector<uint32_t>>>> pieces;
            for (int mi : meshIndices) {
                SceneMesh& m = scene.meshes[static_cast<size_t>(mi)];
                if (meshOk && !meshOk(m)) continue;
                for (auto& sub : m.submeshes) {
                    std::vector<uint32_t> keep;
                    for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                        ++total;
                        float u = 0, v = 0;
                        for (int k = 0; k < 3; ++k) { u += m.vertices[sub.indices[t + static_cast<size_t>(k)]].uv.x; v += m.vertices[sub.indices[t + static_cast<size_t>(k)]].uv.y; }
                        u /= 3; v /= 3;
                        u -= std::floor(u); v -= std::floor(v);
                        std::vector<std::string> cover;
                        for (const auto& mk : masks) {
                            int x = std::min(mk.tex->width - 1, static_cast<int>(u * mk.tex->width)), y = std::min(mk.tex->height - 1, static_cast<int>((1.0f - v) * mk.tex->height));
                            if (mk.tex->rgba[(static_cast<size_t>(y) * mk.tex->width + x) * 4] < 128 && std::find(cover.begin(), cover.end(), mk.owner) == cover.end()) cover.push_back(mk.owner);
                        }
                        if (cover.empty() || allCoverTranslucent(cover)) {
                            keep.insert(keep.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                            continue;
                        }
                        ++dropped;
                        // a garment the game itself hides still covers: it counts as "" (always)
                        for (auto& id : cover)
                            for (const auto& gp : scene.parts)
                                if (gp.id == id && !gp.visibleInPreview) id.clear();
                        std::sort(cover.begin(), cover.end());
                        cover.erase(std::unique(cover.begin(), cover.end()), cover.end());
                        auto& dst = pieces[cover][mi][sub.material];
                        dst.insert(dst.end(), sub.indices.begin() + static_cast<long>(t), sub.indices.begin() + static_cast<long>(t) + 3);
                    }
                    sub.indices = std::move(keep);
                }
            }
            for (auto& [cover, byMesh] : pieces) {
                ScenePart piece;
                const ScenePart parent = scene.parts[pi];   // a copy: scene.parts grows below
                std::string tag;
                for (const auto& id : cover) {
                    std::string shortName = id.rfind("outfit", 0) == 0 && id.find('.') != std::string::npos ? id.substr(id.find('.') + 1) : id;
                    tag += (tag.empty() ? "" : "+") + (shortName.empty() ? std::string("always") : shortName);
                }
                piece.id = parent.id + ".under." + tag;
                piece.label = parent.label + " under " + tag;
                piece.group = parent.group;
                piece.fromClothing = parent.fromClothing;
                piece.status = parent.status;
                piece.bundle = parent.bundle;
                piece.detail = "the " + what + " triangles that " + tag + " covers (kept so the nude body can be shown)";
                piece.visibleInPreview = false;
                piece.startHidden = true;
                piece.includeInExport = parent.includeInExport;
                piece.underPiece = true;
                piece.underOf = parent.id;
                piece.coveredBy = cover;
                const int pieceIdx = static_cast<int>(scene.parts.size());
                for (auto& [mi, byMat] : byMesh) {
                    const SceneMesh src = scene.meshes[static_cast<size_t>(mi)];   // a copy: scene.meshes grows below
                    SceneMesh nm;
                    nm.name = src.name + ".under";
                    nm.part = pieceIdx;
                    nm.reshapable = src.reshapable;
                    std::map<uint32_t, uint32_t> remap;
                    for (auto& [mat, idx] : byMat) {
                        SceneSubmesh sm;
                        sm.material = mat;
                        for (uint32_t i : idx) {
                            auto it = remap.find(i);
                            if (it == remap.end()) { it = remap.emplace(i, static_cast<uint32_t>(nm.vertices.size())).first; nm.vertices.push_back(src.vertices[i]); }
                            sm.indices.push_back(it->second);
                        }
                        nm.submeshes.push_back(std::move(sm));
                    }
                    piece.meshes.push_back(static_cast<int>(scene.meshes.size()));
                    scene.meshes.push_back(std::move(nm));
                }
                scene.parts.push_back(std::move(piece));
            }
        }
        scene.warn(what, what + " mask: " + std::to_string(dropped) + " of " + std::to_string(total) + " triangles hidden under clothing (as the game does)");
    };
    applyMasks("body", [](const ScenePart& p) { return p.id == "body"; }, bodyMasks);
    applyMasks("bra", [](const ScenePart& p) { return p.group == "Outfit" && !p.underPiece && p.id.size() >= 4 && p.id.compare(p.id.size() - 4, 4, ".bra") == 0; }, braMasks,
               [](const SceneMesh& m) { return m.name.rfind("o_bra", 0) == 0; });   // only the bra itself; a body suit in the bra slot has other UVs
    // The body triangles the clothes cover move onto "body.under.*" copies. The skin shading has to follow them:
    // they hold the chest and the cleavage, and those are exactly the places the shade is for.
    for (const auto& p : scene.parts)
        if (p.underPiece && p.underOf == "body")
            for (int mi : p.meshes)
                if (std::find(scene.skinShade.meshes.begin(), scene.skinShade.meshes.end(), mi) == scene.skinShade.meshes.end())
                    scene.skinShade.meshes.push_back(mi);
    {   // the nude switch: one checkbox in the outfit list that brings back all the skin under the clothes
        bool any = false;
        for (const auto& p : scene.parts) any = any || p.underPiece;
        if (any) {
            ScenePart nude;
            nude.id = "nude";
            nude.label = "Nude body";
            nude.group = "Outfit";
            nude.status = "resolved-base";
            nude.detail = "tick to show the whole body, also where clothes cover it; the PMX has the same as the 'Nude body' morph";
            nude.nudeSwitch = true;
            nude.includeInExport = false;
            scene.parts.push_back(std::move(nude));
        }
    }
    {   // PMX has no sampler wrap mode: a UV outside 0..1 repeats there, while the game clamps a clamped texture (the
        // create-job alpha cuts, the ramps). Pull the UVs of a mesh whose materials all sample a clamped main texture
        // into the tile, so the bake, the viewer and the exported file all see the same edge-clamped texel. A mesh
        // mixing clamped and repeating materials cannot satisfy both: keep its wrap and say so.
        size_t clampedMeshes = 0, mixedMeshes = 0;
        for (auto& mesh : scene.meshes) {
            if (mesh.submeshes.empty()) continue;
            bool anyClamped = false, allClamped = true;
            for (const auto& sub : mesh.submeshes) {
                if (sub.material < 0 || static_cast<size_t>(sub.material) >= scene.materials.size()) { allClamped = false; continue; }
                const SceneMaterial& mat = scene.materials[static_cast<size_t>(sub.material)];
                const bool clamped = mat.texture >= 0 && static_cast<size_t>(mat.texture) < scene.textures.size() &&
                                     scene.textures[static_cast<size_t>(mat.texture)].clamp;
                anyClamped = anyClamped || clamped;
                allClamped = allClamped && clamped;
            }
            if (!anyClamped) continue;
            const std::string partId = mesh.part >= 0 && static_cast<size_t>(mesh.part) < scene.parts.size() ? scene.parts[static_cast<size_t>(mesh.part)].id : std::string();
            if (!allClamped) {
                ++mixedMeshes;
                scene.warn(partId, "mesh '" + mesh.name + "' mixes clamp-wrapped and repeating main textures; its UVs keep the wrap");
                continue;
            }
            bool moved = false;
            for (auto& v : mesh.vertices) {
                const float u = std::min(1.0f, std::max(0.0f, v.uv.x));
                const float w = std::min(1.0f, std::max(0.0f, v.uv.y));
                if (u != v.uv.x || w != v.uv.y) { v.uv = {u, w}; moved = true; }
            }
            if (moved) ++clampedMeshes;
        }
        if (clampedMeshes || mixedMeshes)
            log("clamped the UVs of " + std::to_string(clampedMeshes) + " mesh(es) with clamp-wrapped main textures" +
                (mixedMeshes ? ", left " + std::to_string(mixedMeshes) + " mixed-wrap mesh(es) alone" : std::string()));
    }

    if (std::getenv("CARDBRIDGE_DEBUG_RIG"))
        for (const char* nm : {"cf_j_neck", "cf_j_head", "cf_s_head", "cf_J_N_FaceRoot", "cf_J_FaceRoot", "cf_J_FaceBase", "cf_J_FaceUp_ty", "a_n_headside", "cf_J_Mouth_L", "cf_J_Chin_Base", "cf_J_FaceUp_tz", "cf_J_Eye_tx_L", "cf_J_NoseBridge_ty"}) {
            auto w = rig.world.find(nm);
            if (w == rig.world.end()) { std::fprintf(stderr, "rig %s missing\n", nm); continue; }
            const Mat4& m = w->second;
            auto cl = [&](int c) { return std::sqrt(m.m[c] * m.m[c] + m.m[4 + c] * m.m[4 + c] + m.m[8 + c] * m.m[8 + c]); };
            std::fprintf(stderr, "rig %s pos %.3f %.3f %.3f scale %.3f %.3f %.3f\n", nm, m.m[3], m.m[7], m.m[11], cl(0), cl(1), cl(2));
        }
    masksTimer.Stop();
    // Koikatsu's own body skin look: the base body normal map (always bound by the game as _NormalMap) and the
    // card's detailId detail normal / line mask pair (bound as _NormalMapDetail / _LineMask, category 420).
    // Decoded here, once, so the bake below - and the viewer's later re-applications - can use them without
    // re-opening the bundles. When the card is unmodified, the line mask is cf_body_00_line (its G channel carries
    // the clavicle/knee ink lines; the breast region stays clean, exactly as in the game).
    if (!scene.skinShade.meshes.empty() && scene.aoBake.baseNormal.empty()) {
        std::string w;
        if (auto mm = resolver.openBaseBundle("chara/mm_base.unity3d", &w)) {
            const PPtr p = mm->findTexture("cf_body_00_base_n");
            if (!p.isNull()) {
                std::string why;
                if (auto t = mm->readTexture(p, &why)) {
                    scene.aoBake.baseNormal = t->rgba;
                    scene.aoBake.baseNormalWidth = t->width;
                    scene.aoBake.baseNormalHeight = t->height;
                }
            }
            const PPtr pl = mm->findTexture("cf_body_00_line");
            if (!pl.isNull()) {
                std::string why;
                if (auto t = mm->readTexture(pl, &why)) {
                    scene.aoBake.lineMask = t->rgba;
                    scene.aoBake.lineMaskWidth = t->width;
                    scene.aoBake.lineMaskHeight = t->height;
                }
            }
        } else {
            scene.warn("body", "mm_base.unity3d not loaded: " + w + " (the Koikatsu skin look bake is skipped)");
        }
        try {
            const int64_t detailId = card.body.at("detailId").asInt();
            if (auto t = rowTextureFor("body", 420, detailId, "ChaFileBody.detailId", "NormallMapDetail", "body detail normal")) {
                scene.aoBake.detailNormal = t->rgba;
                scene.aoBake.detailNormalWidth = t->width;
                scene.aoBake.detailNormalHeight = t->height;
            }
            if (auto t = rowTextureFor("body", 420, detailId, "ChaFileBody.detailId", "LineMask", "body detail lines")) {
                scene.aoBake.lineMask = t->rgba;
                scene.aoBake.lineMaskWidth = t->width;
                scene.aoBake.lineMaskHeight = t->height;
            }
        } catch (const std::exception& e) {
            scene.warn("body", std::string("body detailId: ") + e.what());
        }
    }
    log("running " + std::to_string(scene.createJobs.size()) + " game create material(s)");
    report(0.88, "create materials");
    checkCancel();
    LogMemory("assemble before create jobs");
    {
        cbtime::Scope _("create materials");
        exporter::RunCreateJobs(scene);
    }
    log("create materials done");
    {
        cbtime::Scope _("nipple tint + skin shade + ao");
        exporter::ApplyNippleTint(scene, opt.nippleColour);
        if (!scene.skinShade.meshes.empty() && (opt.skinShade > 0.0f || opt.skinShadeUnder > 0.0f)) {
            exporter::ApplySkinShade(scene, opt.skinShade, opt.skinShadeUnder, opt.skinShadeTop, opt.skinShadeWidth, true);
            log("automatic skin shading baked from the body mesh");
        }
        // The Koikatsu skin look goes on top of the create result x the cleavage shading (it yields to SkinShade's own
        // darkening, never stacks); both are multiplied into the texture the PMX references.
        if (!scene.skinShade.meshes.empty() && (opt.skinAo > 0.0f || scene.aoBake.applied >= 0.0f)) {
            exporter::ApplySkinAoBake(scene, opt.skinAo, true);
            log("Koikatsu skin look baked (base body normal map + detailId line mask)");
        }
    }
    report(0.96, "finishing");
    LogMemory("assemble after create jobs");
    if (std::getenv("CARDBRIDGE_DEBUG_BODY_UV")) {
        for (const auto& part : scene.parts) for (int mi : part.meshes) {
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : mesh.submeshes) if (scene.materials[static_cast<size_t>(sub.material)].name.find("cf_m_body") != std::string::npos) {
                // the forwardmost, highest chest vertices (the breast eyelids...) approximate: scan for x near 0, y high
                float best = 1e30f; size_t tip = 0;
                for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                    const auto& v = mesh.vertices[i];
                    float d = std::fabs(v.pos.x) + std::fabs(v.pos.y - 1.22f);
                    if (d < best) { best = d; tip = i; }
                }
                const auto& v = mesh.vertices[tip];
                std::fprintf(stderr, "body tip v%zu pos=(%.2f %.2f %.2f) uv0=(%.3f %.3f) uv1=(%.3f %.3f) uv2=(%.3f %.3f) uv3=(%.3f %.3f) nverts=%zu\n", tip, v.pos.x, v.pos.y, v.pos.z, v.uv.x, v.uv.y, v.uv1.x, v.uv1.y, v.uv2.x, v.uv2.y, v.uv3.x, v.uv3.y, mesh.vertices.size());
                // count vertices whose uv1 != (0,0) near the chest (y=1.1..1.3)
                long long nn = 0;
                for (const auto& w : mesh.vertices)
                    if (std::fabs(w.uv1.x) > 0.01f || std::fabs(w.uv1.y) > 0.01f) { if (w.pos.y > 1.0f) ++nn; }
                std::fprintf(stderr, "body verts with uv1!=0 & y>1.0: %lld\n", nn);
            }
        }
    }
    if (std::getenv("CARDBRIDGE_DEBUG_MAT"))
        for (const auto& m : scene.materials) {
            std::fprintf(stderr, "mat %s shader=%s idx=%d cull=%d two-sided=%d tex:", m.name.c_str(), m.shader.c_str(), m.shaderIndex,
                         m.shaderIndex >= 0 ? scene.shaders[static_cast<size_t>(m.shaderIndex)].cull : -1, m.doubleSided ? 1 : 0);
            for (const auto& t : m.texProps) std::fprintf(stderr, " %s=%d(clamp=%d,name=%s)", t.name.c_str(), t.texture, t.texture >= 0 ? (int)scene.textures[static_cast<size_t>(t.texture)].clamp : -1, t.texture >= 0 ? scene.textures[static_cast<size_t>(t.texture)].name.c_str() : "none");
            std::fprintf(stderr, " | props:");
            for (const auto& pr : m.props) if (pr.first.rfind("_Color", 0) == 0 || pr.first.rfind("_overcolor", 0) == 0 || pr.first == "_nipsize" || pr.first.rfind("_paint", 0) == 0) std::fprintf(stderr, " %s=(%.2f %.2f %.2f %.2f)", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
            std::fprintf(stderr, "\n");
        }
    settleHeels(scene);
    if (opt.autoCloth && !std::getenv("CARDBRIDGE_NO_AUTO_CLOTH")) SynthesizeAutoCloth(scene, log);
    report(1.0, "done");
    return scene;
}


bool Reshape(Scene& scene, const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt) {
    if (scene.bones.empty() || scene.chainsSettled) return false;   // settled chains (CARDBRIDGE_DYNBONE) were run for the old shape: assemble again
    int outfit = opt.outfit;
    if (outfit < 0) outfit = static_cast<int>(card.status.at("coordinateType").asInt());
    if (outfit < 0 || static_cast<size_t>(outfit) >= card.outfits.size()) outfit = 0;
    std::string why;
    auto base = resolver.openBaseBundle("chara/oo_base.unity3d", &why);
    if (!base) return false;

    // The same skeleton the assembly builds - body sliders, KKABMX, the head hung on the shaped neck, face sliders - but only the bones.
    Scene scratch;
    Rig rig;
    Overrides shapeOv;
    if (opt.applyShape) shapeOv = bodyShapeOverrides(card, resolver, *base, *base, opt.bodyRigPrefab, scratch, opt);
    const card::AbmxTable abmx = std::getenv("CARDBRIDGE_NO_ABMX") ? card::AbmxTable{} : card::LoadAbmx(card.extended);
    applyAbmx(shapeOv, *base, opt.bodyRigPrefab, abmx, outfit);
    if (!addRigPrefab(scratch, rig, *base, opt.bodyRigPrefab, Mat4{}, -1, "rig", shapeOv.empty() ? nullptr : &shapeOv)) return false;
    Mat4 headBase;
    if (auto hw = rig.world.find(opt.headAttachBone); hw != rig.world.end()) headBase = hw->second;
    // The head model hangs on the head rig's own root, not only on cf_s_head (see Assemble).
    Mat4 headRoot = headBase;
    if (const int64_t headRigRoot = base->findPrefabRoot(opt.headRigPrefab); headRigRoot)
        if (const unity::Xform* hx = base->xform(headRigRoot))
            headRoot = PrefabRootPlacement(headBase, hx->pos, hx->rot, hx->scl);
    const int headAttach = scratch.boneIndex(opt.headAttachBone);
    Overrides headOv;
    {
        resolve::ItemRef hr;
        for (const auto& it : resolve::ItemsFromCard(card, outfit))
            if (it.group == "Head") hr = it;
        if (hr.group == "Head") {
            resolve::ResolvedItem hi = resolver.resolve(card, hr);
            if (hi.ok()) {
                std::string hwhy;
                auto hb = resolver.openBundle(hi, &hwhy);
                if (hb) {
                    if (opt.applyShape) headOv = headShapeOverrides(card, resolver, hi, *hb, *base, opt.headRigPrefab, headRoot, scratch, opt);
                    Overrides attachOv;
                    headAttachOverrides(attachOv, *hb, hi.assetName());
                    for (const auto& [name, o] : attachOv) headOv[name] = o;   // not slider-driven: the head model's own placement wins
                }
            }
        }
    }
    applyAbmx(headOv, *base, opt.headRigPrefab, abmx, outfit);
    addRigPrefab(scratch, rig, *base, opt.headRigPrefab, headBase, headAttach, "rig", headOv.empty() ? nullptr : &headOv);

    // New world of every scene bone: the rig's own, and a bone an item added keeps its place relative to its parent.
    const size_t n = scene.bones.size();
    std::vector<Mat4> nw(n), inv(n);
    for (size_t i = 0; i < n; ++i) {
        const SceneBone& b = scene.bones[i];
        auto it = b.source == "rig" ? rig.world.find(b.name) : rig.world.end();
        if (it != rig.world.end()) nw[i] = it->second;
        else if (b.parent >= 0 && static_cast<size_t>(b.parent) < i) nw[i] = nw[static_cast<size_t>(b.parent)] * (inverse(scene.bones[static_cast<size_t>(b.parent)].world) * b.world);
        else nw[i] = b.world;
        inv[i] = inverse(b.world);
    }
    for (auto& mesh : scene.meshes) {
        if (!mesh.reshapable) continue;
        for (auto& v : mesh.vertices) {
            Vec3 pos{}, nrm{};
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= n) continue;
                const Mat4& m = nw[static_cast<size_t>(v.bone[k])];
                pos = pos + m.point(v.local[k]) * v.weight[k];
                nrm = nrm + m.dir(v.nlocal[k]) * v.weight[k];
                sum += v.weight[k];
            }
            if (sum <= 1e-6f) continue;
            v.pos = pos * (1.0f / sum);
            v.normal = normalize(nrm);
        }
        for (auto& sh : mesh.shapes) {   // the blend shape offsets turn with the bones that carry their vertices
            if (sh.local.size() != sh.offsets.size()) continue;
            for (size_t i = 0; i < sh.offsets.size(); ++i) {
                const SceneVertex& v = mesh.vertices[sh.offsets[i].first];
                Vec3 d{};
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= n) continue;
                    d = d + nw[static_cast<size_t>(v.bone[k])].dir(sh.local[i][static_cast<size_t>(k)]) * v.weight[k];
                    sum += v.weight[k];
                }
                if (sum > 1e-6f) sh.offsets[i].second = d * (1.0f / sum);
            }
        }
    }
    {   // the areola size is a colour property of the body's material (the game's shader draws the nipple texture at that size)
        const float areola = opt.areolaSize >= 0.0f ? opt.areolaSize : static_cast<float>(card.body.at("areolaSize").asFloat());
        for (auto& mat : scene.materials)
            if (mat.cardAreola)
                for (auto& pr : mat.props)
                    if (pr.first == "_nipsize") pr.second.x = areola;
    }
    for (auto& c : scene.colliders) {
        if (c.bone < 0 || static_cast<size_t>(c.bone) >= n) continue;
        const Mat4 delta = nw[static_cast<size_t>(c.bone)] * inv[static_cast<size_t>(c.bone)];
        c.center = delta.point(c.center);
        c.axis = normalize(delta.dir(c.axis));
    }
    for (size_t i = 0; i < n; ++i) {
        scene.bones[i].world = nw[i];
        scene.bones[i].pos = nw[i].translation();
    }
    if (!scene.skinShade.meshes.empty()) scene.skinShade.geometryDirty = true;   // the viewer rebuilds the map when the drag ends
    exporter::ApplyNippleTint(scene, opt.nippleColour);
    settleHeels(scene);
    return true;
}

} // namespace cardbridge::assemble
