// Scratch analyzer: dump bone names + bounding boxes per material from a PMX.
//   usage: pmxcmp <file.pmx> [--skirt] [--bodies]
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
using namespace cardbridge;
using namespace pmxread;

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: pmxcmp <file.pmx> [--skirt] [--bodies]\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    const char* want = nullptr;
    for (int i = 2; i < argc; ++i) { if (!strcmp(argv[i], "--skirt")) want = "skirt"; else if (!strcmp(argv[i], "--bodies")) want = "bodies"; }

    std::fprintf(stderr, "verts %zu idx %zu mats %zu bones %zu bodies %zu joints %zu morphs %zu\n", m.vertices.size(),
                 m.indices.size(), m.materials.size(), m.bones.size(), m.bodies.size(), m.joints.size(), m.morphs.size());
    std::vector<std::pair<size_t, size_t>> ranges(m.materials.size());
    size_t off = 0;
    for (size_t i = 0; i < m.materials.size(); ++i) { ranges[i].first = off; off += m.materials[i].indexCount; ranges[i].second = off; }
    for (size_t i = 0; i < m.bones.size(); ++i) {
        std::string e = lower(m.bones[i].nameEn);
        if (want && e.find(want) == std::string::npos) continue;
        if (!want) continue;
        std::fprintf(stderr, "bone %-30s parent=%d pos=(%.3f %.3f %.3f) flags=%x\n", m.bones[i].name.c_str(), m.bones[i].parent, m.bones[i].pos.x,
                     m.bones[i].pos.y, m.bones[i].pos.z, m.bones[i].flags);
    }
    if (!want || std::string(want) == "bodies") {
        for (const auto& bd : m.bodies)
            if (!want || lower(bd.name).find("skirt") != std::string::npos) std::fprintf(stderr, "body %-30s bone=%d mass=%.1f mode=%d\n", bd.name.c_str(), bd.bone, bd.mass, bd.mode);
    }
    std::fprintf(stderr, "\n-- material bounds (full list)\n");
    std::vector<std::pair<size_t, size_t>> ranges2;
    size_t o2 = 0;
    for (size_t i = 0; i < m.materials.size(); ++i) { ranges2.push_back({o2, o2 + m.materials[i].indexCount}); o2 += m.materials[i].indexCount; }
    for (size_t i = 0; i < m.materials.size(); ++i) {
        V3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (size_t k = ranges2[i].first; k < ranges2[i].second; ++k) {
            const Vertex& v = m.vertices[m.indices[k]];
            mn.x = std::min(mn.x, v.pos.x); mn.y = std::min(mn.y, v.pos.y); mn.z = std::min(mn.z, v.pos.z);
            mx.x = std::max(mx.x, v.pos.x); mx.y = std::max(mx.y, v.pos.y); mx.z = std::max(mx.z, v.pos.z);
        }
        std::fprintf(stderr, "mat %-40s tris=%u y=[%.2f,%.2f] x=[%.2f,%.2f] z=[%.2f,%.2f]\n", m.materials[i].name.c_str(),
                     (unsigned)(ranges2[i].second - ranges2[i].first) / 3, mn.y, mx.y, mn.x, mx.x, mn.z, mx.z);
    }
    return 0;
}
