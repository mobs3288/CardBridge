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

} // namespace cardbridge::assemble
