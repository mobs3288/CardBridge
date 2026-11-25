#include "assemble/GarmentSupport.h"

#include "assemble/BodySuppression.h"
#include "assemble/CoverageGrid.h"
#include "assemble/MeshQueries.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

namespace cardbridge::assemble {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool BreastBoneName(const std::string& n) {
    return n.find("bust") != std::string::npos || n.find("bnip") != std::string::npos;
}

// A garment that is fully translucent does not cover or support anything: the skin shows through it by design.
bool PartOpaque(const Scene& scene, const ScenePart& part) {
    bool any = false;
    for (int mi : part.meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        for (const auto& sm : scene.meshes[static_cast<size_t>(mi)].submeshes) {
            const SceneMaterial* m = sm.material >= 0 && static_cast<size_t>(sm.material) < scene.materials.size()
                                     ? &scene.materials[static_cast<size_t>(sm.material)] : nullptr;
            if (!m || m->translucent || m->hidden || m->fullyCutByAlpha || sm.indices.empty()) continue;
            any = true;
        }
    }
    return any;
}

// The bra slot (category 107 / id "...bra..." on a clothing layer) is one piece of evidence, never the decision.
bool BraSlotMetadata(const ScenePart& p) {
    if (p.underPiece) return false;
    if (p.category == 107) return true;
    if (p.id.rfind("outfit", 0) != 0 && p.group != "Outfit") return false;
    const std::string s = p.id;
    size_t at = s.rfind(".bra");
    if (at == std::string::npos) return false;
    size_t end = at + 4;
    return end == s.size() || s[end] == '.';   // "outfit0.bra", "outfit0.bra.l2", not "outfit0.brassiere-accessory"
}

bool InRegion(const BreastRegion& r, Vec3 v, int side, float margin = 0.0f) {
    const Vec3& c = side > 0 ? r.left : r.right;
    const Vec3 d = sub(v, c);
    return dot(d, d) <= (r.radius + margin) * (r.radius + margin);
}

// Sampled vertex references: a mesh with 30k vertices contributes ~1200 samples, which is plenty for a coverage
// statistic and keeps the classification bounded for a modded mesh with an enormous vertex count.
struct Samples {
    std::vector<std::pair<int, size_t>> refs;
    void add(const Scene& scene, int mi) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) return;
        const auto& vs = scene.meshes[static_cast<size_t>(mi)].vertices;
        const size_t step = std::max<size_t>(1, vs.size() / 1200);
        for (size_t i = 0; i < vs.size(); i += step) refs.emplace_back(mi, i);
    }
    const SceneVertex& at(const Scene& scene, size_t i) const {
        const auto& [mi, vi] = refs[i];
        return scene.meshes[static_cast<size_t>(mi)].vertices[vi];
    }
};

// A uniform grid of the body's own vertices, used to measure how closely a candidate follows the bust surface.
// Built once per classification and reused for all candidates. Body vertices are points, not triangles: a dense
// Koikatsu body mesh makes that a good enough approximation without triangulating a second copy of the geometry.
struct BodyGrid {
    Vec3 origin{0, 0, 0};
    float cell = 0.015f;
    int nx = 0, ny = 0, nz = 0;
    std::vector<std::vector<size_t>> cells;
    std::vector<Vec3> points;

