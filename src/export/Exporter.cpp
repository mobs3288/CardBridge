#include "export/Exporter.h"

#include "core/FileIo.h"
#include "export/BoneLabels.h"
#include "export/PmxIk.h"
#include "export/PmxPhysics.h"
#include "export/Preview.h"
#include "pmx/PmxWriter.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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

std::vector<std::string> ValidateScene(const assemble::Scene& scene) {
    std::vector<std::string> out;
    auto add = [&](const std::string& s) { if (out.size() < 200) out.push_back(s); };
    const int nb = static_cast<int>(scene.bones.size()), nt = static_cast<int>(scene.textures.size()), nm = static_cast<int>(scene.materials.size());
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto& t = scene.textures[i];
        if (t.width <= 0 || t.height <= 0 || t.rgba.size() != static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4)
            add("texture " + std::to_string(i) + " '" + t.name + "' has pixel data that does not match its size");
    }
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        if (m.texture >= nt) add("material '" + m.name + "' names texture " + std::to_string(m.texture) + " of " + std::to_string(nt));
        for (const auto& tb : m.texProps)
            if (tb.texture >= nt) add("material '" + m.name + "' binds " + tb.name + " to texture " + std::to_string(tb.texture) + " of " + std::to_string(nt));
    }
    // The body's height (head part's top) is the yardstick for "far outside".
    float headTop = 0;
    for (const auto& p : scene.parts)
        if (p.id == "head")
            for (int mi : p.meshes)
                for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) headTop = std::max(headTop, v.pos.y);
    for (const auto& part : scene.parts) {
        if (!part.includeInExport) continue;
        float lo = 1e30f, hi = -1e30f;
        for (int mi : part.meshes) {
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            for (size_t v = 0; v < mesh.vertices.size(); ++v) {
                const auto& sv = mesh.vertices[v];
                if (!std::isfinite(sv.pos.x) || !std::isfinite(sv.pos.y) || !std::isfinite(sv.pos.z)) { add("part " + part.id + " mesh '" + mesh.name + "' vertex " + std::to_string(v) + " is not finite"); break; }
                lo = std::min(lo, sv.pos.y);
                hi = std::max(hi, sv.pos.y);
                float wsum = 0;
                for (int k = 0; k < 4; ++k) {
                    wsum += sv.weight[k];
                    if (sv.weight[k] > 0 && (sv.bone[k] < 0 || sv.bone[k] >= nb)) { add("part " + part.id + " mesh '" + mesh.name + "' vertex " + std::to_string(v) + " uses bone " + std::to_string(sv.bone[k]) + " of " + std::to_string(nb)); v = mesh.vertices.size(); break; }
                }
                if (v < mesh.vertices.size() && std::fabs(wsum - 1.0f) > 1e-3f) { add("part " + part.id + " mesh '" + mesh.name + "' vertex " + std::to_string(v) + " weights sum to " + std::to_string(wsum)); break; }
            }
            for (const auto& sub : mesh.submeshes) {
                if (sub.material < 0 || sub.material >= nm) add("part " + part.id + " mesh '" + mesh.name + "' has a submesh with material " + std::to_string(sub.material) + " of " + std::to_string(nm));
                if (sub.indices.size() % 3 != 0) add("part " + part.id + " mesh '" + mesh.name + "' has an index count that is not a multiple of 3");
                for (uint32_t ix : sub.indices)
                    if (ix >= mesh.vertices.size()) { add("part " + part.id + " mesh '" + mesh.name + "' has a triangle index outside its vertices"); break; }
            }
        }
        if (headTop > 0 && lo <= hi && part.id != "body" && !part.underPiece) {
            if (hi > headTop * 1.25f) add("part " + part.id + " reaches " + std::to_string(hi) + " above the floor, the head ends at " + std::to_string(headTop) + " (transform applied twice?)");
            if (lo < -0.15f * headTop) add("part " + part.id + " reaches " + std::to_string(lo) + " below the floor");
        }
    }
    return out;
}

