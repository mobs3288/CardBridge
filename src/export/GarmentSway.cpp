#include "export/GarmentSway.h"

#include "assemble/Scene.h"
#include "export/PmxPhysics.h"   // PhysicsChainBones

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace cardbridge::exporter {

// How much of every skirt-bone influence moves into its sway bone. The skirt bone itself keeps the remaining share,
// so at rest the mesh renders exactly as authored and any flutter is a small secondary rotation on top.
static constexpr float kGarmentSwayMix = 0.50f;

// The per-link envelope of a sway joint, radians: the fold axis can only open on the anti-gravity side (the kick up),
// never droop; the rest are symmetric budgets around the rest pose.
static constexpr float kGarmentSwayFlex = 0.32f;
static constexpr float kGarmentSwayTwist = 0.14f;

// Physics safety: the hold spring is sized from the card's own elasticity, capped so MikuMikuDance's 120 Hz solver stays clean — but the
// cap is low on purpose: a pendulum barely moves if its spring is stiff relative to the base's acceleration. A skirt sloshes like a skirt
// only when its restoring spring is soft (a low omega), with the gravity side of the fold axis pinned at exactly zero so it still never droops.
static constexpr float kGarmentSwayOmegaCap = 5.0f;

static Vec3 normalizeSafe(Vec3 v, Vec3 fallback) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 1e-6f) { v.x /= l; v.y /= l; v.z /= l; return v; }
    return fallback;
}

