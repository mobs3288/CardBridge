#include "pmx/PmxAssetApply.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cardbridge::pmx {
namespace {

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

uint64_t indexCapacity(int size) {
    if (size == 1) return 255;
    if (size == 2) return 65535;
    return 0xFFFFFFFFull;
}

} // namespace

const char* FitStatusName(FitStatus s) {
    switch (s) {
        case FitStatus::Compatible: return "compatible";
        case FitStatus::CompatibleWithWarnings: return "compatible-with-warnings";
        case FitStatus::RequiresManualMapping: return "requires-manual-mapping";
        default: return "incompatible";
    }
}

AssetFitReport AnalyzeAssetFit(const library::AssetTemplate& asset, const Model& doc,
                               const BoneMatchOptions& options, bool allowPartial) {
    AssetFitReport report;
    report.materialCompatibility = std::to_string(asset.materials.size()) + " material(s), " +
                                   std::to_string(asset.textures.size()) + " texture(s)";
    std::vector<bool> required(asset.boneNames.size(), false);
    for (const auto& v : asset.vertices)
        for (int k = 0; k < 4; ++k)
            if (v.weight[k] > 0.0f && v.bone[k] >= 0 && static_cast<size_t>(v.bone[k]) < required.size())
                required[static_cast<size_t>(v.bone[k])] = true;
    report.requiredBones = static_cast<int>(std::count(required.begin(), required.end(), true));
    if (!report.requiredBones) {
        report.reason = "asset has no skinned influences";
        return report;
    }

    std::vector<std::string> targetNames;
    std::vector<Vec3> targetPositions;
    std::vector<int> targetParents;
    targetNames.reserve(doc.bones.size());
    targetPositions.reserve(doc.bones.size());
    targetParents.reserve(doc.bones.size());
    for (const Bone& b : doc.bones) {
        targetNames.push_back(b.nameEn.empty() ? b.name : b.nameEn);
        targetPositions.push_back(b.position);
        targetParents.push_back(b.parent);
    }
    report.mappings = ResolveBones(asset.boneNames, asset.bonePositions, asset.boneParents,
                                   targetNames, targetPositions, targetParents, options);

    for (size_t i = 0; i < required.size(); ++i) {
        if (!required[i]) continue;
        const BoneMapping& m = report.mappings[i];
        if (m.ok()) {
            report.matchedBones++;
            if (m.method == MatchMethod::Geometric || m.confidence < 0.7f)
                report.warnings.push_back(asset.boneNames[i] + " -> " + targetNames[static_cast<size_t>(m.target)] +
                                          " via " + MatchMethodName(m.method) + " (confidence " +
                                          std::to_string(m.confidence) + ")");
        } else if (m.ambiguous) {
            report.ambiguousBones++;
            report.warnings.push_back("ambiguous bone: " + asset.boneNames[i] + " (" + m.note + ")");
        } else {
            report.missingBones++;
            if (report.warnings.size() < 10)
                report.warnings.push_back("missing required bone: " + asset.boneNames[i]);
        }
    }
    const bool degraded = report.missingBones > 0 || report.ambiguousBones > 0;
    if (degraded && !allowPartial) {
        report.status = report.ambiguousBones ? FitStatus::RequiresManualMapping : FitStatus::Incompatible;
        report.reason = report.ambiguousBones ? "ambiguous bone mappings require manual resolution"
                                              : "incompatible skeleton: " + std::to_string(report.missingBones) +
                                                " required bone(s) unresolved";
        return report;
    }
    if (report.matchedBones == 0) {
        report.status = FitStatus::Incompatible;
        report.reason = "no required bone could be resolved to this PMX";
        return report;
    }
    if (degraded)
        report.warnings.push_back("partial mapping: " + std::to_string(report.missingBones) +
                                  " missing and " + std::to_string(report.ambiguousBones) +
                                  " ambiguous bones; their vertex influences are dropped");

    const uint64_t need = static_cast<uint64_t>(doc.vertices.size()) + asset.vertices.size();
    report.indexCapacityOk = need <= indexCapacity(doc.format.vertexSize());
    if (!report.indexCapacityOk) {
        report.status = FitStatus::Incompatible;
        report.reason = "asset would overflow the PMX vertex index size (" +
                        std::to_string(doc.format.vertexSize()) + " bytes)";
        return report;
    }
    const auto capacity = [](int size) -> uint64_t {
        return size == 1 ? 127 : (size == 2 ? 32767 : 0x7FFFFFFFu);
    };
    const uint64_t needTex = static_cast<uint64_t>(doc.textures.size()) + asset.textures.size();
    const uint64_t needMat = static_cast<uint64_t>(doc.materials.size()) +
                             std::max<size_t>(1, asset.submeshRanges.size());
    if (needTex > capacity(doc.format.textureSize()) || needMat > capacity(doc.format.materialSize())) {
        report.status = FitStatus::Incompatible;
        report.reason = "asset would overflow the PMX texture or material index size";
        return report;
    }
    if (asset.submeshRanges.empty() && !asset.materials.empty())
        report.warnings.push_back("submesh material associations were not extracted");

    int withParent = 0, parentAgrees = 0;
    struct Pair { Vec3 a, b; };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < required.size(); ++i) {
        if (!required[i]) continue;
        const BoneMapping& m = report.mappings[i];
        if (!m.ok()) continue;
        if (i < asset.boneParents.size() && asset.boneParents[i] >= 0) {
            ++withParent;
            const BoneMapping& pm = report.mappings[static_cast<size_t>(asset.boneParents[i])];
            if (pm.ok() && pm.target == doc.bones[static_cast<size_t>(m.target)].parent) ++parentAgrees;
        }
        if (i < asset.bonePositions.size())
            pairs.push_back({asset.bonePositions[i], doc.bones[static_cast<size_t>(m.target)].position});
    }
    report.hierarchyCompatibility = withParent ? static_cast<float>(parentAgrees) / withParent : 1.0f;
    if (report.hierarchyCompatibility < 0.7f)
        report.warnings.push_back("skeleton hierarchy agreement is low (" +
                                  std::to_string(report.hierarchyCompatibility) + ")");

    if (pairs.size() >= 2) {
        Vec3 ac{0, 0, 0}, pc{0, 0, 0};
        for (const Pair& p : pairs) {
            ac.x += p.a.x; ac.y += p.a.y; ac.z += p.a.z;
            pc.x += p.b.x; pc.y += p.b.y; pc.z += p.b.z;
        }
        const float inv = 1.0f / static_cast<float>(pairs.size());
        ac.x *= inv; ac.y *= inv; ac.z *= inv;
        pc.x *= inv; pc.y *= inv; pc.z *= inv;
        std::vector<float> ratios;
        for (const Pair& p : pairs) {
            const Vec3 da{p.a.x - ac.x, p.a.y - ac.y, p.a.z - ac.z};
            const float la = length(da);
            if (la > 1e-4f) ratios.push_back(length(Vec3{p.b.x - pc.x, p.b.y - pc.y, p.b.z - pc.z}) / la);
        }
        if (!ratios.empty()) {
            std::sort(ratios.begin(), ratios.end());
            report.scale = ratios[ratios.size() / 2];
            if (std::fabs(report.scale - 1.0f) > 0.25f) {
                report.warnings.push_back("measured scale " + std::to_string(report.scale) +
                                          " is outside the safe range; keeping 1.0");
                report.scale = 1.0f;
            }
        }
    }
    if (!pairs.empty()) {
        // Recompute centroids for the final translation (pairs may be empty above).
        Vec3 ac{0, 0, 0}, pc{0, 0, 0};
        for (const Pair& p : pairs) {
            ac.x += p.a.x; ac.y += p.a.y; ac.z += p.a.z;
            pc.x += p.b.x; pc.y += p.b.y; pc.z += p.b.z;
        }
        const float inv = 1.0f / static_cast<float>(pairs.size());
        ac.x *= inv; ac.y *= inv; ac.z *= inv;
        pc.x *= inv; pc.y *= inv; pc.z *= inv;
        report.translation = {pc.x - ac.x * report.scale, pc.y - ac.y * report.scale, pc.z - ac.z * report.scale};
    }
    report.status = report.warnings.empty() ? FitStatus::Compatible : FitStatus::CompatibleWithWarnings;
    report.compatible = true;
    return report;
}

