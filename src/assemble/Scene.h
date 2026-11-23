#pragma once

#include "core/Math.h"
#include "assemble/FaceFeatures.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::assemble {

// The assembled character, in Unity units, Y up, left-handed (the same handedness
// as PMX - no mirroring needed, only scale). Plain data: the exporter and the
// future engine importer both read this, nothing in it knows about PMX or Vulkan.

struct SceneBone {
    std::string name;
    int parent = -1;
    Vec3 pos;                 // rest position, model space
    std::string source;       // "rig" (shared skeleton) or the part that added it
    Mat4 world;               // rest world matrix (position, rotation and scale as the game had it); what a re-shape moves the vertices from
};

struct SceneVertex {
    Vec3 pos, normal;
    Vec2 uv;                  // Unity convention (v up); the PMX writer flips v
    Vec2 uv1;                 // the second UV set (TEXCOORD1: the body's nipple placement); equals uv when the mesh has none
    Vec2 uv2;                 // the third UV set (TEXCOORD2: the body's underhair placement); equals uv when the mesh has none
    Vec2 uv3;                 // the fourth UV set (TEXCOORD3: the face's eye-shadow placement); equals uv when the mesh has none
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    // The vertex and its normal in the frame of each bone that moves it (pos = sum of weight * bone world * local), so new bone worlds
    // from a changed body shape give the new vertex without assembling again (assemble::Reshape). Valid when SceneMesh::reshapable.
    Vec3 local[4], nlocal[4];
};

struct SceneTexture {
    std::string name;
    std::string source;       // bundle it came from (provenance)
    std::string format;       // the decoded format string it came from ("RGBA32", "DXT5", "PNG (replaced)", "RGBA8 (tint copy)"); empty = unknown
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;   // top row first
    bool clamp = false;          // the texture's wrap mode is clamp (ramps), not repeat
    bool srgb = true;            // the texture is flagged sRGB (colour data); false for masks and other linear data
    uint64_t pixelHash = 0;      // FNV-1a 64 of rgba
    bool bakedInShaderBake = false;   // true when the shader bake wrote this texture (provenance)
    // The game's LUT grade has been applied to this texture's RGB. Single source of truth for "graded": both the viewer's
    // as-exported copy and the PMX writer consult it, so a colour texture is graded exactly once whichever path reaches it
    // (a shader bake, a create job, a KKS shader the bake cannot run, or a card texture shipped straight from the bundle).
    bool colourGraded = false;
};

// True when a texture has no authored transparency at all (every alpha byte is 255). An "alpha"
// shader over such a texture is an alpha-CUTOUT in the game (it discards below _Cutoff but draws
// the remaining fragments with depth writes), not a translucent surface: baking it must not
// invent alpha holes, and the PMX must keep it opaque so MMD writes depth for it too.
inline bool TextureIsFullyOpaque(const SceneTexture& t) {
    if (t.width <= 0 || t.height <= 0 || t.rgba.size() < static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4) return false;
    for (size_t i = 3; i < t.rgba.size(); i += 4)
        if (t.rgba[i] != 255) return false;
    return true;
}

// True when a shader bake came back as a solid black plate over the texels its mesh covered, while the
// authored texture it sampled carried real colour. That means the shader failed to reproduce the material
// (typically it multiplies the source by a colour constant the material does not declare, so it reads zero):
// baking it would replace the card's texture with black — a hair accessory that should be red turns into a
// black plate. Callers must keep the authored texture instead, like an empty bake. `cov` marks covered texels;
// `img` is the bake's RGBA float buffer.
inline bool BakeLostColour(const std::vector<float>& img, const std::vector<uint8_t>& cov, const SceneTexture& src) {
    const size_t n = src.rgba.size() / 4;
    if (n == 0 || cov.empty() || img.size() < cov.size() * 4) return false;
    double bakeLum = 0.0;
    size_t bakedTexels = 0;
    for (size_t i = 0; i < cov.size(); ++i)
        if (cov[i] && img[i * 4 + 3] >= 0.5f) {
            bakeLum += (static_cast<double>(img[i * 4]) + img[i * 4 + 1] + img[i * 4 + 2]) / 3.0;
            ++bakedTexels;
        }
    if (bakedTexels < 16) return false;
    double srcLum = 0.0;
    for (size_t i = 0; i < n; ++i)
        srcLum += (static_cast<double>(src.rgba[i * 4]) + src.rgba[i * 4 + 1] + src.rgba[i * 4 + 2]) / (3.0 * 255.0);
    return bakeLum / bakedTexels < 0.02 && srcLum / n > 0.05;
}

