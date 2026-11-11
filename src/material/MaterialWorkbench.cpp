#include "material/MaterialWorkbench.h"

#include "core/Bytes.h"
#include "shader/Sm4.h"
#include "shader/UnityShader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace cardbridge::material {

namespace {

std::string Lower(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool ContainsLower(const std::string& hay, const std::string& needle) {
    return Lower(hay).find(Lower(needle)) != std::string::npos;
}

float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

const Vec4* FindProp(const assemble::SceneMaterial& m, const std::string& name) {
    for (const auto& pr : m.props) if (pr.first == name) return &pr.second;
    return nullptr;
}

void SetProp(assemble::SceneMaterial& m, const std::string& name, Vec4 v) {
    for (auto& pr : m.props) if (pr.first == name) { pr.second = v; return; }
    m.props.emplace_back(name, v);
}

// Highlight strength used by the game's shaders: _SpecularPower is a 0..1
// strength (Assembler.cpp: it clamps it to 0..1 before scaling the specular
// colour). Roughness is modelled as its inverse (approximation, see
// MATERIAL_EDITOR.md 3.4). The PMX exponent is the strength scaled to the
// usual 0..5 range.
float StrengthFromRoughness(float r) { return Clamp01(1.0f - r); }
float RoughnessFromStrength(float s) { return Clamp01(1.0f - s); }

bool SameVec4(const Vec4& a, const Vec4& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

// A multiplier that maps the original channel to the requested one. Black
// cannot be brightened by a multiplier (0 * anything stays 0), so a zero
// original keeps the texture unchanged instead of producing a wild ratio.
float SafeChannelRatio(float now, float original) {
    if (!(original > 1e-4f) || !std::isfinite(now) || !std::isfinite(original)) return 1.0f;
    return std::clamp(now / original, 0.0f, 64.0f);
}

void BindMainTexture(assemble::SceneMaterial& m, int textureIndex) {
    bool found = false;
    for (auto& b : m.texProps)
        if (b.name == "_MainTex") { b.texture = textureIndex; found = true; }
    if (!found && textureIndex >= 0) {
        assemble::SceneTexBinding b;
        b.name = "_MainTex";
        b.texture = textureIndex;
        m.texProps.push_back(b);
    }
    m.texture = textureIndex;
}

} // namespace

// Whether the forward pixel program provably reads the _Color constant-buffer
// parameter: 1 = reads it, 0 = provably does not, -1 = unknown (the shader is
// missing or unparseable). A dead _Color declaration that no instruction reads
// is treated as "does not read" - main_opaque / main_alpha / main_skin bake
// their colour into _MainTex and ignore _Color entirely.
int ShaderReadsColor(const assemble::Scene& scene, int shaderIndex) {
    if (shaderIndex < 0 || static_cast<size_t>(shaderIndex) >= scene.shaders.size()) return -1;
    const auto& sh = scene.shaders[static_cast<size_t>(shaderIndex)];
    Bytes blob(sh.blob.begin(), sh.blob.end());
    const std::vector<shader::UnityProgram> progs = shader::ParseUnityShaderBlob(blob, nullptr);
    size_t pi = 0, vi = 0;
    shader::Sm4Program ps;
    if (!shader::PickForwardPass(progs, pi, vi) || !shader::DecodeSm4(progs[pi].code.tokens, ps, nullptr)) return -1;
    // cbuffer operands carry their slot and the float4 offset in index0/index1.
    std::set<std::pair<int, int>> used;
    for (const auto& in : ps.code)
        for (const auto& o : in.operands)
            if (o.type == 8) used.insert({o.index0, o.index1});
    for (const auto& cb : progs[pi].cbuffers)
        for (const auto& pm : cb.params)
            if (pm.name == "_Color" && used.count({cb.slot, static_cast<int>(pm.offset / 16)}) != 0) return 1;
    return 0;
}

// Whether the material's forward pixel program provably does NOT read the
// _Color constant-buffer parameter. This is the exact bytecode test the debug
// dump used (out/mats_debug.txt): a property can be present in the material and
// declared by the shader yet never sampled; main_skin / main_opaque /
// main_alpha read the colour baked into _MainTex and ignore _Color entirely.
// Unknown/unparseable shaders stay on the cheap _Color write path.
bool PixelIgnoresColor(const assemble::Scene& scene, WorkbenchState& st, int shaderIndex) {
    if (shaderIndex < 0 || static_cast<size_t>(shaderIndex) >= scene.shaders.size()) return false;
    if (st.readsColor.size() != scene.shaders.size()) st.readsColor.assign(scene.shaders.size(), 0);
    uint8_t& state = st.readsColor[static_cast<size_t>(shaderIndex)];
    if (state == 2) return true;          // known: ignores _Color
    if (state == 1 || state == 3) return false;
    int reads = ShaderReadsColor(scene, shaderIndex);
    state = reads == 0 ? 2 : (reads == 1 ? 1 : 3);
    return state == 2;
}

assemble::SceneTexture MakeTintedTexture(assemble::SceneTexture src, const Vec4& ratio) {
    const size_t pixels = static_cast<size_t>(std::max(0, src.width)) * static_cast<size_t>(std::max(0, src.height));
    if (pixels == 0 || src.rgba.size() < pixels * 4) return src;
    auto scale = [](uint8_t v, float k) {
        return static_cast<uint8_t>(std::lround(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)));
    };
    for (size_t i = 0; i + 3 < pixels * 4; i += 4) {
        src.rgba[i + 0] = scale(src.rgba[i + 0], ratio.x);
        src.rgba[i + 1] = scale(src.rgba[i + 1], ratio.y);
        src.rgba[i + 2] = scale(src.rgba[i + 2], ratio.z);
        src.rgba[i + 3] = scale(src.rgba[i + 3], ratio.w);
    }
    src.pixelHash = Fnv1a64(src.rgba.data(), pixels * 4);
    src.format = "RGBA8 (tint copy)";
    return src;
}

Vec3 RgbToHsv(const Vec3& rgb) {
    const float r = Clamp01(rgb.x), g = Clamp01(rgb.y), b = Clamp01(rgb.z);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    float h = 0.0f;
    if (d > 1e-6f) {
        if (mx == r) h = std::fmod((g - b) / d, 6.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }
    h /= 360.0f;
    const float s = mx <= 1e-6f ? 0.0f : d / mx;
    return {h, Clamp01(s), Clamp01(mx)};
}

Vec3 HsvToRgb(float h, float s, float v) {
    s = Clamp01(s);
    v = Clamp01(v);
    h = h - std::floor(h);   // wrap 1.0 -> 0.0 (red)
    const float sector = h * 6.0f;
    const int i = static_cast<int>(std::floor(sector));
    const float f = sector - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

const char* ClassName(MaterialClass c) {
    switch (c) {
    case MaterialClass::Skin: return "Skin";
    case MaterialClass::Fabric: return "Fabric";
    case MaterialClass::Hair: return "Hair";
    case MaterialClass::Eyes: return "Eyes";
    case MaterialClass::Eyelashes: return "Eyelashes";
    case MaterialClass::Metal: return "Metal";
    case MaterialClass::Plastic: return "Plastic";
    case MaterialClass::Leather: return "Leather";
    case MaterialClass::Glass: return "Glass";
    case MaterialClass::Rubber: return "Rubber";
    case MaterialClass::Generic: return "Generic";
    }
    return "Generic";
}

bool ClassFromName(const std::string& s, MaterialClass& out) {
    // Accept "Fabric", "Fabric/Default" (a preset name) and case-insensitively.
    std::string base = s;
    const size_t slash = base.find('/');
    if (slash != std::string::npos) base = base.substr(0, slash);
    std::string low = Lower(base);
    const MaterialClass all[] = {MaterialClass::Skin, MaterialClass::Fabric, MaterialClass::Hair, MaterialClass::Eyes,
                                 MaterialClass::Eyelashes, MaterialClass::Metal, MaterialClass::Plastic, MaterialClass::Leather,
                                 MaterialClass::Glass, MaterialClass::Rubber, MaterialClass::Generic};
    for (MaterialClass c : all) if (low == Lower(ClassName(c))) { out = c; return true; }
    return false;
}

MaterialClass ClassifyMaterial(const std::string& shaderName, const std::string& materialName) {
    // Order matters: glasses before skin/hair (lenses are not skin), lashes
    // before eyes (eyeline is not the eyeball), hair before the main_ fabric
    // group (main_hair is hair, not fabric).
    auto has = [&](const char* s) { return ContainsLower(shaderName, s) || ContainsLower(materialName, s); };
    if (has("glasses")) return MaterialClass::Glass;
    if (has("toon_nose") || has("brow") || has("lash") || has("eyeline") || has("eyelash")) return MaterialClass::Eyelashes;
    if (has("toon_eye") || has("eye")) return MaterialClass::Eyes;
    if (has("skin")) return MaterialClass::Skin;
    if (has("hair")) return MaterialClass::Hair;
    if (has("main_item") || has("main_opaque") || has("main_alpha")) return MaterialClass::Fabric;
    return MaterialClass::Generic;
}

std::string PresetName(MaterialClass cls) { return std::string(ClassName(cls)) + "/Default"; }

bool MaterialOverride::empty() const {
    return !class_ && !baseColor && !opacity && !roughness && !metallic && !specular && !specularPower &&
           !emissionColor && !emissionIntensity && !normalIntensity && !anisotropic && !transmission && !subsurface &&
           !doubleSided && !displayName && !preset && textures.empty();
}

MaterialOverride PresetForClass(MaterialClass cls) {
    MaterialOverride o;
    o.class_ = cls;
    o.preset = PresetName(cls);
    switch (cls) {
    case MaterialClass::Skin:
        o.metallic = 0.0f;
        o.roughness = 0.55f;
        o.specular = Vec3{0.25f, 0.25f, 0.25f};
        o.normalIntensity = 1.0f;
        o.subsurface = SubsurfaceParams{{1, 0.65f, 0.55f}, 0.35f};
        break;
    case MaterialClass::Fabric:
        o.metallic = 0.0f;
        o.roughness = 0.85f;
        o.specular = Vec3{0.08f, 0.08f, 0.08f};
        o.normalIntensity = 1.0f;
        o.transmission = TransmissionParams{0.25f, {1, 1, 1}, 1.0f, true};
        break;
    case MaterialClass::Hair:
        o.metallic = 0.0f;
        o.roughness = 0.5f;
        o.specular = Vec3{0.5f, 0.5f, 0.5f};
        o.anisotropic = 0.5f;
        o.normalIntensity = 1.0f;
        break;
    case MaterialClass::Eyes:
        o.metallic = 0.0f;
        o.roughness = 0.15f;
        o.specular = Vec3{0.8f, 0.8f, 0.8f};
        o.emissionColor = Vec3{1, 1, 1};
        o.emissionIntensity = 0.1f;
        break;
    case MaterialClass::Eyelashes:
        o.metallic = 0.0f;
        o.roughness = 0.4f;
        o.specular = Vec3{0.2f, 0.2f, 0.2f};
        break;
    case MaterialClass::Metal:
        o.metallic = 1.0f;
        o.roughness = 0.25f;
        o.specular = Vec3{1, 1, 1};
        break;
    case MaterialClass::Plastic:
        o.metallic = 0.0f;
        o.roughness = 0.35f;
        o.specular = Vec3{0.6f, 0.6f, 0.6f};
        break;
    case MaterialClass::Leather:
        o.metallic = 0.0f;
        o.roughness = 0.65f;
        o.specular = Vec3{0.3f, 0.3f, 0.3f};
        break;
    case MaterialClass::Glass:
        o.metallic = 0.0f;
        o.roughness = 0.05f;
        o.specular = Vec3{1, 1, 1};
        o.opacity = 0.35f;
        break;
    case MaterialClass::Rubber:
        o.metallic = 0.0f;
        o.roughness = 0.95f;
        o.specular = Vec3{0.05f, 0.05f, 0.05f};
        break;
    case MaterialClass::Generic:
        o.roughness = 0.7f;
        o.specular = Vec3{0.4f, 0.4f, 0.4f};
        break;
    }
    return o;
}

// ---- WorkbenchState ----------------------------------------------------

bool WorkbenchState::IsDirty() const {
    for (const auto& o : overrides) if (o && !o->empty()) return true;
    return false;
}

size_t WorkbenchState::OverrideCount() const {
    size_t n = 0;
    for (const auto& o : overrides) if (o && !o->empty()) ++n;
    return n;
}

std::string PartForMaterial(const assemble::Scene& scene, int materialIndex) {
    if (materialIndex < 0) return {};
    for (const auto& part : scene.parts) {
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes)
                if (sub.material == materialIndex) return part.id;
        }
    }
    return {};
}

int CountSubmeshesForMaterial(const assemble::Scene& scene, int materialIndex) {
    if (materialIndex < 0) return 0;
    int n = 0;
    for (const auto& mesh : scene.meshes)
        for (const auto& sub : mesh.submeshes)
            if (sub.material == materialIndex) ++n;
    return n;
}

int FindMaterialIndex(const assemble::Scene& scene, const std::string& partId, const std::string& materialName) {
    if (scene.materials.empty()) return -1;
    if (!partId.empty()) {
        for (const auto& part : scene.parts) {
            if (part.id != partId) continue;
            for (int mi : part.meshes) {
                if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
                for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes) {
                    const int idx = sub.material;
                    if (idx < 0 || static_cast<size_t>(idx) >= scene.materials.size()) continue;
                    if (scene.materials[static_cast<size_t>(idx)].name == materialName) return idx;
                }
            }
        }
        return -1;
    }
    for (size_t i = 0; i < scene.materials.size(); ++i)
        if (scene.materials[i].name == materialName) return static_cast<int>(i);
    return -1;
}

