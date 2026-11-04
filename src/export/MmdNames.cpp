#include "export/MmdNames.h"

#include <algorithm>
#include <cmath>
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

// The centre of the sphere that best fits the points (least squares on |p|^2 = 2 c.p + k); false when they do not span a ball.
bool fitSphere(const std::vector<Vec3>& pts, Vec3& centre) {
    if (pts.size() < 20) return false;
    double a[4][5] = {};
    for (const Vec3& p : pts) {
        const double row[5] = {p.x, p.y, p.z, 1.0, double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z};
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 5; ++j) a[i][j] += row[i] * row[j];
    }
    for (int i = 0; i < 4; ++i) {   // Gauss with partial pivoting
        int piv = i;
        for (int r = i + 1; r < 4; ++r) if (std::fabs(a[r][i]) > std::fabs(a[piv][i])) piv = r;
        if (std::fabs(a[piv][i]) < 1e-9) return false;
        for (int j = 0; j < 5; ++j) std::swap(a[i][j], a[piv][j]);
        for (int r = 0; r < 4; ++r) {
            if (r == i) continue;
            const double k = a[r][i] / a[i][i];
            for (int j = i; j < 5; ++j) a[r][j] -= k * a[i][j];
        }
    }
    centre = {static_cast<float>(a[0][4] / a[0][0] / 2.0), static_cast<float>(a[1][4] / a[1][1] / 2.0), static_cast<float>(a[2][4] / a[2][2] / 2.0)};
    return std::isfinite(centre.x) && std::isfinite(centre.y) && std::isfinite(centre.z);
}

// Inserts `b` at index `at`: every later bone moves up one and every reference to it with it (bones, IK, weights, rigid bodies, display
// frames). `b` refers to bones by their indices before the insert. It is listed in the display frame that lists `frameAfter`, after it.
int insertBone(pmx::Model& m, int at, pmx::Bone b, int frameAfter) {
    const auto fix = [at](int& i) { if (i >= at) ++i; };
    const auto fixBone = [&](pmx::Bone& x) { fix(x.parent); fix(x.tailBone); fix(x.appendParent); fix(x.ikTarget); for (auto& l : x.ikLinks) fix(l.bone); };
    for (auto& x : m.bones) fixBone(x);
    fixBone(b);
    for (auto& v : m.vertices) for (int& bi : v.bone) fix(bi);
    for (auto& r : m.rigidBodies) fix(r.bone);
    fix(frameAfter);
    for (auto& f : m.frames) {
        for (int& i : f.bones) fix(i);
        auto it = std::find(f.bones.begin(), f.bones.end(), frameAfter);
        if (frameAfter >= 0 && it != f.bones.end()) { f.bones.insert(it + 1, at); frameAfter = -1; }
    }
    m.bones.insert(m.bones.begin() + at, std::move(b));
    return at;
}

Vec3 lerp(Vec3 a, Vec3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }

} // namespace

