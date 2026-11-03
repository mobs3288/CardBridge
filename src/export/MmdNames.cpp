#include "export/MmdNames.h"

#include <string>

namespace cardbridge::exporter {

namespace {

struct Pair { const char* rig; const char* mmd; const char* en; };

// Bones without a side. The rig's hips become 下半身 (the lower body, which carries the legs), spine01 and spine02 the two upper-body
// bones; spine03 stays as the rig has it, a child of 上半身2 that follows it.
constexpr Pair kCentre[] = {
    {"cf_j_hips", "下半身", "LowerBody"},
    {"cf_j_spine01", "上半身", "UpperBody"},
    {"cf_j_spine02", "上半身2", "UpperBody2"},
    {"cf_j_neck", "首", "Neck"},
    {"cf_j_head", "頭", "Head"},
};

// Bones that exist on both sides: the rig's "_L" / "_R" suffix and MMD's 左 / 右 prefix.
constexpr Pair kSided[] = {
    {"cf_j_shoulder_", "肩", "Shoulder"},
    {"cf_j_arm00_", "腕", "Arm"},
    {"cf_j_forearm01_", "ひじ", "Elbow"},
    {"cf_j_hand_", "手首", "Wrist"},
    {"cf_j_thumb01_", "親指０", "Thumb0"},
    {"cf_j_thumb02_", "親指１", "Thumb1"},
    {"cf_j_thumb03_", "親指２", "Thumb2"},
    {"cf_j_index01_", "人指１", "IndexFinger1"},
    {"cf_j_index02_", "人指２", "IndexFinger2"},
    {"cf_j_index03_", "人指３", "IndexFinger3"},
    {"cf_j_middle01_", "中指１", "MiddleFinger1"},
    {"cf_j_middle02_", "中指２", "MiddleFinger2"},
    {"cf_j_middle03_", "中指３", "MiddleFinger3"},
    {"cf_j_ring01_", "薬指１", "RingFinger1"},
    {"cf_j_ring02_", "薬指２", "RingFinger2"},
    {"cf_j_ring03_", "薬指３", "RingFinger3"},
    {"cf_j_little01_", "小指１", "LittleFinger1"},
    {"cf_j_little02_", "小指２", "LittleFinger2"},
    {"cf_j_little03_", "小指３", "LittleFinger3"},
    {"cf_j_thigh00_", "足", "Leg"},
    {"cf_j_leg01_", "ひざ", "Knee"},
    {"cf_j_foot_", "足首", "Ankle"},
    {"cf_j_toes_", "つま先", "Toe"},
    {"cf_J_hitomi_tx_", "目", "Eye"},   // the face rig spells its J with a capital
};

// The standard facial morphs a motion keys, and the game's expression morph each copies. Eye pattern 0 is the face's default eyes, its
// "closed" state a blink. (The vowels あ い う え お are not here: which of the game's mouth patterns is which vowel is not known.)
constexpr Pair kMorphs[] = {
    {"Eyes 0 closed", "まばたき", "Blink"},
};

int find(const pmx::Model& model, const std::string& name) {
    for (size_t i = 0; i < model.bones.size(); ++i) if (model.bones[i].name == name) return static_cast<int>(i);
    return -1;
}

void rename(pmx::Model& model, int bone, const std::string& mmd, const std::string& en) {
    if (bone < 0 || find(model, mmd) >= 0) return;   // missing, or the name is taken (never two bones with one name)
    model.bones[static_cast<size_t>(bone)].name = mmd;
    model.bones[static_cast<size_t>(bone)].nameEn = en;
}

} // namespace

std::vector<int> ApplyMmdBoneNames(const assemble::Scene& scene, pmx::Model& model) {
    std::vector<int> added;
    if (model.bones.size() < scene.bones.size() || scene.bones.empty()) return added;
    const auto at = [&](const std::string& rigName) { return scene.boneIndex(rigName); };

    // Indices first: renaming changes what a rig name finds.
    const int root = [&] { for (size_t i = 0; i < scene.bones.size(); ++i) if (scene.bones[i].parent < 0) return static_cast<int>(i); return -1; }();
    const int height = at("cf_n_height"), hips = at("cf_j_hips"), spine01 = at("cf_j_spine01");
    const int footIk[2] = {find(model, "cb_ik_foot_L"), find(model, "cb_ik_foot_R")};

    for (const Pair& p : kCentre) rename(model, at(p.rig), p.mmd, p.en);
    for (const Pair& p : kSided)
        for (const char* side : {"L", "R"}) {
            const bool left = side[0] == 'L';
            rename(model, at(std::string(p.rig) + side), std::string(left ? "左" : "右") + p.mmd, std::string(left ? "Left" : "Right") + p.en);
        }

    // The root and the centre chain. 全ての親 is the model's root. センター → グルーブ → 腰 are the rig's three unused IK-target nodes right
    // under the root (cf_t_root, cf_t_hips, cf_t_waist_L: no weights, in the right order and the right chain already) moved to the pelvis, and
    // the hips and the first spine bone hang from 腰 instead of from each other, so a motion's 下半身 turn does not take the torso along.
    // A rig without those nodes gets センター from the height node alone.
    rename(model, root, "全ての親", "ParentNode");
    if (hips >= 0 && find(model, "センター") < 0) {
        auto parentOf = [&](int b) { return model.bones[static_cast<size_t>(b)].parent; };
        const int c = at("cf_t_root"), g = at("cf_t_hips"), w = at("cf_t_waist_L");
        const bool chain = c >= 0 && g > c && w > g && hips > w && parentOf(g) == c && parentOf(w) == g;
        const int top = chain ? w : height;
        const struct { int bone; const char* name; const char* en; } nodes[] = {{chain ? c : height, "センター", "Center"}, {chain ? g : -1, "グルーブ", "Groove"}, {chain ? w : -1, "腰", "Waist"}};
        for (const auto& n : nodes) {
            if (n.bone < 0) continue;
            pmx::Bone& nb = model.bones[static_cast<size_t>(n.bone)];
            nb.name = n.name;
            nb.nameEn = n.en;
            nb.position = model.bones[static_cast<size_t>(hips)].position;
            nb.flags = 0x001E;
            nb.tailBone = -1;
            nb.tailOffset = {0.0f, 0.0f, 0.0f};
        }
        if (top >= 0) {
            if (parentOf(hips) != top && parentOf(hips) >= 0 && top < hips) model.bones[static_cast<size_t>(hips)].parent = top;
            if (spine01 >= 0 && parentOf(spine01) == hips && top < spine01) model.bones[static_cast<size_t>(spine01)].parent = top;
        }
    }

    // Foot IK: aimed at the ankle (the rig's own end bone sits a hand's width up the shin), with a toe IK under it as the standard models have.
    for (int side = 0; side < 2; ++side) {
        const char* s = side == 0 ? "L" : "R";
        const int bi = footIk[side];
        const int ankle = at(std::string("cf_j_foot_") + s), toe = at(std::string("cf_j_toes_") + s);
        if (bi < 0 || ankle < 0) continue;
        pmx::Bone& f = model.bones[static_cast<size_t>(bi)];
        f.ikTarget = ankle;
        {   // a PMX IK chain is the target's ancestors one after the other: the shin bone between the knee and the ankle goes in, locked
            std::vector<pmx::IkLink> links;
            for (int p = model.bones[static_cast<size_t>(ankle)].parent; p >= 0 && !f.ikLinks.empty() && p != f.ikLinks[0].bone; p = model.bones[static_cast<size_t>(p)].parent) {
                pmx::IkLink lock;
                lock.bone = p;
                lock.limited = true;
                links.push_back(lock);
            }
            f.ikLinks.insert(f.ikLinks.begin(), links.begin(), links.end());
        }
        f.position = model.bones[static_cast<size_t>(ankle)].position;
        f.name = std::string(side == 0 ? "左" : "右") + "足ＩＫ";
        f.nameEn = std::string(side == 0 ? "Left" : "Right") + " Leg IK";
        if (toe < 0) continue;
        pmx::Bone t;
        t.name = std::string(side == 0 ? "左" : "右") + "つま先ＩＫ";
        t.nameEn = std::string(side == 0 ? "Left" : "Right") + " Toe IK";
        t.position = model.bones[static_cast<size_t>(toe)].position;
        t.parent = bi;
        t.flags = 0x001E;
        t.tailOffset = {0.0f, 0.0f, 0.0f};
        t.ikTarget = toe;
        t.ikLoop = 3;
        t.ikLimit = 4.0f;
        pmx::IkLink link;
        link.bone = ankle;
        t.ikLinks = {link};
        added.push_back(static_cast<int>(model.bones.size()));
        model.bones.push_back(std::move(t));
    }
    return added;
}

std::vector<int> AddMmdMorphs(pmx::Model& model) {
    std::vector<int> added;
    for (const Pair& p : kMorphs) {
        int src = -1;
        for (size_t i = 0; i < model.morphs.size(); ++i) if (model.morphs[i].name == p.mmd) { src = -2; break; }
        if (src == -2) continue;
        for (size_t i = 0; i < model.morphs.size(); ++i) if (model.morphs[i].name == p.rig) { src = static_cast<int>(i); break; }
        if (src < 0) continue;
        pmx::Morph m = model.morphs[static_cast<size_t>(src)];
        m.name = p.mmd;
        m.nameEn = p.en;
        added.push_back(static_cast<int>(model.morphs.size()));
        model.morphs.push_back(std::move(m));
    }
    return added;
}

} // namespace cardbridge::exporter