void CaptureOriginals(WorkbenchState& st, const assemble::Scene& scene) {
    const bool resized = st.originals.size() != scene.materials.size();
    st.originals.resize(scene.materials.size());
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        st.originals[i].mat = scene.materials[i];
        st.originals[i].autoClass = ClassifyMaterial(scene.materials[i].shader, scene.materials[i].name);
        st.originals[i].part = PartForMaterial(scene, static_cast<int>(i));
    }
    // A new baseline means a new scene (or a changed material table): cached
    // tint copies and shader analyses belong to the previous one.
    st.tintTextures.assign(scene.materials.size(), -1);
    st.tintCache.assign(scene.materials.size(), WorkbenchState::TintCache{});
    st.changedTextures.clear();
    st.readsColor.assign(scene.shaders.size(), 0);
    if (resized) {
        st.overrides.assign(scene.materials.size(), std::nullopt);
        st.undoStack.clear();
        st.redoStack.clear();
    } else if (st.overrides.size() != scene.materials.size()) {
        st.overrides.resize(scene.materials.size());
    }
}

EffectiveMaterial EffectiveOf(const OriginalMaterial& orig, const MaterialOverride* ov) {
    EffectiveMaterial e;
    e.displayName = orig.mat.name;
    e.doubleSided = orig.mat.doubleSided;
    // The game's shaders take their tint from the _Color property, not from
    // SceneMaterial::diffuse (the assembler leaves diffuse at white for a
    // normal material: the colour lives in _Color, or is already baked into
    // the main texture for a create-job material). Reading diffuse first made
    // every material show FFFFFF even though _Color held the real colour.
    const Vec4* colorProp = FindProp(orig.mat, "_Color");
    e.baseColor = ov && ov->baseColor ? *ov->baseColor : (colorProp ? *colorProp : orig.mat.diffuse);
    e.opacity = ov && ov->opacity ? *ov->opacity : e.baseColor.w;
    e.cls = ov && ov->class_ ? *ov->class_ : orig.autoClass;
    if (ov && ov->displayName) e.displayName = *ov->displayName;
    if (ov && ov->doubleSided) e.doubleSided = *ov->doubleSided;
    // Highlight strength: explicit power wins, then roughness, then the
    // material's own _SpecularPower, then a neutral 0.4.
    float strength;
    if (ov && ov->specularPower) strength = Clamp01(*ov->specularPower);
    else if (ov && ov->roughness) strength = StrengthFromRoughness(*ov->roughness);
    else if (const Vec4* p = FindProp(orig.mat, "_SpecularPower")) strength = Clamp01(p->x);
    else strength = 0.4f;
    e.specularPower = strength;
    e.roughness = ov && ov->roughness ? Clamp01(*ov->roughness) : RoughnessFromStrength(strength);
    if (ov && ov->specular) e.specular = *ov->specular;
    else if (const Vec4* p = FindProp(orig.mat, "_SpecularColor")) e.specular = {p->x, p->y, p->z};
    else e.specular = orig.mat.specular;
    e.metallic = ov && ov->metallic ? Clamp01(*ov->metallic) : 0.0f;
    if (ov && ov->emissionColor) e.emissionColor = *ov->emissionColor;
    else if (const Vec4* p = FindProp(orig.mat, "_EmissionColor")) e.emissionColor = {p->x, p->y, p->z};
    if (ov && ov->emissionIntensity) e.emissionIntensity = std::max(0.0f, *ov->emissionIntensity);
    else e.emissionIntensity = (e.emissionColor.x != 0 || e.emissionColor.y != 0 || e.emissionColor.z != 0) ? 1.0f : 0.0f;
    e.normalIntensity = ov && ov->normalIntensity ? *ov->normalIntensity : 1.0f;
    e.anisotropic = ov && ov->anisotropic ? *ov->anisotropic : 0.0f;
    if (ov && ov->transmission) e.transmission = *ov->transmission;
    if (ov && ov->subsurface) e.subsurface = *ov->subsurface;
    (void)colorProp;
    return e;
}

