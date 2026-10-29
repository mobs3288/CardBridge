#pragma once

#include "core/Math.h"
#include "unity/UnitySerialized.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cardbridge::unity {

struct PPtr {
    int32_t fileId = 0;     // 0 = this serialized file, n = externals()[n-1]
    int64_t pathId = 0;
    bool isNull() const { return pathId == 0; }
    bool isLocal() const { return fileId == 0; }
};

struct BoneInfluence { int idx[4] = {0, 0, 0, 0}; float w[4] = {0, 0, 0, 0}; };

// A decoded Unity Mesh: float vertex streams plus triangle lists per submesh.
// Only the channels CardBridge uses are kept (position, normal, uv0, colour,
// skin weights, bind poses).
struct MeshData {
    std::string name;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uv0;
    std::vector<BoneInfluence> skin;      // empty if the mesh is not skinned
    std::vector<Mat4> bindPoses;
    std::vector<std::vector<uint32_t>> subMeshTriangles;   // 3 indices per triangle
    size_t vertexCount() const { return positions.size(); }
};

struct TextureData {
    int wrapMode = 0;            // Unity m_WrapMode: 0 repeat, 1 clamp, 2 mirror, 3 mirror once
    std::string name;
    int width = 0, height = 0;
    int unityFormat = 0;
    std::vector<uint8_t> rgba;            // width*height*4, top row first (PNG order)
};

struct MaterialData {
    std::string name;
    std::string shaderName;
    std::vector<std::pair<std::string, PPtr>> textures;     // property -> texture
    std::vector<std::pair<std::string, Vec4>> colors;
    std::vector<std::pair<std::string, float>> floats;
    std::vector<std::pair<std::string, Vec4>> texST;        // texture property -> (scale.xy, offset.xy)
    int64_t shaderPathId = 0;                               // the Shader object in the same bundle, 0 if external
    const PPtr* texture(const std::string& prop) const {
        for (auto& t : textures) if (t.first == prop) return &t.second;
        return nullptr;
    }
    const Vec4* color(const std::string& prop) const {
        for (auto& c : colors) if (c.first == prop) return &c.second;
        return nullptr;
    }
};

struct Xform {
    int64_t id = 0;               // Transform object id
    int64_t goId = 0;
    std::string name;             // the GameObject's name
    Vec3 pos;
    Quat rot;
    Vec3 scl{1, 1, 1};
    int64_t parent = 0;           // Transform id, 0 for a root
    std::vector<int64_t> children;
};

struct RendererInfo {
    bool skinned = false;
    bool enabled = true;
    bool active = true;   // the GameObject and every ancestor are active (Unity draws nothing under an inactive object)
    int64_t goId = 0;
    int64_t xformId = 0;
    int64_t componentId = 0;   // the renderer component's object id (ChaClothesComponent lists refer to it)
    std::string name;
    PPtr mesh;
    std::vector<PPtr> materials;
    std::vector<int64_t> boneXforms;   // SkinnedMeshRenderer.m_Bones resolved to Transform ids (0 = missing)
};

struct MonoInfo {
    std::string className;     // from the MonoScript, e.g. "ChaClothesComponent"
    int64_t goId = 0;
    Value fields;              // the MonoBehaviour's serialized fields
};

// The loaded view of one bundle: its GameObject/Transform graph and typed
// accessors for the object kinds the assembler needs.
class UnityFile {
public:
    explicit UnityFile(std::shared_ptr<UnityBundle> bundle);   // throws FormatError

    SerializedFile& serialized() { return *sf_; }
    const SerializedFile& serialized() const { return *sf_; }
    const std::string& name() const { return sf_->bundle().name(); }

    const Xform* xform(int64_t id) const;
    const std::map<int64_t, Xform>& xforms() const { return xforms_; }
    // Transform ids of every GameObject with this name (a prefab root is usually unique).
    std::vector<int64_t> findXformsByName(const std::string& goName) const;
    // `localOverride` replaces the local matrix of the listed transforms (accessory move nodes, shape bones).
    Mat4 worldMatrix(int64_t xformId, const std::map<int64_t, Mat4>* localOverride = nullptr) const;
    // This transform and all its descendants, parent before child.
    std::vector<int64_t> subtree(int64_t rootXformId) const;
    // Path of names from the root, e.g. "cf_j_root/cf_n_height" (for diagnostics).
    std::string pathOf(int64_t xformId) const;

    // Renderers whose GameObject is in the subtree of `rootXformId`.
    std::vector<RendererInfo> renderersUnder(int64_t rootXformId) const;

    // Every MonoBehaviour on a GameObject in the subtree, with its class name resolved.
    std::vector<MonoInfo> monoBehavioursUnder(int64_t rootXformId) const;
    // A Texture2D in this bundle by its name (null PPtr if there is none).
    PPtr findTexture(const std::string& name) const;
    // An asset by the name the game loads it with: the AssetBundle container key's file stem,
    // case-insensitive (this is NOT the object's own m_Name - e.g. container 'Theresa_C6' holds a texture named 'Lor').
    PPtr findAsset(const std::string& name) const;
    // True if `xformId` is the transform of, or a descendant of, any GameObject in `goIds`.
    bool isUnderGameObject(int64_t xformId, const std::vector<int64_t>& goIds) const;

    std::optional<MeshData> readMesh(const PPtr& p, std::string* why = nullptr) const;
    std::optional<MaterialData> readMaterial(const PPtr& p, std::string* why = nullptr) const;
    std::optional<TextureData> readTexture(const PPtr& p, std::string* why = nullptr) const;
    // The external file name a PPtr with fileId > 0 points into (empty when local).
    std::string externalNameOf(const PPtr& p) const { return p.fileId > 0 ? externalName(p) : std::string(); }
    // The decompressed Direct3D 11 program blob of a Shader object (what tools/extract_shaders.py writes).
    std::optional<Bytes> readShaderBlob(int64_t shaderPathId, std::string* why = nullptr) const;
    // The shader's texture properties with their default texture ("white", "black", "bump", "gray"): what Unity binds when a material leaves the slot empty.
    std::vector<std::pair<std::string, std::string>> readShaderTextureDefaults(int64_t shaderPathId) const;

    // Texts stored as TextAsset objects (the base game's list tables).
    std::vector<std::pair<std::string, Bytes>> readTextAssets() const;

private:
    std::string externalName(const PPtr& p) const;

    std::shared_ptr<UnityBundle> bundle_;
    std::unique_ptr<SerializedFile> sf_;
    std::map<int64_t, Xform> xforms_;
    std::map<int64_t, int64_t> goToXform_;
    std::map<int64_t, std::string> goNames_;
    std::map<int64_t, bool> goActive_;   // goId -> m_IsActive
    std::map<int64_t, std::vector<std::pair<int, int64_t>>> goComponents_;   // goId -> (classId, componentId)
};

} // namespace cardbridge::unity
