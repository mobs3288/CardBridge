#include "export/SkinShade.h"

#include "core/Bytes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace cardbridge::exporter {

namespace {

inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// A triangle of the body's skin, in scene space. Occlusion rays run against all of them.
struct ShadeTri {
    Vec3 a, b, c;
};

struct BvhNode {
    Vec3 mn, mx;
    int left = -1, right = -1;   // inner: children; leaf: start / count
    int start = 0, count = 0;
};

class SkinBvh {
public:
    void build(std::vector<ShadeTri> tris) {
        tris_ = std::move(tris);
        order_.resize(tris_.size());
        for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<int>(i);
        nodes_.clear();
        if (order_.empty()) return;
        nodes_.reserve(order_.size() * 2);
        buildNode(0, static_cast<int>(order_.size()));
    }

    bool empty() const { return order_.empty(); }

    // True when a surface is close enough along the ray to count as occluding.
    bool occluded(Vec3 o, Vec3 d, float maxT) const {
        if (nodes_.empty()) return false;
        int stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& n = nodes_[static_cast<size_t>(stack[--sp])];
            if (!slab(n, o, d, maxT)) continue;
            if (n.left < 0) {
                for (int i = 0; i < n.count; ++i) {
                    const ShadeTri& t = tris_[static_cast<size_t>(order_[static_cast<size_t>(n.start + i)])];
                    if (hit(t, o, d, maxT)) return true;
                }
            } else {
                if (sp + 2 <= 64) { stack[sp++] = n.left; stack[sp++] = n.right; }
            }
        }
        return false;
    }

private:
    static bool slab(const BvhNode& n, Vec3 o, Vec3 d, float maxT) {
        float t0 = 0.0f, t1 = maxT;
        const float ob[3] = {o.x, o.y, o.z}, db[3] = {d.x, d.y, d.z};
        const float mn[3] = {n.mn.x, n.mn.y, n.mn.z}, mx[3] = {n.mx.x, n.mx.y, n.mx.z};
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(db[a]) < 1e-12f) {
                if (ob[a] < mn[a] || ob[a] > mx[a]) return false;
                continue;
            }
            const float inv = 1.0f / db[a];
            float ta = (mn[a] - ob[a]) * inv, tb = (mx[a] - ob[a]) * inv;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return true;
    }

    static bool hit(const ShadeTri& t, Vec3 o, Vec3 d, float maxT) {
        const Vec3 e1 = sub(t.b, t.a), e2 = sub(t.c, t.a);
        const Vec3 p = cross(d, e2);
        const float det = dot(e1, p);
        if (std::fabs(det) < 1e-12f) return false;
        const float inv = 1.0f / det;
        const Vec3 tv = sub(o, t.a);
        const float u = dot(tv, p) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const Vec3 q = cross(tv, e1);
        const float v = dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        const float dist = dot(e2, q) * inv;
        return dist > 1e-4f && dist < maxT;
    }

    int buildNode(int start, int count) {
        const int self = static_cast<int>(nodes_.size());
        nodes_.push_back({});
        Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < count; ++i) {
            const ShadeTri& t = tris_[static_cast<size_t>(order_[static_cast<size_t>(start + i)])];
            for (Vec3 p : {t.a, t.b, t.c}) {
                mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
                mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
            }
        }
        nodes_[static_cast<size_t>(self)].mn = mn;
        nodes_[static_cast<size_t>(self)].mx = mx;
        if (count <= 4) {
            nodes_[static_cast<size_t>(self)].start = start;
            nodes_[static_cast<size_t>(self)].count = count;
            return self;
        }
        const Vec3 extent{mx.x - mn.x, mx.y - mn.y, mx.z - mn.z};
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        auto centre = [axis](const ShadeTri& t) { return ((axis == 0 ? t.a.x + t.b.x + t.c.x : axis == 1 ? t.a.y + t.b.y + t.c.y : t.a.z + t.b.z + t.c.z) / 3.0f); };
        std::nth_element(order_.begin() + start, order_.begin() + start + count / 2, order_.begin() + start + count,
                         [&](int a, int b) { return centre(tris_[static_cast<size_t>(a)]) < centre(tris_[static_cast<size_t>(b)]); });
        const int mid = count / 2;
        const int l = buildNode(start, mid);
        const int r = buildNode(start + mid, count - mid);
        nodes_[static_cast<size_t>(self)].left = l;
        nodes_[static_cast<size_t>(self)].right = r;
        return self;
    }

    std::vector<ShadeTri> tris_;
    std::vector<int> order_;
    std::vector<BvhNode> nodes_;
};

