#include "export/PmxPhysics.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
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

// How much of a leg's turn a garment hanging straight in front of that thigh takes (less as it hangs further round to the side).
// A skirt flares out from the thighs at rest: following the whole turn lifted it off them and showed its lining (MMD, crouching).
constexpr float kLegFollow = 0.75f;

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

std::vector<int> PhysicsChainBones(const assemble::Scene& scene, const assemble::SceneChain& chain) {
    const int boneCount = static_cast<int>(scene.bones.size());
    std::vector<int> bones;
    for (int b : chain.bones) if (b >= 0 && b < boneCount) bones.push_back(b);
    const std::set<int> inChain(bones.begin(), bones.end());
    std::set<int> hasChild;
    for (int b : bones) {
        const int p = scene.bones[static_cast<size_t>(b)].parent;
        if (p != b) hasChild.insert(p);
    }
    std::vector<int> kept;
    for (int b : bones) {
        const int p = scene.bones[static_cast<size_t>(b)].parent;
        const int gp = p >= 0 ? scene.bones[static_cast<size_t>(p)].parent : -1;
        if (!hasChild.count(b) && inChain.count(p) && gp >= 0) {
            const Vec3 out = sub(scene.bones[static_cast<size_t>(b)].pos, scene.bones[static_cast<size_t>(p)].pos);
            const Vec3 in = sub(scene.bones[static_cast<size_t>(p)].pos, scene.bones[static_cast<size_t>(gp)].pos);
            if (out.x * in.x + out.y * in.y + out.z * in.z < 0.0f) continue;
        }
        kept.push_back(b);
    }
    return kept;
}

