// Scratch: diff two PMXs — bones, rigid bodies, joints, morphs, vertex deltas.
//   usage: pmxdiff2 <a.pmx> <b.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
using namespace cardbridge;
using namespace pmxread;

static bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static bool veq(const V3& a, const V3& b, float eps = 1e-4f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    Bytes rawA = fileio::ReadFile(argv[1]);
    Model A = Read(rawA.data(), rawA.size());
    Bytes rawB = fileio::ReadFile(argv[2]);
    Model B = Read(rawB.data(), rawB.size());

    std::fprintf(stderr, "A: verts=%zu idx=%zu bones=%zu bodies=%zu joints=%zu morphs=%zu\n", A.vertices.size(), A.indices.size(), A.bones.size(), A.bodies.size(), A.joints.size(), A.morphs.size());
    std::fprintf(stderr, "B: verts=%zu idx=%zu bones=%zu bodies=%zu joints=%zu morphs=%zu\n", B.vertices.size(), B.indices.size(), B.bones.size(), B.bodies.size(), B.joints.size(), B.morphs.size());

    // bones
    size_t nb = std::min(A.bones.size(), B.bones.size());
    for (size_t i = 0; i < nb; ++i) {
        const Bone& a = A.bones[i], & b = B.bones[i];
        if (a.name != b.name || a.parent != b.parent || a.flags != b.flags || !veq(a.pos, b.pos)) {
            std::fprintf(stderr, "bone[%zu] %s -> %s parent %d->%d flags %x->%x pos (%.3f %.3f %.3f)->(%.3f %.3f %.3f)\n",
                i, a.name.c_str(), b.name.c_str(), a.parent, b.parent, a.flags, b.flags, a.pos.x, a.pos.y, a.pos.z, b.pos.x, b.pos.y, b.pos.z);
        }
    }
    if (A.bones.size() != B.bones.size()) {
        size_t s = std::min(A.bones.size(), B.bones.size());
        for (size_t i = s; i < A.bones.size(); ++i) std::fprintf(stderr, "bone only in A: %s parent=%d pos=(%.3f %.3f %.3f)\n", A.bones[i].name.c_str(), A.bones[i].parent, A.bones[i].pos.x, A.bones[i].pos.y, A.bones[i].pos.z);
        for (size_t i = s; i < B.bones.size(); ++i) std::fprintf(stderr, "bone only in B: %s parent=%d pos=(%.3f %.3f %.3f)\n", B.bones[i].name.c_str(), B.bones[i].parent, B.bones[i].pos.x, B.bones[i].pos.y, B.bones[i].pos.z);
    }

    // rigid bodies
    size_t nr = std::min(A.bodies.size(), B.bodies.size());
    for (size_t i = 0; i < nr; ++i) {
        const RigidBody& a = A.bodies[i], & b = B.bodies[i];
        bool diff = a.name != b.name || a.bone != b.bone || a.group != b.group || a.mask != b.mask || a.shape != b.shape
            || !veq(a.size, b.size) || !veq(a.pos, b.pos) || !veq(a.rot, b.rot) || !near(a.mass, b.mass) || a.mode != b.mode;
        if (diff) {
            std::fprintf(stderr, "body[%zu] %s -> %s bone %d->%d grp %d->%d mask %x->%x shape %d->%d size (%.3f %.3f %.3f)->(%.3f %.3f %.3f) pos (%.3f %.3f %.3f)->(%.3f %.3f %.3f) mass %.2f->%.2f mode %d->%d\n",
                i, a.name.c_str(), b.name.c_str(), a.bone, b.bone, a.group, b.group, a.mask, b.mask, a.shape, b.shape,
                a.size.x, a.size.y, a.size.z, b.size.x, b.size.y, b.size.z, a.pos.x, a.pos.y, a.pos.z, b.pos.x, b.pos.y, b.pos.z, a.mass, b.mass, a.mode, b.mode);
        }
    }
    if (A.bodies.size() != B.bodies.size()) {
        size_t s = std::min(A.bodies.size(), B.bodies.size());
        for (size_t i = s; i < A.bodies.size(); ++i) std::fprintf(stderr, "body only in A: %s bone=%d pos=(%.3f %.3f %.3f) size=(%.3f %.3f %.3f)\n", A.bodies[i].name.c_str(), A.bodies[i].bone, A.bodies[i].pos.x, A.bodies[i].pos.y, A.bodies[i].pos.z, A.bodies[i].size.x, A.bodies[i].size.y, A.bodies[i].size.z);
        for (size_t i = s; i < B.bodies.size(); ++i) std::fprintf(stderr, "body only in B: %s bone=%d pos=(%.3f %.3f %.3f) size=(%.3f %.3f %.3f)\n", B.bodies[i].name.c_str(), B.bodies[i].bone, B.bodies[i].pos.x, B.bodies[i].pos.y, B.bodies[i].pos.z, B.bodies[i].size.x, B.bodies[i].size.y, B.bodies[i].size.z);
    }

    // joints
    size_t nj = std::min(A.joints.size(), B.joints.size());
    int jdiff = 0;
    for (size_t i = 0; i < nj; ++i) {
        const Joint& a = A.joints[i], & b = B.joints[i];
        bool diff = a.name != b.name || a.a != b.a || a.b != b.b || !veq(a.pos, b.pos) || !veq(a.rotLower, b.rotLower) || !veq(a.rotUpper, b.rotUpper);
        if (diff) {
            ++jdiff;
            if (jdiff <= 200)
                std::fprintf(stderr, "joint[%zu] %s -> %s a=%d->%d b=%d->%d pos (%.3f %.3f %.3f)->(%.3f %.3f %.3f) rotL (%.3f %.3f %.3f)->(%.3f %.3f %.3f) rotU (%.3f %.3f %.3f)->(%.3f %.3f %.3f)\n",
                    i, a.name.c_str(), b.name.c_str(), a.a, b.a, a.b, b.b, a.pos.x, a.pos.y, a.pos.z, b.pos.x, b.pos.y, b.pos.z,
                    a.rotLower.x, a.rotLower.y, a.rotLower.z, b.rotLower.x, b.rotLower.y, b.rotLower.z, a.rotUpper.x, a.rotUpper.y, a.rotUpper.z, b.rotUpper.x, b.rotUpper.y, b.rotUpper.z);
        }
    }
    if (jdiff > 200) std::fprintf(stderr, "... and %d more joint diffs\n", jdiff - 200);
    if (A.joints.size() != B.joints.size()) {
        size_t s = std::min(A.joints.size(), B.joints.size());
        for (size_t i = s; i < A.joints.size(); ++i) std::fprintf(stderr, "joint only in A: %s a=%d b=%d\n", A.joints[i].name.c_str(), A.joints[i].a, A.joints[i].b);
        for (size_t i = s; i < B.joints.size(); ++i) std::fprintf(stderr, "joint only in B: %s a=%d b=%d\n", B.joints[i].name.c_str(), B.joints[i].a, B.joints[i].b);
    }

    // morphs
    size_t nm2 = std::min(A.morphs.size(), B.morphs.size());
    for (size_t i = 0; i < nm2; ++i) {
        const Morph& a = A.morphs[i], & b = B.morphs[i];
        if (a.name != b.name || a.type != b.type || a.count != b.count || a.vertices.size() != b.vertices.size()) {
            std::fprintf(stderr, "morph[%zu] %s -> %s type %d->%d count %zu->%zu verts %zu->%zu\n", i, a.name.c_str(), b.name.c_str(), a.type, b.type, a.count, b.count, a.vertices.size(), b.vertices.size());
            continue;
        }
        size_t vd = 0;
        for (size_t k = 0; k < a.vertices.size(); ++k)
            if (a.vertices[k].first != b.vertices[k].first || !veq(a.vertices[k].second, b.vertices[k].second)) ++vd;
        if (vd) std::fprintf(stderr, "morph[%zu] %s: %zu/%zu morph-vertex diffs\n", i, a.name.c_str(), vd, a.vertices.size());
    }
    if (A.morphs.size() != B.morphs.size()) std::fprintf(stderr, "morph count differs: %zu vs %zu\n", A.morphs.size(), B.morphs.size());
    return 0;
}
