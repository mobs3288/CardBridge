#include "export/PmxIk.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::exporter {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

struct Role {
    const char* key;       // what the IK bone is called after
    const char* label;     // the limb in English
    const char* root;
    const char* mid;
    const char* end;
};
constexpr Role kRoles[] = {
    {"hand", "Arm", "cf_j_arm00_", "cf_j_forearm01_", "cf_j_hand_"},
    {"foot", "Leg", "cf_j_thigh00_", "cf_j_leg01_", "cf_j_leg03_"},
};

constexpr float kFlexMin = 0.0087f;   // 0.5 degrees: a solver must never be asked to straighten a limb exactly
constexpr float kFlexMax = 2.62f;     // 150 degrees

} // namespace

std::vector<Limb> FindLimbs(const assemble::Scene& scene) {
    std::vector<Limb> out;
    for (const Role& r : kRoles)
        for (const char* side : {"L", "R"}) {
            Limb l;
            l.role = r.key;
            l.side = side;
            l.root = scene.boneIndex(std::string(r.root) + side);
            l.mid = scene.boneIndex(std::string(r.mid) + side);
            l.end = scene.boneIndex(std::string(r.end) + side);
            if (l.root < 0 || l.mid < 0 || l.end < 0) continue;
            if (scene.bones[static_cast<size_t>(l.mid)].parent != l.root || scene.bones[static_cast<size_t>(l.end)].parent != l.mid) continue;
            out.push_back(l);
        }
    return out;
}

bool BendLimits(Vec3 root, Vec3 mid, Vec3 end, Vec3& lower, Vec3& upper) {
    const Vec3 u = sub(mid, root), l = sub(end, mid);
    const Vec3 n = cross(u, l);
    const float nl = length(n), ul = length(u), ll = length(l);
    if (ul < 1e-6f || ll < 1e-6f || nl < 1e-3f * ul * ll) return false;   // straighter than 0.06 degrees: no bend direction
    const Vec3 a{n.x / nl, n.y / nl, n.z / nl};
    // Turning the lower bone about +a bends it further from the upper one. The limit is per model axis, so the axis has to be one of them.
    const float c[3] = {a.x, a.y, a.z};
    int d = 0;
    for (int i = 1; i < 3; ++i) if (std::fabs(c[i]) > std::fabs(c[d])) d = i;
    if (std::fabs(c[d]) < 0.9f) return false;
    const float lo = c[d] > 0 ? kFlexMin : -kFlexMax, hi = c[d] > 0 ? kFlexMax : -kFlexMin;
    float lw[3] = {0, 0, 0}, up[3] = {0, 0, 0};
    lw[d] = lo;
    up[d] = hi;
    lower = {lw[0], lw[1], lw[2]};
    upper = {up[0], up[1], up[2]};
    return true;
}

std::vector<int> AddIk(const assemble::Scene& scene, pmx::Model& model, float scale) {
    std::vector<int> added;
    if (model.bones.size() < scene.bones.size()) return added;
    int rootBone = -1;
    for (size_t i = 0; i < scene.bones.size() && rootBone < 0; ++i) if (scene.bones[i].parent < 0) rootBone = static_cast<int>(i);
    for (const Limb& l : FindLimbs(scene)) {
        const Role* role = nullptr;
        for (const Role& r : kRoles) if (l.role == r.key) role = &r;
        const std::string name = "cb_ik_" + l.role + "_" + l.side;
        if (scene.boneIndex(name) >= 0) continue;   // never shadow a bone of the rig
        pmx::Bone ik;
        ik.name = name;
        ik.nameEn = std::string(l.side == "L" ? "Left " : "Right ") + role->label + " IK";
        ik.position = scene.bones[static_cast<size_t>(l.end)].pos * scale;
        ik.parent = rootBone;
        ik.flags = 0x001E;
        ik.tailOffset = {0.0f, 0.0f, 0.0f};
        ik.ikTarget = l.end;
        ik.ikLoop = 40;
        ik.ikLimit = 1.0f;
        pmx::IkLink bend;   // nearest the target first
        bend.bone = l.mid;
        bend.limited = BendLimits(scene.bones[static_cast<size_t>(l.root)].pos, scene.bones[static_cast<size_t>(l.mid)].pos, scene.bones[static_cast<size_t>(l.end)].pos, bend.lower, bend.upper);
        pmx::IkLink top;
        top.bone = l.root;
        ik.ikLinks = {bend, top};
        added.push_back(static_cast<int>(model.bones.size()));
        model.bones.push_back(std::move(ik));
    }
    return added;
}

} // namespace cardbridge::exporter
