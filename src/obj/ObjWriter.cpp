#include "obj/ObjWriter.h"
#include "export/FormatCommon.h"
#include "core/FileIo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace cardbridge::obj {

namespace {

using json = nlohmann::json;
using namespace cardbridge::exportcommon;

std::string F(float v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(7) << (std::isfinite(v) ? v : 0.0f);
    return os.str();
}

void AddWarnings(exporter::ModelExportResult& result, const assemble::Scene& scene, const exporter::ModelExportOptions& opt) {
    bool hasShapes = false, hasWeights = false, hasMask = false;
    for (const auto& mesh : scene.meshes) {
        hasShapes = hasShapes || !mesh.shapes.empty();
        for (const auto& v : mesh.vertices)
            hasWeights = hasWeights || (v.weight[1] > 0.0f || v.weight[2] > 0.0f || v.weight[3] > 0.0f);
    }
    for (const auto& mat : scene.materials) {
        float cutoff = 0.5f;
        if (ClassifyAlpha(scene, mat, cutoff) == AlphaMode::Mask) hasMask = true;
    }
    if (hasWeights) result.warnings.push_back("OBJ is a static format: bone weights and armature are not exported");
    if (hasShapes) result.warnings.push_back("OBJ is a static format: morph targets / expressions are not exported");
    if (hasMask) result.warnings.push_back("OBJ/MTL has no alpha-test mode: cutout materials are written with d=1; transparency may differ in Blender");
    if (opt.exportAnimation) result.warnings.push_back("no animation source data exists in the canonical character; --animation ignored");
    result.warnings.push_back("Koikatsu toon shading is baked into textures; OBJ/MTL carries only a diffuse approximation");
}

void BuildManifest(const std::string& outDir, const std::string& file, const std::string& mtlFile,
                   const exporter::ModelExportOptions& opt, const exporter::ModelExportResult& result) {
    json j = {
        {"schema_version", 1},
        {"export", {{"format", "obj"}, {"file", file}, {"mtl", mtlFile},
                    {"axes", "y-up, right-handed"}, {"scale", opt.scale},
                    {"uv", "v flipped to top-left origin"}, {"facing", "+z"},
                    {"textures_dir", "textures"}}},
        {"counts", {{"vertices", result.vertices}, {"triangles", result.triangles},
                    {"materials", result.materials}, {"textures", result.textures}}},
        {"warnings", result.warnings}
    };
    WriteAtomicText(fileio::Join(outDir, "cardbridge-export.json"), j.dump(2) + "\n", opt.overwrite);
}

} // namespace

