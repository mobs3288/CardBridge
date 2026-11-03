#pragma once

#include "assemble/Scene.h"

#include <string>
#include <vector>

namespace cardbridge::assemble {

// Posing the assembled character. The scene is the rest pose; a Pose says how far each bone is moved from it. Bones have no rest
// rotation here (the PMX way: every bone's axes are the model's), so a bone's local rotation turns it, and everything under it, about
// its own position in the model's axes, a local position moves it in its parent's frame, and a local scale scales its own frame
// (and with it the offsets of its children, as in Unity). Every vertex follows by its skin weights.
struct BonePose {
    Vec3 pos{0, 0, 0};       // local translation, model units
    Vec3 rot{0, 0, 0};       // Euler angles in degrees, Unity's order (z, then x, then y)
    Vec3 scl{1, 1, 1};
};
bool IsIdentity(const BonePose& p);

struct Pose {
    std::vector<BonePose> bones;   // per scene bone; shorter than the scene = the rest are untouched
    std::vector<Quat> base;        // per scene bone, turned before the edit (the automatic A-pose); empty = none

    bool empty() const;
    BonePose& edit(size_t bone);   // grows `bones` as needed
};

// Posed world matrix of every bone.
std::vector<Mat4> BoneWorlds(const Scene& scene, const Pose& pose);

// Per bone the matrix that takes a rest-pose point to the posed one: posed world * inverse(rest world). Identity everywhere for a rest pose.
std::vector<Mat4> SkinMatrices(const Scene& scene, const Pose& pose);

// An automatic A-pose: the rotations that bring the arms down from a T-pose by `degrees` (clavicle and upper arm share it, the arm takes
// most), keeping the arm's length and the rest of the body. The bones are found by name (cf_j_shoulder_L / _R, cf_j_arm00_L / _R,
// cf_j_forearm01_L / _R, or any rig with the same roles); returns false, leaving `base` empty, when the rig has no arms to turn.
bool MakeAPose(const Scene& scene, float degrees, std::vector<Quat>& base);

// Writes the pose into the scene as its new rest pose: vertices, normals, blend shape offsets, bone positions and colliders move to
// where the skin matrices put them. The bone table keeps its names and order. Afterwards `scene.pose` says what was applied.
void BakePose(Scene& scene, const Pose& pose);

// Dragging a bone in the view. TurnBone: the new local Euler angles (degrees) of `bone`'s edit so that its world orientation turns by `worldTurn`
// (a rotation matrix about the bone's own position, world axes), whatever its parent and the A-pose do. MoveBone: the new local position edit
// so that the bone moves by `worldDelta` in the world.
Vec3 TurnBone(const Scene& scene, const Pose& pose, size_t bone, const Mat4& worldTurn);
Vec3 MoveBone(const Scene& scene, const Pose& pose, size_t bone, Vec3 worldDelta);

// The rotation matrix that turns direction `from` onto direction `to` the short way (both need not be unit length).
Mat4 RotationBetween(Vec3 from, Vec3 to);

// The Euler angles (degrees, Unity's order: z, then x, then y) of a rotation matrix; the inverse of shape::EulerToQuat on rotations.
Vec3 EulerFromMatrix(const Mat4& rotation);

// Applies the skin matrices to a point / a direction of one vertex (its weights and bones); the viewer's quick re-skin and BakePose share it.
Vec3 SkinPoint(const SceneVertex& v, const std::vector<Mat4>& skin);
Vec3 SkinDirection(const SceneVertex& v, const std::vector<Mat4>& skin);

} // namespace cardbridge::assemble
