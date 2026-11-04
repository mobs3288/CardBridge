#include "export/PmxPhysics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace cardbridge::exporter {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// What one link of a chain is, in PMX units (the model scale already applied).
struct Link {
    int bone = -1;
    Vec3 head, centre, unit;
    float len = 0.0f, radius = 0.0f, cylinder = 0.0f, mass = 1.0f;
};

// Bullet's capsule inertia (btCapsuleShape::calculateLocalInertia): the box that bounds the capsule, about the centre; the axis is Y.
// Returns the inertia about an axis across the capsule.
float capsuleInertiaAcross(float mass, float radius, float cylinder) {
    const float lx = 2.0f * radius, ly = cylinder + 2.0f * radius;
    return mass * (1.0f / 12.0f) * (ly * ly + lx * lx);
}

// A rigid body's core as a segment (a point for a sphere) and its radius, for the rest-pose overlap test.
struct Core { Vec3 a, b; float r = 0.0f; };

} // namespace

// The fastest a single link may swing on its joint's spring, radians a second (the solver is stable well below half its step rate).
constexpr float kMaxBodySwing = 15.0f;

// The least distance between the segments a-b and c-d (a segment may be a point).
float SegmentDistance(Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    auto dot = [](Vec3 u, Vec3 v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
    const Vec3 u = sub(b, a), v = sub(d, c), w = sub(a, c);
    const float uu = dot(u, u), vv = dot(v, v), uv = dot(u, v), uw = dot(u, w), vw = dot(v, w);
    float s = 0.0f, t = 0.0f;
    const float den = uu * vv - uv * uv;
    if (uu < 1e-12f && vv < 1e-12f) return length(w);
    if (uu < 1e-12f) t = clamp01(vw / vv);
    else if (vv < 1e-12f) s = clamp01(-uw / uu);
    else {
        s = den > 1e-12f ? clamp01((uv * vw - vv * uw) / den) : 0.0f;
        t = (uv * s + vw) / vv;
        if (t < 0.0f) { t = 0.0f; s = clamp01(-uw / uu); }
        else if (t > 1.0f) { t = 1.0f; s = clamp01((uv - uw) / uu); }
    }
    const Vec3 p = {w.x + u.x * s - v.x * t, w.y + u.y * s - v.y * t, w.z + u.z * s - v.z * t};
    return length(p);
}

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
    const float gravity = 9.8f * scale;   // model units a second squared
    std::vector<Core> colliderCore;       // per collider body, in the order they were added
    std::vector<int> colliderBody;        // its rigid body index
    for (const auto& c : scene.colliders) {
        if (c.bone < 0 || c.bone >= boneCount || c.inside || c.radius <= 0.0f) continue;
        pmx::RigidBody rb;
        rb.name = "collider " + c.name;
        rb.bone = c.bone;
        rb.group = 0;   // the collider groups are assigned below
        rb.mask = 1u << kPmxGroupChain;   // colliders meet chains and nothing else
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
        Core core;
        core.r = c.radius * scale;
        core.a = core.b = c.center * scale;
        if (cyl > 1e-4f) {
            const Vec3 half = normalize(c.axis) * (0.5f * cyl * scale);
            core.a = sub(c.center * scale, half);
            core.b = c.center * scale + half;
        }
        colliderCore.push_back(core);
        colliderBody.push_back(static_cast<int>(model.rigidBodies.size()));
        model.rigidBodies.push_back(std::move(rb));
    }
    std::vector<Core> chainCore;   // per moving chain body
    std::vector<int> chainBody;

    for (const auto& ch : scene.chains) {
        if (ch.bones.size() < 2) continue;
        // The body's own jiggle chains (breast, hip) are skin on the body, not cloth: whatever their limits, a light link carrying vertices of
        // the body shivers in MikuMikuDance (tried again with a 7 degree swing: the bust shook visibly at rest). They stay rigid in the PMX; the
        // game's jiggle is not carried.
        if (ch.characterOwn) continue;
        std::vector<int> bones;
        for (int b : ch.bones) if (b >= 0 && b < boneCount) bones.push_back(b);
        if (bones.size() < 2) continue;
        const std::set<int> inChain(bones.begin(), bones.end());
        std::map<int, std::vector<int>> next;   // chain bone -> its chain children
        for (int b : bones) {
            const int p = scene.bones[static_cast<size_t>(b)].parent;
            if (inChain.count(p) && p != b) next[p].push_back(b);
        }

        // One link per bone, as the component moves them: the root turns to point at its first child too, so the whole chain swings,
        // each bone's body spanning from its head to its first chain child (a leaf goes on in the direction it came from).
        std::map<int, Link> links;
        for (int b : bones) {
            const Vec3 head = scene.bones[static_cast<size_t>(b)].pos * scale;
            Link l;
            l.bone = b;
            l.head = head;
            Vec3 dir;
            auto nx = next.find(b);
            if (nx != next.end() && !nx->second.empty()) {
                dir = sub(scene.bones[static_cast<size_t>(nx->second[0])].pos * scale, head);
                l.len = length(dir);
            } else {
                const int p = scene.bones[static_cast<size_t>(b)].parent;
                dir = p >= 0 ? sub(head, scene.bones[static_cast<size_t>(p)].pos * scale) : Vec3{0, -1, 0};
                l.len = length(dir) * 0.5f;
            }
            if (l.len < 1e-5f * scale) { dir = {0, -1, 0}; l.len = 0.02f * scale; }
            l.unit = normalize(dir);
            l.radius = (ch.radius > 0.0f ? ch.radius : std::min(0.03f, std::max(0.005f, l.len / scale * 0.15f))) * scale;
            l.cylinder = std::max(0.0f, l.len - 2.0f * l.radius);
            l.centre = head + l.unit * (l.len * 0.5f);
            l.mass = std::max(0.05f, l.len / scale * 10.0f);   // a tenth of a metre of strand is a unit of mass
            links[b] = l;
        }

        // The component's damping, elasticity and stiffness vary along the chain by its distribution curves (the scene carries the result per bone).
        std::map<int, size_t> chainIndex;
        for (size_t i = 0; i < ch.bones.size(); ++i) chainIndex[ch.bones[i]] = i;
        auto along = [&](const std::vector<float>& perBone, float whole, int bone) {
            auto it = chainIndex.find(bone);
            return perBone.empty() || it == chainIndex.end() || it->second >= perBone.size() ? whole : perBone[it->second];
        };

        std::map<int, int> bodyOf;   // chain bone -> rigid body
        for (size_t k = 0; k < bones.size(); ++k) {
            const Link& l = links[bones[k]];
            pmx::RigidBody rb;
            rb.name = ch.name + " " + std::to_string(k);
            rb.bone = l.bone;
            rb.group = kPmxGroupChain;
            rb.mask = 0;   // set below: the colliders it does not already overlap
            rb.shape = l.cylinder > 1e-4f ? 2 : 0;
            rb.size = {l.radius, l.cylinder, 0};
            rb.position = l.centre;
            if (rb.shape == 2) rb.rotation = PmxRotationFromYAxis(l.unit);
            rb.mass = l.mass;
            // The component's damping is a fraction of the velocity lost each update; Bullet's is a rate a second (v *= (1 - d)^dt).
            // 60 * damping is the same decay in both, so the swing dies away at the same pace.
            const float decay = std::max(0.6f, 60.0f * along(ch.boneDamping, ch.damping, l.bone));
            const float damp = std::min(0.9995f, 1.0f - std::exp(-decay));
            rb.moveDamping = damp;
            rb.rotDamping = damp;
            rb.friction = 0.5f;
            // The root follows its bone and collides with nothing: it is what the chain hangs from. (A moving root, MMD's "physics with the bone
            // aligned" mode, fed back into the bone below it and shook the whole model, breasts and thighs included.)
            rb.mode = k == 0 ? 0 : 1;
            if (k == 0) { rb.mass = 1.0f; rb.moveDamping = rb.rotDamping = 0.0f; }
            bodyOf[l.bone] = static_cast<int>(model.rigidBodies.size());
            if (k > 0) {
                chainBody.push_back(static_cast<int>(model.rigidBodies.size()));
                chainCore.push_back({sub(l.centre, l.unit * (0.5f * l.cylinder)), l.centre + l.unit * (0.5f * l.cylinder), l.radius});
            }
            model.rigidBodies.push_back(std::move(rb));
        }

        // Everything hanging from a bone, itself included.
        std::function<void(int, std::vector<int>&)> below = [&](int b, std::vector<int>& out) {
            out.push_back(b);
            auto nx = next.find(b);
            if (nx != next.end()) for (int c : nx->second) below(c, out);
        };
        for (size_t k = 1; k < bones.size(); ++k) {
            const int b = bones[k];
            const Link& self = links[b];
            // The component pulls a particle back by `elasticity` of its offset each update: a spring of about 60 * sqrt(elasticity) rad/s.
            const float omega = std::min(25.0f, 60.0f * std::sqrt(std::max(0.005f, std::min(1.0f, along(ch.boneElasticity, ch.elasticity, b)))));
            // The component keeps a particle within 2 * (1 - stiffness) of a bone length of its rest place: that is a turn of 2 asin(1 - stiffness).
            // MikuMikuDance is kinder to a calm default than to the game's loosest settings: no link turns more than 35 degrees.
            const float stiffness = along(ch.boneStiffness, ch.stiffness, b);
            const float bend = std::min(0.6f, std::max(0.05f, 2.0f * std::asin(clamp01(1.0f - stiffness))));
            // How far a strand may droop when it stands out sideways: a loose chain gives way, a stiff one hardly (never more than half the limit).
            const float droop = std::min(0.5f * bend, 0.25f - 0.15f * clamp01(stiffness * 2.0f));
            const int parentBone = scene.bones[static_cast<size_t>(b)].parent;
            auto pb = bodyOf.find(parentBone);
            const int bodyA = pb != bodyOf.end() ? pb->second : bodyOf[bones[0]];   // a bone whose parent is outside the chain hangs from the root
            // The spring at this joint carries everything below it: its inertia gives the swing rate, its weight when it stands out
            // sideways gives the least stiffness that keeps the droop small.
            std::vector<int> hanging;
            below(b, hanging);
            float inertia = 0.0f, lever = 0.0f, sideways = 0.0f;   // sideways: the weight's torque about the joint at rest (gravity pulls down)
            for (int d : hanging) {
                const Link& l = links[d];
                const float r = length(sub(l.centre, self.head));
                inertia += l.mass * r * r + capsuleInertiaAcross(l.mass, l.radius, l.cylinder);
                lever += l.mass * r;
                const Vec3 arm = sub(l.centre, self.head);
                sideways += l.mass * std::sqrt(arm.x * arm.x + arm.z * arm.z);
            }
            // The spring carries everything below it, but it acts on this one body: a spring stiff enough to hold a long chain makes a light
            // link ring at hundreds of radians a second, which MMD's solver turns into a permanent shiver. The body's own swing rate is capped.
            const float own = self.mass * length(sub(self.centre, self.head)) * length(sub(self.centre, self.head)) + capsuleInertiaAcross(self.mass, self.radius, self.cylinder);
            const float stiffnessK = std::min(std::max(inertia * omega * omega, lever * gravity / droop), own * kMaxBodySwing * kMaxBodySwing);
            pmx::Joint j;
            j.name = ch.name + " " + std::to_string(k);
            j.bodyA = bodyA;
            j.bodyB = bodyOf[b];
            j.position = self.head;
            j.rotation = model.rigidBodies[static_cast<size_t>(bodyOf[b])].rotation;
            // A link that gravity pushes further than its spring can hold inside the limits would rest on the stop, and a body resting on a
            // joint limit shivers in MMD: such a joint is locked (a zero range) instead.
            const bool holds = sideways * gravity / stiffnessK < 0.6f * bend;
            const float range = holds ? bend : 0.0f;
            j.rotLower = {-range, -0.1f * range, -range};
            j.rotUpper = {range, 0.1f * range, range};
            j.springRot = {stiffnessK, stiffnessK, stiffnessK};
            model.joints.push_back(std::move(j));
        }
    }

    // Who collides with whom. A body that starts inside a collider (a skirt over the thigh, a breast bone in the bust sphere, a strand
    // in the head) is thrown out of it the moment the simulation starts, and the whole model with it. So a body meets only the colliders
    // it does not overlap in the rest pose: colliders that overlap the same bodies share a group, and a body's mask lists the groups none
    // of whose colliders it overlaps. Group 0 (overlaps nothing) is met by every body; at most kPmxMaxColliderGroups groups, the last one
    // takes any signature beyond them (so it excludes the union).
    const float margin = 0.02f * scale;
    std::map<std::vector<int>, int> groupOf;   // overlapped bodies -> collider group
    std::vector<std::set<int>> overlapped(16);
    for (size_t ci = 0; ci < colliderCore.size(); ++ci) {
        std::vector<int> sig;
        for (size_t bi = 0; bi < chainCore.size(); ++bi)
            if (SegmentDistance(colliderCore[ci].a, colliderCore[ci].b, chainCore[bi].a, chainCore[bi].b) < colliderCore[ci].r + chainCore[bi].r + margin) sig.push_back(static_cast<int>(bi));
        int group = 0;
        if (!sig.empty()) {
            auto it = groupOf.find(sig);
            if (it != groupOf.end()) group = it->second;
            else {
                group = std::min(15, 2 + static_cast<int>(groupOf.size()));
                groupOf[sig] = group;
            }
        }
        model.rigidBodies[static_cast<size_t>(colliderBody[ci])].group = static_cast<uint8_t>(group);
        overlapped[static_cast<size_t>(group)].insert(sig.begin(), sig.end());
    }
    std::set<int> usedGroups = {0};
    for (const auto& [sig, group] : groupOf) usedGroups.insert(group);
    for (size_t bi = 0; bi < chainBody.size(); ++bi) {
        uint16_t mask = 0;
        for (int g : usedGroups)
            if (!overlapped[static_cast<size_t>(g)].count(static_cast<int>(bi))) mask |= static_cast<uint16_t>(1u << g);
        model.rigidBodies[static_cast<size_t>(chainBody[bi])].mask = mask;
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
