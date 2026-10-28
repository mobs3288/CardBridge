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
    if (j["counts"].value("bones", 0) != m.bones.size()) fail("manifest bone count != PMX bone count");

    if (bad) { std::printf("%d problem(s)\n", bad); return 3; }
    std::puts("OK: the engine's PMX loader reads the file and every check passes");
    return 0;
}
