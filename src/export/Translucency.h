#pragma once

// Hand-painted translucency for clothing parts.
//
// Koikatsu's see-through clothes (a thin shirt over a bra) are an authored material: the game draws the garment in its
// transparent queue with the texture's alpha. The converted PMX cannot rely on the same shader, so this module bakes the
// effect into the garment's own texture instead, exactly the way the skin shading is baked into the body's:
//
//  - the viewer's translucency brush ray-casts the pointer onto the garment's mesh (posed, like the picture) and stamps
//    a disc into a per-texel mask of the garment's texture in UV space. Every stamp is gated by the mesh's own UV islands
//    (the union of its triangles), so painting one chart never bleeds into an unrelated island;
//  - the bake then writes the mask into the texture's alpha channel (unpainted texels keep their base alpha, painted ones
//    fade toward a strength-controlled floor) and moves the material into the transparent pass (alpha + translucent, restored
//    when the bake reverts). The texture is skipped by BakeShaderTextures like any authored translucency material, so the
//    painted alpha survives the export; MMD draws it in its transparent pass, and the skin (an under piece, kept visible
//    for exactly this reason in EffectiveHiddenParts) or a bra shows through;
//  - toggling the brush (or lowering the strength to 0) restores the base pixels and the material's original pass flags:
//    the card's texture is never destroyed. The mask is session-only, like the Morphs tab's live weights.
//
// One layer is kept per (part, material texture): the submesh under the brush is the one painted. The PMX carries the result
// with no CardBridge-specific rendering, like every other bake in this project.

#include "assemble/Scene.h"
#include "export/Preview.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// What the brush hit: the part's mesh index, the UV to stamp there, and the world-space point.
// `uvPerMetre` is the UV distance per metre at the hit (from the triangle's UV-to-world area ratio), used to size the brush.
struct BrushHit {
    int part = -1;              // Scene::parts index
    int mesh = -1;              // Scene::meshes index
    int submesh = -1;           // Scene::meshes[]::submeshes index (which material's texture gets painted)
    Vec2 uv{};                  // UV at the hit (Unity convention, like SceneVertex::uv)
    Vec3 pos{};                 // world-space hit point (posed)
    float uvPerMetre = 1.0f;    // UV units per metre of world at the hit
    bool ok() const { return part >= 0 && mesh >= 0 && submesh >= 0; }
};

// The nearest part under the pointer that the brush may paint: visible (hiddenParts respects the viewer's checkboxes),
// not an under piece (skin), not Body or Head (skin), with at least one textured submesh, ray-cast against the posed
// (or morphed, or rest) mesh. False when the pointer hits nothing paintable.
bool PickBrushPart(const assemble::Scene& scene, const std::vector<char>* hiddenParts, const ViewFraming& f, float W, float H,
                   const ViewCamera& cam, float px, float py, const std::vector<Mat4>* skin,
                   const std::vector<std::vector<Vec3>>* morphDelta, BrushHit& out);

// Creates the layer for (part, material) — or finds it — and captures the texture as it is as the restore copy.
// `mesh` is the mesh the paint came from (the gate for stamps). Returns the layer index, -1 when the part has no textured material.
int TranslucencyLayer(assemble::Scene& scene, int part, int material, int mesh);

// True when the bake currently holds a layer of this part in the transparent pass (the live counterpart of "see-through is on").
// Used by EffectiveHiddenParts to keep the skin under a painted garment visible.
bool TranslucencyActive(const assemble::Scene& scene, int part);

// True when the layer holds at least one painted texel.
bool TranslucencyLayerPainted(const assemble::SceneTranslucency::Layer& layer);

// Stamps the brush at (u, v): adds `amount` (0..255) to the mask, or clears it when `erase`. The stamp is a disc of radius
// `uvRadius` in UV units, gated by the mesh's own UV coverage. Returns the number of texels changed.
int TranslucencyStamp(assemble::Scene& scene, int layerIndex, float u, float v, float uvRadius, bool erase, float amount);

// Re-applies every layer at `strength01`: the fraction of a fully painted texel's alpha that disappears (0 = fully restored,
// 0.9 = the weakest the brush slider allows). Restores the base pixels and the material's original pass flags when every
// layer ends up unpainted or at strength 0. Returns the number of textures whose bytes actually changed; when `changedTextures`
// is given it receives their indices (duplicates removed), so the caller knows exactly what to re-upload.
int TranslucencyApply(assemble::Scene& scene, float strength01, std::vector<int>* changedTextures = nullptr);

// Wipes every layer's mask and restores textures, pass flags and the stored base pixels. Returns the number of parts cleared;
// when `changedTextures` is given it receives the indices of the textures the revert rewrote.
int TranslucencyClearAll(assemble::Scene& scene, std::vector<int>* changedTextures = nullptr);

}  // namespace cardbridge::exporter
