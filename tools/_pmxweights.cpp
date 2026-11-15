// Scratch: which bones does a material's mesh use?   usage: pmxweights <file.pmx> <material-substring>
#include "../tests/PmxReader.h"
#include "../src/core/FileIo.h"
#include "../src/core/Bytes.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <algorithm>
#include <vector>
using namespace cardbridge;
using namespace pmxread;

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    Bytes raw = fileio::ReadFile(argv[1]);
    Model m = Read(raw.data(), raw.size());
    std::string want = argv[2];
    std::vector<std::pair<size_t, size_t>> ranges(m.materials.size());
    size_t o = 0;
    for (size_t i = 0; i < m.materials.size(); ++i) { ranges[i] = {o, o + m.materials[i].indexCount}; o += m.materials[i].indexCount; }
    for (size_t i = 0; i < m.materials.size(); ++i) {
        if (m.materials[i].name.find(want) == std::string::npos) continue;
        std::map<int, double> acc;
        for (size_t k = ranges[i].first; k < ranges[i].second; ++k) {
            const Vertex& v = m.vertices[m.indices[k]];
            for (int b = 0; b < 4; ++b) if (v.weight[b] > 0 && v.bone[b] >= 0) acc[v.bone[b]] += v.weight[b];
        }
        std::vector<std::pair<int, double>> list(acc.begin(), acc.end());
        double total = 0.0;
        for (const auto& [b, w] : list) total += w;
        std::sort(list.begin(), list.end(), [](const std::pair<int, double>& a, const std::pair<int, double>& b) { return a.second > b.second; });
        std::fprintf(stderr, "material %s: indexCount %u\n", m.materials[i].name.c_str(), m.materials[i].indexCount);
        for (const auto& [b, w] : list) {
            if (b < (int)m.bones.size())
                std::fprintf(stderr, "    %-40s %.1f%%\n", m.bones[static_cast<size_t>(b)].name.c_str(), 100.0 * w / total);
        }
    }
    return 0;
}
