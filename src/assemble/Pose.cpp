#include "assemble/Pose.h"

#include "shape/Shape.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::assemble {

namespace {

Quat mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

} // namespace

bool IsIdentity(const BonePose& p) {
    return p.pos.x == 0 && p.pos.y == 0 && p.pos.z == 0 && p.rot.x == 0 && p.rot.y == 0 && p.rot.z == 0 && p.scl.x == 1 && p.scl.y == 1 && p.scl.z == 1;
}

bool Pose::empty() const {
    for (const auto& b : bones) if (!IsIdentity(b)) return false;
    for (const auto& q : base) if (q.x != 0 || q.y != 0 || q.z != 0) return false;
    return true;
}

BonePose& Pose::edit(size_t bone) {
    if (bones.size() <= bone) bones.resize(bone + 1);
    return bones[bone];
}

std::vector<Mat4> BoneWorlds(const Scene& scene, const Pose& pose) {
    const size_t n = scene.bones.size();
    std::vector<Mat4> world(n);
    std::vector<char> done(n, 0);
    // Parents come before children in a scene, but do not rely on it: resolve by walking up.
    std::vector<size_t> stack;
    for (size_t start = 0; start < n; ++start) {
        stack.assign(1, start);
        while (!stack.empty()) {
            const size_t i = stack.back();
            if (done[i]) { stack.pop_back(); continue; }
            const int p = scene.bones[i].parent;
            if (p >= 0 && static_cast<size_t>(p) < n && p != static_cast<int>(i) && !done[static_cast<size_t>(p)]) { stack.push_back(static_cast<size_t>(p)); continue; }
            const bool hasParent = p >= 0 && static_cast<size_t>(p) < n && p != static_cast<int>(i);
            const Vec3 rest = scene.bones[i].pos;
            const Vec3 local = hasParent ? sub(rest, scene.bones[static_cast<size_t>(p)].pos) : rest;
            const BonePose bp = i < pose.bones.size() ? pose.bones[i] : BonePose{};
            Quat q = shape::EulerToQuat(bp.rot);
            if (i < pose.base.size()) q = mul(pose.base[i], q);
            const Mat4 localM = Mat4::TRS(local + bp.pos, q, bp.scl);
            world[i] = hasParent ? world[static_cast<size_t>(p)] * localM : localM;
            done[i] = 1;
            stack.pop_back();
        }
    }
    return world;
}

std::vector<Mat4> SkinMatrices(const Scene& scene, const Pose& pose) {
    std::vector<Mat4> m = BoneWorlds(scene, pose);
    for (size_t i = 0; i < m.size(); ++i) m[i] = m[i] * Mat4::TRS(scene.bones[i].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
    return m;
}

Vec3 SkinPoint(const SceneVertex& v, const std::vector<Mat4>& skin) {
    Vec3 out{};
    float sum = 0;
    for (int k = 0; k < 4; ++k) {
        if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= skin.size()) continue;
        out = out + skin[static_cast<size_t>(v.bone[k])].point(v.pos) * v.weight[k];
        sum += v.weight[k];
    }
    return sum > 1e-6f ? out * (1.0f / sum) : v.pos;
}

Vec3 SkinDirection(const SceneVertex& v, const std::vector<Mat4>& skin) {
    Vec3 out{};
    float sum = 0;
    for (int k = 0; k < 4; ++k) {
        if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= skin.size()) continue;
        out = out + skin[static_cast<size_t>(v.bone[k])].dir(v.normal) * v.weight[k];
        sum += v.weight[k];
    }
    return sum > 1e-6f ? normalize(out) : v.normal;
}

bool MakeAPose(const Scene& scene, float degrees, std::vector<Quat>& base) {
    base.clear();
    if (degrees == 0.0f) return false;
    std::vector<Quat> out(scene.bones.size());
    bool any = false;
    for (const char* side : {"_L", "_R"}) {
        const int shoulder = scene.boneIndex(std::string("cf_j_shoulder") + side), arm = scene.boneIndex(std::string("cf_j_arm00") + side),
                  forearm = scene.boneIndex(std::string("cf_j_forearm01") + side);
        if (arm < 0 || forearm < 0) continue;
        const Vec3 dir = normalize(sub(scene.bones[static_cast<size_t>(forearm)].pos, scene.bones[static_cast<size_t>(arm)].pos));
        const Vec3 axis = cross(dir, Vec3{0, -1, 0});   // turning the arm about this axis by a positive angle lowers it
        const float al = len(axis);
        if (al < 1e-3f) continue;   // the arm already hangs (or points straight up): nothing to turn
        const Vec3 n = axis * (1.0f / al);
        auto turn = [&](int bone, float deg) {
            const float h = deg * 3.14159265f / 360.0f;   // half the angle in radians
            out[static_cast<size_t>(bone)] = {n.x * std::sin(h), n.y * std::sin(h), n.z * std::sin(h), std::cos(h)};
        };
        if (shoulder >= 0) { turn(shoulder, degrees * 0.2f); turn(arm, degrees * 0.8f); }
        else turn(arm, degrees);
        any = true;
    }
    if (any) base = std::move(out);
    return any;
}

void BakePose(Scene& scene, const Pose& pose) {
    if (pose.empty()) return;
    const std::vector<Mat4> skin = SkinMatrices(scene, pose);
    for (auto& mesh : scene.meshes) {
        // blend shape offsets turn with the vertex they move (rotation and scale part of its skin)
        for (auto& sh : mesh.shapes)
            for (auto& [vi, d] : sh.offsets) {
                if (vi >= mesh.vertices.size()) continue;
                const SceneVertex& v = mesh.vertices[vi];
                Vec3 acc{};
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    if (v.weight[k] <= 0 || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= skin.size()) continue;
                    acc = acc + skin[static_cast<size_t>(v.bone[k])].dir(d) * v.weight[k];
                    sum += v.weight[k];
                }
                if (sum > 1e-6f) d = acc * (1.0f / sum);
            }
        for (auto& v : mesh.vertices) {
            const Vec3 p = SkinPoint(v, skin), n = SkinDirection(v, skin);
            v.pos = p;
            v.normal = n;
        }
    }
    for (auto& c : scene.colliders) {
        if (c.bone < 0 || static_cast<size_t>(c.bone) >= skin.size()) continue;
        c.center = skin[static_cast<size_t>(c.bone)].point(c.center);
        c.axis = normalize(skin[static_cast<size_t>(c.bone)].dir(c.axis));
    }
    for (size_t i = 0; i < scene.bones.size(); ++i) scene.bones[i].pos = skin[i].point(scene.bones[i].pos);
    scene.pose.clear();
    for (size_t i = 0; i < pose.bones.size() && i < scene.bones.size(); ++i)
        if (!IsIdentity(pose.bones[i])) scene.pose.push_back({scene.bones[i].name, pose.bones[i].pos, pose.bones[i].rot, pose.bones[i].scl});
    scene.aposeApplied = false;
    for (const auto& q : pose.base) if (q.x != 0 || q.y != 0 || q.z != 0) scene.aposeApplied = true;
}

} // namespace cardbridge::assemble