EffectiveMaterial EffectiveOf(const WorkbenchState& st, size_t index) {
    if (index >= st.originals.size()) return {};
    const MaterialOverride* ov = index < st.overrides.size() && st.overrides[index] ? &*st.overrides[index] : nullptr;
    return EffectiveOf(st.originals[index], ov);
}

void ApplyOverrides(assemble::Scene& scene, WorkbenchState& st) {
    if (st.originals.size() != scene.materials.size()) CaptureOriginals(st, scene);
    if (st.tintTextures.size() != scene.materials.size()) st.tintTextures.assign(scene.materials.size(), -1);
    if (st.tintCache.size() != scene.materials.size()) st.tintCache.assign(scene.materials.size(), WorkbenchState::TintCache{});
    st.changedTextures.clear();
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        // Restore from the original first: clearing a field really reverts it.
        assemble::SceneMaterial m = st.originals[i].mat;
        const MaterialOverride* ov = i < st.overrides.size() && st.overrides[i] ? &*st.overrides[i] : nullptr;
        if (ov && !ov->empty()) {
            if (ov->displayName) m.name = *ov->displayName;
            if (ov->doubleSided) m.doubleSided = *ov->doubleSided;
            if (ov->baseColor) {
                m.diffuse = *ov->baseColor;
                // Write _Color even when the material did not store one: the
                // shader declares it, and a material whose colour is baked into
                // its main texture (skin, outfit create results) has no stored
                // _Color at all, so updating only diffuse changed nothing on
                // the game-shader path. Adding it gives that tint back.
                Vec4 v = FindProp(m, "_Color") ? *FindProp(m, "_Color") : Vec4{1, 1, 1, 1};
                v.x = m.diffuse.x; v.y = m.diffuse.y; v.z = m.diffuse.z; v.w = m.diffuse.w;
                SetProp(m, "_Color", v);
            }
            if (ov->opacity) {
                m.diffuse.w = *ov->opacity;
                Vec4 v = FindProp(m, "_Color") ? *FindProp(m, "_Color") : Vec4{1, 1, 1, 1};
                v.w = *ov->opacity;
                SetProp(m, "_Color", v);
            }
            // Specular colour + highlight strength (see StrengthFromRoughness).
            {
                const Vec3 specColor = ov->specular ? *ov->specular : (FindProp(m, "_SpecularColor") ? Vec3{FindProp(m, "_SpecularColor")->x, FindProp(m, "_SpecularColor")->y, FindProp(m, "_SpecularColor")->z} : m.specular);
                float strength;
                if (ov->specularPower) strength = Clamp01(*ov->specularPower);
                else if (ov->roughness) strength = StrengthFromRoughness(*ov->roughness);
                else if (const Vec4* p = FindProp(m, "_SpecularPower")) strength = Clamp01(p->x);
                else strength = Clamp01(m.specularPower / 5.0f);
                Vec3 scaled{specColor.x * strength, specColor.y * strength, specColor.z * strength};
                m.specular = scaled;
                m.specularPower = strength * 5.0f;
                SetProp(m, "_SpecularColor", {specColor.x, specColor.y, specColor.z, specColor.x != 0 || specColor.y != 0 || specColor.z != 0 ? 1.0f : 0.0f});
                SetProp(m, "_SpecularPower", {strength, 0, 0, 0});
            }
            if (ov->metallic) SetProp(m, "_Metallic", {Clamp01(*ov->metallic), 0, 0, 0});
            if (ov->emissionColor || ov->emissionIntensity) {
                Vec3 col = ov->emissionColor ? *ov->emissionColor : (FindProp(m, "_EmissionColor") ? Vec3{FindProp(m, "_EmissionColor")->x, FindProp(m, "_EmissionColor")->y, FindProp(m, "_EmissionColor")->z} : Vec3{0, 0, 0});
                const float k = ov->emissionIntensity ? *ov->emissionIntensity : 1.0f;
                SetProp(m, "_EmissionColor", {col.x * k, col.y * k, col.z * k, 1.0f});
            }
            // Texture slots: enable/disable or rebind.
            for (const auto& t : ov->textures) {
                const std::string prop = t.property.empty() ? "_MainTex" : t.property;
                auto bind = [&](int texIdx) {
                    bool found = false;
                    for (auto& b : m.texProps) if (b.name == prop) { b.texture = texIdx; found = true; }
                    if (!found && texIdx >= 0) { assemble::SceneTexBinding b; b.name = prop; b.texture = texIdx; m.texProps.push_back(b); }
                    if (prop == "_MainTex") m.texture = texIdx;
                };
                if (!t.enabled) bind(-1);
                else if (t.hasTexture()) {
                    int idx = t.textureIndex;
                    if (idx < 0 || static_cast<size_t>(idx) >= scene.textures.size()) {
                        idx = -1;
                        if (!t.textureName.empty())
                            for (size_t k = 0; k < scene.textures.size(); ++k) if (scene.textures[k].name == t.textureName) { idx = static_cast<int>(k); break; }
                        if (idx < 0) { scene.warn(st.originals[i].part, "material workbench: texture '" + t.textureName + "' for " + m.name + " not in the scene; kept original"); continue; }
                    }
                    bind(idx);
                }
            }
            // Base colour routing. A shader that reads _Color takes the cheap
            // property write above. The game's main_skin / main_opaque /
            // main_alpha do not read _Color at all: their create job already
            // painted the card's colour into _MainTex, so a _Color edit cannot
            // show. Give those a tinted copy of the main texture instead (the
            // ratio of the requested colour to the original one) and leave
            // diffuse white so the plain renderer and PMX do not tint twice.
            if ((ov->baseColor || ov->opacity) && m.texture >= 0 &&
                static_cast<size_t>(m.texture) < scene.textures.size() && PixelIgnoresColor(scene, st, m.shaderIndex)) {
                const int sourceIndex = m.texture;
                int tintIndex = st.tintTextures[i];
                if (tintIndex < 0 || static_cast<size_t>(tintIndex) >= scene.textures.size()) {
                    scene.textures.emplace_back();
                    tintIndex = static_cast<int>(scene.textures.size()) - 1;
                    st.tintTextures[i] = tintIndex;
                    st.tintCache[i] = WorkbenchState::TintCache{};
                }
                const assemble::SceneTexture& source = scene.textures[static_cast<size_t>(sourceIndex)];
                if (source.width > 0 && source.height > 0 &&
                    source.rgba.size() >= static_cast<size_t>(source.width) * static_cast<size_t>(source.height) * 4) {
                    const EffectiveMaterial orig = EffectiveOf(st.originals[i], nullptr);
                    Vec4 ratio{1, 1, 1, 1};
                    if (ov->baseColor) {
                        ratio.x = SafeChannelRatio(m.diffuse.x, orig.baseColor.x);
                        ratio.y = SafeChannelRatio(m.diffuse.y, orig.baseColor.y);
                        ratio.z = SafeChannelRatio(m.diffuse.z, orig.baseColor.z);
                    }
                    ratio.w = SafeChannelRatio(m.diffuse.w, orig.baseColor.w);
                    WorkbenchState::TintCache& cache = st.tintCache[i];
                    const bool same = cache.source == sourceIndex && SameVec4(cache.ratio, ratio) &&
                                      cache.sourceHash == source.pixelHash;
                    if (!same) {
                        const std::string sourceName = source.name;
                        scene.textures[static_cast<size_t>(tintIndex)] = MakeTintedTexture(source, ratio);
                        scene.textures[static_cast<size_t>(tintIndex)].name = sourceName + "~wb";
                        cache.source = sourceIndex;
                        cache.ratio = ratio;
                        cache.sourceHash = source.pixelHash;
                        st.changedTextures.push_back(tintIndex);
                    }
                    BindMainTexture(m, tintIndex);
                    m.diffuse.x = 1.0f; m.diffuse.y = 1.0f; m.diffuse.z = 1.0f;
                    m.diffuse.w = ov->opacity ? *ov->opacity : m.diffuse.w;
                }
            }
        }
        scene.materials[i] = std::move(m);
    }
}

