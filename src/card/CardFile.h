#pragma once

#include "core/Bytes.h"
#include "core/Value.h"

#include <set>
#include <string>
#include <vector>

namespace cardbridge::card {

enum class CardFamily { Koikatsu, KoikatsuParty, KoikatsuPartySP, KoikatsuSunshine, EmotionCreators, Other };
const char* FamilyName(CardFamily f);

enum class CardErrorKind {
    NotPng,            // not a PNG at all (JPG, text, ...)
    TruncatedPng,      // PNG chunks run off the end of the file
    NoPayload,         // a plain PNG: nothing after IEND
    UnknownMarker,     // data after IEND, but not a card of a family we know
    ClothesFile,       // a Koikatsu *outfit* file, not a character
    CorruptPayload,    // marker fine, container or a section is damaged
};
const char* ErrorKindName(CardErrorKind k);

struct CardError : FormatError {
    CardErrorKind kind;
    CardError(CardErrorKind k, const std::string& msg) : FormatError(msg), kind(k) {}
};

struct BlockInfo {
    std::string name, version;
    uint64_t pos = 0, size = 0;
};

// One row of the Sideloader "universal auto resolver" block: which mod an
// item ID in the card belongs to. `slot` is the ID the card stores (the mod
// author's own); `localSlot` is the session-local ID the game assigned when
// the card was saved and is not used for resolution.
struct ResolveRow {
    std::string modId;
    int64_t slot = 0;
    int64_t localSlot = 0;
    std::string property;     // e.g. "outfit0.ChaFileClothes.ClothesTop"
    int category = 0;
};

struct Outfit {
    Value clothes, accessory, makeup;
    bool enableMakeup = false;
};

struct Card {
    std::string path;
    uint64_t fileSize = 0;
    std::string sha256;

    CardFamily family = CardFamily::Other;
    std::string marker;
    std::string formatVersion;
    int productNo = 0;
    Bytes faceThumbnail;
    std::vector<BlockInfo> blocks;

    // false when the container was read but the family is not converted yet
    // (Sunshine): the sections below are then left empty.
    bool supported = false;
    std::string supportNote;

    Value face, body, hair, parameter, status;
    std::vector<Outfit> outfits;
    Value extended;                  // KKEx: plugin id -> [version, payload]
    std::vector<ResolveRow> rows;

    // Added clothing (in-memory only, set by ApplyClothing): which parts came
    // from a ClothingFile so the viewer/exporter can list them apart from the
    // base model. Empty when no outfit file was applied.
    bool hasClothing = false;
    std::string clothingName;   // the file's own outfit name
    std::string clothingPath;   // the file it was applied from
    int clothingOutfit = -1;    // the outfit slot it overwrote
    std::set<std::string> clothingPartIds;   // "outfitN.top", "outfitN.acc03", ...

    std::string displayName() const;

    // The game's own name for one of a Koikatsu card's coordinate slots:
    // "School Uniform (In School)" through "Sleeping Over" (0..6). Any other
    // family, and any index outside those seven, falls back to "Outfit N"
    // (one-based), so the caller can always show something.
    std::string outfitName(int index) const;
};

// Reads and decodes a card. Never launches or touches the game. Throws
// CardError with a specific kind for non-cards, FormatError for damage inside
// the payload.
Card LoadCard(const std::string& path);

// An outfit file (PNG with marker 【KoiKatuClothes】): one coordinate (the
// clothes, accessories and makeup of a single outfit) plus the same KKEx
// extended map a character card carries. Nothing is written back: the file is
// only a source of outfit data for a character.
struct ClothingFile {
    std::string path;
    uint64_t fileSize = 0;
    std::string sha256;

    std::string marker;
    std::string formatVersion;
    std::string name;            // the outfit's own name (the dotnet string after the version)
    int productNo = 0;

    bool supported = false;
    std::string supportNote;

    Outfit outfit;               // the file's single outfit
    Value extended;              // KKEx map: plugin id -> [version, payload]
    std::vector<ResolveRow> rows;

