#pragma once

#include "card/CardFile.h"
#include "core/Bytes.h"

#include <set>
#include <string>
#include <vector>

namespace cardbridge::card {

// Writes a Koikatsu *outfit* file (【KoiKatuClothes】 .png) from one coordinate
// of a character card — the inverse of `LoadClothingFile` / `ApplyClothing`.
//
// The file is the game's own container for one coordinate: a PNG image
// (cosmetic icon) with the coordinate (clothes / accessories / makeup,
// MessagePack) and the Sideloader / MaterialEditor / KCOX payloads appended
// after IEND. Ids are translated author Slot -> session LocalSlot for mod
// items the same way the outfit-file importer translates them back, so a
// written file applies to any character through the existing merge path.
//
// Hidden accessories (Status.showAccessory == 0, §5.9 of docs/CLOTHING_EXPORT.md)
// and hair-type accessories are omitted by default; occupied by an empty slot
// for slots 0-19 and by compaction (with rows/ME re-numbering) for slots past
// 20. Pass `includeHidden` / `withHairAccessories` / `hiddenSlots` to change
// that. See docs/CLOTHING_EXPORT.md for the full design.
//
// Throws FormatError when the card is not a convertible family, when the
// outfit index is out of range, or when a coordinate claims accessories
// beyond slot 20 with no backing MoreAccessories XML — never a silent drop.
struct ClothingExportOptions {
    int outfit = -1;               // -1: the card's current (Status.coordinateType), like convert
    std::string name;              // empty: default derived from character + outfit
    std::string thumbnailPngPath;  // empty: use `thumbnailBytes`; empty there too: reuse the card's face thumbnail
    Bytes thumbnailBytes;           // empty: render or fall back to the face thumbnail (see docs)
    bool withAbmx = false;         // carry the whole KKABMPlugin.ABMData table verbatim (default off)
    bool includeHidden = false;    // keep accessories the card hides (showAccessory==0); default off
    bool withHairAccessories = false;  // keep hair-type accessories (type 122/123); default off
    std::set<int> hiddenSlots;      // extra accessory slots to omit (viewer untick / CLI --hide)
};

struct ClothingExportReport {
    std::string outfitName;
    int outfit = -1;
    int clothesIdsLocalized = 0;   // author Slot -> LocalSlot rewrites (clothes + subs + patterns + emblems)
    int accessoryIdsLocalized = 0;
    int subPartsLocalized = 0;
    int uarRowsKept = 0;
    int meListRows = 0;            // MaterialEditor rows rebased to coordinate 0
    int meTexturesKept = 0;        // TextureDictionary entries that survived the subset filter
    int kcoxGarments = 0;          // overlays carried over
    int extraAccessoriesKept = 0;  // accessories beyond slot 20 carried in the MoreAccessories XML
    int hiddenAccessoriesDropped = 0;  // slots the card hides (or the user listed), omitted (§5.9)
    int hairAccessoriesDropped = 0;    // hair-type accessories omitted by default (§5.9)
    std::vector<std::string> droppedPlugins;   // KKEx keys omitted, by name — nothing is silent
    std::vector<std::string> warnings;         // unresolved/unknown ids written raw, etc.
};

// Packs one coordinate into a standalone outfit file. Throws FormatError /
// CardError (ClothesFile/family checks) like the reader.
Bytes BuildClothingFileBytes(const Card& card, const ClothingExportOptions& opt,
                             ClothingExportReport* report = nullptr);

// Writes to disk (creates parent directories). Refuses to overwrite the
// source card (out resolves to the same path) and refuses to overwrite an
// existing file unless `overwrite` is true.
ClothingExportReport ExportClothingToFile(const Card& card, const std::string& outPath,
                                          const ClothingExportOptions& opt, bool overwrite = false);

} // namespace cardbridge::card