// A deterministic hemisphere sample (the first 24 points of a Hammersley set mapped with a cosine distribution).
struct SampleDir {
    float x, y, z;
};
constexpr int kSamples = 24;
const SampleDir* SampleDirs() {
    static const std::vector<SampleDir> kDirs = [] {
        auto radicalInverse = [](uint32_t bits) {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        };
        std::vector<SampleDir> dirs;
        dirs.reserve(kSamples);
        for (int i = 1; i <= kSamples; ++i) {
            const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(kSamples);
            const float u2 = radicalInverse(static_cast<uint32_t>(i));
            const float r = std::sqrt(u1), phi = 6.28318530718f * u2;
            dirs.push_back({r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1))});
        }
        return dirs;
    }();
    return kDirs.data();
}
const SampleDir* SkySampleDirs() {
    static const std::vector<SampleDir> kDirs = [] {
        const SampleDir* src = SampleDirs();
        std::vector<SampleDir> dirs;
        dirs.reserve(kSamples);
        for (int i = 0; i < kSamples; ++i) dirs.push_back({src[i].x, src[i].z, src[i].y});   // the +Y pole: used in world space
        return dirs;
    }();
    return kDirs.data();
}

// One mesh's per-vertex shade (multiplier at strength 1: 1 = untouched).
std::vector<float> ShadeMesh(const assemble::SceneMesh& mesh, const SkinBvh& bvh) {
    const size_t n = mesh.vertices.size();
    std::vector<float> shade(n, 1.0f);
    std::vector<std::vector<int>> adjacency(n);
    for (const auto& sub : mesh.submeshes) {
        for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
            const uint32_t i[3] = {sub.indices[t], sub.indices[t + 1], sub.indices[t + 2]};
            for (int k = 0; k < 3; ++k) {
                if (i[k] >= n || i[(k + 1) % 3] >= n) continue;
                adjacency[i[k]].push_back(static_cast<int>(i[(k + 1) % 3]));
                adjacency[i[(k + 1) % 3]].push_back(static_cast<int>(i[k]));
            }
        }
    }
    const SampleDir* dirs = SampleDirs();
    const float eps = 0.0015f;      // 1.5 mm off the skin: no self-hit, still catches a touching fold
    const float maxDist = 0.11f;    // 11 cm: the breast's own size, not the other arm across the body
    auto worker = [&](size_t begin) {
        for (size_t i = begin; i < n; i += static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()))) {
            const assemble::SceneVertex& v = mesh.vertices[i];
            Vec3 nrm = normalize(v.normal);
            if (length(nrm) < 0.5f) { shade[i] = 1.0f; continue; }
            // Local concavity: neighbours above the tangent plane mean a crease (a convex surface curves away below it).
            float concavity = 0.0f;
            int count = 0;
            for (int j : adjacency[i]) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                const Vec3 d = sub(mesh.vertices[static_cast<size_t>(j)].pos, v.pos);
                const float len = length(d);
                if (len < 1e-7f) continue;
                concavity += dot(mul(d, 1.0f / len), nrm);
                ++count;
            }
            concavity = count ? std::max(0.0f, concavity / static_cast<float>(count)) : 0.0f;
            const float cavity = clampf(concavity * 4.0f, 0.0f, 1.0f);
            // Ambient occlusion: how much of the sky above the vertex the body itself hides.
            const Vec3 up = std::fabs(nrm.y) < 0.92f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            const Vec3 tangent = normalize(cross(up, nrm));
            const Vec3 bitangent = cross(nrm, tangent);
            const Vec3 origin = v.pos + mul(nrm, eps);
            int blocked = 0;
            for (int s = 0; s < kSamples; ++s) {
                const Vec3 d = tangent * dirs[s].x + bitangent * dirs[s].y + nrm * dirs[s].z;
                if (bvh.occluded(origin, d, maxDist)) ++blocked;
            }
            const float ao = 1.0f - static_cast<float>(blocked) / static_cast<float>(kSamples);
            // Overhead occlusion: how much of the sky above the vertex the body itself hides. The normal-hemisphere AO
            // cannot see it (its own dome points away from it), which is why a flat underside stays white without this.
            const SampleDir* skyDirs = SkySampleDirs();
            int skyBlocked = 0;
            for (int s = 0; s < kSamples; ++s) {
                const Vec3 d{skyDirs[s].x, skyDirs[s].y, skyDirs[s].z};
                if (bvh.occluded(origin, d, maxDist)) ++skyBlocked;
            }
            const float skyOcc = static_cast<float>(skyBlocked) / static_cast<float>(kSamples);
            const float down = clampf(-nrm.y, 0.0f, 1.0f);   // only skin facing down goes darker, never the up-facing top
            const float occ = 0.72f * (1.0f - ao) + 0.28f * cavity + 0.9f * skyOcc * down;
            shade[i] = clampf(1.0f - 0.68f * occ, 0.18f, 1.0f);
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned w = 1; w < hw; ++w) pool.emplace_back(worker, static_cast<size_t>(w));
    worker(0);
    for (auto& th : pool) th.join();
    // Smooth the per-vertex values: the map moves from vertex resolution to texel resolution, and faceting reads as
    // dirt. Two rings are enough at the body's density.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = shade;
        for (size_t i = 0; i < n; ++i) {
            if (adjacency[i].empty()) continue;
            float sum = shade[i];
            float weights = 1.0f;
            for (int j : adjacency[i]) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                sum += shade[static_cast<size_t>(j)];
                weights += 1.0f;
            }
            next[i] = sum / weights;
        }
        shade.swap(next);
    }
    return shade;
}

