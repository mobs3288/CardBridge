#include "measure/TransferMeasure.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::measure {
namespace {

const char* const kNames[kTransferCount] = {
    "Height", "Head size", "Head-to-body ratio", "Bust", "Waist", "Hip", "Shoulder width", "Neck",
    "Arm length", "Upper arm", "Lower arm", "Leg length", "Thigh", "Calf"
};

float dist(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Both sides are measured where they exist; a card with only one side still gets a value.
float bothSides(float l, float r) {
    if (l > 0.0f && r > 0.0f) return 0.5f * (l + r);
    return l > 0.0f ? l : r;
}

void put(TransferSet& out, int id, float value, bool valid, int confidence, const char* source) {
    TransferValue& v = out[static_cast<size_t>(id)];
    v.valid = valid && std::isfinite(value) && value > 0.0f;
    v.value = v.valid ? value : 0.0f;
    v.confidence = confidence;
    v.source = source ? source : "";
}

struct HeadExtent {
    bool any = false;
    float minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
    float height() const { return maxY - minY; }
    float width() const { return std::max(maxX - minX, maxZ - minZ); }
};

HeadExtent HeadBox(const assemble::Scene& scene) {
    HeadExtent e;
    for (const auto& part : scene.parts) {
        if (part.group != "Head" && part.group != "Face" && part.id != "head") continue;
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                if (!e.any) {
                    e.minX = e.maxX = v.pos.x; e.minY = e.maxY = v.pos.y; e.minZ = e.maxZ = v.pos.z;
                    e.any = true;
                    continue;
                }
                e.minX = std::min(e.minX, v.pos.x); e.maxX = std::max(e.maxX, v.pos.x);
                e.minY = std::min(e.minY, v.pos.y); e.maxY = std::max(e.maxY, v.pos.y);
                e.minZ = std::min(e.minZ, v.pos.z); e.maxZ = std::max(e.maxZ, v.pos.z);
            }
        }
    }
    return e;
}

} // namespace

const char* TransferName(int id) {
    return id >= 0 && id < kTransferCount ? kNames[id] : "";
}

const char* TransferUnit(int id) {
    return id == kTransferHeadBodyRatio ? "x" : "cm";
}

bool TransferIsRatio(int id) {
    return id == kTransferHeadBodyRatio;
}

float BoneLocalLength(const assemble::Scene& scene, const std::string& bone) {
    const int i = scene.boneIndex(bone);
    if (i < 0) return 0.0f;
    const auto& b = scene.bones[static_cast<size_t>(i)];
    if (b.parent < 0 || static_cast<size_t>(b.parent) >= scene.bones.size()) return 0.0f;
    const Mat4 inv = inverse(scene.bones[static_cast<size_t>(b.parent)].world);
    const Vec3 local = inv.point(b.pos);   // b.pos is the world translation of the bone
    return dist(local, Vec3{});
}

float BoneWorldLength(const assemble::Scene& scene, const std::string& bone) {
    const int i = scene.boneIndex(bone);
    if (i < 0) return 0.0f;
    const auto& b = scene.bones[static_cast<size_t>(i)];
    if (b.parent < 0 || static_cast<size_t>(b.parent) >= scene.bones.size()) return 0.0f;
    return dist(b.pos, scene.bones[static_cast<size_t>(b.parent)].pos);
}

// The cf_s_* bones are the shape formulas' zero-offset driver bones (a few tenths of a millimetre
// apart). Limb geometry lives on the cf_j_* deform bones the skin and clothes are skinned to.
std::vector<std::string> ArmChain(bool left) {
    const std::string s = left ? "_L" : "_R";
    return {"cf_j_shoulder" + s, "cf_j_arm00" + s, "cf_j_forearm01" + s, "cf_j_hand" + s};
}

std::vector<std::string> UpperArmChain(bool left) {
    const std::string s = left ? "_L" : "_R";
    return {"cf_j_shoulder" + s, "cf_j_arm00" + s, "cf_j_forearm01" + s};
}

std::vector<std::string> LowerArmChain(bool left) {
    const std::string s = left ? "_L" : "_R";
    return {"cf_j_forearm01" + s, "cf_j_hand" + s};
}

std::vector<std::string> LegChain(bool left) {
    const std::string s = left ? "_L" : "_R";
    return {"cf_j_thigh00" + s, "cf_j_leg01" + s, "cf_j_leg03" + s, "cf_j_foot" + s};
}

float ChainWorldLength(const assemble::Scene& scene, const std::vector<std::string>& chain) {
    float total = 0.0f;
    Vec3 prev{};
    for (size_t i = 0; i < chain.size(); ++i) {
        const int idx = scene.boneIndex(chain[i]);
        if (idx < 0) return 0.0f;   // a missing bone makes the whole chain unusable
        if (i) {
            const Vec3& now = scene.bones[static_cast<size_t>(idx)].pos;
            total += dist(now, prev);   // joint-to-joint, independent of which intermediate bones exist
        }
        prev = scene.bones[static_cast<size_t>(idx)].pos;
    }
    return total * 100.0f;
}

