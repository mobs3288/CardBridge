#include "export/Translucency.h"

#include "assemble/Pose.h"
#include "core/Bytes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace cardbridge::exporter {

namespace {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }

// Möller–Trumbore; `t` is the ray distance, `u`/`v` the barycentric weights of b and c.
inline bool RayTri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float& t, float& bu, float& bv) {
    const Vec3 e1 = vsub(b, a), e2 = vsub(c, a);
    const Vec3 p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vec3 tv = vsub(o, a);
    const float u = dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(tv, e1);
    const float v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float dist = dot(e2, q) * inv;
    if (!(dist > 1e-4f) || !std::isfinite(dist)) return false;
    t = dist; bu = u; bv = v;
    return true;
}

// Rasterizes the triangles of every submesh of `meshes` whose material binds `texture` into the layer's coverage map
// (UV space, like the sample calls read it). Front and back shells of the same garment share one atlas, so the gate
// is per-texture across the whole part, not per one mesh. Wrap modes are honoured: a chart that crosses 0/1 is
// rasterized once per shifted copy that overlaps the unit square.
void BuildCovered(const assemble::Scene& scene, int texture, const std::vector<int>& meshes, bool clampTex, assemble::SceneTranslucency::Layer& layer,
                  int W, int H) {
    layer.covered.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    layer.coveredBuilt = true;
    for (int mi : meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
        for (const auto& sub : mesh.submeshes) {
            if (sub.material < 0 || static_cast<size_t>(sub.material) >= scene.materials.size()) continue;
            if (scene.materials[static_cast<size_t>(sub.material)].texture != texture) continue;
            const auto& idx = sub.indices;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const auto& a = mesh.vertices[idx[t]], &b = mesh.vertices[idx[t + 1]], &c = mesh.vertices[idx[t + 2]];
            const float us[3] = {a.uv.x, b.uv.x, c.uv.x};
            const float vs[3] = {a.uv.y, b.uv.y, c.uv.y};
            for (float ox : {0.0f, 1.0f, -1.0f})
                for (float oy : {0.0f, 1.0f, -1.0f}) {
                    const float u0 = us[0] + ox, v0 = vs[0] + oy, u1 = us[1] + ox, v1 = vs[1] + oy, u2 = us[2] + ox, v2 = vs[2] + oy;
                    const float umin = std::min({u0, u1, u2}), umax = std::max({u0, u1, u2});
                    const float vmin = std::min({v0, v1, v2}), vmax = std::max({v0, v1, v2});
                    // Only the shifts overlapping the unit square do work.
                    if (!clampTex && (umax <= 0.0f || umin >= 1.0f || vmax <= 0.0f || vmin >= 1.0f)) continue;
                    if (clampTex && (ox != 0.0f || oy != 0.0f)) continue;   // clamp maps to the edge texels; one pass is enough
                    const float fx0 = std::max(0.0f, umin * static_cast<float>(W)), fx1 = std::min(static_cast<float>(W), umax * static_cast<float>(W));
                    // Memory row is (1 - v) * H.
                    const float fy0 = std::max(0.0f, (1.0f - vmax) * static_cast<float>(H)), fy1 = std::min(static_cast<float>(H), (1.0f - vmin) * static_cast<float>(H));
                    const int ix0 = std::max(0, static_cast<int>(std::floor(fx0)) - 1), ix1 = std::min(W - 1, static_cast<int>(std::ceil(fx1)) + 1);
                    const int iy0 = std::max(0, static_cast<int>(std::floor(fy0)) - 1), iy1 = std::min(H - 1, static_cast<int>(std::ceil(fy1)) + 1);
                    if (ix0 > ix1 || iy0 > iy1) continue;
                    const float e1u = u1 - u0, e1v = v1 - v0, e2u = u2 - u0, e2v = v2 - v0;
                    const float det = e1u * e2v - e2u * e1v;
                    if (std::fabs(det) < 1e-12f) continue;
                    const float inv = 1.0f / det;
                    for (int y = iy0; y <= iy1; ++y) {
                        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(H);
                        for (int x = ix0; x <= ix1; ++x) {
                            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(W);
                            const float pw = u - u0, pv = v - v0;
                            const float b1 = (pw * e2v - pv * e2u) * inv, b2 = (pv * e1u - pw * e1v) * inv, b0 = 1.0f - b1 - b2;
                            const float tol = 1.0f / static_cast<float>(std::max(W, H));
                            if (b0 < -tol || b1 < -tol || b2 < -tol) continue;
                            layer.covered[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = 1;
                        }
                    }
                }
        }
    }
}
}

