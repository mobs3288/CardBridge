#include "export/Exporter.h"

#include "core/FileIo.h"
#include "core/Version.h"
#include "export/BoneLabels.h"
#include "export/MmdNames.h"
#include "export/PmxIk.h"
#include "export/PmxPhysics.h"
#include "export/Preview.h"
#include "pmx/PmxWriter.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Writes a material's shade ramp as a 32-wide PNG, one row per ramp row, next to the other textures (once per distinct ramp) and
// returns its index in the model's texture list.
int ToonTexture(const std::vector<Vec3>& ramp, pmx::Model& model, std::map<std::string, int>& done, const ExportOptions& opt) {
    constexpr int kW = 32;
    assemble::SceneTexture t;
    t.width = kW;
    t.height = static_cast<int>(ramp.size());
    t.rgba.resize(static_cast<size_t>(kW) * ramp.size() * 4);
    std::string key;
    for (size_t y = 0; y < ramp.size(); ++y) {
        const float c[3] = {ramp[y].x, ramp[y].y, ramp[y].z};
        uint8_t px[4];
        for (int ch = 0; ch < 3; ++ch) px[ch] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, c[ch] * 255.0f + 0.5f)));
        px[3] = 255;
        key.append(reinterpret_cast<const char*>(px), 3);
        for (int x = 0; x < kW; ++x) std::memcpy(&t.rgba[(y * kW + static_cast<size_t>(x)) * 4], px, 4);
    }
    auto it = done.find(key);
    if (it != done.end()) return it->second;
    t.name = "toon" + std::to_string(done.size());
    const std::string file = "textures/" + t.name + ".png";
    Bytes png = encodePng(t);
    fileio::WriteFile(fileio::Join(opt.outDir, file), png.data(), png.size());
    model.textures.push_back(file);
    return done[key] = static_cast<int>(model.textures.size()) - 1;
}

// The material's sphere map written once per kind and strength (MmdSphereImage), as a PMX texture index.
int SphereTexture(pmx::Model& model, std::map<std::string, int>& done, const ExportOptions& opt, const MmdLook& look) {
    const assemble::SceneTexture t = MmdSphereImage(look);
    auto it = done.find(t.name);
    if (it != done.end()) return it->second;
    const std::string file = "textures/" + t.name + ".png";
    Bytes png = encodePng(t);
    fileio::WriteFile(fileio::Join(opt.outDir, file), png.data(), png.size());
    model.textures.push_back(file);
    return done[t.name] = static_cast<int>(model.textures.size()) - 1;
}

constexpr uint8_t kItemCutout = 64;   // main_item discards where 2 x alpha - 0.5 < 0: alpha below 0.25

bool IsTooth(const assemble::SceneMaterial& mat) {
    return mat.name.find("tooth") != std::string::npos || mat.name.find("canine") != std::string::npos;
}

// Mean colour of a material's texture (sRGB, the texels that are not see-through), times its diffuse; white without a texture.
Vec3 MeanColour(const assemble::Scene& scene, const assemble::SceneMaterial& mat) {
    double sum[3] = {0, 0, 0}, n = 0;
    if (mat.texture >= 0 && static_cast<size_t>(mat.texture) < scene.textures.size()) {
        const auto& px = scene.textures[static_cast<size_t>(mat.texture)].rgba;
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            if (px[i + 3] < 128) continue;
            for (int c = 0; c < 3; ++c) sum[c] += px[i + static_cast<size_t>(c)];
            n += 255.0;
        }
    }
    if (n == 0) return {mat.diffuse.x, mat.diffuse.y, mat.diffuse.z};
    return {static_cast<float>(sum[0] / n) * mat.diffuse.x, static_cast<float>(sum[1] / n) * mat.diffuse.y, static_cast<float>(sum[2] / n) * mat.diffuse.z};
}

} // namespace

