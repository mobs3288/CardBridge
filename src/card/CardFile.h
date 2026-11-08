#pragma once

#include "core/Bytes.h"
#include "core/Value.h"

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

// What applying a clothing file does to a character. The clothing file is
// only a source of outfit data: ApplyClothing changes the in-memory Card and
// never writes a card file back.
struct ApplyClothingOptions {
    int outfit = -1;                  // -1: the card's current outfit (Status.coordinateType)
    bool keepHairAccessories = true;  // hair worn as accessories is never deleted (see below)
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

} // namespace cardbridge::card
