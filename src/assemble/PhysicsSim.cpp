#include "assemble/PhysicsSim.h"

#include "assemble/Pose.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cardbridge::assemble {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
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

void PhysicsSim::Reset(const Scene& scene, const std::vector<Mat4>& poseWorld) {
    particles_.clear();
    chains_.clear();
    speed_ = 0.0f;
    if (poseWorld.size() != scene.bones.size()) return;
    for (size_t ci = 0; ci < scene.chains.size(); ++ci) {
        const SceneChain& sc = scene.chains[ci];
        if (sc.bones.size() < 2) continue;
        Chain chain;
        chain.scene = ci;
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

void PhysicsSim::Nudge(Vec3 impulse) {
    for (const Chain& chain : chains_)
        for (size_t k = 1; k < chain.particles.size(); ++k) {
            Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
            p.prev = sub(p.prev, impulse);   // the next update sees this much more velocity
        }
    speed_ = 1.0f;
}

bool PhysicsSim::Step(const Scene& scene, const std::vector<Mat4>& poseWorld, int steps, std::vector<Mat4>& skin) {
    if (poseWorld.size() != scene.bones.size() || skin.size() != scene.bones.size()) return false;
    // The colliders as the pose carries them: each rides its bone.
    std::vector<WorldCollider> colliders;
    for (const auto& c : scene.colliders) {
        if (c.bone < 0 || static_cast<size_t>(c.bone) >= poseWorld.size() || c.inside || c.radius <= 0.0f) continue;
        const Mat4 m = poseWorld[static_cast<size_t>(c.bone)] * Mat4::TRS(scene.bones[static_cast<size_t>(c.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        const Vec3 centre = m.point(c.center);
        const float half = std::max(0.0f, c.height * 0.5f - c.radius);
        const Vec3 axis = normalize(m.dir(c.axis));
        colliders.push_back({sub(centre, axis * half), centre + axis * half, c.radius});
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
                // gravity as the component has it: what the rest pose already hangs under is left out, the rest of it acts
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
                        const float maxLen = p.restLen * (1.0f - sc.stiffness) * 2.0f;
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

} // namespace cardbridge::assemble
