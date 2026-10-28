#include "export/Exporter.h"

#include "core/FileIo.h"
#include "export/Preview.h"
#include "pmx/PmxWriter.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace cardbridge::exporter {

namespace {

std::string safeFileName(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') out.push_back(static_cast<char>(c));
        else out.push_back('_');
    }
    if (out.empty()) out = "unnamed";
    if (out.size() > 80) out.resize(80);
    return out;
}

Bytes encodePng(const assemble::SceneTexture& t) {
    Bytes out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* b = static_cast<Bytes*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        b->insert(b->end(), p, p + size);
    };
    if (!stbi_write_png_to_func(cb, &out, t.width, t.height, 4, t.rgba.data(), t.width * 4))
        throw FormatError("PNG encode failed for texture " + t.name);
    return out;
}

} // namespace

nlohmann::json BuildManifest(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt,
                             const std::string& pmxFile, const std::vector<std::string>& textureFiles,
                             const std::vector<std::pair<int, int>>& range) {
    using nlohmann::json;
    json j;
    j["schema_version"] = 1;
    j["tool"] = {{"name", "cardbridge"}, {"version", "0.1.0-m1"}};
    j["input"] = {{"filename", fileio::FileName(card.path)}, {"kind", "koikatsu_character_card"}, {"sha256", card.sha256},
                  {"family", card::FamilyName(card.family)}, {"marker", card.marker}, {"format_version", card.formatVersion}};
    j["character"] = {{"name", scene.name}, {"outfit", opt.outfit}};
    j["game_adapter"] = "koikatsu";
    j["provenance"] = {{"asset_library", opt.assetLibrary}, {"mods_folder", opt.modsFolder}};
    j["export"] = {{"format", "pmx"}, {"file", pmxFile}, {"scale", opt.scale}, {"axes", "y-up, left-handed (as Unity and PMX)"},
                   {"uv", "v flipped to top-left origin"}, {"texture_mode", "copy_required_textures"}, {"textures_dir", "textures"}};

    json parts = json::array();
    size_t allResolved = 0, expected = 0;
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const auto& p = scene.parts[i];
        json jp;
        jp["id"] = p.id;
        jp["label"] = p.label;
        jp["group"] = p.group;
        jp["category"] = p.category;
        jp["source_item_id"] = p.sourceItemId;
        jp["property"] = p.property;
        jp["visible_in_preview"] = p.visibleInPreview;
        jp["include_in_export"] = p.includeInExport;
        jp["asset_status"] = p.status;
        if (!p.detail.empty()) jp["detail"] = p.detail;
        if (!p.bundle.empty()) jp["bundle"] = p.bundle;
        if (!p.modGuid.empty())
            jp["mod"] = {{"guid", p.modGuid}, {"name", p.modName}, {"version", p.modVersion}, {"author", p.modAuthor},
                         {"file", fileio::FileName(p.modPath)}};
        size_t verts = 0, tris = 0;
        for (int mi : p.meshes) {
            verts += scene.meshes[static_cast<size_t>(mi)].vertices.size();
            for (const auto& s : scene.meshes[static_cast<size_t>(mi)].submeshes) tris += s.indices.size() / 3;
        }
        jp["meshes"] = p.meshes.size();
        jp["vertices"] = verts;
        jp["triangles"] = tris;
        if (i < range.size() && range[i].second > 0)
            jp["pmx_materials"] = {{"first", range[i].first}, {"count", range[i].second}};
        parts.push_back(std::move(jp));
        if (p.id != "body") {
            ++expected;
            if (p.status == "resolved-base" || p.status == "resolved-mod") ++allResolved;
        }
    }
    j["parts"] = std::move(parts);

    json tex = json::array();
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto& t = scene.textures[i];
        tex.push_back({{"file", i < textureFiles.size() ? textureFiles[i] : std::string()}, {"name", t.name},
                       {"width", t.width}, {"height", t.height}, {"source_bundle", t.source},
                       {"pixel_fnv1a64", std::to_string(t.pixelHash)}});
    }
    j["textures"] = std::move(tex);
    j["counts"] = {{"bones", scene.bones.size()}, {"materials", scene.materials.size()}, {"textures", scene.textures.size()}};

    json issues = json::array();
    bool anyError = false;
    for (const auto& is : scene.issues) {
        issues.push_back({{"severity", is.severity}, {"part", is.part}, {"message", is.message}});
        if (is.severity == "error") anyError = true;
    }
    j["issues"] = std::move(issues);
    j["validation"] = {{"card_parse", "passed"},
                       {"assets_resolved", allResolved == expected ? "passed" : "partial"},
                       {"scene_assembled", anyError ? "failed" : "passed"},
                       {"pmx_validated", "not_run"},
                       {"target_viewer_test", "not_run"}};
    return j;
}