struct SceneTexBinding {
    std::string name;            // Unity property, e.g. "_MainTex"
    int texture = -1;            // index into Scene::textures
    Vec4 st{1, 1, 0, 0};         // scale.xy, offset.xy
};

constexpr int kToonRows = 32;   // rows of SceneMaterial::toon

struct SceneMaterial {
    std::string name;
    std::string shader;
    int texture = -1;
    Vec4 diffuse{1, 1, 1, 1};
    bool doubleSided = false;
    bool alpha = false;
    int renderQueue = -1;         // m_CustomRenderQueue, -1 = the shader's own queue
    // The game's toon look, from the Shader Forge material: the colour a surface turns in shade, its highlight and its outline.
    Vec3 shadow{0.5f, 0.5f, 0.5f};           // _ShadowColor (per-channel multiplier of the lit colour)
    Vec3 specular{0, 0, 0};                  // _SpecularColor scaled by its power
    float specularPower = 5.0f;
    Vec3 outlineColor{0, 0, 0};              // _LineColor
    float outlineWidth = 0.0f;               // _LineWidthS (0 = no outline)
    // Everything the game's shader reads: the material's colours and floats (floats in .x) and all its textures, plus
    // the compiled shader (index into Scene::shaders, -1 when the shader is not in the bundle).
    std::vector<std::pair<std::string, Vec4>> props;
    std::vector<SceneTexBinding> texProps;
    int shaderIndex = -1;
    int readsColor = -1;              // whether the forward pixel program reads _Color (1 reads, 0 does not, -1 unknown)
    bool translucent = false;          // an alpha/translucency shader (the card's IBL_Shader_alpha and friends): the material keeps the texture's alpha and blends over what is behind it
    // The game shader's shade as a function of N.L, measured by BakeShaderTextures: kToonRows rows from N.L = 1 (top, fully lit) to
    // -1 (bottom), each the colour factor the shader gives that light angle relative to the baked front light. Empty = none.
    std::vector<Vec3> toon;
    bool bakedLit = false;                   // the texture already holds the shader's lighting: write the material unlit
    // The game's shader discards every fragment it draws (e.g. a crest painted with a black fallback texture and no binding of its
    // own): the game shows nothing for it, so the views and the exported model skip it too instead of drawing a flat white plate.
    bool hidden = false;
    bool cardAreola = false;                 // its _nipsize is the card's areola size, not the material's own default (a re-shape follows the slider)
    bool cardSkin = false;                   // the body's skin material (cf_m_body): _overcolor1 tints the nipple / areola overlay
    Vec4 cardNippleColour;                   // the card's own _overcolor1 (restored when the automatic pink tint is off)
    bool hairTinted = false;                 // _Color holds the card's measured hair tint (sRGB): the standard views linearise it like any colour, the bake feeds it raw like every other colour (see BakeShaderTextures)
    // A shader whose name belongs to the "opaque" family can still be an alpha test (queue 2450, _Cutoff) and discard
    // its texture's low-alpha texels: the game shows the body or the layer underneath there. BakeShaderTextures proves
    // this from the shader's own output; viewers and the PMX writer then honour the holes instead of painting them black.
    bool shaderClipsAlpha = false;
};

