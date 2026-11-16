// Scratch analyzer: dump the exported PMX's physics (chains from body names,
// locked joints, spring/limit tables, rest-pose overlap of chain bodies vs colliders).
//   usage: physicsdump <file.pmx> [--chain <substring>] [--locked] [--colliders] [--bones]
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
using namespace cardbridge;
using namespace pmxread;

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: physicsdump <file.pmx> [--chain S] [--locked] [--colliders] [--bones]\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    const char* chainFilter = nullptr;
    bool onlyLocked = false, dumpColliders = false, dumpBones = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--chain") && i + 1 < argc) chainFilter = argv[++i];
        else if (!strcmp(argv[i], "--locked")) onlyLocked = true;
        else if (!strcmp(argv[i], "--colliders")) dumpColliders = true;
        else if (!strcmp(argv[i], "--bones")) dumpBones = true;
    }
    std::fprintf(stderr, "verts %zu bones %zu bodies %zu joints %zu\n", m.vertices.size(), m.bones.size(), m.bodies.size(), m.joints.size());
    if (dumpBones) {
        if (chainFilter) for (size_t i = 0; i < m.bones.size(); ++i) if (lower(m.bones[i].name).find(lower(chainFilter)) != std::string::npos)
            std::fprintf(stderr, "bone %-40s parent=%d pos=(%.3f %.3f %.3f)\n", m.bones[i].name.c_str(), m.bones[i].parent, m.bones[i].pos.x, m.bones[i].pos.y, m.bones[i].pos.z);
    }
    // bodies: print with --chain filter; the chain name is the body name minus the trailing " <n>"
    std::map<std::string, std::vector<size_t>> byChain;
    for (size_t i = 0; i < m.bodies.size(); ++i) {
        std::string n = m.bodies[i].name;
        size_t sp = n.find_last_of(' ');
        std::string key = (sp == std::string::npos) ? n : n.substr(0, sp);
        byChain[key].push_back(i);
    }
    for (const auto& [key, idxs] : byChain) {
        if (chainFilter && lower(key).find(lower(chainFilter)) == std::string::npos) continue;
        std::fprintf(stderr, "\nchain '%s' (%zu bodies)\n", key.c_str(), idxs.size());
        for (size_t i : idxs) {
            const RigidBody& b = m.bodies[i];
            std::fprintf(stderr, "  body %-30s bone=%-40d mode=%d shape=%d mass=%.3f size=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f) grp=%d mask=%04x damp=(%.3f %.3f) fric=%.2f\n",
                         b.name.c_str(), b.bone, b.mode, b.shape, b.mass, b.size.x, b.size.y, b.size.z, b.pos.x, b.pos.y, b.pos.z,
                         b.rot.x, b.rot.y, b.rot.z, b.group, b.mask, b.moveDamping, b.rotDamping, b.friction);
        }
        for (size_t j = 0; j < m.joints.size(); ++j) {
            const Joint& jt = m.joints[j];
            // joint belongs to this chain when its bodyB is one of the chain's bodies
            bool in = false; for (size_t i : idxs) if ((int)i == jt.b) in = true;
            if (!in) continue;
            const float xlim = jt.rotUpper.x, ylim = jt.rotUpper.y, zlim = jt.rotUpper.z;
            const bool locked = xlim == 0.0f && ylim == 0.0f && zlim == 0.0f;
            if (onlyLocked && !locked) continue;
            std::fprintf(stderr, "  joint %-30s a=%zu b=%zu%s pos=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f) rotLim X=(%.1f %.1f) Y=(%.1f %.1f) Z=(%.1f %.1f) springRot=(%.1f %.1f %.1f) springMove=(%.1f %.1f %.1f)\n",
                         jt.name.c_str(), (size_t)jt.a, (size_t)jt.b, locked ? "  LOCKED" : "", jt.pos.x, jt.pos.y, jt.pos.z,
                         jt.rot.x, jt.rot.y, jt.rot.z, jt.rotLower.x * 57.29578f, jt.rotUpper.x * 57.29578f, jt.rotLower.y * 57.29578f, jt.rotUpper.y * 57.29578f,
                         jt.rotLower.z * 57.29578f, jt.rotUpper.z * 57.29578f, jt.springRot.x, jt.springRot.y, jt.springRot.z, jt.springMove.x, jt.springMove.y, jt.springMove.z);
        }
    }
    if (dumpColliders) {
        std::fprintf(stderr, "\ncolliders\n");
        for (size_t i = 0; i < m.bodies.size(); ++i) if (m.bodies[i].name.rfind("collider", 0) == 0 || m.bodies[i].name.rfind("limb collider", 0) == 0)
            std::fprintf(stderr, "  %-34s bone=%-36d grp=%d mask=%04x shape=%d size=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f)\n",
                         m.bodies[i].name.c_str(), m.bodies[i].bone, m.bodies[i].group, m.bodies[i].mask, m.bodies[i].shape,
                         m.bodies[i].size.x, m.bodies[i].size.y, m.bodies[i].size.z, m.bodies[i].pos.x, m.bodies[i].pos.y, m.bodies[i].pos.z,
                         m.bodies[i].rot.x, m.bodies[i].rot.y, m.bodies[i].rot.z);
    }
    // lock statistics
    int total = 0, locked = 0;
    for (const auto& jt : m.joints) if (jt.rotUpper.x == 0.0f && jt.rotUpper.y == 0.0f && jt.rotUpper.z == 0.0f) { ++locked; } ++total;
    std::fprintf(stderr, "\njoints total %d locked %d (%.0f%%)\n", total, locked, 100.0f * locked / std::max(1, total));
    return 0;
}