bool HasCutout(const assemble::Scene& scene, const assemble::SceneMaterial& mat) {
    if (mat.alpha || mat.shader.find("main_item") == std::string::npos || mat.texture < 0 || static_cast<size_t>(mat.texture) >= scene.textures.size()) return false;
    if (scene.materials.empty() || &mat < &scene.materials.front() || &mat > &scene.materials.back()) return false;
    const int mi = static_cast<int>(&mat - &scene.materials.front());
    const auto& t = scene.textures[static_cast<size_t>(mat.texture)];
    if (t.width <= 0 || t.height <= 0 || t.rgba.size() < static_cast<size_t>(t.width) * t.height * 4) return false;
    // The texture's alpha where the material's triangles sample it (their centres): empty UV space does not count.
    for (const auto& mesh : scene.meshes)
        for (const auto& sm : mesh.submeshes) {
            if (sm.material != mi) continue;
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                const auto& a = mesh.vertices[sm.indices[i]].uv, & b = mesh.vertices[sm.indices[i + 1]].uv, & c = mesh.vertices[sm.indices[i + 2]].uv;
                float u = (a.x + b.x + c.x) / 3.0f, v = (a.y + b.y + c.y) / 3.0f;
                u -= std::floor(u); v -= std::floor(v);
                const int x = std::min(t.width - 1, static_cast<int>(u * t.width)), y = std::min(t.height - 1, static_cast<int>((1.0f - v) * t.height));   // rows top first, v up
                if (t.rgba[(static_cast<size_t>(y) * t.width + x) * 4 + 3] < kItemCutout) return true;
            }
        }
    return false;
}

// The material's mesh is modelled as two co-located layers (an outer and an inner surface sharing one vertex grid, the game's
// way of drawing a see-through lining): the triangle winding of a large share of them runs opposite to their vertex normals.
// A single surface (the skirt) has no such layer. `mat` must be one of `scene.materials`.
bool HasInnerLayer(const assemble::Scene& scene, const assemble::SceneMaterial& mat) {
    if (scene.materials.empty() || &mat < &scene.materials.front() || &mat > &scene.materials.back()) return false;
    const int mi = static_cast<int>(&mat - &scene.materials.front());
    size_t reversed = 0, total = 0;
    for (const auto& mesh : scene.meshes)
        for (const auto& sm : mesh.submeshes) {
            if (sm.material != mi) continue;
            for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                const auto& a = mesh.vertices[sm.indices[i]];
                const auto& b = mesh.vertices[sm.indices[i + 1]];
                const auto& c = mesh.vertices[sm.indices[i + 2]];
                // The triangle's geometric normal from its winding, against the vertex normals the game authored.
                const Vec3 e1{b.pos.x - a.pos.x, b.pos.y - a.pos.y, b.pos.z - a.pos.z}, e2{c.pos.x - a.pos.x, c.pos.y - a.pos.y, c.pos.z - a.pos.z};
                const Vec3 g{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                if (g.x * g.x + g.y * g.y + g.z * g.z < 1e-24f) continue;   // degenerate: no direction either way
                const Vec3 n{(a.normal.x + b.normal.x + c.normal.x) / 3.0f, (a.normal.y + b.normal.y + c.normal.y) / 3.0f,
                             (a.normal.z + b.normal.z + c.normal.z) / 3.0f};
                if ((g.x * n.x + g.y * n.y + g.z * n.z) < 0.0f) ++reversed;
                ++total;
            }
        }
    return total > 0 && reversed * 5 > total;   // over a fifth wound against the normals: a co-located inner layer
}