nlohmann::json BuildManifest(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt,
                             const std::string& pmxFile, const std::vector<std::string>& textureFiles,
                             const std::vector<std::pair<int, int>>& range, const std::vector<std::string>& morphNames) {
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
        if (p.underPiece) { jp["under_piece_of"] = p.underOf; jp["covered_by"] = p.coveredBy; }
        if (p.startHidden) jp["start_hidden"] = true;
        if (p.nudeSwitch) jp["nude_switch"] = true;
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
    j["morphs"] = morphNames;

    json tex = json::array();
    for (size_t i = 0; i < scene.textures.size(); ++i) {
        const auto& t = scene.textures[i];
        tex.push_back({{"file", i < textureFiles.size() ? textureFiles[i] : std::string()}, {"name", t.name},
                       {"width", t.width}, {"height", t.height}, {"source_bundle", t.source},
                       {"pixel_fnv1a64", std::to_string(t.pixelHash)}});
    }
    j["textures"] = std::move(tex);
    j["counts"] = {{"bones", scene.bones.size()}, {"materials", scene.materials.size()}, {"textures", scene.textures.size()},
                   {"expressions", scene.expressions.size()}, {"physics_chains", scene.chains.size()}, {"colliders", scene.colliders.size()}};
    if (!scene.pose.empty() || scene.aposeApplied) {   // the pose is already in the vertices and bone positions; this says what it was
        j["pose"] = {{"a_pose", scene.aposeApplied}, {"bones", nlohmann::json::array()}};
        for (const auto& e : scene.pose)
            j["pose"]["bones"].push_back({{"bone", e.bone}, {"position", {e.pos.x, e.pos.y, e.pos.z}}, {"rotation_euler_degrees", {e.rot.x, e.rot.y, e.rot.z}}, {"scale", {e.scl.x, e.scl.y, e.scl.z}}});
    }

    json issues = json::array();
    bool anyError = false;
    for (const auto& is : scene.issues) {
        issues.push_back({{"severity", is.severity}, {"part", is.part}, {"message", is.message}});
        if (is.severity == "error") anyError = true;
    }
    j["issues"] = std::move(issues);
    std::vector<std::string> findings = ValidateScene(scene);
    j["scene_findings"] = findings;
    j["validation"] = {{"card_parse", "passed"},
                       {"scene_structure", findings.empty() ? "passed" : "findings"},
                       {"assets_resolved", allResolved == expected ? "passed" : "partial"},
                       {"scene_assembled", anyError ? "failed" : "passed"},
                       {"pmx_validated", "not_run"},
                       {"target_viewer_test", "not_run"}};
    return j;
}

