#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "core/Bytes.h"

#include <string>
#include <vector>

namespace cardbridge::exporter {

// The outfit the card has on screen, as a Koikatsu-style outfit-file thumbnail: a faceless, bald,
// grey mannequin wearing exactly the parts the exported file carries (the outfit's clothes and its
// kept accessories; hair and the omitted accessories are not drawn). Skin textures are dropped and
// the body/head are flat grey with the toon shading baked in; the mannequin stands on a stage-style
// backdrop (the embedded template: pink tiled wallpaper, filigree frame, floor) with a soft floor
// shadow baked behind it - the same convention Koikatsu uses for coordinate files (a mannequin in
// the outfit, not the character). The outfit renders with its own textures and shading, unchanged.
//
// hiddenParts, when given, is parallel to scene.parts with Koikatsu's meaning:
//   0 = unchanged
//   1 = do not draw
//   2 = draw even when the part starts hidden (force on: skin under garments)
// When null the scene's own final state is used.
// `note`, when set, receives a non-empty message on a degraded path (the embedded template failing
// to decode): the icon still renders, on the plain preview background, and the message says so.
struct OutfitIconOptions {
    int viewWidth = 256;
    int viewHeight = 384;
    const std::vector<char>* hiddenParts = nullptr;
    std::string* note = nullptr;
};
Bytes RenderOutfitIcon(const assemble::Scene& scene, const OutfitIconOptions& opt = {});

// The flags for the outfit icon, parallel to scene.parts: base keeps the caller's own
// flags (a viewer-ish "1 = do not draw" per part; an empty vector means "draw everything"),
// hair is hidden unless withHair, hair-type accessories and accessories the maker hid stay
// hidden, under pieces are forced on (otherwise the garment masks leave the mannequin with
// holes). `withHair` adds the character's hair parts and the hair-type accessories to the
// render (off by default). `outfit` picks the coordinate.
std::vector<char> OutfitIconHiddenParts(const assemble::Scene& scene, const card::Card& card, int outfit,
                                        const std::vector<char>& base, bool withHair = false);

} // namespace cardbridge::exporter