// True when a shader bake produced a real alpha cutout over the texels the material's mesh covers. A shader that ignores
// alpha leaves every covered texel solid; only a shader that actually clips produces a substantial low-alpha area. Stray
// atlas padding must not count, hence the fraction and minimum.
inline bool BakeHasAlphaCutout(const std::vector<float>& img, const std::vector<uint8_t>& cov) {
    size_t covered = 0, clipped = 0;
    for (size_t i = 0; i < cov.size(); ++i)
        if (cov[i]) {
            ++covered;
            if (img.size() > i * 4 + 3 && img[i * 4 + 3] < 0.5f) ++clipped;
        }
    return covered >= 32 && clipped * 20 >= covered;   // at least 5% of the covered texels are clipped
}

// True when the game draws this material with the texture's alpha ignored: the
// "opaque" shader family — Shader Forge/main_opaque, Koikano/main_clothes_opaque,
// Koikano/low_clothes_opaque — is the default queue and reads no alpha test. Some
// garment textures still carry stray low-alpha texels (the game draws them anyway),
// so callers that decide "transparent" (exporter, software and GPU viewers) must not
// mark these materials transparent — that turns intact garments into look-throughs.
inline bool IsOpaqueShaded(const SceneMaterial& mat) {
    return mat.shader.find("opaque") != std::string::npos;
}


// A texture the game builds at run time by drawing one full-screen quad through a "create" material's shader (eye iris and
// white, face and body skin: Graphics.Blit into a render texture). `material` carries the shader, colours and input textures;
// the result is written into Scene::textures[texture] (a placeholder until then) by export::RunCreateJobs.
struct SceneCreateJob {
    SceneMaterial material;
    int texture = -1;            // index into Scene::textures that receives the result
    int width = 512, height = 512;
    // An image laid over the result (a KoiSkinOverlayX layer from the card), source-over, scaled to the target; empty = none.
    std::vector<uint8_t> overlay;
    int overlayWidth = 0, overlayHeight = 0;
    // The game runs the draw in linear light: sRGB-flagged textures are decoded when sampled, the colour properties are linear
    // values and the result is encoded when it is written (clothes). Otherwise everything is combined as stored.
    bool linear = false;
    bool overlayReplaces = false;   // the overlay replaces the result instead of covering it (KoiClothesOverlayX "override")
};

struct SceneShader {
    std::string name;            // e.g. "Shader Forge/main_opaque"
    std::vector<uint8_t> blob;   // decompressed Direct3D 11 program blob
    std::vector<std::pair<std::string, Vec4>> propDefaults;   // colour, vector and float defaults the shader declares
    std::vector<std::pair<std::string, std::string>> texDefaults;   // texture property -> default ("white", "black", "bump", "gray")
    int cull = -1;                // the forward pass's face culling: 0 off (both sides drawn), 1 front, 2 back; -1 not known
    std::string cullProperty;     // the material float that sets the culling instead, when the shader reads it from one
    bool zWrite = true;           // whether the forward pass writes depth (false = the game's transparent compositing)
    int queue = 2000;             // the shader's render queue (m_Tags QUEUE)
};

struct SceneSubmesh {
    int material = 0;
    std::vector<uint32_t> indices;   // triangles, into SceneMesh::vertices
};

// A blend shape of a mesh (Unity's m_Shapes), as model-space offsets of the vertices at weight 100 (skinning applied to the delta).
// `rest` is the weight (0..100) already built into SceneMesh::vertices, the resting expression the game plays.
struct SceneBlendShape {
    std::string name;
    float rest = 0.0f;
    std::vector<std::pair<uint32_t, Vec3>> offsets;   // vertex index, offset
    // Each offset in the frame of the bones that move its vertex (as SceneVertex::local does for positions), so a re-shape that moves
    // the bones moves the offsets too (assemble::Reshape). Same length as `offsets` when the mesh is reshapable, else empty.
    std::vector<std::array<Vec3, 4>> local;
};

struct SceneMesh {
    std::string name;
    int part = -1;
    std::vector<SceneVertex> vertices;
    std::vector<SceneSubmesh> submeshes;
    std::vector<SceneBlendShape> shapes;
    bool reshapable = false;          // the vertices carry local / nlocal
};

