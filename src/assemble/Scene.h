#pragma once

#include "core/Math.h"

#include <array>
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
    Mat4 world;               // rest world matrix (position, rotation and scale as the game had it); what a re-shape moves the vertices from
};

struct SceneVertex {
    Vec3 pos, normal;
    Vec2 uv;                  // Unity convention (v up); the PMX writer flips v
    Vec2 uv1;                 // the second UV set (TEXCOORD1: the body's nipple placement); equals uv when the mesh has none
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    // The vertex and its normal in the frame of each bone that moves it (pos = sum of weight * bone world * local), so new bone worlds
    // from a changed body shape give the new vertex without assembling again (assemble::Reshape). Valid when SceneMesh::reshapable.
    Vec3 local[4], nlocal[4];
};

struct SceneTexture {
    std::string name;
    std::string source;       // bundle it came from (provenance)
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // top row first
    bool clamp = false;          // the texture's wrap mode is clamp (ramps), not repeat
    bool srgb = true;            // the texture is flagged sRGB (colour data); false for masks and other linear data
    uint64_t pixelHash = 0;      // FNV-1a 64 of rgba
};

struct SceneTexBinding {
    std::string name;            // Unity property, e.g. "_MainTex"
    int texture = -1;            // index into Scene::textures
    Vec4 st{1, 1, 0, 0};         // scale.xy, offset.xy
};

constexpr int kToonRows = 32;   // rows of SceneMaterial::toon

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
    // The game shader's shade as a function of N.L, measured by BakeShaderTextures: kToonRows rows from N.L = 1 (top, fully lit) to
    // -1 (bottom), each the colour factor the shader gives that light angle relative to the baked front light. Empty = none.
    std::vector<Vec3> toon;
    bool bakedLit = false;                   // the texture already holds the shader's lighting: write the material unlit
    bool cardAreola = false;                 // its _nipsize is the card's areola size, not the material's own default (a re-shape follows the slider)
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
    // The game runs the draw in linear light: sRGB-flagged textures are decoded when sampled, the colour properties are linear
    // values and the result is encoded when it is written (clothes). Otherwise everything is combined as stored.
    bool linear = false;
    bool overlayReplaces = false;   // the overlay replaces the result instead of covering it (KoiClothesOverlayX "override")
};

struct SceneShader {
    std::string name;            // e.g. "Shader Forge/main_opaque"
    std::vector<uint8_t> blob;   // decompressed Direct3D 11 program blob
    std::vector<std::pair<std::string, Vec4>> propDefaults;   // colour, vector and float defaults the shader declares
    std::vector<std::pair<std::string, std::string>> texDefaults;   // texture property -> default ("white", "black", "bump", "gray")
};

struct SceneSubmesh {
    int material = 0;
    std::vector<uint32_t> indices;   // triangles, into SceneMesh::vertices
};

// A blend shape of a mesh (Unity's m_Shapes), as model-space offsets of the vertices at weight 100 (skinning applied to the delta).
// `rest` is the weight (0..100) already built into SceneMesh::vertices, the resting expression the game plays.
struct SceneBlendShape {
    std::string name;
    float rest = 0.0f;
    std::vector<std::pair<uint32_t, Vec3>> offsets;   // vertex index, offset
    // Each offset in the frame of the bones that move its vertex (as SceneVertex::local does for positions), so a re-shape that moves
    // the bones moves the offsets too (assemble::Reshape). Same length as `offsets` when the mesh is reshapable, else empty.
    std::vector<std::array<Vec3, 4>> local;
};

struct SceneMesh {
    std::string name;
    int part = -1;
    std::vector<SceneVertex> vertices;
    std::vector<SceneSubmesh> submeshes;
    std::vector<SceneBlendShape> shapes;
    bool reshapable = false;          // the vertices carry local / nlocal
};

// An expression the game's FaceBlendShape component plays: eyebrow, eye or mouth pattern in its closed or open state, as the
// blend shape weights it sets on the face meshes. Exported as a vertex morph (offsets relative to the resting expression).
struct SceneExpression {
    struct Target { int mesh = -1; int shape = -1; float weight = 0.0f; };
    std::string name;
    int panel = 4;                    // MMD panel: 1 brows, 2 eyes, 3 mouth, 4 other
    std::vector<Target> targets;
};

// A DynamicBone chain (hair strand, skirt, ribbon, breast and hip jiggle): bones[0] is the anchor that follows the body, the
// rest hang from it; each bone's parent in Scene::bones is its predecessor. Parameters are the component's, 0..1 as the game has them.
struct SceneChain {
    std::string name, part;
    std::vector<int> bones;
    float damping = 0.1f, elasticity = 0.1f, stiffness = 0.1f, inert = 0.0f;
    // The same three per bone (parallel to `bones`), with the component's distribution curves applied along the chain (the curve is read at
    // the bone's distance from the root over the chain's length, as the component does): a ponytail with stiffness 1 near the root and 0 at
    // the tip is stiff where it is tied and free where it hangs. Empty = no curves, the chain-wide values hold.
    std::vector<float> boneDamping, boneElasticity, boneStiffness;
    float radius = 0.0f;              // collision radius in model units (0 = none given)
    bool characterOwn = false;        // the character's own Ver02 component (breast and hip jiggle: skin, not cloth or hair)
    Vec3 gravity, force;
};

// A DynamicBoneCollider the chains collide with: a sphere or capsule fixed to a body bone.
struct SceneCollider {
    std::string name;
    int bone = -1;
    Vec3 center;                      // model space
    Vec3 axis{0, 1, 0};               // capsule axis, model space (unit)
    float radius = 0.0f, height = 0.0f;   // height 0 or <= 2 radius: a sphere
    bool inside = false;              // keeps the chain inside (not exported as a body)
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
    // The nude body. A garment's body mask hides the triangles under it (as the game does); those triangles are not thrown away but
    // kept as an "under piece" part so the skin can be shown again: a piece is hidden by default (visibleInPreview false), lists the
    // garments that cover it in coveredBy, and is shown when none of them is (or when the nude switch is on). "" in coveredBy is a
    // garment the game hides itself (it still covers). The same is done for the bra, which a top's mask cuts.
    bool underPiece = false;
    std::string underOf;                    // id of the part the piece was cut from
    std::vector<std::string> coveredBy;     // part ids of the garments over it
    bool nudeSwitch = false;                // the "Nude body" checkbox: no geometry, switches every under piece on
    // Export: the part's PMX materials start fully transparent (the viewer sets this from its checkboxes); the PMX morphs bring them back.
    bool startHidden = false;
};

// What BakePose wrote into the scene as its new rest pose, for the manifest.
struct ScenePoseEntry {
    std::string bone;
    Vec3 pos, rot, scl;      // local edit: translation, Euler degrees, scale
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
    std::vector<SceneExpression> expressions;
    std::vector<SceneChain> chains;
    std::vector<SceneCollider> colliders;
    std::vector<ScenePoseEntry> pose;   // the bone edits baked into this scene (see BakePose), empty for the rest pose
    bool aposeApplied = false;          // BakePose included the automatic A-pose
    bool chainsSettled = false;         // an item's DynamicBone chains were run to rest (CARDBRIDGE_DYNBONE): they depend on the body's shape, so Reshape cannot redo them
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
