#include "pmx/PmxBoneGraft.h"

#include <algorithm>
#include <map>

namespace cardbridge::pmx {
namespace {

uint64_t boneCapacity(int size) { return size == 1 ? 127 : (size == 2 ? 32767 : 0x7FFFFFFFu); }

bool validRef(int r, int oldCount, int newCount) { return r >= -1 && r < newCount; }

} // namespace

GraftResult AnalyzeBoneGraft(const GraftSpec& spec, const Model& doc) {
    GraftResult result;
    if (!spec.confirmed) { result.error = "bone grafting requires explicit confirmation"; return result; }
    if (spec.bones.empty()) { result.error = "no bones to graft"; return result; }
    const size_t oldN = doc.bones.size();
    const int newN = static_cast<int>(oldN + spec.bones.size());
    if (static_cast<uint64_t>(newN) > boneCapacity(doc.format.boneSize())) {
        result.error = "bone capacity of the imported PMX format exceeded";
        return result;
    }
    std::map<std::string, int> names;
    for (const Bone& b : doc.bones) {
        if (!b.name.empty()) ++names[b.name];
        if (!b.nameEn.empty()) ++names[b.nameEn];
    }
    for (size_t i = 0; i < spec.bones.size(); ++i) {
        const Bone& b = spec.bones[i].bone;
        if (b.name.empty()) { result.error = "graft bone " + std::to_string(i) + " has no name"; return result; }
        if (names.count(b.name) || (!b.nameEn.empty() && names.count(b.nameEn))) {
            result.error = "duplicate bone name '" + b.name + "'";
            return result;
        }
        names[b.name]++;
        if (!b.nameEn.empty()) names[b.nameEn]++;
        if (!validRef(b.parent, static_cast<int>(oldN), newN) || b.parent == static_cast<int>(oldN + i)) {
            result.error = "invalid parent on graft bone '" + b.name + "'";
            return result;
        }
        if (!validRef(b.tailBone, static_cast<int>(oldN), newN) || b.tailBone == static_cast<int>(oldN + i)) {
            result.error = "invalid tail on graft bone '" + b.name + "'";
            return result;
        }
        if (!validRef(b.appendParent, static_cast<int>(oldN), newN) || b.appendParent == static_cast<int>(oldN + i)) {
            result.error = "invalid append parent on graft bone '" + b.name + "'";
            return result;
        }
        const int appendLayer = b.appendParent < 0 ? 0 :
            (b.appendParent < static_cast<int>(oldN) ? doc.bones[static_cast<size_t>(b.appendParent)].layer
                                                     : spec.bones[static_cast<size_t>(b.appendParent - static_cast<int>(oldN))].bone.layer);
        if (b.appendParent >= 0 && b.layer <= appendLayer) {
            result.error = "append parent of '" + b.name + "' must have a lower layer";
            return result;
        }
        if (b.ikTarget >= 0) {
            if (!validRef(b.ikTarget, static_cast<int>(oldN), newN) || b.ikLinks.empty()) {
                result.error = "invalid IK configuration on graft bone '" + b.name + "'";
                return result;
            }
            for (const IkLink& link : b.ikLinks)
                if (!validRef(link.bone, static_cast<int>(oldN), newN)) {
                    result.error = "IK link out of range on graft bone '" + b.name + "'";
                    return result;
                }
        }
        if (spec.bones[i].remapWeights && (spec.bones[i].sourceBone < 0 || spec.bones[i].sourceBone >= newN)) {
            result.error = "weight remap source out of range on graft bone '" + b.name + "'";
            return result;
        }
    }
    // Cycle check over grafted parents only.
    for (size_t i = 0; i < spec.bones.size(); ++i) {
        int cur = static_cast<int>(oldN + i);
        for (size_t steps = 0; steps <= spec.bones.size(); ++steps) {
            if (cur < static_cast<int>(oldN)) break;
            const size_t idx = static_cast<size_t>(cur - static_cast<int>(oldN));
            cur = spec.bones[idx].bone.parent;
            if (cur == static_cast<int>(oldN + i)) { result.error = "parent cycle on grafted bones"; return result; }
        }
    }
    result.ok = true;
    return result;
}

GraftResult AppendBones(Model& doc, const GraftSpec& spec) {
    const GraftResult check = AnalyzeBoneGraft(spec, doc);
    if (!check.ok) return check;
    GraftResult result = check;
    const size_t oldN = doc.bones.size();
    for (const GraftBone& gb : spec.bones) doc.bones.push_back(gb.bone);
    for (size_t i = 0; i < spec.bones.size(); ++i) {
        const GraftBone& gb = spec.bones[i];
        if (!gb.remapWeights) continue;
        const int src = gb.sourceBone;
        const int dst = static_cast<int>(oldN + i);
        for (Vertex& v : doc.vertices) {
            for (int k = 0; k < 4; ++k) {
                if (v.bone[k] != src || v.weight[k] <= 0.0f) continue;
                int free = -1;
                for (int j = 0; j < 4; ++j) if (v.weight[j] <= 0.0f) { free = j; break; }
                if (free < 0) continue;
                const float half = v.weight[k] * 0.5f;
                v.weight[k] = half;
                v.bone[free] = dst;
                v.weight[free] = half;
                ++result.remappedVertices;
                break;
            }
        }
    }
    result.addedBones = spec.bones.size();
    return result;
}

} // namespace cardbridge::pmx