bool Visible(const std::vector<char>* hiddenParts, size_t pi, const assemble::ScenePart& part) {
    const char flag = hiddenParts && pi < hiddenParts->size() ? (*hiddenParts)[pi] : 0;
    if (!part.includeInExport) return false;
    if (flag == 2) return true;   // forced on (never painted: callers skip under pieces)
    return part.visibleInPreview && !flag;
}

// The index in scene.shaders of a cloth shader that blends with the texture's alpha instead of discarding
// (the game's "main_alpha" family). A non-transparent Shader Forge/main_opaque garment ridden with the brush
// keeps *this* shader while it holds alpha-painted texels; the original is restored on revert. If the card has
// no such shader at all, -1 (keep the current one; the painted area will still show in the exported PMX).
int AlphaTwinShaderIndex(const assemble::Scene& scene) {
    int fallback = -1;
    for (size_t i = 0; i < scene.shaders.size(); ++i) {
        std::string d = scene.shaders[i].name;
        for (auto& c : d) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool isClothFamily = d.rfind("shader forge/main_", 0) == 0 || d.rfind("shader forge/ib_", 0) == 0 || d.find("ib_") == 0;
        if (!isClothFamily) continue;
        if (d.find("main_alpha") != std::string::npos) return static_cast<int>(i);
        if (fallback < 0 && !scene.shaders[i].zWrite && scene.shaders[i].queue >= 2500) fallback = static_cast<int>(i);
    }
    if (fallback >= 0) return fallback;
    for (size_t i = 0; i < scene.shaders.size(); ++i)
        if (!scene.shaders[i].zWrite && scene.shaders[i].queue >= 2500) return static_cast<int>(i);
    return -1;
}

// Whether a part carries at least one texture-bound submesh (the brush needs a texture to write alpha into).
bool HasTexturedSubmesh(const assemble::Scene& scene, const assemble::SceneMesh& mesh) {
    for (const auto& sub : mesh.submeshes)
        if (sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size()) {
            const auto& mat = scene.materials[static_cast<size_t>(sub.material)];
            if (mat.texture >= 0 && static_cast<size_t>(mat.texture) < scene.textures.size()) return true;
        }
    return false;
}

}  // namespace

bool PickBrushPart(const assemble::Scene& scene, const std::vector<char>* hiddenParts, const ViewFraming& f, float W, float H,
                   const ViewCamera& cam, float px, float py, const std::vector<Mat4>* skin,
                   const std::vector<std::vector<Vec3>>* morphDelta, BrushHit& out) {
    out = BrushHit{};
    Vec3 origin, dir;
    if (!detail::ViewRayAt(f, W, H, cam, px, py, origin, dir)) return false;
    const bool posed = skin && !skin->empty() && skin->size() >= scene.bones.size();
    float bestT = std::numeric_limits<float>::max();
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& part = scene.parts[pi];
        if (!Visible(hiddenParts, pi, part)) continue;
        if (part.underPiece) continue;                       // skin kept under cloth: the bake keeps it visible, never paints it
        if (part.group == "Body" || part.group == "Head") continue;   // skin
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            if (!HasTexturedSubmesh(scene, mesh)) continue;
            const size_t n = mesh.vertices.size();
            const std::vector<Vec3>* md = (morphDelta && static_cast<size_t>(mi) < morphDelta->size() && (*morphDelta)[static_cast<size_t>(mi)].size() == n) ? &(*morphDelta)[static_cast<size_t>(mi)] : nullptr;
            std::vector<Vec3> P(n);
            for (size_t i = 0; i < n; ++i) {
                assemble::SceneVertex v = mesh.vertices[i];
                if (md) v.pos = {v.pos.x + (*md)[i].x, v.pos.y + (*md)[i].y, v.pos.z + (*md)[i].z};
                P[i] = posed ? assemble::SkinPoint(v, *skin) : v.pos;
            }
            for (size_t si = 0; si < mesh.submeshes.size(); ++si) {
                const auto& sub = mesh.submeshes[si];
                if (sub.material < 0 || static_cast<size_t>(sub.material) >= scene.materials.size()) continue;
                const auto& mat = scene.materials[static_cast<size_t>(sub.material)];
                if (mat.texture < 0 || static_cast<size_t>(mat.texture) >= scene.textures.size()) continue;
                const auto& idx = sub.indices;
                for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                    const uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
                    if (i0 >= n || i1 >= n || i2 >= n) continue;
                    float tt, bu, bv;
                    if (!RayTri(origin, dir, P[i0], P[i1], P[i2], tt, bu, bv)) continue;
                    if (tt >= bestT) continue;
                    // UV/world area ratio at the hit: the brush's scale.
                    const Vec3 e1 = vsub(P[i1], P[i0]), e2 = vsub(P[i2], P[i0]);
                    const float wArea = length(cross(e1, e2)) * 0.5f;
                    const Vec2 du = {mesh.vertices[i1].uv.x - mesh.vertices[i0].uv.x, mesh.vertices[i1].uv.y - mesh.vertices[i0].uv.y};
                    const Vec2 dv = {mesh.vertices[i2].uv.x - mesh.vertices[i0].uv.x, mesh.vertices[i2].uv.y - mesh.vertices[i0].uv.y};
                    const float uvArea = std::fabs(du.x * dv.y - du.y * dv.x) * 0.5f;
                    const float uvPerMetre = (wArea > 1e-12f && uvArea > 1e-12f) ? std::sqrt(uvArea / wArea) : 1.0f;
                    bestT = tt;
                    out.part = static_cast<int>(pi);
                    out.mesh = mi;
                    out.submesh = static_cast<int>(si);
                    const Vec2 u0 = mesh.vertices[i0].uv, u1 = mesh.vertices[i1].uv, u2 = mesh.vertices[i2].uv;
                    const float w0 = 1.0f - bu - bv;
                    out.uv = {u0.x * w0 + u1.x * bu + u2.x * bv, u0.y * w0 + u1.y * bu + u2.y * bv};
                    out.pos = {origin.x + dir.x * tt, origin.y + dir.y * tt, origin.z + dir.z * tt};
                    out.uvPerMetre = uvPerMetre;
                }
            }
        }
    }
    return out.ok();
}