    // The coordinate inside the payload this file represents. A real outfit
    // file always carries exactly one coordinate, so this is 0. When a file is
    // synthesised in memory from a coordinate of a character card (see
    // ExtractClothingFromCard), this records which coordinate of the source
    // card it holds, so the coordinate-scoped readers of the merge path
    // (ABMX/ME/KCOX/MoreAccessories) aim at that coordinate instead of 0.
    // Author-slot ids are still used as-is (card coordinates carry no
    // "local" ids); rows are rebased to the unprefixed shape a file stores.
    int srcCoordinate = 0;
    // How many outfits the source had (1 for a real file; the card's outfit
    // count for a synthesised source). The coordinate-scoped payload rebase
    // uses it to size per-coordinate arrays.
    int srcOutfitCount = 1;
    // True when this file is a real outfit file rather than a coordinate
    // extracted from a character card. A real outfit file IS its one
    // coordinate: every coordinate-indexed payload in it (MaterialEditor rows,
    // DynamicBoneEditor rows, KCOX overlays, HairAccessoryCustomizer / pushup
    // payloads, Additional_Card_Info) belongs to that one coordinate, whatever
    // index the author saved it under. The merge reads those payloads
    // tolerant-shape in that case (see the singleCoordinate parameter);
    // `srcCoordinate` still records the author's index when it can be detected
    // for the payloads that need one (ABMX per-coordinate lists).
    bool singleCoordinateSource = false;

    // Additional_Card_Info (CoordinateInfo): which slots the file asks to keep.
    // Empty (the sample) means no explicit preference; the merge below keeps
    // hair regardless (see ApplyClothingOptions).
    std::vector<int64_t> accKeep, hairKeep;

