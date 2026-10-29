#include "assemble/DynamicBone.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::assemble {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

Vec3 column(const Mat4& m, int c) { return {m.m[c], m.m[4 + c], m.m[8 + c]}; }

// The rotation part of a matrix (scale removed): Transform.TransformDirection.
Mat4 rotationOf(const Mat4& w) {
    Mat4 r;
    for (int c = 0; c < 3; ++c) {
        Vec3 v = normalize(column(w, c));
        r.m[c] = v.x; r.m[4 + c] = v.y; r.m[8 + c] = v.z;
    }
    r.m[3] = r.m[7] = r.m[11] = 0.0f;
    return r;
}

Vec3 transformDirection(const Mat4& w, Vec3 v) { return rotationOf(w).dir(v); }
Vec3 inverseTransformDirection(const Mat4& w, Vec3 v) {
    Mat4 r = rotationOf(w);
    return {r.m[0] * v.x + r.m[4] * v.y + r.m[8] * v.z, r.m[1] * v.x + r.m[5] * v.y + r.m[9] * v.z, r.m[2] * v.x + r.m[6] * v.y + r.m[10] * v.z};
}

// Quaternion.FromToRotation as a rotation matrix.
Mat4 fromToRotation(Vec3 a, Vec3 b) {
    a = normalize(a);
    b = normalize(b);
    Mat4 r;
    float c = dot(a, b);
    if (c > 0.999999f) return r;
    Vec3 axis;
    if (c < -0.999999f) {   // opposite: half a turn about any perpendicular axis
        axis = std::fabs(a.x) < 0.9f ? cross(a, Vec3{1, 0, 0}) : cross(a, Vec3{0, 1, 0});
        axis = normalize(axis);
        const float u[3] = {axis.x, axis.y, axis.z};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i * 4 + j] = 2.0f * u[i] * u[j] - (i == j ? 1.0f : 0.0f);
        return r;
    }
    Vec3 v = cross(a, b);
    float k = 1.0f / (1.0f + c);
    r.m[0] = 1 + k * (-(v.z * v.z) - v.y * v.y); r.m[1] = -v.z + k * v.x * v.y;                 r.m[2] = v.y + k * v.x * v.z;
    r.m[4] = v.z + k * v.x * v.y;                r.m[5] = 1 + k * (-(v.z * v.z) - v.x * v.x); r.m[6] = -v.x + k * v.y * v.z;
    r.m[8] = -v.y + k * v.x * v.z;               r.m[9] = v.x + k * v.y * v.z;                r.m[10] = 1 + k * (-(v.y * v.y) - v.x * v.x);
    return r;
}

struct Particle {
    int node = -1;       // -1: the end point of a leaf bone
    int parent = -1;     // particle index
    float damping = 0, elasticity = 0, stiffness = 0, inert = 0, radius = 0, boneLength = 0;
    Vec3 pos, prev, endOffset;
};

// DynamicBoneCollider.Collide
void collide(const DbCollider& c, Vec3& p, float particleRadius) {
    const float scale = std::fabs(length(column(c.world, 2)));
    const float radius = c.radius * scale;
    const float half = (c.height - c.radius) * 0.5f;
    auto outsideSphere = [&](Vec3 center, float r) {
        float reach = r + particleRadius;
        Vec3 d = sub(p, center);
        float sq = dot(d, d);
        if (sq > 0.0f && sq < reach * reach) p = center + d * (reach / std::sqrt(sq));
    };
    auto insideSphere = [&](Vec3 center, float r) {
        float reach = r + particleRadius;
        Vec3 d = sub(p, center);
        float sq = dot(d, d);
        if (sq > reach * reach) p = center + d * (reach / std::sqrt(sq));
    };
    if (half <= 0.0f) {
        Vec3 center = c.world.point(c.center);
        c.inside ? insideSphere(center, radius) : outsideSphere(center, radius);
        return;
    }
    Vec3 a = c.center, b = c.center;
    (c.direction == 0 ? a.x : c.direction == 1 ? a.y : a.z) -= half;
    (c.direction == 0 ? b.x : c.direction == 1 ? b.y : b.z) += half;
    Vec3 p0 = c.world.point(a), p1 = c.world.point(b);
    const float reach = radius + particleRadius;
    Vec3 axis = sub(p1, p0), rel = sub(p, p0);
    float t = dot(rel, axis);
    if (t <= 0.0f) {
        float sq = dot(rel, rel);
        if (c.inside ? sq > reach * reach : (sq > 0.0f && sq < reach * reach)) p = p0 + rel * (reach / std::sqrt(sq));
        return;
    }
    float axisSq = dot(axis, axis);
    if (t >= axisSq) {
        rel = sub(p, p1);
        float sq = dot(rel, rel);
        if (c.inside ? sq > reach * reach : (sq > 0.0f && sq < reach * reach)) p = p1 + rel * (reach / std::sqrt(sq));
    } else if (axisSq > 0.0f) {
        t /= axisSq;
        rel = sub(rel, axis * t);
        float sq = dot(rel, rel);
        if (c.inside ? sq > reach * reach : (sq > 0.0f && sq < reach * reach)) {
            float d = std::sqrt(sq);
            p = p + rel * ((reach - d) / d);
        }
    }
}

} // namespace