std::vector<std::string> UnsupportedFields(const MaterialOverride& ov) {
    std::vector<std::string> out;
    auto push = [&](const char* name, bool set) { if (set) out.push_back(name); };
    push("metallic", ov.metallic.has_value());
    push("roughness", ov.roughness.has_value());
    push("emission", ov.emissionColor.has_value() || ov.emissionIntensity.has_value());
    push("normal intensity", ov.normalIntensity.has_value());
    push("transmission", ov.transmission.has_value());
    push("subsurface", ov.subsurface.has_value());
    push("anisotropic highlight", ov.anisotropic.has_value());
    push("classification", ov.class_.has_value());
    return out;
}

void ReportUnsupported(assemble::Scene& scene, const WorkbenchState& st) {
    for (size_t i = 0; i < st.overrides.size() && i < st.originals.size(); ++i) {
        if (!st.overrides[i] || st.overrides[i]->empty()) continue;
        for (const auto& f : UnsupportedFields(*st.overrides[i]))
            scene.warn(st.originals[i].part, "material workbench: '" + st.originals[i].mat.name + "' field '" + f + "' is not stored in PMX (kept in the CardBridge material file only)");
    }
}

// ---- undo/redo + edits ----

void PushUndo(WorkbenchState& st) {
    st.undoStack.push_back(st.overrides);
    if (st.undoStack.size() > 200) st.undoStack.erase(st.undoStack.begin());
    st.redoStack.clear();
}

