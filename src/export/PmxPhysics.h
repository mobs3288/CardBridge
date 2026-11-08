#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// Collision groups of the exported bodies. In the PMX a rigid body's mask has bit n set for every group it collides with (the
// Bullet filter; saba, babylon-mmd and MikuMikuDance itself take it as is), so colliders list only the chains and chains list only
// colliders. Chains are group 1. Colliders are group 0, or 2..14 when they overlap some chain body in the rest pose (see AddPhysics):
// a chain body's mask lists the collider groups it may meet. The exception is a jiggle body (the body's own): it meets the generated
// arm colliders (group 15) only, never its own bust / hip hit spheres, or those would push it for ever and it would shiver at rest.
constexpr uint8_t kPmxGroupChain = 1;

// Physics for the PMX: every DynamicBone chain of the scene (hair, skirts, ribbons; the body's own breast / hip jiggle chains when their switch is on) becomes one capsule body per
// chain bone, the root following its bone (kinematic) and the rest free, joined by 6-DOF spring joints at the bones' heads, and every body
// collider becomes a kinematic capsule or sphere the chains collide with. Positions are scaled by `scale` like the rest of the model.
//
// The mapping from the game's parameters follows what the particle update does: damping becomes the bodies' damping rates (the same
// decay a second), elasticity the swing rate of each joint's spring (stiffness = inertia of everything below it * rate^2, never more than
// 15 rad/s for the one link it acts on), stiffness the rotation limits (at most 35 degrees a link) and the least spring that keeps a strand
// standing out sideways from drooping; gravity and force are not carried (the PMX world has its own gravity, 9.8 m/s^2 is assumed for
// the spring). The body's own jiggle is built like the hand-made MMD models' breast jiggle (mode-2 bodies, fixed 0.9 damping, fat
// capsules with weight tapering toward the tip, no collider - see the mask note above) plus a joint spring, which the reference models lack
// but which pulls the part back to its rest pose instead of letting it hang displaced like jelly.
void AddPhysics(const assemble::Scene& scene, pmx::Model& model, float scale, bool jiggleBust = true, bool jiggleButt = true, float jiggleIntensity = 1.0f);

// Skirts and coat tails turn with the legs (called by AddPhysics). Most links of a garment chain are locked to hold its rest shape, so a
// thigh swung forward cannot push them aside and passes through the cloth. Instead a chain tied around the hips and hanging over the legs
// follows a thigh by append rotation (MMD's 付与), by how far in front of that thigh it hangs; one between the legs follows both, the second
// leg through the root's own parent bone. Those bones go to deform layer 1 so the leg (and its foot IK) is solved first. Needs the game's
// leg bones (cf_j_thigh00_L ...) and `model.bones` parallel to `scene.bones`; otherwise it does nothing.
void AddLegFollow(const assemble::Scene& scene, pmx::Model& model);

// The bones of a chain that get a rigid body: the chain's valid bones without its stubs. A stub is a leaf node that points back up the chain
// (the skirts' cf_j_sk_xx_05_end sits above its parent): a body spanning to it would stand on its joint upside down and flip over in
// MikuMikuDance, so the parent stays the last link and its body carries on in the direction the chain came from.
std::vector<int> PhysicsChainBones(const assemble::Scene& scene, const assemble::SceneChain& chain);

// Face expressions: one vertex morph per scene expression (brow / eye / mouth pattern, closed or open, relative to the resting
// expression built into the vertices) and one per raw blend shape. `meshBase[i]` is the PMX index of scene mesh i's first vertex,
// -1 for a mesh that was not exported. Returns the number of morphs added. `shapes`, when given, gets each expression morph's name mapped to
// the game pattern it plays (SceneExpression::shape, "a_l_op"), what AddMmdMorphs builds the standard morphs from.
size_t AddExpressionMorphs(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase, float scale,
                           std::map<std::string, std::string>* shapes = nullptr);

// The least distance between the segments a-b and c-d (a segment may be a point).
float SegmentDistance(Vec3 a, Vec3 b, Vec3 c, Vec3 d);

// Euler angles (radians, x y z) of the rotation that turns +Y onto `axis`, in the order PMX rigid bodies use (yaw, then pitch, then roll).
Vec3 PmxRotationFromYAxis(Vec3 axis);

} // namespace cardbridge::exporter
