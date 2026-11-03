#include "export/PmxPhysics.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace cardbridge::exporter {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

} // namespace

Vec3 PmxRotationFromYAxis(Vec3 axis) {
    Vec3 y = normalize(axis);
    if (length(y) < 1e-6f) return {0, 0, 0};
    // Any frame with Y on the axis: X is perpendicular to it and to the world Z (or the world X when the axis runs along Z).
    Vec3 ref = std::fabs(y.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    Vec3 x = normalize(cross(y, ref));
    Vec3 z = cross(x, y);
    // R = [x y z] as columns; R = Ry(a) Rx(b) Rz(c): R[1][2] = -sin b, R[0][2] = sin a cos b, R[2][2] = cos a cos b, R[1][0] = cos b sin c, R[1][1] = cos b cos c.
    const float r10 = x.y, r11 = y.y, r12 = z.y, r02 = z.x, r22 = z.z;
    const float b = std::asin(std::max(-1.0f, std::min(1.0f, -r12)));
    if (std::fabs(std::cos(b)) < 1e-4f) return {b, 0.0f, std::atan2(-x.z, x.x)};   // gimbal: fold the roll into the yaw
    return {b, std::atan2(r02, r22), std::atan2(r10, r11)};
}

void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale) {
    const int boneCount = static_cast<int>(scene.bones.size());
    const uint16_t kColliderMask = 0xFFFD, kChainMask = 0xFFFE;   // colliders (group 0) meet chains (group 1) and nothing else
    for (const auto& c : scene.colliders) {
        if (c.bone < 0 || c.bone >= boneCount || c.inside || c.radius <= 0.0f) continue;
        pmx::RigidBody rb;
        rb.name = "collider " + c.name;
        rb.bone = c.bone;
        rb.group = 0;
        rb.mask = kColliderMask;
        rb.mode = 0;
        rb.position = c.center * scale;
        const float cyl = c.height - 2.0f * c.radius;
        if (cyl > 1e-4f) {
            rb.shape = 2;
            rb.size = {c.radius * scale, cyl * scale, 0};
            rb.rotation = PmxRotationFromYAxis(c.axis);
        } else {
            rb.shape = 0;
            rb.size = {c.radius * scale, 0, 0};
        }
        model.rigidBodies.push_back(std::move(rb));
    }

    for (const auto& ch : scene.chains) {
        if (ch.bones.size() < 2) continue;
        std::map<int, int> bodyOf;   // scene bone -> rigid body
        std::map<int, std::vector<int>> next;   // chain bone -> its chain children
        std::set<int> inChain(ch.bones.begin(), ch.bones.end());
        for (int b : ch.bones) {
            if (b < 0 || b >= boneCount) continue;
            const int p = scene.bones[static_cast<size_t>(b)].parent;
            if (inChain.count(p) && p != b) next[p].push_back(b);
        }
        const float damp = clamp01(0.5f + 0.5f * ch.damping);
        for (size_t k = 0; k < ch.bones.size(); ++k) {
            const int b = ch.bones[k];
            if (b < 0 || b >= boneCount) continue;
            const Vec3 head = scene.bones[static_cast<size_t>(b)].pos;
            pmx::RigidBody rb;
            rb.name = ch.name + " " + std::to_string(k);
            rb.bone = b;
            rb.group = 1;
            if (k == 0) {   // the anchor follows the body and collides with nothing
                rb.mask = 0xFFFF;
                rb.mode = 0;
                rb.shape = 0;
                rb.size = {0.01f * scale, 0, 0};
                rb.position = head * scale;
                bodyOf[b] = static_cast<int>(model.rigidBodies.size());
                model.rigidBodies.push_back(std::move(rb));
                continue;
            }
            // The body spans from this bone to its first chain child; a leaf continues in the direction it came from.
            Vec3 dir;
            float len;
            auto nx = next.find(b);
            if (nx != next.end() && !nx->second.empty()) {
                dir = sub(scene.bones[static_cast<size_t>(nx->second[0])].pos, head);
                len = length(dir);
            } else {
                const int p = scene.bones[static_cast<size_t>(b)].parent;
                dir = p >= 0 ? sub(head, scene.bones[static_cast<size_t>(p)].pos) : Vec3{0, -1, 0};
                len = length(dir) * 0.5f;
            }
            if (len < 1e-5f) { dir = {0, -1, 0}; len = 0.02f; }
            const Vec3 unit = normalize(dir);
            float radius = ch.radius > 0.0f ? ch.radius : std::min(0.03f, std::max(0.005f, len * 0.15f));
            const float cyl = std::max(0.0f, len - 2.0f * radius);
            rb.shape = cyl > 1e-4f ? 2 : 0;
            rb.size = {radius * scale, cyl * scale, 0};
            rb.position = (head + unit * (len * 0.5f)) * scale;
            if (rb.shape == 2) rb.rotation = PmxRotationFromYAxis(unit);
            rb.mass = 1.0f;
            rb.moveDamping = damp;
            rb.rotDamping = damp;
            rb.friction = 0.5f;
            rb.mask = kChainMask;
            rb.mode = 1;
            bodyOf[b] = static_cast<int>(model.rigidBodies.size());
            model.rigidBodies.push_back(std::move(rb));
        }
        for (size_t k = 1; k < ch.bones.size(); ++k) {
            const int b = ch.bones[k];
            auto self = bodyOf.find(b);
            if (self == bodyOf.end()) continue;
            int parent = scene.bones[static_cast<size_t>(b)].parent;
            auto pb = bodyOf.find(parent);
            if (pb == bodyOf.end()) pb = bodyOf.find(ch.bones[0]);   // a bone whose parent is outside the chain hangs from the anchor
            if (pb == bodyOf.end()) continue;
            pmx::Joint j;
            j.name = model.rigidBodies[static_cast<size_t>(self->second)].name;
            j.bodyA = pb->second;
            j.bodyB = self->second;
            j.position = scene.bones[static_cast<size_t>(b)].pos * scale;
            j.rotation = model.rigidBodies[static_cast<size_t>(self->second)].rotation;
            const float limit = (0.2f + 0.8f * (1.0f - clamp01(ch.stiffness))) * 1.2f;   // radians: a stiff chain barely turns
            j.rotLower = {-limit, -limit * 0.5f, -limit};
            j.rotUpper = {limit, limit * 0.5f, limit};
            j.springRot = {50.0f + 150.0f * ch.elasticity, 50.0f + 150.0f * ch.elasticity, 50.0f + 150.0f * ch.elasticity};
            model.joints.push_back(std::move(j));
        }
    }
}

