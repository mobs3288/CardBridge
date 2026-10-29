#pragma once

#include "core/Math.h"

#include <array>
#include <vector>

namespace cardbridge::assemble {

// The game's DynamicBone (the classic component: particles on a bone chain with damping, elasticity, stiffness, inertia, force and
// colliders) run until the chain stops moving. A character standing in a still pose, as in a card, ends in that rest state; hair and
// accessory chains hang or lean there instead of at the authored pose. Nothing here needs the game; the caller reads the component's
// fields from the prefab and hands over the transform tree.

struct DbCollider {
    Vec3 center;
    float radius = 0.5f, height = 0.0f;
    int direction = 0;       // 0 X, 1 Y, 2 Z
    bool inside = false;     // Bound.Inside
    Mat4 world;              // world matrix of the collider's transform
};

// An AnimationCurve as keyframes (time, value, inSlope, outSlope); empty = not set.
struct DbCurve {
    std::vector<std::array<float, 4>> keys;
    float eval(float t) const;
};

struct DbParams {
    int root = -1;           // node index of m_Root
    int owner = -1;          // node of the GameObject that carries the component
    float updateRate = 60.0f;
    float damping = 0.1f, elasticity = 0.1f, stiffness = 0.1f, inert = 0.0f, radius = 0.0f, endLength = 0.0f;
    Vec3 endOffset, gravity, force;
    int freezeAxis = 0;      // 0 none, 1 X, 2 Y, 3 Z
    DbCurve dampingC, elasticityC, stiffnessC, inertC, radiusC;
    std::vector<int> exclusions, notRolls;
};

// A transform hierarchy: local matrices, parents (-1 = hangs on `rootWorld`), parents before children.
struct DbTree {
    std::vector<Mat4> restLocal, local;
    std::vector<int> parent;
    std::vector<std::vector<int>> children;
    Mat4 rootWorld;

    void Init(const std::vector<Mat4>& locals, const std::vector<int>& parents, const Mat4& rootWorldMatrix);
    Mat4 WorldOf(int node, bool rest = false) const;
};

// Runs one component against `colliders` and writes the resulting local matrices of the moved nodes into tree.local; returns the number
// of update steps taken. Without `initial` (or with steps <= 0) the particles start at rest and run until they stop. With `initial`, a tree of
// the same hierarchy in the pose the game had when the component started (the character before its shape sliders), the particles start
// there, the component's object jumps to its place in `tree` in the first step (inertia carries them along) and exactly `steps` updates run:
// the state a still frame a few updates after the sliders were applied is in.
int SettleDynamicBone(DbTree& tree, const DbParams& p, const std::vector<DbCollider>& colliders, const DbTree* initial = nullptr, int steps = 0);

} // namespace cardbridge::assemble