    int ix(float x) const { return std::min(nx - 1, std::max(0, static_cast<int>((x - origin.x) / cell))); }
    void build(const Scene& scene, const BreastRegion& r) {
        const float pad = 0.12f;
        const Vec3 lo{r.left.x - r.radius - pad, std::min(r.yMin, r.waistY) - pad, std::min(r.left.z, r.right.z) - r.radius - pad};
        const Vec3 hi{r.right.x + r.radius + pad, r.yMax + pad, std::max(r.left.z, r.right.z) + r.radius + pad};
        origin = lo;
        nx = std::max(1, static_cast<int>((hi.x - lo.x) / cell) + 1);
        ny = std::max(1, static_cast<int>((hi.y - lo.y) / cell) + 1);
        nz = std::max(1, static_cast<int>((hi.z - lo.z) / cell) + 1);
        if (static_cast<size_t>(nx) * ny * nz > 400000u) { cell = 0.03f; build(scene, r); return; }

        cells.assign(static_cast<size_t>(nx) * ny * nz, {});
        for (const auto& p : scene.parts) {
            if (p.id != "body" || p.underPiece) continue;
            for (int mi : p.meshes) {
                if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
                for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                    if (!finite(v.pos)) continue;
                    const int x = ix(v.pos.x), y = std::min(ny - 1, std::max(0, static_cast<int>((v.pos.y - origin.y) / cell))),
                              z = std::min(nz - 1, std::max(0, static_cast<int>((v.pos.z - origin.z) / cell)));
                    points.push_back(v.pos);
                    cells[static_cast<size_t>(z) * ny * nx + static_cast<size_t>(y) * nx + static_cast<size_t>(x)].push_back(points.size() - 1);
                }
            }
        }
    }
    float nearest(Vec3 p, float maxDist) const {
        if (points.empty()) return std::numeric_limits<float>::max();
        const int x = ix(p.x), y = std::min(ny - 1, std::max(0, static_cast<int>((p.y - origin.y) / cell))),
                  z = std::min(nz - 1, std::max(0, static_cast<int>((p.z - origin.z) / cell)));
        const int reach = std::max(1, static_cast<int>(std::ceil(maxDist / cell)));
        float best = std::numeric_limits<float>::max();
        for (int dz = -reach; dz <= reach; ++dz) for (int dy = -reach; dy <= reach; ++dy) for (int dx = -reach; dx <= reach; ++dx) {
            const int xx = x + dx, yy = y + dy, zz = z + dz;
            if (xx < 0 || xx >= nx || yy < 0 || yy >= ny || zz < 0 || zz >= nz) continue;
            for (size_t id : cells[static_cast<size_t>(zz) * ny * nx + static_cast<size_t>(yy) * nx + static_cast<size_t>(xx)])
                best = std::min(best, length(sub(points[id], p)));
        }
        return best;
    }
};

// Opaque triangles of one candidate, tagged with its part id so the shared grid can be used unchanged.
std::vector<CoverageProvider> ProvidersOf(const Scene& scene, const ScenePart& part, const BreastRegion& r) {
    std::vector<CoverageProvider> out;
    for (const ScenePart& entry : scene.parts) {
        const ScenePart* owner = &entry;
        // A top's OverBraMask moves some of the bra's triangles onto an ".under." piece (kept for the nude morph).
        // It is still the SAME worn garment, so it still supplies support evidence; ignoring it would miss exactly
        // the common case of a bra under a shirt.
        if (owner->id != part.id && owner->underOf != part.id) continue;
        for (int mi : owner->meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& m = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sm : m.submeshes) {
                const SceneMaterial* mat = sm.material >= 0 && static_cast<size_t>(sm.material) < scene.materials.size()
                                           ? &scene.materials[static_cast<size_t>(sm.material)] : nullptr;
                if (!mat || mat->translucent || mat->hidden || mat->fullyCutByAlpha) continue;
                for (size_t t = 0; t + 2 < sm.indices.size(); t += 3) {
                    const Vec3 a = m.vertices[sm.indices[t]].pos, b = m.vertices[sm.indices[t + 1]].pos, c = m.vertices[sm.indices[t + 2]].pos;
                    if (!finite(a) || !finite(b) || !finite(c)) continue;
                    if (!InRegion(r, a, 1, 0.1f) && !InRegion(r, a, -1, 0.1f)) continue;
                    const Vec3 u = sub(b, a);
                    const Vec3 v = sub(c, a);
                    const Vec3 face{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
                    out.push_back({MakeTri(a, b, c, face), part.id});
                }
            }
        }
    }
    return out;
}

