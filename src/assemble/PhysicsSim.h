#pragma once

#include "assemble/Scene.h"

#include <vector>

namespace cardbridge::assemble {

// A live preview of the scene's DynamicBone chains (hair, skirts, ribbons, the body's jiggle chains): the classic component's particle
// update (inertia, damping, elasticity, stiffness, gravity and force, colliders) run at 60 updates a second against the bones as the pose
// puts them, so moving a bone makes what hangs from it swing and settle. It drives the chain bones' skin matrices only; the PMX physics
// (rigid bodies and joints) is a separate, approximate mapping of the same data.
class PhysicsSim {
public:
    // Starts every chain at rest under the pose (`poseWorld` = assemble::BoneWorlds). The body's own jiggle chains are simulated only
    // when their switch is on (as the PMX export does), with the intensity as the swing envelope; the defaults match the export's.
    void Reset(const Scene& scene, const std::vector<Mat4>& poseWorld, bool jiggleBust = true, bool jiggleButt = false, float jiggleIntensity = 0.5f);

    // Runs `steps` updates (1/60 s each; 0 only refreshes the output after the anchors moved) and writes the skin matrices of the chain bones
    // (every bone of a chain but its first, which follows the pose) into `skin`, sized like the scene's bones. Returns true while any
    // particle is still moving.
    bool Step(const Scene& scene, const std::vector<Mat4>& poseWorld, int steps, std::vector<Mat4>& skin);

    // Gives every particle a push (a velocity, model units per update), to see the chains swing without moving a bone.
    void Nudge(Vec3 impulse);

    bool empty() const { return chains_.empty(); }
    size_t particleCount() const { return particles_.size(); }

private:
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
        float envelopeScale = 1.0f;   // a jiggle chain: how wide its swing is, the intensity (see Reset)
    };
    std::vector<Particle> particles_;
    std::vector<Chain> chains_;
    float speed_ = 0.0f;          // largest particle displacement in the last update
};

} // namespace cardbridge::assemble