bool Undo(WorkbenchState& st) {
    if (st.undoStack.empty()) return false;
    st.redoStack.push_back(st.overrides);
    st.overrides = st.undoStack.back();
    st.undoStack.pop_back();
    return true;
}

bool Redo(WorkbenchState& st) {
    if (st.redoStack.empty()) return false;
    st.undoStack.push_back(st.overrides);
    st.overrides = st.redoStack.back();
    st.redoStack.pop_back();
    return true;
}

void SetOverride(WorkbenchState& st, size_t index, MaterialOverride ov) {
    if (index >= st.originals.size()) return;
    if (st.overrides.size() != st.originals.size()) st.overrides.resize(st.originals.size());
    auto vec4eq = [](const std::optional<Vec4>& a, const std::optional<Vec4>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a || (a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w);
    };
    auto vec3eq = [](const std::optional<Vec3>& a, const std::optional<Vec3>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a || (a->x == b->x && a->y == b->y && a->z == b->z);
    };
    auto optEq = [](const auto& a, const auto& b) { return a == b; };
    const MaterialOverride* curPtr = st.overrides[index] ? &*st.overrides[index] : nullptr;
    bool same = false;
    if (!curPtr && ov.empty()) same = true;
    else if (curPtr) {
        const MaterialOverride& cur = *curPtr;
        same = cur.class_ == ov.class_ && vec4eq(cur.baseColor, ov.baseColor) && optEq(cur.opacity, ov.opacity) &&
               optEq(cur.roughness, ov.roughness) && optEq(cur.metallic, ov.metallic) && vec3eq(cur.specular, ov.specular) &&
               optEq(cur.specularPower, ov.specularPower) && vec3eq(cur.emissionColor, ov.emissionColor) &&
               optEq(cur.emissionIntensity, ov.emissionIntensity) && optEq(cur.normalIntensity, ov.normalIntensity) &&
               optEq(cur.anisotropic, ov.anisotropic) && optEq(cur.doubleSided, ov.doubleSided) && optEq(cur.displayName, ov.displayName) &&
               optEq(cur.preset, ov.preset) && cur.transmission.has_value() == ov.transmission.has_value() &&
               (!cur.transmission || (cur.transmission->strength == ov.transmission->strength && cur.transmission->thickness == ov.transmission->thickness && cur.transmission->backface == ov.transmission->backface && cur.transmission->color.x == ov.transmission->color.x && cur.transmission->color.y == ov.transmission->color.y && cur.transmission->color.z == ov.transmission->color.z)) &&
               cur.subsurface.has_value() == ov.subsurface.has_value() &&
               (!cur.subsurface || (cur.subsurface->strength == ov.subsurface->strength && cur.subsurface->color.x == ov.subsurface->color.x && cur.subsurface->color.y == ov.subsurface->color.y && cur.subsurface->color.z == ov.subsurface->color.z)) &&
               cur.textures.size() == ov.textures.size();
        if (same) {
            for (size_t k = 0; k < cur.textures.size(); ++k) {
                const auto& a = cur.textures[k];
                const auto& b = ov.textures[k];
                if (a.property != b.property || a.enabled != b.enabled || a.textureIndex != b.textureIndex || a.textureName != b.textureName) { same = false; break; }
            }
        }
    }
    if (same) { if (ov.empty()) st.overrides[index].reset(); else st.overrides[index] = std::move(ov); return; }
    PushUndo(st);
    if (ov.empty()) st.overrides[index].reset();
    else st.overrides[index] = std::move(ov);
}

void ResetMaterial(WorkbenchState& st, size_t index) {
    if (index >= st.originals.size() || index >= st.overrides.size()) return;
    if (!st.overrides[index] || st.overrides[index]->empty()) return;
    PushUndo(st);
    st.overrides[index].reset();
}

void ResetAll(WorkbenchState& st) {
    if (!st.IsDirty()) return;
    PushUndo(st);
    for (auto& o : st.overrides) o.reset();
}

const char* ResetParamName(ResetParam p) {
    switch (p) {
    case ResetParam::BaseColor: return "base color";
    case ResetParam::Opacity: return "opacity";
    case ResetParam::Roughness: return "roughness";
    case ResetParam::Metallic: return "metallic";
    case ResetParam::Specular: return "specular";
    case ResetParam::Emission: return "emission";
    case ResetParam::NormalIntensity: return "normal intensity";
    case ResetParam::Transmission: return "transmission";
    case ResetParam::Subsurface: return "subsurface";
    case ResetParam::Textures: return "textures";
    case ResetParam::Class: return "classification";
    case ResetParam::All: return "all";
    }
    return "all";
}