ExportResult ExportScene(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt) {
    ExportResult res;
    auto log = [&](const std::string& s) { if (opt.log) opt.log(s); };
    std::string stem = opt.baseName.empty() ? safeFileName(scene.name) : safeFileName(opt.baseName);

    if (fileio::Exists(opt.outDir) && !opt.overwrite) {
        std::vector<std::string> existing = fileio::ListFiles(opt.outDir, {".pmx", ".png", ".json"});
        if (!existing.empty())
            throw FormatError("output folder already has files: " + opt.outDir + " (use a new folder or --overwrite)");
    }
    fileio::CreateDirectories(fileio::Join(opt.outDir, "textures"));
    log("writing textures and materials");
    // A re-export replaces the previous one: stale textures from an earlier run must not linger beside it.
    if (opt.overwrite)
        for (const std::string& f : fileio::ListFiles(fileio::Join(opt.outDir, "textures"), {".png"})) fileio::RemoveFile(f);

    // --- textures: only those a written material uses ---------------------
    std::vector<int> texMap(scene.textures.size(), -1);   // scene texture -> pmx texture
    pmx::Model model;
    model.name = scene.name;
    model.comment = "Converted by CardBridge from " + fileio::FileName(card.path);

    {   // every scene bone, in scene order (a parent always comes before its children); the tail points at the first child
        std::vector<int> firstChild(scene.bones.size(), -1);
        for (size_t i = 0; i < scene.bones.size(); ++i) {
            const int p = scene.bones[i].parent;
            if (p >= 0 && static_cast<size_t>(p) < scene.bones.size() && firstChild[static_cast<size_t>(p)] < 0) firstChild[static_cast<size_t>(p)] = static_cast<int>(i);
        }
        for (size_t i = 0; i < scene.bones.size(); ++i) {
            const auto& b = scene.bones[i];
            pmx::Bone pb;
            pb.name = b.name;
            pb.nameEn = BoneLabelEn(b.name);
            pb.position = b.pos * opt.scale;
            pb.parent = b.parent;
            pb.tailBone = firstChild[i];
            if (IsHelperBone(b.name)) pb.flags = 0x0006;   // rotatable, translatable; not shown and not operable: a driven, shape or attach node
            model.bones.push_back(pb);
        }
    }
    std::vector<int> meshBase(scene.meshes.size(), -1);   // PMX index of each exported mesh's first vertex

    std::vector<std::pair<int, int>> range(scene.parts.size(), {0, 0});
    std::vector<float> baseAlpha;   // per PMX material: its alpha before a startHidden part made it transparent
    std::vector<std::string> textureFiles(scene.textures.size());
    int meshOrdinal = 0;
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& part = scene.parts[pi];
        if (!part.includeInExport) continue;
        range[pi].first = static_cast<int>(model.materials.size());
        for (int mi : part.meshes) {
            const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
            uint32_t base = static_cast<uint32_t>(model.vertices.size());
            meshBase[static_cast<size_t>(mi)] = static_cast<int>(base);
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
                if (mat.shader.find("glasses") != std::string::npos) {   // lenses: the shader's _Color alpha (see-through), not a solid
                    float a = 0.35f;
                    for (const auto& pr : mat.props) if (pr.first == "_Color") a = pr.second.w < 0.99f ? pr.second.w : 0.35f;
                    pm.diffuse.w = a;
                }
                // MMD shades as diffuse * light + ambient: the game's shadow colour goes to ambient so the dark side keeps
                // the tint the card's shader gives it.
                pm.ambient = {mat.diffuse.x * mat.shadow.x, mat.diffuse.y * mat.shadow.y, mat.diffuse.z * mat.shadow.z};
                pm.specular = mat.specular;
                pm.specularPower = mat.specularPower;
                if (mat.bakedLit) {   // the texture holds the game's lighting already: show it as it is, no second shading
                    pm.diffuse = {0.0f, 0.0f, 0.0f, pm.diffuse.w};
                    pm.ambient = {1.0f, 1.0f, 1.0f};
                    pm.specular = {0.0f, 0.0f, 0.0f};
                }
                pm.drawFlags = (mat.doubleSided ? 0x01 : 0x00) | (mat.outlineWidth > 0.0f ? 0x10 : 0x00);
                pm.edgeColor = {mat.outlineColor.x, mat.outlineColor.y, mat.outlineColor.z, 1.0f};
                pm.edgeSize = std::min(2.0f, mat.outlineWidth * 20.0f);
                if (mat.texture >= 0) {
                    size_t ti = static_cast<size_t>(mat.texture);
                    if (texMap[ti] < 0) {
                        const auto& t = scene.textures[ti];
                        std::string file = "textures/" + safeFileName(t.name) + "_" + std::to_string(ti) + ".png";
                        Bytes png = encodePng(t);
                        fileio::WriteFile(fileio::Join(opt.outDir, file), png.data(), png.size());
                        log("texture " + t.name + " (" + std::to_string(t.width) + "x" + std::to_string(t.height) + ")");
                        textureFiles[ti] = file;
                        texMap[ti] = static_cast<int>(model.textures.size());
                        model.textures.push_back(file);
                    }
                    pm.texture = texMap[ti];
                }
                baseAlpha.push_back(pm.diffuse.w);
                if (part.startHidden) { pm.diffuse.w = 0.0f; pm.edgeColor.w = 0.0f; }   // the morphs bring it back
                pm.indexCount = static_cast<uint32_t>(sm.indices.size());
                for (uint32_t idx : sm.indices) model.indices.push_back(base + idx);
                model.materials.push_back(std::move(pm));
            }
            ++meshOrdinal;
        }
        range[pi].second = static_cast<int>(model.materials.size()) - range[pi].first;
    }

    // Morphs: switch outfit parts off and the nude body on, so one PMX serves every state (the game's own clothes-off needs no second model).
    //   "Hide <part>" / "Show <part>"  one part (transparent / opaque again, by how it starts)
    //   "Under <piece>"                 the skin a set of garments covers
    //   "Take off <part>"               hide a garment and show the skin only it covers
    //   "Nude body"                     every covered skin piece (clothes stay as they are)
    //   "All clothes off"               every outfit part hidden and the whole body shown
    std::vector<std::string> morphNames;
    {
        auto offsets = [&](size_t pi, bool hide, std::vector<pmx::MaterialOffset>& out) {
            for (int k = 0; k < range[pi].second; ++k) {
                pmx::MaterialOffset off;
                off.material = range[pi].first + k;
                if (hide) {   // multiply: alpha 0, everything else unchanged
                    off.add = false;
                    off.diffuse = {1, 1, 1, 0}; off.specular = {1, 1, 1}; off.specularPower = 1; off.ambient = {1, 1, 1};
                    off.edgeColor = {1, 1, 1, 0}; off.edgeSize = 1;
                    off.texture = {1, 1, 1, 1}; off.sphere = {1, 1, 1, 1}; off.toon = {1, 1, 1, 1};
                } else {      // add: the alpha it had before it was made transparent
                    off.add = true;
                    off.diffuse = {0, 0, 0, baseAlpha[static_cast<size_t>(off.material)]};
                    off.edgeColor = {0, 0, 0, 1};
                }
                out.push_back(off);
            }
        };
        std::map<size_t, int> showMorph, hideMorph;   // part -> morph index
        auto addMorph = [&](pmx::Morph m) { model.morphs.push_back(std::move(m)); return static_cast<int>(model.morphs.size()) - 1; };
        std::vector<size_t> outfit, others, under;   // outfit: the garments; others: every part but the body and head (they get Hide / Show)
        for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
            const auto& p = scene.parts[pi];
            if (!p.includeInExport || range[pi].second == 0) continue;
            if (p.underPiece) under.push_back(pi);
            else if (p.id != "body" && p.id != "head") { others.push_back(pi); if (p.group == "Outfit") outfit.push_back(pi); }
        }
        for (size_t pi : under) {
            if (!scene.parts[pi].startHidden) continue;
            pmx::Morph m;
            m.name = "Under " + scene.parts[pi].id;
            offsets(pi, false, m.materials);
            showMorph[pi] = addMorph(std::move(m));
        }
        for (size_t pi : others) {
            pmx::Morph m;
            m.name = std::string(scene.parts[pi].startHidden ? "Show " : "Hide ") + scene.parts[pi].id;
            offsets(pi, !scene.parts[pi].startHidden, m.materials);
            (scene.parts[pi].startHidden ? showMorph : hideMorph)[pi] = addMorph(std::move(m));
        }
        if (!under.empty()) {
            pmx::Morph nude;
            nude.name = "Nude body";
            for (auto& [pi, mi] : showMorph) if (scene.parts[pi].underPiece) nude.group.emplace_back(mi, 1.0f);
            if (!nude.group.empty()) addMorph(std::move(nude));
        }
        const int nudeIndex = [&] { for (size_t i = 0; i < model.morphs.size(); ++i) if (model.morphs[i].name == "Nude body") return static_cast<int>(i); return -1; }();
        for (size_t pi : outfit) {
            auto h = hideMorph.find(pi);
            if (h == hideMorph.end()) continue;
            pmx::Morph m;
            m.name = "Take off " + scene.parts[pi].id;
            m.group.emplace_back(h->second, 1.0f);
            for (auto& [ui, mi] : showMorph)
                if (scene.parts[ui].underPiece && scene.parts[ui].coveredBy.size() == 1 && scene.parts[ui].coveredBy[0] == scene.parts[pi].id) m.group.emplace_back(mi, 1.0f);
            if (m.group.size() > 1) addMorph(std::move(m));   // without skin under it, "Hide" says the same
        }
        if (!outfit.empty()) {
            pmx::Morph all;
            all.name = "All clothes off";
            for (size_t pi : outfit) { auto h = hideMorph.find(pi); if (h != hideMorph.end()) all.group.emplace_back(h->second, 1.0f); }
            if (nudeIndex >= 0) all.group.emplace_back(nudeIndex, 1.0f);
            if (!all.group.empty()) addMorph(std::move(all));
        }
    }
    const size_t expressionMorphs = AddExpressionMorphs(scene, model, meshBase, opt.scale);
    for (const auto& m : model.morphs) morphNames.push_back(m.name);
    AddPhysics(scene, model, opt.scale);
    const std::vector<int> ikBones = AddIk(scene, model, opt.scale);
    {   // display frames: Root, Exp (every morph), then the bones by group
        pmx::DisplayFrame root;
        root.name = "Root"; root.special = true;
        pmx::DisplayFrame exp;
        exp.name = "\xE8\xA1\xA8\xE6\x83\x85"; exp.nameEn = "Exp"; exp.special = true;   // 表情
        for (size_t i = 0; i < model.morphs.size(); ++i) exp.morphs.push_back(static_cast<int>(i));
        std::vector<pmx::DisplayFrame> frames;
        for (auto& [group, list] : BoneFrames(scene)) {
            if (group == "Root") { root.bones = list; continue; }
            pmx::DisplayFrame f;
            f.name = group;
            f.bones = std::move(list);
            frames.push_back(std::move(f));
        }
        model.frames.push_back(std::move(root));
        model.frames.push_back(std::move(exp));
        for (auto& f : frames) model.frames.push_back(std::move(f));
        if (!ikBones.empty()) {
            pmx::DisplayFrame f;
            f.name = "IK";
            f.bones = ikBones;
            model.frames.push_back(std::move(f));
        }
    }
    log("expression morphs: " + std::to_string(expressionMorphs) + ", rigid bodies: " + std::to_string(model.rigidBodies.size()) + ", joints: " + std::to_string(model.joints.size()));

    log("writing PMX (" + std::to_string(model.vertices.size()) + " vertices, " + std::to_string(model.indices.size() / 3) + " triangles)");
    Bytes pmxBytes = pmx::WritePmx(model);
    std::string pmxFile = stem + ".pmx";
    res.pmxPath = fileio::Join(opt.outDir, pmxFile);
    fileio::WriteFile(res.pmxPath, pmxBytes.data(), pmxBytes.size());

    // A picture of what was written (front, side, back): a conversion can be judged without a PMX viewer.
    log("rendering preview images");
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
    {   // a three-quarter view through the same camera the viewer uses (keeps the viewer's renderer under test)
        ViewCamera cam;
        cam.yawDeg = 30;
        cam.pitchDeg = 8;
        ViewOptions vo;
        auto vs = NewViewState();
        std::vector<uint32_t> px;
        RenderView(scene, cam, 640, 800, vo, *vs, px);
        std::vector<uint8_t> rgba(px.size() * 4);
        for (size_t i = 0; i < px.size(); ++i) { rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255; }
        Bytes png;
        stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n); }, &png, 640, 800, 4, rgba.data(), 640 * 4);
        fileio::WriteFile(fileio::Join(opt.outDir, stem + "_view.png"), png.data(), png.size());
    }
    {   // the same view through the game's own shaders (materials whose shader is in the bundle), for comparing with the card picture
        ViewCamera cam;
        cam.yawDeg = 0;
        cam.pitchDeg = 4;
        ViewOptions vo;
        vo.shaded = true;
        auto vs = NewViewState();
        std::vector<uint32_t> px;
        RenderView(scene, cam, 640, 800, vo, *vs, px);
        std::vector<uint8_t> rgba(px.size() * 4);
        for (size_t i = 0; i < px.size(); ++i) { rgba[i * 4] = (px[i] >> 16) & 255; rgba[i * 4 + 1] = (px[i] >> 8) & 255; rgba[i * 4 + 2] = px[i] & 255; rgba[i * 4 + 3] = 255; }
        Bytes png;
        stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n); }, &png, 640, 800, 4, rgba.data(), 640 * 4);
        fileio::WriteFile(fileio::Join(opt.outDir, stem + "_shaded.png"), png.data(), png.size());
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

    nlohmann::json manifest = BuildManifest(scene, card, opt, pmxFile, textureFiles, range, morphNames);
    manifest["counts"]["rigid_bodies"] = model.rigidBodies.size();
    manifest["counts"]["ik_bones"] = ikBones.size();   // added after the scene's bones, so the PMX has "bones" + "ik_bones"
    manifest["counts"]["joints"] = model.joints.size();
    log("writing manifest");
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