void RasterizeToUv(const assemble::SceneMesh& mesh, const std::vector<float>& shade, int W, int H,
                   std::vector<uint8_t>& map, std::vector<uint8_t>& covered) {
    auto texel = [&](float u, float v) {
        return std::pair<int, int>{std::min(W - 1, std::max(0, static_cast<int>(u * static_cast<float>(W)))),
                                  std::min(H - 1, std::max(0, static_cast<int>((1.0f - v) * static_cast<float>(H))))};
    };
    for (const auto& sub : mesh.submeshes) {
        for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
            const uint32_t idx[3] = {sub.indices[t], sub.indices[t + 1], sub.indices[t + 2]};
            if (idx[0] >= mesh.vertices.size() || idx[1] >= mesh.vertices.size() || idx[2] >= mesh.vertices.size()) continue;
            const auto [ax, ay] = texel(mesh.vertices[idx[0]].uv.x, mesh.vertices[idx[0]].uv.y);
            const auto [bx, by] = texel(mesh.vertices[idx[1]].uv.x, mesh.vertices[idx[1]].uv.y);
            const auto [cx, cy] = texel(mesh.vertices[idx[2]].uv.x, mesh.vertices[idx[2]].uv.y);
            // Pixel-space coordinates with the same orientation as BakeShaderTextures: v up, row 0 at the top.
            const float fx[3] = {mesh.vertices[idx[0]].uv.x * static_cast<float>(W), mesh.vertices[idx[1]].uv.x * static_cast<float>(W), mesh.vertices[idx[2]].uv.x * static_cast<float>(W)};
            const float fy[3] = {(1.0f - mesh.vertices[idx[0]].uv.y) * static_cast<float>(H), (1.0f - mesh.vertices[idx[1]].uv.y) * static_cast<float>(H), (1.0f - mesh.vertices[idx[2]].uv.y) * static_cast<float>(H)};
            const float den = (fy[1] - fy[2]) * (fx[0] - fx[2]) + (fx[2] - fx[1]) * (fy[0] - fy[2]);
            if (std::fabs(den) < 1e-7f) continue;
            const int x0 = std::max(0, std::min({ax, bx, cx})), x1 = std::min(W - 1, std::max({ax, bx, cx}));
            const int y0 = std::max(0, std::min({ay, by, cy})), y1 = std::min(H - 1, std::max({ay, by, cy}));
            const float sv[3] = {shade[idx[0]], shade[idx[1]], shade[idx[2]]};
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    const float l0 = ((fy[1] - fy[2]) * (px - fx[2]) + (fx[2] - fx[1]) * (py - fy[2])) / den;
                    const float l1 = ((fy[2] - fy[0]) * (px - fx[2]) + (fx[0] - fx[2]) * (py - fy[2])) / den;
                    const float l2 = 1.0f - l0 - l1;
                    if (l0 < -0.002f || l1 < -0.002f || l2 < -0.002f) continue;
                    const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
                    if (covered[at]) continue;   // overlapping UV islands: the first triangle owns the texel, as the bake does
                    covered[at] = 1;
                    const float s = l0 * sv[0] + l1 * sv[1] + l2 * sv[2];
                    map[at] = static_cast<uint8_t>(clampf(s, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }
}

void BlurMap(std::vector<uint8_t>& map, const std::vector<uint8_t>& covered, int W, int H) {
    std::vector<uint8_t> next = map;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (!covered[at]) continue;
            int sum = map[at], n = 1;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(W) + static_cast<size_t>(nx);
                    if (!covered[ni]) continue;
                    sum += map[ni];
                    ++n;
                }
            }
            next[at] = static_cast<uint8_t>((sum + n / 2) / n);
        }
    }
    map.swap(next);
}

}  // namespace