void WriteObj(const assemble::Scene& scene, const card::Card& card,
              const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result) {
    (void)card;
    fileio::CreateDirectories(opt.outDir);
    result.folder = opt.outDir;
    AddWarnings(result, scene, opt);
    const float scale = opt.scale > 0.0f && std::isfinite(opt.scale) ? opt.scale : 1.0f;
    const std::string stem = SafeStem(opt.baseName.empty() ? scene.name : opt.baseName);
    const std::string mtlName = stem + ".mtl";

    TextureWriter textures(scene, opt.outDir, opt.exportTextures, opt.overwrite);
    std::vector<int> matMap(scene.materials.size(), -1);
    std::vector<std::string> matNames;
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& mat = scene.materials[i];
        const int tex = textures.Use(mat.texture);
        if (opt.exportMtl || tex >= 0) {
            matMap[i] = static_cast<int>(matNames.size());
            matNames.push_back(Token(mat.name.empty() ? ("material_" + std::to_string(i)) : mat.name));
        }
    }

    std::ostringstream obj;
    obj << "# CardBridge " << CARDBRIDGE_VERSION << " - static OBJ export from the assembled character\n";
    if (opt.exportMtl && !matNames.empty()) obj << "mtllib " << mtlName << "\n";
    obj << std::fixed << std::setprecision(7);

    size_t vertexOffset = 1;   // OBJ indices are 1-based and shared across all objects
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        CheckCancelled(opt.cancelled);
        const auto& mesh = scene.meshes[mi];
        if (mesh.vertices.empty()) continue;
        bool any = false;
        for (const auto& sm : mesh.submeshes) any = any || sm.indices.size() >= 3;
        if (!any) continue;

        const std::string name = mesh.name.empty() ? ("mesh_" + std::to_string(mi)) : mesh.name;
        if (opt.separateObjects) obj << "o " << Token(name) << "\n";
        obj << "g " << Token(name) << "\n";

        const size_t base = vertexOffset;
        std::vector<Vec3> pos(mesh.vertices.size()), nrm(mesh.vertices.size());
        std::vector<Vec2> uv(mesh.vertices.size());
        for (size_t v = 0; v < mesh.vertices.size(); ++v) {
            pos[v] = Point(mesh.vertices[v].pos, scale);
            nrm[v] = Direction(mesh.vertices[v].normal);
            uv[v] = {mesh.vertices[v].uv.x, 1.0f - mesh.vertices[v].uv.y};
            obj << "v " << F(pos[v].x) << ' ' << F(pos[v].y) << ' ' << F(pos[v].z) << '\n';
        }
        for (const auto& t : uv) obj << "vt " << F(t.x) << ' ' << F(t.y) << '\n';
        for (const auto& n : nrm) obj << "vn " << F(n.x) << ' ' << F(n.y) << ' ' << F(n.z) << '\n';

        for (const auto& sm : mesh.submeshes) {
            if (sm.indices.size() < 3) continue;
            if (sm.material >= 0 && static_cast<size_t>(sm.material) < matMap.size() && matMap[static_cast<size_t>(sm.material)] >= 0)
                obj << "usemtl " << matNames[static_cast<size_t>(matMap[static_cast<size_t>(sm.material)])] << '\n';
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                // Mirror of X reverses orientation; swap corners so normals stay outward.
                const uint32_t a = sm.indices[i] + static_cast<uint32_t>(base);
                const uint32_t b = sm.indices[i + 2] + static_cast<uint32_t>(base);
                const uint32_t c = sm.indices[i + 1] + static_cast<uint32_t>(base);
                obj << "f " << a << '/' << a << '/' << a << ' '
                    << b << '/' << b << '/' << b << ' '
                    << c << '/' << c << '/' << c << '\n';
                ++result.triangles;
            }
        }
        result.vertices += mesh.vertices.size();
        vertexOffset += mesh.vertices.size();
    }

    // MTL after the geometry, so the material list reflects only materials that were used.
    std::ostringstream mtl;
    mtl << "# CardBridge " << CARDBRIDGE_VERSION << " - material library\n";
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        if (matMap[i] < 0 || static_cast<size_t>(matMap[i]) >= matNames.size()) continue;
        const auto& mat = scene.materials[i];
        float cutoff = 0.5f;
        const AlphaMode alpha = ClassifyAlpha(scene, mat, cutoff);
        const Vec4 kd = {std::clamp(mat.diffuse.x, 0.0f, 1.0f), std::clamp(mat.diffuse.y, 0.0f, 1.0f),
                         std::clamp(mat.diffuse.z, 0.0f, 1.0f), std::clamp(mat.diffuse.w, 0.0f, 1.0f)};
        mtl << "newmtl " << matNames[static_cast<size_t>(matMap[i])] << '\n';
        mtl << "Ka " << F(kd.x * mat.shadow.x) << ' ' << F(kd.y * mat.shadow.y) << ' ' << F(kd.z * mat.shadow.z) << '\n';
        mtl << "Kd " << F(kd.x) << ' ' << F(kd.y) << ' ' << F(kd.z) << '\n';
        mtl << "Ks " << F(mat.specular.x) << ' ' << F(mat.specular.y) << ' ' << F(mat.specular.z) << '\n';
        mtl << "Ns " << F(std::clamp(mat.specularPower * 4.0f, 0.0f, 1000.0f)) << '\n';
        mtl << "d " << F(alpha == AlphaMode::Blend ? kd.w : 1.0f) << '\n';
        mtl << "illum " << (mat.specular.x + mat.specular.y + mat.specular.z > 1e-5f ? 2 : 1) << '\n';

        const int tex = textures.Use(mat.texture);
        if (tex >= 0) mtl << "map_Kd " << textures.Entries()[static_cast<size_t>(tex)].file << '\n';
        for (const auto& b : mat.texProps) {
            std::string lower = b.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if ((lower.find("bump") != std::string::npos || lower.find("normal") != std::string::npos) && b.texture >= 0) {
                const int bump = textures.Use(b.texture);
                if (bump >= 0) mtl << "map_Bump " << textures.Entries()[static_cast<size_t>(bump)].file << '\n';
                break;
            }
        }
        ++result.materials;
    }

    textures.Flush();
    result.textures = textures.Entries().size();
    const std::string objFile = stem + ".obj";
    WriteAtomicText(fileio::Join(opt.outDir, objFile), obj.str(), opt.overwrite);
    if (opt.exportMtl && !matNames.empty())
        WriteAtomicText(fileio::Join(opt.outDir, mtlName), mtl.str(), opt.overwrite);
    BuildManifest(opt.outDir, objFile, opt.exportMtl ? mtlName : std::string(), opt, result);
    result.primaryPath = fileio::Join(opt.outDir, objFile);
    result.manifestPath = fileio::Join(opt.outDir, "cardbridge-export.json");
    for (const auto& part : scene.parts)
        if (part.includeInExport) ++result.parts;
}

} // namespace cardbridge::obj