// Fraction of the body's breast-surface samples that an opaque garment triangle covers within a short ray.
// Coverage is measured per side: a pastie covering both nipples covers only a tiny fraction of each breast.
float SideCoverage(const Scene& scene, const BreastRegion& r, const std::vector<CoverageProvider>& providers, int side) {
    if (providers.empty()) return 0.0f;
    float lo[3] = {std::min(r.left.x, r.right.x) - r.radius - 0.1f, r.yMin - 0.1f, std::min(r.left.z, r.right.z) - r.radius - 0.1f};
    float hi[3] = {std::max(r.left.x, r.right.x) + r.radius + 0.1f, r.yMax + 0.1f, std::max(r.left.z, r.right.z) + r.radius + 0.1f};
    const CoverageGrid grid = CoverageGrid::Build(providers, lo, hi);
    size_t total = 0, covered = 0;
    for (const auto& p : scene.parts) {
        if (p.id != "body" || p.underPiece) continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& m = scene.meshes[static_cast<size_t>(mi)];
            const size_t step = std::max<size_t>(1, m.vertices.size() / 800);
            for (size_t vi = 0; vi < m.vertices.size(); vi += step) {
                const SceneVertex& v = m.vertices[vi];
                if (!InRegion(r, v.pos, side, 0.0f) || v.pos.y < r.yMin || v.pos.y > r.yMax || !finite(v.normal)) continue;
                Vec3 n = normalize(v.normal);
                if (!finite(n) || length(n) < 0.5f) continue;
                ++total;
                const Vec3 ro{v.pos.x + n.x * 1e-4f, v.pos.y + n.y * 1e-4f, v.pos.z + n.z * 1e-4f};
                if (grid.grid.forEachTriangleOnRay(ro, n, 1e-5f, 0.06f, [&](size_t ti) {
                        float t = 0.0f;
                        return RayHitsAabb(ro, n, 1e-5f, 0.06f, providers[ti].tri) &&
                               RayHitsTriangle(ro, n, providers[ti].tri, 1e-5f, 0.06f, t);
                    }))
                    ++covered;
            }
        }
    }
    return total ? static_cast<float>(covered) / static_cast<float>(total) : 0.0f;
}

float Median(std::vector<float> v) {
    if (v.empty()) return std::numeric_limits<float>::max();
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void note(GarmentEvidence& e, const std::string& s) { e.notes.push_back(s); }

} // namespace

const char* GarmentSupportVerdictName(GarmentSupportVerdict v) {
    switch (v) {
    case GarmentSupportVerdict::Supportive: return "supportive";
    case GarmentSupportVerdict::NonSupportive: return "non-supportive";
    case GarmentSupportVerdict::Other: return "other";
    case GarmentSupportVerdict::Unknown: return "unknown";
    }
    return "unknown";
}

bool BuildBreastRegion(const Scene& scene, BreastRegion& out, std::string* why) {
    std::vector<Vec3> left, right;
    Vec3 root[2] = {{0, 0, 0}, {0, 0, 0}};
    bool haveRoot[2] = {false, false};
    for (const auto& b : scene.bones) {
        if (!BreastBoneName(b.name) || !finite(b.pos)) continue;
        const int side = BoneSideSign(b.name);
        if (side == 0) continue;
        const int i = side > 0 ? 0 : 1;
        (side > 0 ? left : right).push_back(b.pos);
        if (b.name.find("bust00") != std::string::npos) { root[i] = b.pos; haveRoot[i] = true; }
    }
    auto fail = [&](const char* s) { if (why) *why = s; return false; };
    if (left.size() < 2 || right.size() < 2) return fail("no paired breast bones in this rig");
    auto centre = [](const std::vector<Vec3>& v) {
        Vec3 c{0, 0, 0};
        for (const Vec3& p : v) c = c + p;
        return c * (1.0f / static_cast<float>(v.size()));
    };
    out.left = centre(left);
    out.right = centre(right);
    out.chest = haveRoot[0] && haveRoot[1] ? (root[0] + root[1]) * 0.5f : (out.left + out.right) * 0.5f;
    float spread = 0.0f;
    for (const Vec3& p : left) spread = std::max(spread, length(sub(p, out.left)));
    for (const Vec3& p : right) spread = std::max(spread, length(sub(p, out.right)));
    out.radius = std::min(0.20f, std::max(0.05f, spread * 1.25f));
    out.yMin = std::numeric_limits<float>::max();
    out.yMax = -std::numeric_limits<float>::max();
    for (const Vec3& p : left) { out.yMin = std::min(out.yMin, p.y); out.yMax = std::max(out.yMax, p.y); }
    for (const Vec3& p : right) { out.yMin = std::min(out.yMin, p.y); out.yMax = std::max(out.yMax, p.y); }
    out.yMin -= 0.02f;
    out.yMax += 0.02f;
    out.waistY = -std::numeric_limits<float>::max();
    for (const auto& b : scene.bones)
        if ((b.name == "cf_s_waist01" || b.name == "cf_s_waist02") && finite(b.pos)) out.waistY = std::max(out.waistY, b.pos.y);
    if (!std::isfinite(out.waistY)) out.waistY = out.yMin - 0.20f;
    if (out.waistY > out.yMin - 0.02f) out.waistY = out.yMin - 0.20f;
    out.valid = true;
    return true;
}

