#pragma once

#include "assemble/JiggleSettings.h"
#include "assemble/Scene.h"

#include <vector>

namespace cardbridge::assemble {

// A live preview of the scene's DynamicBone chains (hair, skirts, ribbons, the body's own jiggle chains). The cloth and hair
// keep the classic component's particle update (inertia, damping, elasticity, stiffness, gravity and force, colliders) at 60
// updates a second. The body's own chains are a real mass-spring-damper simulation instead: every link has a mass, a spring to
// its posed place, damping and gravity, stepped at JiggleSettings::simulationHz with interpolation, so the sway is stable at
// any frame rate and moves on all three axes (vertical, sideways, forward / back) with a subtle bone rotation. The scene drives
// the chain bones' skin matrices only; the PMX physics (rigid bodies and joints) is a separate, physical mapping of the same
// settings.
class PhysicsSim {
public:
    // Starts every chain at rest under the pose (`poseWorld` = assemble::BoneWorlds). The body's own jiggle chains are simulated
    // only when their switch is on (as the PMX export does). `jiggle` drives the mass-spring-damper path; `jiggleOnly` (the static
    // settle, Pose::BakeSettledPhysics) limits the sim to the body's own chains: hair and accessory chains keep their authored
    // pose, which is how the game displays them anyway. `outfitOnly` limits the sim to the clothing and accessory chains.
    void Reset(const Scene& scene, const std::vector<Mat4>& poseWorld, bool jiggleBust = true, bool jiggleButt = true,
               const JiggleSettings& jiggle = {}, bool jiggleOnly = false, bool outfitOnly = false);

    // Runs `steps` fixed 1/60 s updates (0 only refreshes the output after the anchors moved) and writes the skin matrices of
    // the chain bones into `skin`, sized like the scene's bones. The jiggle chains accumulate their own fixed substeps from the
    // same elapsed time. Returns true while any particle is still moving.
    bool Step(const Scene& scene, const std::vector<Mat4>& poseWorld, int steps, std::vector<Mat4>& skin);

    // Advances by `seconds` of wall time: cloth / hair at 60 Hz, the body's jiggle at JiggleSettings::simulationHz, and writes
    // the jiggle bones interpolated between the last two substeps. This is the frame-rate-independent path the viewer uses.
    bool Advance(const Scene& scene, const std::vector<Mat4>& poseWorld, double seconds, std::vector<Mat4>& skin);

    // Gives every particle a push (a velocity, model units per update), to see the chains swing without moving a bone.
    void Nudge(Vec3 impulse);

    bool empty() const { return chains_.empty() && jiggles_.empty(); }
    size_t particleCount() const;
    size_t jiggleParticleCount() const;

private:
    struct WorldCollider {
        Vec3 a, b;     // capsule segment ends (equal for a sphere)
        float radius = 0;
        bool bodyOwn = false;   // the body's own hit spheres (breast / butt / hip / waist): the character's jiggle chains sit inside them,
                                 // so breast/butt jiggle skips them (colliding with its own rest pose buzzed every substep)
    };
    struct Particle {
        int bone = -1;
        int parent = -1;          // particle index, -1 for the chain's anchor
        int child = -1;           // first child particle (the one the bone points at), -1 for a leaf
        Vec3 restLocal;           // rest offset from the parent particle, in the parent's frame
        float restLen = 0.0f;
        Vec3 pos, prev;
        Mat4 world;               // the bone's simulated world matrix
    };
    struct Chain {
        size_t scene = 0;         // index into Scene::chains
        std::vector<int> particles;
        Vec3 anchorPrev;
        bool primed = false;
        bool jiggle = false;       // the body's own bust / butt chain: a shake hits it harder than hair
        float envelopeScale = 1.0f;   // a jiggle chain: how wide its swing is, the intensity (see Reset)
    };
    struct JiggleLink {
        int bone = -1;
        int parent = -1;          // link index, -1 for the root (anchored to parentBone)
        int child = -1;           // link index of the first chain child, -1 for a leaf
        int parentBone = -1;      // the scene bone the root hangs from
        Vec3 toChild;             // rest offset from this bone to its first chain child, model axes
        float restLen = 0.0f;     // distance to the parent link / anchor at Reset
        float mass = 1.0f;        // kg
        Vec3 pos, vel;
        Vec3 target;              // the posed place this link springs back to, current frame
        Mat4 world, prevWorld;    // the bone's world at this substep and the one before, for interpolation
    };
    struct JiggleChain {
        size_t scene = 0;
        std::vector<JiggleLink> links;
        std::vector<Vec3> lastTargets;   // to wake a sleeping chain when the pose moves
        bool bust = false;
        int sideSign = 0;
        bool asleep = false;
        float stillSeconds = 0.0f;
        std::vector<char> skipColliders;   // per collider (index into colliders_): 1 when a link starts inside it at reset - the chain
                                            // is never pushed out of its own rest pose by physics (recomputed on every pose change)
    };

    void CollectColliders(const Scene& scene, const std::vector<Mat4>& poseWorld);
    bool StepGeneric(const Scene& scene, const std::vector<Mat4>& poseWorld, float& moved);
    void StepJiggle(const Scene& scene, const std::vector<Mat4>& poseWorld, float dt, float& moved);
    void UpdateJiggleTargets(const Scene& scene, const std::vector<Mat4>& poseWorld, bool& woke);
    void UpdateJiggleWorlds(const Scene& scene, const std::vector<Mat4>& poseWorld);
    void WriteOutput(const Scene& scene, const std::vector<Mat4>& poseWorld, std::vector<Mat4>& skin, bool interpolate, float alpha);
    bool AdvanceInternal(const Scene& scene, const std::vector<Mat4>& poseWorld, double seconds, std::vector<Mat4>& skin, bool interpolate);

    std::vector<Particle> particles_;
    std::vector<Chain> chains_;
    std::vector<JiggleChain> jiggles_;
    std::vector<WorldCollider> colliders_;
    JiggleSettings jiggle_;
    double genericAccum_ = 0.0;   // seconds not yet simulated at 60 Hz
    double jiggleAccum_ = 0.0;    // seconds not yet simulated at the jiggle rate
    float speed_ = 0.0f;          // largest particle displacement / speed in the last update
};

} // namespace cardbridge::assemble