// Automatic skin shading, computed from the body's own geometry instead of a hand-painted texture: rays cast from each
// skin vertex plus its local concavity darken the creases (cleavage, under the breasts, the navel, the armpits) in the
// body's skin texture in UV space. The result is a multiplier map the texture can be re-shaded from at any strength.
struct SceneSkinShade {
    std::vector<int> meshes;         // Scene::meshes the body's skin geometry comes from (index into Scene::meshes)
    int texture = -1;                // Scene::textures the shade was baked into (the body's create result)
    float applied = -1.0f;           // the cleavage strength currently baked into the texture (-1 = never applied)
    float appliedUnder = -1.0f;      // the underboob strength currently baked into the texture (-1 = never applied)
    float appliedTop = 1.0f;         // the cleavage top extent currently baked in (0 = absent, 1 = full height)
    float appliedWidth = 1.0f;       // the cleavage width currently baked in (1 = the default half-width)
    bool geometryDirty = true;       // the body moved since the map was built (Reshape): rebuild before re-applying
    std::vector<uint8_t> base;       // the texture before shading (same dimensions as the texture)
    std::vector<uint8_t> shade;      // cleavage factors at strength 1 (255 = lit, lower = creased); width * height
    std::vector<uint8_t> shadeU;     // underboob factors at strength 1 (255 = lit, lower = creased); width * height
    std::vector<uint8_t> covered;    // 1 where a body triangle reached the texel, parallel to `shade`
    bool ready() const { return texture >= 0 && !meshes.empty(); }
};

// Koikatsu's own skin look, baked into the body's skin texture the way the game's Shader Forge/main_skin does it:
// the always-bound base body normal map (cf_body_00_base_n) - a local curvature (cavity) factor is lifted from its
// neighbourhood contrast, and the card's body normal map blended through _DetailNormalMapScale in the game shader -
// plus the ink-line channel of the line mask the game binds to _LineMask (the card's body detailId row, category 420;
// cf_body_00_line for the default).
// Held here so the bake can be re-applied at a new strength without re-opening the bundles it came from; the maps
// are per-texel factors at strength 1 and are multiplied in after ApplySkinShade's cleavage/underboob factors,
// yielding to SkinShade's own darkening - never stacking with it.
struct SceneSkinAoBake {
    std::vector<uint8_t> baseNormal;   // decoded base body normal map (cf_body_00_base_n), RGBA32; empty when unavailable
    int baseNormalWidth = 0, baseNormalHeight = 0;
    std::vector<uint8_t> lineMask;     // decoded _LineMask for the card's body detailId (cf_body_00_line at default), RGBA32
    int lineMaskWidth = 0, lineMaskHeight = 0;
    float collarLo = -1.0f, collarHi = -1.0f;   // the row band the clavicle arcs occupy in the line mask (found from the mask itself, no fixed atlas coords); < 0 = no band
    float collarDamp = 0.35f;                    // factor applied to the ink-line multiplier inside the band
    std::vector<uint8_t> detailNormal; // decoded detailId NormallMapDetail; empty when none (e.g. the 8x8 blank at default)
    int detailNormalWidth = 0, detailNormalHeight = 0;
    std::vector<float> aoShade;        // per-texel normal-lighting factor at strength 1 (1 = lit); width * height
    std::vector<float> aoLine;         // per-texel ink-line factor at strength 1 (1 = fully line-covered); width * height
    std::vector<float> aoDamp;         // per-texel limb damp factor (1 = full AO, ~0.25 = fully limb-bound arm/leg); parallel to aoShade
    std::vector<uint8_t> covered;      // 1 where a body triangle reached the texel, parallel to aoShade
    float applied = -1.0f;             // the strength currently baked into the texture (-1 = never applied)
    bool built = false;                // aoShade / aoLine are current for the body geometry
};

