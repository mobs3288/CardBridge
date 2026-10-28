#pragma once

#include "core/Bytes.h"
#include "core/Math.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

// A PMX 2.0 model as flat data. The writer knows nothing about Koikatsu or
// Unity; the exporter fills this from an assembled scene.
struct Vertex {
    Vec3 position, normal;
    Vec2 uv;
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    float edgeScale = 1.0f;
};

struct Material {
    std::string name, nameEn;
    Vec4 diffuse{1, 1, 1, 1};
    Vec3 specular{0, 0, 0};
    float specularPower = 5.0f;
    Vec3 ambient{0.5f, 0.5f, 0.5f};
    uint8_t drawFlags = 0;          // 0x01 double-sided, 0x10 edge
    Vec4 edgeColor{0, 0, 0, 1};
    float edgeSize = 1.0f;
    int texture = -1;               // index into Model::textures
    uint32_t indexCount = 0;        // indices (3 per triangle) this material owns, in order
    std::string memo;
};

struct Bone {
    std::string name, nameEn;
    Vec3 position;
    int parent = -1;
    int layer = 0;
    uint16_t flags = 0x001E;        // rotatable, translatable, visible, enabled
    Vec3 tailOffset;
};

struct Model {
    std::string name, nameEn, comment, commentEn;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // ordered by material
    std::vector<std::string> textures;   // relative paths, forward slashes
    std::vector<Material> materials;
    std::vector<Bone> bones;
};

// Serialises to PMX 2.0 (UTF-16LE text, no morphs, rigid bodies or joints).
// Throws FormatError if the model is inconsistent (index out of range, material
// index counts not matching the index buffer).
Bytes WritePmx(const Model& m);

// UTF-8 to UTF-16LE for PMX text fields (exposed for tests).
Bytes Utf8ToUtf16Le(const std::string& s);

} // namespace cardbridge::pmx
