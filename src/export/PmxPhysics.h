#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// Collision groups of the exported bodies. In the PMX a rigid body's mask has bit n set for every group it collides with (the
// Bullet filter; saba, babylon-mmd and MikuMikuDance itself take it as is), so colliders list only the chains and chains list only
// colliders. Chains are group 1. Colliders are group 0, or 2..15 when they overlap some chain body in the rest pose (see AddPhysics):
// a chain body's mask lists the collider groups it may meet.
constexpr uint8_t kPmxGroupChain = 1;

// Physics for the PMX: every DynamicBone chain of the scene (hair, skirts, ribbons; not the body's own breast / hip jiggle chains, which stay rigid) becomes one capsule body per
// chain bone, the root following its bone (kinematic) and the rest free, joined by 6-DOF spring joints at the bones' heads, and every body
// collider becomes a kinematic capsule or sphere the chains collide with. Positions are scaled by `scale` like the rest of the model.
//
// The mapping from the game's parameters follows what the particle update does: damping becomes the bodies' damping rates (the same
// decay a second), elasticity the swing rate of each joint's spring (stiffness = inertia of everything below it * rate^2, never more than
// 15 rad/s for the one link it acts on), stiffness the rotation limits (at most 35 degrees a link) and the least spring that keeps a strand
// standing out sideways from drooping; gravity and force are not carried (the PMX world has its own gravity, 9.8 m/s^2 is assumed for
// the spring).
void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale);

// Face expressions: one vertex morph per scene expression (brow / eye / mouth pattern, closed or open, relative to the resting
// expression built into the vertices) and one per raw blend shape. `meshBase[i]` is the PMX index of scene mesh i's first vertex,
// -1 for a mesh that was not exported. Returns the number of morphs added.
size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale);

// The least distance between the segments a-b and c-d (a segment may be a point).
float SegmentDistance(Vec3 a, Vec3 b, Vec3 c, Vec3 d);

// Euler angles (radians, x y z) of the rotation that turns +Y onto `axis`, in the order PMX rigid bodies use (yaw, then pitch, then roll).
Vec3 PmxRotationFromYAxis(Vec3 axis);

} // namespace cardbridge::exporter
