#include "export/ModelExport.h"
#include "export/Exporter.h"
#include "gltf/GltfWriter.h"
#include "obj/ObjWriter.h"

#include <algorithm>
#include <cctype>

namespace cardbridge::exporter {

const char* FormatName(ExportFormat format) {
    switch (format) {
        case ExportFormat::Pmx: return "pmx";
        case ExportFormat::Glb: return "glb";
        case ExportFormat::Gltf: return "gltf";
        case ExportFormat::Obj: return "obj";
    }
    return "pmx";
}

const char* FormatExtension(ExportFormat format) {
    switch (format) {
        case ExportFormat::Pmx: return ".pmx";
        case ExportFormat::Glb: return ".glb";
        case ExportFormat::Gltf: return ".gltf";
        case ExportFormat::Obj: return ".obj";
    }
    return ".pmx";
}

bool ParseExportFormat(const std::string& text, ExportFormat& out) {
    std::string s = text;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "pmx" || s == "mmd") { out = ExportFormat::Pmx; return true; }
    if (s == "glb" || s == "gltf-binary" || s == "gltf2") { out = ExportFormat::Glb; return true; }
    if (s == "gltf" || s == "gltf-json" || s == "gltf-separate") { out = ExportFormat::Gltf; return true; }
    if (s == "obj" || s == "obj-mtl" || s == "wavefront") { out = ExportFormat::Obj; return true; }
    return false;
}

ModelExportResult ExportModel(const assemble::Scene& scene, const card::Card& card, const ModelExportOptions& opt) {
    ModelExportResult result;
    switch (opt.format) {
        case ExportFormat::Pmx: {
            // Keep the established PMX path byte-for-byte: only the general options are forwarded.
            ExportOptions eo;
            eo.outDir = opt.outDir;
            eo.baseName = opt.baseName;
            eo.overwrite = opt.overwrite;
            eo.log = opt.log;
            eo.progress = opt.progress;
            eo.cancelled = opt.cancelled;
            ExportResult r = ExportScene(scene, card, eo);
            result.folder = r.folder;
            result.primaryPath = r.pmxPath;
            result.manifestPath = r.manifestPath;
            result.vertices = r.vertices;
            result.triangles = r.triangles;
            result.materials = r.materials;
            result.bones = r.bones;
            result.textures = r.textures;
            result.parts = r.parts;
            break;
        }
        case ExportFormat::Glb:
            gltf::WriteGlb(scene, card, opt, result);
            break;
        case ExportFormat::Gltf:
            gltf::WriteGltf(scene, card, opt, result);
            break;
        case ExportFormat::Obj:
            obj::WriteObj(scene, card, opt, result);
            break;
    }
    return result;
}

} // namespace cardbridge::exporter