ExportResult ExportScene(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt) {
    ExportResult res;
    std::string stem = opt.baseName.empty() ? safeFileName(scene.name) : safeFileName(opt.baseName);

    if (fileio::Exists(opt.outDir) && !opt.overwrite) {
        std::vector<std::string> existing = fileio::ListFiles(opt.outDir, {".pmx", ".png", ".json"});
        if (!existing.empty())
            throw FormatError("output folder already has files: " + opt.outDir + " (use a new folder or --overwrite)");
    }
    fileio::CreateDirectories(fileio::Join(opt.outDir, "textures"));
    // A re-export replaces the previous one: stale textures from an earlier run must not linger beside it.
    if (opt.overwrite)
        for (const std::string& f : fileio::ListFiles(fileio::Join(opt.outDir, "textures"), {".png"})) fileio::RemoveFile(f);

    // --- textures: only those a written material uses ---------------------
    std::vector<int> texMap(scene.textures.size(), -1);   // scene texture -> pmx texture
    pmx::Model model;
    model.name = scene.name;
    model.comment = "Converted by CardBridge from " + fileio::FileName(card.path);

    for (size_t i = 0; i < scene.bones.size(); ++i) {
        const auto& b = scene.bones[i];
        pmx::Bone pb;
        pb.name = b.name;
        pb.position = b.pos * opt.scale;
        pb.parent = b.parent;
        model.bones.push_back(pb);
    }

    std::vector<std::pair<int, int>> range(scene.parts.size(), {0, 0});
    std::vector<std::string> textureFiles(scene.textures.size());
    int meshOrdinal = 0;
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& part = scene.parts[pi];
        if (!part.includeInExport) continue;
        range[pi].first = static_cast<int>(model.materials.size());
        for (int mi : part.meshes) {
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            uint32_t base = static_cast<uint32_t>(model.vertices.size());
            for (const auto& v : mesh.vertices) {
                pmx::Vertex pv;
                pv.position = v.pos * opt.scale;
                pv.normal = v.normal;
                pv.uv = {v.uv.x, 1.0f - v.uv.y};
                for (int k = 0; k < 4; ++k) { pv.bone[k] = v.bone[k]; pv.weight[k] = v.weight[k]; }
                model.vertices.push_back(pv);
            }
            int sub = 0;
            for (const auto& sm : mesh.submeshes) {
                const auto& mat = scene.materials[static_cast<size_t>(sm.material)];
                pmx::Material pm;
                pm.name = part.id + "/" + mesh.name + "#" + std::to_string(sub++);
                pm.nameEn = pm.name;
                pm.memo = "part=" + part.id + "; material=" + mat.name + "; shader=" + mat.shader;
                pm.diffuse = mat.diffuse;
                pm.ambient = {mat.diffuse.x * 0.5f, mat.diffuse.y * 0.5f, mat.diffuse.z * 0.5f};
                pm.drawFlags = mat.doubleSided ? 0x01 : 0x00;
                if (mat.texture >= 0) {
                    size_t ti = static_cast<size_t>(mat.texture);
                    if (texMap[ti] < 0) {
                        const auto& t = scene.textures[ti];
                        std::string file = "textures/" + safeFileName(t.name) + "_" + std::to_string(ti) + ".png";
                        Bytes png = encodePng(t);
                        fileio::WriteFile(fileio::Join(opt.outDir, file), png.data(), png.size());
                        textureFiles[ti] = file;
                        texMap[ti] = static_cast<int>(model.textures.size());
                        model.textures.push_back(file);
                    }
                    pm.texture = texMap[ti];
                }
                pm.indexCount = static_cast<uint32_t>(sm.indices.size());
                for (uint32_t idx : sm.indices) model.indices.push_back(base + idx);
                model.materials.push_back(std::move(pm));
            }
            ++meshOrdinal;
        }
        range[pi].second = static_cast<int>(model.materials.size()) - range[pi].first;
    }

    Bytes pmxBytes = pmx::WritePmx(model);
    std::string pmxFile = stem + ".pmx";
    res.pmxPath = fileio::Join(opt.outDir, pmxFile);
    fileio::WriteFile(res.pmxPath, pmxBytes.data(), pmxBytes.size());

    // A picture of what was written (front, side, back): a conversion can be judged without a PMX viewer.
    res.previewPath = fileio::Join(opt.outDir, stem + "_preview.png");
    Bytes prev = RenderPreviewPng(scene);
    fileio::WriteFile(res.previewPath, prev.data(), prev.size());
    {   // a head close-up, to compare face and hair against the card's own picture
        PreviewOptions head;
        head.views = 1;
        head.viewWidth = 640;
        head.viewHeight = 640;
        head.yTop = 1.0f;
        head.yBottom = 0.84f;
        Bytes hp = RenderPreviewPng(scene, head);
        fileio::WriteFile(fileio::Join(opt.outDir, stem + "_head.png"), hp.data(), hp.size());
    }
    {   // the feet from the front, side and back
        PreviewOptions feet;
        feet.viewWidth = 560;
        feet.viewHeight = 560;
        feet.yTop = 0.16f;
        feet.yBottom = 0.0f;
        Bytes fp = RenderPreviewPng(scene, feet);
        fileio::WriteFile(fileio::Join(opt.outDir, stem + "_feet.png"), fp.data(), fp.size());
    }

    nlohmann::json manifest = BuildManifest(scene, card, opt, pmxFile, textureFiles, range);
    res.manifestPath = fileio::Join(opt.outDir, stem + ".cardbridge.json");
    fileio::WriteTextFile(res.manifestPath, manifest.dump(2) + "\n");

    res.folder = opt.outDir;
    res.vertices = model.vertices.size();
    res.triangles = model.indices.size() / 3;
    res.materials = model.materials.size();
    res.bones = model.bones.size();
    res.textures = model.textures.size();
    res.parts = scene.parts.size();
    (void)meshOrdinal;
    return res;
}

} // namespace cardbridge::exporter
