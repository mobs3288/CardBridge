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

    // True when a surface is close enough along the ray to count as occluding. `minT` skips hits that are too
    // close to the origin (e.g. the surface the origin sits on).
    bool occluded(Vec3 o, Vec3 d, float maxT, float minT = 0.0f) const {
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
                    if (hit(t, o, d, maxT, minT)) return true;
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

    static bool hit(const ShadeTri& t, Vec3 o, Vec3 d, float maxT, float minT = 0.0f) {
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
        return dist > std::max(1e-4f, minT) && dist < maxT;
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

// Which vertices are breast? The breast skin is skinned to the bust chain (bones carrying "bnip" / "bust").
// Only those are shaded: armpits, the groin, the navel and the back stay untouched. A character with no bust chains
// falls back to shading everything (the pre-existing behaviour, used by the tests' synthetic scenes).
struct BreastMaskResult {
    std::vector<char> keep;         // 1 = vertex in the breast region
    bool hasBustBones = false;      // true when the card carries a bust chain we can recognise
};

BreastMaskResult BreastMask(const assemble::Scene& scene, const assemble::SceneMesh& mesh) {
    const size_t n = mesh.vertices.size();
    BreastMaskResult out;
    out.keep.assign(n, 0);
    std::vector<char> bustBone(scene.bones.size(), 0);
    size_t nBust = 0;
    for (size_t bi = 0; bi < scene.bones.size(); ++bi)
        if (scene.bones[bi].name.find("bnip") != std::string::npos || scene.bones[bi].name.find("bust") != std::string::npos) {
            bustBone[bi] = 1;
            ++nBust;
        }
    out.hasBustBones = nBust > 0;
    if (nBust == 0) {   // nothing on this card says "breast": shade the whole body as before
        std::fill(out.keep.begin(), out.keep.end(), char{1});
        return out;
    }
    for (size_t i = 0; i < n; ++i) {
        const assemble::SceneVertex& v = mesh.vertices[i];
        float w = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const int b = v.bone[k];
            if (b >= 0 && static_cast<size_t>(b) < bustBone.size() && bustBone[static_cast<size_t>(b)]) w += v.weight[k];
        }
        if (w >= 0.12f) out.keep[i] = 1;   // at least a bit of bust-chain influence: the breast sheet, its fold and its crease
    }
    return out;
}

// One mesh's per-vertex shade factors (multipliers at strength 1: 1 = untouched).
struct ShadeFactors {
    std::vector<float> cleavage;   // cleavage fold darkness, ahead of the apply pass's strength scaling
    std::vector<float> underboob;  // under-breast fold darkness
};

ShadeFactors ShadeMesh(const assemble::SceneMesh& mesh, const SkinBvh& bvh, const assemble::Scene& scene, float topExtent, float width) {
    const size_t n = mesh.vertices.size();
    std::vector<float> shade(n, 1.0f), shadeU(n, 1.0f);
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
    BreastMaskResult bmask = BreastMask(scene, mesh);
    std::vector<char> keepMask = bmask.keep;
    const bool hasBustBones = bmask.hasBustBones;
    // Two one-ring expansions pull the spine-weighted seam between the breasts into the shaded region
    // (the skin is bust-weighted nearly everywhere there).
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<char> next = keepMask;
        for (size_t i = 0; i < n; ++i) {
            if (!keepMask[i]) continue;
            for (int j : adjacency[i]) if (j >= 0 && static_cast<size_t>(j) < n) next[static_cast<size_t>(j)] = 1;
        }
        keepMask.swap(next);
    }
    // The fold itself (breast sweeping into the torso) is torso-weighted, not bust-weighted, so it is invisible to the
    // bust mask and the ring expansions and the underboob had nothing to shade. Pull in the front skin just below the
    // bust region: the underside of the breast lives there. The front-only test keeps it a chest band, not a second body.
    float knLoPre = 1e30f, knHiPre = -1e30f;   // pre-extension bounds of the bust region (the fold lives just below it)
    if (hasBustBones) {
        float knLo = 1e30f, knHi = -1e30f;
        for (size_t i = 0; i < n; ++i)
            if (keepMask[i]) { knLo = std::min(knLo, mesh.vertices[i].pos.y); knHi = std::max(knHi, mesh.vertices[i].pos.y); }
        knLoPre = knLo;
        knHiPre = knHi;
        const float knSpan = std::max(knHi - knLo, 0.05f);
        const float extLo = knLo - knSpan * 0.6f;   // a little below the bust base
        const float extHi = knHi + knSpan * 0.1f;   // a little above the bust top
        if (knHi > knLo) {
            for (size_t i = 0; i < n; ++i) {
                if (keepMask[i]) continue;
                const float y = mesh.vertices[i].pos.y;
                if (y < extLo || y > extHi) continue;
                const Vec3 ni = normalize(mesh.vertices[i].normal);
                if (ni.z <= -0.1f) continue;   // front slopes and down-forward slopes, never the back
                keepMask[i] = 1;
            }
        }
    }
    // Bounds of the mesh and of the kept (bust) region. The cleavage top extent is measured against the bust region
    // itself: 1 = the top of the bust (the whole breast line), 2 = a full bust length higher, up toward the neck.
    float yMeshLo = 1e30f, yMeshHi = -1e30f, yKeepLo = 1e30f, yKeepHi = -1e30f;
    size_t keptCount = 0;
    for (size_t i = 0; i < n; ++i) {
        yMeshLo = std::min(yMeshLo, mesh.vertices[i].pos.y);
        yMeshHi = std::max(yMeshHi, mesh.vertices[i].pos.y);
        if (keepMask[i]) {
            yKeepLo = std::min(yKeepLo, mesh.vertices[i].pos.y);
            yKeepHi = std::max(yKeepHi, mesh.vertices[i].pos.y);
            ++keptCount;
        }
    }
    const float yLo = keptCount ? yKeepLo : yMeshLo;
    const float yHi = keptCount ? yKeepHi : yMeshHi;
    const float ySpan = std::max(yHi - yLo, 0.05f);
    const float topPos = yLo + clampf(topExtent, 0.0f, 2.0f) * ySpan;   // shading reaches up to here (1 = the full breast line)
    const float band = std::max(ySpan * 0.15f, 0.02f);                  // the fade-out zone above the chest's top
    // The breast profile: per vertical band, how much the bust protrudes beyond the sternum dip at that height - the z
    // range of the kept region. Recomputed on every apply, so reshaping the bust deepens and widens the cleavage at the
    // apex and narrows it toward the collarbone - the line follows the actual shape and stays dynamic with the shape slider.
    const int kBands = 64;
    const float bandSpan = ySpan / static_cast<float>(kBands);
    std::vector<float> depthBand(static_cast<size_t>(kBands), 0.0f);
    {
        std::vector<float> zLoB(static_cast<size_t>(kBands), 1e30f), zHiB(static_cast<size_t>(kBands), -1e30f);
        for (size_t i = 0; i < n; ++i) {
            if (!keepMask[i]) continue;
            const int b = static_cast<int>((mesh.vertices[i].pos.y - yLo) / bandSpan);
            if (b < 0 || b >= kBands) continue;
            const float z = mesh.vertices[i].pos.z;
            zLoB[static_cast<size_t>(b)] = std::min(zLoB[static_cast<size_t>(b)], z);
            zHiB[static_cast<size_t>(b)] = std::max(zHiB[static_cast<size_t>(b)], z);
        }
        for (int b = 0; b < kBands; ++b)
            if (zHiB[static_cast<size_t>(b)] > zLoB[static_cast<size_t>(b)])
                depthBand[static_cast<size_t>(b)] = zHiB[static_cast<size_t>(b)] - zLoB[static_cast<size_t>(b)];
        std::vector<float> smooth = depthBand;   // one box pass: a peak does not need to be quantised to one band
        for (int b = 0; b < kBands; ++b) {
            const float l = b > 0 ? depthBand[static_cast<size_t>(b - 1)] : depthBand[static_cast<size_t>(b)];
            const float c = depthBand[static_cast<size_t>(b)];
            const float r = b + 1 < kBands ? depthBand[static_cast<size_t>(b + 1)] : depthBand[static_cast<size_t>(b)];
            smooth[static_cast<size_t>(b)] = (l + 2.0f * c + r) * 0.25f;
        }
        depthBand.swap(smooth);
    }
    float depthMax = 0.1f;
    int depthTopBand = 0;
    for (int b = 0; b < kBands; ++b)
        if (depthBand[static_cast<size_t>(b)] > depthMax) { depthMax = depthBand[static_cast<size_t>(b)]; depthTopBand = b; }
    if (std::getenv("CARDBRIDGE_DEBUG_SKIN"))
        std::fprintf(stderr, "skin shade: breast profile depth max %.3f at y %.3f\n",
                     static_cast<double>(depthMax), static_cast<double>(yLo + (static_cast<float>(depthTopBand) + 0.5f) * bandSpan));
    if (std::getenv("CARDBRIDGE_DEBUG_SKIN"))
        std::fprintf(stderr, "skin shade: bust region y [%.3f, %.3f], span %.3f, top extent %.2f -> reach %.3f\n",
                     static_cast<double>(yLo), static_cast<double>(yHi), static_cast<double>(ySpan),
                     static_cast<double>(topExtent), static_cast<double>(topPos));
    const SampleDir* dirs = SampleDirs();
    const float eps = 0.0015f;      // 1.5 mm off the skin: no self-hit, still catches a touching fold
    const float maxDist = 0.11f;    // 11 cm: the breast's own size, not the other arm across the body
    const bool dbg = std::getenv("CARDBRIDGE_DEBUG_SKIN") != nullptr;
    // The fold line is the vertical layer of the bust where the normal swings downward most sharply (the contour where the
    // breast sweeps into the torso). Found from the same adjacency data as the crease measure - no extra raycasts - and
    // it lands wherever the breast lives, so it does not need a table of heights per card.
    float foldY = yLo + ySpan * 0.22f;   // if no sharp swing is found, shade just above the breast base
    {
        float bestTurn = -1.0f;
        for (size_t i = 0; i < n; ++i) {
            if (!keepMask[i]) continue;
            const Vec3 ni = normalize(mesh.vertices[i].normal);
            if (ni.y > 0.45f) continue;   // the fold is on the downward side of the breast
            const float foldCap = (knLoPre < knHiPre) ? knLoPre + 0.35f * (knHiPre - knLoPre) : yLo + 0.35f * ySpan;
            if (mesh.vertices[i].pos.y > foldCap) continue;   // and below the bust mass, never at the collarbone
            float turn = 0.0f;
            for (int j : adjacency[i]) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                const Vec3 nj = normalize(mesh.vertices[static_cast<size_t>(j)].normal);
                const float dy = std::fabs(mesh.vertices[static_cast<size_t>(j)].pos.y - mesh.vertices[i].pos.y);
                if (dy < 1e-5f) continue;
                turn += std::fabs(nj.y - ni.y) / dy;
            }
            if (turn > bestTurn) { bestTurn = turn; foldY = mesh.vertices[i].pos.y; }
        }
    }
    const float sigmaY = std::max(ySpan * 0.09f, 0.012f);
    if (dbg)
        std::fprintf(stderr, "skin shade: fold line at y %.3f (sigma %.3f), bust span [%.3f, %.3f]\n",
                     static_cast<double>(foldY), static_cast<double>(sigmaY), static_cast<double>(yLo), static_cast<double>(yHi));
    std::atomic<int> cntKept{0}, cntDown{0}, cntDownCav{0}, cntFired{0};
    auto worker = [&](size_t begin) {
        for (size_t i = begin; i < n; i += static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()))) {
            const assemble::SceneVertex& v = mesh.vertices[i];
            if (!keepMask[i]) { shade[i] = 1.0f; continue; }   // outside the breast: the skin keeps its base colour
            if (dbg) ++cntKept;
            Vec3 nrm = normalize(v.normal);
            if (length(nrm) < 0.5f) { shade[i] = 1.0f; continue; }
            // Local concavity: neighbours above the tangent plane mean a crease. Measured as the tangent-plane slope of the
            // neighbour direction (sin of the turning angle), not the raw height, so a deep fold reads strongly and a shallow,
            // nearly flat seam fades; a threshold keeps gentle folds from shading at all.
            float concavity = 0.0f;
            int count = 0;
            for (int j : adjacency[i]) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                const Vec3 d = sub(mesh.vertices[static_cast<size_t>(j)].pos, v.pos);
                const float len = length(d);
                if (len < 1e-7f) continue;
                const float h = dot(d, nrm);
                const float out = std::sqrt(std::max(1e-12f, len * len - h * h));   // in-plane (tangent) distance to the neighbour
                if (out < 1e-7f) continue;
                concavity += clampf(h / out, -1.0f, 1.0f);
                ++count;
            }
            concavity = count ? std::max(0.0f, concavity / static_cast<float>(count)) : 0.0f;
            const float cavity = clampf((concavity - 0.18f) * 2.4f, 0.0f, 1.0f);
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
            // The model faces +Z (the breast apex's outward normals point at +Z), so the cleavage - a fold facing the viewer -
            // is the +Z side of the chest. The sternum is a continuous, shallow depression from the top of the chest down
            // between the breasts, so the crease threshold is low; the arm pit crease faces sideways and is filtered out by this test.
            const bool front = !hasBustBones || nrm.z > 0.15f;
            // The cleavage falls off above `topPos`: a soft band so the line never ends in a hard horizontal cut.
            float fade = 1.0f;
            if (v.pos.y > topPos) fade = clampf((topPos + band - v.pos.y) / band, 0.0f, 1.0f);
            // The fold is the breast's lower contour: a gaussian band along `foldY` (found from the sharp turn in the
            // normal's vertical slope - the place where the breast sweeps into the torso), weighted by how downward the
            // normal faces and by the local occlusion. Nothing outside the band is shaded.
            const float foldDY = (v.pos.y - foldY) / sigmaY;
            const float foldW = std::exp(-0.5f * foldDY * foldDY);
            const float downT = clampf(-nrm.y / 0.7f, 0.0f, 1.0f);
            const float occFold = foldW * (0.25f + 0.55f * downT) * (0.55f + 0.75f * clampf(1.0f - ao, 0.0f, 1.0f));
            const bool cleavage = front;
            const bool underboob = occFold > 0.03f;
            if (dbg) {
                if (down > 0.30f) ++cntDown;
                if (down > 0.30f && foldW > 0.3f) ++cntDownCav;
                if (underboob) ++cntFired;
            }
            if (!cleavage && !underboob) { shade[i] = 1.0f; shadeU[i] = 1.0f; continue; }   // everything else keeps the card's colour
            // The cleavage wants to read as a line, not just a darkened blob under the fold walls: a soft band along the midline
            // (x == 0 in the bust region) with a gentle floor, so the whole breast line is shaded and the depth is set by the fold's
            // own occlusion. The band fades by `fade` at the chosen top extent.
            // The cleavage follows the breast: each height band's sternum-to-apex range sets how wide and how deep the line
            // runs. At the apex the seam is wide and deep, toward the collarbone it narrows and fades - and when the bust is
            // reshaped live, the profile follows.
            const int biRaw = static_cast<int>((v.pos.y - yLo) / bandSpan);
            const int bi = std::min(std::max(biRaw, 0), kBands - 1);
            const float shape = std::clamp(depthBand[static_cast<size_t>(bi)] / depthMax, 0.0f, 1.0f);
            const float halfWidth = std::max(ySpan * 0.22f, 0.04f) * clampf(width, 0.1f, 2.0f) * (0.35f + 0.85f * shape);   // how wide the cleavage line runs: width scales it around the default ~4 cm
            const float ex = v.pos.x / halfWidth;
            const float midBand = std::exp(-ex * ex);
            const float occBand = midBand * (0.45f * (1.0f - ao) + 0.38f * cavity + 0.12f) * fade * (0.30f + 0.75f * shape);
            const float occC = front ? std::max((0.8f * (1.0f - ao) + 1.2f * cavity) * fade * (0.25f + 0.75f * midBand), occBand) : 0.0f;
            const float occU = underboob ? occFold : 0.0f;
            shade[i] = clampf(1.0f - 0.55f * occC, 0.28f, 1.0f);
            shadeU[i] = clampf(1.0f - 0.4f * occU, 0.28f, 1.0f);
        }
    };
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned w = 1; w < hw; ++w) pool.emplace_back(worker, static_cast<size_t>(w));
    worker(0);
    for (auto& th : pool) th.join();
    if (dbg)
        std::fprintf(stderr, "skin shade: gates kept %d, down>0.30 %d, down+foldW>0.3 %d, underboob fired %d\n",
                     cntKept.load(), cntDown.load(), cntDownCav.load(), cntFired.load());
    // Smooth the per-vertex values: the map moves from vertex resolution to texel resolution, and faceting reads as
    // dirt. Two rings are enough at the body's density.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = shade, nextU = shadeU;
        for (size_t i = 0; i < n; ++i) {
            if (adjacency[i].empty()) continue;
            float sumC = shade[i], sumU = shadeU[i];
            float weights = 1.0f;
            for (int j : adjacency[i]) {
                if (j < 0 || static_cast<size_t>(j) >= n) continue;
                sumC += shade[static_cast<size_t>(j)];
                sumU += shadeU[static_cast<size_t>(j)];
                weights += 1.0f;
            }
            next[i] = sumC / weights;
            nextU[i] = sumU / weights;
        }
        shade.swap(next);
        shadeU.swap(nextU);
    }
    return {shade, shadeU};
}

