// Scratch: report how many exported vertices carry the sway layer.
//   usage: swayprobe <file.pmx>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: swayprobe <file.pmx>\n"); return 1; }
    Bytes raw = fileio::ReadFile(argv[1]);
    pmxread::Model m = pmxread::Read(raw.data(), raw.size());

    std::set<int> swaySet;
    for (size_t i = 0; i < m.bones.size(); ++i) {
        const std::string& n = m.bones[i].name;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, " sway") == 0) swaySet.insert(static_cast<int>(i));
    }
    std::printf("total bones=%zu sway bones=%zu\n", m.bones.size(), swaySet.size());

    const size_t total = m.vertices.size();
    size_t swayed = 0;
    double sumShare = 0.0, maxShare = 0.0;
    for (const auto& v : m.vertices) {
        float sw = 0.0f;
        for (int k = 0; k < 4; ++k) sw += (swaySet.count(v.bone[k]) ? v.weight[k] : 0.0f);
        if (sw > 0.0f) { ++swayed; sumShare += sw; if (sw > maxShare) maxShare = sw; }
    }
    size_t swayBodies = 0, swayJoints = 0;
    std::string probe[] = {"", "sway"};
    for (const auto& rb : m.bodies) if (rb.name.find("sway") != std::string::npos) ++swayBodies;
    for (const auto& j : m.joints) if (j.name.find("sway") != std::string::npos) ++swayJoints;
    (void)probe;
    std::printf("vertices=%zu swayed=%zu (%.1f%%) avgShare=%.3f maxShare=%.3f\n", total, swayed, 100.0 * double(swayed) / double(total), swayed ? sumShare / double(swayed) : 0.0, maxShare);
    std::printf("sway bodies=%zu joints=%zu\n", swayBodies, m.joints.size());
    return 0;
}
