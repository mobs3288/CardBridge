#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// Collision groups of the exported bodies. In the PMX a rigid body's mask has bit n set for every group it collides with (the
// Bullet filter; saba and babylon-mmd pass it on as is), so colliders (group 0) list only the chains and chains list only the colliders.
constexpr uint8_t kPmxGroupCollider = 0, kPmxGroupChain = 1;

// Physics for the PMX: every DynamicBone chain of the scene (hair, skirts, ribbons, the body's jiggle chains) becomes a kinematic anchor
// body on the bone the chain hangs from plus one capsule body per chain bone (the root included: the component turns it to point at its
// child too), joined by 6-DOF spring joints at the bones' heads, and every body collider becomes a kinematic capsule or sphere the chains
// collide with. Positions are scaled by `scale` like the rest of the model.
//
// The mapping from the game's parameters follows what the particle update does: damping becomes the bodies' damping rates (the same
// decay a second), elasticity the swing rate of each joint's spring (stiffness = inertia of everything below it * rate^2), stiffness the
// rotation limits and the least spring that keeps a strand standing out sideways from drooping; gravity and force are not carried (the
// PMX world has its own gravity, 9.8 m/s^2 is assumed for the spring).
void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale);

// Face expressions: one vertex morph per scene expression (brow / eye / mouth pattern, closed or open, relative to the resting
// expression built into the vertices) and one per raw blend shape. `meshBase[i]` is the PMX index of scene mesh i's first vertex,
// -1 for a mesh that was not exported. Returns the number of morphs added.
size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale);

// Euler angles (radians, x y z) of the rotation that turns +Y onto `axis`, in the order PMX rigid bodies use (yaw, then pitch, then roll).
Vec3 PmxRotationFromYAxis(Vec3 axis);

} // namespace cardbridge::exporter
