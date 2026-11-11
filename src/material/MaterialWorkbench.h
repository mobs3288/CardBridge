#pragma once

#include "assemble/Scene.h"

#include <optional>
#include <string>
#include <vector>

namespace cardbridge::material {

// The Material Workbench core. UI-free, like assemble::Pose: the viewer and the
// CLI are shells over it, and the tests exercise it without a window.
//
// Three tiers (docs/MATERIAL_EDITOR.md section 3):
//   Original   - an immutable snapshot of assemble::SceneMaterial taken once at
//                load (CaptureOriginals). Never mutated.
//   Interpreted - the Workbench's understanding of "what is this material?"
//                (EffectiveOf): classification, base colour, roughness, ... Never
//                written to the scene directly.
//   Override   - sparse user edits (MaterialOverride). Effective =
//                Original + Override, applied to the live scene by
//                ApplyOverrides. Reset = erase. Undo = snapshot stack.

// What CardBridge understands a material to be. Independent of the Koikatsu
// shader name; the auto-classifier (ClassifyMaterial) only guesses it.
enum class MaterialClass { Skin, Fabric, Hair, Eyes, Eyelashes, Metal, Plastic, Leather, Glass, Rubber, Generic };

// Whether the forward pixel program provably reads the _Color constant-buffer
// parameter: 1 = reads it, 0 = provably does not, -1 = unknown (the shader is
// missing or unparseable). A dead _Color declaration that no instruction reads
// is treated as "does not read" - main_opaque / main_alpha / main_skin bake
// their colour into _MainTex and ignore _Color entirely.
int ShaderReadsColor(const assemble::Scene& scene, int shaderIndex);

const char* ClassName(MaterialClass c);
bool ClassFromName(const std::string& s, MaterialClass& out);
// Best-effort guess from the shader and material names, using the same
// substrings the exporter already trusts (Exporter.cpp: HasCutout,
// HasInnerLayer, MmdMaterialLook). Metal/Plastic/Leather/Rubber are never
// guessed: there is no reliable substring, so they stay Generic until the
// user classifies them. Always user-overridable.
MaterialClass ClassifyMaterial(const std::string& shaderName, const std::string& materialName);

struct TransmissionParams {
    float strength = 0.0f;            // 0..1 wrap-light amount (preview-only)
    Vec3 color{1, 1, 1};             // transmission tint (preview-only)
    float thickness = 1.0f;           // 0 thin .. 2 thick; exp(-thickness) attenuation
    bool backface = true;             // wrap on back faces too (preview-only)
};

struct SubsurfaceParams {
    Vec3 color{1, 0.7f, 0.6f};       // subsurface tint (preview-only)
    float strength = 0.0f;           // 0..1 (preview-only)
};

// One texture slot of a material, e.g. "_MainTex". JSON uses the name without
// the leading underscore ("MainTex"); the API uses the SceneTexBinding name.
struct TexOverride {
    std::string property;          // "_MainTex", "_NormalMap", ...
    bool enabled = true;
    // Texture binding: -2 = keep the original, -1 = disable, >= 0 = bind this
    // scene-texture index. textureName is the stable name used in JSON (the
    // index is only meaningful for the scene that created it).
    int textureIndex = -2;
    std::string textureName;
    bool hasTexture() const { return textureIndex >= 0 || !textureName.empty(); }
};

// Sparse user edits for one material. Only set fields are kept, serialised and
// applied. Colour is never overwritten by a preset (see PresetForClass).
struct MaterialOverride {
    std::optional<MaterialClass> class_;
    std::optional<Vec4> baseColor;        // -> SceneMaterial::diffuse + _Color
    std::optional<float> opacity;         // -> diffuse.w (+ _Color.w)
    std::optional<float> roughness;       // 0 smooth .. 1 rough; -> _SpecularPower inverse (approximation)
    std::optional<float> metallic;        // 0..1; preview-only except a best-effort _Metallic write
    std::optional<Vec3> specular;         // specular colour -> _SpecularColor
    std::optional<float> specularPower;   // highlight strength 0..1 -> _SpecularPower
    std::optional<Vec3> emissionColor;
    std::optional<float> emissionIntensity;
    std::optional<float> normalIntensity; // preview-only
    std::optional<float> anisotropic;     // hair highlight approximation; preview-only
    std::optional<TransmissionParams> transmission;  // fabric; preview-only
    std::optional<SubsurfaceParams> subsurface;      // skin; preview-only
    std::optional<bool> doubleSided;
    std::optional<std::string> displayName;  // shown in place of the prefab name
    std::optional<std::string> preset;       // e.g. "Fabric/Default"
    std::vector<TexOverride> textures;
    bool empty() const;
};

// The interpreted view of one material (Original + Override).
struct EffectiveMaterial {
    MaterialClass cls = MaterialClass::Generic;
    Vec4 baseColor{1, 1, 1, 1};
    float opacity = 1.0f;
    float roughness = 0.7f;
    float metallic = 0.0f;
    Vec3 specular{0, 0, 0};
    float specularPower = 2.0f;       // highlight strength 0..1 (PMX exponent is strength * 5)
    Vec3 emissionColor{0, 0, 0};
    float emissionIntensity = 0.0f;
    float normalIntensity = 1.0f;
    float anisotropic = 0.0f;
    TransmissionParams transmission;
    SubsurfaceParams subsurface;
    bool doubleSided = false;
    std::string displayName;
};

// One material's snapshot at load.
struct OriginalMaterial {
    assemble::SceneMaterial mat;
    MaterialClass autoClass = MaterialClass::Generic;
    std::string part;               // first part that uses this material ("" if none found)
};

struct WorkbenchState {
    std::vector<OriginalMaterial> originals;
    std::vector<std::optional<MaterialOverride>> overrides;   // parallel to originals
    // Undo/redo: one entry per gesture (drag start -> drag end is one step,
    // mirroring the pose stacks in viewer/main.cpp). Capped at 200 like poses.
    std::vector<std::vector<std::optional<MaterialOverride>>> undoStack;
    std::vector<std::vector<std::optional<MaterialOverride>>> redoStack;