void AddMmdStandardBones(pmx::Model& model) {
    const auto bone = [&](int i) -> pmx::Bone& { return model.bones[static_cast<size_t>(i)]; };
    const auto named = [](const std::string& name, const std::string& en, Vec3 pos, int parent, uint16_t flags) {
        pmx::Bone b;
        b.name = name; b.nameEn = en; b.position = pos; b.parent = parent; b.flags = flags;
        return b;
    };
    for (const char* side : {"左", "右"}) {
        const std::string s = side, en = s == "左" ? "Left " : "Right ";
        // 肩P / 肩C: 肩P above the shoulder, 肩C between the shoulder and the arm undoing 肩P's turn, so a motion's 肩P lifts the shoulder alone.
        if (int sh = find(model, s + "肩"), arm = find(model, s + "腕"); sh >= 0 && arm >= 0 && bone(arm).parent == sh && find(model, s + "肩P") < 0) {
            const int p = insertBone(model, sh, named(s + "肩P", en + "Shoulder P", bone(sh).position, bone(sh).parent, 0x001A), sh);
            ++sh; ++arm;
            bone(sh).parent = p;
            pmx::Bone c = named(s + "肩C", en + "Shoulder C", bone(arm).position, sh, 0x0012);
            c.appendParent = p; c.appendRatio = -1.0f;
            const int ci = insertBone(model, sh + 1, std::move(c), -1);
            bone(arm + 1).parent = ci;   // the arm moved one further on
        }
        // Twist bones: 腕捩 between the arm and the elbow, 手捩 between the elbow and the wrist, each turning only about the limb (a motion keys
        // the forearm's and the hand's roll on them). The game's driven helpers along the limb (cf_d_arm01-03, cf_d_forearm02, cf_d_wrist,
        // which carry the skin) take the twist in proportion to how far along they sit, as the 捩1-3 bones of the standard models do.
        const struct { const char* upper; const char* lower; const char* name; const char* nameEn; } twists[] = {{"腕", "ひじ", "腕捩", "Arm Twist"}, {"ひじ", "手首", "手捩", "Wrist Twist"}};
        for (const auto& t : twists) {
            const int up = find(model, s + t.upper), low = find(model, s + t.lower);
            if (up < 0 || low < 0 || bone(low).parent != up || find(model, s + t.name) >= 0) continue;
            const Vec3 a = bone(up).position, b = bone(low).position, d = {b.x - a.x, b.y - a.y, b.z - a.z};
            const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (len2 < 1e-8f) continue;
            const float inv = 1.0f / std::sqrt(len2);
            pmx::Bone tw = named(s + t.name, en + t.nameEn, lerp(a, b, 0.6f), up, 0x001A);
            tw.fixedAxis = {d.x * inv, d.y * inv, d.z * inv};
            const int ti = insertBone(model, up + 1, std::move(tw), up);
            bone(low + 1).parent = ti;
            for (size_t i = 0; i < model.bones.size(); ++i) {
                pmx::Bone& h = model.bones[i];
                if (h.parent != up || h.name.rfind("cf_d_", 0) != 0) continue;
                const Vec3 p = {h.position.x - a.x, h.position.y - a.y, h.position.z - a.z};
                const float f = (p.x * d.x + p.y * d.y + p.z * d.z) / len2;
                if (f <= 0.0f || f > 1.0f) continue;
                h.appendParent = ti;
                h.appendRatio = std::round(f * 20.0f) / 20.0f;
            }
        }
        // 腰キャンセル: the leg undoes 腰's turn, so a motion's 腰 bends the body over the legs instead of swinging them.
        if (int leg = find(model, s + "足"), waist = find(model, "腰"); leg >= 0 && waist >= 0 && find(model, "腰キャンセル" + s) < 0) {
            pmx::Bone c = named("腰キャンセル" + s, en + "Waist Cancel", bone(leg).position, bone(leg).parent, 0x0012);
            c.appendParent = waist; c.appendRatio = -1.0f;
            const int ci = insertBone(model, leg, std::move(c), -1);
            bone(leg + 1).parent = ci;
        }
        // 足IK親: the foot IK's parent on the floor, so a motion can carry the foot IK there.
        if (int ik = find(model, s + "足ＩＫ"), root = find(model, "全ての親"); ik >= 0 && root >= 0 && find(model, s + "足IK親") < 0) {
            const Vec3 at = bone(ik).position;
            const int pi = insertBone(model, ik, named(s + "足IK親", en + "Leg IK Parent", {at.x, 0.0f, at.z}, root, 0x001E), ik);
            bone(pi + 1).parent = pi;
        }
    }
    // 操作中心: MikuMikuDance's view centre bone, first in the list on its own.
    if (find(model, "操作中心") < 0) insertBone(model, 0, named("操作中心", "View Center", {0, 0, 0}, -1, 0x001E), -1);
}