void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale, bool jiggleBust, bool jiggleButt, float jiggleIntensity) {
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
    std::vector<char> chainJiggle;   // the same, per body: the body's own jiggle (skin, not a strand: it meets no collider)

    for (const auto& ch : scene.chains) {
        if (ch.bones.size() < 2) continue;
        // The body's own jiggle chains (breast, hip): skin on the body, not cloth. They are jiggle physics when their switch is on, built
        // like the hand-made MMD models' breast jiggle (mode-2 bodies, fat capsules with weight tapering to the tip, heavy damping, no
        // collider), plus our own joint spring: the reference models' bare pendulum hangs displaced under gravity and wobbles like jelly,
        // the spring pulls the part back to the rest pose so it bounces like skin instead.
        float jiggle = 0.0f;   // > 0: this chain is enabled jiggle, the intensity to swing at
        bool jigBust = false;  // which jiggle: the bust gets a wider envelope than the butt
        if (ch.characterOwn) {
            const bool bust = assemble::IsBustJiggleChain(scene, ch);
            if (bust && jiggleBust) jiggle = jiggleIntensity;
            else if (!bust && jiggleButt) jiggle = jiggleIntensity;
            if (jiggle <= 0.0f) continue;   // switch off: the part stays rigid, following its bone
            jigBust = bust;
        }
        std::vector<int> bones = PhysicsChainBones(scene, ch);
        if (std::getenv("CARDBRIDGE_DEBUG_CHAINS")) {
            std::fprintf(stderr, "chain '%s' own=%d raw=[", ch.name.c_str(), ch.characterOwn ? 1 : 0);
            for (int b : ch.bones) std::fprintf(stderr, "%s ", (b >= 0 && static_cast<size_t>(b) < scene.bones.size()) ? scene.bones[static_cast<size_t>(b)].name.c_str() : "?");
            std::fprintf(stderr, "] pruned=[");
            for (int b : bones) std::fprintf(stderr, "%s ", scene.bones[static_cast<size_t>(b)].name.c_str());
            std::fprintf(stderr, "]\n");
        }
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
            if (jiggle > 0.0f) l.mass = std::pow(0.5f, static_cast<float>(links.size()));   // the reference models' taper: 1, 1/2, 1/4... toward the tip
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
        int jigAnchor = -1;   // a jiggle chain's anchor: a kinematic body on the parent bone, rigid-jointed to the root, like the reference models' torso body
        Vec3 jigAnchorPos;   // the anchor body's authored position (its bone's), the anchor joint's place
        for (size_t k = 0; k < bones.size(); ++k) {
            const Link& l = links[bones[k]];
            if (jiggle > 0.0f && k == 0) {
                const int parent = scene.bones[static_cast<size_t>(l.bone)].parent;
                if (parent >= 0) {
                    // The anchor is authored at its OWN bone's position, like the reference models' torso body. Authored at the root
                    // body's place instead, the kinematic body snaps inward to the parent bone every frame and the rigid joint drags the
                    // whole chain inward with it: the part twitches toward the inside and vibrates.
                    const Vec3 parentPos = scene.bones[static_cast<size_t>(parent)].pos * scale;
                    pmx::RigidBody anchor;
                    anchor.name = ch.name + " anchor";
                    anchor.bone = parent;
                    anchor.group = 0;
                    anchor.mask = 0x0002;   // meets chains, like the other kinematic bodies
                    anchor.shape = l.cylinder > 1e-4f ? 2 : 0;
                    anchor.size = {std::max(l.radius, 0.6f * l.cylinder), l.cylinder, 0};
                    anchor.position = parentPos;
                    if (anchor.shape == 2) anchor.rotation = PmxRotationFromYAxis(l.unit);
                    anchor.mass = 1.0f;
                    anchor.mode = 0;   // kinematic: it follows the parent bone, and the rigid joint carries the jiggle root with it
                    jigAnchor = static_cast<int>(model.rigidBodies.size());
                    jigAnchorPos = parentPos;
                    model.rigidBodies.push_back(std::move(anchor));
                }
            }
            pmx::RigidBody rb;
            rb.name = ch.name + " " + std::to_string(k);
            rb.bone = l.bone;
            rb.group = kPmxGroupChain;
            rb.mask = 0;   // set below: the colliders it does not already overlap
            rb.shape = l.cylinder > 1e-4f ? 2 : 0;
            // The jiggle bodies are the reference models' breast bodies: fat capsules (about as wide as a link is long), weight tapering
            // toward the tip, so the swing is a smooth body sway rather than a thin strand snapping.
            const float jigRadius = std::max(l.radius, 0.6f * l.cylinder);
            rb.size = {jiggle > 0.0f ? jigRadius : l.radius, l.cylinder, 0};
            rb.position = l.centre;
            if (rb.shape == 2) rb.rotation = PmxRotationFromYAxis(l.unit);
            rb.mass = jiggle > 0.0f ? std::pow(0.5f, static_cast<float>(k)) : l.mass;   // 1, 1/2, 1/4, ... toward the tip
            // The component's damping is a fraction of the velocity lost each update; Bullet's is a rate a second (v *= (1 - d)^dt).
            // 60 * damping is the same decay in both, so the swing dies away at the same pace.
            float decay = std::max(0.6f, 60.0f * along(ch.boneDamping, ch.damping, l.bone));
            // Jiggle bodies take the reference models' fixed heavy damping, not the game's: their swing dies out the same way every time.
            const float damp = jiggle > 0.0f ? 0.9f : std::min(0.9995f, 1.0f - std::exp(-decay));
            rb.moveDamping = damp;
            rb.rotDamping = damp;
            rb.friction = jiggle > 0.0f ? 0.0f : 0.5f;
            // The root follows its bone and collides with nothing: it is what the chain hangs from. (A moving root on a HAIR chain, in
            // MMD's "physics with the bone aligned" mode, fed back into the bone below it and shook the whole model.) The jiggle bodies
            // are the reference models' breast bodies instead: every one is that mode, the body following its bone with physics on top.
            rb.mode = jiggle > 0.0f ? 2 : (k == 0 ? 0 : 1);
            if (k == 0 && jiggle <= 0.0f) { rb.mass = 1.0f; rb.moveDamping = rb.rotDamping = 0.0f; }
            bodyOf[l.bone] = static_cast<int>(model.rigidBodies.size());
            if (k > 0) {
                chainBody.push_back(static_cast<int>(model.rigidBodies.size()));
                chainJiggle.push_back(jiggle > 0.0f ? 1 : 0);
                chainCore.push_back({sub(l.centre, l.unit * (0.5f * l.cylinder)), l.centre + l.unit * (0.5f * l.cylinder), l.radius});
            }
            model.rigidBodies.push_back(std::move(rb));
            if (jiggle > 0.0f && k == 0 && jigAnchor >= 0) {
                // The rigid anchor joint: every limit and spring zero, like the reference models' torso-to-breast joint. Without it the
                // mode-2 chain has nothing holding it to the model and stays where the model spawned.
                const pmx::RigidBody& rootBody = model.rigidBodies[static_cast<size_t>(bodyOf[l.bone])];
                pmx::Joint j;
                j.name = ch.name + " anchor";
                j.bodyA = jigAnchor;
                j.bodyB = bodyOf[l.bone];
                j.position = jigAnchorPos;   // the anchor's place, so the rigid joint is already satisfied at rest
                j.rotation = rootBody.rotation;
                j.moveLower = j.moveUpper = j.rotLower = j.rotUpper = j.springMove = j.springRot = {0, 0, 0};
                model.joints.push_back(std::move(j));
            }
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
            // MikuMikuDance is kinder to a calm default than to the game's loosest settings: no link turns more than 35 degrees. A jiggle
            // chain gets a wider envelope so the part can hang: the bust ~34 degrees a link, the butt less.
            const float stiffness = along(ch.boneStiffness, ch.stiffness, b);
            const float bendCap = jiggle > 0.0f ? (jigBust ? 0.60f : 0.30f) : 0.6f;
            const float bend = std::min(bendCap, std::max(0.05f, 2.0f * std::asin(clamp01(1.0f - stiffness))));
            // How far a strand may droop when it stands out sideways: a loose chain gives way, a stiff one hardly (never more than half the limit).
            // A jiggle chain droops more: the hanging breast is allowed to lean well past halfway, so gravity shows.
            const float baseDroop = std::min(0.5f * bend, 0.25f - 0.15f * clamp01(stiffness * 2.0f));
            const float droop = jiggle > 0.0f ? std::min(bend * 0.85f, baseDroop * 2.0f) : baseDroop;
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
            // A jiggle link is light and short, so that cap strangles its spring far below what gravity needs: the part then rests on its
            // joint limit and twitches there. A jiggle spring is allowed to reach the droop stiffness instead (its heavier body and the
            // fixed 0.9 damping keep the swing slow), so it holds its rest pose instead of hanging on the stop.
            const float own = self.mass * length(sub(self.centre, self.head)) * length(sub(self.centre, self.head)) + capsuleInertiaAcross(self.mass, self.radius, self.cylinder);
            const float capK = jiggle > 0.0f ? std::numeric_limits<float>::max() : own * kMaxBodySwing * kMaxBodySwing;
            float stiffnessK = std::min(std::max(inertia * omega * omega, lever * gravity / droop), capK);
            // A jiggle spring is softer than the droop calc alone gives: it has to let the part hang and swing, not pin it
            // to the rest pose. The baked rest itself already hangs (AddSagPose), so a weaker spring still settles there,
            // and the fixed 0.9 damping keeps the swing slow instead of shivering on the joint limit.
            if (jiggle > 0.0f) stiffnessK *= 0.5f;
            pmx::Joint j;
            j.name = ch.name + " " + std::to_string(k);
            j.bodyA = bodyA;
            j.bodyB = bodyOf[b];
            j.position = self.head;
            j.rotation = model.rigidBodies[static_cast<size_t>(bodyOf[b])].rotation;
            // A link that gravity pushes further than its spring can hold inside the limits would rest on the stop, and a body resting on a
            // joint limit shivers in MMD: such a joint is locked (a zero range) instead.
            const bool holds = sideways * gravity / stiffnessK < 0.6f * bend;
            // The intensity opens a jiggle joint's envelope. The spring below stays: with the limits alone, a pendulum hangs displaced
            // under gravity and wobbles like jelly; the spring pulls it back to the rest pose, so it bounces like skin instead. And a jiggle
            // joint is never droop-locked: locked means no jiggle at all, which is worse than a soft hang.
            float range = (holds || jiggle > 0.0f) ? bend : 0.0f;
            if (jiggle > 0.0f) range = std::min(0.9f, range * jiggle);
            // The reference models' breast joints are wide on the flop axis, moderate on the sway, narrow on the twist (about 0.44 / 0.26 /
            // 0.18 rad). A strand keeps the old shape: the twist nearly locked, the two bends wide.
            const float yF = jiggle > 0.0f ? 0.6f : 0.1f, zF = jiggle > 0.0f ? 0.4f : 1.0f;
            j.rotLower = {-range, -yF * range, -zF * range};
            j.rotUpper = {range, yF * range, zF * range};
            j.springRot = {stiffnessK, stiffnessK, stiffnessK};
            model.joints.push_back(std::move(j));
        }
    }

    // Who collides with whom. A body that starts inside a collider (a skirt over the thigh, a breast bone in the bust sphere, a strand
    // in the head) is thrown out of it the moment the simulation starts, and the whole model with it. So a body meets only the colliders
    // it does not overlap in the rest pose: colliders that overlap the same bodies share a group, and a body's mask lists the groups none
    // of whose colliders it overlaps. Group 0 (overlaps nothing) is met by every body; at most kPmxMaxColliderGroups groups, the last one
    // takes any signature beyond them (so it excludes the union). The one exception is a jiggle body: it is the body itself, not a strand,
    // so its own bust / hip hit spheres would shove it for ever (the shiver at rest) — below it meets the generated arm colliders only.
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
        if (chainJiggle[bi]) continue;   // jiggle masks are set with the arm colliders below (arm only, never the bust spheres)
        uint16_t mask = 0;
        for (int g : usedGroups)
            if (!overlapped[static_cast<size_t>(g)].count(static_cast<int>(bi))) mask |= static_cast<uint16_t>(1u << g);
        model.rigidBodies[static_cast<size_t>(chainBody[bi])].mask = mask;
    }
    // Generated arm colliders, so the breast slides off the arm instead of clipping through it. The bust's own hit
    // spheres are deliberately not met (they shove the jiggle forever); these arms are clear of the breast at rest,
    // so the jiggle can meet them safely. Group 15 is the arms alone.
    {
        constexpr uint8_t kArmGroup = 15;
        bool hasArm = false;
        auto findBone = [&](const std::string& name) { return scene.boneIndex(name); };
        for (const char* side : {"_L", "_R"}) {
            const int shoulder = findBone(std::string("cf_j_shoulder") + side);
            const int arm = findBone(std::string("cf_j_arm00") + side);
            const int forearm = findBone(std::string("cf_j_forearm01") + side);
            auto addSeg = [&](int a, int b, float radius) {
                if (a < 0 || b < 0 || static_cast<size_t>(a) >= scene.bones.size() || static_cast<size_t>(b) >= scene.bones.size()) return;
                const Vec3 pa = scene.bones[static_cast<size_t>(a)].pos * scale;
                const Vec3 pb = scene.bones[static_cast<size_t>(b)].pos * scale;
                const Vec3 dir = sub(pb, pa);
                const float len3d = length(dir);
                if (len3d < 1e-5f) return;
                const Vec3 axis = normalize(dir);
                pmx::RigidBody rb;
                rb.name = std::string("arm collider ") + side + " " + (a == shoulder ? "upper" : "forearm");
                rb.bone = a;
                rb.group = kArmGroup;
                rb.mask = static_cast<uint16_t>(1u << kPmxGroupChain);
                rb.mode = 0;
                rb.shape = (len3d - 2.0f * radius * scale) > 1e-4f ? 2 : 0;
                rb.size = {radius * scale, std::max(0.0f, len3d - 2.0f * radius * scale), 0};
                rb.position = (pa + pb) * 0.5f;
                if (rb.shape == 2) rb.rotation = PmxRotationFromYAxis(axis);
                rb.mass = 1.0f;
                model.rigidBodies.push_back(std::move(rb));
                hasArm = true;
            };
            addSeg(shoulder, arm, 0.040f);
            addSeg(arm, forearm, 0.032f);
        }
        if (hasArm) {
            for (size_t bi = 0; bi < chainBody.size(); ++bi) {
                if (chainJiggle[bi]) model.rigidBodies[static_cast<size_t>(chainBody[bi])].mask = static_cast<uint16_t>(1u << kArmGroup);
                else model.rigidBodies[static_cast<size_t>(chainBody[bi])].mask |= static_cast<uint16_t>(1u << kArmGroup);
            }
        }
    }
    AddLegFollow(scene, model);
}

