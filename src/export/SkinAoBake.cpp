#include "export/SkinAoBake.h"

#include "core/Bytes.h"
#include "core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace cardbridge::exporter {

namespace {

inline Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalizeSafe(Vec3 a) { const float l2 = dot(a, a); return l2 > 1e-12f ? mul(a, 1.0f / std::sqrt(l2)) : Vec3{0.0f, 0.0f, 1.0f}; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Unity DXT5nm-style decode: X in the alpha channel, Y in green, Z reconstructed.
inline Vec3 DecodeNormal(const uint8_t* p) {
    const float nx = p[3] * (2.0f / 255.0f) - 1.0f;
    const float ny = p[1] * (2.0f / 255.0f) - 1.0f;
    const float nz2 = std::max(0.0f, 1.0f - nx * nx - ny * ny);
    return Vec3{nx, ny, std::sqrt(nz2)};
}

// Nearest-sample of a top-row-first RGBA buffer at a (u, v) in the same space as the body UVs (v up).
inline const uint8_t* SampleTexel(const std::vector<uint8_t>& rgba, int tw, int th, float u, float v) {
    int x = static_cast<int>(u * static_cast<float>(tw));
    x = std::max(0, std::min(tw - 1, x));
    int y = static_cast<int>((1.0f - v) * static_cast<float>(th));
    y = std::max(0, std::min(th - 1, y));
    return rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(tw) + static_cast<size_t>(x)) * 4;
}

// Rasterizes the body's (u, v) into the texture grid, keeping each texel's tangent-space normal (the base body normal
// map, plus the card's detailId detail normal when it has one - the same blend Koikatsu's shader applies through
// _DetailNormalMapScale) and, separately, the line-mask factor for each texel.
void RasterizeAo(const assemble::SceneMesh& mesh, const assemble::SceneSkinAoBake& ao, int W, int H,
                 std::vector<Vec3>& localNormals, std::vector<float>& aoLine, std::vector<uint8_t>& covered) {
    if (ao.baseNormal.empty() || ao.baseNormalWidth <= 0 || ao.baseNormalHeight <= 0) return;
    const bool hasDetail = !ao.detailNormal.empty() && ao.detailNormalWidth > 0 && ao.detailNormalHeight > 0;
    const bool hasLine = !ao.lineMask.empty() && ao.lineMaskWidth > 0 && ao.lineMaskHeight > 0;
    auto texel = [&](float u, float v) {
        return std::pair<int, int>{std::min(W - 1, std::max(0, static_cast<int>(u * static_cast<float>(W)))),
                                   std::min(H - 1, std::max(0, static_cast<int>((1.0f - v) * static_cast<float>(H))))};
    };
    for (const auto& sub : mesh.submeshes) {
        for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
            const uint32_t idx[3] = {sub.indices[t], sub.indices[t + 1], sub.indices[t + 2]};
            if (idx[0] >= mesh.vertices.size() || idx[1] >= mesh.vertices.size() || idx[2] >= mesh.vertices.size()) continue;
            const auto v0 = mesh.vertices[idx[0]], v1 = mesh.vertices[idx[1]], v2 = mesh.vertices[idx[2]];
            const auto [ax, ay] = texel(v0.uv.x, v0.uv.y);
            const auto [bx, by] = texel(v1.uv.x, v1.uv.y);
            const auto [cx, cy] = texel(v2.uv.x, v2.uv.y);
            // Texel-space coordinates with the same orientation as the skin bake (v up, row 0 at the top)
            const float fx[3] = {v0.uv.x * static_cast<float>(W), v1.uv.x * static_cast<float>(W), v2.uv.x * static_cast<float>(W)};
            const float fy[3] = {(1.0f - v0.uv.y) * static_cast<float>(H), (1.0f - v1.uv.y) * static_cast<float>(H), (1.0f - v2.uv.y) * static_cast<float>(H)};
            const float den = (fy[1] - fy[2]) * (fx[0] - fx[2]) + (fx[2] - fx[1]) * (fy[0] - fy[2]);
            if (std::fabs(den) < 1e-7f) continue;
            const int x0 = std::max(0, std::min({ax, bx, cx})), x1 = std::min(W - 1, std::max({ax, bx, cx}));
            const int y0 = std::max(0, std::min({ay, by, cy})), y1 = std::min(H - 1, std::max({ay, by, cy}));
            for (int y = y0; y <= y1; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                for (int x = x0; x <= x1; ++x) {
                    const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
                    if (covered[at]) continue;   // overlapping islands: the first triangle wins, as the skin bake does
                    const float px = static_cast<float>(x) + 0.5f;
                    const float l0 = ((fy[1] - fy[2]) * (px - fx[2]) + (fx[2] - fx[1]) * (py - fy[2])) / den;
                    const float l1 = ((fy[2] - fy[0]) * (px - fx[2]) + (fx[0] - fx[2]) * (py - fy[2])) / den;
                    const float l2 = 1.0f - l0 - l1;
                    if (l0 < -0.002f || l1 < -0.002f || l2 < -0.002f) continue;
                    const float u = l0 * v0.uv.x + l1 * v1.uv.x + l2 * v2.uv.x;
                    const float v = l0 * v0.uv.y + l1 * v1.uv.y + l2 * v2.uv.y;
                    Vec3 nLocal = DecodeNormal(SampleTexel(ao.baseNormal, ao.baseNormalWidth, ao.baseNormalHeight, u, v));
                    if (hasDetail) {
                        const Vec3 dn = DecodeNormal(SampleTexel(ao.detailNormal, ao.detailNormalWidth, ao.detailNormalHeight, u, v));
                        nLocal = normalizeSafe({nLocal.x + dn.x, nLocal.y + dn.y, nLocal.z + dn.z});
                    }
                    float line = 0.0f;
                    if (hasLine) {
                        const uint8_t* lp = SampleTexel(ao.lineMask, ao.lineMaskWidth, ao.lineMaskHeight, u, v);
                        const float g = lp[1] * (1.0f / 255.0f);
                        // Only the pure ink-line channel (G) is baked - the R/B passes are faint rim/carve data for the
                        // game's lighting, not ink, and multiply all wrong into a diffuse texture.
                        line = clampf(g, 0.0f, 1.0f);
                    }
                    covered[at] = 1;
                    localNormals[at] = nLocal;
                    aoLine[at] = line;
                }
            }
        }
    }
}