MmdBones ApplyMmdBoneNames(const assemble::Scene& scene, pmx::Model& model, float scale) {
    MmdBones result;
    std::vector<int>& added = result.ik;
    if (model.bones.size() < scene.bones.size() || scene.bones.empty()) return result;
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

    // Eyes: a motion turns an eye about the eyeball's centre, but the rig's pupil bone sits on the pupil (turning there only tilts the iris
    // disc), so the pivot moves back to the centre of a sphere fitted to the eye white. 両目 (under the head) turns both: the eyes follow it by
    // append rotation, which needs them on a later layer.
    const int head = find(model, "頭");
    int eyeBone[2] = {-1, -1};
    Vec3 eyeCentre[2];
    bool eyeOk[2] = {false, false};
    for (int side = 0; side < 2; ++side) {
        eyeBone[side] = find(model, std::string(side == 0 ? "左" : "右") + "目");
        const std::string meshName = std::string("cf_Ohitomi_") + (side == 0 ? "L" : "R");
        for (const auto& mesh : scene.meshes) {
            if (mesh.name != meshName) continue;
            std::vector<Vec3> pts;
            for (const auto& v : mesh.vertices) pts.push_back(v.pos * scale);
            eyeOk[side] = fitSphere(pts, eyeCentre[side]);
            break;
        }
    }
    if (head >= 0 && eyeBone[0] >= 0 && eyeBone[1] >= 0 && find(model, "両目") < 0) {
        Vec3 mid = model.bones[static_cast<size_t>(eyeBone[0])].position;
        const Vec3 other = model.bones[static_cast<size_t>(eyeBone[1])].position;
        mid = {(mid.x + other.x) * 0.5f, (mid.y + other.y) * 0.5f, (mid.z + other.z) * 0.5f};
        pmx::Bone both;
        both.name = "両目";
        both.nameEn = "Eyes";
        both.position = mid;
        both.parent = head;
        both.flags = 0x001E;
        const int bothIndex = static_cast<int>(model.bones.size());
        result.extra.push_back(bothIndex);
        model.bones.push_back(std::move(both));
        for (int side = 0; side < 2; ++side) {
            pmx::Bone& e = model.bones[static_cast<size_t>(eyeBone[side])];
            // Only the depth is taken from the fit (the pupil's own x and y are right): the pivot moves straight back, into the ball.
            const float back = eyeOk[side] ? eyeCentre[side].z - e.position.z : 0.0f;
            if (back < 0.0f && back > -1.5f) e.position.z += back;
            e.layer = 1;
            e.appendParent = bothIndex;
            e.appendRatio = 0.4f;   // the game's eye is a flat iris on a shallow cap: past about 10 degrees it leaves the lid opening or pokes through the skin
        }
        // The eye white turns with the eyeball. The iris is a patch cut to the same cap, and the cap is not a sphere about the pivot: with the
        // white left still, a turned iris slid behind it (a sliver left on one eye) or out of the lid opening (on the other).
        size_t first = 0;
        std::vector<int> eyeVerts[2][2];   // [side][0 white, 1 iris]
        for (const auto& mat : model.materials) {
            for (int side = 0; side < 2; ++side) {
                const std::string key = std::string("cf_Ohitomi_") + (side == 0 ? "L" : "R");
                const size_t at = mat.name.find(key);
                if (at == std::string::npos) continue;
                const size_t next = at + key.size();
                const bool iris = next < mat.name.size() && mat.name[next] >= '0' && mat.name[next] <= '9';   // cf_Ohitomi_L02 is the iris
                for (size_t i = first; i < first + mat.indexCount && i < model.indices.size(); ++i) {
                    const int vi = static_cast<int>(model.indices[i]);
                    eyeVerts[side][iris ? 1 : 0].push_back(vi);
                    if (iris) continue;
                    pmx::Vertex& v = model.vertices[static_cast<size_t>(vi)];
                    for (int k = 0; k < 4; ++k) { v.bone[k] = k == 0 ? eyeBone[side] : 0; v.weight[k] = k == 0 ? 1.0f : 0.0f; }
                }
            }
            first += mat.indexCount;
        }
        for (auto& side : eyeVerts) for (auto& list : side) { std::sort(list.begin(), list.end()); list.erase(std::unique(list.begin(), list.end()), list.end()); }
        // The white and the iris keep the game's shape and place. Laying them on a ball about the pivot (a rounder eye when turned) pushed them
        // back into the eye socket: the skin behind the cap then covered the white's edges and the top of the iris, and scaled onto a smaller
        // ball both shrank as well.
        // The blink shapes squeeze the white with the lid but not the iris, which then showed in front of the lid. The iris takes one rigid
        // offset per shape, the mean of the white's (it stays round, and the lid hides what it should).
        for (auto& mo : model.morphs) {
            if (mo.vertices.empty()) continue;
            std::vector<std::pair<int, Vec3>> extra;
            for (int side = 0; side < 2; ++side) {
                Vec3 sum{0, 0, 0};
                int n = 0;
                for (const auto& [vi, d] : mo.vertices)
                    if (std::binary_search(eyeVerts[side][0].begin(), eyeVerts[side][0].end(), vi)) { sum = {sum.x + d.x, sum.y + d.y, sum.z + d.z}; ++n; }
                if (n == 0) continue;
                const Vec3 mean{sum.x / static_cast<float>(n), sum.y / static_cast<float>(n), sum.z / static_cast<float>(n)};
                for (int iv : eyeVerts[side][1]) extra.emplace_back(iv, mean);
            }
            mo.vertices.insert(mo.vertices.end(), extra.begin(), extra.end());
        }
    }
    return result;
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

    // Vowels, made from the game's own mouth patterns (the face's blend shapes, each a mouth shape with its teeth and cavity): pattern 2 open
    // is the open mouth with both rows of teeth (あ; い at 0.3 and え at 0.6), pattern 22 the tall round open mouth (お at 0.7, う at 0.3).
    // Only scaled, never reshaped: lips, teeth and cavity move together in a game pattern, and pushing the lips about on their own left the
    // teeth showing under the lower lip. (The spread and forward columns are for a vowel that needs them; none does now.)
    // Without those patterns (another head) the plain open mouth (pattern 0) stands in for all of them at twice its opening, so the vowels
    // still exist, only without teeth. Which pattern is which was read off renders of each one.
    for (const auto& m : model.morphs) if (m.name == "あ") return added;   // made already
    auto find = [&](const char* n) { for (size_t i = 0; i < model.morphs.size(); ++i) if (model.morphs[i].name == n) return static_cast<int>(i); return -1; };
    struct Vowel { const char* mmd; const char* en; const char* source; float opening, spread, forward; };
    const Vowel vowels[] = {{"あ", "A", "Mouth 2 open", 1.0f, 0.0f, 0.0f}, {"い", "I", "Mouth 2 open", 0.3f, 0.0f, 0.0f}, {"う", "U", "Mouth 22 open", 0.3f, 0.0f, 0.0f},
                            {"え", "E", "Mouth 2 open", 0.6f, 0.0f, 0.0f}, {"お", "O", "Mouth 22 open", 0.7f, 0.0f, 0.0f}};
    const int plain = find("Mouth 0 open");
    for (const Vowel& v : vowels) {
        int src = find(v.source);
        float gain = 1.0f;
        if (src < 0 || model.morphs[static_cast<size_t>(src)].vertices.empty()) { src = plain; gain = 2.0f; }
        if (src < 0 || model.morphs[static_cast<size_t>(src)].vertices.empty()) continue;
        const auto srcVerts = model.morphs[static_cast<size_t>(src)].vertices;   // a copy: push_back below moves the morphs
        float maxLen = 0.0f;
        for (const auto& [vi, d] : srcVerts) maxLen = std::max(maxLen, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
        if (maxLen < 1e-6f) continue;
        double sumX = 0, sumW = 0;
        for (const auto& [vi, d] : srcVerts) {
            const double w = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / maxLen;
            sumX += w * model.vertices[static_cast<size_t>(vi)].position.x;
            sumW += w;
        }
        const float centreX = sumW > 0 ? static_cast<float>(sumX / sumW) : 0.0f;
        pmx::Morph m;
        m.name = v.mmd;
        m.nameEn = v.en;
        m.panel = 3;
        for (const auto& [vi, d] : srcVerts) {
            const float w = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / maxLen;
            const float x = model.vertices[static_cast<size_t>(vi)].position.x - centreX;
            const float k = v.opening * gain;
            m.vertices.emplace_back(vi, Vec3{d.x * k + v.spread * x * w, d.y * k, d.z * k + v.forward * maxLen * w});
        }
        added.push_back(static_cast<int>(model.morphs.size()));
        model.morphs.push_back(std::move(m));
    }
    return added;
}

} // namespace cardbridge::exporter
