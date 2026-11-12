#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "core/Bytes.h"

#include <string>
#include <vector>

namespace cardbridge::exporter {

// A flat icon of the outfit the card has on screen, the way Koikatsu itself
// does it in an outfit file: a faceless, bald, grey mannequin wearing exactly
// the parts the exported file carries (the outfit's clothes and its kept
// accessories; hair and the omitted accessories are not drawn). Skin textures
// are dropped and the body/head are a single flat grey, eyes and other painted
// face layers are not drawn. Sorted front view on the preview background, PNG
// bytes.
//
// hiddenParts, when given, is parallel to scene.parts with Koikatsu's meaning:
//   0 = unchanged
//   1 = do not draw
//   2 = draw even when the part starts hidden (force on: skin under garments)
// When null the scene's own final state is used.
// The caller renders hair by passing hiddenParts from OutfitIconHiddenParts(..., withHair): the
// mannequin icon stays bald by default (Koikatsu's own convention for coordinate files); withHair
// adds the hair meshes and the hair-type accessories to the render.
struct OutfitIconOptions {
    int viewWidth = 256;
    int viewHeight = 384;
    const std::vector<char>* hiddenParts = nullptr;
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