void ResetParameter(WorkbenchState& st, size_t index, ResetParam p) {
    if (index >= st.overrides.size() || !st.overrides[index]) return;
    if (p == ResetParam::All) { ResetMaterial(st, index); return; }
    MaterialOverride& o = *st.overrides[index];
    bool changed = false;
    switch (p) {
    case ResetParam::BaseColor: changed = o.baseColor.has_value(); o.baseColor.reset(); break;
    case ResetParam::Opacity: changed = o.opacity.has_value(); o.opacity.reset(); break;
    case ResetParam::Roughness: changed = o.roughness.has_value(); o.roughness.reset(); break;
    case ResetParam::Metallic: changed = o.metallic.has_value(); o.metallic.reset(); break;
    case ResetParam::Specular: changed = o.specular.has_value() || o.specularPower.has_value(); o.specular.reset(); o.specularPower.reset(); break;
    case ResetParam::Emission: changed = o.emissionColor.has_value() || o.emissionIntensity.has_value(); o.emissionColor.reset(); o.emissionIntensity.reset(); break;
    case ResetParam::NormalIntensity: changed = o.normalIntensity.has_value(); o.normalIntensity.reset(); break;
    case ResetParam::Transmission: changed = o.transmission.has_value(); o.transmission.reset(); break;
    case ResetParam::Subsurface: changed = o.subsurface.has_value(); o.subsurface.reset(); break;
    case ResetParam::Textures: changed = !o.textures.empty(); o.textures.clear(); break;
    case ResetParam::Class: changed = o.class_.has_value(); o.class_.reset(); break;
    case ResetParam::All: break;
    }
    if (!changed) return;
    if (o.empty()) st.overrides[index].reset();
}

MaterialOverride CopyOverride(const WorkbenchState& st, size_t index) {
    if (index < st.overrides.size() && st.overrides[index]) return *st.overrides[index];
    return {};
}

void PasteOverride(WorkbenchState& st, size_t index, const MaterialOverride& ov) {
    if (index >= st.originals.size()) return;
    if (st.overrides.size() != st.originals.size()) st.overrides.resize(st.originals.size());
    PushUndo(st);
    if (ov.empty()) st.overrides[index].reset();
    else st.overrides[index] = ov;
}

void PasteToMultiple(WorkbenchState& st, const std::vector<size_t>& targets, const MaterialOverride& ov) {
    if (targets.empty()) return;
    if (st.overrides.size() != st.originals.size()) st.overrides.resize(st.originals.size());
    PushUndo(st);
    for (size_t i : targets) {
        if (i >= st.originals.size()) continue;
        if (ov.empty()) st.overrides[i].reset();
        else st.overrides[i] = ov;
    }
}

void ApplyPreset(WorkbenchState& st, size_t index, MaterialClass cls) {
    if (index >= st.originals.size()) return;
    MaterialOverride p = PresetForClass(cls);
    // Keep any existing base colour / opacity / textures: a preset must not
    // overwrite the colour (spec section 6).
    const MaterialOverride* cur = index < st.overrides.size() && st.overrides[index] ? &*st.overrides[index] : nullptr;
    if (cur) {
        if (cur->baseColor) p.baseColor = cur->baseColor;
        if (cur->opacity) p.opacity = cur->opacity;
        if (!cur->textures.empty()) p.textures = cur->textures;
        if (cur->displayName) p.displayName = cur->displayName;
    }
    SetOverride(st, index, std::move(p));
}

// ---- textures ----

namespace {

TexOverride& TexSlot(WorkbenchState& st, size_t index, const std::string& property) {
    if (st.overrides.size() != st.originals.size()) st.overrides.resize(st.originals.size());
    if (!st.overrides[index]) st.overrides[index] = MaterialOverride{};
    MaterialOverride& o = *st.overrides[index];
    const std::string prop = property.empty() ? "_MainTex" : property;
    for (auto& t : o.textures) if (t.property == prop) return t;
    TexOverride t;
    t.property = prop;
    o.textures.push_back(t);
    return o.textures.back();
}

} // namespace

bool SetTextureEnabled(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, bool enabled) {
    if (index >= st.originals.size()) return false;
    PushUndo(st);
    TexSlot(st, index, property).enabled = enabled;
    ApplyOverrides(scene, st);
    return true;
}

bool RebindTexture(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, int textureIndex) {
    if (index >= st.originals.size()) return false;
    if (textureIndex < -1 || (textureIndex >= 0 && static_cast<size_t>(textureIndex) >= scene.textures.size())) {
        scene.warn(st.originals[index].part, "material workbench: texture index " + std::to_string(textureIndex) + " out of range for '" + st.originals[index].mat.name + "'; kept original");
        return false;
    }
    PushUndo(st);
    TexOverride& t = TexSlot(st, index, property);
    t.enabled = true;
    t.textureIndex = textureIndex;
    t.textureName = textureIndex >= 0 ? scene.textures[static_cast<size_t>(textureIndex)].name : std::string();
    ApplyOverrides(scene, st);
    return true;
}

int ReplaceTexture(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, assemble::SceneTexture texture) {
    if (index >= st.originals.size()) return -1;
    if (texture.width <= 0 || texture.height <= 0 || texture.rgba.size() < static_cast<size_t>(texture.width) * texture.height * 4) {
        scene.warn(st.originals[index].part, "material workbench: replacement texture '" + texture.name + "' has no pixels; kept original");
        return -1;
    }
    texture.pixelHash = 0;
    // FNV-1a 64, same as the assembler (see Assembler.cpp: Fnv1a64).
    {
        uint64_t h = 1469598103934665603ULL;
        for (uint8_t b : texture.rgba) { h ^= b; h *= 1099511628211ULL; }
        texture.pixelHash = h;
    }
    scene.textures.push_back(std::move(texture));
    const int idx = static_cast<int>(scene.textures.size()) - 1;
    const std::string name = scene.textures[static_cast<size_t>(idx)].name;
    PushUndo(st);
    TexOverride& slot = TexSlot(st, index, property);
    slot.enabled = true;
    slot.textureIndex = idx;
    slot.textureName = name;
    ApplyOverrides(scene, st);
    return idx;
}

