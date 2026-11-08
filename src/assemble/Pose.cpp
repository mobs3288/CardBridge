#include "assemble/Pose.h"

#include "assemble/PhysicsSim.h"
#include "shape/Shape.h"

#include <nlohmann/json.hpp>

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
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

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

// Rodrigues' formula; half a turn about a perpendicular axis when the directions are opposite.
Mat4 RotationBetween(Vec3 from, Vec3 to) {
    const Vec3 a = normalize(from), b = normalize(to);
    Mat4 r;
    const Vec3 v = cross(a, b);
    const float c = dot(a, b);
    if (c < -0.99999f) {   // opposite: half a turn about any perpendicular axis
        Vec3 axis = std::fabs(a.x) < 0.9f ? cross(a, Vec3{1, 0, 0}) : cross(a, Vec3{0, 1, 0});
        axis = normalize(axis);
        r.m[0] = 2 * axis.x * axis.x - 1; r.m[1] = 2 * axis.x * axis.y;     r.m[2] = 2 * axis.x * axis.z;
        r.m[4] = 2 * axis.y * axis.x;     r.m[5] = 2 * axis.y * axis.y - 1; r.m[6] = 2 * axis.y * axis.z;
        r.m[8] = 2 * axis.z * axis.x;     r.m[9] = 2 * axis.z * axis.y;     r.m[10] = 2 * axis.z * axis.z - 1;
        return r;
    }
    const float k = 1.0f / (1.0f + c);
    r.m[0] = c + v.x * v.x * k;  r.m[1] = v.x * v.y * k - v.z; r.m[2] = v.x * v.z * k + v.y;
    r.m[4] = v.y * v.x * k + v.z; r.m[5] = c + v.y * v.y * k;  r.m[6] = v.y * v.z * k - v.x;
    r.m[8] = v.z * v.x * k - v.y; r.m[9] = v.z * v.y * k + v.x; r.m[10] = c + v.z * v.z * k;
    return r;
}

Vec3 EulerFromMatrix(const Mat4& r) {
    const float rad = 180.0f / 3.14159265f;
    const float x = std::asin(std::max(-1.0f, std::min(1.0f, -r.m[6])));
    float y, z;
    if (std::fabs(std::cos(x)) > 1e-4f) { y = std::atan2(r.m[2], r.m[10]); z = std::atan2(r.m[4], r.m[5]); }
    else { y = std::atan2(-r.m[8], r.m[0]); z = 0.0f; }   // straight up or down: all the turn goes into y
    return {x * rad, y * rad, z * rad};
}

namespace {
Mat4 rotationOnly(const Mat4& m) {
    Mat4 r = m;
    r.m[3] = r.m[7] = r.m[11] = 0.0f;
    return r;
}
} // namespace

Vec3 TurnBone(const Scene& scene, const Pose& pose, size_t bone, const Mat4& worldTurn) {
    if (bone >= scene.bones.size()) return {};
    const std::vector<Mat4> w = BoneWorlds(scene, pose);
    const int p = scene.bones[bone].parent;
    Mat4 parentRot;
    if (p >= 0 && static_cast<size_t>(p) < w.size() && p != static_cast<int>(bone)) parentRot = rotationOnly(w[static_cast<size_t>(p)]);
    const Mat4 base = bone < pose.base.size() ? Mat4::TRS({0, 0, 0}, pose.base[bone], {1, 1, 1}) : Mat4();
    const Mat4 frame = parentRot * base;   // what the edit's rotation is applied in
    const Vec3 old = bone < pose.bones.size() ? pose.bones[bone].rot : Vec3{};
    const Mat4 user = Mat4::TRS({0, 0, 0}, shape::EulerToQuat(old), {1, 1, 1});
    return EulerFromMatrix(inverse(frame) * worldTurn * frame * user);
}

Vec3 MoveBone(const Scene& scene, const Pose& pose, size_t bone, Vec3 worldDelta) {
    if (bone >= scene.bones.size()) return {};
    const std::vector<Mat4> w = BoneWorlds(scene, pose);
    const int p = scene.bones[bone].parent;
    Mat4 parentRot;
    if (p >= 0 && static_cast<size_t>(p) < w.size() && p != static_cast<int>(bone)) parentRot = rotationOnly(w[static_cast<size_t>(p)]);
    const Vec3 old = bone < pose.bones.size() ? pose.bones[bone].pos : Vec3{};
    return old + inverse(parentRot).dir(worldDelta);
}