bool ApplySkinShade(assemble::Scene& scene, float strength, bool rebuild) {
    auto& ss = scene.skinShade;
    if (!ss.ready()) return false;
    if (static_cast<size_t>(ss.texture) >= scene.textures.size()) return false;
    auto& tex = scene.textures[static_cast<size_t>(ss.texture)];
    if (tex.width <= 0 || tex.height <= 0 || tex.rgba.size() < static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) * 4) return false;
    const int W = tex.width, H = tex.height;
    if (ss.base.size() != tex.rgba.size()) ss.base = tex.rgba;   // first call: the untouched create result

    if (rebuild || ss.shade.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) {
        std::vector<ShadeTri> tris;
        for (int mi : ss.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sub : mesh.submeshes)
                for (size_t t = 0; t + 2 < sub.indices.size(); t += 3)
                    if (sub.indices[t] < mesh.vertices.size() && sub.indices[t + 1] < mesh.vertices.size() && sub.indices[t + 2] < mesh.vertices.size())
                        tris.push_back({mesh.vertices[sub.indices[t]].pos, mesh.vertices[sub.indices[t + 1]].pos, mesh.vertices[sub.indices[t + 2]].pos});
        }
        if (tris.empty()) return false;
        SkinBvh bvh;
        bvh.build(std::move(tris));
        ss.shade.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 255);
        ss.covered.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
        for (int mi : ss.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            if (mesh.vertices.empty()) continue;
            const std::vector<float> shade = ShadeMesh(mesh, bvh);
            RasterizeToUv(mesh, shade, W, H, ss.shade, ss.covered);
        }
        BlurMap(ss.shade, ss.covered, W, H);
        BlurMap(ss.shade, ss.covered, W, H);
        ss.geometryDirty = false;
    }

    const float st = clampf(strength, 0.0f, 2.0f);
    if (ss.applied == st) {
        tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
        return true;
    }
    for (size_t i = 0; i < ss.covered.size(); ++i) {
        if (!ss.covered[i]) continue;
        const float lit = static_cast<float>(ss.shade[i]) / 255.0f;   // the map is the look at strength 1
        const float mul = clampf(1.0f - st * (1.0f - lit), 0.04f, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const size_t at = i * 4 + static_cast<size_t>(c);
            tex.rgba[at] = static_cast<uint8_t>(clampf(static_cast<float>(ss.base[at]) * mul, 0.0f, 255.0f) + 0.5f);
        }
        tex.rgba[i * 4 + 3] = ss.base[i * 4 + 3];
    }
    ss.applied = st;
    tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
    if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) {
        size_t n = 0;
        double sum = 0;
        int lo = 255, hi = 0;
        for (size_t i = 0; i < ss.shade.size(); ++i) {
            if (!ss.covered[i]) continue;
            ++n;
            sum += ss.shade[i];
            lo = std::min<int>(lo, ss.shade[i]);
            hi = std::max<int>(hi, ss.shade[i]);
        }
        std::fprintf(stderr, "skin shade: %zu texel(s), mean %.1f, min %d, max %d, strength %.2f\n", n,
                     n ? sum / static_cast<double>(n) : 0.0, lo, hi, static_cast<double>(st));
    }
    return true;
}

int ApplyNippleTint(assemble::Scene& scene, const Vec4& colour) {
    const bool override = colour.w > 0.0f;
    int changed = 0;
    for (auto& mat : scene.materials) {
        if (!mat.cardSkin) continue;
        Vec4 target = override ? Vec4{colour.x, colour.y, colour.z, 1.0f} : mat.cardNippleColour;
        if (!override && target.w <= 0.0f) target = Vec4{1, 1, 1, 1};
        if (target.w <= 0.0f) target.w = 1.0f;
        bool found = false;
        for (auto& pr : mat.props) {
            if (pr.first != "_overcolor1") continue;
            found = true;
            const Vec4 old = pr.second;
            if (std::fabs(old.x - target.x) < 1e-6f && std::fabs(old.y - target.y) < 1e-6f &&
                std::fabs(old.z - target.z) < 1e-6f && std::fabs(old.w - target.w) < 1e-6f) break;
            pr.second = target;
            ++changed;
            break;
        }
        if (!found) {
            mat.props.emplace_back("_overcolor1", target);
            ++changed;
        }
    }
    return changed;
}

}  // namespace cardbridge::exporter
