#pragma once

#include "core/Bytes.h"
#include "core/Math.h"
#include "library/AssetCatalog.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cardbridge::library {

// A source material as far as PMX can express it. Properties that PMX cannot
// represent are recorded in `AssetTemplate::warnings`, never silently dropped.
struct AssetMaterial {
    std::string name, shaderName;
    Vec4 diffuse{1, 1, 1, 1};
    Vec3 ambient{0.5f, 0.5f, 0.5f};
    Vec3 specular{0, 0, 0};
    float specularPower = 5.0f;
    Vec3 edgeColor{0, 0, 0};
    float edgeSize = 1.0f;
    bool doubleSided = false;
    bool translucent = false;         // source shader/material uses alpha
    int diffuseTexture = -1;          // index into AssetTemplate::textures
    int toonTexture = -1;
    int sphereTexture = -1;
    uint8_t sphereMode = 0;           // 0 none, 1 multiply, 2 add
    uint8_t drawFlags = 0x1E;
};

// An in-memory texture extracted from a base library or zipmod. `bytes` holds
// the original encoded PNG when available, otherwise a PNG re-encoded from the
// decoded RGBA (compressed Unity texture formats cannot be written raw).
struct AssetTexture {
    std::string key;                  // stable source identifier (bundle|asset name)
    std::string name;                 // original filename where available
    std::string format = "PNG";
    std::string role = "diffuse";     // diffuse, toon, sphere, mask, ...
    std::string source;               // base library or owning zipmod
    Bytes bytes;                      // encoded PNG bytes
    int width = 0, height = 0;
};

// The geometry of one catalog asset, extracted without a card. Skin weights
// refer to `boneNames` (parallel rest-world positions in `bonePositions`).
struct AssetVertex {
    Vec3 pos, normal;
    Vec2 uv;
    int bone[4] = {-1, -1, -1, -1};
    float weight[4] = {0, 0, 0, 0};
};

struct AssetTemplate {
    std::string key, name, attachNode;
    std::vector<AssetVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> boneNames;
    std::vector<Vec3> bonePositions;
    std::vector<int> boneParents;         // renderer bone hierarchy, -1 for roots
    std::vector<AssetMaterial> materials;
    std::vector<AssetTexture> textures;
    std::vector<int> submeshMaterials;    // parallel to submeshRanges, index into materials
    std::vector<std::pair<size_t, size_t>> submeshRanges;  // (first index, count) in indices
    std::vector<std::string> warnings;
};

struct AssetTemplateOptions {
    std::string libraryRoot;
    std::string modsRoot;
    std::string modCachePath;
};

// Opens the asset's prefab from the base library or owning zipmod and returns
// its skinned/rest-pose geometry. Rigid pieces are bound to their parent node.
// No Card and no running game is involved.
bool LoadAssetTemplate(const AssetRecord& record, const AssetTemplateOptions& options,
                       AssetTemplate& out, std::string& error);

} // namespace cardbridge::library