void BakeSkin(Scene& scene, const std::vector<Mat4>& skin) {
    if (skin.empty()) return;
    for (auto& mesh : scene.meshes) {
        mesh.reshapable = false;
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
}

void BakePose(Scene& scene, const Pose& pose) {
    if (pose.empty()) return;
    BakeSkin(scene, SkinMatrices(scene, pose));
    scene.pose.clear();
    for (size_t i = 0; i < pose.bones.size() && i < scene.bones.size(); ++i)
        if (!IsIdentity(pose.bones[i])) scene.pose.push_back({scene.bones[i].name, pose.bones[i].pos, pose.bones[i].rot, pose.bones[i].scl});
    scene.aposeApplied = false;
    for (const auto& q : pose.base) if (q.x != 0 || q.y != 0 || q.z != 0) scene.aposeApplied = true;
}

void BakeSettledPhysics(Scene& scene, bool jiggleBust, bool jiggleButt, float jiggleIntensity) {
    if (scene.bones.empty() || (!jiggleBust && !jiggleButt)) return;
    const Pose rest;
    const std::vector<Mat4> poseWorld = BoneWorlds(scene, rest);
    PhysicsSim sim;
    sim.Reset(scene, poseWorld, jiggleBust, jiggleButt, jiggleIntensity);
    // Empty sim means no jiggle chains were found (Reset left nothing): nothing to settle.
    // PhysicsSim exposes its emptiness through Step's contract: a fresh sim with no chains writes nothing.
    std::vector<Mat4> skin = SkinMatrices(scene, rest);
    bool anyMoved = false;
    int ran = 0;
    for (int step = 0; step < 600; ++step) {
        ++ran;
        const bool moved = sim.Step(scene, poseWorld, 1, skin);
        anyMoved = anyMoved || moved;
        if (!moved) break;
    }
    if (std::getenv("CARDBRIDGE_DEBUG_SETTLE")) {
        float maxMove = 0.0f;
        for (size_t i = 0; i < skin.size() && i < scene.bones.size(); ++i) {
            const Vec3 a = poseWorld[i].translation(), b = skin[i].point(scene.bones[i].pos);
            const float d = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            if (d > maxMove) maxMove = d;
        }
        std::fprintf(stderr, "settle: %d steps, max jiggle bone move %.4f m, moved=%s\n", ran, maxMove, anyMoved ? "yes" : "no");
        for (const auto& ch : scene.chains) {
            if (!ch.characterOwn) continue;
            std::fprintf(stderr, "settle chain %s damp=%.3f elas=%.3f stiff=%.3f inert=%.3f rad=%.3f grav(%.3f %.3f %.3f) force(%.3f %.3f %.3f) bones", ch.name.c_str(),
                         ch.damping, ch.elasticity, ch.stiffness, ch.inert, ch.radius, ch.gravity.x, ch.gravity.y, ch.gravity.z, ch.force.x, ch.force.y, ch.force.z);
            for (int b : ch.bones) {
                const Vec3 a = poseWorld[static_cast<size_t>(b)].translation();
                const Vec3 c = skin[static_cast<size_t>(b)].point(scene.bones[static_cast<size_t>(b)].pos);
                const Vec3 r0 = poseWorld[static_cast<size_t>(b)].dir({0, 0, 1});
                const Mat4 wr = skin[static_cast<size_t>(b)];
                const Vec3 r1 = wr.dir({0, 0, 1});
                std::fprintf(stderr, " [%s rest(%.3f %.3f %.3f) settled(%.3f %.3f %.3f) z dir rest(%.3f %.3f %.3f) settled(%.3f %.3f %.3f)]",
                             scene.bones[static_cast<size_t>(b)].name.c_str(),
                             a.x, a.y, a.z, c.x, c.y, c.z, r0.x, r0.y, r0.z, r1.x, r1.y, r1.z);
            }
            std::fprintf(stderr, "\n");
        }
    }
    // Step touches only the jiggle chain bones: anything it displaced is real sag, so bake it in.
    if (anyMoved) BakeSkin(scene, skin);
}

std::string PoseFileToJson(const PoseFile& file) {
    using json = nlohmann::json;
    json j;
    j["format"] = "cardbridge-pose";
    j["version"] = 1;
    j["a_pose"] = file.aPose;
    j["a_pose_degrees"] = file.aPoseDegrees;
    j["bones"] = json::object();
    for (const auto& [name, e] : file.bones)
        j["bones"][name] = {{"position", {e.pos.x, e.pos.y, e.pos.z}}, {"rotation_euler_degrees", {e.rot.x, e.rot.y, e.rot.z}}, {"scale", {e.scl.x, e.scl.y, e.scl.z}}};
    return j.dump(2) + "\n";
}

bool PoseFileFromJson(const std::string& text, PoseFile& out, std::string* error) {
    using json = nlohmann::json;
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
    if (j.value("format", std::string()) != "cardbridge-pose") return fail("not a cardbridge pose file (\"format\" is missing or different)");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() > 1) return fail("a newer pose file version than this program reads");
    PoseFile f;
    if (j.contains("a_pose")) { if (!j["a_pose"].is_boolean()) return fail("\"a_pose\" is not true or false"); f.aPose = j["a_pose"].get<bool>(); }
    if (j.contains("a_pose_degrees")) { if (!j["a_pose_degrees"].is_number()) return fail("\"a_pose_degrees\" is not a number"); f.aPoseDegrees = j["a_pose_degrees"].get<float>(); }
    if (j.contains("bones")) {
        if (!j["bones"].is_object()) return fail("\"bones\" is not an object");
        auto vec = [&](const json& b, const char* key, Vec3& v) {
            if (!b.contains(key)) return true;
            const json& a = b[key];
            if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() || !a[2].is_number()) return false;
            v = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
            return true;
        };
        for (const auto& [name, b] : j["bones"].items()) {
            BonePose e;
            if (!b.is_object() || !vec(b, "position", e.pos) || !vec(b, "rotation_euler_degrees", e.rot) || !vec(b, "scale", e.scl))
                return fail("bone \"" + name + "\" needs position, rotation_euler_degrees and scale as three numbers each");
            f.bones.emplace_back(name, e);
        }
    }
    out = std::move(f);
    return true;
}

Pose MakePose(const Scene& scene, const PoseFile& file, std::vector<std::string>* unknown) {
    Pose p;
    for (const auto& [name, e] : file.bones) {
        const int b = scene.boneIndex(name);
        if (b < 0) { if (unknown) unknown->push_back(name); continue; }
        p.edit(static_cast<size_t>(b)) = e;
    }
    if (file.aPose) MakeAPose(scene, file.aPoseDegrees, p.base);
    return p;
}

} // namespace cardbridge::assemble
