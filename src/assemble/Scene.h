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
    Vec2 uv1;                 // the second UV set (TEXCOORD1: the body's nipple placement); equals uv when the mesh has none
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
};

struct SceneTexture {
    std::string name;
    std::string source;       // bundle it came from (provenance)
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // top row first
    bool clamp = false;          // the texture's wrap mode is clamp (ramps), not repeat
    uint64_t pixelHash = 0;      // FNV-1a 64 of rgba
};

struct SceneTexBinding {
    std::string name;            // Unity property, e.g. "_MainTex"
    int texture = -1;            // index into Scene::textures
    Vec4 st{1, 1, 0, 0};         // scale.xy, offset.xy
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
    // Everything the game's shader reads: the material's colours and floats (floats in .x) and all its textures, plus
    // the compiled shader (index into Scene::shaders, -1 when the shader is not in the bundle).
    std::vector<std::pair<std::string, Vec4>> props;
    std::vector<SceneTexBinding> texProps;
    int shaderIndex = -1;
    bool bakedLit = false;                   // the texture already holds the shader's lighting: write the material unlit
};

// A texture the game builds at run time by drawing one full-screen quad through a "create" material's shader (eye iris and
// white, face and body skin: Graphics.Blit into a render texture). `material` carries the shader, colours and input textures;
// the result is written into Scene::textures[texture] (a placeholder until then) by export::RunCreateJobs.
struct SceneCreateJob {
    SceneMaterial material;
    int texture = -1;            // index into Scene::textures that receives the result
    int width = 512, height = 512;
    // An image laid over the result (a KoiSkinOverlayX layer from the card), source-over, scaled to the target; empty = none.
    std::vector<uint8_t> overlay;
    int overlayWidth = 0, overlayHeight = 0;
};

struct SceneShader {
    std::string name;            // e.g. "Shader Forge/main_opaque"
    std::vector<uint8_t> blob;   // decompressed Direct3D 11 program blob
    std::vector<std::pair<std::string, std::string>> texDefaults;   // texture property -> default ("white", "black", "bump", "gray")
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
    std::vector<SceneShader> shaders;
    std::vector<SceneCreateJob> createJobs;
    // Shader globals the game sets for every material (Shader.SetGlobalTexture / SetGlobalColor / SetGlobalFloat):
    // the shadow ramp, the ambient shadow colour, the outline colour and width.
    std::vector<SceneTexBinding> globalTextures;
    std::vector<std::pair<std::string, Vec4>> globalProps;
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
