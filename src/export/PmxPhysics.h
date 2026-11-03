#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <vector>

namespace cardbridge::exporter {

// Physics for the PMX: every DynamicBone chain of the scene (hair, skirts, ribbons, the body's jiggle chains) becomes a kinematic anchor
// body on its first bone plus one capsule body per following bone, joined by 6-DOF spring joints, and every body collider becomes a
// kinematic capsule or sphere the chains collide with. Positions are scaled by `scale` like the rest of the model.
//
// The mapping from the game's parameters is approximate - Bullet is not the DynamicBone solver: damping, elasticity and stiffness
// choose the damping, spring and rotation limits; gravity and force are not carried (the PMX world has its own gravity).
void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale);

// Face expressions: one vertex morph per scene expression (brow / eye / mouth pattern, closed or open, relative to the resting
// expression built into the vertices) and one per raw blend shape. `meshBase[i]` is the PMX index of scene mesh i's first vertex,
// -1 for a mesh that was not exported. Returns the number of morphs added.
size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale);

// Euler angles (radians, x y z) of the rotation that turns +Y onto `axis`, in the order PMX rigid bodies use (yaw, then pitch, then roll).
Vec3 PmxRotationFromYAxis(Vec3 axis);

} // namespace cardbridge::exporter
