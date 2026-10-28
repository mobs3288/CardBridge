#pragma once

#include "core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::assemble {

// The assembled character, in Unity units, Y up, left-handed (the same handedness
// as PMX - no mirroring needed, only scale). Plain data: the exporter and the
// future engine importer both read this, nothing in it knows about PMX or Vulkan.

struct SceneBone {
    std::string name;
    int parent = -1;
    Vec3 pos;                 // rest position, model space
    std::string source;       // "rig" (shared skeleton) or the part that added it
};

struct SceneVertex {
    Vec3 pos, normal;
    Vec2 uv;                  // Unity convention (v up); the PMX writer flips v
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
};

struct SceneTexture {
    std::string name;
    std::string source;       // bundle it came from (provenance)
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // top row first
    uint64_t pixelHash = 0;      // FNV-1a 64 of rgba
};

struct SceneMaterial {
    std::string name;
    std::string shader;
    int texture = -1;
    Vec4 diffuse{1, 1, 1, 1};
    bool doubleSided = false;
    bool alpha = false;
    // The game's toon look, from the Shader Forge material: the colour a surface turns in shade, its highlight and its outline.
    Vec3 shadow{0.5f, 0.5f, 0.5f};           // _ShadowColor (per-channel multiplier of the lit colour)
    Vec3 specular{0, 0, 0};                  // _SpecularColor scaled by its power
    float specularPower = 5.0f;
    Vec3 outlineColor{0, 0, 0};              // _LineColor
    float outlineWidth = 0.0f;               // _LineWidthS (0 = no outline)
};

struct SceneSubmesh {
    int material = 0;
    std::vector<uint32_t> indices;   // triangles, into SceneMesh::vertices
};

struct SceneMesh {
    std::string name;
    int part = -1;
    std::vector<SceneVertex> vertices;
    std::vector<SceneSubmesh> submeshes;
};

struct ScenePart {
    std::string id;           // stable: "head", "hair.back", "outfit0.top", "outfit0.acc03"
    std::string label;
    std::string group;        // Body, Head, Hair, Outfit, Accessories
    std::string property;
    int category = 0;
    int64_t sourceItemId = 0;
    std::string status;       // resolved-mod, resolved-base, missing-mod, ... (see resolve::StatusName)
    std::string detail;
    std::string modGuid, modName, modVersion, modAuthor, modPath;
    std::string bundle;       // "<abdata-relative path>" of the bundle the geometry came from
    bool visibleInPreview = true;
    bool includeInExport = true;
    std::vector<int> meshes;  // indices into Scene::meshes
};

struct SceneIssue {
    std::string severity;     // "warning" or "error"
    std::string part;         // part id or "" for the whole scene
    std::string message;
};

struct Scene {
    std::string name;
    std::vector<SceneBone> bones;
    std::vector<SceneTexture> textures;
    std::vector<SceneMaterial> materials;
    std::vector<SceneMesh> meshes;
    std::vector<ScenePart> parts;
    std::vector<SceneIssue> issues;

    int boneIndex(const std::string& name) const {
        for (size_t i = 0; i < bones.size(); ++i)
            if (bones[i].name == name) return static_cast<int>(i);
        return -1;
    }
    void warn(const std::string& part, const std::string& msg) { issues.push_back({"warning", part, msg}); }
    void error(const std::string& part, const std::string& msg) { issues.push_back({"error", part, msg}); }
};

} // namespace cardbridge::assemble
