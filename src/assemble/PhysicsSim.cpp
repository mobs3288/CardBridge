#include "assemble/PhysicsSim.h"

#include "assemble/Pose.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cardbridge::assemble {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Mat4 rotationPart(const Mat4& m) {
    Mat4 r = m;
    r.m[3] = r.m[7] = r.m[11] = 0.0f;
    return r;
}

Mat4 translated(Mat4 rot, Vec3 t) {
    rot.m[3] = t.x; rot.m[7] = t.y; rot.m[11] = t.z;
    return rot;
}

struct WorldCollider {
    Vec3 a, b;     // capsule segment ends (equal for a sphere)
    float radius = 0;
};

// Closest point on triangle to point P (Ericson, Real-Time Collision Detection).
static Vec3 closestPointTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = {b.x - a.x, b.y - a.y, b.z - a.z}, ac = {c.x - a.x, c.y - a.y, c.z - a.z}, ap = {p.x - a.x, p.y - a.y, p.z - a.z};
    const Vec3 bp = {p.x - b.x, p.y - b.y, p.z - b.z}, cp = {p.x - c.x, p.y - c.y, p.z - c.z};
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { const float v = d1 / (d1 - d3); return {a.x + ab.x * v, a.y + ab.y * v, a.z + ab.z * v}; }
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { const float w = d2 / (d2 - d6); return {a.x + ac.x * w, a.y + ac.y * w, a.z + ac.z * w}; }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return {b.x + (c.x - b.x) * w, b.y + (c.y - b.y) * w, b.z + (c.z - b.z) * w}; }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    return {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
}

// Pushes a particle of radius r out of a collider; a sphere is a capsule with a zero-length segment.
void collide(Vec3& p, float r, const WorldCollider& c) {
    const Vec3 ab = sub(c.b, c.a);
    const float len2 = dot(ab, ab);
    const float t = len2 > 1e-12f ? std::min(1.0f, std::max(0.0f, dot(sub(p, c.a), ab) / len2)) : 0.0f;
    const Vec3 closest = c.a + ab * t;
    const Vec3 d = sub(p, closest);
    const float dist = length(d);
    const float want = c.radius + r;
    if (dist < want && dist > 1e-9f) p = closest + d * (want / dist);
}

} // namespace

void PhysicsSim::Reset(const Scene& scene, const std::vector<Mat4>& poseWorld, bool jiggleBust, bool jiggleButt, float jiggleIntensity, bool jiggleOnly, bool outfitOnly) {
    particles_.clear();
    chains_.clear();
    speed_ = 0.0f;
    if (poseWorld.size() != scene.bones.size()) return;
    for (size_t ci = 0; ci < scene.chains.size(); ++ci) {
        const SceneChain& sc = scene.chains[ci];
        if (jiggleOnly && !sc.characterOwn) continue;   // the static settle moves the body's jiggle only; hair keeps its authored pose
        if (outfitOnly && sc.part.rfind("outfit", 0) != 0) continue;   // the outfit settle moves the garments only; hair and skin keep their pose
        if (sc.bones.size() < 2) continue;
        bool jiggle = false;
        if (sc.characterOwn) {   // the body's own jiggle: skipped while its switch is off, like the PMX export
            jiggle = IsBustJiggleChain(scene, sc) ? jiggleBust : jiggleButt;
            if (!jiggle) continue;
        }
        Chain chain;
        chain.scene = ci;
        chain.jiggle = jiggle;
        // The intensity opens the swing envelope, like the export's rotation limits. The +0.5 keeps the gentle default (0.5)
        // lively enough to see the hanging swing instead of a clamped wobble.
        if (jiggle) chain.envelopeScale = 0.5f + jiggleIntensity;
        std::map<int, int> byBone;
        for (size_t k = 0; k < sc.bones.size(); ++k) {
            const int b = sc.bones[k];
            if (b < 0 || static_cast<size_t>(b) >= scene.bones.size()) continue;
            Particle p;
            p.bone = b;
            p.world = poseWorld[static_cast<size_t>(b)];
            p.pos = p.prev = p.world.translation();
            if (k > 0) {
                const int parentBone = scene.bones[static_cast<size_t>(b)].parent;
                auto it = byBone.find(parentBone);
                p.parent = it != byBone.end() ? it->second : chain.particles.empty() ? -1 : chain.particles[0];
                if (p.parent >= 0) {
                    const Vec3 off = sub(scene.bones[static_cast<size_t>(b)].pos, scene.bones[static_cast<size_t>(particles_[static_cast<size_t>(p.parent)].bone)].pos);
                    p.restLocal = off;
                    p.restLen = length(off);
                    if (particles_[static_cast<size_t>(p.parent)].child < 0) particles_[static_cast<size_t>(p.parent)].child = static_cast<int>(particles_.size());
                }
            }
            byBone[b] = static_cast<int>(particles_.size());
            chain.particles.push_back(static_cast<int>(particles_.size()));
            particles_.push_back(p);
        }
        if (chain.particles.size() >= 2) {
            chain.anchorPrev = particles_[static_cast<size_t>(chain.particles[0])].pos;
            chains_.push_back(std::move(chain));
        }
    }
}