void FeatherAoMap(std::vector<float>& map, std::vector<uint8_t>& covered, int W, int H, int passes) {
    for (int p = 0; p < passes; ++p) {
        std::vector<float> nextMap = map;
        std::vector<uint8_t> nextCovered = covered;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
                if (covered[at]) continue;
                float sum = 0.0f;
                int n = 0;
                bool touching = false;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(W) + static_cast<size_t>(nx);
                        sum += map[ni];   // uncovered texels hold 1.0, so the ramp softens toward lit
                        ++n;
                        if (covered[ni]) touching = true;
                    }
                }
                if (!touching || n == 0) continue;
                nextMap[at] = sum / static_cast<float>(n);
                nextCovered[at] = 1;
            }
        }
        map.swap(nextMap);
        covered.swap(nextCovered);
    }
}

void BlurAoMap(std::vector<float>& map, const std::vector<uint8_t>& covered, int W, int H) {
    std::vector<float> next = map;
    static const int dx[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
    static const int dy[8] = {0, 0, -1, 1, -1, 1, -1, 1};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!covered[at]) continue;
            float sum = map[at];
            int n = 1;
            for (int k = 0; k < 8; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(W) + static_cast<size_t>(nx);
                if (!covered[ni]) continue;
                sum += map[ni];
                ++n;
            }
            next[at] = sum / static_cast<float>(n);
        }
    }
    map.swap(next);
}

// The AO factor for one covered texel: how much its normal curves away from its neighbours' average.
// Orientation-independent (no view or light direction in it), so it cannot favour any side of the body the way a
// fixed-light bake would; it marks creases, ridges and contact folds and leaves flat skin alone.
inline float CavityFactor(const Vec3& n, const Vec3& avg) {
    const float dev = std::sqrt((n.x - avg.x) * (n.x - avg.x) + (n.y - avg.y) * (n.y - avg.y) + (n.z - avg.z) * (n.z - avg.z));
    const float occ = clampf((dev - 0.045f) / 0.25f, 0.0f, 1.0f);   // mark the strong creases/joints (~4% of texels)
    return 1.0f - 0.55f * std::pow(occ, 1.6f);                      // never darker than 0.45 on its own, before attenuation
}

}  // namespace