void RasterizeToUv(const assemble::SceneMesh& mesh, const std::vector<float>& shade, const std::vector<float>& shadeU, int W, int H,
                   std::vector<uint8_t>& map, std::vector<uint8_t>& mapU, std::vector<uint8_t>& covered) {
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
            const float su[3] = {shadeU[idx[0]], shadeU[idx[1]], shadeU[idx[2]]};
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
                    const float u = l0 * su[0] + l1 * su[1] + l2 * su[2];
                    map[at] = static_cast<uint8_t>(clampf(s, 0.0f, 1.0f) * 255.0f + 0.5f);
                    mapU[at] = static_cast<uint8_t>(clampf(u, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }
}

// Feather the boundary of the covered region outward over a few texels. The shade map terminates at triangle edges;
// interpolating between 255 (uncovered, lit) and the shaded texels turns that hard cut into a soft ramp, so the UV
// edge and the triangle outline read as one continuous gradient.
void FeatherMap(std::vector<uint8_t>& map, std::vector<uint8_t>& covered, int W, int H, int passes) {
    for (int p = 0; p < passes; ++p) {
        std::vector<uint8_t> nextMap = map;
        std::vector<uint8_t> nextCovered = covered;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
                if (covered[at]) continue;
                int sum = 0, n = 0;
                bool touching = false;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        const size_t ni = static_cast<size_t>(ny) * static_cast<size_t>(W) + static_cast<size_t>(nx);
                        sum += map[ni];   // uncovered texels hold 255, so the ramp softens toward lit
                        ++n;
                        if (covered[ni]) touching = true;
                    }
                }
                if (!touching || n == 0) continue;
                nextMap[at] = static_cast<uint8_t>((sum + n / 2) / n);
                nextCovered[at] = 1;
            }
        }
        map.swap(nextMap);
        covered.swap(nextCovered);
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