void PhysicsSim::SetColliderOverride(const std::vector<SceneCollider>& colliders) {
    colliderOverride_ = colliders;
}

void PhysicsSim::Nudge(Vec3 impulse) {
    for (const Chain& chain : chains_) {
        // The bust / butt chains are heavy skin, not hair strands: a shake needs a bigger hit to show, so it gets 2.5x.
        const Vec3 hit = chain.jiggle ? impulse * 2.5f : impulse;
        for (size_t k = 1; k < chain.particles.size(); ++k) {
            Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
            p.prev = sub(p.prev, hit);   // the next update sees this much more velocity
        }
    }
    speed_ = 1.0f;
}

bool PhysicsSim::Step(const Scene& scene, const std::vector<Mat4>& poseWorld, int steps, std::vector<Mat4>& skin) {
    if (poseWorld.size() != scene.bones.size() || skin.size() != scene.bones.size()) return false;
    // The colliders to use this step: the override when set (a settle that fits the cloth outside the skin), otherwise
    // the scene's own, resolved against the pose.
    std::vector<WorldCollider> colliders;
    if (!colliderOverride_.empty()) {
        for (const auto& c : colliderOverride_) {
            if (c.bone < 0 || c.bone >= static_cast<int>(poseWorld.size()) || c.inside || c.radius <= 0.0f) continue;
            const Mat4 m = poseWorld[static_cast<size_t>(c.bone)] * Mat4::TRS(scene.bones[static_cast<size_t>(c.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
            const Vec3 centre = m.point(c.center);
            const float half = std::max(0.0f, c.height * 0.5f - c.radius);
            const Vec3 axis = normalize(m.dir(c.axis));
            colliders.push_back({sub(centre, axis * half), centre + axis * half, c.radius});
        }
    } else {
        for (const auto& c : scene.colliders) {
            if (c.bone < 0 || static_cast<size_t>(c.bone) >= poseWorld.size() || c.inside || c.radius <= 0.0f) continue;
            const Mat4 m = poseWorld[static_cast<size_t>(c.bone)] * Mat4::TRS(scene.bones[static_cast<size_t>(c.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
            const Vec3 centre = m.point(c.center);
            const float half = std::max(0.0f, c.height * 0.5f - c.radius);
            const Vec3 axis = normalize(m.dir(c.axis));
            colliders.push_back({sub(centre, axis * half), centre + axis * half, c.radius});
        }
    }
    // The arms are not in the game's collider list, but the breast clipping into them is the classic break:
    // add an upper-arm + forearm capsule per arm so the jiggle particles slide off the arm instead of through it.
    // Hand capsules (forearm -> hand) and shin capsules (knee -> ankle) close the other two
    // universal gaps: sleeves draped to the wrist, and long coat tails swinging to the
    // calves where no authored collider reaches.
    for (const char* side : {"_L", "_R"}) {
        const int shoulder = scene.boneIndex(std::string("cf_j_shoulder") + side);
        const int arm = scene.boneIndex(std::string("cf_j_arm00") + side);
        const int forearm = scene.boneIndex(std::string("cf_j_forearm01") + side);
        const int hand = scene.boneIndex(std::string("cf_j_hand_") + side);
        const int knee = scene.boneIndex(std::string("cf_j_leg01_") + side);
        const int leg03 = scene.boneIndex(std::string("cf_j_leg03_") + side);
        auto addArmSeg = [&](int a, int b, float r) {
            if (a < 0 || b < 0 || static_cast<size_t>(a) >= poseWorld.size() || static_cast<size_t>(b) >= poseWorld.size()) return;
            colliders.push_back({poseWorld[static_cast<size_t>(a)].translation(), poseWorld[static_cast<size_t>(b)].translation(), r});
        };
        addArmSeg(shoulder, arm, 0.040f);
        addArmSeg(arm, forearm, 0.032f);
        addArmSeg(forearm, hand, 0.030f);
        addArmSeg(knee, leg03, 0.038f);
    }

    float moved = 0.0f;
    for (int s = 0; s < std::max(1, steps); ++s) {
        const bool simulate = s < steps;
        for (Chain& chain : chains_) {
            const SceneChain& sc = scene.chains[chain.scene];
            Particle& anchor = particles_[static_cast<size_t>(chain.particles[0])];
            const Mat4 anchorWorld = poseWorld[static_cast<size_t>(anchor.bone)];
            const Vec3 anchorPos = anchorWorld.translation();
            anchor.world = anchorWorld;
            if (simulate) {
                const Vec3 objectMove = sub(anchorPos, chain.anchorPrev);
                chain.anchorPrev = anchorPos;
                // gravity as the component has it: what the rest pose already hangs under is left out, the rest of it acts.
                Vec3 force = sc.gravity;
                const float gl = length(sc.gravity);
                if (gl > 1e-12f) {
                    const Vec3 fdir = sc.gravity * (1.0f / gl);
                    const Vec3 rf = rotationPart(anchorWorld).dir(sc.gravity);
                    force = sub(force, fdir * std::max(dot(rf, fdir), 0.0f));
                }
                force = force + sc.force;
                // step 1: inertia, damping, force
                for (size_t k = 0; k < chain.particles.size(); ++k) {
                    Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
                    if (k == 0) { p.prev = p.pos; p.pos = anchorPos; continue; }
                    const Vec3 v = sub(p.pos, p.prev);
                    const Vec3 rmove = objectMove * sc.inert;
                    p.prev = p.pos + rmove;
                    p.pos = p.pos + v * (1.0f - std::min(1.0f, std::max(0.0f, sc.damping))) + force + rmove;
                }
                // step 2: spring back to the rest place under the parent, keep the length, collide
                for (size_t k = 1; k < chain.particles.size(); ++k) {
                    Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
                    const Particle& par = particles_[static_cast<size_t>(p.parent)];
                    // the rest place: under the parent's simulated position, in the frame the pose gives the parent (the component resets its bones
                    // to their rest turn before it runs, so what it springs back to does not depend on how the chain turned a moment ago)
                    const Vec3 restPos = par.pos + rotationPart(poseWorld[static_cast<size_t>(par.bone)]).dir(p.restLocal);
                    Vec3 d = sub(restPos, p.pos);
                    p.pos = p.pos + d * sc.elasticity;
                    if (sc.stiffness > 0.0f) {
                        d = sub(restPos, p.pos);
                        const float len = length(d);
                        const float maxLen = p.restLen * (1.0f - sc.stiffness) * 2.0f * chain.envelopeScale;
                        if (len > maxLen && len > 1e-9f) p.pos = p.pos + d * ((len - maxLen) / len);
                    }
                    for (const auto& c : colliders) collide(p.pos, sc.radius, c);
                    const Vec3 dd = sub(par.pos, p.pos);
                    const float dl = length(dd);
                    if (dl > 1e-9f) p.pos = p.pos + dd * ((dl - p.restLen) / dl);
                    moved = std::max(moved, length(sub(p.pos, p.prev)));
                }
            }
            // step 3: the bones follow their particles, top down: each takes its parent's turn, then turns on to point at its first child
            for (size_t k = 1; k < chain.particles.size(); ++k) {
                Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
                const Particle& par = particles_[static_cast<size_t>(p.parent)];
                Mat4 rot = rotationPart(par.world);
                if (p.child >= 0) {
                    const Vec3 want = sub(particles_[static_cast<size_t>(p.child)].pos, p.pos);
                    const Vec3 have = rot.dir(particles_[static_cast<size_t>(p.child)].restLocal);
                    if (length(want) > 1e-9f && length(have) > 1e-9f) rot = RotationBetween(have, want) * rot;
                }
                p.world = translated(rot, p.pos);
            }
        }
        if (!simulate) break;
    }
    for (const Chain& chain : chains_)
        for (size_t k = 1; k < chain.particles.size(); ++k) {
            const Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
            skin[static_cast<size_t>(p.bone)] = p.world * Mat4::TRS(scene.bones[static_cast<size_t>(p.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        }
    if (steps > 0) speed_ = moved;
    return speed_ > 1e-5f;
}

// Snaps the outfit cloth's chain bones to a tiny fixed distance off the skin, both directions: pull a too-distant particle in to the
// clearance, push a too-close one out to it. The cloth ends up hugging the skin silhouette (the authored straight-then-hanging pose)
// instead of ballooning off the body (the raw collider radius fill) while never slicing into it either.
void PhysicsSim::PushOutsideBody(const Scene& scene) {
    // collect the body's triangles once; the loop per chain per particle is cheap against a few k tris
    std::vector<Vec3> tri, nrm;
    for (const auto& p : scene.parts) {
        if (p.group != "Body" && p.group != "Head") continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            for (const auto& sm : mesh.submeshes) {
                if (sm.material < 0 || static_cast<size_t>(sm.material) >= scene.materials.size()) continue;
                for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                    const Vec3 a = mesh.vertices[sm.indices[i]].pos, b = mesh.vertices[sm.indices[i + 1]].pos, c = mesh.vertices[sm.indices[i + 2]].pos;
                    Vec3 n = cross(sub(b, a), sub(c, a));
                    const float nl = length(n);
                    if (nl < 1e-8f) continue;
                    n = scale(n, 1.0f / nl);
                    tri.push_back(a); tri.push_back(b); tri.push_back(c); nrm.push_back(n);
                }
            }
        }
    }
    if (tri.empty()) return;
    const float kClearance = 0.012f;   // ~1.5 mm of air: enough to avoid z-fighting the skin, tight enough for a skin-hugging kiss
    for (const Chain& chain : chains_) {
        const SceneChain& sc = scene.chains[chain.scene];
        if (sc.part.rfind("outfit", 0) != 0) continue;   // outfit cloth only
        bool pushed = false;
        for (size_t k = 1; k < chain.particles.size(); ++k) {
            Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
            int best = -1; float bestDist2 = 1e30f;
            for (size_t ti = 0; ti < tri.size() / 3; ++ti) {
                const Vec3& a = tri[ti * 3], & b = tri[ti * 3 + 1], & c = tri[ti * 3 + 2];
                const Vec3 cp = closestPointTriangle(p.pos, a, b, c);
                const Vec3 dd = sub(p.pos, cp);
                const float d2 = dot(dd, dd);
                if (d2 < bestDist2) { bestDist2 = d2; best = static_cast<int>(ti); }
            }
            if (best < 0) continue;
            const Vec3& a = tri[best * 3], & b = tri[best * 3 + 1], & c = tri[best * 3 + 2];
            const Vec3 cp = closestPointTriangle(p.pos, a, b, c);
            const float signedDist = dot(sub(p.pos, cp), nrm[best]);
            const float offset = signedDist - kClearance;
            if (std::fabs(offset) < 1e-5f) continue;   // already kissing
            const Vec3 shift = scale(nrm[best], -offset);   // positive offset -> shift inwards; negative -> push out
            p.pos = add(p.pos, shift);
            p.prev = add(p.prev, shift);
            pushed = true;
        }
        if (pushed) {   // re-derive the chain bones from the moved particles, top down
            for (size_t k = 1; k < chain.particles.size(); ++k) {
                Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
                const Particle& par = particles_[static_cast<size_t>(p.parent)];
                Mat4 rot = rotationPart(par.world);
                if (p.child >= 0) {
                    const Vec3 want = sub(particles_[static_cast<size_t>(p.child)].pos, p.pos);
                    const Vec3 have = rot.dir(particles_[static_cast<size_t>(p.child)].restLocal);
                    if (length(want) > 1e-9f && length(have) > 1e-9f) rot = RotationBetween(have, want) * rot;
                }
                p.world = translated(rot, p.pos);
            }
        }
    }
}

} // namespace cardbridge::assemble