bool ApplySkinAoBake(assemble::Scene& scene, float strength, bool rebuild) {
    auto& ss = scene.skinShade;
    auto& ao = scene.aoBake;
    if (ss.meshes.empty()) return false;
    if (static_cast<size_t>(ss.texture) >= scene.textures.size()) return false;
    auto& tex = scene.textures[static_cast<size_t>(ss.texture)];
    if (tex.width <= 0 || tex.height <= 0 || tex.rgba.size() < static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) * 4) return false;
    const int W = tex.width, H = tex.height;
    if (ao.baseNormal.empty()) return false;   // nothing to bake from: keep whatever ApplySkinShade built
    if (ss.base.size() != tex.rgba.size()) ss.base = tex.rgba;   // the create result, whether or not ApplySkinShade ran

    // `rebuild` tracks the body shape (the assembler always passes it; the viewer passes it when the body moved), so a
    // strength drag stays cheap - the maps are reused and only the multiply re-runs.
    if (rebuild || ao.aoShade.size() != static_cast<size_t>(W) * static_cast<size_t>(H) || !ao.built) {
        const size_t WH = static_cast<size_t>(W) * static_cast<size_t>(H);
        ao.aoShade.assign(WH, 1.0f);
        ao.aoLine.assign(WH, 0.0f);
        ao.covered.assign(WH, 0);
        std::vector<Vec3> localNormals(WH, Vec3{0.0f, 0.0f, 1.0f});
        for (int mi : ss.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            if (mesh.vertices.empty()) continue;
            RasterizeAo(mesh, ao, W, H, localNormals, ao.aoLine, ao.covered);
        }
        // The cavity factor, from a radius-8 average of neighbouring normals (covered neighbours only).
        const int R = 8;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
                if (!ao.covered[at]) continue;
                float sx = 0, sy = 0, sz = 0;
                int n = 0;
                for (int dy = -R; dy <= R; dy += 2) {
                    for (int dx = -R; dx <= R; dx += 2) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(W) + static_cast<size_t>(nx);
                        if (!ao.covered[ni]) continue;
                        const Vec3& p = localNormals[ni];
                        sx += p.x; sy += p.y; sz += p.z; ++n;
                    }
                }
                const Vec3& c = localNormals[at];
                if (n >= 9) {
                    const Vec3 avg{sx / n, sy / n, sz / n};
                    ao.aoShade[at] = CavityFactor(c, avg);
                }
            }
        }
        FeatherAoMap(ao.aoShade, ao.covered, W, H, 4);
        FeatherAoMap(ao.aoLine, ao.covered, W, H, 4);
        BlurAoMap(ao.aoShade, ao.covered, W, H);
        BlurAoMap(ao.aoLine, ao.covered, W, H);
        BlurAoMap(ao.aoShade, ao.covered, W, H);
        BlurAoMap(ao.aoLine, ao.covered, W, H);
        ao.built = true;
        if (std::getenv("CARDBRIDGE_DEBUG_AO")) {
            size_t n = 0;
            double sumShade = 0, sumLine = 0, lo = 1e9, hi = -1e9;
            for (size_t i = 0; i < ao.covered.size(); ++i) {
                if (!ao.covered[i]) continue;
                ++n;
                sumShade += ao.aoShade[i];
                sumLine += ao.aoLine[i];
                lo = std::min<double>(lo, ao.aoShade[i]);
                hi = std::max<double>(hi, ao.aoShade[i]);
            }
            std::fprintf(stderr, "ao debug: covered texels %zu / %zu, mean shade %.3f (%.3f..%.3f), mean line %.4f\n",
                         n, ao.covered.size(), n ? sumShade / n : 0.0, n ? lo : 0.0, n ? hi : 0.0, n ? sumLine / n : 0.0);
        }
    }

    const float s = clampf(strength, 0.0f, 2.0f);
    if (ao.applied == s) {   // nothing changed: the buffer already holds this pass
        tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
        return true;
    }
    // The SkinShade factor at the strength currently baked into ss.base (the identical formula ApplySkinShade uses).
    const float sc = ss.applied >= 0.0f ? clampf(ss.applied, 0.0f, 2.0f) : 0.0f;
    const float su = ss.applied >= 0.0f ? clampf(ss.appliedUnder, 0.0f, 2.0f) : 0.0f;
    const bool shadeLo = !ss.shade.empty() && !ss.shadeU.empty() && !ss.covered.empty() && ss.shade.size() == ao.covered.size();
    for (size_t i = 0; i < ao.covered.size(); ++i) {
        float splitMul = 1.0f;
        if (shadeLo && ss.covered[i]) {
            const float litC = static_cast<float>(ss.shade[i]) / 255.0f;
            const float litU = static_cast<float>(ss.shadeU[i]) / 255.0f;
            const float mulC = clampf(1.0f - sc * (1.0f - litC), 0.04f, 1.0f);
            const float mulU = clampf(1.0f - su * (1.0f - litU), 0.04f, 1.0f);
            splitMul = mulU < mulC ? mulU : mulC;
        }
        float mul = splitMul;
        if (ao.covered[i]) {
            // Attenuate where SkinShade already darkened this texel: the new bake yields to it, never stacks on top.
            const float existingDark = 1.0f - splitMul;
            const float att = clampf(1.0f - 0.7f * existingDark, 0.0f, 1.0f);
            const float aoMul = std::max(0.30f, 1.0f - s * (1.0f - ao.aoShade[i]) * att);
            const float lineMul = std::max(0.45f, 1.0f - s * ao.aoLine[i] * att);
            mul = splitMul * aoMul * lineMul;
        }
        for (int c = 0; c < 3; ++c) {
            const size_t at = i * 4 + static_cast<size_t>(c);
            tex.rgba[at] = static_cast<uint8_t>(clampf(static_cast<float>(ss.base[at]) * mul, 0.0f, 255.0f) + 0.5f);
        }
        tex.rgba[i * 4 + 3] = ss.base[i * 4 + 3];
    }
    ao.applied = s;
    tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
    return true;
}

