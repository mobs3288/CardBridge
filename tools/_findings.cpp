// Scratch: run the repo's own PMX physics findings check on an exported file.
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include "../src/export/PmxPhysics.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
using namespace cardbridge;
using namespace pmxread;

pmxread::V3 operator-(pmxread::V3 a, pmxread::V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }float lengthOf(pmxread::V3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A capsule's long axis in the world: +Y turned by a rigid body's rotation in PMX order (roll about z, then pitch about x, then yaw about y).
pmxread::V3 capsuleAxis(pmxread::V3 e) {
    pmxread::V3 v{0, 1, 0};
    v = {v.x * std::cos(e.z) - v.y * std::sin(e.z), v.x * std::sin(e.z) + v.y * std::cos(e.z), v.z};
    v = {v.x, v.y * std::cos(e.x) - v.z * std::sin(e.x), v.y * std::sin(e.x) + v.z * std::cos(e.x)};
    v = {v.x * std::cos(e.y) + v.z * std::sin(e.y), v.y, -v.x * std::sin(e.y) + v.z * std::cos(e.y)};
    return v;
}

// Everything that can be wrong with the physics of a written PMX, each finding one line: bodies on missing bones or far from them,
// capsules not along their bone, joints on missing bodies, an anchor that is not kinematic, a chain body that is, collision filters that
// let a chain hit anything but the colliders, vertex morphs and display frames pointing past their lists, and chains with no anchor.
std::vector<std::string> pmxPhysicsFindings(const pmxread::Model& m) {
    std::vector<std::string> out;
    const int nb = static_cast<int>(m.bodies.size());
    auto bit = [](uint16_t mask, int group) { return (mask >> group & 1) != 0; };
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        const auto& b = m.bodies[i];
        const std::string who = "body " + std::to_string(i) + " '" + b.name + "'";
        if (b.bone < 0 || b.bone >= static_cast<int>(m.bones.size())) { out.push_back(who + ": bone " + std::to_string(b.bone) + " does not exist"); continue; }
        if (b.shape < 0 || b.shape > 2) out.push_back(who + ": unknown shape");
        if (b.mode < 0 || b.mode > 2) out.push_back(who + ": unknown mode");
        if (b.mode != 0 && !(b.mass > 0)) out.push_back(who + ": a moving body without mass");
        if (b.mode != 0 && (b.moveDamping < 0 || b.moveDamping > 1 || b.rotDamping < 0 || b.rotDamping > 1)) out.push_back(who + ": damping outside 0..1");
        if (!(b.size.x > 0)) out.push_back(who + ": no radius");
        const pmxread::V3 head = m.bones[static_cast<size_t>(b.bone)].pos;
        const float reach = b.shape == 2 ? 0.5f * b.size.y + b.size.x : b.size.x;
        const float dist = lengthOf(b.pos - head);
        if (b.group == exporter::kPmxGroupChain && b.mode != 0 && dist > reach + 1e-3f) out.push_back(who + ": its centre is " + std::to_string(dist) + " from its bone, further than its own half length " + std::to_string(reach));
        if (b.group == exporter::kPmxGroupChain && b.mode != 0 && b.shape == 2) {
            // a chain link runs from its bone's head: the axis points from the head to the centre
            const pmxread::V3 axis = capsuleAxis(b.rot), want = b.pos - head;
            const float l = lengthOf(want);
            const float d = l > 1e-6f ? (axis.x * want.x + axis.y * want.y + axis.z * want.z) / l : 1.0f;
            if (d < 0.999f) out.push_back(who + ": the capsule does not follow its bone (cos " + std::to_string(d) + ")");
        }
        if (b.group != exporter::kPmxGroupChain) {
            if (b.mode != 0) out.push_back(who + ": a collider that moves");
            if (!bit(b.mask, exporter::kPmxGroupChain)) out.push_back(who + ": a collider that does not meet chains");
            if ((b.mask & ~(1u << exporter::kPmxGroupChain)) != 0) out.push_back(who + ": a collider that meets more than chains");
        } else if (b.mode != 0) {
            if (bit(b.mask, exporter::kPmxGroupChain)) out.push_back(who + ": a chain body that meets chains (mask " + std::to_string(b.mask) + ")");
            // it meets only colliders it does not already overlap: a body that starts inside one is thrown out of it, and the model with it
            for (size_t k = 0; k < m.bodies.size(); ++k) {
                const auto& c = m.bodies[k];
                if (c.group == exporter::kPmxGroupChain || !bit(b.mask, c.group)) continue;
                auto core = [](const pmxread::RigidBody& x, Vec3& p, Vec3& q) {
                    const pmxread::V3 ax = capsuleAxis(x.rot);
                    const float h = x.shape == 2 ? 0.5f * x.size.y : 0.0f;
                    p = {x.pos.x - ax.x * h, x.pos.y - ax.y * h, x.pos.z - ax.z * h};
                    q = {x.pos.x + ax.x * h, x.pos.y + ax.y * h, x.pos.z + ax.z * h};
                };
                Vec3 a0, a1, c0, c1;
                core(b, a0, a1);
                core(c, c0, c1);
                if (exporter::SegmentDistance(a0, a1, c0, c1) < b.size.x + c.size.x) out.push_back(who + ": it may meet collider " + std::to_string(k) + " '" + c.name + "' but starts inside it");
            }
        } else if (b.mask != 0) {
            out.push_back(who + ": an anchor that collides");
        }
    }
    std::vector<std::vector<int>> linked(m.bodies.size());
    for (size_t i = 0; i < m.joints.size(); ++i) {
        const auto& j = m.joints[i];
        if (j.a < 0 || j.a >= nb || j.b < 0 || j.b >= nb || j.a == j.b) { out.push_back("joint " + std::to_string(i) + ": does not join two bodies"); continue; }
        linked[static_cast<size_t>(j.a)].push_back(j.b);
        linked[static_cast<size_t>(j.b)].push_back(j.a);
        if (j.type != 0) out.push_back("joint " + std::to_string(i) + ": not a spring 6-DOF joint");
        for (float l : {j.rotLower.x, j.rotLower.y, j.rotLower.z}) if (!(l <= 0)) out.push_back("joint " + std::to_string(i) + ": a lower rotation limit above 0");
        for (float u : {j.rotUpper.x, j.rotUpper.y, j.rotUpper.z}) if (!(u >= 0)) out.push_back("joint " + std::to_string(i) + ": an upper rotation limit below 0");
        if (!(j.springRot.x > 0 && j.springRot.y > 0 && j.springRot.z > 0)) out.push_back("joint " + std::to_string(i) + ": no rotation spring");
        // the joint stands where the second body meets the first: on or near the second body
        const auto& bb = m.bodies[static_cast<size_t>(j.b)];
        const float reach = bb.shape == 2 ? 0.5f * bb.size.y + bb.size.x : bb.size.x;
        if (lengthOf(j.pos - bb.pos) > reach + 1e-3f) out.push_back("joint " + std::to_string(i) + ": far from its second body");
    }
    // every moving body is tied to a kinematic one through joints
    std::vector<char> reached(m.bodies.size(), 0);
    std::vector<int> stack;
    for (int i = 0; i < nb; ++i) if (m.bodies[static_cast<size_t>(i)].mode == 0 && m.bodies[static_cast<size_t>(i)].group == exporter::kPmxGroupChain) { reached[static_cast<size_t>(i)] = 1; stack.push_back(i); }
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        for (int n : linked[static_cast<size_t>(at)]) if (!reached[static_cast<size_t>(n)]) { reached[static_cast<size_t>(n)] = 1; stack.push_back(n); }
    }
    for (int i = 0; i < nb; ++i)
        if (m.bodies[static_cast<size_t>(i)].mode != 0 && !reached[static_cast<size_t>(i)]) out.push_back("body " + std::to_string(i) + " is not joined to an anchor");
    for (size_t i = 0; i < m.bones.size(); ++i) {   // IK: the target and links exist, the links are the target's ancestors nearest first, the handle stands on the target
        const auto& b = m.bones[i];
        if (!b.isIk()) continue;
        const int n = static_cast<int>(m.bones.size());
        const std::string who = "IK bone " + std::to_string(i) + " '" + b.name + "'";
        if (b.ikTarget < 0 || b.ikTarget >= n || b.ikTarget == static_cast<int>(i)) { out.push_back(who + ": no target"); continue; }
        if (b.ikLinks.empty() || b.ikLoop <= 0 || !(b.ikLimit > 0)) out.push_back(who + ": no links, loops or angle limit");
        int expect = m.bones[static_cast<size_t>(b.ikTarget)].parent;
        for (const auto& l : b.ikLinks) {
            if (l.bone < 0 || l.bone >= n || l.bone != expect) { out.push_back(who + ": a link that is not the next ancestor of the target"); break; }
            if (l.limited && (l.lower.x > l.upper.x || l.lower.y > l.upper.y || l.lower.z > l.upper.z)) out.push_back(who + ": a limit with min above max");
            expect = m.bones[static_cast<size_t>(l.bone)].parent;
        }
        if (lengthOf(b.pos - m.bones[static_cast<size_t>(b.ikTarget)].pos) > 1e-3f) out.push_back(who + ": its handle is not on the target");
    }
    for (size_t i = 0; i < m.morphs.size(); ++i)
        for (const auto& vo : m.morphs[i].vertices)
            if (vo.first < 0 || vo.first >= static_cast<int>(m.vertices.size())) { out.push_back("morph " + std::to_string(i) + ": a vertex index past the list"); break; }
    for (size_t i = 0; i < m.frames.size(); ++i)
        for (const auto& it : m.frames[i].items)
            if (it.second < 0 || it.second >= static_cast<int>(it.first == 0 ? m.bones.size() : m.morphs.size())) { out.push_back("frame " + std::to_string(i) + ": an item past its list"); break; }
    for (size_t i = 0; i < m.bones.size(); ++i) {
        if (m.bones[i].parent >= static_cast<int>(m.bones.size())) out.push_back("bone " + std::to_string(i) + ": parent past the list");
        if (m.bones[i].tailBone >= static_cast<int>(m.bones.size())) out.push_back("bone " + std::to_string(i) + ": tail past the list");
    }
    return out;
}
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: findings <file.pmx>\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    const auto findings = pmxPhysicsFindings(m);
    std::printf("%zu finding(s)\n", findings.size());
    for (const auto& f : findings) std::printf("  %s\n", f.c_str());
    return findings.empty() ? 0 : 1;
}