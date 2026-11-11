#pragma once

// Automatic skin shading and nipple tint.
//
// The cards ship a nearly flat skin texture: the game paints the body with one skin colour and the shader supplies the
// lighting. Converted characters therefore read as a flat surface with no cleavage, no shadow under the breasts and no
// navel. This module bakes the body mesh's own shape into the skin texture in UV space - ray-cast ambient occlusion
// from every breast vertex, local concavity and a soft band along the midline - so the breasts and the cleavage read as
// form in both the viewer and the exported PMX. Only the breast region is shaded (vertices skinned to the bust chain
// plus a ring of neighbours); the armpits, the groin and the rest of the skin keep the card's base colour. `topExtent`
// stretches the cleavage from the apex of the fold all the way up the breast line (1 = the full line, 2 = toward the
// base of the neck).
//
// The nipple / areola colour is a material property (the game's _overcolor1). A pink tint is applied by default because
// the card's own nipColor is a pale skin tone; the tint is adjustable and can be turned off (the card's colour returns).

#include "assemble/Scene.h"

#include <cstdint>

namespace cardbridge::exporter {

// Rebuilds (when `rebuild`) the per-texel skin shade maps from the scene's body meshes and writes the texture with both
// folds at their given strengths (0 = the untouched create result, 1 = default, up to 2). Returns false when the scene
// has no body skin mesh or texture to shade.
bool ApplySkinShade(assemble::Scene& scene, float cleavageStrength, float underboobStrength, float topExtent, bool rebuild);

// Applies the automatic nipple / areola tint to every cardSkin material: colour.w > 0 uses colour.rgb, otherwise the
// card's own _overcolor1 is restored. Returns the number of materials whose colour changed.
int ApplyNippleTint(assemble::Scene& scene, const Vec4& colour);

}  // namespace cardbridge::exporter