AssetApplyResult ApplyAsset(Model& doc, const library::AssetTemplate& asset, const AssetFitReport& fit) {
    AssetApplyResult result;
    if (!fit.compatible) { result.error = fit.reason.empty() ? "asset is not compatible" : fit.reason; return result; }
    std::vector<int> boneMap(asset.boneNames.size(), -1);
    for (const BoneMapping& m : fit.mappings)
        if (m.source >= 0 && static_cast<size_t>(m.source) < boneMap.size() && m.ok())
            boneMap[static_cast<size_t>(m.source)] = m.target;

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
            if (v.weight[k] <= 0.0f || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= boneMap.size()) continue;
            const int mapped = boneMap[static_cast<size_t>(v.bone[k])];
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

    const size_t baseMaterials = doc.materials.size();
    const size_t baseTextures = doc.textures.size();
    result.textureStart = baseTextures;
    auto safeStem = [](std::string s) {
        for (char& c : s) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!std::isalnum(u) && c != '-' && c != '_' && c != '.') c = '_';
        }
        return s.substr(0, 80);
    };
    for (const library::AssetTexture& tex : asset.textures) {
        std::string name = safeStem(tex.name.empty() ? "texture.png" : tex.name);
        std::string unique = name;
        for (int n = 2; std::find(doc.textures.begin(), doc.textures.end(), "textures/" + unique) != doc.textures.end(); ++n) {
            const size_t dot = name.rfind('.');
            unique = (dot == std::string::npos ? name : name.substr(0, dot)) + "_" + std::to_string(n) +
                     (dot == std::string::npos ? "" : name.substr(dot));
        }
        doc.textures.push_back("textures/" + unique);
        result.importedTextureBytes.push_back(tex.bytes);
    }
    auto mapTexture = [&](int local) -> int {
        if (local < 0 || static_cast<size_t>(local) >= asset.textures.size()) return -1;
        return static_cast<int>(baseTextures + static_cast<size_t>(local));
    };

    // Preserve submesh material association. PMX assigns faces to materials by
    // contiguous index ranges, so ranges are appended in extraction order.
    std::vector<std::pair<size_t, size_t>> ranges = asset.submeshRanges;
    if (ranges.empty()) ranges.emplace_back(0, asset.indices.size());
    for (size_t r = 0; r < ranges.size(); ++r) {
        const size_t first = ranges[r].first, count = ranges[r].second;
        const size_t before = doc.indices.size();
        for (size_t k = first; k + 2 < first + count && k + 2 < asset.indices.size(); k += 3) {
            const uint32_t a = asset.indices[k], b = asset.indices[k + 1], c = asset.indices[k + 2];
            if (a >= remap.size() || b >= remap.size() || c >= remap.size()) continue;
            if (remap[a] < 0 || remap[b] < 0 || remap[c] < 0) continue;   // never keep a partial triangle
            doc.indices.push_back(static_cast<uint32_t>(remap[a]));
            doc.indices.push_back(static_cast<uint32_t>(remap[b]));
            doc.indices.push_back(static_cast<uint32_t>(remap[c]));
        }
        Material mat;
        const library::AssetMaterial* src = nullptr;
        if (r < asset.submeshMaterials.size() && static_cast<size_t>(asset.submeshMaterials[r]) < asset.materials.size())
            src = &asset.materials[static_cast<size_t>(asset.submeshMaterials[r])];
        if (src) {
            mat.name = src->name.empty() ? asset.name : src->name;
            mat.nameEn = mat.name;
            mat.diffuse = src->diffuse;
            mat.ambient = src->ambient;
            mat.specular = src->specular;
            mat.specularPower = src->specularPower;
            mat.edgeColor = {src->edgeColor.x, src->edgeColor.y, src->edgeColor.z, 1.0f};
            mat.edgeSize = src->edgeSize;
            mat.drawFlags = src->drawFlags;
            mat.texture = mapTexture(src->diffuseTexture);
            mat.sphere = mapTexture(src->sphereTexture);
            mat.sphereMode = src->sphereMode;
            mat.toon = mapTexture(src->toonTexture);
            mat.toonFlag = mat.toon >= 0 ? 0 : 1;
            mat.sharedToon = 0;
        } else {
            mat.name = asset.name.empty() ? "Applied asset" : asset.name;
            mat.nameEn = mat.name;
            mat.diffuse = {1, 1, 1, 1};
            mat.ambient = {0.5f, 0.5f, 0.5f};
            mat.drawFlags = 0x1E;
            mat.edgeColor = {0, 0, 0, 1};
            mat.edgeSize = 1.0f;
            mat.toonFlag = 1;
            result.warnings.push_back("material data for submesh " + std::to_string(r) + " was unavailable; using a default material");
        }
        mat.indexCount = static_cast<uint32_t>(doc.indices.size() - before);
        doc.materials.push_back(mat);
    }
    result.addedMaterials = doc.materials.size() - baseMaterials;
    result.addedTextures = doc.textures.size() - baseTextures;
    result.ok = true;
    result.addedVertices = doc.vertices.size() - base;
    result.addedIndices = doc.indices.size() - baseIndices;
    return result;
}

} // namespace cardbridge::pmx
