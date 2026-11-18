#include "assemble/PhysicsSim.h"

#include "assemble/Pose.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace cardbridge::assemble {

namespace {

constexpr float kGravity = 9.81f;
constexpr float kMaxSpeed = 2.0f;   // m/s, a hard lid so a shove or an extreme setting can neither fling a link out of the model nor carry enough
                                    // momentum to generate fresh energy each contact (a bust never exceeds roughly a quarter of this in a fast jump)
// Left/right coordination (the same mechanism as the PMX export's coordination joints, so the viewer shows what MikuMikuDance
// plays): one soft spring between corresponding links of the bust / hip pair, pulling their lags (pos - target) together — the
// fascia of the chest wall tying the two sides. Common motion costs nothing (both lag equally), independent motion rides on it,
// opposite-direction bouncing is damped. Per-link taper toward the tip; the base carries the coordination, the tip stays freest.
constexpr float kCoordSpringShare = 0.25f;   // a share of the hold stiffness, like the PMX's share of the hold spring
constexpr float kCoordTaper = 0.75f;         // per extra link depth from the root

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

Mat4 lerpMatrix(const Mat4& a, const Mat4& b, float t) {
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = a.m[i] + (b.m[i] - a.m[i]) * t;
    return r;
}

// Pushes a particle of radius r out of a capsule (a sphere is a capsule with a zero-length segment).
void collide(Vec3& p, float r, Vec3 a, Vec3 b, float colliderRadius) {
    const Vec3 ab = sub(b, a);
    const float len2 = dot(ab, ab);
    const float t = len2 > 1e-12f ? std::min(1.0f, std::max(0.0f, dot(sub(p, a), ab) / len2)) : 0.0f;
    const Vec3 closest = a + ab * t;
    const Vec3 d = sub(p, closest);
    const float dist = length(d);
    const float want = colliderRadius + r;
    if (dist < want && dist > 1e-9f) p = closest + d * (want / dist);
}

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

} // namespace

