#pragma once

#include "core/Bytes.h"
#include "core/Value.h"
#include "unity/UnityBundle.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::unity {

// Unity class IDs this reader cares about.
namespace cls {
constexpr int GameObject = 1;
constexpr int Transform = 4;
constexpr int Material = 21;
constexpr int MeshRenderer = 23;
constexpr int Texture2D = 28;
constexpr int MeshFilter = 33;
constexpr int Mesh = 43;
constexpr int Shader = 48;
constexpr int TextAsset = 49;
constexpr int MonoBehaviour = 114;
constexpr int SkinnedMeshRenderer = 137;
} // namespace cls

struct TypeNode {
    std::string type;
    std::string name;
    uint8_t level = 0;
    bool isArray = false;
    int32_t byteSize = 0;
    int32_t metaFlags = 0;
    std::vector<int> children;   // indices into the same type's node list
};

struct SerializedType {
    int classId = 0;
    std::vector<TypeNode> nodes;   // nodes[0] is the root; empty if the file has no type tree
};

struct ObjectInfo {
    int64_t pathId = 0;
    uint64_t byteStart = 0;
    uint32_t byteSize = 0;
    int typeIndex = -1;
    int classId = 0;
};

// One serialized file (a bundle's "CAB-xxx" node): the object table and a
// type-tree interpreter that turns any object into a Value tree - the same
// approach UnityPy takes, which is why this reads game classes (Mesh,
// Material, MonoBehaviour) without any of their C# definitions.
class SerializedFile {
public:
    SerializedFile(std::shared_ptr<UnityBundle> bundle, const UnityBundle::Node& node);   // throws FormatError

    int formatVersion() const { return version_; }
    const std::string& unityVersion() const { return unityVersion_; }
    const std::vector<std::string>& externals() const { return externals_; }
    const std::vector<ObjectInfo>& objects() const { return objects_; }
    const ObjectInfo* find(int64_t pathId) const;
    std::vector<const ObjectInfo*> ofClass(int classId) const;

    Value read(const ObjectInfo& o) const;                  // throws if the file has no type tree for it
    // The object's m_Name without decoding the rest (which for a texture is megabytes). Valid for
    // classes whose first serialized field is the name: Texture2D, Material, Shader, Mesh, TextAsset.
    std::string peekName(const ObjectInfo& o) const;
    const UnityBundle& bundle() const { return *bundle_; }
    std::shared_ptr<UnityBundle> bundlePtr() const { return bundle_; }

private:
    Value readNode(ByteReader& r, const SerializedType& t, int nodeIdx) const;

    std::shared_ptr<UnityBundle> bundle_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    int version_ = 0;
    uint32_t dataOffset_ = 0;
    bool bigEndian_ = false;
    std::string unityVersion_;
    std::vector<SerializedType> types_;
    std::vector<ObjectInfo> objects_;
    std::map<int64_t, size_t> byId_;
    std::vector<std::string> externals_;
};

} // namespace cardbridge::unity
