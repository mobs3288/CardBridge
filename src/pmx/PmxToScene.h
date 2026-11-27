#pragma once

#include "assemble/Scene.h"
#include "pmx/PmxWriter.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

// A PMX imported into the shared scene representation used by the viewer,
// renderer, posing and the editing tools. `warnings` explains anything the
// scene cannot express (bones/flip/impulse morphs, physics, extra UV 4, ...);
// the original document still holds all of it verbatim.
struct PmxLoadResult {
    assemble::Scene scene;
    std::vector<std::string> warnings;
};

// Builds a scene from an imported document. `pmxDir` is the folder the .pmx was
// read from; texture paths in the document are resolved relative to it.
PmxLoadResult BuildScene(const Model& doc, const std::string& pmxDir);

// Copies the editable material fields of `scene` back into a document that was
// imported (or a copy of one). Source-only fields (toon/sphere references,
// memos, morphs, bones, physics, frames, format) are left untouched.
void ApplySceneMaterials(const assemble::Scene& scene, Model& doc);

// Writes `doc` plus copies of its textures into `outDir/<baseName>.pmx` and
// `outDir/textures/`. Texture paths in the written PMX are relative again, so
// the exported folder is self-contained. Returns the written .pmx path.
std::string ExportDocument(const Model& doc, const std::string& pmxDir,
                           const std::string& outDir, const std::string& baseName);

// Same export, with in-memory encoded texture bytes for selected texture
// indices (empty entry = use the disk-backed path). Names are collision-safe
// and case-insensitive; duplicate content is written only once. The 4-arg
// overload keeps the original disk-backed behaviour byte-for-byte.
std::string ExportDocument(const Model& doc, const std::string& pmxDir,
                           const std::string& outDir, const std::string& baseName,
                           const std::vector<Bytes>& textureOverrides);

} // namespace cardbridge::pmx