int TranslucencyLayer(assemble::Scene& scene, int part, int material, int mesh) {
    if (part < 0 || static_cast<size_t>(part) >= scene.parts.size()) return -1;
    if (mesh < 0 || static_cast<size_t>(mesh) >= scene.meshes.size()) return -1;
    if (material < 0 || static_cast<size_t>(material) >= scene.materials.size()) return -1;
    const auto& mat = scene.materials[static_cast<size_t>(material)];
    const int ti = mat.texture;
    if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size()) return -1;
    const auto& tex = scene.textures[static_cast<size_t>(ti)];
    if (tex.width <= 0 || tex.height <= 0 || tex.rgba.size() != static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) * 4) return -1;
    // Reuse the layer for this (part, texture) when there is one — the mask stays put across strokes.
    auto& layers = scene.translucency.layers;
    for (size_t i = 0; i < layers.size(); ++i)
        if (layers[i].part == part && layers[i].texture == ti) return static_cast<int>(i);
    assemble::SceneTranslucency::Layer layer;
    layer.part = part;
    layer.mesh = mesh;
    layer.material = material;
    layer.texture = ti;
    layer.base = tex.rgba;
    layer.mask.assign(static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height), 0);
    layer.afterHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
    layers.push_back(std::move(layer));
    return static_cast<int>(layers.size() - 1);
}

bool TranslucencyLayerPainted(const assemble::SceneTranslucency::Layer& layer) {
    for (uint8_t b : layer.mask)
        if (b) return true;
    return false;
}

bool TranslucencyActive(const assemble::Scene& scene, int part) {
    for (const auto& layer : scene.translucency.layers)
        if (layer.part == part && layer.passHeld) return true;
    return false;
}

