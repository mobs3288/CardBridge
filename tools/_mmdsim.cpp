// Offline replica of MMD's physics for an exported PMX, built on Bullet (the same engine MMD uses):
//   gravity 0,-98,0; fixed 1/120 s steps (saba's default, MikuMikuDance's rate); btGeneric6DofSpringConstraint
//   exactly as saba's MMDJoint::CreateJoint does (including its sign flip on the Z rotation spring);
//   kinematic bodies (mode 0) fixed at their rest places; dynamic bodies (mode 1/2) straight Bullet dynamics;
//   DISABLE_DEACTIVATION like MMD. No ground plane, bones only (physics only).
//   usage: mmdsim <file.pmx> [--seconds N] [--report-every S] [--shift X,Y,Z after T] [--chain F]
// Prints, per chain (body name minus the trailing " <n>"): start/end y and displacement of each dynamic body, and
// the maximum speed of any dynamic body over the last second (the "buzz" meter) plus the peak speed overall.
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"

#include <btBulletDynamicsCommon.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
using namespace cardbridge;
using namespace pmxread;

struct MotionState : public btMotionState {
    bool dynamic = false;
    btTransform t0;
    void getWorldTransform(btTransform& out) const override { out = t0; }
    void setWorldTransform(const btTransform& t) override { /* dynamic: unused (MMD writes the bone, not the body) */ }
};

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main(int argc, char** argv) {
    if (std::getenv("MMDSIM_TRACE")) std::fprintf(stderr, "start\n");
    if (argc < 2) { std::fprintf(stderr, "usage: mmdsim <file.pmx> [--seconds N] [--shift Sx,Sy,Sz,startT,durT] [--chain F] [--summary] [--limb-all|--limb-deep] [--nocollide]\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    double seconds = 5.0;
    double shiftX = 0, shiftY = 0, shiftZ = 0, shiftAt = -1, shiftDur = 0.25;
    const char* chainFilter = nullptr;
    double springScale = 1.0, springAbs = -1.0, dampAbs = -1.0, limitDeg = -1.0, gravY = -98.0;
    bool summaryOnly = false;
    int limbExclude = 0;   // 0 = as written in the file; 1 = drop limb bit for bodies overlapping a limb collider (rest-overlap rule); 2 = only for bodies whose centre is inside a limb collider
    bool noColliders = false;   // drop every collider (every body with a group other than 1): for isolating joint vs collider problems
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--chain") && i + 1 < argc) chainFilter = argv[++i];
        else if (!strcmp(argv[i], "--limb-all")) limbExclude = 1;
        else if (!strcmp(argv[i], "--limb-deep")) limbExclude = 2;
        else if (!strcmp(argv[i], "--nocollide")) noColliders = true;
        else if (!strcmp(argv[i], "--summary")) summaryOnly = true;
        else if (!strcmp(argv[i], "--spring-scale") && i + 1 < argc) springScale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--spring-abs") && i + 1 < argc) springAbs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--damp") && i + 1 < argc) dampAbs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--limit-deg") && i + 1 < argc) limitDeg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gravity") && i + 1 < argc) gravY = atof(argv[++i]);
        else if (!strcmp(argv[i], "--shift") && i + 1 < argc) {
            char buf[128]; std::snprintf(buf, sizeof buf, "%s", argv[++i]);
            if (std::sscanf(buf, "%lf,%lf,%lf,%lf,%lf", &shiftX, &shiftY, &shiftZ, &shiftAt, &shiftDur) < 4) shiftAt = -1;
        }
    }
    // Optionally re-apply rest-overlap filtering to the generated limb colliders (group 15), as the game's colliders are filtered.
    if (limbExclude == 1 || limbExclude == 2) {
        auto segOf = [&](size_t i, btVector3& a, btVector3& b, float& r) {
            const RigidBody& rb = m.bodies[i];
            r = rb.size.x;
            if (rb.shape == 2) {
                btMatrix3x3 rot; rot.setEulerZYX(rb.rot.x, rb.rot.y, rb.rot.z);
                const btVector3 axis = rot * btVector3(0, 1, 0);
                const float half = std::max(0.0f, rb.size.y * 0.5f);
                a = btVector3(rb.pos.x, rb.pos.y, rb.pos.z) - axis * half;
                b = btVector3(rb.pos.x, rb.pos.y, rb.pos.z) + axis * half;
            } else {
                a = b = btVector3(rb.pos.x, rb.pos.y, rb.pos.z);
            }
        };
        auto distPointSeg = [](const btVector3& p, const btVector3& a, const btVector3& b) {
            const btVector3 ab = b - a;
            const float l2 = ab.length2();
            float t = l2 > 1e-12f ? btClamped((p - a).dot(ab) / l2, 0.0f, 1.0f) : 0.0f;
            return (p - (a + ab * t)).length();
        };
        auto distSegSeg = [](const btVector3& p1, const btVector3& q1, const btVector3& p2, const btVector3& q2) {
            const btVector3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
            const float a = d1.length2(), e = d2.length2(), f = d2.dot(r);
            const float c = d1.dot(r), b = d1.dot(d2);
            const float denom = a * e - b * b;
            float s = denom > 1e-12f ? btClamped((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            float t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = btClamped(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = btClamped((b - c) / a, 0.0f, 1.0f); }
            const btVector3 diff = (p1 + d1 * s) - (p2 + d2 * t);
            return diff.length();
        };
        std::vector<std::pair<btVector3, btVector3>> limbs;
        std::vector<float> limbR;
        for (size_t i = 0; i < m.bodies.size(); ++i) if (m.bodies[i].group == 15) {
            btVector3 a, b; float r;
            segOf(i, a, b, r);
            limbs.push_back({a, b});
            limbR.push_back(r);
        }
        for (size_t i = 0; i < m.bodies.size(); ++i) {
            if (m.bodies[i].group != 1 || m.bodies[i].mode == 0) continue;   // dynamic chain bodies only
            btVector3 ca, cb; float cr;
            segOf(i, ca, cb, cr);
            for (size_t li = 0; li < limbs.size(); ++li) {
                if (limbExclude == 1) {
                    if (distSegSeg(limbs[li].first, limbs[li].second, ca, cb) < limbR[li] + cr + 0.02f * 12.5f) { m.bodies[i].mask = m.bodies[i].mask & ~(1 << 15); break; }
                } else {
                    if (distPointSeg((ca + cb) * 0.5f, limbs[li].first, limbs[li].second) < limbR[li]) { m.bodies[i].mask = m.bodies[i].mask & ~(1 << 15); break; }
                }
            }
        }
    }

    btDiscreteDynamicsWorld world(new btCollisionDispatcher(new btDefaultCollisionConfiguration), new btDbvtBroadphase, new btSequentialImpulseConstraintSolver, new btDefaultCollisionConfiguration);
    // saba's config: sequential impulse solver, 120 Hz, gravity (1 unit = 10 cm in MMD)
    world.setGravity(btVector3(0, (float)gravY, 0));

    struct BodyEntry { btRigidBody* rb = nullptr; MotionState* ms = nullptr; int index = -1; btVector3 start; };
    std::vector<BodyEntry> entries(m.bodies.size());
    // The kinematic bodies anchor the chains: at rest they hold their rest places. A --shift moves them (a head-turn / step sideways).
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        if (noColliders && (m.bodies[i].name.rfind("collider ", 0) == 0 || m.bodies[i].name.rfind("limb collider ", 0) == 0)) continue;   // every collider is dropped for this run
        const RigidBody& b = m.bodies[i];
        btCollisionShape* shape = nullptr;
        if (b.shape == 0) shape = new btSphereShape(b.size.x);
        else if (b.shape == 1) shape = new btBoxShape(btVector3(b.size.x, b.size.y, b.size.z));
        else shape = new btCapsuleShape(b.size.x, b.size.y);
        const bool dynamic = b.mode != 0;
        const btScalar mass = dynamic ? btScalar(b.mass) : btScalar(0);
        btVector3 inertia(0, 0, 0);
        if (mass > 0) shape->calculateLocalInertia(mass, inertia);
        MotionState* ms = new MotionState;
        ms->dynamic = dynamic;
        btTransform tf;
        tf.setIdentity();
        btMatrix3x3 rot;
        rot.setEulerZYX(b.rot.x, b.rot.y, b.rot.z);
        tf.setBasis(rot);
        tf.setOrigin(btVector3(b.pos.x, b.pos.y, b.pos.z));
        ms->t0 = tf;
        entries[i].ms = ms;
        entries[i].index = (int)i;
        entries[i].start = btVector3(b.pos.x, b.pos.y, b.pos.z);
        btRigidBody::btRigidBodyConstructionInfo info(mass, ms, shape, inertia);
        info.m_linearDamping = dampAbs >= 0 ? dampAbs : b.moveDamping;
        info.m_angularDamping = dampAbs >= 0 ? dampAbs : b.rotDamping;
        info.m_restitution = b.restitution;
        info.m_friction = b.friction;
        info.m_additionalDamping = true;   // like MMD/saba
        entries[i].rb = new btRigidBody(info);
        entries[i].rb->setUserPointer((void*)(intptr_t)i);
        entries[i].rb->setSleepingThresholds(0.01f, 0.1f * (float)(3.14159265 / 180.0));   // saba sets angular threshold 0.1 rad
        entries[i].rb->setActivationState(DISABLE_DEACTIVATION);
        world.addRigidBody(entries[i].rb, 1 << b.group, b.mask);
        if (!dynamic) entries[i].rb->setCollisionFlags(entries[i].rb->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        if (std::getenv("MMDSIM_TRACE")) std::fprintf(stderr, "body %zu ok\n", i);
    }
    size_t ji = 0;
    for (const Joint& j : m.joints) {
        if (std::getenv("MMDSIM_TRACE")) std::fprintf(stderr, "joint %zu a=%zu b=%zu\n", ji, (size_t)j.a, (size_t)j.b);
        ++ji;
        btMatrix3x3 rotA; rotA.setEulerZYX(m.bodies[j.a].rot.x, m.bodies[j.a].rot.y, m.bodies[j.a].rot.z);
        btTransform ta; ta.setIdentity(); ta.setBasis(rotA); ta.setOrigin(btVector3(m.bodies[j.a].pos.x, m.bodies[j.a].pos.y, m.bodies[j.a].pos.z));
        btMatrix3x3 rotB; rotB.setEulerZYX(m.bodies[j.b].rot.x, m.bodies[j.b].rot.y, m.bodies[j.b].rot.z);
        btTransform tb; tb.setIdentity(); tb.setBasis(rotB); tb.setOrigin(btVector3(m.bodies[j.b].pos.x, m.bodies[j.b].pos.y, m.bodies[j.b].pos.z));
        btTransform jointTf; jointTf.setIdentity();
        jointTf.setOrigin(btVector3(j.pos.x, j.pos.y, j.pos.z));
        btMatrix3x3 jrot; jrot.setEulerZYX(j.rot.x, j.rot.y, j.rot.z);
        jointTf.setBasis(jrot);
        btTransform invA = ta.inverse() * jointTf;
        btTransform invB = tb.inverse() * jointTf;
        auto* c = new btGeneric6DofSpringConstraint(*entries[j.a].rb, *entries[j.b].rb, invA, invB, true);
        c->setLinearLowerLimit(btVector3(j.moveLower.x, j.moveLower.y, j.moveLower.z));
        c->setLinearUpperLimit(btVector3(j.moveUpper.x, j.moveUpper.y, j.moveUpper.z));
        c->setAngularLowerLimit(btVector3(j.rotLower.x, j.rotLower.y, j.rotLower.z));
        c->setAngularUpperLimit(btVector3(j.rotUpper.x, j.rotUpper.y, j.rotUpper.z));
        if (limitDeg >= 0) {
            const double rad = limitDeg * 3.14159265358979323846 / 180.0;
            const double yrad = rad * 0.1;
            c->setAngularLowerLimit(btVector3(-rad, -yrad, -rad));
            c->setAngularUpperLimit(btVector3(rad, yrad, rad));
        }
        // saba's enableSpring/setStiffness (including its sign flip on the Z angular spring, index 5)
        const btScalar sp[6] = {j.springMove.x, j.springMove.y, j.springMove.z, j.springRot.x, j.springRot.y, j.springRot.z};
        for (int axis = 0; axis < 6; ++axis) {
            btScalar k = sp[axis];
            if (axis >= 3 && (springAbs >= 0 || springScale != 1.0)) k = (springAbs >= 0 ? (btScalar)springAbs : k) * (btScalar)springScale;
            if (axis == 2 || axis == 5) k = -k;
            if (sp[axis] != 0 || (axis >= 3 && springAbs >= 0)) { c->enableSpring(axis, true); c->setStiffness(axis, k); }
        }
        world.addConstraint(c);
    }

    // group/mask: bullet's addRigidBody uses (group, mask) as (collisionFilterGroup, collisionFilterMask); saba passes group, mask the same way.
    const double dt = 1.0 / 120.0;
    const int totalSteps = (int)(seconds / dt + 0.5);
    // report schedule: snapshot displacement at a few times
    const double reportTimes[] = {0.0, 0.25, 0.5, 1.0, 2.0, 4.0, 6.0, seconds};
    std::vector<std::vector<btVector3>> snaps(m.bodies.size());
    int nextReport = 0;
    const double shiftStart = shiftAt, shiftEnd = shiftAt >= 0 ? shiftAt + shiftDur : -1;
    btVector3 shift((float)shiftX, (float)shiftY, (float)shiftZ);
    double peakSpeed = 0.0;
    std::map<std::string, double> peakByChain;
    std::vector<double> speedWindow;   // rolling last second
    double lastSecondPeak = 0.0;
    auto displacementAt = [&](double t, bool record) {
        for (size_t i = 0; i < m.bodies.size(); ++i) if (entries[i].rb) snaps[i].push_back(entries[i].rb->getCenterOfMassPosition());
    };
    for (int step = 0; step < totalSteps; ++step) {
        const double t = step * dt;
        // move the kinematic anchors with the shift over [shiftStart, shiftEnd], easing in/out
        if (shiftAt >= 0 && t >= shiftStart && t <= shiftEnd + 0.0001) {
            double phase = (t - shiftStart) / std::max(1e-6, shiftDur);
            double w = phase <= 0 ? 0 : (phase >= 1 ? 1 : 0.5 - 0.5 * std::cos(3.14159265 * phase));
            for (size_t i = 0; i < m.bodies.size(); ++i) if (m.bodies[i].mode == 0 && entries[i].rb) {
                entries[i].ms->t0.setOrigin(btVector3(m.bodies[i].pos.x + shift.x() * w, m.bodies[i].pos.y + shift.y() * w, m.bodies[i].pos.z + shift.z() * w));
            }
        }
        if (t >= seconds) break;
        if (step % 60 == 0 && std::getenv("MMDSIM_TRACE")) std::fprintf(stderr, "step %d t=%.3f\n", step, t);
        world.stepSimulation((float)dt, 1, (float)dt);
        // report
        double speedMax = 0.0;
        for (size_t i = 0; i < m.bodies.size(); ++i) if (m.bodies[i].mode != 0 && entries[i].rb) {
            const btVector3 v = entries[i].rb->getLinearVelocity();
            const double s = v.length();
            speedMax = std::max(speedMax, s);
            std::string n = m.bodies[i].name; size_t sp = n.find_last_of(' '); std::string key = sp == std::string::npos ? n : n.substr(0, sp);
            peakByChain[key] = std::max(peakByChain[key], s);
        }
        peakSpeed = std::max(peakSpeed, speedMax);
        // remember the last second's peak
        if (t >= seconds - 1.0) lastSecondPeak = std::max(lastSecondPeak, speedMax);
        if (nextReport < 8 && t >= reportTimes[nextReport]) { displacementAt(t, true); nextReport++; }
    }

    // chains: body name minus trailing " N"
    std::map<std::string, std::vector<size_t>> byChain;
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        std::string n = m.bodies[i].name; size_t sp = n.find_last_of(' '); if (sp != std::string::npos) n = n.substr(0, sp);
        byChain[n].push_back(i);
    }
    std::vector<int> nextReportArr;   // unused
    std::fprintf(stderr, "gravity 0,%.1f hz 120 seconds %.2f springScale %.2f springAbs %.1f damp %.2f limitDeg %.1f\n", gravY, seconds, springScale, springAbs, dampAbs, limitDeg);
    std::fprintf(stderr, "peak speed any body %.4f (units/s); last-second peak %.4f\n", peakSpeed, lastSecondPeak);
    if (summaryOnly) {
        // one line per chain: max displacement of any dynamic body from its start, and the peak speed
        std::map<std::string, double> maxDisp, maxSpd;
        for (const auto& [key, idxs] : byChain) {
            double dmax = 0;
            for (size_t i : idxs) {
                if (m.bodies[i].mode == 0) continue;
                for (const auto& p : snaps[i]) dmax = std::max(dmax, (double)(p - entries[i].start).length());
            }
            maxDisp[key] = dmax;
            auto it = peakByChain.find(key);
            maxSpd[key] = it == peakByChain.end() ? 0.0 : it->second;
        }
        double worstD = 0, worstS = 0; int bad = 0;
        for (const auto& [key, d] : maxDisp) {
            const double s = maxSpd[key];
            if (d > 0.15 || s > 3.0) ++bad;
            worstD = std::max(worstD, d); worstS = std::max(worstS, s);
            if (chainFilter && lower(key).find(lower(chainFilter)) == std::string::npos) continue;
            std::fprintf(stderr, "summary %-34s maxDisp %.3f peak %.2f %s\n", key.c_str(), d, s, (d > 0.15 || s > 3.0) ? "  <-- BAD" : "");
        }
        std::fprintf(stderr, "summary: %d chains with maxDisp > 0.15 or peak > 3; worst displacement %.3f, worst peak %.2f\n", bad, worstD, worstS);
        return 0;
    }
    size_t repIndex = 0;
    for (const auto& [key, idxs] : byChain) {
        if (chainFilter && lower(key).find(lower(chainFilter)) == std::string::npos) continue;
        bool anyDyn = false; for (size_t i : idxs) if (m.bodies[i].mode != 0) anyDyn = true;
        if (!anyDyn) continue;
        std::fprintf(stderr, "\nchain '%s' (peak %.3f units/s)\n", key.c_str(), peakByChain[key]);
        for (size_t i : idxs) {
            const RigidBody& b = m.bodies[i];
            std::fprintf(stderr, "  body %-28s mode=%d start=(%.2f %.2f %.2f)", b.name.c_str(), b.mode, b.pos.x, b.pos.y, b.pos.z);
            for (size_t k = 0; k < snaps[i].size() && k < 6; ++k) std::fprintf(stderr, "  t%.2f=(%.2f %.2f %.2f)", reportTimes[k], snaps[i][k].x(), snaps[i][k].y(), snaps[i][k].z());
            std::fprintf(stderr, "\n");
        }
    }
    return 0;
}
