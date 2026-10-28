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
};

// Reads and decodes a card. Never launches or touches the game. Throws
// CardError with a specific kind for non-cards, FormatError for damage inside
// the payload.
Card LoadCard(const std::string& path);

} // namespace cardbridge::card