float DbCurve::eval(float t) const {
    if (keys.empty()) return 1.0f;
    if (t <= keys.front()[0]) return keys.front()[1];
    if (t >= keys.back()[0]) return keys.back()[1];
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const auto& a = keys[i];
        const auto& b = keys[i + 1];
        if (t > b[0]) continue;
        float dt = b[0] - a[0];
        if (dt <= 0.0f) return b[1];
        float u = (t - a[0]) / dt, u2 = u * u, u3 = u2 * u;
        float m0 = a[3] * dt, m1 = b[2] * dt;   // outSlope of the left key, inSlope of the right key
        return (2 * u3 - 3 * u2 + 1) * a[1] + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b[1] + (u3 - u2) * m1;
    }
    return keys.back()[1];
}

void DbTree::Init(const std::vector<Mat4>& locals, const std::vector<int>& parents, const Mat4& rootWorldMatrix) {
    restLocal = local = locals;
    parent = parents;
    rootWorld = rootWorldMatrix;
    children.assign(locals.size(), {});
    for (size_t i = 0; i < parents.size(); ++i)
        if (parents[i] >= 0) children[static_cast<size_t>(parents[i])].push_back(static_cast<int>(i));
}

Mat4 DbTree::WorldOf(int node, bool rest) const {
    const auto& l = rest ? restLocal : local;
    Mat4 w = l[static_cast<size_t>(node)];
    for (int p = parent[static_cast<size_t>(node)]; p >= 0; p = parent[static_cast<size_t>(p)]) w = l[static_cast<size_t>(p)] * w;
    return rootWorld * w;
}

