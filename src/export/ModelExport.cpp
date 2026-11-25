#include "export/ModelExport.h"
#include "export/Exporter.h"
#include "core/Bytes.h"
#include "gltf/GltfWriter.h"
#include "obj/ObjWriter.h"
#include "mesh/Simplify.h"

#include <algorithm>
#include <cctype>
#include <memory>

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
    // The new formats share the PMX simplification path: it works on a deep copy, so the
    // caller's scene (and the live viewer) is never simplified. Blend shapes and weights
    // travel with the surviving vertices.
    std::shared_ptr<assemble::Scene> simplified;
    const assemble::Scene* source = &scene;
    if (opt.format != ExportFormat::Pmx && opt.simplifyPercent > 0.0f) {
        const float requested = std::clamp(opt.simplifyPercent, 0.0f, 95.0f) / 100.0f;
        mesh::SimplifyOptions so;
        so.maxReduction = requested;
        so.cancelled = opt.cancelled;
        mesh::SceneSimplifyStats st;
        simplified = mesh::SimplifyScene(scene, requested, so, st,
            opt.progress ? std::function<void(float, const std::string&)>([&opt](float f, const std::string& s) {
                opt.progress(f * 0.45, "simplifying: " + s);
            }) : std::function<void(float, const std::string&)>{});
        for (const auto& w : st.warnings) {
            result.warnings.push_back(w);
            if (opt.log) opt.log("warning: " + w);
        }
        if (!simplified) {
            if (opt.cancelled && opt.cancelled()) throw Cancelled{};
            throw FormatError("simplification failed");
        }
        if (opt.log) opt.log("simplified for export: " + std::to_string(st.originalTriangles) +
                             " -> " + std::to_string(st.resultTriangles) + " triangles");
        source = simplified.get();
    }
    const assemble::Scene& model = *source;
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
            gltf::WriteGlb(model, card, opt, result);
            break;
        case ExportFormat::Gltf:
            gltf::WriteGltf(model, card, opt, result);
            break;
        case ExportFormat::Obj:
            obj::WriteObj(model, card, opt, result);
            break;
    }
    return result;
}

} // namespace cardbridge::exporter
