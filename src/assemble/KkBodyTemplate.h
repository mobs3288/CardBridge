#pragma once

#include "core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::assemble {

// A skinned body mesh extracted from a KK/KKS base library, with no card and
// no character loaded. Coordinates are the bundle's rest pose; skin weights
// refer to `boneNames` by index.
struct BodyVertex {
    Vec3 pos, normal;
    Vec2 uv;
    int bone[4] = {-1, -1, -1, -1};
    float weight[4] = {0, 0, 0, 0};
};

struct BodyTemplate {
    std::string source;                       // library::GameInfo::name()
    std::vector<BodyVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::string> boneNames;       // renderer bone order
    std::vector<Vec3> bonePositions;          // rest-world translation of each bone, parallel to boneNames
    std::vector<std::string> warnings;
    size_t size() const { return vertices.size(); }
};

struct BodyTemplateOptions {
    std::string libraryRoot;
    std::string modsRoot;
    std::string modCachePath;
};

// Extracts the body prefab from <library>/abdata/chara/oo_base.unity3d.
// Both KK (m_Skin) and KKS (blend channels 12/13) skinning are decoded by the
// shared UnityScene reader. Returns false with `error` when the bundle or the
// body prefab is missing. Nothing here needs a card::Card.
bool LoadBodyTemplate(const BodyTemplateOptions& options, BodyTemplate& out, std::string& error);

} // namespace cardbridge::assemble