bool ApplySkinShade(assemble::Scene& scene, float strengthC, float strengthU, float topExtent, float width, bool rebuild) {
    auto& ss = scene.skinShade;
    if (!ss.ready()) return false;
    if (static_cast<size_t>(ss.texture) >= scene.textures.size()) return false;
    auto& tex = scene.textures[static_cast<size_t>(ss.texture)];
    if (tex.width <= 0 || tex.height <= 0 || tex.rgba.size() < static_cast<size_t>(tex.width) * static_cast<size_t>(tex.height) * 4) return false;
    const int W = tex.width, H = tex.height;
    if (ss.base.size() != tex.rgba.size()) ss.base = tex.rgba;   // first call: the untouched create result

    const float te = clampf(topExtent, 0.0f, 2.0f);
    const float wa = clampf(width, 0.1f, 2.0f);
    const bool topChanged = ss.appliedTop != te;
    const bool widthChanged = ss.appliedWidth != wa;
    if (rebuild || topChanged || widthChanged || ss.shade.size() != static_cast<size_t>(W) * static_cast<size_t>(H)) {
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
        ss.shadeU.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 255);
        ss.covered.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
        for (int mi : ss.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            if (mesh.vertices.empty()) continue;
            const ShadeFactors factors = ShadeMesh(mesh, bvh, scene, te, wa);
            RasterizeToUv(mesh, factors.cleavage, factors.underboob, W, H, ss.shade, ss.shadeU, ss.covered);
        }
        FeatherMap(ss.shade, ss.covered, W, H, 8);
        FeatherMap(ss.shadeU, ss.covered, W, H, 8);
        BlurMap(ss.shade, ss.covered, W, H);
        BlurMap(ss.shadeU, ss.covered, W, H);
        BlurMap(ss.shade, ss.covered, W, H);
        BlurMap(ss.shadeU, ss.covered, W, H);
        BlurMap(ss.shade, ss.covered, W, H);
        BlurMap(ss.shadeU, ss.covered, W, H);
        ss.geometryDirty = false;
    }

    const float sc = clampf(strengthC, 0.0f, 2.0f), su = clampf(strengthU, 0.0f, 2.0f);
    if (ss.applied == sc && ss.appliedUnder == su && ss.appliedTop == te && ss.appliedWidth == wa) {
        tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
        return true;
    }
    for (size_t i = 0; i < ss.covered.size(); ++i) {
        if (!ss.covered[i]) continue;
        const float litC = static_cast<float>(ss.shade[i]) / 255.0f;
        const float litU = static_cast<float>(ss.shadeU[i]) / 255.0f;
        const float mulC = clampf(1.0f - sc * (1.0f - litC), 0.04f, 1.0f);
        const float mulU = clampf(1.0f - su * (1.0f - litU), 0.04f, 1.0f);
        const float mul = mulU < mulC ? mulU : mulC;
        for (int c = 0; c < 3; ++c) {
            const size_t at = i * 4 + static_cast<size_t>(c);
            tex.rgba[at] = static_cast<uint8_t>(clampf(static_cast<float>(ss.base[at]) * mul, 0.0f, 255.0f) + 0.5f);
        }
        tex.rgba[i * 4 + 3] = ss.base[i * 4 + 3];
    }
    ss.applied = sc;
    ss.appliedUnder = su;
    ss.appliedTop = te;
    ss.appliedWidth = wa;
    tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
    if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) {
        size_t n = 0;
        double sumC = 0, sumU = 0;
        int loC = 255, loU = 255, hiC = 0, hiU = 0;
        for (size_t i = 0; i < ss.shade.size(); ++i) {
            if (!ss.covered[i]) continue;
            ++n;
            sumC += ss.shade[i]; sumU += ss.shadeU[i];
            loC = std::min<int>(loC, ss.shade[i]); hiC = std::max<int>(hiC, ss.shade[i]);
            loU = std::min<int>(loU, ss.shadeU[i]); hiU = std::max<int>(hiU, ss.shadeU[i]);
        }
        std::fprintf(stderr, "skin shade: %zu texels, mean C %.1f/U %.1f, min C %d/U %d, max C %d/U %d, strength C %.2f/U %.2f, top %.2f, width %.2f\n",
                     n, n ? sumC / static_cast<double>(n) : 0.0, n ? sumU / static_cast<double>(n) : 0.0,
                     loC, loU, hiC, hiU, static_cast<double>(sc), static_cast<double>(su), static_cast<double>(te), static_cast<double>(wa));
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
