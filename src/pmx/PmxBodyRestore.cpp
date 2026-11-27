#include "pmx/PmxBodyRestore.h"

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

} // namespace

RestoreResult RestoreBody(Model& doc, const assemble::BodyTemplate& body, const RestoreOptions& options) {
    RestoreResult result;
    std::map<std::string, int> byName;
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const Bone& b = doc.bones[i];
        if (!b.name.empty()) byName[lower(b.name)] = static_cast<int>(i);
        if (!b.nameEn.empty()) byName[lower(b.nameEn)] = static_cast<int>(i);
    }
    std::vector<int> mapping(body.boneNames.size(), -1);
    struct Match { Vec3 body, pmx; };
    std::vector<Match> matches;
    for (size_t i = 0; i < body.boneNames.size(); ++i) {
        auto it = byName.find(lower(body.boneNames[i]));
        if (it == byName.end() || i >= body.bonePositions.size()) continue;
        mapping[i] = it->second;
        matches.push_back({body.bonePositions[i], doc.bones[static_cast<size_t>(it->second)].position});
    }
    if (matches.size() < 6) {
        result.error = "skeleton incompatible: only " + std::to_string(matches.size()) +
                       " of " + std::to_string(body.boneNames.size()) +
                       " body bones have a matching name in this PMX";
        return result;
    }
    // Fit from matched rest-bone positions: translate the body so its bone
    // centroid lands on the PMX's, and scale only when the measured ratio is
    // close to 1. Nothing else about the imported model is altered.
    Vec3 bodyCentroid{0, 0, 0}, pmxCentroid{0, 0, 0};
    for (const Match& m : matches) {
        bodyCentroid.x += m.body.x; bodyCentroid.y += m.body.y; bodyCentroid.z += m.body.z;
        pmxCentroid.x += m.pmx.x; pmxCentroid.y += m.pmx.y; pmxCentroid.z += m.pmx.z;
    }
    const float inv = 1.0f / static_cast<float>(matches.size());
    bodyCentroid.x *= inv; bodyCentroid.y *= inv; bodyCentroid.z *= inv;
    pmxCentroid.x *= inv; pmxCentroid.y *= inv; pmxCentroid.z *= inv;
    float scale = 1.0f;
    if (options.allowUniformScale) {
        std::vector<float> ratios;
        for (const Match& m : matches) {
            const Vec3 db{m.body.x - bodyCentroid.x, m.body.y - bodyCentroid.y, m.body.z - bodyCentroid.z};
            const Vec3 dp{m.pmx.x - pmxCentroid.x, m.pmx.y - pmxCentroid.y, m.pmx.z - pmxCentroid.z};
            const float lb = length(db);
            if (lb > 1e-4f) ratios.push_back(length(dp) / lb);
        }
        if (!ratios.empty()) {
            std::sort(ratios.begin(), ratios.end());
            const float median = ratios[ratios.size() / 2];
            if (std::fabs(median - 1.0f) <= options.maxScaleDeviation) scale = median;
            else result.warnings.push_back("measured scale " + std::to_string(median) +
                                           " is outside the safe range; keeping 1.0 to preserve proportions");
        }
    }
    const Vec3 anchor{pmxCentroid.x - bodyCentroid.x * scale,
                      pmxCentroid.y - bodyCentroid.y * scale,
                      pmxCentroid.z - bodyCentroid.z * scale};
    result.warnings.push_back("fit from " + std::to_string(matches.size()) +
                              " matched bones: scale " + std::to_string(scale));

    const size_t baseVertices = doc.vertices.size();
    const size_t baseIndices = doc.indices.size();
    std::vector<int> remap(body.vertices.size(), -1);
    for (size_t vi = 0; vi < body.vertices.size(); ++vi) {
        const assemble::BodyVertex& v = body.vertices[vi];
        int bound[4] = {-1, -1, -1, -1};
        float weights[4] = {0, 0, 0, 0};
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            if (v.weight[k] <= 0.0f || v.bone[k] < 0 || static_cast<size_t>(v.bone[k]) >= mapping.size()) continue;
            const int mapped = mapping[static_cast<size_t>(v.bone[k])];
            if (mapped < 0) continue;
            bound[n] = mapped;
            weights[n] = v.weight[k];
            ++n;
        }
        if (!n) { ++result.skippedVertices; continue; }
        Vertex nv;
        nv.position = {v.pos.x * scale + anchor.x, v.pos.y * scale + anchor.y, v.pos.z * scale + anchor.z};
        nv.normal = v.normal;
        nv.uv = v.uv;
        for (int k = 0; k < 4; ++k) { nv.bone[k] = bound[k]; nv.weight[k] = weights[k]; }
        nv.deformType = n == 1 ? 0 : 2;
        if (n == 1) nv.weight[0] = 1.0f;
        remap[vi] = static_cast<int>(doc.vertices.size());
        doc.vertices.push_back(nv);
    }
    if (doc.vertices.size() == baseVertices) {
        result.error = "no restored body vertex could be bound to this skeleton";
        return result;
    }
    for (uint32_t index : body.indices) {
        if (index >= remap.size() || remap[index] < 0) continue;
        doc.indices.push_back(static_cast<uint32_t>(remap[index]));
    }

    Material mat;
    mat.name = "KK restored body";
    mat.nameEn = "KK restored body";
    mat.diffuse = {1, 1, 1, 1};
    mat.specular = {0, 0, 0};
    mat.specularPower = 5.0f;
    mat.ambient = {0.5f, 0.5f, 0.5f};
    mat.drawFlags = 0x1E;
    mat.edgeColor = {0, 0, 0, 1};
    mat.edgeSize = 1.0f;
    mat.toonFlag = 1;
    mat.sharedToon = 0;
    mat.indexCount = static_cast<uint32_t>(doc.indices.size() - baseIndices);
    doc.materials.push_back(mat);

    result.ok = true;
    result.addedVertices = doc.vertices.size() - baseVertices;
    result.addedIndices = doc.indices.size() - baseIndices;
    result.warnings.insert(result.warnings.begin(),
        "restored " + std::to_string(result.addedVertices) + " vertices from " + body.source +
        "; original vertices, morphs, frames and physics are unchanged");
    if (result.skippedVertices)
        result.warnings.push_back(std::to_string(result.skippedVertices) + " body vertices had no matching bone and were skipped");
    return result;
}

} // namespace cardbridge::pmx