Vec3 MaterialIdColor(int materialIndex) {
    // Deterministic hash -> colour (stable across runs).
    uint32_t h = static_cast<uint32_t>(materialIndex + 1) * 2654435761u;
    h ^= h >> 15;
    const float r = static_cast<float>((h >> 0) & 0xFF) / 255.0f;
    const float g = static_cast<float>((h >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>((h >> 16) & 0xFF) / 255.0f;
    return {Clamp01(r * 0.85f + 0.15f), Clamp01(g * 0.85f + 0.15f), Clamp01(b * 0.85f + 0.15f)};
}

// ---- JSON ----

namespace {

using json = nlohmann::json;

bool ReadFloats(const json& a, float* out, size_t n) {
    if (!a.is_array() || a.size() < n) return false;
    for (size_t i = 0; i < n; ++i) if (!a[i].is_number()) return false;
    for (size_t i = 0; i < n; ++i) out[i] = a[i].get<float>();
    return true;
}

} // namespace

std::string MaterialFileToJson(const MaterialFile& file) {
    json j;
    j["format"] = "cardbridge-materials";
    j["version"] = 1;
    if (!file.cardSha256.empty()) j["card_sha256"] = file.cardSha256;
    j["materials"] = json::array();
    for (const auto& e : file.entries) {
        json m;
        m["part"] = e.part;
        m["material"] = e.material;
        if (!e.shader.empty()) m["shader"] = e.shader;
        const MaterialOverride& o = e.ov;
        if (o.class_) m["class"] = ClassName(*o.class_);
        if (o.baseColor) m["base_color"] = {o.baseColor->x, o.baseColor->y, o.baseColor->z, o.baseColor->w};
        if (o.opacity) m["opacity"] = *o.opacity;
        if (o.roughness) m["roughness"] = *o.roughness;
        if (o.metallic) m["metallic"] = *o.metallic;
        if (o.specular) m["specular"] = {o.specular->x, o.specular->y, o.specular->z};
        if (o.specularPower) m["specular_power"] = *o.specularPower;
        if (o.emissionColor) m["emission_color"] = {o.emissionColor->x, o.emissionColor->y, o.emissionColor->z};
        if (o.emissionIntensity) m["emission_intensity"] = *o.emissionIntensity;
        if (o.normalIntensity) m["normal_intensity"] = *o.normalIntensity;
        if (o.anisotropic) m["anisotropic"] = *o.anisotropic;
        if (o.transmission) m["transmission"] = {{"strength", o.transmission->strength}, {"color", {o.transmission->color.x, o.transmission->color.y, o.transmission->color.z}}, {"thickness", o.transmission->thickness}, {"backface", o.transmission->backface}};
        if (o.subsurface) m["subsurface"] = {{"color", {o.subsurface->color.x, o.subsurface->color.y, o.subsurface->color.z}}, {"strength", o.subsurface->strength}};
        if (o.doubleSided) m["double_sided"] = *o.doubleSided;
        if (o.displayName) m["display_name"] = *o.displayName;
        if (o.preset) m["preset"] = *o.preset;
        if (!o.textures.empty()) {
            json t = json::object();
            for (const auto& x : o.textures) {
                std::string key = x.property;
                if (key.size() > 1 && key[0] == '_') key = key.substr(1);
                json v;
                v["enabled"] = x.enabled;
                if (x.hasTexture() && !x.textureName.empty()) v["texture"] = x.textureName;
                else if (x.textureIndex >= 0) v["texture_index"] = x.textureIndex;
                t[key] = std::move(v);
            }
            m["textures"] = std::move(t);
        }
        j["materials"].push_back(std::move(m));
    }
    return j.dump(2) + "\n";
}

bool MaterialFileFromJson(const std::string& text, MaterialFile& out, std::string* error) {
    using json = nlohmann::json;
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
    if (j.value("format", std::string()) != "cardbridge-materials") return fail("not a cardbridge materials file (\"format\" is missing or different)");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() > 1) return fail("a newer materials file version than this program reads");
    MaterialFile f;
    if (j.contains("card_sha256")) {
        if (!j["card_sha256"].is_string()) return fail("\"card_sha256\" is not a string");
        f.cardSha256 = j["card_sha256"].get<std::string>();
    }
    if (j.contains("materials")) {
        if (!j["materials"].is_array()) return fail("\"materials\" is not an array");
        for (const auto& m : j["materials"]) {
            if (!m.is_object()) return fail("each material entry must be an object");
            MaterialEntry e;
            if (m.contains("part") && !m["part"].is_string()) return fail("\"part\" must be a string");
            if (m.contains("material") && !m["material"].is_string()) return fail("\"material\" must be a string");
            if (m.contains("shader") && !m["shader"].is_string()) return fail("\"shader\" must be a string");
            e.part = m.value("part", std::string());
            e.material = m.value("material", std::string());
            e.shader = m.value("shader", std::string());
            if (e.material.empty()) return fail("a material entry without \"material\"");
            MaterialOverride o;
            if (m.contains("class")) {
                if (!m["class"].is_string()) return fail("\"class\" must be a string");
                MaterialClass c;
                if (!ClassFromName(m["class"].get<std::string>(), c)) return fail("unknown material class \"" + m["class"].get<std::string>() + "\"");
                o.class_ = c;
            }
            auto vec4 = [&](const char* key, Vec4& v, bool alpha) {
                if (!m.contains(key)) return true;
                const auto& a = m[key];
                if (!a.is_array() || (alpha ? a.size() != 4 : a.size() != 3)) return false;
                if (!ReadFloats(a, reinterpret_cast<float*>(&v), alpha ? 4 : 3)) return false;
                if (!alpha) v.w = 1.0f;
                return true;
            };
            {
                Vec4 v;
                const json& a = m.contains("base_color") ? m["base_color"] : json(nullptr);
                if (m.contains("base_color")) {
                    if (!a.is_array() || (a.size() != 4 && a.size() != 3) || !ReadFloats(a, reinterpret_cast<float*>(&v), a.size())) return fail("\"base_color\" must be an array of 3 or 4 numbers");
                    if (a.size() == 3) v.w = 1.0f;
                    o.baseColor = v;
                }
                (void)vec4;
            }
            auto num = [&](const char* key, std::optional<float>& dst, const char* label) {
                if (!m.contains(key)) return true;
                if (!m[key].is_number()) { if (error) *error = std::string("\"") + label + "\" must be a number"; return false; }
                dst = m[key].get<float>();
                return true;
            };
            if (!num("opacity", o.opacity, "opacity") || !num("roughness", o.roughness, "roughness") || !num("metallic", o.metallic, "metallic") ||
                !num("specular_power", o.specularPower, "specular_power") || !num("emission_intensity", o.emissionIntensity, "emission_intensity") ||
                !num("normal_intensity", o.normalIntensity, "normal_intensity") || !num("anisotropic", o.anisotropic, "anisotropic"))
                return fail("a shading field must be a number");
            if (m.contains("specular")) {
                Vec4 v;
                if (!m["specular"].is_array() || m["specular"].size() != 3 || !ReadFloats(m["specular"], reinterpret_cast<float*>(&v), 3)) return fail("\"specular\" must be an array of 3 numbers");
                o.specular = {v.x, v.y, v.z};
            }
            if (m.contains("emission_color")) {
                Vec4 v;
                if (!m["emission_color"].is_array() || m["emission_color"].size() != 3 || !ReadFloats(m["emission_color"], reinterpret_cast<float*>(&v), 3)) return fail("\"emission_color\" must be an array of 3 numbers");
                o.emissionColor = {v.x, v.y, v.z};
            }
            if (m.contains("transmission")) {
                const auto& t = m["transmission"];
                if (!t.is_object()) return fail("\"transmission\" must be an object");
                TransmissionParams p;
                if (t.contains("strength") && (!t["strength"].is_number())) return fail("transmission \"strength\" must be a number");
                if (t.contains("thickness") && (!t["thickness"].is_number())) return fail("transmission \"thickness\" must be a number");
                if (t.contains("backface") && (!t["backface"].is_boolean())) return fail("transmission \"backface\" must be true or false");
                if (t.contains("color")) {
                    Vec4 v;
                    if (!t["color"].is_array() || t["color"].size() != 3 || !ReadFloats(t["color"], reinterpret_cast<float*>(&v), 3)) return fail("transmission \"color\" must be an array of 3 numbers");
                    p.color = {v.x, v.y, v.z};
                }
                if (t.contains("strength")) p.strength = t["strength"].get<float>();
                if (t.contains("thickness")) p.thickness = t["thickness"].get<float>();
                if (t.contains("backface")) p.backface = t["backface"].get<bool>();
                o.transmission = p;
            }
            if (m.contains("subsurface")) {
                const auto& t = m["subsurface"];
                if (!t.is_object()) return fail("\"subsurface\" must be an object");
                SubsurfaceParams p;
                if (t.contains("strength") && (!t["strength"].is_number())) return fail("subsurface \"strength\" must be a number");
                if (t.contains("color")) {
                    Vec4 v;
                    if (!t["color"].is_array() || t["color"].size() != 3 || !ReadFloats(t["color"], reinterpret_cast<float*>(&v), 3)) return fail("subsurface \"color\" must be an array of 3 numbers");
                    p.color = {v.x, v.y, v.z};
                }
                if (t.contains("strength")) p.strength = t["strength"].get<float>();
                o.subsurface = p;
            }
            if (m.contains("double_sided")) {
                if (!m["double_sided"].is_boolean()) return fail("\"double_sided\" must be true or false");
                o.doubleSided = m["double_sided"].get<bool>();
            }
            if (m.contains("display_name")) {
                if (!m["display_name"].is_string()) return fail("\"display_name\" must be a string");
                o.displayName = m["display_name"].get<std::string>();
            }
            if (m.contains("preset")) {
                if (!m["preset"].is_string()) return fail("\"preset\" must be a string");
                o.preset = m["preset"].get<std::string>();
                // A preset name is metadata; the class field (if any) is authoritative.
            }
            if (m.contains("textures")) {
                const auto& t = m["textures"];
                if (!t.is_object()) return fail("\"textures\" must be an object of property -> {enabled, texture}");
                for (const auto& [key, v] : t.items()) {
                    if (!v.is_object()) return fail("texture \"" + key + "\" must be an object");
                    TexOverride x;
                    x.property = "_" + key;
                    if (v.contains("enabled")) {
                        if (!v["enabled"].is_boolean()) return fail("texture \"" + key + "\" \"enabled\" must be true or false");
                        x.enabled = v["enabled"].get<bool>();
                    }
                    if (v.contains("texture")) {
                        if (!v["texture"].is_string()) return fail("texture \"" + key + "\" \"texture\" must be a string");
                        x.textureName = v["texture"].get<std::string>();
                    }
                    if (v.contains("texture_index")) {
                        if (!v["texture_index"].is_number_integer()) return fail("texture \"" + key + "\" \"texture_index\" must be an integer");
                        x.textureIndex = static_cast<int>(v["texture_index"].get<int>());
                    } else x.textureIndex = -2;
                    o.textures.push_back(std::move(x));
                }
            }
            e.ov = std::move(o);
            f.entries.push_back(std::move(e));
        }
    }
    out = std::move(f);
    return true;
}

MaterialFile MakeMaterialFile(const assemble::Scene& scene, const WorkbenchState& st, const std::string& cardSha256) {
    MaterialFile f;
    f.cardSha256 = cardSha256;
    for (size_t i = 0; i < st.overrides.size() && i < st.originals.size(); ++i) {
        if (!st.overrides[i] || st.overrides[i]->empty()) continue;
        MaterialEntry e;
        e.part = st.originals[i].part;
        e.material = st.originals[i].mat.name;
        e.shader = st.originals[i].mat.shader;
        e.ov = *st.overrides[i];
        // Persist texture names (indices are scene-local).
        for (auto& t : e.ov.textures) {
            if (t.textureIndex >= 0 && static_cast<size_t>(t.textureIndex) < scene.textures.size())
                t.textureName = scene.textures[static_cast<size_t>(t.textureIndex)].name;
        }
        f.entries.push_back(std::move(e));
    }
    return f;
}

int ApplyMaterialFile(assemble::Scene& scene, WorkbenchState& st, const MaterialFile& file, std::vector<std::string>* unknown) {
    if (st.originals.size() != scene.materials.size()) CaptureOriginals(st, scene);
    if (st.overrides.size() != st.originals.size()) st.overrides.resize(st.originals.size());
    int applied = 0;
    bool anyChange = false;
    for (const auto& e : file.entries) {
        const int idx = FindMaterialIndex(scene, e.part, e.material);
        if (idx < 0) {
            if (unknown) unknown->push_back(e.part.empty() ? e.material : (e.part + "/" + e.material));
            scene.warn(e.part, "material workbench: material '" + e.material + "' (part '" + e.part + "') not in this scene; skipped");
            continue;
        }
        // Merge into any existing override (later entries win per-field is
        // overkill: a file has one entry per material by construction).
        MaterialOverride ov = e.ov;
        // Resolve texture names to indices now (they are scene-local).
        for (auto& t : ov.textures) {
            if (t.textureIndex < 0 && !t.textureName.empty()) {
                for (size_t k = 0; k < scene.textures.size(); ++k)
                    if (scene.textures[k].name == t.textureName) { t.textureIndex = static_cast<int>(k); break; }
                if (t.textureIndex < 0 && t.enabled) {
                    scene.warn(e.part, "material workbench: texture '" + t.textureName + "' for '" + e.material + "' not in the scene; kept original");
                    continue;
                }
            }
        }
        st.overrides[static_cast<size_t>(idx)] = std::move(ov);
        anyChange = true;
        ++applied;
    }
    (void)anyChange;
    ApplyOverrides(scene, st);
    return applied;
}

} // namespace cardbridge::material