MmdLook MmdMaterialLook(const assemble::Scene& scene, const assemble::SceneMaterial& mat) {
    MmdLook look;
    look.diffuse = mat.diffuse;
    if (mat.shader.find("glasses") != std::string::npos) {   // lenses: the shader's _Color alpha (see-through), not a solid
        float a = 0.35f;
        for (const auto& pr : mat.props) if (pr.first == "_Color") a = pr.second.w < 0.99f ? pr.second.w : 0.35f;
        look.diffuse.w = a;
    }
    // MMD shades as diffuse * light + ambient: the game's shadow colour goes to ambient so the dark side keeps
    // the tint the card's shader gives it.
    look.ambient = {mat.diffuse.x * mat.shadow.x, mat.diffuse.y * mat.shadow.y, mat.diffuse.z * mat.shadow.z};
    look.specularPower = mat.specularPower;
    // No specular: the bake leaves the game's highlight out, and MMD's broad sheen (power 5) whitened the face, the hair and the shoes.
    look.specular = {0.0f, 0.0f, 0.0f};
    if (mat.bakedLit) {   // the texture holds the game's lighting already: show it as it is, no second shading
        look.diffuse = {0.0f, 0.0f, 0.0f, look.diffuse.w};
        look.ambient = {1.0f, 1.0f, 1.0f};
    } else if (!mat.toon.empty()) {
        look.ambient = {1.0f, 1.0f, 1.0f};   // MMD multiplies the lit colour by the ramp; the shade tint lives in the ramp now
    }
    // The face's painted layers (eyes, eye whites, brows, eyelines, nose line: the toon_eye* / toon_nose shaders) and see-through lenses
    // neither cast nor take shadow and get no edge, like the eyes of hand-made PMX models; everything else gets MikuMikuDance's ground
    // shadow, casts and receives self-shadow and has an edge, as those models do (without the flags MMD drew no shadow at all).
    const bool overlay = mat.shader.find("toon_") != std::string::npos, lens = mat.shader.find("glasses") != std::string::npos;
    const bool tooth = IsTooth(mat);
    // Transparent (alpha) materials blend over what is behind them: MikuMikuDance's ground shadow, shadow map and self-shadow on such a
    // material draw it as a solid dark shape (the underpants / swimsuit / lace looked like a shadow), so they get none of those, like
    // lenses.
    const bool transparent = mat.alpha;
    look.drawFlags = mat.doubleSided || tooth ? 0x01 : 0x00;   // teeth: seen from inside a mouth, a single-sided mesh can face away from the camera
    // A mesh cut to a shape by its texture (HasCutout) still takes shadow, but MMD's ground shadow, shadow map and edge use the whole mesh:
    // they would draw the cut-away part (as dark shapes round the armour of a MaterialEditor-built outfit).
    const bool cutout = HasCutout(scene, mat);
    // A mesh modelled as two co-located layers (an outer and an inner surface sharing one vertex grid): MikuMikuDance's edge draws back
    // faces pushed outward, so it draws the inner layer pushed through the outer one. On a flat garment that covered it in the edge colour
    // (the underpants went flat grey); on hair strands it pokes the inner layer out through the outer one and through the body beside it,
    // which reads as hair clipping back-to-front. Such a mesh gets no edge; its texture still shows. Single-layer meshes (the skirt) keep theirs.
    const bool innerLayer = HasInnerLayer(scene, mat);
    const bool hair = mat.shader.find("hair") != std::string::npos;
    if (!overlay && !lens && !transparent) look.drawFlags |= cutout ? 0x08 : 0x02 | 0x04 | 0x08;
    // Hair gets no edge either: the game's outline width for hair is mostly 0, and MMD's fixed 0.6 edge, drawn in the
    // card's dark outline colour, covers these thin strands and reads as a second, black, shaggy hair over the real
    // one (in the viewer there is no such outline, so the hair there is purple and clean). Its shading still comes
    // from the toon ramp and the sphere gloss below.
    if (!overlay && !lens && !tooth && !cutout && !innerLayer && !hair) look.drawFlags |= 0x10;
    // The edge: MMD draws the back faces pushed outwards in one colour. A dark game outline colour (the card's hair line) is kept; the
    // game's light default (0.5 grey) darkens the surface it rings, so it is applied to the texture's mean colour (a skin edge is a
    // dark skin tone, never the white rim a light colour made round hair strands lying on the face).
    Vec3 e = mat.outlineColor;
    if (0.3f * e.x + 0.59f * e.y + 0.11f * e.z >= 0.3f) {
        const Vec3 m = MeanColour(scene, mat);
        e = {e.x * m.x, e.y * m.y, e.z * m.z};
    }
    look.edgeColor = {e.x, e.y, e.z, 1.0f};
    look.edgeSize = 0.6f;   // the hand-made reference models' 0.6; the game's own outline widths are not material data (mostly 0)
    // Sphere maps, the gloss MMD models carry instead of specular, chosen from the material itself: the teeth get roundness (multiply), any
    // hair shader the anime ring highlight, and any other surface the game gives a highlight (_SpecularColor x _SpecularPower) a soft add-gloss
    // as strong as that highlight. Skin, the face's layers, lenses and lit bakes get none.
    const bool skin = mat.shader.find("skin") != std::string::npos;
    const float gloss = 0.3f * mat.specular.x + 0.59f * mat.specular.y + 0.11f * mat.specular.z;
    if (tooth) look.sphere = 1;
    else if (!mat.bakedLit && !overlay && !lens && hair) { look.sphere = 3; look.sphereStrength = 0.15f; }
    else if (!mat.bakedLit && !overlay && !lens && !skin && gloss * 0.25f >= 0.02f) { look.sphere = 2; look.sphereStrength = std::min(0.25f, gloss * 0.25f); }
    return look;
}

