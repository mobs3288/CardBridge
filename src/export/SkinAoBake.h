#pragma once

// Koikatsu's own skin look, baked into the body's skin texture.
//
// The game renders the body through Shader Forge/main_skin: the always-bound base body normal map
// (cf_body_00_base_n) gives the under-bust valley, the ribs and the abs their form under the toon
// lighting, and _LineMask (the card's body detailId row in the game lists, category 420 -
// cf_body_00_line for the default, a mod's line texture on BetterRepack-style cards) paints the
// ink lines at the collarbone, the knees, the navel and the fingernails. CardBridge binds neither
// today, which is why the converted body reads as a flat surface with no anatomy.
//
// This module bakes both into the body's skin texture, in UV space, at export time:
//
//  - a cavity factor from the base body normal map: how much each covered texel's tangent-space
//    normal curves away from its neighbours' average. Orientation-independent (no view or light
//    direction), so it cannot re-author the lighting the way MMD does - it only darkens creases,
//    joints and contact folds and leaves flat skin alone. The card's detailId detail normal
//    (NormallMapDetail) is blended into the same local vector, exactly as Koikatsu's shader
//    blends _DetailNormalMapScale.
//  - the ink lines from the card's detailId LineMask (G channel; the default cf_body_00_line's
//    clavicle/knee/navel strokes and a BetterRepack kit's sternum line author-marked in the same
//    channel family).
//
// It complements - never stacks with - ApplySkinShade: the new bake fades toward no-op wherever
// SkinShade already darkened a texel (the fade is interpolated per texel from the existing
// cleavage/underboob factors), the ink lines are held out of SkinShade's crease band, and the AO
// factors are floored so a texel never goes below 0.30 from this module alone (SkinShade's own
// factor stays authoritative in its band). Alpha is never touched and the skin colour
// customization is preserved - the bake multiplies the composed colour, it never writes it.
//
// Like ApplySkinShade, the result is baked into the texture the PMX exporter references, so every
// viewer of the exported model - MikuMikuDance, Project K, CardBridge itself - sees it with no
// CardBridge-specific rendering.

#include "assemble/Scene.h"

#include <cstdint>

namespace cardbridge::exporter {

// Bakes the cavity shading and the ink lines into the body skin texture at `strength`
// (0 = off: the texture is restored to base-x-SkinShade, 1 = the default, up to 2). `rebuild`
// recomputes the per-texel maps from the current body geometry (after a shape Reshape); otherwise
// only the strength multiplication is re-applied. Returns false when the body skin texture, or the
// base body normal map for the card's body, is unavailable - the caller keeps the texture untouched.
bool ApplySkinAoBake(assemble::Scene& scene, float strength, bool rebuild);

// The viewer's diagnostic strip ("SkinAO") renders this gray: one byte per body-skin texel with the
// multiplier the current bake applies (255 = no effect, 0 = fully darkened). Uncovered texels are
// 255. Empty when there is no body skin texture or no AO data - the viewer then renders the body
// flat (the correct look for "AO off").
std::vector<uint8_t> BuildSkinAoFactor(const assemble::Scene& scene);

}  // namespace cardbridge::exporter