int SettleDynamicBone(DbTree& tree, const DbParams& cfg, const std::vector<DbCollider>& colliders, const DbTree* initial, int fixedSteps) {
    if (cfg.root < 0 || static_cast<size_t>(cfg.root) >= tree.local.size()) return 0;
    const size_t n = tree.local.size();
    std::vector<Mat4> rest(n);   // the pose every update starts from: InitTransforms resets the chain each frame
    for (size_t i = 0; i < n; ++i) rest[i] = tree.WorldOf(static_cast<int>(i), true);
    auto isIn = [](const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); };
    const Mat4 ownerWorld = cfg.owner >= 0 ? rest[static_cast<size_t>(cfg.owner)] : tree.rootWorld;
    const float objectScale = std::fabs(length(column(ownerWorld, 0)));

    // AppendParticles
    std::vector<Particle> ps;
    float totalLength = 0.0f;
    auto append = [&](auto&& self, int node, int parentIdx, float boneLength) -> void {
        Particle pt;
        pt.node = node;
        pt.parent = parentIdx;
        if (node >= 0) {
            pt.pos = pt.prev = rest[static_cast<size_t>(node)].translation();
        } else {
            const int pn = ps[static_cast<size_t>(parentIdx)].node;
            const Mat4& pw = rest[static_cast<size_t>(pn)];
            if (cfg.endLength > 0.0f) {
                int gp = tree.parent[static_cast<size_t>(pn)];
                if (gp >= 0) {
                    Vec3 away = pw.translation() * 2.0f;
                    away = sub(away, rest[static_cast<size_t>(gp)].translation());
                    pt.endOffset = inverse(pw).point(away) * cfg.endLength;
                } else {
                    pt.endOffset = {cfg.endLength, 0, 0};
                }
            } else {
                pt.endOffset = inverse(pw).point(transformDirection(ownerWorld, cfg.endOffset) + pw.translation());
            }
            pt.pos = pt.prev = pw.point(pt.endOffset);
        }
        if (parentIdx >= 0) {
            boneLength += length(sub(rest[static_cast<size_t>(ps[static_cast<size_t>(parentIdx)].node)].translation(), pt.pos));
            pt.boneLength = boneLength;
            totalLength = std::max(totalLength, boneLength);
        }
        const int mine = static_cast<int>(ps.size());
        ps.push_back(pt);
        if (node < 0) return;
        const auto& kids = tree.children[static_cast<size_t>(node)];
        int rolled = -1;
        for (int k : kids) {
            if (isIn(cfg.exclusions, k)) continue;
            if (isIn(cfg.notRolls, k)) { rolled = k; continue; }
            self(self, k, mine, boneLength);
        }
        if (rolled >= 0)   // a "not roll" child is skipped and its children take its place
            for (int g : tree.children[static_cast<size_t>(rolled)])
                if (!isIn(cfg.exclusions, g) && !isIn(cfg.notRolls, g)) self(self, g, mine, boneLength);
        if (kids.empty() && (cfg.endLength > 0.0f || cfg.endOffset.x != 0 || cfg.endOffset.y != 0 || cfg.endOffset.z != 0)) self(self, -1, mine, boneLength);
    };
    append(append, cfg.root, -1, 0.0f);
    if (ps.size() < 2) return 0;
    Vec3 objectMove{0, 0, 0};
    if (initial && fixedSteps > 0 && initial->local.size() == n) {   // start from the pose before the sliders, the owner jumping into place
        std::vector<Mat4> before(n);
        for (size_t i = 0; i < n; ++i) before[i] = initial->WorldOf(static_cast<int>(i), true);
        for (auto& p : ps) {
            if (p.node >= 0) p.pos = p.prev = before[static_cast<size_t>(p.node)].translation();
            else p.pos = p.prev = before[static_cast<size_t>(ps[static_cast<size_t>(p.parent)].node)].point(p.endOffset);
        }
        Vec3 was = (cfg.owner >= 0 ? before[static_cast<size_t>(cfg.owner)] : initial->rootWorld).translation();
        Vec3 now = ownerWorld.translation();
        objectMove = {now.x - was.x, now.y - was.y, now.z - was.z};
    }

    for (auto& p : ps) {
        p.damping = cfg.damping; p.elasticity = cfg.elasticity; p.stiffness = cfg.stiffness; p.inert = cfg.inert; p.radius = cfg.radius;
        if (totalLength > 0.0f) {
            float t = p.boneLength / totalLength;
            if (!cfg.dampingC.keys.empty()) p.damping *= cfg.dampingC.eval(t);
            if (!cfg.elasticityC.keys.empty()) p.elasticity *= cfg.elasticityC.eval(t);
            if (!cfg.stiffnessC.keys.empty()) p.stiffness *= cfg.stiffnessC.eval(t);
            if (!cfg.inertC.keys.empty()) p.inert *= cfg.inertC.eval(t);
            if (!cfg.radiusC.keys.empty()) p.radius *= cfg.radiusC.eval(t);
        }
        p.damping = clamp01(p.damping); p.elasticity = clamp01(p.elasticity); p.stiffness = clamp01(p.stiffness); p.inert = clamp01(p.inert);
        p.radius = std::max(p.radius, 0.0f);
    }

    // Gravity as UpdateParticles1 builds it: the part of the world gravity the rest pose already balances is removed.
    Vec3 gravity = cfg.gravity;
    {
        Vec3 g0 = normalize(cfg.gravity);
        Vec3 localG = inverseTransformDirection(rest[static_cast<size_t>(cfg.root)], cfg.gravity);
        Vec3 rootG = transformDirection(rest[static_cast<size_t>(cfg.root)], localG);
        gravity = sub(gravity, g0 * std::max(dot(rootG, g0), 0.0f));
        gravity = (gravity + cfg.force) * objectScale;
    }

    int steps = 0;
    for (; steps < 4000; ++steps) {
        // UpdateParticles1 (the object does not move: no inertia term)
        for (auto& p : ps) {
            if (p.parent >= 0) {
                Vec3 v = sub(p.pos, p.prev);
                Vec3 push = objectMove * p.inert;
                p.prev = p.pos + push;
                p.pos = p.pos + v * (1.0f - p.damping) + gravity + push;
            } else {
                p.prev = p.pos;
                p.pos = rest[static_cast<size_t>(p.node)].translation();
            }
        }
        // UpdateParticles2
        for (size_t i = 1; i < ps.size(); ++i) {
            Particle& p = ps[i];
            const Particle& par = ps[static_cast<size_t>(p.parent)];
            const Mat4& pw = rest[static_cast<size_t>(par.node)];
            float restLen = p.node >= 0 ? length(sub(pw.translation(), rest[static_cast<size_t>(p.node)].translation())) : length(pw.dir(p.endOffset));
            float k = p.stiffness;   // Mathf.Lerp(1, stiffness, weight=1)
            if (k > 0.0f || p.elasticity > 0.0f) {
                Mat4 m = pw;
                m.m[3] = par.pos.x; m.m[7] = par.pos.y; m.m[11] = par.pos.z;
                Vec3 target = p.node >= 0 ? m.point(tree.restLocal[static_cast<size_t>(p.node)].translation()) : m.point(p.endOffset);
                Vec3 d = sub(target, p.pos);
                p.pos = p.pos + d * p.elasticity;
                if (k > 0.0f) {
                    d = sub(target, p.pos);
                    float dl = length(d);
                    float reach = restLen * (1.0f - k) * 2.0f;
                    if (dl > reach) p.pos = p.pos + d * ((dl - reach) / dl);
                }
            }
            for (const auto& c : colliders) collide(c, p.pos, p.radius * objectScale);
            if (cfg.freezeAxis != 0) {
                Vec3 nrm = column(pw, cfg.freezeAxis - 1);   // right / up / forward of the parent transform
                nrm = normalize(nrm);
                float dist = dot(nrm, sub(p.pos, par.pos));
                p.pos = sub(p.pos, nrm * dist);
            }
            Vec3 d = sub(par.pos, p.pos);
            float dl = length(d);
            if (dl > 0.0f) p.pos = p.pos + d * ((dl - restLen) / dl);
        }
        objectMove = {0, 0, 0};
        if (fixedSteps > 0 && initial) { if (steps + 1 >= fixedSteps) { ++steps; break; } continue; }
        float moved = 0.0f;
        for (auto& p : ps) moved = std::max(moved, length(sub(p.pos, p.prev)));
        if (moved < 1e-7f && steps > 2) break;
    }

    // ApplyParticlesToTransforms
    for (const auto& p : ps)
        if (p.node >= 0) tree.local[static_cast<size_t>(p.node)] = tree.restLocal[static_cast<size_t>(p.node)];
    auto parentWorld = [&](int node) { int pp = tree.parent[static_cast<size_t>(node)]; return pp >= 0 ? tree.WorldOf(pp) : tree.rootWorld; };
    for (size_t i = 1; i < ps.size(); ++i) {
        const Particle& p = ps[i];
        const Particle& par = ps[static_cast<size_t>(p.parent)];
        if (tree.children[static_cast<size_t>(par.node)].size() <= 1) {
            Vec3 val = p.node >= 0 ? tree.local[static_cast<size_t>(p.node)].translation() : p.endOffset;
            Mat4 pwNow = tree.WorldOf(par.node);
            Mat4 turn = fromToRotation(transformDirection(pwNow, val), sub(p.pos, par.pos));
            Mat4 nw = turn * pwNow;
            nw.m[3] = pwNow.m[3]; nw.m[7] = pwNow.m[7]; nw.m[11] = pwNow.m[11];
            tree.local[static_cast<size_t>(par.node)] = inverse(parentWorld(par.node)) * nw;
        }
        if (p.node >= 0) {
            Mat4 cw = tree.WorldOf(p.node);
            cw.m[3] = p.pos.x; cw.m[7] = p.pos.y; cw.m[11] = p.pos.z;
            tree.local[static_cast<size_t>(p.node)] = inverse(parentWorld(p.node)) * cw;
        }
    }
    return steps;
}

} // namespace cardbridge::assemble