    // Base colour for a shader that ignores _Color is baked into a copy of the
    // material's main texture (main_skin / main_opaque / main_alpha in the
    // game do not read _Color at all; their create jobs painted the card's
    // colour into the texture). tintTextures[i] is the scene texture that holds
    // material i's copy (-1 before the first edit), tintCache skips rebuilding
    // it while the ratio and source pixels are unchanged, and
    // changedTextures lists the texture indices whose pixels ApplyOverrides
    // rewrote (the viewer invalidates its mip chain and re-uploads those).
    struct TintCache {
        int source = -1;              // scene texture the copy was made from
        Vec4 ratio{1, 1, 1, 1};       // colour multiplier that produced it
        uint64_t sourceHash = 0;      // source pixelHash at the time
    };
    std::vector<int> tintTextures;
    std::vector<TintCache> tintCache;
    std::vector<int> changedTextures;
    // Per Scene::shaders: 0 unknown, 1 the forward pixel program reads _Color,
    // 2 it provably ignores it, 3 it could not be analysed. Filled lazily.
    std::vector<uint8_t> readsColor;

    bool empty() const { return originals.empty(); }
    bool IsDirty() const;                 // any override set anywhere
    size_t OverrideCount() const;         // materials with a non-empty override
};

// Snapshot the scene's materials as originals. Must be called once after
// assembly (before any override is applied). Clears overrides and undo/redo
// when the material count changes; otherwise keeps them.
void CaptureOriginals(WorkbenchState& st, const assemble::Scene& scene);

// The interpreted view of one material (requires CaptureOriginals first).
EffectiveMaterial EffectiveOf(const WorkbenchState& st, size_t index);
// Per-material effective view using an explicit original (for tests).
EffectiveMaterial EffectiveOf(const OriginalMaterial& orig, const MaterialOverride* ov);

// Writes Original + Override into the live scene. For every material the
// scene copy is restored from the original first, then only overridden
// fields are applied, so clearing a field really reverts it. Texture blocks
// are handled by ApplyTexOverrides (re-binding by index, or by name when the
// index is stale, with a scene.warn on miss).
//
// Base colour is routed by what the forward pixel shader actually reads:
//   - a shader that reads _Color gets the cheap same-frame property write;
//   - a shader that provably ignores _Color (the game's main_skin,
//     main_opaque and main_alpha bake their colour into _MainTex) gets a tinted
//     copy of the main texture, bound as _MainTex, and white diffuse so the
//     tint is not applied twice. The copy is reused and only rebuilt when the
//     colour ratio or the source pixels change; its index is reported in
//     WorkbenchState::changedTextures so callers can invalidate mip chains and
//     re-upload it to the GPU.
// No geometry is rebuilt and no shader is recompiled.
void ApplyOverrides(assemble::Scene& scene, WorkbenchState& st);

// Multiplies a texture's RGBA by `ratio` (clamped to 0..255, alpha included)
// and recomputes pixelHash. The tint-copy path uses it; tests exercise it
// directly so the colour maths is covered without needing a compiled shader.
assemble::SceneTexture MakeTintedTexture(assemble::SceneTexture src, const Vec4& ratio);

// Colour picker maths (UI-free so tests can cover it, viewer does the drawing).
// HSV is a Vec3{h, s, v} with h in [0,1) (0 = red, 1/3 = green, 2/3 = blue),
// s and v in [0,1]. H wraps: h == 1 and h == 0 are the same hue.
Vec3 RgbToHsv(const Vec3& rgb);
Vec3 HsvToRgb(float h, float s, float v);

// Emits one scene.warn per unsupported-in-PMX field that is set (metallic,
// roughness, emission, normal intensity, transmission, subsurface,
// anisotropic, classification), so a dropped field is never silent. The
// messages also land in the manifest issues via BuildManifest.
void ReportUnsupported(assemble::Scene& scene, const WorkbenchState& st);
// Names of fields in `ov` that PMX cannot carry (same set as ReportUnsupported).
std::vector<std::string> UnsupportedFields(const MaterialOverride& ov);

// Undo/redo. PushUndo snapshots the overrides before a mutation (one gesture).
void PushUndo(WorkbenchState& st);
bool Undo(WorkbenchState& st);
bool Redo(WorkbenchState& st);
inline bool CanUndo(const WorkbenchState& st) { return !st.undoStack.empty(); }
inline bool CanRedo(const WorkbenchState& st) { return !st.redoStack.empty(); }

// Reset helpers (all push an undo step when they change anything).
void ResetMaterial(WorkbenchState& st, size_t index);
void ResetAll(WorkbenchState& st);
enum class ResetParam { BaseColor, Opacity, Roughness, Metallic, Specular, Emission, NormalIntensity, Transmission, Subsurface, Textures, Class, All };
const char* ResetParamName(ResetParam p);
void ResetParameter(WorkbenchState& st, size_t index, ResetParam p);

// Copy/paste and multi-apply (paste pushes one undo step).
MaterialOverride CopyOverride(const WorkbenchState& st, size_t index);
void PasteOverride(WorkbenchState& st, size_t index, const MaterialOverride& ov);
void PasteToMultiple(WorkbenchState& st, const std::vector<size_t>& targets, const MaterialOverride& ov);
void SetOverride(WorkbenchState& st, size_t index, MaterialOverride ov);   // pushes undo if changed

// Apply a class preset: sets the classification and the class's standard
// shading parameters. Colour is left alone. Returns the filled override.
MaterialOverride PresetForClass(MaterialClass cls);
std::string PresetName(MaterialClass cls);
void ApplyPreset(WorkbenchState& st, size_t index, MaterialClass cls);   // pushes undo

// Shared-instance helpers: editing material N affects every submesh that
// names it. These report that; nothing here merges or splits materials.
int CountSubmeshesForMaterial(const assemble::Scene& scene, int materialIndex);
std::string PartForMaterial(const assemble::Scene& scene, int materialIndex);
int FindMaterialIndex(const assemble::Scene& scene, const std::string& partId, const std::string& materialName);

// Texture helpers operating directly on the state + scene. Enable/disable
// writes texProps[slot].texture = -1 (the sampler then falls back to the
// shader's default). RebindTexture binds an existing scene texture.
// ReplaceTexture appends new pixels as a new scene texture and binds it,
// recording the binding in the override (texture index + name). All push an
// undo step and then ApplyOverrides.
bool SetTextureEnabled(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, bool enabled);
bool RebindTexture(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, int textureIndex);
int ReplaceTexture(assemble::Scene& scene, WorkbenchState& st, size_t index, const std::string& property, assemble::SceneTexture texture);
// Deterministic material-ID colour for diagnostics (Stage 4), stable across runs.
Vec3 MaterialIdColor(int materialIndex);

// ---- cardbridge-materials JSON (format "cardbridge-materials", version 1) --
// Sparse, keyed by part + material name (stable across reassembly). Unknown
// fields are ignored; a newer version is rejected with `error`. Unknown
// materials are *not* a parse error: ApplyMaterialFile warns and skips them.
struct MaterialEntry {
    std::string part;
    std::string material;      // SceneMaterial name
    std::string shader;        // informational (the imported shader name)
    MaterialOverride ov;
};
struct MaterialFile {
    std::string cardSha256;               // optional binding; warned, not enforced
    std::vector<MaterialEntry> entries;
};
std::string MaterialFileToJson(const MaterialFile& file);
bool MaterialFileFromJson(const std::string& text, MaterialFile& out, std::string* error = nullptr);
// Builds a file from the current overrides (sparse: only non-empty overrides).
MaterialFile MakeMaterialFile(const assemble::Scene& scene, const WorkbenchState& st, const std::string& cardSha256 = {});
// Applies a file's entries to the state + scene. Returns the number applied.
// Unknown entries are pushed into `unknown` (when given) and (via scene.warn)
// reported once each; unsupported fields are reported by ReportUnsupported.
int ApplyMaterialFile(assemble::Scene& scene, WorkbenchState& st, const MaterialFile& file, std::vector<std::string>* unknown = nullptr);

} // namespace cardbridge::material