// A hand-painted translucency mask on a garment part, brushed on by the user in the viewer (ray-cast onto the part's mesh,
// stamped into the garment's own texture in UV space). The bake writes the painted alpha into that texture, so the body - or
// a bra - shows through the garment in the viewer and in every PMX viewer after export. One layer per (part, material texture):
// the material of the submesh under the brush is the one that goes to the transparent pass while the bake holds it.
// The mask is session-only (never saved or serialized, like the Morphs tab's live weights): reloading the card drops it.
struct SceneTranslucency {
    struct Layer {
        int part = -1;            // Scene::parts index the layer belongs to
        int mesh = -1;            // Scene::meshes index whose UVs the mask is painted on
        int material = -1;        // Scene::materials index whose texture carries the paint
        int texture = -1;         // Scene::textures index the alpha is written into
        std::vector<uint8_t> base;     // the texture's pixels before the bake (RGBA); re-captured when the texture changes outside the bake
        std::vector<uint8_t> mask;     // per texel 0 = opaque .. 255 = fully translucent; parallel to the texture
        std::vector<uint8_t> covered;  // per texel 1 where any triangle of the part binding this texture covers it (UV space, wrap-matched to the texture); the brush's gate
        bool coveredBuilt = false;     // `covered` has been rasterized for the part
        uint64_t afterHash = 0;        // FNV-1a of the texture right after our last write (the external-change detector)
        bool passHeld = false;   // the bake currently holds the material in the transparent pass (alpha + translucent)
        bool hadAlpha = false;   // the material's flags before the bake held it (restored on revert)
        bool hadTranslucent = false;
        int cutoffProp = -1;           // index into Scene::materials[].props of the _Cutoff property, while the bake holds the material over it
        Vec4 cutoffPrior{};            // its value before the bake (restored on revert)
        bool cutoffTouched = false;    // the _Cutoff was rewritten by the bake (and must be put back)
        int shaderIndexHad = -1;       // the material's shader index before the bake swapped it to the alpha-capable twin (-1 = not swapped)
        bool shaderSwapped = false;    // the bake swapped the material's shader for the sheer-cloth one
    };
    std::vector<Layer> layers;
};

// An expression the game's FaceBlendShape component plays: eyebrow, eye or mouth pattern in its closed or open state, as the
// blend shape weights it sets on the face meshes. Exported as a vertex morph (offsets relative to the resting expression).
struct SceneExpression {
    struct Target { int mesh = -1; int shape = -1; float weight = 0.0f; };
    std::string name;
    int panel = 4;                    // MMD panel: 1 brows, 2 eyes, 3 mouth, 4 other
    std::vector<Target> targets;
    std::string shape;                // the main blend shape's own name without its mesh and face prefix ("a_l_op", "def_cl"): what the pattern is
};

// A DynamicBone chain (hair strand, skirt, ribbon, breast and hip jiggle): bones[0] is the anchor that follows the body, the
// rest hang from it; each bone's parent in Scene::bones is its predecessor. Parameters are the component's, 0..1 as the game has them.
struct SceneChain {
    std::string name, part;
    std::vector<int> bones;
    float damping = 0.1f, elasticity = 0.1f, stiffness = 0.1f, inert = 0.0f;
    // The same three per bone (parallel to `bones`), with the component's distribution curves applied along the chain (the curve is read at
    // the bone's distance from the root over the chain's length, as the component does): a ponytail with stiffness 1 near the root and 0 at
    // the tip is stiff where it is tied and free where it hangs. Empty = no curves, the chain-wide values hold.
    std::vector<float> boneDamping, boneElasticity, boneStiffness;
    float radius = 0.0f;              // collision radius in model units (0 = none given)
    bool characterOwn = false;        // the character's own Ver02 component (breast and hip jiggle: skin, not cloth or hair)
    Vec3 gravity, force;
};

