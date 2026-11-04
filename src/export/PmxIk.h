#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <vector>

namespace cardbridge::exporter {

// A limb the rig can pose with IK: a root bone (shoulder joint, hip), the bone that bends (elbow, knee) and the end bone the IK pulls
// (wrist, ankle). Each is the child of the one before it.
struct Limb {
    std::string role;      // "hand" or "foot"
    std::string side;      // "L" or "R"
    int root = -1, mid = -1, end = -1;   // scene bones
};

// The limbs of the scene's rig, found by the names the Koikatsu rig gives them (cf_j_arm00_L / cf_j_forearm01_L / cf_j_hand_L,
// cf_j_thigh00_L / cf_j_leg01_L / cf_j_leg03_L, and the right side); a limb whose bones are missing or not in a parent chain is left out.
// Only the roles are named here, every position and axis comes from the scene.
std::vector<Limb> FindLimbs(const assemble::Scene& scene);

// The bending limits of a limb from its rest pose: the bend axis is the normal of the plane through root, mid and end, and the bone may
// only flex further the way it is already bent (about 0.5 degrees to 150 degrees), the other axes locked. Returns false when the limb is
// straight (no bend direction to go by) or the axis is not along a model axis (a PMX limit is per axis), leaving `lower` and `upper` alone.
bool BendLimits(Vec3 root, Vec3 mid, Vec3 end, Vec3& lower, Vec3& upper);

// Appends one IK bone per limb to the model, at the end bone's place under the model's root: the solver turns the middle bone (with the
// bending limits) and the root bone until the end bone reaches the IK bone, so a PMX editor can pose a limb by moving one handle. The
// scene's bones are the model's first bones, unchanged. Returns the indices of the bones added. With arms=false only the legs get one: MMD
// solves every IK bone each frame and a motion keys the foot IK but not an arm one, so an arm handle at rest would pin the arm to the A-pose.
std::vector<int> AddIk(const assemble::Scene& scene, pmx::Model& model, float scale, bool arms = true);

} // namespace cardbridge::exporter