GarmentSupportResult ClassifyGarmentSupport(const Scene& scene, bool disregardSlotMetadata) {
    GarmentSupportResult result;
    BreastRegion region;
    std::string why;
    if (!BuildBreastRegion(scene, region, &why)) {
        result.diagnostics.push_back("bra-aware physics: no reliable breast region (" + why + "); existing physics kept");
        return result;
    }
    const BodyGrid body = [&] { BodyGrid g; g.build(scene, region); return g; }();
    if (body.points.empty()) {
        result.diagnostics.push_back("bra-aware physics: no body surface to compare against; existing physics kept");
        return result;
    }

    for (const auto& part : scene.parts) {
        if (part.underPiece || !part.includeInExport || part.meshes.empty()) continue;
        if (part.group != "Outfit" && part.group != "Accessories") continue;
        Samples samples;
        for (int mi : part.meshes) samples.add(scene, mi);
        if (samples.refs.empty()) continue;

        size_t inL = 0, inR = 0, total = 0;
        float loY = std::numeric_limits<float>::max(), hiY = -std::numeric_limits<float>::max();
        for (size_t i = 0; i < samples.refs.size(); ++i) {
            const SceneVertex& v = samples.at(scene, i);
            if (!finite(v.pos)) continue;
            ++total;
            loY = std::min(loY, v.pos.y); hiY = std::max(hiY, v.pos.y);
            const bool l = InRegion(region, v.pos, 1), r = InRegion(region, v.pos, -1);
            if (l) ++inL;
            if (r) ++inR;
        }
        // A garment that does not touch the breast region is not a candidate at all (skirt, shoes, head accessory).
        const size_t inside = inL + inR;
        if (inside < 12 || total == 0) continue;

        GarmentEvidence e;
        e.part = part.id;
        const float insideShare = static_cast<float>(inside) / static_cast<float>(total);
        const float balance = static_cast<float>(std::min(inL, inR)) / static_cast<float>(std::max(inL, inR));
        const float bandSpan = std::max(0.05f, region.yMax - region.yMin);
        const float garmentSpan = hiY - loY;
        // A real bra includes shoulder straps (up to the shoulders) and a band under the bust, so its *total*
        // height is several times the breast-bone band. The distinguishing feature is how far it reaches BELOW
        // the band: a shirt/dress continues down the torso, a bra stops just under the bust.
        const float lowerReach = std::max(0.0f, region.yMin - loY);

        // Geometry first: distance from the garment to the body's own surface where it overlaps the bust.
        std::vector<float> distances;
        size_t bustWeighted = 0;
        for (size_t i = 0; i < samples.refs.size(); ++i) {
            const SceneVertex& v = samples.at(scene, i);
            if (!InRegion(region, v.pos, 1) && !InRegion(region, v.pos, -1)) continue;
            distances.push_back(body.nearest(v.pos, 0.04f));
            bool onBust = false;
            for (int k = 0; k < 4; ++k)
                if (v.weight[k] > 0.05f && v.bone[k] >= 0 && static_cast<size_t>(v.bone[k]) < scene.bones.size() &&
                    BreastBoneName(scene.bones[static_cast<size_t>(v.bone[k])].name)) { onBust = true; break; }
            if (onBust) ++bustWeighted;
        }
        const float medianDistance = Median(distances);
        const float skinShare = distances.empty() ? 0.0f : static_cast<float>(bustWeighted) / static_cast<float>(distances.size());
        const std::vector<CoverageProvider> providers = ProvidersOf(scene, part, region);
        const float covL = PartOpaque(scene, part) ? SideCoverage(scene, region, providers, 1) : 0.0f;
        const float covR = PartOpaque(scene, part) ? SideCoverage(scene, region, providers, -1) : 0.0f;
        const float coverage = std::min(covL, covR);

        note(e, "bust overlap " + std::to_string(inside) + "/" + std::to_string(total) + " samples, sides " +
                 std::to_string(inL) + "/" + std::to_string(inR) + ", median surface distance " +
                 std::to_string(medianDistance) + " m, body coverage " + std::to_string(covL) + "/" + std::to_string(covR) +
                 ", vertical span " + std::to_string(garmentSpan) + " m (bust band " + std::to_string(bandSpan) +
                 " m), reaches " + std::to_string(lowerReach) + " m below the band, waist " + std::to_string(region.waistY));

        // Vetoes: a garment has to cover both breasts, cover a substantial part of them, fit closely and stay a
        // breast-length garment. These are independent of any name, slot, item id or mod identifier.
        if (!PartOpaque(scene, part)) {
            e.verdict = GarmentSupportVerdict::NonSupportive;
            note(e, "translucent / cut-out garment: nothing to support with");
        } else if (insideShare < 0.12f || inL < 12 || inR < 12 || balance < 0.40f) {
            e.verdict = GarmentSupportVerdict::NonSupportive;
            note(e, "does not cover both breasts");
        } else if (coverage < 0.40f) {
            e.verdict = GarmentSupportVerdict::NonSupportive;
            note(e, "covers less than 40% of each breast (pasties / partial cover, not support)");
        } else if (loY < region.waistY - 0.03f || lowerReach > 0.18f) {
            e.verdict = GarmentSupportVerdict::NonSupportive;
            note(e, "torso-length garment (shirt / dress), not a breast band");
        } else if (medianDistance > 0.030f) {
            e.verdict = GarmentSupportVerdict::NonSupportive;
            note(e, "too loose against the bust to provide support");
        }

        if (e.verdict == GarmentSupportVerdict::Unknown) {
            const bool slot = !disregardSlotMetadata && BraSlotMetadata(part);
            if (slot) note(e, "card metadata: the bra slot");
            // Weighted evidence. No single line can decide; the geometry quorum above is mandatory.
            const float wSlot = slot ? 0.20f : 0.0f;
            const float wShare = 0.12f * clamp01(insideShare / 0.60f);
            const float wBalance = 0.08f * clamp01((balance - 0.40f) / 0.45f);
            const float wFit = 0.18f * clamp01((0.030f - medianDistance) / 0.020f);
            const float wCompact = 0.12f * clamp01((0.18f - lowerReach) / 0.12f);
            const float wCover = 0.25f * clamp01((coverage - 0.40f) / 0.30f);
            const float wSkin = 0.05f * clamp01(skinShare / 0.30f);
            e.score = wSlot + wShare + wBalance + wFit + wCompact + wCover + wSkin;
            e.confidence = e.score;
            // A confident verdict needs the full geometric quorum, not just a high metadata score.
            const bool quorum = coverage >= 0.45f && medianDistance <= 0.022f && lowerReach <= 0.14f &&
                                insideShare >= 0.20f && balance >= 0.60f;
            e.verdict = (e.score >= 0.72f && quorum) ? GarmentSupportVerdict::Supportive : GarmentSupportVerdict::Unknown;
            note(e, std::string("evidence score ") + std::to_string(e.score) + " -> " + GarmentSupportVerdictName(e.verdict));
            if (e.verdict == GarmentSupportVerdict::Unknown) note(e, "insufficient confidence; existing physics kept");
        }
        result.candidates.push_back(std::move(e));
    }

    for (const auto& e : result.candidates) {
        result.diagnostics.push_back("bra-aware physics: " + e.part + ": " + GarmentSupportVerdictName(e.verdict) +
                                     " (confidence " + std::to_string(e.confidence) + ")");
        if (std::getenv("CARDBRIDGE_DEBUG_BRA")) {
            std::fprintf(stderr, "bra-aware: %s -> %s, score %.3f\n", e.part.c_str(), GarmentSupportVerdictName(e.verdict), e.score);
            for (const auto& n : e.notes) std::fprintf(stderr, "    %s\n", n.c_str());
        }
    }
    for (const auto& e : result.candidates)
        if (e.verdict == GarmentSupportVerdict::Supportive) result.supportiveBra = true;
    if (!result.supportiveBra)
        result.diagnostics.push_back("bra-aware physics: no supportive bra detected with confidence; existing physics kept");
    return result;
}

} // namespace cardbridge::assemble