// The side a bone belongs to, from the game's suffix convention (cf_s_bnip02_L, cf_s_siri_R2): +1 left, -1 right, 0 neither.
// Shared by the preview's jiggle and the PMX export so the two sides can breathe independently but stay coordinated.
inline int BoneSideSign(const std::string& name) {
    for (const char* suffix : {"_L", "_R"}) {
        size_t p = name.rfind(suffix);
        if (p == std::string::npos) continue;
        size_t q = p + 2;
        while (q < name.size() && name[q] >= '0' && name[q] <= '9') ++q;
        if (q == name.size()) return suffix[1] == 'L' ? 1 : -1;
    }
    return 0;
}

// Which of a character's own (characterOwn) jiggle chains this is: see IsBustJiggleChain below (after Scene).

// A DynamicBoneCollider the chains collide with: a sphere or capsule fixed to a body bone.
struct SceneCollider {
    std::string name;
    int bone = -1;
    Vec3 center;                      // model space
    Vec3 axis{0, 1, 0};               // capsule axis, model space (unit)
    float radius = 0.0f, height = 0.0f;   // height 0 or <= 2 radius: a sphere
    bool inside = false;              // keeps the chain inside (not exported as a body)
};

struct ScenePart {
    std::string id;           // stable: "head", "hair.back", "outfit0.top", "outfit0.acc03"
    std::string label;
    std::string group;        // Body, Head, Hair, Outfit, Accessories
    bool fromClothing = false;   // the mesh came from the added clothing file (see Card::hasClothing); viewer lists it under "Added outfit"
    // An outfit garment (including one worn through an accessory slot, see AssembleOptions): the PMX gives its cloth the garment pass,
    // no edge and no self-shadow receive, so a garment clipping the body or its see-through lining never paints black slices. Its shade
    // still comes from the material and its ground/cast shadow is kept (the silhouette belongs in the shadow).
    bool garment = false;
    std::string property;
    int category = 0;
    int64_t sourceItemId = 0;
    // Top sub-parts (created from the card's subPartsId) share a group id. An outer sub-part's OverInnerMask hides the
    // regions of the inner sub-part(s) of that same group; the relation comes from the card, never from slot numbers.
    std::string subGroup;
    std::string status;       // resolved-mod, resolved-base, missing-mod, ... (see resolve::StatusName)
    std::string detail;
    std::string modGuid, modName, modVersion, modAuthor, modPath;
    std::string bundle;       // "<abdata-relative path>" of the bundle the geometry came from
    std::string attachNode;   // accessories: the body node the item hangs on (e.g. a_n_bust); empty for garments
    bool visibleInPreview = true;
    bool includeInExport = true;
    std::vector<int> meshes;  // indices into Scene::meshes
    // The nude body. A garment's body mask hides the triangles under it (as the game does); those triangles are not thrown away but
    // kept as an "under piece" part so the skin can be shown again: a piece is hidden by default (visibleInPreview false), lists the
    // garments that cover it in coveredBy, and is shown when none of them is (or when the nude switch is on). "" in coveredBy is a
    // garment the game hides itself (it still covers). The same is done for the bra, which a top's mask cuts.
    bool underPiece = false;
    std::string underOf;                    // id of the part the piece was cut from
    std::vector<std::string> coveredBy;     // part ids of the garments over it
    bool nudeSwitch = false;                // the "Nude body" checkbox: no geometry, switches every under piece on
    // Export: the part's PMX materials start fully transparent (the viewer sets this from its checkboxes); the PMX morphs bring them back.
    bool startHidden = false;
    // Footwear: the list row's display Name (Stiletto's heelName) and the resolved heel
    // configuration (zipmod Stiletto txt, else the known table). settleHeels reads these;
    // hasHeelConfig false = geometry fallback.
    std::string listName;
    bool hasHeelConfig = false;
    float heelAnkleDeg = 0, heelLegDeg = 0, heelToeDeg = 0, heelHeightMeters = 0;
    float heelShoeScaleX = 1, heelShoeScaleY = 1, heelShoeScaleZ = 1;
    float heelShoeAngleDeg = 0, heelShoeTranslateY = 0, heelShoeTranslateZ = 0;
    float heelShoeShearY = 0, heelShoeShearZ = 0;
    std::string heelConfigSource;   // "zipmod <guid>" / "known <Name>" for the manifest
    // Set by SeparateCoincidentLayers once this part has been shifted off a duplicated layer. Keeps a part in at
    // most one pair (a card can repeat one item in several slots), so the correction is deterministic and a second
    // pass over the same scene is a no-op.
    bool layerSeparated = false;
};