std::vector<uint8_t> BuildSkinAoFactor(const assemble::Scene& scene) {
    auto& ss = scene.skinShade;
    auto& ao = scene.aoBake;
    if (!ao.built || ss.texture < 0 || static_cast<size_t>(ss.texture) >= scene.textures.size()) return {};
    const auto& tex = scene.textures[static_cast<size_t>(ss.texture)];
    if (tex.width <= 0 || tex.height <= 0) return {};
    const size_t WH = static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height);
    std::vector<uint8_t> out(WH, 255);
    const float s = clampf(ao.applied < 0.0f ? 0.0f : ao.applied, 0.0f, 2.0f);
    const float sc = ss.applied >= 0.0f ? clampf(ss.applied, 0.0f, 2.0f) : 0.0f;
    const float su = ss.applied >= 0.0f ? clampf(ss.appliedUnder, 0.0f, 2.0f) : 0.0f;
    const bool shadeLo = !ss.shade.empty() && !ss.shadeU.empty() && !ss.covered.empty() && ss.shade.size() == ao.covered.size();
    for (size_t i = 0; i < ao.covered.size(); ++i) {
        if (!ao.covered[i]) continue;
        float splitMul = 1.0f;
        if (shadeLo && ss.covered[i]) {
            const float litC = static_cast<float>(ss.shade[i]) / 255.0f;
            const float litU = static_cast<float>(ss.shadeU[i]) / 255.0f;
            const float mulC = clampf(1.0f - sc * (1.0f - litC), 0.04f, 1.0f);
            const float mulU = clampf(1.0f - su * (1.0f - litU), 0.04f, 1.0f);
            splitMul = mulU < mulC ? mulU : mulC;
        }
        const float existingDark = 1.0f - splitMul;
        const float att = clampf(1.0f - 0.7f * existingDark, 0.0f, 1.0f);
        const float aoMul = std::max(0.30f, 1.0f - s * (1.0f - ao.aoShade[i]) * att);
        const float lineMul = std::max(0.45f, 1.0f - s * ao.aoLine[i] * att);
        out[i] = static_cast<uint8_t>(clampf(aoMul * lineMul, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return out;
}

}  // namespace cardbridge::exporter