int TranslucencyStamp(assemble::Scene& scene, int layerIndex, float u, float v, float uvRadius, bool erase, float amount) {
    if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= scene.translucency.layers.size()) return 0;
    if (!(uvRadius > 0.0f)) return 0;
    auto& layer = scene.translucency.layers[static_cast<size_t>(layerIndex)];
    const int ti = layer.texture;
    if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size()) return 0;
    const auto& tex = scene.textures[static_cast<size_t>(ti)];
    const int W = tex.width, H = tex.height;
    if (W <= 0 || H <= 0 || layer.mask.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) return 0;
    if (!layer.coveredBuilt || layer.covered.empty()) {
        // The gate covers the whole part's islands for this texture (front and back shells of the same garment share one atlas).
        std::vector<int> coverMeshes;
        const auto& part = scene.parts[static_cast<size_t>(layer.part)];
        for (int mi : part.meshes)
            for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes)
                if (sub.material >= 0 && static_cast<size_t>(sub.material) < scene.materials.size() && scene.materials[static_cast<size_t>(sub.material)].texture == ti) { coverMeshes.push_back(mi); break; }
        BuildCovered(scene, ti, coverMeshes, tex.clamp, layer, W, H);
    }
    // The stamp area in texels (memory rows are flipped for v); wrapped coordinates measure the minimal distance.
    const float rMax = std::min(uvRadius, 0.75f);
    const float uC = tex.clamp ? u : u - std::floor(u);
    const float vC = tex.clamp ? v : v - std::floor(v);
    const int x0 = static_cast<int>(std::floor((uC - rMax) * static_cast<float>(W)));
    const int x1 = static_cast<int>(std::ceil((uC + rMax) * static_cast<float>(W)));
    const int y0 = static_cast<int>(std::floor((1.0f - (vC + rMax)) * static_cast<float>(H)));
    const int y1 = static_cast<int>(std::ceil((1.0f - (vC - rMax)) * static_cast<float>(H)));
    const float invH = 1.0f / static_cast<float>(H);
    const float a = clampf(amount, 0.0f, 255.0f);
    int changed = 0;
    for (int y = y0; y <= y1; ++y) {
        const float vT = 1.0f - (static_cast<float>(y) + 0.5f) * invH;
        for (int x = x0; x <= x1; ++x) {
            const size_t texel = static_cast<size_t>(((y % H) + H) % H) * static_cast<size_t>(W) + static_cast<size_t>(((x % W) + W) % W);
            if (!layer.covered[texel]) continue;
            const float uT = (static_cast<float>(((x % W) + W) % W) + 0.5f) / static_cast<float>(W);
            float du = uT - u;
            float dv = vT - v;
            if (!tex.clamp) { du -= std::floor(du + 0.5f); dv -= std::floor(dv + 0.5f); }   // the torus distance, matching the wrap-around sampling
            const float dr = std::sqrt(du * du + dv * dv);
            if (dr > rMax) continue;
            // A smooth radial falloff: full strength to the inner half, a smoothstep down to nothing at the rim -
            // the painted area fades out instead of ending on a hard circle.
            float fall;
            if (dr <= rMax * 0.45f) fall = 1.0f;
            else {
                const float t = (dr - rMax * 0.45f) / (rMax * 0.55f);
                fall = 1.0f - (t * t * (3.0f - 2.0f * t));
            }
            uint8_t& m = layer.mask[texel];
            if (erase) {
                if (m) { m = static_cast<uint8_t>(m * (1.0f - fall)); ++changed; }
            } else {
                const float add = a * fall * (1.0f - m / 255.0f * 0.5f);   // soft build-up to full strength
                const int nv = std::min(255, static_cast<int>(m) + static_cast<int>(std::lround(add)));
                if (nv != m) { m = static_cast<uint8_t>(nv); ++changed; }
            }
        }
    }
    return changed;
}