// What BakePose wrote into the scene as its new rest pose, for the manifest.
struct ScenePoseEntry {
    std::string bone;
    Vec3 pos, rot, scl;      // local edit: translation, Euler degrees, scale
};

struct SceneIssue {
    std::string severity;     // "warning" or "error"
    std::string part;         // part id or "" for the whole scene
    std::string message;
};

// Facial features DETECTED from the card's own textures, independent of one another. Recorded on the scene so the
// manifest can state exactly what the source authored (and what the pipeline preserved) without guessing from ids.
struct FacialFeatureDetection {
    bool noseLine = false;
    bool nostrils = false;
    bool upperLipLine = false;
    bool lowerLipLine = false;
};

struct Scene {
    std::string name;
    std::vector<SceneBone> bones;
    std::vector<SceneTexture> textures;
    std::vector<SceneMaterial> materials;
    std::vector<SceneShader> shaders;
    std::vector<SceneCreateJob> createJobs;
    // Shader globals the game sets for every material (Shader.SetGlobalTexture / SetGlobalColor / SetGlobalFloat):
    // the shadow ramp, the ambient shadow colour, the outline colour and width.
    std::vector<SceneTexBinding> globalTextures;
    std::vector<std::pair<std::string, Vec4>> globalProps;
    std::vector<SceneMesh> meshes;
    SceneSkinShade skinShade;
    SceneSkinAoBake aoBake;
    SceneTranslucency translucency;
    std::vector<ScenePart> parts;
    std::vector<SceneExpression> expressions;
    std::vector<SceneChain> chains;
    std::vector<SceneCollider> colliders;
    std::vector<ScenePoseEntry> pose;   // the bone edits baked into this scene (see BakePose), empty for the rest pose
    FacialFeatureDetection facialFeatures;
    NoseRidge noseRidge;   // per-card presentation setting for the head mod's centre nose shading
    bool aposeApplied = false;          // BakePose included the automatic A-pose
    bool chainsSettled = false;         // an item's DynamicBone chains were run to rest (CARDBRIDGE_DYNBONE): they depend on the body's shape, so Reshape cannot redo them
    std::vector<SceneIssue> issues;

    int boneIndex(const std::string& name) const {
        for (size_t i = 0; i < bones.size(); ++i)
            if (bones[i].name == name) return static_cast<int>(i);
        return -1;
    }
    void warn(const std::string& part, const std::string& msg) { issues.push_back({"warning", part, msg}); }
    void error(const std::string& part, const std::string& msg) { issues.push_back({"error", part, msg}); }
    void note(const std::string& part, const std::string& msg) { issues.push_back({"info", part, msg}); }
};

// The effective Unity render queue: the material's own override, else its shader's, else Geometry (2000).
// Transparent and alpha-test queues start at 2450; below that the game draws the surface in its opaque
// pass. Shared by the material-semantics helpers here and by classification in the exporter, so the
// same material is read the same way on every path. Defined after Scene (it needs scene.shaders).
inline int EffectiveRenderQueue(const Scene& scene, const SceneMaterial& mat) {
    if (mat.renderQueue >= 0) return mat.renderQueue;
    if (mat.shaderIndex >= 0 && static_cast<size_t>(mat.shaderIndex) < scene.shaders.size())
        return scene.shaders[static_cast<size_t>(mat.shaderIndex)].queue;
    return 2000;
}