void PhysicsSim::Reset(const Scene& scene, const std::vector<Mat4>& poseWorld, bool jiggleBust, bool jiggleButt,
                       const JiggleSettings& jiggle, bool jiggleOnly, bool outfitOnly, bool fallFully) {
    fallFully_ = fallFully;
    particles_.clear();
    chains_.clear();
    jiggles_.clear();
    colliders_.clear();
    genericAccum_ = 0.0;
    jiggleAccum_ = 0.0;
    speed_ = 0.0f;
    jiggle_ = SanitizeJiggleSettings(jiggle);
    if (poseWorld.size() != scene.bones.size()) return;
    const bool jiggleOff = jiggle_.strength <= 0.0f;
    for (size_t ci = 0; ci < scene.chains.size(); ++ci) {
        const SceneChain& sc = scene.chains[ci];
        if (jiggleOnly && !sc.characterOwn) continue;   // the static settle moves the body's jiggle only; hair keeps its authored pose
        if (outfitOnly && sc.part.rfind("outfit", 0) != 0) continue;   // the outfit settle moves the garments only
        if (sc.bones.size() < 2) continue;
        bool jiggleChain = false;
        if (sc.characterOwn) {   // the body's own jiggle: skipped while its switch is off, like the PMX export
            jiggleChain = IsBustJiggleChain(scene, sc) ? jiggleBust : jiggleButt;
            if (!jiggleChain) continue;
        }
        if (jiggleChain) {
            if (jiggleOff) continue;   // global strength 0: rigid, exactly as the PMX export does
            JiggleChain jc;
            jc.scene = ci;
            jc.bust = IsBustJiggleChain(scene, sc);
            std::map<int, int> byBone;
            for (size_t k = 0; k < sc.bones.size(); ++k) {
                const int b = sc.bones[k];
                if (b < 0 || static_cast<size_t>(b) >= scene.bones.size()) continue;
                JiggleLink l;
                l.bone = b;
                const int parentBone = scene.bones[static_cast<size_t>(b)].parent;
                l.parentBone = parentBone;
                if (k > 0) {
                    auto it = byBone.find(parentBone);
                    l.parent = it != byBone.end() ? it->second : static_cast<int>(jc.links.size()) - 1;
                    if (l.parent >= 0) jc.links[static_cast<size_t>(l.parent)].child = static_cast<int>(jc.links.size());
                }
                if (jc.sideSign == 0) jc.sideSign = BoneSideSign(scene.bones[static_cast<size_t>(b)].name);
                l.mass = std::max(0.01f, jiggle_.mass * std::pow(0.5f, static_cast<float>(k)));
                const Vec3 posed = poseWorld[static_cast<size_t>(b)].translation();
                l.pos = l.target = posed;
                // The length this link keeps to its parent: the posed bone distance, so a scaled / posed rig preserves its shape.
                if (l.parent >= 0) {
                    l.restLen = length(sub(posed, jc.links[static_cast<size_t>(l.parent)].pos));
                } else if (parentBone >= 0 && static_cast<size_t>(parentBone) < poseWorld.size()) {
                    l.restLen = length(sub(posed, poseWorld[static_cast<size_t>(parentBone)].translation()));
                }
                l.world = l.prevWorld = poseWorld[static_cast<size_t>(b)];
                byBone[b] = static_cast<int>(jc.links.size());
                jc.links.push_back(l);
            }
            if (jc.links.size() < 2) continue;
            // The rest offset to the first chain child, for the bone's aim; the chain order is the animation order.
            for (JiggleLink& l : jc.links) {
                if (l.child < 0) continue;
                const int childBone = jc.links[static_cast<size_t>(l.child)].bone;
                l.toChild = sub(scene.bones[static_cast<size_t>(childBone)].pos, scene.bones[static_cast<size_t>(l.bone)].pos);
            }
            jc.lastTargets.reserve(jc.links.size());
            for (const JiggleLink& l : jc.links) jc.lastTargets.push_back(l.target);
            jiggles_.push_back(std::move(jc));
            continue;
        }
        Chain chain;
        chain.scene = ci;
        chain.envelopeScale = 1.0f;
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
    // A chain whose links start inside a collider is never pushed out of it at rest — the rest pose is how the character is authored;
    // physics must not fight its own skin (that pushed the butt sideways and buzzed the spring every substep). Remember those colliders
    // per chain and skip them in the contact response; the response against every other object is unchanged. Rebuilt against the same
    // colliders Step uses on every reset (i.e. every pose change).
    CollectColliders(scene, poseWorld);
    // Pair the left and right chains of each class (bust with bust, hip with hip) for the coordination spring in StepJiggle — the same
    // mechanism as the PMX export's coordination joints, so the viewer shows the same coherence MikuMikuDance plays. A chain with no
    // counterpart on the other side keeps no pair.
    jigglePairs_.clear();
    for (size_t i = 0; i < jiggles_.size(); ++i) {
        if (jiggles_[i].sideSign <= 0) continue;   // a centre chain has no side; the other side keys off it below
        for (size_t j = i + 1; j < jiggles_.size(); ++j)
            if (jiggles_[j].bust == jiggles_[i].bust && jiggles_[j].sideSign < 0) { jigglePairs_.push_back({static_cast<int>(i), static_cast<int>(j)}); break; }
    }
    for (JiggleChain& jc : jiggles_) {
        const SceneChain& sc = scene.chains[jc.scene];
        const float radius = std::max(0.004f, sc.radius);
        jc.skipColliders.assign(colliders_.size(), 0);
        for (size_t ci = 0; ci < colliders_.size(); ++ci) {
            const WorldCollider& c = colliders_[ci];
            for (const JiggleLink& l : jc.links) {
                const Vec3 ab = {c.b.x - c.a.x, c.b.y - c.a.y, c.b.z - c.a.z};
                const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
                float t = 0.0f;
                if (len2 > 1e-12f) t = std::max(0.0f, std::min(1.0f, ((l.pos.x - c.a.x) * ab.x + (l.pos.y - c.a.y) * ab.y + (l.pos.z - c.a.z) * ab.z) / len2));
                const Vec3 q = {c.a.x + ab.x * t, c.a.y + ab.y * t, c.a.z + ab.z * t};
                const Vec3 d = {l.pos.x - q.x, l.pos.y - q.y, l.pos.z - q.z};
                if (d.x * d.x + d.y * d.y + d.z * d.z < (c.radius + radius) * (c.radius + radius)) { jc.skipColliders[ci] = 1; break; }
            }
        }
    }
}

size_t PhysicsSim::particleCount() const {
    size_t n = particles_.size();
    for (const JiggleChain& jc : jiggles_) n += jc.links.size();
    return n;
}

size_t PhysicsSim::jiggleParticleCount() const {
    size_t n = 0;
    for (const JiggleChain& jc : jiggles_) n += jc.links.size();
    return n;
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
    // The jiggle links are velocity states already: the old impulse was model units per 1/60 update, so convert it to m/s.
    Vec3 hit = impulse * (60.0f * 2.5f);
    const float hl = length(hit);
    if (hl > kMaxSpeed) hit = hit * (kMaxSpeed / hl);
    for (JiggleChain& jc : jiggles_) {
        for (JiggleLink& l : jc.links) l.vel = l.vel + hit;
        jc.asleep = false;
        jc.stillSeconds = 0.0f;
    }
    speed_ = 1.0f;
}

void PhysicsSim::CollectColliders(const Scene& scene, const std::vector<Mat4>& poseWorld) {
    colliders_.clear();
    // The colliders as the pose carries them: each rides its bone. The body's own hit spheres (breast / butt / hip hits) are tagged:
    // the character's own jiggle chains start inside them, so the jiggle must ignore them (it collides with the arms, hands and
    // shins instead, and with every other hit sphere).
    for (const auto& c : scene.colliders) {
        if (c.bone < 0 || static_cast<size_t>(c.bone) >= poseWorld.size() || c.inside || c.radius <= 0.0f) continue;
        const std::string& boneName = scene.bones[static_cast<size_t>(c.bone)].name;
        const bool bodyOwn = boneName.find("bust") != std::string::npos || boneName.find("siri") != std::string::npos ||
                             boneName.find("kosi") != std::string::npos || boneName.find("mune") != std::string::npos ||
                             boneName.find("waist") != std::string::npos;
        const Mat4 m = poseWorld[static_cast<size_t>(c.bone)] * Mat4::TRS(scene.bones[static_cast<size_t>(c.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        const Vec3 centre = m.point(c.center);
        const float half = std::max(0.0f, c.height * 0.5f - c.radius);
        const Vec3 axis = normalize(m.dir(c.axis));
        colliders_.push_back({sub(centre, axis * half), centre + axis * half, c.radius, bodyOwn});
    }
    // The arms are not in the game's collider list, but the breast clipping into them is the classic break:
    // add an upper-arm + forearm capsule per arm so the jiggle particles slide off the arm instead of through it.
    // Hand capsules (forearm -> hand) and shin capsules (knee -> ankle) close the other two universal gaps.
    for (const char* side : {"_L", "_R"}) {
        const int shoulder = scene.boneIndex(std::string("cf_j_shoulder") + side);
        const int arm = scene.boneIndex(std::string("cf_j_arm00") + side);
        const int forearm = scene.boneIndex(std::string("cf_j_forearm01") + side);
        const int hand = scene.boneIndex(std::string("cf_j_hand_") + side);
        const int knee = scene.boneIndex(std::string("cf_j_leg01_") + side);
        const int leg03 = scene.boneIndex(std::string("cf_j_leg03_") + side);
        auto addArmSeg = [&](int a, int b, float r) {
            if (a < 0 || b < 0 || static_cast<size_t>(a) >= poseWorld.size() || static_cast<size_t>(b) >= poseWorld.size()) return;
            colliders_.push_back({poseWorld[static_cast<size_t>(a)].translation(), poseWorld[static_cast<size_t>(b)].translation(), r});
        };
        addArmSeg(shoulder, arm, 0.040f);
        addArmSeg(arm, forearm, 0.032f);
        addArmSeg(forearm, hand, 0.030f);
        addArmSeg(knee, leg03, 0.038f);
    }
}

bool PhysicsSim::StepGeneric(const Scene& scene, const std::vector<Mat4>& poseWorld, float& moved) {
    for (Chain& chain : chains_) {
        const SceneChain& sc = scene.chains[chain.scene];
        Particle& anchor = particles_[static_cast<size_t>(chain.particles[0])];
        const Mat4 anchorWorld = poseWorld[static_cast<size_t>(anchor.bone)];
        const Vec3 anchorPos = anchorWorld.translation();
        anchor.world = anchorWorld;
        const Vec3 objectMove = sub(anchorPos, chain.anchorPrev);
        chain.anchorPrev = anchorPos;
        // gravity as the component has it: what the rest pose already hangs under is left out, the rest of it acts.
        // (The garment hang bake sets fallFully_: MikuMikuDance's world gravity applies in full at load, so the whole thing acts.)
        Vec3 force = sc.gravity;
        const float gl = fallFully_ ? 0.0f : length(sc.gravity);
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
            const Vec3 restPos = par.pos + rotationPart(poseWorld[static_cast<size_t>(par.bone)]).dir(p.restLocal);
            Vec3 d = sub(restPos, p.pos);
            p.pos = p.pos + d * sc.elasticity;
            if (sc.stiffness > 0.0f) {
                d = sub(restPos, p.pos);
                const float len = length(d);
                const float maxLen = p.restLen * (1.0f - sc.stiffness) * 2.0f * chain.envelopeScale;
                if (len > maxLen && len > 1e-9f) p.pos = p.pos + d * ((len - maxLen) / len);
            }
            for (const auto& c : colliders_) collide(p.pos, sc.radius, c.a, c.b, c.radius);
            const Vec3 dd = sub(par.pos, p.pos);
            const float dl = length(dd);
            if (dl > 1e-9f) p.pos = p.pos + dd * ((dl - p.restLen) / dl);
            moved = std::max(moved, length(sub(p.pos, p.prev)));
        }
    }
    return moved > 0.0f;
}

void PhysicsSim::UpdateJiggleTargets(const Scene& scene, const std::vector<Mat4>& poseWorld, bool& woke) {
    for (JiggleChain& jc : jiggles_) {
        bool moved = false;
        for (size_t k = 0; k < jc.links.size(); ++k) {
            JiggleLink& l = jc.links[k];
            const Vec3 target = poseWorld[static_cast<size_t>(l.bone)].translation();
            l.target = target;
            const Vec3 d = sub(target, jc.lastTargets[k]);
            if (dot(d, d) > 1e-10f) moved = true;
            jc.lastTargets[k] = target;
        }
        if (moved && jc.asleep) {
            jc.asleep = false;
            jc.stillSeconds = 0.0f;
            woke = true;
        }
    }
}

void PhysicsSim::UpdateJiggleWorlds(const Scene& scene, const std::vector<Mat4>& poseWorld) {
    for (JiggleChain& jc : jiggles_) {
        if (jc.asleep) continue;
        for (size_t k = 0; k < jc.links.size(); ++k) {
            JiggleLink& l = jc.links[k];
            // Root: the pose's own orientation (this keeps a static sag or a bone edit); children: the simulated parent's turn.
            Mat4 base = k == 0 ? rotationPart(poseWorld[static_cast<size_t>(l.bone)])
                               : rotationPart(jc.links[static_cast<size_t>(l.parent)].world);
            Mat4 rot = base;
            if (l.child >= 0) {
                const Vec3 have = base.dir(l.toChild);
                const Vec3 want = sub(jc.links[static_cast<size_t>(l.child)].pos, l.pos);
                const float hl = length(have), wl = length(want);
                if (hl > 1e-8f && wl > 1e-8f) {
                    const float t = clamp01(jiggle_.rotationStrength);
                    Vec3 mixed = have * (1.0f - t) + want * t;
                    if (length(mixed) > 1e-8f) rot = RotationBetween(have, mixed) * base;
                }
            }
            l.world = translated(rot, l.pos);
        }
    }
}

void PhysicsSim::StepJiggle(const Scene& scene, const std::vector<Mat4>& poseWorld, float dt, float& moved) {
    if (jiggles_.empty() || dt <= 0.0f) return;
    const float gravity = jiggle_.gravity * kGravity;
    // The world each link had at the last substep boundary, for the render interpolation.
    for (JiggleChain& jc : jiggles_)
        if (!jc.asleep)
            for (JiggleLink& l : jc.links) l.prevWorld = l.world;
    // Coordination across pairs (the fascia between the two sides): a soft spring pulling each link's lag toward its pair's, computed
    // from this substep's lag snapshot so the force is symmetric between the two chains. A non-negligible pull wakes a sleeping pair.
    std::vector<std::vector<Vec3>> pairAccel(jiggles_.size());
    auto lagOf = [](const JiggleLink& l) { return sub(l.pos, l.target); };
    for (const auto& [ai, bi] : jigglePairs_) {
        const JiggleChain& A = jiggles_[static_cast<size_t>(ai)];
        const JiggleChain& B = jiggles_[static_cast<size_t>(bi)];
        const size_t n = std::min(A.links.size(), B.links.size());
        float maxPull = 0.0f;
        std::vector<Vec3> pullA(n), pullB(n);
        for (size_t k = 0; k < n; ++k) {
            const float taper = std::pow(kCoordTaper, static_cast<float>(k));
            const float share = kCoordSpringShare * taper;
            const Vec3 lagA = lagOf(A.links[k]), lagB = lagOf(B.links[k]);
            const Vec3 pull = sub(lagA, lagB) * share;   // the lag difference: the fascia pulls the pair toward a common lag, symmetric motion costs nothing
            pullA[k] = pull * (jiggle_.stiffness / std::max(0.01f, A.links[k].mass));
            pullB[k] = pull * (-jiggle_.stiffness / std::max(0.01f, B.links[k].mass));
            maxPull = std::max(maxPull, dot(pull, pull));
        }
        if (maxPull > 1e-12) {   // the pair diverges: both wake (a one-sided edit must pull the sibling, like the PMX does)
            jiggles_[static_cast<size_t>(ai)].asleep = false;
            jiggles_[static_cast<size_t>(bi)].asleep = false;
            jiggles_[static_cast<size_t>(ai)].stillSeconds = 0.0f;
            jiggles_[static_cast<size_t>(bi)].stillSeconds = 0.0f;
        }
        pairAccel[static_cast<size_t>(ai)] = std::move(pullA);
        pairAccel[static_cast<size_t>(bi)] = std::move(pullB);
    }
    for (size_t ci = 0; ci < jiggles_.size(); ++ci) {
        JiggleChain& jc = jiggles_[ci];
        if (jc.asleep) continue;
        const SceneChain& sc = scene.chains[jc.scene];
        const float stiffness = jiggle_.stiffness;
        const float damping = jiggle_.damping;
        const float amplitudeScale = 1.0f;
        const float rootMass = std::max(0.01f, jc.links[0].mass);
        const float radius = std::max(0.004f, sc.radius);
        const float limitX = JiggleMaxDisplacement(jiggle_, 0) * amplitudeScale;
        const float limitY = JiggleMaxDisplacement(jiggle_, 1) * amplitudeScale;
        const float limitZ = JiggleMaxDisplacement(jiggle_, 2) * amplitudeScale;

        // Semi-implicit Euler: stable at every rate the sanitizer allows once the spring and damper are capped below the
        // fixed step's stability limit (an extreme stiffness at 30 Hz would otherwise ring itself apart).
        for (size_t lk = 0; lk < jc.links.size(); ++lk) {
            JiggleLink& l = jc.links[lk];
            const float stableK = 0.8f * l.mass / (dt * dt);
            const float k = std::min(stiffness, stableK);
            const float dampingC = std::min(damping * (l.mass / rootMass), 0.8f * l.mass / dt);
            Vec3 accel = sub(l.target, l.pos) * (k / l.mass);
            accel.y -= (1.0f - kJiggleSupport) * gravity;
            accel = accel + l.vel * (-dampingC / l.mass);
            if (ci < pairAccel.size() && lk < pairAccel[ci].size()) accel = accel + pairAccel[ci][lk];
            l.vel = l.vel + accel * dt;
            const float speed = length(l.vel);
            if (speed > kMaxSpeed) l.vel = l.vel * (kMaxSpeed / speed);
            l.pos = l.pos + l.vel * dt;
        }
        // The visible envelope: how far a link may travel from its posed place on each world axis. A zero amplitude pins it. A link whose
        // travel was just clamped loses that axis of motion too - otherwise the clamp absorbs the speed and the link pumps against the wall forever.
        for (JiggleLink& l : jc.links) {
            auto clampAxis = [&l](float& value, float target, float limit, float& axisVel) {
                if (limit <= 0.0f) { value = target; axisVel = 0.0f; return; }
                if (value - target > limit) { value = target + limit; if (axisVel > 0.0f) axisVel = 0.0f; }
                else if (value - target < -limit) { value = target - limit; if (axisVel < 0.0f) axisVel = 0.0f; }
            };
            clampAxis(l.pos.x, l.target.x, limitX, l.vel.x);
            clampAxis(l.pos.y, l.target.y, limitY, l.vel.y);
            clampAxis(l.pos.z, l.target.z, limitZ, l.vel.z);
        }
        // The links collide with the arm and hand capsules (the breast slides over them instead of through them) and with the body's other
        // hit spheres. The colliders the chain starts inside at rest (its own skin, this card's cf_hit_*) are skipped per link chain —
        // colliding with its own rest pose would push the mesh every substep. A link that was pushed out of a collider loses the velocity
        // component into its surface completely (inelastic): no energy comes back as a bounce, the slide along the surface is free.
        auto collideJiggle = [&](JiggleLink& l, const std::vector<char>& skip) {
            for (size_t ci = 0; ci < colliders_.size(); ++ci) {
                const WorldCollider& c = colliders_[ci];
                if (c.bodyOwn || (ci < skip.size() && skip[ci])) continue;
                const Vec3 before = l.pos;
                collide(l.pos, radius, c.a, c.b, c.radius);
                const Vec3 pushed = sub(l.pos, before);
                const float pl = length(pushed);
                if (pl > 1e-9f) {
                    const Vec3 n = pushed * (1.0f / pl);
                    const float vn = l.vel.x * n.x + l.vel.y * n.y + l.vel.z * n.z;
                    // Kill the into-surface component completely: the contact is inelastic, so no energy comes back as a bounce. The slide
                    // along the surface is free (the breast slides over an arm), kept damped by the spring elsewhere.
                    l.vel = sub(l.vel, n * (vn < 0.0f ? vn : 0.0f));
                }
            }
        };
        for (JiggleLink& l : jc.links) collideJiggle(l, jc.skipColliders);
        // Keep every link on its rod, root to tip. The rod is a bilateral distance constraint, so a link whose position was just snapped
        // to the rod loses its radial velocity too - otherwise gravity pumps the link against the rod into a limit cycle that never settles.
        for (JiggleLink& l : jc.links) {
            Vec3 parentPos;
            bool hasParent = false;
            if (l.parent >= 0) {
                parentPos = jc.links[static_cast<size_t>(l.parent)].pos;
                hasParent = true;
            } else if (l.parentBone >= 0 && static_cast<size_t>(l.parentBone) < poseWorld.size()) {
                parentPos = poseWorld[static_cast<size_t>(l.parentBone)].translation();
                hasParent = true;
            }
            if (!hasParent) continue;
            const Vec3 d = sub(l.pos, parentPos);
            const float dl = length(d);
            if (dl > 1e-9f && l.restLen > 1e-6f) {
                const Vec3 n = d * (1.0f / dl);
                l.pos = parentPos + n * l.restLen;
                const float vr = l.vel.x * n.x + l.vel.y * n.y + l.vel.z * n.z;
                l.vel = sub(l.vel, n * vr);
            }
        }
        for (JiggleLink& l : jc.links) collideJiggle(l, jc.skipColliders);
    }
    UpdateJiggleWorlds(scene, poseWorld);
    for (JiggleChain& jc : jiggles_) {
        if (jc.asleep) continue;
        float maxSpeed = 0.0f;
        for (const JiggleLink& l : jc.links) {
            maxSpeed = std::max(maxSpeed, length(l.vel));
            moved = std::max(moved, length(l.vel));
        }
        if (maxSpeed < 0.002f) {
            jc.stillSeconds += dt;
            if (jc.stillSeconds > 0.4f) {
                jc.asleep = true;
                for (JiggleLink& l : jc.links) l.vel = Vec3{0, 0, 0};
            }
        } else {
            jc.stillSeconds = 0.0f;
        }
        if (std::getenv("CARDBRIDGE_DEBUG_JIGGLE")) {
            for (const JiggleLink& l : jc.links)
                std::fprintf(stderr, "jiggle substep: bone=%d pos(%.5f %.5f %.5f) vel(%.6f %.6f %.6f) maxSpeed=%.6f asleep=%d still=%.2f\n",
                             l.bone, l.pos.x, l.pos.y, l.pos.z, l.vel.x, l.vel.y, l.vel.z, maxSpeed, jc.asleep ? 1 : 0, jc.stillSeconds);
        }
    }
}

void PhysicsSim::WriteOutput(const Scene& scene, const std::vector<Mat4>& poseWorld, std::vector<Mat4>& skin, bool interpolate, float alpha) {
    for (const Chain& chain : chains_) {
        Particle& anchor = particles_[static_cast<size_t>(chain.particles[0])];
        anchor.world = poseWorld[static_cast<size_t>(anchor.bone)];
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
        for (size_t k = 1; k < chain.particles.size(); ++k) {
            const Particle& p = particles_[static_cast<size_t>(chain.particles[k])];
            skin[static_cast<size_t>(p.bone)] = p.world * Mat4::TRS(scene.bones[static_cast<size_t>(p.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        }
    }
    for (const JiggleChain& jc : jiggles_) {
        for (const JiggleLink& l : jc.links) {
            Mat4 world = l.world;
            if (interpolate) world = lerpMatrix(l.prevWorld, l.world, clamp01(alpha));
            skin[static_cast<size_t>(l.bone)] = world * Mat4::TRS(scene.bones[static_cast<size_t>(l.bone)].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        }
    }
}

bool PhysicsSim::AdvanceInternal(const Scene& scene, const std::vector<Mat4>& poseWorld, double seconds, std::vector<Mat4>& skin, bool interpolate) {
    if (poseWorld.size() != scene.bones.size() || skin.size() != scene.bones.size()) return false;
    const double sec = std::min(0.25, std::max(0.0, seconds));
    if (sec > 0.0) CollectColliders(scene, poseWorld);
    bool woke = false;
    UpdateJiggleTargets(scene, poseWorld, woke);
    float moved = 0.0f;
    bool stepped = false;
    const double fixed = 1.0 / 60.0;
    genericAccum_ += sec;
    for (int guard = 0; genericAccum_ >= fixed - 1e-9 && guard < 32; ++guard) {
        StepGeneric(scene, poseWorld, moved);
        genericAccum_ -= fixed;
        stepped = true;
    }
    if (genericAccum_ < 0.0) genericAccum_ = 0.0;
    const double jiggleStep = 1.0 / std::max(30.0, static_cast<double>(jiggle_.simulationHz));
    jiggleAccum_ += sec;
    for (int guard = 0; jiggleAccum_ >= jiggleStep - 1e-9 && guard < 64; ++guard) {
        StepJiggle(scene, poseWorld, static_cast<float>(jiggleStep), moved);
        jiggleAccum_ -= jiggleStep;
        stepped = true;
    }
    if (jiggleAccum_ < 0.0) jiggleAccum_ = 0.0;
    const float alpha = static_cast<float>(jiggleAccum_ / jiggleStep);
    WriteOutput(scene, poseWorld, skin, interpolate, alpha);
    if (stepped) speed_ = moved;
    return speed_ > 1e-5f;
}

bool PhysicsSim::Step(const Scene& scene, const std::vector<Mat4>& poseWorld, int steps, std::vector<Mat4>& skin) {
    const double seconds = static_cast<double>(std::max(0, steps)) / 60.0;
    return AdvanceInternal(scene, poseWorld, seconds, skin, false);
}

bool PhysicsSim::Advance(const Scene& scene, const std::vector<Mat4>& poseWorld, double seconds, std::vector<Mat4>& skin) {
    return AdvanceInternal(scene, poseWorld, seconds, skin, true);
}

} // namespace cardbridge::assemble
