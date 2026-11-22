#pragma once

#include <cstdint>
#include <vector>

namespace cardbridge::assemble {

// The nose texture (cf_O_noseline / category 414) is ONE texture in KK/KKS but can carry two independent features:
//   * a nose line - a tall, thin stroke running down the middle of the face, and
//   * nostrils - one or two short, wide strokes (arcs or dots) at the base of the nose.
// A card may have either, both or neither. They must never be gated on each other: a card with nostrils and no nose line
// is the common case for head mods, and softening that texture as if it were a nose line erases the nostrils.
struct NoseFeatures {
    bool line = false;      // a vertical centre stroke (the classic Koikatsu "nose line")
    bool nostrils = false;  // one or two short strokes away from the centre (the nostril arcs / dots)
};

// Classify the features authored in a nose texture from its alpha shape. Pure geometry analysis: no card ids, names or
// hardcoded UV coordinates, so it works for base-game rows and modded textures alike.
NoseFeatures DetectNoseFeatures(const std::vector<uint8_t>& rgba, int w, int h);

// Feather only the nose-LINE pixels. Nostril pixels are copied byte for byte, including their alpha: a faint nostril arc
// whose alpha sits just above the material's _Cutoff must stay above it, or the whole overlay is discarded by the bake.
// RGB is never touched. Pass the detection result from DetectNoseFeatures.
void SoftenNoseLineOnly(std::vector<uint8_t>& rgba, int w, int h, const NoseFeatures& features);

// The lip-line texture (cf_O_lip_line / category 404) can likewise carry a line where the lips meet (upper) and a
// separate line just below them (lower). Each is detected and kept independently of the other.
struct LipLineFeatures {
    bool upper = false;
    bool lower = false;
};
LipLineFeatures DetectLipLineFeatures(const std::vector<uint8_t>& rgba, int w, int h);

// Some head mods bake a narrow shading ridge down the centre of the nose bridge into the face skin. It is part of the
// mod's source data, not a nose line, but it can read like one. This is a presentation setting: 1 keeps the authored
// ridge, 0 removes it, values between/above make it subtler or stronger. Detection is per card, so a card whose face
// has no ridge defaults to 0 and nothing is ever added to it.
struct NoseRidge {
    float strength = 1.0f;        // 1 = authored, 0 = off
    float detected = 0.0f;        // how pronounced the authored ridge is (0 = none)
    int texture = -1;             // the face create-result texture this applies to
    int width = 0, height = 0;
    std::vector<uint8_t> original;  // untouched pixels, so the strength can be changed repeatedly without drift
    std::vector<float> mask;        // per-pixel ridge weight from DetectNoseRidge
};

// Finds the centre ridge in a face-skin texture. `weight` (w*h floats, 0..1) marks the pixels that belong to it.
// Returns the peak strength of the authored ridge (0 when there is none).
float DetectNoseRidge(const std::vector<uint8_t>& rgba, int w, int h, std::vector<float>& weight);

// Rebuilds the face texture from `original` with the ridge softened by (1 - strength). Requires the mask from
// DetectNoseRidge. Pure: repeated calls with different strengths never accumulate.
void ApplyNoseRidge(std::vector<uint8_t>& rgba, int w, int h, const std::vector<float>& weight, float strength);

struct Scene;   // Scene.h includes this header; a forward declaration keeps the dependency one-way

// Applies the setting to a scene's stored face texture, restoring the untouched pixels each time so slider changes
// never accumulate. Safe on cards with no detected ridge (no-op).
void SetNoseRidgeStrength(Scene& scene, float strength);

} // namespace cardbridge::assemble
