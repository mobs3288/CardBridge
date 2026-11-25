#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"

#include <functional>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// One user-facing format choice. PMX is deliberately first in the enum and is the CLI default:
// the existing PMX pipeline is never routed through a new writer.
enum class ExportFormat { Pmx, Glb, Gltf, Obj };

// Lower-case names accepted by --format and the viewer's format menu.
const char* FormatName(ExportFormat format);
const char* FormatExtension(ExportFormat format);
bool ParseExportFormat(const std::string& text, ExportFormat& out);

// Options shared by every model exporter. Format-irrelevant settings are not shown for a format,
// but they all live in one struct so the CLI, the viewer and tests drive the same code path.
struct ModelExportOptions {
    ExportFormat format = ExportFormat::Glb;
    std::string outDir;            // created; must be empty or absent unless `overwrite`
    std::string baseName;          // file stem; defaults to the character name
    bool overwrite = false;

    bool exportTextures = true;    // GLB embeds them, glTF/OBJ write PNGs beside the model
    bool exportArmature = true;    // glTF only
    bool exportSkinning = true;    // glTF only (requires exportArmature)
    bool exportMorphs = true;      // glTF only
    bool exportAnimation = false;  // no animation data exists in Scene; true is rejected
    bool separateObjects = true;   // OBJ: one object per mesh instead of one combined object
    bool exportMtl = true;         // OBJ only

    float scale = 1.0f;            // metres per scene unit; glTF/OBJ only (PMX keeps 12.5)

    std::function<void(const std::string&)> log;
    std::function<void(double, const std::string&)> progress;
    std::function<bool()> cancelled;
};

struct ModelExportResult {
    std::string folder, primaryPath, manifestPath;
    size_t vertices = 0, triangles = 0, materials = 0, bones = 0, textures = 0, morphs = 0, parts = 0;
    // Named warnings for data that cannot be represented in the target format. They are also
    // written into the manifest, so a user can audit what was lost without reading a log.
    std::vector<std::string> warnings;
};

// Dispatches to the existing PMX exporter (unchanged) or the glTF/OBJ writers. Both new writers
// consume only `scene`; `card` is provenance for the manifest. Throws FormatError on IO or
// consistency errors and Cancelled when the cancellation callback returns true.
ModelExportResult ExportModel(const assemble::Scene& scene, const card::Card& card, const ModelExportOptions& opt);

} // namespace cardbridge::exporter
