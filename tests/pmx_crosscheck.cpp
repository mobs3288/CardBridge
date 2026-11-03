// Independent read-back of a CardBridge PMX with the engine's own PMX loader
// (engine/src/assets/PmxFile.cpp), which shares no code with CardBridge's
// writer. Checks what the manifest says was written against what the loader
// reads, plus structural sanity (weights, indices, bone parents, texture files).
//
//   cardbridge_pmx_crosscheck <model.pmx> <model.cardbridge.json>
//
// Exit 0 when everything matches.

#include "assets/PmxFile.h"
#include "core/FileIo.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: cardbridge_pmx_crosscheck <model.pmx> <manifest.json>"); return 1; }
    engine::pmx::PmxFile m;
    std::string err;
    if (!engine::pmx::Load(argv[1], m, err)) { std::printf("FAIL: engine PMX loader rejected the file: %s\n", err.c_str()); return 2; }

    int bad = 0;
    auto fail = [&](const std::string& s) { ++bad; std::printf("FAIL: %s\n", s.c_str()); };

    std::ifstream f(argv[2], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str());

    std::printf("engine reader: PMX %.1f, %zu vertices, %zu triangles, %zu materials, %zu bones, %zu textures, %zu morphs\n", m.version,
                m.vertices.size(), m.indices.size() / 3, m.materials.size(), m.bones.size(), m.texturePaths.size(), m.morphs.size());

    if (std::fabs(m.version - 2.0f) > 1e-6f) fail("version is not 2.0");
    if (m.indices.size() % 3 != 0) fail("index count is not a multiple of 3");
    for (uint32_t i : m.indices) if (i >= m.vertices.size()) { fail("index past the vertex list"); break; }

    size_t sumIdx = 0;
    for (const auto& mat : m.materials) {
        sumIdx += mat.indexCount;
        if (mat.textureIndex >= static_cast<int>(m.texturePaths.size())) fail("material texture index out of range");
    }
    if (sumIdx != m.indices.size()) fail("material index counts do not add up to the index buffer");

    size_t badW = 0, badB = 0;
    for (const auto& v : m.vertices) {
        float s = 0;
        for (int k = 0; k < 4; ++k) {
            s += v.weights[k];
            if (v.weights[k] > 0 && (v.bones[k] < 0 || v.bones[k] >= static_cast<int>(m.bones.size()))) ++badB;
        }
        if (std::fabs(s - 1.0f) > 1e-3f) ++badW;
    }
    if (badW) fail(std::to_string(badW) + " vertices whose weights do not sum to 1");
    if (badB) fail(std::to_string(badB) + " weights bound to a bone that does not exist");
    for (const auto& b : m.bones)
        if (b.parentIndex >= static_cast<int>(m.bones.size())) { fail("bone parent out of range"); break; }

    std::string dir = engine::fileio::DirectoryOf(argv[1]);
    size_t missingTex = 0;
    for (const auto& t : m.texturePaths)
        if (!engine::fileio::Exists(engine::fileio::JoinPath(dir, t))) ++missingTex;
    if (missingTex) fail(std::to_string(missingTex) + " texture file(s) listed in the PMX are missing next to it");

    // manifest vs file
    size_t partTris = 0, partMats = 0;
    for (const auto& p : j["parts"]) {
        if (p.contains("pmx_materials")) partMats += p["pmx_materials"].value("count", 0);
    }
    (void)partTris;
    if (partMats != m.materials.size()) fail("manifest part material ranges (" + std::to_string(partMats) + ") != PMX materials (" + std::to_string(m.materials.size()) + ")");
    const size_t ikBones = j["counts"].value("ik_bones", 0);
    if (j["counts"].value("bones", 0) + ikBones != m.bones.size()) fail("manifest bone count (+ IK bones) != PMX bone count");

    // physics, as the engine's loader reads it (it parses rigid bodies and joints; the mask is the Bullet filter, bit n = collides with group n)
    if (j["counts"].contains("rigid_bodies") && j["counts"].value("rigid_bodies", 0) != m.rigidBodies.size()) fail("manifest rigid body count != PMX rigid body count");
    if (j["counts"].contains("joints") && j["counts"].value("joints", 0) != m.joints.size()) fail("manifest joint count != PMX joint count");
    size_t kinematic = 0, moving = 0, badBody = 0, badJoint = 0, badFilter = 0, farBody = 0;
    for (const auto& rb : m.rigidBodies) {
        if (rb.boneIndex < 0 || rb.boneIndex >= static_cast<int>(m.bones.size()) || rb.shape > 2 || rb.physicsMode > 2 || !(rb.size.x > 0.0f)) { ++badBody; continue; }
        (rb.physicsMode == 0 ? kinematic : moving)++;
        if (rb.physicsMode != 0 && rb.mass <= 0.0f) ++badBody;
        // chain bodies (group 1) meet colliders (group 0) only; colliders meet chains only; anchors meet nothing
        if (rb.group == 1 && rb.physicsMode != 0 && rb.nonCollisionMask != 0x0001) ++badFilter;
        if (rb.group == 1 && rb.physicsMode == 0 && rb.nonCollisionMask != 0) ++badFilter;
        if (rb.group == 0 && rb.nonCollisionMask != 0x0002) ++badFilter;
        if (rb.group == 1 && rb.physicsMode != 0) {
            const float reach = (rb.shape == 2 ? 0.5f * rb.size.y : 0.0f) + rb.size.x + 1e-3f;
            const auto d = rb.position - m.bones[static_cast<size_t>(rb.boneIndex)].position;
            if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) > reach) ++farBody;
        }
    }
    for (const auto& jt : m.joints) {
        const int n = static_cast<int>(m.rigidBodies.size());
        if (jt.rigidBodyA < 0 || jt.rigidBodyA >= n || jt.rigidBodyB < 0 || jt.rigidBodyB >= n || jt.rigidBodyA == jt.rigidBodyB) { ++badJoint; continue; }
        if (m.rigidBodies[static_cast<size_t>(jt.rigidBodyB)].physicsMode == 0) ++badJoint;   // the second body of a chain joint is the moving one
        if (jt.rotationSpring.x <= 0.0f) ++badJoint;
    }
    // IK bones: the target and links exist, the links are the target's ancestors nearest first, the handle stands on the target at rest, limits are ordered
    size_t ikCount = 0, badIk = 0, ikLimited = 0;
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const auto& b = m.bones[i];
        if (!b.IsIk()) continue;
        ++ikCount;
        const int n = static_cast<int>(m.bones.size());
        bool ok = b.ikTargetIndex >= 0 && b.ikTargetIndex < n && b.ikTargetIndex != static_cast<int>(i) && !b.ikLinks.empty() && b.ikLoopCount > 0 && b.ikLimitRadian > 0.0f;
        int expect = ok ? m.bones[static_cast<size_t>(b.ikTargetIndex)].parentIndex : -1;
        for (const auto& l : b.ikLinks) {
            if (!ok) break;
            if (l.boneIndex < 0 || l.boneIndex >= n || l.boneIndex != expect) { ok = false; break; }
            if (l.hasLimits && (l.limitMin.x > l.limitMax.x || l.limitMin.y > l.limitMax.y || l.limitMin.z > l.limitMax.z)) ok = false;
            expect = m.bones[static_cast<size_t>(l.boneIndex)].parentIndex;
        }
        for (const auto& l : b.ikLinks) if (l.hasLimits) ++ikLimited;
        if (ok) {
            const auto d = b.position - m.bones[static_cast<size_t>(b.ikTargetIndex)].position;
            if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) > 1e-3f) ok = false;
        }
        if (!ok) ++badIk;
    }
    if (j["counts"].contains("ik_bones") && j["counts"].value("ik_bones", 0) != ikCount) fail("manifest IK bone count != the IK bones the engine reader sees");
    if (badIk) fail(std::to_string(badIk) + " IK bones with a bad target, links that are not the target's ancestors, or a handle off its target");
    std::printf("engine reader: %zu IK bones, %zu limited links\n", ikCount, ikLimited);
    if (badBody) fail(std::to_string(badBody) + " rigid bodies on a missing bone, with a bad shape / mode / size or a moving body without mass");
    if (badJoint) fail(std::to_string(badJoint) + " joints that do not join two existing bodies, or spring-less ones");
    if (badFilter) fail(std::to_string(badFilter) + " rigid bodies whose collision filter lets chains hit anything but colliders");
    if (farBody) fail(std::to_string(farBody) + " chain bodies further from their bone than their own half length");
    std::printf("engine reader: %zu rigid bodies (%zu kinematic, %zu moving), %zu joints\n", m.rigidBodies.size(), kinematic, moving, m.joints.size());

    if (bad) { std::printf("%d problem(s)\n", bad); return 3; }
    std::puts("OK: the engine's PMX loader reads the file and every check passes");
    return 0;
}