int TranslucencyApply(assemble::Scene& scene, float strength01, std::vector<int>* changedTextures) {
    strength01 = clampf(strength01, 0.0f, 1.0f);
    const float drop = strength01 * 0.7f;   // a fully painted texel keeps thirty percent of its alpha at full strength
    auto& layers = scene.translucency.layers;
    std::vector<int> changed;
    for (auto& layer : layers) {
        const int ti = layer.texture;
        if (ti >= 0 && static_cast<size_t>(ti) < scene.textures.size()) {
            const int material = layer.material;
            auto& tex = scene.textures[static_cast<size_t>(ti)];
            const int W = tex.width, H = tex.height;
            if (layer.base.size() != tex.rgba.size() || W <= 0 || H <= 0) {   // the texture was replaced by a differently-sized one: start over
                layer.base = tex.rgba;
                layer.mask.assign(static_cast<size_t>(std::max(0, W)) * static_cast<size_t>(std::max(0, H)), 0);
                layer.covered.clear();
                layer.coveredBuilt = false;
            }
            if (layer.base.size() == tex.rgba.size()) {
                const uint64_t cur = Fnv1a64(tex.rgba.data(), tex.rgba.size());
                if (cur != layer.afterHash) {   // changed outside our bake (the Material Workbench, a create job): adopt as the new restore copy
                    layer.base = tex.rgba;
                    layer.afterHash = cur;
                }
                std::vector<uint8_t> out = layer.base;
                bool painted = false;
                const size_t px = layer.mask.size();
                for (size_t i = 0; i < px; ++i) {
                    const uint8_t m = layer.mask[i];
                    if (!m) continue;
                    painted = true;
                    const float keep = 1.0f - (m / 255.0f) * drop;
                    const uint8_t base = out[i * 4 + 3];
                    // The game's cloth shaders often ignore the alpha channel, or use it as a cutout mask rather than a
                    // real transparency - on those texels the authored shape must stay exactly as it is (a belly-out
                    // lace edge must not be blended). Only the artwork the game actually draws is faded out.
                    out[i * 4 + 3] = base >= 128 ? static_cast<uint8_t>(std::lround(255.0f * keep)) : base;
                }
                const bool pin = painted && drop > 0.001f;
                tex.rgba = pin ? std::move(out) : layer.base;
                const uint64_t newHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
                if (newHash != cur) {   // the pixels really changed: the caller must re-upload / rebuild caches
                    tex.pixelHash = newHash;
                    layer.afterHash = newHash;
                    changed.push_back(ti);
                }
                if (pin && !layer.passHeld) {
                    if (material >= 0 && static_cast<size_t>(material) < scene.materials.size()) {
                        auto& mat = scene.materials[static_cast<size_t>(material)];
                        layer.hadAlpha = mat.alpha;
                        layer.hadTranslucent = mat.translucent;
                        mat.alpha = true;
                        mat.translucent = true;
                        // While the bake holds the material, the alpha test (clip at _Cutoff) is off, so the soft blend the
                        // paint creates is what draws - matching MikuMikuDance, which does not alpha-test a transparent material.
                        layer.cutoffTouched = false;
                        for (size_t k = 0; k < mat.props.size(); ++k)
                            if (mat.props[k].first == "_Cutoff") { layer.cutoffProp = static_cast<int>(k); layer.cutoffPrior = mat.props[k].second; layer.cutoffTouched = true; break; }
                        if (layer.cutoffTouched) mat.props[static_cast<size_t>(layer.cutoffProp)].second = {1e-3f, 0.0f, 0.0f, 0.0f};
                        // A Shader Forge main_opaque garment is hard-authored to *discard* below 0.5 alpha - the painted area
                        // would simply vanish. Swap the program for the alpha-cloth twin (the IBL_Shader_alpha/main_alpha the
                        // game uses for genuinely sheer garments): same template, same properties, soft blend instead of the clip.
                        // The original is restored on revert. The pinafore and friends ship in this shader, so the swap is a
                        // no-op for them.
                        layer.shaderSwapped = false;
                        {
                            std::string cur = (mat.shaderIndex >= 0 && static_cast<size_t>(mat.shaderIndex) < scene.shaders.size()) ? scene.shaders[static_cast<size_t>(mat.shaderIndex)].name : "";
                            for (auto& c : cur) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (cur.find("alpha") == std::string::npos) {
                                const int alpha = AlphaTwinShaderIndex(scene);
                                if (alpha >= 0 && alpha != mat.shaderIndex) {
                                    layer.shaderIndexHad = mat.shaderIndex;
                                    mat.shaderIndex = alpha;
                                    layer.shaderSwapped = true;
                                }
                            }
                        }
                        layer.passHeld = true;
                    }
                } else if (!pin && layer.passHeld) {
                    bool other = false;   // another layer may hold this same material in the transparent pass
                    for (const auto& l2 : layers)
                        if (&l2 != &layer && l2.passHeld && l2.material == layer.material) other = true;
                    if (!other && material >= 0 && static_cast<size_t>(material) < scene.materials.size()) {
                        auto& mat = scene.materials[static_cast<size_t>(material)];
                        mat.alpha = layer.hadAlpha;
                        mat.translucent = layer.hadTranslucent;
                        if (layer.cutoffTouched && layer.cutoffProp >= 0 && static_cast<size_t>(layer.cutoffProp) < mat.props.size())
                            mat.props[static_cast<size_t>(layer.cutoffProp)].second = layer.cutoffPrior;
                        if (layer.shaderSwapped && layer.shaderIndexHad >= 0 && static_cast<size_t>(layer.shaderIndexHad) < scene.shaders.size())
                            mat.shaderIndex = layer.shaderIndexHad;
                    }
                    layer.passHeld = false;
                    layer.cutoffTouched = false;
                    layer.shaderSwapped = false;
                }
            }
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    if (changedTextures) *changedTextures = changed;
    return static_cast<int>(changed.size());
}

int TranslucencyClearAll(assemble::Scene& scene, std::vector<int>* changedTextures) {
    int cleared = 0;
    for (auto& layer : scene.translucency.layers) {
        bool any = false;
        for (uint8_t& m : layer.mask)
            if (m) { m = 0; any = true; }
        if (any) ++cleared;
    }
    TranslucencyApply(scene, 0.0f, changedTextures);
    return cleared;
}

}  // namespace cardbridge::exporter
