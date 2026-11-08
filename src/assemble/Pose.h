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

// Writes skin matrices into the scene as its new rest pose (the body of BakePose, so the jiggle settle can use it).
void BakeSkin(Scene& scene, const std::vector<Mat4>& skin);

// Writes the pose into the scene as its new rest pose: vertices, normals, blend shape offsets, bone positions and colliders move to
// where the skin matrices put them. The bone table keeps its names and order. Afterwards `scene.pose` says what was applied.
void BakePose(Scene& scene, const Pose& pose);

// The model's standing height in metres (head top to feet on the Body / Head meshes, at rest, no pose). What the
// height editor (cm) and the CLI --height-cm scale from/to.
float SceneHeightMeters(const Scene& scene);

// The sag slider's starting value for this card (0..2, 1 = its intended hang): loose chains and larger busts start
// higher, stiff / small ones start lower, so the default differs per card instead of being one fixed number.
float CardSagDefault(const Scene& scene, float bustSize = 0.5f);

// Cosmetically finishes a posed (or rest) scene exactly as the character is seen: the body's own jiggle (bust and, when on, hips) is
// simulated with gravity until it hangs, and the settled skin is baked into the vertices like any pose. The exported PMX starts
// with the breast as it hangs in the game instead of pulled straight out of the chest. `intensity` is the same jiggle-intensity
// the export writes as the joint swing limits; `sag` scales the static hang (1 = the card's own, 0 = none, 2 = double).
void BakeSettledPhysics(Scene& scene, bool jiggleBust, bool jiggleButt, float jiggleIntensity, float sag = 1.0f);

// How far the bust / butt hangs at rest, radians (shared by the bake above and the viewer preview below). The hang
// respects the card: it comes from the card's own body jiggle chains (stiffness / elasticity / gravity), so a stiff,
// perky rig hangs almost not at all while a loose one hangs ~25-30 degrees at the default intensity. `jiggleIntensity`
// scales how wide it swings, as the PMX joints do.
float SagTiltRad(bool bust, float jiggleIntensity);
float SagTiltRad(bool bust, float jiggleIntensity, float stiffness, float elasticity, float gravityScale);
// Adds that hang to a pose (the viewer calls this so the static preview already sags; the bake above uses the same pose).
// Bust bones (bust / bnip) droop down-forward; butt bones (siri / kosi) droop down-back (less). Only the root of each
// side is tilted, so the whole breast hangs as one piece. A side with no body jiggle chain, or with a stiff chain, gets
// no (or barely any) tilt: the card is left as it is. `sag` scales the hang (1 = the card's own). A bone the user
// already edited keeps its own edit.
void AddSagPose(const Scene& scene, Pose& pose, bool jiggleBust, bool jiggleButt, float jiggleIntensity, float sag = 1.0f);

// Dragging a bone in the view. TurnBone: the new local Euler angles (degrees) of `bone`'s edit so that its world orientation turns by `worldTurn`
// (a rotation matrix about the bone's own position, world axes), whatever its parent and the A-pose do. MoveBone: the new local position edit
// so that the bone moves by `worldDelta` in the world.
Vec3 TurnBone(const Scene& scene, const Pose& pose, size_t bone, const Mat4& worldTurn);
Vec3 MoveBone(const Scene& scene, const Pose& pose, size_t bone, Vec3 worldDelta);

// A pose as a small JSON file: per edited bone (by name) the local translation, Euler degrees and scale, plus the A-pose switch. The
// viewer saves and loads it, `cardbridge convert --pose <file>` bakes it into the PMX like --apose does.
//   { "format": "cardbridge-pose", "version": 1, "a_pose": true, "a_pose_degrees": 40,
//     "bones": { "cf_j_arm00_L": { "position": [0, 0, 0], "rotation_euler_degrees": [0, 0, 20], "scale": [1, 1, 1] } } }
struct PoseFile {
    bool aPose = false;
    float aPoseDegrees = 40.0f;
    std::vector<std::pair<std::string, BonePose>> bones;   // edited bones only, in file order
};
std::string PoseFileToJson(const PoseFile& file);
// False with `error` for text that is not a pose file (not JSON, another format or a newer version, a field of the wrong shape). A bone with
// only some of its three fields keeps the others at rest.
bool PoseFileFromJson(const std::string& text, PoseFile& out, std::string* error = nullptr);
// The Pose a file describes for this scene: bones the scene does not have are skipped and listed in `unknown`; the A-pose is made when the
// file asks for it and the rig has arms (otherwise `base` stays empty).
Pose MakePose(const Scene& scene, const PoseFile& file, std::vector<std::string>* unknown = nullptr);

// The rotation matrix that turns direction `from` onto direction `to` the short way (both need not be unit length).
Mat4 RotationBetween(Vec3 from, Vec3 to);

// The Euler angles (degrees, Unity's order: z, then x, then y) of a rotation matrix; the inverse of shape::EulerToQuat on rotations.
Vec3 EulerFromMatrix(const Mat4& rotation);

// Applies the skin matrices to a point / a direction of one vertex (its weights and bones); the viewer's quick re-skin and BakePose share it.
Vec3 SkinPoint(const SceneVertex& v, const std::vector<Mat4>& skin);
Vec3 SkinDirection(const SceneVertex& v, const std::vector<Mat4>& skin);

} // namespace cardbridge::assemble
