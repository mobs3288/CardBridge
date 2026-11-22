#include "pmx/PmxToScene.h"

#include "core/FileIo.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace cardbridge::pmx {
namespace {

std::string slashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint64_t fnv1a(const uint8_t* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

} // namespace

PmxLoadResult BuildScene(const Model& doc, const std::string& pmxDir) {
    PmxLoadResult result;
    assemble::Scene& s = result.scene;
    s.name = doc.nameEn.empty() ? doc.name : doc.nameEn;
    auto warn = [&](const std::string& text) { result.warnings.push_back(text); };

    // Textures are read from the PMX's own folder; a missing file becomes an
    // empty placeholder so the model still opens with a clear warning.
    s.textures.reserve(doc.textures.size());
    for (const std::string& raw : doc.textures) {
        const std::string rel = slashes(raw);
        const std::string full = (!rel.empty() && rel[0] == '/') ? rel : join(pmxDir, rel);
        assemble::SceneTexture tex;
        tex.name = fileio::FileName(rel);
        tex.source = "pmx:" + full;
        tex.format = "PNG";
        const bool toon = lower(tex.name).find("toon") != std::string::npos;
        tex.clamp = toon;
        tex.srgb = !toon;
        try {
            const Bytes bytes = fileio::ReadFile(full);
            int w = 0, h = 0, n = 0;
            stbi_set_flip_vertically_on_load(0);   // SceneTexture::rgba is top row first
            stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
            if (px) {
                tex.width = w; tex.height = h;
                tex.rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
                tex.pixelHash = fnv1a(tex.rgba.data(), tex.rgba.size());
                stbi_image_free(px);
            } else {
                warn("texture could not be decoded: " + rel);
            }
        } catch (const std::exception& e) {
            warn(std::string("texture missing: ") + rel + " (" + e.what() + ")");
        }
        s.textures.push_back(std::move(tex));
    }

    // Materials. "MMD" keeps every Koikatsu-specific shader heuristic out of
    // the way; the plain (as-exported) view uses the PMX values directly.
    s.materials.reserve(doc.materials.size());
    for (const Material& mat : doc.materials) {
        assemble::SceneMaterial sm;
        sm.name = mat.name + (mat.nameEn.empty() || mat.nameEn == mat.name ? "" : " / " + mat.nameEn);
        sm.shader = "MMD";
        sm.diffuse = mat.diffuse;
        sm.shadow = {1, 1, 1};                 // ambient = diffuse in the MMD look
        sm.specular = mat.specular;
        sm.specularPower = mat.specularPower;
        sm.outlineColor = {mat.edgeColor.x, mat.edgeColor.y, mat.edgeColor.z};
        sm.outlineWidth = mat.edgeSize;
        sm.doubleSided = (mat.drawFlags & 0x01) != 0;
        sm.alpha = mat.diffuse.w < 0.999f;
        sm.translucent = mat.diffuse.w < 0.999f;
        sm.texture = mat.texture;
        if (mat.drawFlags & 0x10) sm.outlineWidth = std::max(sm.outlineWidth, 0.0001f);
        s.materials.push_back(std::move(sm));
    }
    if (!doc.materials.empty())
        warn("PMX ambient, sphere/toon references and material memos are preserved in the document, not editable in this scene");

    // Bones: rest positions and the hierarchy are all the posing/gizmo code needs.
    s.bones.reserve(doc.bones.size());
    for (const Bone& b : doc.bones) {
        assemble::SceneBone sb;
        sb.name = b.nameEn.empty() ? b.name : b.nameEn;
        sb.parent = b.parent;
        sb.pos = b.position;
        sb.source = "pmx";
        sb.world = Mat4::TRS(b.position, Quat{}, {1, 1, 1});
        s.bones.push_back(std::move(sb));
    }

    // Vertices. Deform types beyond BDEF4 are rendered/skinned as their
    // closest supported equivalent; the document keeps the original weights.
    assemble::SceneMesh mesh;
    mesh.name = doc.name.empty() ? "model" : doc.name;
    mesh.part = 0;
    mesh.vertices.reserve(doc.vertices.size());
    for (const Vertex& v : doc.vertices) {
        assemble::SceneVertex sv;
        sv.pos = v.position;
        sv.normal = v.normal;
        sv.uv = v.uv;
        sv.uv1 = {v.additionalUv[0].x, v.additionalUv[0].y};
        sv.uv2 = {v.additionalUv[1].x, v.additionalUv[1].y};
        sv.uv3 = {v.additionalUv[2].x, v.additionalUv[2].y};
        for (int k = 0; k < 4; ++k) { sv.bone[k] = v.bone[k]; sv.weight[k] = v.weight[k]; }
        if (v.deformType == 1 || v.deformType == 3) sv.weight[1] = 1.0f - v.weight[0];
        if (v.deformType == 3) warn("SDEF vertices are skinned as BDEF2 in the preview; the document keeps SDEF");
        if (v.deformType == 4) warn("QDEF vertices are skinned as BDEF4 in the preview; the document keeps QDEF");
        mesh.vertices.push_back(std::move(sv));
    }
    size_t consumed = 0;
    for (size_t mi = 0; mi < doc.materials.size(); ++mi) {
        const size_t count = doc.materials[mi].indexCount;
        if (!count || consumed + count > doc.indices.size()) continue;
        assemble::SceneSubmesh sub;
        sub.material = static_cast<int>(mi);
        sub.indices.assign(doc.indices.begin() + static_cast<std::ptrdiff_t>(consumed),
                           doc.indices.begin() + static_cast<std::ptrdiff_t>(consumed + count));
        mesh.submeshes.push_back(std::move(sub));
        consumed += count;
    }
    if (consumed != doc.indices.size())
        warn("material face counts did not cover the index buffer; unassigned faces are not previewed");

    // Vertex morphs become scene blend shapes; group morphs become expressions.
    size_t vertexMorphs = 0, groupMorphs = 0, unsupportedMorphs = 0;
    std::vector<int> morphToShape(doc.morphs.size(), -1);
    for (size_t i = 0; i < doc.morphs.size(); ++i) {
        const Morph& mo = doc.morphs[i];
        if (mo.type == 1 && !mo.vertices.empty()) {
            assemble::SceneBlendShape shape;
            shape.name = mo.name;
            for (const auto& off : mo.vertices)
                if (off.first >= 0 && static_cast<size_t>(off.first) < mesh.vertices.size())
                    shape.offsets.emplace_back(static_cast<uint32_t>(off.first), off.second);
            mesh.shapes.push_back(std::move(shape));
            morphToShape[i] = static_cast<int>(mesh.shapes.size()) - 1;
            ++vertexMorphs;
        } else if (mo.type == 0 && !mo.group.empty()) {
            assemble::SceneExpression expr;
            expr.name = mo.name;
            expr.panel = mo.panel;
            for (const auto& gr : mo.group)
                if (gr.first >= 0 && static_cast<size_t>(gr.first) < morphToShape.size() && morphToShape[static_cast<size_t>(gr.first)] >= 0)
                    expr.targets.push_back({0, morphToShape[static_cast<size_t>(gr.first)], gr.second});
            s.expressions.push_back(std::move(expr));
            ++groupMorphs;
        } else {
            ++unsupportedMorphs;
        }
    }
    s.meshes.push_back(std::move(mesh));

    assemble::ScenePart part;
    part.id = "model";
    part.label = s.name;
    part.group = "Body";
    part.status = "native-pmx";
    part.meshes = {0};
    s.parts.push_back(std::move(part));

    if (!doc.rigidBodies.empty() || !doc.joints.empty())
        warn(std::to_string(doc.rigidBodies.size()) + " rigid bodies and " + std::to_string(doc.joints.size()) +
             " joints are preserved in the document; the physics editor is a later phase");
    if (!doc.frames.empty())
        warn(std::to_string(doc.frames.size()) + " display frames are preserved in the document, not shown in the Model Explorer yet");
    if (doc.format.additionalUv() > 3)
        warn("PMX has 4 additional UV channels; the scene holds 3 and the fourth stays in the document");
    if (groupMorphs) warn(std::to_string(groupMorphs) + " group morphs are previewed as expressions; the document keeps them exactly");
    if (unsupportedMorphs)
        warn(std::to_string(unsupportedMorphs) + " material/bone/UV/flip/impulse morphs are preserved but not editable in this phase");
    if (vertexMorphs) result.warnings.push_back(std::to_string(vertexMorphs) + " vertex morphs loaded");
    return result;
}

void ApplySceneMaterials(const assemble::Scene& scene, Model& doc) {
    const size_t n = std::min(scene.materials.size(), doc.materials.size());
    for (size_t i = 0; i < n; ++i) {
        const assemble::SceneMaterial& sm = scene.materials[i];
        Material& dm = doc.materials[i];
        dm.diffuse = sm.diffuse;
        dm.specular = sm.specular;
        dm.specularPower = sm.specularPower;
        dm.edgeColor = {sm.outlineColor.x, sm.outlineColor.y, sm.outlineColor.z, dm.edgeColor.w};
        dm.edgeSize = sm.outlineWidth;
        uint8_t flags = 0;
        if (sm.doubleSided) flags |= 0x01;
        if ((dm.drawFlags & 0x10) || sm.outlineWidth > 0.0f) flags |= 0x10;
        dm.drawFlags = flags;
        if (sm.texture >= 0 && static_cast<size_t>(sm.texture) < doc.textures.size()) dm.texture = sm.texture;
    }
}

std::string ExportDocument(const Model& doc, const std::string& pmxDir,
                           const std::string& outDir, const std::string& baseName) {
    Model out = doc;
    fileio::CreateDirectories(outDir);
    const std::string texDir = join(outDir, "textures");
    fileio::CreateDirectories(texDir);
    std::vector<std::string> used;
    for (std::string& rel : out.textures) {
        const std::string src = slashes(rel);
        const std::string full = (!src.empty() && src[0] == '/') ? src : join(pmxDir, src);
        std::string name = fileio::FileName(src);
        if (name.empty()) name = "texture.png";
        std::string unique = name;
        for (int n = 2; std::find(used.begin(), used.end(), unique) != used.end(); ++n) {
            const size_t dot = name.rfind('.');
            unique = (dot == std::string::npos ? name : name.substr(0, dot)) + "_" + std::to_string(n) +
                     (dot == std::string::npos ? "" : name.substr(dot));
        }
        used.push_back(unique);
        try {
            const Bytes bytes = fileio::ReadFile(full);
            fileio::WriteFile(join(texDir, unique), bytes.data(), bytes.size());
        } catch (const std::exception&) {
            // Keep the reference: a missing source texture stays a missing file
            // with the same relative path instead of silently pointing elsewhere.
        }
        rel = "textures/" + unique;
    }
    const std::string pmxPath = join(outDir, baseName + ".pmx");
    const Bytes bytes = WritePmx(out);
    fileio::WriteFile(pmxPath, bytes.data(), bytes.size());
    return pmxPath;
}

} // namespace cardbridge::pmx
