#include "assemble/Assembler.h"
#include "assemble/DynamicBone.h"

#include "core/Bytes.h"
#include "shape/Shape.h"
#include "card/Abmx.h"
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

Quat quatMul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// KKABMX on top of the game's shape: for every bone of the prefab the table names, the local scale, rotation and position the
// plugin writes (BoneModifier.Apply) from the bone's own values at that moment (the shape override when there is one).
void applyAbmx(Overrides& ov, UnityFile& uf, const std::string& prefabName, const card::AbmxTable& table, int coordinate) {
    if (table.empty()) return;
    auto roots = uf.findXformsByName(prefabName);
    if (roots.empty()) return;
    std::set<std::string> done;
    for (int64_t id : uf.subtree(roots[0])) {
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
    auto roots = head.findXformsByName(prefabName);
    if (roots.empty()) return;
    for (int64_t id : head.subtree(roots[0])) {
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
        b.world = w;
        scene.bones.push_back(b);
        idx[id] = static_cast<int>(scene.bones.size()) - 1;
        rig.world[x->name] = w;
    }
    return true;
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
    std::set<int64_t> tintOnly;           // accessories: the renderers the game colours (rendNormal / rendHair); empty = all
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

    // `colour` = the image is colour data (a main texture, sRGB); the other maps (normal, line mask, ramp) are linear.
    int embeddedTexture(int texId, const std::string& partId, bool colour = true) {
        const int key = colour ? texId : texId + (1 << 20);
        auto it = meTextures.find(key);
        if (it != meTextures.end()) return it->second;
        card::MaterialEdits::Image img;
        int result = -1;
        if (edits && edits->decode(texId, img)) {
            result = addSceneTexture("card_tex_" + std::to_string(texId), "card (MaterialEditor)", img.width, img.height, std::move(img.rgba));
            scene.textures[static_cast<size_t>(result)].srgb = colour;
        } else scene.warn(partId, "MaterialEditor texture " + std::to_string(texId) + " stored in the card could not be decoded");
        meTextures[key] = result;
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
            scene.textures[static_cast<size_t>(result)].srgb = t->colorSpace == 1;
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
    int materialFor(UnityFile& uf0, const PPtr& p0, const std::string& partId, const Vec4* fallbackColor, const ItemLook* look,
                    const RendererInfo& r, size_t matIndex) {
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
            if (m->shaderPathId) sm.shaderIndex = shaderFor(uf, m->shaderPathId, m->shaderName);
            for (const auto& c : m->colors) sm.props.emplace_back(c.first, c.second);
            for (const auto& rp : runtime) {
                bool found = false;
                for (auto& pr : sm.props) if (pr.first == rp.first) { pr.second = rp.second; found = true; }
                if (!found) sm.props.push_back(rp);
                if (rp.first == "_nipsize") sm.cardAreola = true;
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
            sm.doubleSided = sm.alpha || contains(m->shaderName, "hair");   // hair cards are drawn from both sides in the game; a culled PMX loses half of them
            if (look && matIndex == 0) {
                if (look->use1.count(r.componentId) && look->tex1 >= 0) sm.texture = look->tex1;
                else if (look->use2.count(r.componentId) && look->tex2 >= 0) sm.texture = look->tex2;
            }
            if (sm.texture < 0) {
                const PPtr* tp = m->texture("_MainTex");
                if (tp && !tp->isNull()) {
                    if (look && look->materialTint && (look->tintOnly.empty() || look->tintOnly.count(r.componentId))) {
                        auto main = decode(uf, *tp, partId);
                        const PPtr* mp = m->texture("_ColorMask");
                        std::shared_ptr<unity::TextureData> mask;
                        std::string maskKey = "-";
                        if (mp && !mp->isNull() && mp->isLocal()) { mask = decode(uf, *mp, partId); maskKey = uf.name() + "#" + std::to_string(mp->pathId); }
                        Vec4 tint[3] = {look->colors[0], look->colors[1], look->colors[2]};
                        if (edits && meType >= 0)   // MaterialEditor colour overrides replace the card's slot colours in the tint
                            for (const auto* e : edits->editsFor(edits->colors, meCoordinate, meType, meSlot, sm.name)) {
                                int k = e->property == "Color" ? 0 : e->property == "Color2" ? 1 : e->property == "Color3" ? 2 : -1;
                                if (k >= 0) tint[k] = Vec4{e->value[0], e->value[1], e->value[2], e->value[3]};
                            }
                        sm.texture = bake(main, uf.name() + "#" + std::to_string(tp->pathId), mask, maskKey, tint, uf.name());
                    } else {
                        sm.texture = textureFor(uf, *tp, partId);
                    }
                }
            }
            if (sm.texture < 0 && look && look->materialTint && (look->tintOnly.empty() || look->tintOnly.count(r.componentId))) sm.diffuse = {look->colors[0].x, look->colors[0].y, look->colors[0].z, 1.0f};
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
            // Texture overrides: the main texture (also what the PMX shows) and the other maps the shader reads, each with its tiling.
            for (const auto* e : edits->texEditsFor(meCoordinate, meType, meSlot, sm.name)) {
                const std::string prop = "_" + e->property;
                const bool main = e->property == "MainTex";
                int t = e->texId >= 0 ? embeddedTexture(e->texId, partId, main) : -1;
                if (main && t >= 0) { sm.texture = t; sm.diffuse = {1, 1, 1, 1}; }
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
    Mat4 itemBoneWorld(UnityFile& uf, int64_t id, int64_t prefabRoot) {
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
        // A hair item's prefab hangs under the head's hair parent (cf_J_FaceUp_ty) in the game.
        if (!anchored && hairParent) bw = *hairParent * (rootInv * Wp(uf, id));
        return bw;
    }

    // The scene bone of one of the item's own transforms: found by name, else added under the nearest ancestor the scene already has.
    int itemBone(UnityFile& uf, int64_t id, int64_t prefabRoot, const std::string& partId) {
        const unity::Xform* bx = uf.xform(id);
        if (!bx) return -1;
        int bi = scene.boneIndex(bx->name);
        if (bi >= 0) return bi;
        const Mat4 bw = itemBoneWorld(uf, id, prefabRoot);
        int parent = 0;
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
    void addItemBones(UnityFile& uf, int64_t prefabRoot, const std::string& partId, const Mat4& placement, int staticBone, bool accessoryMode) {
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
            else itemBone(uf, id, prefabRoot, partId);
        }
        addChains(uf, prefabRoot, partId, accessoryMode);
    }

    // The DynamicBone chains of an item (hair strands, skirt and ribbon joints, the body's breast and hip jiggle) as scene chains, for
    // the PMX rigid bodies and joints. Handles the classic component (a root and everything under it) and the character's Ver02 one
    // (a root, a list of bones, per-bone parameters in the selected pattern).
    void addChains(UnityFile& uf, int64_t prefabRoot, const std::string& partId, bool accessoryMode) {
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
            const std::string comment = mb.fields.at("Comment").asStr();
            ch.name = comment.empty() ? scene.bones[static_cast<size_t>(ch.bones[0])].name : comment;
            scene.chains.push_back(std::move(ch));
        }
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
        std::vector<int> boneIdx;
        std::vector<Mat4> boneMat;
        if (skinned && accessoryMode) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                int64_t bid = r.boneXforms[i];
                if (!uf.xform(bid)) { boneIdx.push_back(staticBone); boneMat.push_back(placement * (rootInv * rendererW)); continue; }
                boneIdx.push_back(accessoryBone(uf, bid, prefabRoot, placement, staticBone, partId));
                boneMat.push_back(placement * (rootInv * Wp(uf, bid)) * mesh->bindPoses[i]);
            }
        } else if (skinned) {
            for (size_t i = 0; i < r.boneXforms.size(); ++i) {
                const unity::Xform* bx = uf.xform(r.boneXforms[i]);
                std::string bname = bx ? bx->name : std::string();
                int bi = bname.empty() ? -1 : scene.boneIndex(bname);
                auto rw = rig.world.find(bname);
                if (rw == rig.world.end() && bx && bi < 0) {   // a bone only this item has (a mod's necklace joint ...)
                    bi = itemBone(uf, r.boneXforms[i], prefabRoot, partId);
                    rw = rig.world.find(bname);
                }
                const Mat4 bw = rw != rig.world.end() ? rw->second : itemBoneWorld(uf, r.boneXforms[i], prefabRoot);
                if (bi < 0) bi = 0;
                boneIdx.push_back(bi);
                boneMat.push_back(bw * mesh->bindPoses[i]);
            }
        }

        Mat4 staticM = placement * (rootInv * rendererW);
        if (hairParent) {   // a hair mesh without a skin hangs on the hair parent (cf_J_FaceUp_ty) like the prefab does in the game
            staticM = *hairParent * (rootInv * rendererW);
            if (int fi = scene.boneIndex("cf_J_FaceUp_ty"); fi >= 0) staticBone = fi;
        }
        // Inverse rest worlds of the bones that move this mesh: a vertex is also stored in each bone's own frame (SceneVertex::local), so a
        // re-shape that moves the bones moves the vertices too. Taking the frame from the scene's bone makes the identity re-shape exact.
        std::vector<Mat4> invBone(boneIdx.size());
        for (size_t i = 0; i < boneIdx.size(); ++i) invBone[i] = inverse(scene.bones[static_cast<size_t>(boneIdx[i])].world);
        const Mat4 invStatic = inverse(scene.bones[static_cast<size_t>(staticBone)].world);
        sm.reshapable = true;
        for (size_t v = 0; v < n; ++v) {
            SceneVertex& out = sm.vertices[v];
            Vec3 p = mesh->positions[v];
            Vec3 nrm = v < mesh->normals.size() ? mesh->normals[v] : Vec3{0, 1, 0};
            out.uv = v < mesh->uv0.size() ? mesh->uv0[v] : Vec2{};
            out.uv1 = v < mesh->uv1.size() ? mesh->uv1[v] : out.uv;
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
                out.bone[0] = staticBone;
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
                ss.material = materialFor(uf, r.materials[mi], partId, fallbackColor, look, r, mi);
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


} // namespace

Scene Assemble(const card::Card& card, resolve::Resolver& resolver, const AssembleOptions& opt) {
    Scene scene;
    scene.name = card.displayName();
    auto log = [&](const std::string& s) { if (opt.log) opt.log(s); };
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
    log("loading the shared skeleton and body");
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
        for (int64_t root : base->findXformsByName(opt.bodyRigPrefab))
            for (const auto& mb : base->monoBehavioursUnder(root)) {
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
        for (int64_t root : base->findXformsByName(opt.bodyRigPrefab)) b.addChains(*base, root, "rig", false);   // the body's own jiggle chains
    }
    int headAttach = scene.boneIndex(opt.headAttachBone);
    Mat4 headBase;
    if (headAttach >= 0) {
        auto hw = rig.world.find(opt.headAttachBone);
        if (hw != rig.world.end()) headBase = hw->second;
    } else {
        scene.warn("", "bone '" + opt.headAttachBone + "' not found; the head rig is placed at the origin");
    }
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
                    if (opt.applyShape) headOv = headShapeOverrides(card, resolver, hi, *hb, *base, opt.headRigPrefab, headBase, scene, opt);
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
        if (!tb) {   // a mod can ship the texture as a loose image instead of a bundle (Sideloader)
            if (auto loose = resolver.openLooseTexture(fi, ab, texName, nullptr)) { if (keyOut) *keyOut = "loose:" + ab + "/" + texName; return loose; }
            scene.warn(partId, what + ": texture bundle not loaded: " + w);
            return nullptr;
        }
        PPtr tp = tb->findAsset(texName);
        if (tp.isNull()) tp = tb->findTexture(texName);
        if (tp.isNull()) {   // not in the bundle of that name: a mod may ship the image loose next to where the bundle would be
            if (auto loose = resolver.openLooseTexture(fi, ab, texName, nullptr)) { if (keyOut) *keyOut = "loose:" + ab + "/" + texName; return loose; }
            scene.warn(partId, what + ": texture '" + texName + "' not found in " + ab);
            return nullptr;
        }
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
        if (std::getenv("CARDBRIDGE_DEBUG_CREATE")) {
            std::fprintf(stderr, "createjob %s part %s size %d props:", matName, partId.c_str(), size);
            for (const auto& pr : job.material.props) std::fprintf(stderr, " %s=(%.3f %.3f %.3f %.3f)", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
            std::fprintf(stderr, " tex:");
            for (const auto& tb : job.material.texProps) std::fprintf(stderr, " %s=%d", tb.name.c_str(), tb.texture);
            std::fprintf(stderr, "\n");
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
                                if (t) {
                                    auto copy = t->rgba;
                                    idx = b.addSceneTexture(t->name, "body overlay", t->width, t->height, std::move(copy));
                                    scene.textures[static_cast<size_t>(idx)].clamp = t->wrapMode == 1;   // the overlay is placed by _nipsize: outside its square it must not repeat
                                    scene.textures[static_cast<size_t>(idx)].srgb = t->colorSpace == 1;
                                }
                                bodyOverlayTex.emplace_back(texName, idx);
                                bodyOverlayColors.emplace_back(colName, col);
                            };
                            addOverlay("_overtex1", "_overcolor1", 423, bd.at("nipId").asInt(), "ChaFileBody.nipId", "NipTex", "nipple", vec4Of(bd.at("nipColor")));
                            addOverlay("_overtex2", "_overcolor2", 424, bd.at("underhairId").asInt(), "ChaFileBody.underhairId", "UnderhairTex", "underhair", vec4Of(bd.at("underhairColor")));
                            bodyOverlayColors.emplace_back("_nipsize", Vec4{opt.areolaSize >= 0 ? opt.areolaSize : static_cast<float>(bd.at("areolaSize").asFloat()), 0, 0, 0});
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
        log("body: " + std::to_string(scene.parts.back().meshes.size()) + " mesh(es)");
    }

    // --- card items ------------------------------------------------------
    std::vector<resolve::ItemRef> work = resolve::ItemsFromCard(card, outfit);
    log("assembling " + std::to_string(work.size()) + " card item(s)");
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
                        texs.push_back({patTex[k], rowTextureFor(ref.partId, 430, ci.at(k).at("pattern").asInt(), "ChaFileClothes.pattern", "MainTex", "pattern" + std::to_string(k + 1), nullptr, "MainTexAB")});
                        const size_t src = (k == 0 && subC) ? 3 : k;   // sub part C takes the fourth colour slot as its first
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
                        scene.createJobs.back().linear = true;
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
        std::map<int64_t, Mat4> simW;   // hair and accessory chains settle under their DynamicBones
        b.simW = nullptr;
        // Off by default: against the game dumps the settle helped one card in four and hurt the others (docs: sidecar notes), so hair and accessory chains
        // stay at their authored pose unless CARDBRIDGE_DYNBONE=1.
        if (std::getenv("CARDBRIDGE_DYNBONE") && ((ref.group == "Hair" && b.hairParent) || accessoryMode)) {
            const Mat4 anchor = ref.group == "Hair" ? *b.hairParent : placement;
            Mat4 start;
            const Mat4* startPtr = nullptr;
            if (b.rigDefault) {
                auto sw = b.rigDefault->world.find(ref.group == "Hair" ? std::string("cf_J_FaceUp_ty") : ref.attachNode);
                if (sw != b.rigDefault->world.end()) { start = sw->second; startPtr = &start; }
            }
            if (settleItem(*uf, roots[0], anchor, startPtr, b.dbColliders, b.localOv, simW)) { b.simW = &simW; scene.chainsSettled = true; }
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
        bool haveAccComponent = false;
        bool useC[3] = {true, true, true};
        for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
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
            } else if (mb.className == "ChaCustomHairComponent") {
                ids("rendHair", rtHairItem);
                ids("rendAccessory", rtHairAcs);
            }
        }
        if (look.materialTint && haveAccComponent) {   // ChangeAccessoryColor sets colours 1-3 on rendNormal only; rendAlpha gets just _Color4 and rendHair the hair colour
            look.tintOnly = rtNormal;
            look.tintOnly.insert(rtHair.begin(), rtHair.end());
            if (look.tintOnly.empty()) look.tintOnly.insert(-1);   // a component that lists none: nothing is tinted
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
        b.addItemBones(*uf, roots[0], ref.partId, placement, staticBone, accessoryMode);
        if (ref.group == "Head") {   // the expressions the face component can play: every pattern of brows, eyes and mouth, closed and open
            for (const auto& mb : uf->monoBehavioursUnder(roots[0])) {
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
    }

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
                        if (cover.empty()) {
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

    if (std::getenv("CARDBRIDGE_DEBUG_RIG"))
        for (const char* nm : {"cf_j_neck", "cf_j_head", "cf_s_head", "cf_J_N_FaceRoot", "cf_J_FaceRoot", "cf_J_FaceBase", "cf_J_FaceUp_ty", "a_n_headside", "cf_J_Mouth_L", "cf_J_Chin_Base", "cf_J_FaceUp_tz", "cf_J_Eye_tx_L", "cf_J_NoseBridge_ty"}) {
            auto w = rig.world.find(nm);
            if (w == rig.world.end()) { std::fprintf(stderr, "rig %s missing\n", nm); continue; }
            const Mat4& m = w->second;
            auto cl = [&](int c) { return std::sqrt(m.m[c] * m.m[c] + m.m[4 + c] * m.m[4 + c] + m.m[8 + c] * m.m[8 + c]); };
            std::fprintf(stderr, "rig %s pos %.3f %.3f %.3f scale %.3f %.3f %.3f\n", nm, m.m[3], m.m[7], m.m[11], cl(0), cl(1), cl(2));
        }
    log("running " + std::to_string(scene.createJobs.size()) + " game create material(s)");
    exporter::RunCreateJobs(scene);
    log("create materials done");
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
                    if (opt.applyShape) headOv = headShapeOverrides(card, resolver, hi, *hb, *base, opt.headRigPrefab, headBase, scratch, opt);
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
    return true;
}

} // namespace cardbridge::assemble