    std::string displayName() const;
};

// Reads an outfit file (【KoiKatuClothes】). Throws CardError for non-cards
// and for damage inside the payload; a Sunshine outfit file parses but is
// reported unsupported (supported == false), never thrown.
ClothingFile LoadClothingFile(const std::string& path);

// What a PNG dropped on the importer actually is, decided from the bytes
// alone (never the file name): the Koikatsu PNG families all carry a marker
// string appended after the PNG's IEND chunk, and that marker is what this
// reports. Sunshine/EmotionCreators character markers are deliberately not
// reported as Coordinate: their payload layouts differ and neither is
// converted yet, so they travel the character path exactly as they do today.
enum class ImageSourceKind {
    Invalid,            // unreadable PNG, or bytes/marker we do not know
    PlainPng,           // a PNG with no card data after it (a plain picture)
    RegularClothing,    // a Koikatsu outfit file (【KoiKatuClothes】): the clothing import path
    Coordinate,         // a Koikatsu character card (【KoiKatuChara】 family) used as a coordinate source
};
const char* ImageSourceKindName(ImageSourceKind k);

// Detects the source kind by parsing the PNG signature, the IEND chunk and
// the marker after it. Never throws: anything unreadable or unrecognised is
// reported as Invalid (callers fall back to their previous behaviour).
ImageSourceKind DetectImageSource(const std::string& path);

// True when `marker` is one of the Koikatsu character-card markers this
// build converts (Koikatsu / Party / PartySP). Used by DetectImageSource.
bool IsKoikatsuCharacterMarker(const std::string& marker);
bool IsKoikatsuClothesMarker(const std::string& marker);

// Builds an in-memory ClothingFile from one coordinate of a character card:
// the coordinate payload (clothes, accessories, makeup) is copied verbatim,
// the card's resolver rows for that coordinate are rebased to the prefix-free
// shape a file stores ("outfitN." stripped, LocalSlot cleared so the merge's
// local->slot translation is a no-op - card coordinates already store author
// slot ids), and the coordinate's KKEx payloads (ABMX/ME/KCOX/moreAccessories)
// are kept with srcCoordinate recorded so the merge aims at that coordinate.
// Nothing of the source's identity (Custom/Parameter/Status, hair, other
// coordinates) is copied. Throws FormatError when the source is not a
// supported Koikatsu card or the coordinate index is out of range.
ClothingFile ExtractClothingFromCard(const Card& src, int coordinate);

// What applying a clothing file does to a character. The clothing file is
// only a source of outfit data: ApplyClothing changes the in-memory Card and
// never writes a card file back.
struct ApplyClothingOptions {
    int outfit = -1;                  // -1: the card's current outfit (Status.coordinateType)
    bool keepHairAccessories = true;  // hair worn as accessories is never deleted (see below)
    int srcOutfit = -1;               // Coordinate sources only: which coordinate of the source card to extract
                                      // (-1: the source's own current outfit). Ignored for plain outfit files.
    // Coordinate import of an outfit file: the file's coordinate is applied
    // wholesale - accessories replace verbatim (slots the file leaves empty
    // clear the target's, hair-type included), every resolver row and
    // extended-plugin row scoped to the target outfit is dropped rather than
    // re-indexed, matching a whole-coordinate replacement; the target's stale
    // MoreAccessories XML block for the outfit is removed, and ABMX is
    // rebased per coordinate. Additional_Card_Info travels in its coordinate
    // sense (the full entry, not just keep flags). Regular clothing imports
    // keep hair preservation, the merge, and the slot-local row re-indexing.
    bool coordinateImport = false;
};

struct ApplyClothingReport {
    int outfit = -1;
    int clothesReplaced = 0;          // clothing slots that changed id
    int accessoriesAdded = 0;         // clothing accessories placed (incl. relocated)
    int accessoriesRelocated = 0;     // placed elsewhere to keep hair
    int hairPreserved = 0;            // character hair accessories kept in place
    int hairSlotsPreserved = 0;       // character hair slots (back/front/side/extension) kept
    std::vector<std::string> notes;
};

// True when one accessory slot holds hair (type 122/123, or a mod id that names
// hair). Hair can be worn entirely as accessories (a "no hair" back slot with
// the hair in accessory slots), so the merge must never clear them.
bool IsHairAccessoryPart(const Value& part, const std::vector<ResolveRow>& rows, int outfit, int accIndex);

// Overwrites one outfit of `card` with `clothes` in memory: the clothes
// replace, the makeup follows the file's own enable flag, and the accessories
// merge so hair is never deleted. Throws FormatError when the outfit index is
// out of range.
ApplyClothingReport ApplyClothing(Card& card, const ClothingFile& clothes, const ApplyClothingOptions& opt);

// The outfit-import path generalized over both source kinds: Applies `path`
// (a regular outfit file, or a Koikatsu Coordinate PNG - a character card
// used as an outfit source) to `card` in memory, as an outfit (clothes +
// accessories) import. The source is detected from its bytes, never from the
// file name. A Coordinate source is extracted (identity, face, body,
// personality and every other coordinate are left behind) and then run
// through the same ApplyClothing merge a regular outfit file takes - so
// replacement, hair keeping, accessory placement, KCOX/ABMX/MaterialEditor
// merging and resolver-row handling behave identically. With
// `opt.coordinateImport` set, a regular outfit file is applied wholesale
// (the whole coordinate replaces the slot, accessories replaced including
// hair-type) like a Coordinate import; a Coordinate file gets the same
// semantics through the ApplyClothing path (its identity data still never
// travels). Throws CardError when the source is unreadable; never silently
// falls back to a different source type.
struct OutfitSourceReport {
    bool fromCoordinate = false;    // the source was a character card (Koikatsu Coordinate PNG)
    int srcOutfit = -1;             // which coordinate of the source was imported (Coordinate sources)
    std::string sourceName;         // the source's own display name (outfit name or character name)
    ApplyClothingReport apply;      // the underlying merge report
};
OutfitSourceReport ApplyOutfitSource(Card& card, const std::string& path, const ApplyClothingOptions& opt);

// Imports a whole coordinate from another card: the target outfit is
// replaced wholesale with the source coordinate (clothes, accessories and
// makeup - positioned, rotated, scaled and parented exactly as the source
// stores them), and the coordinate-scoped KKEx payloads (MaterialEditor rows,
// KCOX garment paint, ABMX bone mods) are rebased from the source coordinate
// to the target outfit. Unlike ApplyClothing this does not merge accessories
// around hair: the source coordinate is imported as stored, including its
// accessories. Nothing outside the coordinate is imported: the source's
// identity (Custom/Parameter/Status beyond the accessory visibility flags),
// hair, body, other coordinates and unrelated KKEx plugins are never copied.
// Throws FormatError when either index is out of range or the source card is
// not supported.
struct ApplyCoordinateOptions {
    int srcOutfit = -1;               // -1: the source card's current outfit (Status.coordinateType)
    int dstOutfit = -1;               // -1: the target's current outfit (Status.coordinateType)
};
struct ApplyCoordinateReport {
    int srcOutfit = -1;
    int dstOutfit = -1;
    int clothesReplaced = 0;          // clothing slots whose id changed
    int accessoriesReplaced = 0;      // target accessory slots dropped/replaced (all non-empty source slots)
    bool makeupApplied = false;       // the source coordinate had makeup enabled
    std::string sourceName;           // the source card's own display name
    std::vector<std::string> notes;   // one line per plugin merged, for the log
};
ApplyCoordinateReport ApplyCoordinate(Card& card, const Card& src, const ApplyCoordinateOptions& opt);

} // namespace cardbridge::card