size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale) {
    size_t added = 0;
    auto push = [&](const std::string& name, int panel, std::map<int, Vec3>& acc) {
        pmx::Morph m;
        m.name = name;
        m.panel = panel;
        for (const auto& [v, d] : acc)
            if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) > 1e-6f) m.vertices.emplace_back(v, d);
        if (m.vertices.empty()) return;
        model.morphs.push_back(std::move(m));
        ++added;
    };
    for (const auto& ex : scene.expressions) {
        std::map<int, Vec3> acc;
        for (const auto& t : ex.targets) {
            if (t.mesh < 0 || static_cast<size_t>(t.mesh) >= scene.meshes.size() || static_cast<size_t>(t.mesh) >= meshBase.size() || meshBase[static_cast<size_t>(t.mesh)] < 0) continue;
            const auto& shapes = scene.meshes[static_cast<size_t>(t.mesh)].shapes;
            if (t.shape < 0 || static_cast<size_t>(t.shape) >= shapes.size()) continue;
            const float k = (t.weight - shapes[static_cast<size_t>(t.shape)].rest) * 0.01f * scale;
            for (const auto& [vi, d] : shapes[static_cast<size_t>(t.shape)].offsets) acc[meshBase[static_cast<size_t>(t.mesh)] + static_cast<int>(vi)] = acc[meshBase[static_cast<size_t>(t.mesh)] + static_cast<int>(vi)] + d * k;
        }
        push(ex.name, ex.panel, acc);
    }
    for (size_t mi = 0; mi < scene.meshes.size() && mi < meshBase.size(); ++mi) {
        if (meshBase[mi] < 0) continue;
        for (const auto& sh : scene.meshes[mi].shapes) {
            if (sh.offsets.empty()) continue;
            std::map<int, Vec3> acc;
            const float k = (100.0f - sh.rest) * 0.01f * scale;
            for (const auto& [vi, d] : sh.offsets) acc[meshBase[mi] + static_cast<int>(vi)] = d * k;
            // Panel by the names the game gives its face meshes and shapes (mayuge = brows, eyeline / namida = eyes, kuti / tooth / tang = mouth).
            const std::string full = scene.meshes[mi].name + "." + sh.name;
            auto has = [&](const char* s) { return full.find(s) != std::string::npos; };
            const int panel = has("mayu") ? 1 : (has("eye") || has("namida") || has("gag")) ? 2 : (has("kuti") || has("tooth") || has("tang") || has("mouth")) ? 3 : 4;
            push(full, panel, acc);
        }
    }
    return added;
}

} // namespace cardbridge::exporter