void AddLegFollow(const assemble::Scene& scene, pmx::Model& model) {
    auto find = [&](const std::string& name) {
        for (size_t i = 0; i < scene.bones.size(); ++i) if (scene.bones[i].name == name) return static_cast<int>(i);
        return -1;
    };
    int thigh[2], knee[2], foot[2], toes[2];
    for (int s = 0; s < 2; ++s) {
        const std::string side = s == 0 ? "L" : "R";
        thigh[s] = find("cf_j_thigh00_" + side); knee[s] = find("cf_j_leg01_" + side);
        foot[s] = find("cf_j_foot_" + side); toes[s] = find("cf_j_toes_" + side);
        if (thigh[s] < 0 || knee[s] < 0 || foot[s] < 0 || toes[s] < 0) return;
    }
    if (model.bones.size() < scene.bones.size()) return;
    auto pos = [&](int b) { return scene.bones[static_cast<size_t>(b)].pos; };
    auto flat = [](Vec3 v) { return Vec3{v.x, 0.0f, v.z}; };
    const Vec3 front = normalize(flat(sub(pos(toes[0]), pos(foot[0])) + sub(pos(toes[1]), pos(foot[1]))));
    const Vec3 hips = (pos(thigh[0]) + pos(thigh[1])) * 0.5f;
    const float hipY = std::max(pos(thigh[0]).y, pos(thigh[1]).y), kneeY = std::max(pos(knee[0]).y, pos(knee[1]).y);
    const float legLen = hipY - std::min(pos(foot[0]).y, pos(foot[1]).y);
    if (length(front) < 0.5f || legLen <= 0.0f) return;
    auto underLeg = [&](int b) {
        for (int p = b; p >= 0; p = scene.bones[static_cast<size_t>(p)].parent)
            if (p == thigh[0] || p == thigh[1]) return true;
        return false;
    };
    // The bones that move after the leg: an append parent has to be solved (foot IK included) before the bone that follows it.
    auto lateLayer = [&](int top) {
        for (size_t b = 0; b < model.bones.size(); ++b)
            for (int p = static_cast<int>(b); p >= 0; p = model.bones[static_cast<size_t>(p)].parent)
                if (p == top) { model.bones[b].layer = std::max(model.bones[b].layer, 1); break; }
    };
    for (const auto& ch : scene.chains) {
        if (ch.characterOwn) continue;
        const std::vector<int> bones = PhysicsChainBones(scene, ch);
        if (bones.size() < 2) continue;
        const int root = bones[0];
        const Vec3 r = pos(root);
        float lowest = r.y;
        for (int b : bones) lowest = std::min(lowest, pos(b).y);
        // A garment hanging over the legs: tied between the knees and a bit above the hips, near the hips, reaching below the hip joints.
        if (r.y < kneeY || r.y > hipY + 0.5f * (hipY - kneeY) || length(flat(sub(r, hips))) > 0.5f * legLen || lowest >= hipY || underLeg(root)) continue;
        if (model.bones[static_cast<size_t>(root)].appendParent >= 0) continue;
        float rate[2];
        const Vec3 across = normalize(flat(sub(pos(thigh[0]), pos(thigh[1]))));
        const float halfHip = std::max(1e-4f, 0.5f * length(flat(sub(pos(thigh[0]), pos(thigh[1])))));
        for (int s = 0; s < 2; ++s) {
            const Vec3 d = flat(sub(r, pos(thigh[s])));
            const float ahead = length(d) > 1e-5f ? clamp01((d.x * front.x + d.z * front.z) / length(d)) : 0.0f;
            // In front of the thigh it takes the whole share; the piece between the legs two thirds of each (both thighs reach under it).
            const float share = clamp01(1.0f - std::fabs(d.x * across.x + d.z * across.z) / (3.0f * halfHip));
            rate[s] = kLegFollow * ahead * share;
        }
        const int lead = rate[0] >= rate[1] ? 0 : 1;
        if (rate[lead] < 0.05f) continue;
        pmx::Bone& rb = model.bones[static_cast<size_t>(root)];
        rb.appendParent = thigh[lead];
        rb.appendRatio = rate[lead];
        lateLayer(root);
        // A piece between the legs (the front of a skirt) follows both: the other leg turns the root's parent, when that bone carries this chain alone.
        const int parent = scene.bones[static_cast<size_t>(root)].parent;
        if (rate[1 - lead] < 0.05f || parent < 0 || underLeg(parent) || model.bones[static_cast<size_t>(parent)].appendParent >= 0) continue;
        int children = 0;
        for (const auto& b : scene.bones) children += b.parent == parent ? 1 : 0;
        if (children != 1) continue;
        model.bones[static_cast<size_t>(parent)].appendParent = thigh[1 - lead];
        model.bones[static_cast<size_t>(parent)].appendRatio = rate[1 - lead];
        lateLayer(parent);
    }
}

size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale,
                           std::map<std::string, std::string>* shapes) {
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
        if (shapes && !ex.shape.empty()) (*shapes)[ex.name] = ex.shape;
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
