#include "pmx/BoneMatcher.h"

#include "pmx/BoneAliases.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::pmx {
namespace {

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 mul(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

Vec3 centroidOf(const std::vector<Vec3>& p) {
    Vec3 c{0, 0, 0};
    if (p.empty()) return c;
    for (const Vec3& v : p) { c.x += v.x; c.y += v.y; c.z += v.z; }
    const float inv = 1.0f / static_cast<float>(p.size());
    c.x *= inv; c.y *= inv; c.z *= inv;
    return c;
}

float scaleOf(const std::vector<Vec3>& p, const Vec3& c) {
    float maxD = 0.0f;
    for (const Vec3& v : p) maxD = std::max(maxD, length(sub(v, c)));
    return std::max(maxD, 1e-4f);
}

bool qualifiersMatch(const BoneIdentity& a, const BoneIdentity& b) {
    return a.side == b.side && a.twist == b.twist && a.ik == b.ik && a.helper == b.helper;
}

} // namespace

const char* MatchMethodName(MatchMethod m) {
    switch (m) {
        case MatchMethod::Exact: return "exact";
        case MatchMethod::Normalized: return "normalized";
        case MatchMethod::Alias: return "alias";
        case MatchMethod::Hierarchy: return "hierarchy";
        case MatchMethod::Geometric: return "geometric";
        default: return "none";
    }
}

std::vector<BoneMapping> ResolveBones(const std::vector<std::string>& sourceNames,
                                      const std::vector<Vec3>& sourcePositions,
                                      const std::vector<int>& sourceParents,
                                      const std::vector<std::string>& targetNames,
                                      const std::vector<Vec3>& targetPositions,
                                      const std::vector<int>& targetParents,
                                      const BoneMatchOptions& options) {
    const size_t ns = sourceNames.size(), nt = targetNames.size();
    std::vector<BoneMapping> out(ns);
    for (size_t i = 0; i < ns; ++i) out[i].source = static_cast<int>(i);
    std::vector<bool> used(nt, false);

    std::vector<std::string> srcNorm(ns), dstNorm(nt);
    std::vector<BoneIdentity> srcId(ns), dstId(nt);
    for (size_t i = 0; i < ns; ++i) { srcNorm[i] = NormalizeBoneName(sourceNames[i]); srcId[i] = IdentifyBone(sourceNames[i]); }
    for (size_t j = 0; j < nt; ++j) { dstNorm[j] = NormalizeBoneName(targetNames[j]); dstId[j] = IdentifyBone(targetNames[j]); }

    // Stages 1-3: exact, normalized, explicit alias. A match that is not unique
    // is marked ambiguous and never applied.
    for (int stage = 0; stage < 3; ++stage) {
        for (size_t i = 0; i < ns; ++i) {
            if (out[i].target >= 0 || out[i].ambiguous) continue;
            std::vector<size_t> hits;
            for (size_t j = 0; j < nt; ++j) {
                if (used[j]) continue;
                bool hit = false;
                if (stage == 0) hit = sourceNames[i] == targetNames[j];
                else if (stage == 1) hit = srcNorm[i] == dstNorm[j] && !srcNorm[i].empty();
                else hit = srcId[i].valid && dstId[j].valid && EquivalentBone(srcId[i], dstId[j]);
                if (hit) hits.push_back(j);
            }
            if (hits.size() == 1) {
                const size_t j = hits[0];
                out[i].target = static_cast<int>(j);
                out[i].method = static_cast<MatchMethod>(stage + 1);
                out[i].confidence = stage == 0 ? 1.0f : (stage == 1 ? 0.95f : 0.85f);
                used[j] = true;
                if (i < sourcePositions.size() && j < targetPositions.size())
                    out[i].deviation = length(sub(sourcePositions[i], targetPositions[j]));
            } else if (hits.size() > 1) {
                out[i].ambiguous = true;
                out[i].note = "multiple candidates at " + std::string(MatchMethodName(static_cast<MatchMethod>(stage + 1))) + " stage";
            }
        }
    }

    const Vec3 sc = centroidOf(sourcePositions), tc = centroidOf(targetPositions);
    const float ss = scaleOf(sourcePositions, sc), ts = scaleOf(targetPositions, tc);

    // Stage 4: hierarchy. A child of an already-mapped parent whose direction
    // from that parent agrees with the source is a weak but useful signal.
    for (size_t i = 0; i < ns; ++i) {
        if (out[i].target >= 0 || out[i].ambiguous) continue;
        const int sp = i < sourceParents.size() ? sourceParents[i] : -1;
        if (sp < 0 || static_cast<size_t>(sp) >= ns || out[static_cast<size_t>(sp)].target < 0) continue;
        const int mappedParent = out[static_cast<size_t>(sp)].target;
        const Vec3 sdir = i < sourcePositions.size() ? sub(sourcePositions[i], sourcePositions[static_cast<size_t>(sp)]) : Vec3{};
        const float sl = length(sdir);
        if (sl < 1e-5f) continue;
        struct Candidate { size_t j; float score; };
        std::vector<Candidate> candidates;
        for (size_t j = 0; j < nt; ++j) {
            if (used[j]) continue;
            int tp = j < targetParents.size() ? targetParents[j] : -1;
            if (tp != mappedParent) continue;
            if (!qualifiersMatch(srcId[i], dstId[j])) continue;
            const Vec3 tdir = sub(targetPositions[j], targetPositions[static_cast<size_t>(mappedParent)]);
            const float tl = length(tdir);
            if (tl < 1e-5f) continue;
            const float agree = dot(sdir, tdir) / (sl * tl);
            const float semantic = srcId[i].valid && dstId[j].valid && srcId[i].canonical == dstId[j].canonical ? 0.15f : 0.0f;
            candidates.push_back({j, 0.62f + 0.18f * agree + semantic});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        if (candidates.size() == 1 && candidates[0].score >= options.minConfidence) {
            const size_t j = candidates[0].j;
            out[i].target = static_cast<int>(j);
            out[i].method = MatchMethod::Hierarchy;
            out[i].confidence = candidates[0].score;
            out[i].deviation = length(sub(sourcePositions[i], targetPositions[j])) / ts;
            used[j] = true;
        } else if (candidates.size() > 1 &&
                   candidates[0].score - candidates[1].score < options.ambiguityMargin) {
            out[i].ambiguous = true;
            out[i].note = "multiple hierarchy candidates";
        }
    }

    // Stage 5: conservative geometric fallback on normalized rest positions.
    if (options.allowGeometric) {
        for (size_t i = 0; i < ns; ++i) {
            if (out[i].target >= 0 || out[i].ambiguous) continue;
            if (i >= sourcePositions.size()) continue;
            const Vec3 pn = mul(sub(sourcePositions[i], sc), 1.0f / ss);
            struct Candidate { size_t j; float deviation; float score; };
            std::vector<Candidate> candidates;
            for (size_t j = 0; j < nt; ++j) {
                if (used[j] || j >= targetPositions.size()) continue;
                if (!qualifiersMatch(srcId[i], dstId[j])) continue;
                const Vec3 tn = mul(sub(targetPositions[j], tc), 1.0f / ts);
                const float d = length(sub(pn, tn));
                if (d > options.maxGeometricDeviation) continue;
                candidates.push_back({j, d, 0.78f - d * 0.6f});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.deviation < b.deviation; });
            if (candidates.size() == 1 && candidates[0].score >= options.minConfidence) {
                const size_t j = candidates[0].j;
                out[i].target = static_cast<int>(j);
                out[i].method = MatchMethod::Geometric;
                out[i].confidence = candidates[0].score;
                out[i].deviation = candidates[0].deviation;
                used[j] = true;
                out[i].note = "geometric fallback (no name or hierarchy match)";
            } else if (candidates.size() > 1 &&
                       candidates[1].deviation - candidates[0].deviation < options.ambiguityMargin) {
                out[i].ambiguous = true;
                out[i].note = "multiple geometric candidates";
            }
        }
    }
    return out;
}

} // namespace cardbridge::pmx