static float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float clampF(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

// World directions of the joint frame's local axes from its Euler angles (same convention as the rest of the physics exporter:
// R = Ry(y)*Rx(x)*Rz(z), the columns are the world framings of local X, Y, Z).
static void EulerAxes(float x, float y, float z, Vec3& X, Vec3& Y, Vec3& Z) {
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
    X = {cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz};
    Y = {-cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz};
    Z = {sy * cx, -sx, cy * cx};
}

namespace {
struct SwayChain {
    std::vector<int> bones;                // scene (== pmx) bone indices, in chain order
    const assemble::SceneChain* chain = nullptr;
    std::vector<int> sway;                 // pmx index of the appended sway bone per chain bone
    std::vector<int> bodyOfSway;           // pmx index of the rigid body attached to the sway bone
    std::vector<Vec3> head;                // model units
    std::vector<float> radius, segmentLen;
    int anchorBody = -1;                   // the kinematic anchor on the skirt root bone
};
} // namespace

void AddGarmentSway(const assemble::Scene& scene, pmx::Model& model, float scale) {
    // Garment chains to sway: outfit DynamicBone chains only — never the body's own jiggle, never hair (outfit-slot hair items
    // carry chains too), and only chains with at least two valid bones.
    std::vector<const assemble::SceneChain*> wanted;
    for (const auto& ch : scene.chains) {
        if (ch.characterOwn) continue;
        if (ch.part.rfind("outfit", 0) != 0) continue;
        if (assemble::SceneChainIsHair(scene, ch)) continue;
        const std::vector<int> bones = PhysicsChainBones(scene, ch);
        if (bones.size() < 2) continue;
        wanted.push_back(&ch);
    }
    if (wanted.empty()) return;

    std::vector<SwayChain> chains;
    for (const auto* ch : wanted) {
        SwayChain sc;
        sc.chain = ch;
        sc.bones = PhysicsChainBones(scene, *ch);
        for (int b : sc.bones) {
            pmx::Bone nb;
            nb.name = scene.bones[static_cast<size_t>(b)].name + " sway";
            nb.nameEn = nb.name;
            nb.position = scene.bones[static_cast<size_t>(b)].pos * scale;
            nb.parent = b;               // childed directly at the skirt bone, same rest position
            nb.flags = 0x0006;           // rotatable, translatable: solved with the skeleton, never shown or operable
            nb.tailBone = -1;
            nb.tailOffset = {0, 0, 0};
            sc.sway.push_back(static_cast<int>(model.bones.size()));
            model.bones.push_back(std::move(nb));
        }
        chains.push_back(std::move(sc));
    }

    // Geometry per link: a capsule from the sway bone's head to the next sway bone's head (the last link takes half the previous
    // spacing), and its mass small enough that gravity torque never approaches the hold spring (the shape is rigid to physics).
    for (auto& sc : chains) {
        const size_t n = sc.bones.size();
        for (size_t i = 0; i < n; ++i) {
            const int b = sc.bones[i];
            sc.head.push_back(scene.bones[static_cast<size_t>(b)].pos * scale);
        }
        for (size_t i = 0; i + 1 < n; ++i) {
            Vec3 dir = {sc.head[i + 1].x - sc.head[i].x, sc.head[i + 1].y - sc.head[i].y, sc.head[i + 1].z - sc.head[i].z};
            sc.segmentLen.push_back(length(dir));
        }
        sc.segmentLen.push_back(sc.segmentLen.empty() ? 0.1f * scale : sc.segmentLen.back() * 0.5f);
        for (size_t i = 0; i < n; ++i) {
            const float len = sc.segmentLen[i];
            float radius = sc.chain->radius > 0.0f ? sc.chain->radius * scale : clampF(len * 0.3f, 0.02f * scale, 0.3f * scale);
            sc.radius.push_back(radius);
        }
    }

    // Bodies: a kinematic (mode 0) ANCHOR on the skirt root bone — welded to the skeleton, unreachable by gravity — plus a dynamic (mode 1)
    // body per sway bone, including the root of the sway chain, so the whole skirt swings as one pendulum from the hips with delay, exactly
    // like a real skirt. Everything is in group 1 with an EMPTY mask: the sway bodies never push or shove anything; they answer only to
    // their own joint springs.
    for (auto& sc : chains) {
        {
            pmx::RigidBody anchor;
            anchor.name = sc.chain->name + " sway anchor";
            anchor.nameEn = anchor.name;
            anchor.bone = sc.bones[0];
            anchor.group = 1;      // like a hair chain's root body: the reachability audit and the solver both treat it as part of the chains
            anchor.mask = 0;       // meets nothing: kinematic and inert, only the joints carry anything
            anchor.mode = 0;
            anchor.shape = 0;       // sphere
            anchor.size = {sc.radius[0] * 1.5f, 0, 0};
            anchor.position = sc.head[0];
            anchor.mass = 1.0f;
            anchor.moveDamping = anchor.rotDamping = 0.0f;
            sc.anchorBody = static_cast<int>(model.rigidBodies.size());
            model.rigidBodies.push_back(std::move(anchor));
        }
        for (size_t i = 0; i < sc.bones.size(); ++i) {
            const int b = sc.bones[i];
            Vec3 dir = (i + 1 < sc.bones.size())
                ? Vec3{sc.head[i + 1].x - sc.head[i].x, sc.head[i + 1].y - sc.head[i].y, sc.head[i + 1].z - sc.head[i].z}
                : (i >= 1 ? Vec3{sc.head[i].x - sc.head[i - 1].x, sc.head[i].y - sc.head[i - 1].y, sc.head[i].z - sc.head[i - 1].z}
                          : Vec3{0, -1, 0});
            dir = normalizeSafe(dir, {0, -1, 0});
            const float len = sc.segmentLen[i];
            pmx::RigidBody rb;
            rb.name = scene.bones[static_cast<size_t>(b)].name + " sway";
            rb.nameEn = rb.name;
            rb.bone = sc.sway[i];
            rb.group = 1;
            rb.mask = 0;
            rb.mode = 1;
            const float cyl = std::max(0.0f, len - 2.0f * sc.radius[i]);
            rb.shape = cyl > 1e-5f ? 2 : 0;
            rb.size = {sc.radius[i], (rb.shape == 2 ? cyl : 0.0f), 0};
            rb.position = sc.head[i] + dir * (len * 0.5f);
            if (rb.shape == 2) rb.rotation = PmxRotationFromYAxis(dir);
            rb.mass = 0.10f;   // cloth with real inertia; light enough for the soft spring to slosh
            rb.moveDamping = rb.rotDamping = 0.85f;
            rb.friction = 0.0f;
            sc.bodyOfSway.push_back(static_cast<int>(model.rigidBodies.size()));
            model.rigidBodies.push_back(std::move(rb));
        }
    }

    // Joints: the root joint connects the anchor to the sway root — translation rigid (zero range), rotation same one-sided fold envelope as the
    // links so the WHOLE panel lags the hip as one piece, exactly like a real skirt swings from the waist with delay. Each link joint below pins
    // its fold axis on the gravity side at zero, allows a one-sided flex on the rest with a soft hold spring sized from the card's own
    // elasticity (capped so MMD's 120 Hz solver stays clean). A soft spring is what makes cloth slosh instead of snapping back.
    for (auto& sc : chains) {
        const size_t n = sc.bones.size();
        const float elasticity = clampF(sc.chain->elasticity, 0.005f, 1.0f);
        const float omega = std::min(kGarmentSwayOmegaCap, 60.0f * std::sqrt(elasticity));
        const float gForce = 9.8f * scale;

        for (size_t k = 0; k < n; ++k) {
            pmx::Joint j;
            j.bodyA = k == 0 ? sc.anchorBody : sc.bodyOfSway[k - 1];
            j.bodyB = sc.bodyOfSway[k];
            j.position = sc.head[k];
            j.rotation = model.rigidBodies[static_cast<size_t>(j.bodyB)].rotation;
            j.name = sc.chain->name + " sway " + std::to_string(k);
            j.nameEn = j.name;
            if (k == 0) { j.moveLower = {0, 0, 0}; j.moveUpper = {0, 0, 0}; }
            // Inertia of everything below this joint (the last links are the lightest, but every link carries real cloth inertia).
            float inertia = 0.0f;
            const Vec3 pivot = (k == 0) ? sc.head[0] : sc.head[k - 1];
            for (size_t d = k; d < n; ++d) {
                const Vec3 arm = {sc.head[d].x - pivot.x, sc.head[d].y - pivot.y, sc.head[d].z - pivot.z};
                const float r2 = arm.x * arm.x + arm.y * arm.y + arm.z * arm.z;
                inertia += 0.10f * r2 + 0.10f * sc.radius[d] * sc.radius[d];
            }
            const float bend = clampF(omega * omega * inertia, 0.0f, 2500.0f);   // the hold spring, stiffness units
            // Fold axis: the horizontal axis perpendicular to the link and gravity (arm x g). The axis most aligned with it gets the one-sided stop.
            const Vec3 seg = (k == 0 && n >= 2) ? Vec3{sc.head[1].x - sc.head[0].x, sc.head[1].y - sc.head[0].y, sc.head[1].z - sc.head[0].z}
                                                 : Vec3{sc.head[k].x - sc.head[k - 1].x, sc.head[k].y - sc.head[k - 1].y, sc.head[k].z - sc.head[k - 1].z};
            Vec3 g = {0.0f, -gForce, 0.0f};
            Vec3 tw = cross3(seg, g);
            tw = normalizeSafe(tw, {1, 0, 0});
            Vec3 linkDir = normalizeSafe(seg, {0, -1, 0});
            Vec3 bfX, bfY, bfZ;
            EulerAxes(j.rotation.x, j.rotation.y, j.rotation.z, bfX, bfY, bfZ);
            const Vec3* bf[3] = {&bfX, &bfY, &bfZ};
            int twistAxis = 0;
            float bestTwist = -1.0f;
            for (int a = 0; a < 3; ++a) {
                const float s = std::fabs(bf[a]->x * linkDir.x + bf[a]->y * linkDir.y + bf[a]->z * linkDir.z);
                if (s > bestTwist) { bestTwist = s; twistAxis = a; }
            }
            const float tScore[3] = {std::fabs(bf[0]->x * tw.x + bf[0]->y * tw.y + bf[0]->z * tw.z),
                                     std::fabs(bf[1]->x * tw.x + bf[1]->y * tw.y + bf[1]->z * tw.z),
                                     std::fabs(bf[2]->x * tw.x + bf[2]->y * tw.y + bf[2]->z * tw.z)};
            const float twMax = std::max(tScore[0], std::max(tScore[1], tScore[2]));
            float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
            for (int a = 0; a < 3; ++a) {
                if (a == twistAxis) { lo[a] = -kGarmentSwayTwist; hi[a] = kGarmentSwayTwist; continue; }
                if (twMax < 1e-4f || tScore[a] < 0.3f * twMax) { lo[a] = -kGarmentSwayFlex; hi[a] = kGarmentSwayFlex; continue; }   // side plane: symmetric
                const float d = bf[a]->x * tw.x + bf[a]->y * tw.y + bf[a]->z * tw.z;   // +rotation increases the hang
                if (d > 0.0f) { lo[a] = -kGarmentSwayFlex; hi[a] = 0.0f; } else { lo[a] = 0.0f; hi[a] = kGarmentSwayFlex; }
            }
            j.rotLower = {lo[0], lo[1], lo[2]};
            j.rotUpper = {hi[0], hi[1], hi[2]};
            j.springRot = {bend, bend, bend};
            model.joints.push_back(std::move(j));
        }
    }

    // Re-weight: every influence sitting on a swayed skirt bone gives up kGarmentSwayMix of its share to the matching sway bone.
    // A vertex already at four influences keeps its weights (it shares nothing new); everything else just works out.
    std::map<int, int> skirtToSway;
    for (const auto& sc : chains)
        for (size_t i = 0; i < sc.bones.size(); ++i) skirtToSway[sc.bones[i]] = sc.sway[i];

    for (auto& v : model.vertices) {
        struct Added { int bone; float weight; } add[4];
        int adds = 0;
        bool touched = false;
        for (int k = 0; k < 4; ++k) {
            if (v.weight[k] <= 0.0f) continue;
            auto it = skirtToSway.find(v.bone[k]);
            if (it == skirtToSway.end()) continue;
            const float share = v.weight[k] * kGarmentSwayMix;
            v.weight[k] -= share;
            if (adds < 4) add[adds++] = {it->second, share};
            touched = true;
        }
        if (!touched) continue;
        for (int a = 0; a < adds; ++a) {
            const int sway = add[a].bone;
            const float w = add[a].weight;
            bool merged = false;
            for (int k = 0; k < 4; ++k) {
                if (v.bone[k] == sway && v.weight[k] > 0.0f) { v.weight[k] += w; merged = true; break; }
            }
            if (merged) continue;
            int free = -1;
            for (int k = 0; k < 4; ++k) if (v.weight[k] <= 0.0f) { free = k; break; }
            if (free < 0) {
                // No free influence slot: hand the share back to the biggest skirt influence.
                int best = 0;
                for (int k = 1; k < 4; ++k) if (v.weight[k] > v.weight[static_cast<size_t>(best)]) best = k;
                v.weight[static_cast<size_t>(best)] += w;
                continue;
            }
            v.bone[static_cast<size_t>(free)] = sway;
            v.weight[static_cast<size_t>(free)] = w;
        }
    }
}

} // namespace cardbridge::exporter