TransferSet MeasureTransfer(const assemble::Scene& scene) {
    TransferSet out;
    const Result bwh = Measure(scene);
    const float heightCm = bwh.heightMeters * 100.0f;
    put(out, kTransferHeight, heightCm, bwh.valid, kTransferMesh, "body mesh bounding height");
    put(out, kTransferBust, bwh.bust * 100.0f, bwh.valid, kTransferMesh, "torso contour above the waist");
    put(out, kTransferWaist, bwh.waist * 100.0f, bwh.valid, kTransferMesh, "narrowest torso contour");
    put(out, kTransferHip, bwh.hip * 100.0f, bwh.valid, kTransferMesh, "torso contour below the waist");

    const HeadExtent head = HeadBox(scene);
    const float headCm = head.height() * 100.0f;
    put(out, kTransferHeadSize, headCm, head.any && head.height() > 0.01f, kTransferMesh, "head / face part bounding height");
    if (head.any && bwh.valid && heightCm > 1.0f)
        put(out, kTransferHeadBodyRatio, head.height() / bwh.heightMeters, true, kTransferDerived, "head height / body height");

    // Shoulder width: the distance between the two shaped shoulder bones on the shared rig.
    const int sl = scene.boneIndex("cf_s_shoulder02_L"), sr = scene.boneIndex("cf_s_shoulder02_R");
    if (sl >= 0 && sr >= 0) {
        const float cm = dist(scene.bones[static_cast<size_t>(sl)].pos, scene.bones[static_cast<size_t>(sr)].pos) * 100.0f;
        put(out, kTransferShoulderWidth, cm, cm > 1.0f, kTransferBone, "cf_s_shoulder02_L / _R distance");
    }

    // Neck: the real skin contour at the neck bone's height (the torso slicer excludes arms).
    int neck = scene.boneIndex("cf_s_neck");
    float neckY = neck >= 0 ? scene.bones[static_cast<size_t>(neck)].pos.y : 0.0f;
    if (neck < 0 || neckY <= 0.0f) neckY = bwh.waistY + 0.22f * std::max(0.0f, bwh.heightMeters);
    if (neckY > 0.0f) {
        const float perimeter = TorsoPerimeterAt(scene, neckY);
        put(out, kTransferNeck, perimeter * 100.0f, perimeter > 0.06f && perimeter < 0.60f, kTransferMesh, "skin contour at cf_s_neck height");
    }

    const float armL = ChainWorldLength(scene, ArmChain(true)), armR = ChainWorldLength(scene, ArmChain(false));
    put(out, kTransferArmLength, bothSides(armL, armR), armL > 0 || armR > 0, kTransferBone, "sum of arm / forearm bone segments");
    const float upL = ChainWorldLength(scene, UpperArmChain(true)), upR = ChainWorldLength(scene, UpperArmChain(false));
    put(out, kTransferUpperArm, bothSides(upL, upR), upL > 0 || upR > 0, kTransferBone, "sum of upper-arm bone segments");
    const float loL = ChainWorldLength(scene, LowerArmChain(true)), loR = ChainWorldLength(scene, LowerArmChain(false));
    put(out, kTransferLowerArm, bothSides(loL, loR), loL > 0 || loR > 0, kTransferBone, "sum of forearm / wrist bone segments");
    const float legL = ChainWorldLength(scene, LegChain(true)), legR = ChainWorldLength(scene, LegChain(false));
    put(out, kTransferLegLength, bothSides(legL, legR), legL > 0 || legR > 0, kTransferBone, "sum of thigh / leg bone segments");

    // Limb circumferences: the existing slicer picks the axis-centred trunk chain, so use the new
    // centroid-aware variant and pick the contour nearest each limb bone at mid-thigh / mid-calf.
    for (int side = 0; side < 2; ++side) {
        const bool left = side == 0;
        const char* suffix = left ? "_L" : "_R";
        const int thigh = scene.boneIndex(std::string("cf_s_thigh02") + suffix);
        const int calf = scene.boneIndex(std::string("cf_s_leg02") + suffix);
        if (thigh >= 0) {
            const Vec3& p = scene.bones[static_cast<size_t>(thigh)].pos;
            const float c = LimbPerimeterAt(scene, p.y, p.x, p.z) * 100.0f;   // metres -> cm, like every other row
            const float prev = out[kTransferThigh].value;
            if (c > 0.0f) put(out, kTransferThigh, bothSides(c, prev), true, kTransferMesh, "skin contour at cf_s_thigh02 height");
        }
        if (calf >= 0) {
            const Vec3& p = scene.bones[static_cast<size_t>(calf)].pos;
            const float c = LimbPerimeterAt(scene, p.y, p.x, p.z) * 100.0f;   // metres -> cm
            const float prev = out[kTransferCalf].value;
            if (c > 0.0f) put(out, kTransferCalf, bothSides(c, prev), true, kTransferMesh, "skin contour at cf_s_leg02 height");
        }
    }
    return out;
}

} // namespace cardbridge::measure
