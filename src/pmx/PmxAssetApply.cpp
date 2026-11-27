#include "pmx/PmxAssetApply.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace cardbridge::pmx {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

uint64_t indexCapacity(int size) {
    if (size == 1) return 255;
    if (size == 2) return 65535;
    return 0xFFFFFFFFull;
}

} // namespace

AssetFitReport AnalyzeAssetFit(const library::AssetTemplate& asset, const Model& doc) {
    AssetFitReport report;
    std::map<std::string, int> byName;
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        if (!doc.bones[i].name.empty()) byName[lower(doc.bones[i].name)] = static_cast<int>(i);
        if (!doc.bones[i].nameEn.empty()) byName[lower(doc.bones[i].nameEn)] = static_cast<int>(i);
    }
    std::vector<int> mapping(asset.boneNames.size(), -1);
    std::vector<bool> required(asset.boneNames.size(), false);
    for (const auto& v : asset.vertices)
        for (int k = 0; k < 4; ++k)
            if (v.weight[k] > 0.0f && v.bone[k] >= 0 && static_cast<size_t>(v.bone[k]) < required.size())
                required[static_cast<size_t>(v.bone[k])] = true;
    for (size_t i = 0; i < asset.boneNames.size(); ++i) {
        if (!required[i]) continue;
        report.requiredBones++;
        auto it = byName.find(lower(asset.boneNames[i]));
        if (it != byName.end()) { mapping[i] = it->second; report.matchedBones++; }
        else if (report.warnings.size() < 8)
            report.warnings.push_back("missing required bone: " + asset.boneNames[i]);
    }
    if (!report.requiredBones) {
        report.reason = "asset has no skinned influences";
        return report;
    }
    if (report.matchedBones != report.requiredBones) {
        report.reason = "incompatible skeleton: " + std::to_string(report.matchedBones) + " of " +
                        std::to_string(report.requiredBones) + " required bones are present in this PMX";
        return report;
    }
    const uint64_t need = static_cast<uint64_t>(doc.vertices.size()) + asset.vertices.size();
    if (need > indexCapacity(doc.format.vertexSize())) {
        report.reason = "asset would overflow the PMX vertex index size (" +
                        std::to_string(doc.format.vertexSize()) + " bytes)";
        return report;
    }
    struct Match { Vec3 asset, pmx; };
    std::vector<Match> matches;
    for (size_t i = 0; i < asset.boneNames.size(); ++i) {
        if (mapping[i] < 0 || i >= asset.bonePositions.size()) continue;
        matches.push_back({asset.bonePositions[i], doc.bones[static_cast<size_t>(mapping[i])].position});
    }
    if (matches.empty()) { report.reason = "no matched rest-bone positions to fit from"; return report; }
    Vec3 ac{0, 0, 0}, pc{0, 0, 0};
    for (const Match& m : matches) {
        ac.x += m.asset.x; ac.y += m.asset.y; ac.z += m.asset.z;
        pc.x += m.pmx.x; pc.y += m.pmx.y; pc.z += m.pmx.z;
    }
    const float inv = 1.0f / static_cast<float>(matches.size());
    ac.x *= inv; ac.y *= inv; ac.z *= inv;
    pc.x *= inv; pc.y *= inv; pc.z *= inv;
    std::vector<float> ratios;
    for (const Match& m : matches) {
        const Vec3 da{m.asset.x - ac.x, m.asset.y - ac.y, m.asset.z - ac.z};
        const float la = length(da);
        if (la > 1e-4f) {
            const Vec3 dp{m.pmx.x - pc.x, m.pmx.y - pc.y, m.pmx.z - pc.z};
            ratios.push_back(length(dp) / la);
        }
    }
    if (!ratios.empty()) {
        std::sort(ratios.begin(), ratios.end());
        const float median = ratios[ratios.size() / 2];
        if (std::fabs(median - 1.0f) <= 0.25f) report.scale = median;
        else report.warnings.push_back("measured scale " + std::to_string(median) +
                                       " is outside the safe range; keeping 1.0");
    }
    report.translation = {pc.x - ac.x * report.scale, pc.y - ac.y * report.scale, pc.z - ac.z * report.scale};
    report.warnings.insert(report.warnings.begin(),
        "fit from " + std::to_string(matches.size()) + " matched bones; scale " + std::to_string(report.scale));
    report.compatible = true;
    return report;
}

AssetApplyResult ApplyAsset(Model& doc, const library::AssetTemplate& asset, const AssetFitReport& fit) {
    AssetApplyResult result;
    if (!fit.compatible) { result.error = fit.reason.empty() ? "asset is not compatible" : fit.reason; return result; }
    std::map<std::string, int> byName;
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        if (!doc.bones[i].name.empty()) byName[lower(doc.bones[i].name)] = static_cast<int>(i);
        if (!doc.bones[i].nameEn.empty()) byName[lower(doc.bones[i].nameEn)] = static_cast<int>(i);
    }
    std::vector<int> mapping(asset.boneNames.size(), -1);
    for (size_t i = 0; i < asset.boneNames.size(); ++i) {
        auto it = byName.find(lower(asset.boneNames[i]));
        if (it != byName.end()) mapping[i] = it->second;
    }
    const size_t base = doc.vertices.size();
    const size_t baseIndices = doc.indices.size();
    std::vector<int> remap(asset.vertices.size(), -1);
    for (size_t i = 0; i < asset.vertices.size(); ++i) {
        const auto& v = asset.vertices[i];
        Vertex nv;
        nv.position = {v.pos.x * fit.scale + fit.translation.x,
                       v.pos.y * fit.scale + fit.translation.y,
                       v.pos.z * fit.scale + fit.translation.z};
        nv.normal = v.normal;
        nv.uv = v.uv;
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            if (v.weight[k] <= 0.0f || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= mapping.size()) continue;
            const int mapped = mapping[static_cast<size_t>(v.bone[k])];
            if (mapped < 0) continue;
            nv.bone[n] = mapped; nv.weight[n] = v.weight[k]; ++n;
        }
        if (!n) continue;
        nv.deformType = n == 1 ? 0 : 2;
        if (n == 1) nv.weight[0] = 1.0f;
        remap[i] = static_cast<int>(doc.vertices.size());
        doc.vertices.push_back(nv);
    }
    if (doc.vertices.size() == base) { result.error = "no asset vertex could be bound"; return result; }
    for (uint32_t idx : asset.indices)
        if (idx < remap.size() && remap[idx] >= 0) doc.indices.push_back(static_cast<uint32_t>(remap[idx]));
    Material mat;
    mat.name = asset.name.empty() ? "Applied asset" : asset.name;
    mat.nameEn = mat.name;
    mat.diffuse = {1, 1, 1, 1};
    mat.ambient = {0.5f, 0.5f, 0.5f};
    mat.drawFlags = 0x1E;
    mat.edgeColor = {0, 0, 0, 1};
    mat.edgeSize = 1.0f;
    mat.toonFlag = 1;
    mat.indexCount = static_cast<uint32_t>(doc.indices.size() - baseIndices);
    doc.materials.push_back(mat);
    result.ok = true;
    result.addedVertices = doc.vertices.size() - base;
    result.addedIndices = doc.indices.size() - baseIndices;
    return result;
}

} // namespace cardbridge::pmx