assemble::SceneTexture MmdSphereImage(const MmdLook& look) {
    constexpr int kS = 64;
    assemble::SceneTexture t;
    const int pct = static_cast<int>(std::lround(look.sphereStrength * 100.0f));
    t.name = look.sphere == 1 ? "sphere_tooth" : (look.sphere == 3 ? "sphere_hair_" : "sphere_gloss_") + std::to_string(pct);
    t.width = t.height = kS;
    t.rgba.resize(static_cast<size_t>(kS) * kS * 4);
    // u right, v up, z toward the camera: the normal as MMD looks the sphere up (its view-space x and y).
    const float lx = look.sphere == 1 ? -0.35f : 0.35f, ly = look.sphere == 1 ? 0.45f : 0.55f, lz = look.sphere == 1 ? 0.82f : 0.75f;
    const float ln = std::sqrt(lx * lx + ly * ly + lz * lz);
    for (int y = 0; y < kS; ++y)
        for (int x = 0; x < kS; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / kS * 2.0f - 1.0f, v = 1.0f - (static_cast<float>(y) + 0.5f) / kS * 2.0f;
            const float r2 = std::min(1.0f, u * u + v * v), z = std::sqrt(1.0f - r2);
            const float d = std::max(0.0f, (u * lx + v * ly + z * lz) / ln);
            float g;
            if (look.sphere == 1) g = 0.62f + 0.38f * d;   // teeth: bright toward the upper left front, 0.62 at the grazing edge
            else if (look.sphere == 3) g = look.sphereStrength * std::exp(-((v - 0.45f) / 0.12f) * ((v - 0.45f) / 0.12f)) * (1.0f - std::min(1.0f, u * u));   // a band across the upper head
            else g = look.sphereStrength * std::pow(d, 12.0f);   // a soft highlight toward MMD's light (upper right, front)
            uint8_t* px = &t.rgba[(static_cast<size_t>(y) * kS + static_cast<size_t>(x)) * 4];
            px[0] = px[1] = px[2] = static_cast<uint8_t>(std::min(255.0f, 255.0f * g + 0.5f));
            px[3] = 255;
        }
    return t;
}

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
    j["tool"] = {{"name", "cardbridge"}, {"version", CARDBRIDGE_VERSION}, {"author", CARDBRIDGE_AUTHOR}};
    j["input"] = {{"filename", fileio::FileName(card.path)}, {"kind", "koikatsu_character_card"}, {"sha256", card.sha256},
                  {"family", card::FamilyName(card.family)}, {"marker", card.marker}, {"format_version", card.formatVersion}};
    j["character"] = {{"name", scene.name}, {"outfit", opt.outfit}};
    j["game_adapter"] = "koikatsu";
    j["provenance"] = {{"asset_library", opt.assetLibrary}, {"mods_folder", opt.modsFolder}};
    j["export"] = {{"format", "pmx"}, {"file", pmxFile}, {"scale", opt.scale}, {"axes", "y-up, left-handed"}, {"facing", opt.faceMinusZ ? "-z" : "+z"},
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
        if (p.id.find(".shoes") != std::string::npos) {
            // Provenance for Stiletto heel reconstruction (section 4): which config, which
            // slot and which list-name it matched, or "geometry" when no config supplied it.
            jp["heel"] = {{"list_name", p.listName}, {"has_config", p.hasHeelConfig}, {"source", p.heelConfigSource},
                          {"ankle_deg", p.heelAnkleDeg}, {"leg_deg", p.heelLegDeg}, {"toe_deg", p.heelToeDeg},
                          {"height_m", p.heelHeightMeters}, {"shoe_scale", {p.heelShoeScaleX, p.heelShoeScaleY, p.heelShoeScaleZ}},
                          {"shoe_angle_deg", p.heelShoeAngleDeg}, {"shoe_translate_y", p.heelShoeTranslateY},
                          {"shoe_translate_z", p.heelShoeTranslateZ}, {"shoe_shear_y", p.heelShoeShearY}, {"shoe_shear_z", p.heelShoeShearZ}};
        }
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
    std::vector<int> solidTexMap(scene.textures.size(), -1);   // the same, written with alpha 255 (for the item shader)
    pmx::Model model;
    model.name = scene.name;
    {   // the model information MikuMikuDance shows (it wants Windows line breaks): the tool, its author, where the model came from
        const std::string author = CARDBRIDGE_AUTHOR;   // fixed: not an option, not read from the environment
        model.nameEn = scene.name;
        model.comment = "Converted by CardBridge " CARDBRIDGE_VERSION "\r\nCardBridge by " + author + "\r\nCharacter: " + scene.name + "\r\nSource card: " + fileio::FileName(card.path);
        model.commentEn = model.comment;
    }

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
    std::map<std::string, int> toonFiles;   // ramp bytes -> PMX texture index
    std::map<std::string, int> sphereFiles; // sphere map name -> PMX texture index
    int meshOrdinal = 0;
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& part = scene.parts[pi];
        if (!part.includeInExport) continue;
        range[pi].first = static_cast<int>(model.materials.size());
        // The teeth are drawn before the face: in MikuMikuDance teeth drawn after it never showed, however the mouth opened (the game's face has
        // a see-through window onto the mouth, and MMD's depth test evidently still takes it for a surface); drawn first they show through.
        std::vector<int> drawOrder = part.meshes;
        std::stable_partition(drawOrder.begin(), drawOrder.end(), [&](int mi) {
            for (const auto& sm : scene.meshes[static_cast<size_t>(mi)].submeshes) {
                const std::string& n = scene.materials[static_cast<size_t>(sm.material)].name;
                if (n.find("tooth") != std::string::npos || n.find("canine") != std::string::npos) return true;
            }
            return false;
        });
        for (int mi : drawOrder) {
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
                const MmdLook look = MmdMaterialLook(scene, mat);
                pm.diffuse = look.diffuse;
                pm.ambient = look.ambient;
                pm.specular = look.specular;
                pm.specularPower = look.specularPower;
                pm.drawFlags = look.drawFlags;
                pm.edgeColor = look.edgeColor;
                pm.edgeSize = look.edgeSize;
                if (!mat.toon.empty() && !mat.bakedLit) pm.toon = ToonTexture(mat.toon, model, toonFiles, opt);   // the game shader's shade, as the material's own toon ramp (bright at the top, like MMD's)
                if (look.sphere > 0) { pm.sphere = SphereTexture(model, sphereFiles, opt, look); pm.sphereMode = look.sphere == 1 ? 1 : 2; }
                if (mat.texture >= 0) {
                    size_t ti = static_cast<size_t>(mat.texture);
                    // The game's item shader (main_item) is opaque but discards where the texture's alpha is below 0.25 (a card's
                    // MaterialEditor image cut to a shape): this material gets the image with exactly that, alpha 0 or 255, so MMD neither
                    // blends the rest see-through nor shows the cut texels (which were black).
                    const bool solid = !mat.alpha && mat.shader.find("main_item") != std::string::npos;
                    int& pmxTex = solid ? solidTexMap[ti] : texMap[ti];
                    if (pmxTex < 0) {
                        // Encoded straight from the scene texture: copying a 2048 texture (16 MB) only to hand it to the
                        // PNG encoder was a needless transient at export time. The solid variant needs its own copy
                        // because its alpha channel is rewritten.
                        assemble::SceneTexture solidCopy;
                        const assemble::SceneTexture* tp = &scene.textures[ti];
                        if (solid) {
                            solidCopy = scene.textures[ti];
                            for (size_t px = 3; px < solidCopy.rgba.size(); px += 4) solidCopy.rgba[px] = solidCopy.rgba[px] < kItemCutout ? 0 : 255;
                            tp = &solidCopy;
                        }
                        const assemble::SceneTexture& t = *tp;
                        std::string file = "textures/" + safeFileName(t.name) + "_" + std::to_string(ti) + (solid ? "_opaque" : "") + ".png";
                        Bytes png = encodePng(t);
                        fileio::WriteFile(fileio::Join(opt.outDir, file), png.data(), png.size());
                        log("texture " + t.name + " (" + std::to_string(t.width) + "x" + std::to_string(t.height) + ")");
                        if (textureFiles[ti].empty()) textureFiles[ti] = file;
                        pmxTex = static_cast<int>(model.textures.size());
                        model.textures.push_back(file);
                    }
                    pm.texture = pmxTex;
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
            // The under pieces themselves, not the "Nude body" group morph: MikuMikuDance crashes on a group morph inside a group morph.
            if (nudeIndex >= 0) for (auto& [ui, mi] : showMorph) if (scene.parts[ui].underPiece) all.group.emplace_back(mi, 1.0f);
            if (!all.group.empty()) addMorph(std::move(all));
        }
    }
    std::map<std::string, std::string> expressionShapes;
    const size_t expressionMorphs = AddExpressionMorphs(scene, model, meshBase, opt.scale, &expressionShapes);
    if (std::getenv("CARDBRIDGE_DEBUG_MORPHS")) for (const auto& [n, sh] : expressionShapes) std::fprintf(stderr, "expression %s = %s\n", n.c_str(), sh.c_str());
    const std::vector<int> mmdMorphs = opt.mmdNames ? AddMmdMorphs(model, expressionShapes) : std::vector<int>();
    for (const auto& m : model.morphs) morphNames.push_back(m.name);
    if (opt.physics) AddPhysics(scene, model, opt.scale, opt.jiggleBust, opt.jiggleButt, opt.jiggleIntensity);
    std::vector<int> ikBones = AddIk(scene, model, opt.scale, !opt.mmdNames);   // MMD names: no arm IK, it would pin the arm to the rest pose against a motion's FK keys
    std::vector<int> mmdExtraBones;
    if (opt.mmdNames) {
        const MmdBones mb = ApplyMmdBoneNames(scene, model, opt.scale);
        ikBones.insert(ikBones.end(), mb.ik.begin(), mb.ik.end());
        mmdExtraBones = mb.extra;
    }
    {   // display frames: Root, Exp (the first 255 morphs), then the bones by group
        pmx::DisplayFrame root;
        root.name = "Root"; root.special = true;
        pmx::DisplayFrame exp;
        exp.name = "\xE8\xA1\xA8\xE6\x83\x85"; exp.nameEn = "Exp"; exp.special = true;   // 表情
        // MikuMikuDance keeps the number of listed morphs in a byte and fills tables sized by it: 256 or more corrupt its heap (a crash a few
        // seconds after the file opens). The outfit morphs and the game's expressions come first, so what does not fit is the raw per-mesh
        // shapes; they stay in the file (the engine and PMX Editor use them) but MMD's morph panel does not list them.
        constexpr size_t kMmdMaxListedMorphs = 255;
        exp.morphs = mmdMorphs;   // the standard ones first
        for (size_t i = 0; i < model.morphs.size() && exp.morphs.size() < kMmdMaxListedMorphs; ++i)
            if (std::find(mmdMorphs.begin(), mmdMorphs.end(), static_cast<int>(i)) == mmdMorphs.end()) exp.morphs.push_back(static_cast<int>(i));
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
        if (!mmdExtraBones.empty()) {
            pmx::DisplayFrame f;
            f.name = "MMD";
            f.bones = mmdExtraBones;
            model.frames.push_back(std::move(f));
        }
    }
    if (opt.mmdNames) AddMmdStandardBones(model);
    log("expression morphs: " + std::to_string(expressionMorphs) + ", rigid bodies: " + std::to_string(model.rigidBodies.size()) + ", joints: " + std::to_string(model.joints.size()));

    if (opt.faceMinusZ) pmx::TurnAboutY(model);
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