// True when the game draws this material with texture alpha ignored, including the character's own skin.
//
// Besides the "opaque" shader family above, KK and KKS build the body and face skin at run time (a create-job whose
// alpha channel is a coverage mask for the clothes, not transparency) and draw it opaque: KK's face uses the
// main_skin shader family, the body's create-job shader is often absent from the library altogether, and KKS uses
// the same arrangement. Treating those alpha holes as a cutout sends the skin to MMD's blended pass, where it loses
// depth writes and the back of the head shows through the face. Identify skin structurally - shader family, or a
// shader-less create-job material used by the character's body/head part - never by card, texture or material index.
inline bool IgnoresTextureAlpha(const Scene& scene, const SceneMaterial& mat) {
    if (mat.shaderClipsAlpha) return false;   // the bake proved this shader clips: its low-alpha texels are real holes
    if (IsOpaqueShaded(mat) || mat.shader.find("skin") != std::string::npos) return true;
    // A material with no authored alpha flag, no translucency and an opaque render queue is drawn by
    // the game without reading texture alpha at all — whatever the UV-flat bake left in the atlas. Hair
    // is the common case: a baked strand sheet contains alpha-0 padding between its islands and jagged
    // strand tips, neither of which the game treats as a hole in the surface. Using that padding as
    // proof of an alpha cutout sent the whole rear hair to the blended (no-depth-write) pass. This rule
    // is structural (the material's own metadata), not a name or card exception: a material that wants
    // alpha testing declares it with mat.alpha and/or a transparent/alpha-test queue.
    if (!mat.alpha && !mat.translucent && EffectiveRenderQueue(scene, mat) < 2450) return true;
    if (!mat.shader.empty() || mat.shaderIndex >= 0 || scene.materials.empty()) return false;
    if (&mat < &scene.materials.front() || &mat > &scene.materials.back()) return false;
    const int mi = static_cast<int>(&mat - &scene.materials.front());
    for (const auto& part : scene.parts) {
        if (part.id != "body" && part.id != "head" && part.group != "Body" && part.group != "Head") continue;
        for (int meshIdx : part.meshes) {
            if (meshIdx < 0 || static_cast<size_t>(meshIdx) >= scene.meshes.size()) continue;
            for (const auto& sm : scene.meshes[static_cast<size_t>(meshIdx)].submeshes)
                if (sm.material == mi) return true;
        }
    }
    return false;
}

// A chain is hair when its part is the character's hair slot, or its item hangs on one of the head's attach sockets (the list row's
// attach node: a_n_head, a_n_headside, a_n_headtop, a_n_hair*, ...): imported custom-skeleton hair (the card's back / front / ponytail
// accessories) and the base game's own hair items. Garments, skirts and the body's own jiggle are not hair - they want the garment
// treatment (their authored drape falls under MMD's world gravity), while hair keeps its authored pose (the hair components carry
// no gravity in the game, so its silhouette IS the intended resting shape).
inline bool SceneChainIsHair(const Scene& scene, const SceneChain& ch) {
    if (ch.characterOwn) return false;   // the body's own jiggle (breast / hip) is not hair
    const std::string& partId = ch.part;
    for (const auto& p : scene.parts) {
        if (p.id != partId) continue;
        if (p.group == "Hair") return true;
        auto has = [&](const char* s) { return p.attachNode.rfind(s, 0) == 0; };
        return has("a_n_head") || has("a_n_hair");
    }
    // No part row (or an empty part id, as in synthetic test scenes): not hair.
    return false;
}

// Which of a character's own (characterOwn) jiggle chains this is: the breast chains carry the bnip / bust bones, the hip (butt) chains
// the siri / kosi ones. Anything else of the body's own is left to the butt switch's side.
inline bool IsBustJiggleChain(const Scene& scene, const SceneChain& ch) {
    for (int b : ch.bones) {
        if (b < 0 || static_cast<size_t>(b) >= scene.bones.size()) continue;
        const std::string& n = scene.bones[static_cast<size_t>(b)].name;
        if (n.find("bnip") != std::string::npos || n.find("bust") != std::string::npos) return true;
    }
    return false;
}

} // namespace cardbridge::assemble
