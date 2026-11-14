#include "card/CardFile.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "card/Abmx.h"
#include "core/FileIo.h"
#include "core/MsgPack.h"
#include "core/Sha256.h"

namespace cardbridge::card {

const char* FamilyName(CardFamily f) {
    switch (f) {
    case CardFamily::Koikatsu: return "koikatsu";
    case CardFamily::KoikatsuParty: return "koikatsu-party";
    case CardFamily::KoikatsuPartySP: return "koikatsu-party-sp";
    case CardFamily::KoikatsuSunshine: return "koikatsu-sunshine";
    case CardFamily::EmotionCreators: return "emotion-creators";
    default: return "other";
    }
}

const char* ErrorKindName(CardErrorKind k) {
    switch (k) {
    case CardErrorKind::NotPng: return "not-a-png";
    case CardErrorKind::TruncatedPng: return "truncated-png";
    case CardErrorKind::NoPayload: return "png-without-card-data";
    case CardErrorKind::UnknownMarker: return "unknown-card-marker";
    case CardErrorKind::ClothesFile: return "outfit-file-not-a-character";
    default: return "corrupt-card-payload";
    }
}

std::string Card::displayName() const {
    std::string n = parameter.at("lastname").asStr();
    std::string f = parameter.at("firstname").asStr();
    if (!n.empty() && !f.empty()) return n + " " + f;
    return n.empty() ? f : n;
}

std::string Card::outfitName(int index) const {
    // The seven coordinate slots of a Koikatsu character, in the order the
    // game's outfit menu lists them (the card itself stores no names).
    static const char* kNames[] = {
        "School Uniform (In School)",
        "School Uniform (Going Home)",
        "Gym Uniform",
        "Swimsuit",
        "Club Activities",
        "Casual Clothes",
        "Sleeping Over",
    };
    const bool koikatsu = family == CardFamily::Koikatsu || family == CardFamily::KoikatsuParty ||
                          family == CardFamily::KoikatsuPartySP;
    if (koikatsu && index >= 0 && index < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) {
        return kNames[index];
    }
    return "Outfit " + std::to_string(index + 1);
}

namespace {

// Offset of the first byte after the PNG's IEND chunk.
size_t pngEnd(const Bytes& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (b.size() < 8 || std::memcmp(b.data(), sig, 8) != 0)
        throw CardError(CardErrorKind::NotPng, "not a PNG file");
    ByteReader r(b, true);
    r.skip(8);
    while (true) {
        if (r.remaining() < 12) throw CardError(CardErrorKind::TruncatedPng, "PNG ends before its IEND chunk");
        uint32_t len = r.u32();
        std::string type = r.str(4);
        if (static_cast<uint64_t>(len) + 4 > r.remaining())
            throw CardError(CardErrorKind::TruncatedPng, "PNG chunk '" + type + "' runs past the end of the file");
        r.skip(static_cast<size_t>(len) + 4);   // data + CRC
        if (type == "IEND") return r.pos();
    }
}

CardFamily familyOf(const std::string& marker, bool& isClothes) {
    isClothes = false;
    if (marker == "\xE3\x80\x90KoiKatuChara\xE3\x80\x91") return CardFamily::Koikatsu;                  // 【KoiKatuChara】
    if (marker == "\xE3\x80\x90KoiKatuCharaS\xE3\x80\x91") return CardFamily::KoikatsuParty;            // 【KoiKatuCharaS】
    if (marker == "\xE3\x80\x90KoiKatuCharaSP\xE3\x80\x91") return CardFamily::KoikatsuPartySP;        // 【KoiKatuCharaSP】
    if (marker == "\xE3\x80\x90KoiKatuCharaSun\xE3\x80\x91") return CardFamily::KoikatsuSunshine;      // 【KoiKatuCharaSun】
    if (marker == "\xE3\x80\x90" "EroMakeChara\xE3\x80\x91") return CardFamily::EmotionCreators;       // 【EroMakeChara】
    if (marker.rfind("\xE3\x80\x90KoiKatuClothes", 0) == 0) isClothes = true;                        // 【KoiKatuClothes】 and variants
    return CardFamily::Other;
}

Value decodeCoordinate(const Bytes& blob, Outfit& out) {
    ByteReader r(blob, false);
    auto chunk = [&]() {
        int32_t n = r.i32();
        if (n < 0 || static_cast<size_t>(n) > r.remaining()) throw FormatError("coordinate section runs past its data");
        // Parsed in place: copying the chunk first held the section twice.
        ByteReader sub(r.ptr(), static_cast<size_t>(n), false);
        Value v = MsgPackRead(sub);
        r.skip(static_cast<size_t>(n));
        return v;
    };
    out.clothes = chunk();
    out.accessory = chunk();
    out.enableMakeup = r.u8() != 0;
    out.makeup = chunk();
    if (r.remaining() != 0) throw FormatError("coordinate section has trailing bytes");
    return Value();
}

} // namespace

const char* ImageSourceKindName(ImageSourceKind k) {
    switch (k) {
    case ImageSourceKind::PlainPng: return "plain-png";
    case ImageSourceKind::RegularClothing: return "regular-clothing";
    case ImageSourceKind::Coordinate: return "coordinate";
    default: return "invalid";
    }
}

bool IsKoikatsuClothesMarker(const std::string& marker) {
    return marker.rfind("\xE3\x80\x90KoiKatuClothes", 0) == 0;                        // 【KoiKatuClothes】 and variants
}

bool IsKoikatsuCharacterMarker(const std::string& marker) {
    // The families this build converts. Sunshine/EmotionCreators cards are not
    // usable as coordinate sources yet and stay on the character path.
    return marker == "\xE3\x80\x90KoiKatuChara\xE3\x80\x91" ||
           marker == "\xE3\x80\x90KoiKatuCharaS\xE3\x80\x91" ||
           marker == "\xE3\x80\x90KoiKatuCharaSP\xE3\x80\x91";
}

ImageSourceKind DetectImageSource(const std::string& path) {
    try {
        Bytes file = fileio::ReadFile(path);
        const size_t end = pngEnd(file);   // throws NotPng / TruncatedPng
        if (end >= file.size()) return ImageSourceKind::PlainPng;
        ByteReader r(file.data() + end, file.size() - end, false);
        std::string marker;
        try {
            r.skip(4);                    // productNo; its value is not needed for detection
            marker = r.dotnetString();
        } catch (const FormatError&) {
            return ImageSourceKind::Invalid;
        }
        if (IsKoikatsuClothesMarker(marker)) return ImageSourceKind::RegularClothing;
        if (IsKoikatsuCharacterMarker(marker)) return ImageSourceKind::Coordinate;
        return ImageSourceKind::Invalid;
    } catch (const std::exception&) {
        return ImageSourceKind::Invalid;
    }
}

namespace {

// The MoreAccessories plugin keeps the accessory slots beyond the game's 20 as XML in the card's extended data:
// <accessorySet type="<coordinate>"><accessory type id parentKey addMove<k><r><axis> color<n><channel> .../>...</accessorySet>.
// Slot 20 + n of that outfit is the n-th <accessory>. They are appended to the outfit's accessory parts in the shape the
// card's own PartsInfo has, so everything downstream reads them like the first 20.
std::string xmlAttr(const std::string& tag, const std::string& name) {
    std::string key = " " + name + "=\"";
    size_t at = tag.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    size_t end = tag.find('"', at);
    return end == std::string::npos ? std::string() : tag.substr(at, end - at);
}

// MoreAccessories for a character holds its extra accessories (slots 20+) for
// the whole card in one XML string. The game double-writes them in current
// saves: the coordinate chunk carries the extras *and* the XML carries the
// same elements. Appending the XML on top of the chunk then duplicates
// every extra (the twin-ribbon case: two identical parts added to the
// coordinate). Only append as many XML elements as needed to bring a
// coordinate up to 20 + its own XML count.
void appendMoreAccessories(Card& c) {
    const Value& more = c.extended.at("moreAccessories").at(size_t(1)).at("additionalAccessories");
    if (!more.isStr()) return;
    const std::string& xml = more.asStr();

    // First pass: how many <accessory> elements does each outfit's XML block hold?
    std::map<int, int> xmlCount;
    {
        int coordinate = -1;
        size_t pos = 0;
        while ((pos = xml.find('<', pos)) != std::string::npos) {
            size_t end = xml.find('>', pos);
            if (end == std::string::npos) break;
            std::string tag = xml.substr(pos, end - pos);
            pos = end + 1;
            if (tag.compare(0, 13, "<accessorySet") == 0) { coordinate = std::atoi(xmlAttr(tag, "type").c_str()); continue; }
            if (tag.compare(0, 10, "<accessory") != 0 || tag.compare(0, 13, "<accessorySet") == 0) continue;
            if (coordinate < 0) continue;   // no wrapper (unsupported shape): nothing to attribute
            ++xmlCount[coordinate];
        }
    }

    // How many elements does each outfit still need from its XML block?
    // (Computed from the parts state before anything is appended.)
    std::map<int, int> toAppend;
    for (const auto& [coord, xmlN] : xmlCount) {
        int current = 0;
        if (static_cast<size_t>(coord) < c.outfits.size()) {
            const auto& acc = c.outfits[static_cast<size_t>(coord)].accessory;
            if (acc.isMap()) current = std::max(0, static_cast<int>(acc.at("parts").size()) - 20);
        }
        toAppend[coord] = std::max(0, xmlN - current);
    }

    int coordinate = -1;
    int appended = 0;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos, end - pos);
        pos = end + 1;
        if (tag.compare(0, 13, "<accessorySet") == 0) { coordinate = std::atoi(xmlAttr(tag, "type").c_str()); appended = 0; continue; }
        if (tag.compare(0, 10, "<accessory") != 0 || tag.compare(0, 13, "<accessorySet") == 0) continue;
        if (coordinate < 0 || static_cast<size_t>(coordinate) >= c.outfits.size()) continue;
        if (appended >= toAppend[coordinate]) continue;   // the chunk already carries these
        ++appended;
        auto num = [&](const std::string& n, double def) { std::string v = xmlAttr(tag, n); return v.empty() ? def : std::atof(v.c_str()); };
        Value::Array colors;
        for (int k = 0; k < 4; ++k) {
            Value::Array rgba;
            const char* ch = "rgba";
            for (int j = 0; j < 4; ++j) rgba.emplace_back(num("color" + std::to_string(k) + ch[j], 1.0));
            colors.emplace_back(std::move(rgba));
        }
        Value::Array flat;   // addMove[2][3] flattened: per move node a position, a rotation (degrees) and a scale
        for (int k = 0; k < 2; ++k)
            for (int r = 0; r < 3; ++r) {
                Value::Array v3;
                for (const char* axis : {"x", "y", "z"})
                    v3.emplace_back(num("addMove" + std::to_string(k) + std::to_string(r) + axis, r == 2 ? 1.0 : 0.0));
                flat.emplace_back(std::move(v3));
            }
        Value::Array addMove;
        addMove.emplace_back(int64_t{2});
        addMove.emplace_back(int64_t{3});
        addMove.emplace_back(std::move(flat));
        Value::Map part;
        part.emplace_back(Value(std::string("type")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "type").c_str()))));
        part.emplace_back(Value(std::string("id")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "id").c_str()))));
        part.emplace_back(Value(std::string("parentKey")), Value(xmlAttr(tag, "parentKey")));
        part.emplace_back(Value(std::string("color")), Value(std::move(colors)));
        part.emplace_back(Value(std::string("addMove")), Value(std::move(addMove)));
        part.emplace_back(Value(std::string("hideCategory")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "hideCategory").c_str()))));
        part.emplace_back(Value(std::string("noShake")), Value(xmlAttr(tag, "noShake") == "true"));
        Value& acc = c.outfits[static_cast<size_t>(coordinate)].accessory;
        if (!acc.isMap()) continue;
        for (auto& kv : acc.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts" && kv.second.isArray()) kv.second.array().emplace_back(Value(std::move(part)));
    }
}

// Removes the <accessorySet type="N"> ... </accessorySet> blocks of one
// coordinate from a MoreAccessories XML string, keeping every other
// coordinate's block. A coordinate import replaces the whole coordinate and
// its extras travel inside the coordinate payload itself (the accessory map is
// copied wholesale), so the target's old XML block for that slot would
// double-append on the next card load. Flat XML (no <accessorySet> wrapper)
// is left untouched: the card loader never attributes its elements anyway.
std::string dropAccessorySetBlocks(const std::string& xml, int coordinate) {
    std::string out;
    out.reserve(xml.size());
    size_t pos = 0;
    while (pos < xml.size()) {
        const size_t setPos = xml.find("<accessorySet", pos);
        if (setPos == std::string::npos) { out += xml.substr(pos); break; }
        out += xml.substr(pos, setPos - pos);
        const size_t blockEndMark = xml.find("</accessorySet>", setPos);
        const size_t tagEnd = xml.find('>', setPos);
        if (blockEndMark == std::string::npos || tagEnd == std::string::npos) {
            // Unterminated block: stop editing, keep the rest verbatim rather than corrupt it.
            out += xml.substr(setPos);
            return out;
        }
        const size_t blockEnd = blockEndMark + std::string("</accessorySet>").size();
        const std::string tag = xml.substr(setPos, tagEnd - setPos + 1);
        if (std::atoi(xmlAttr(tag, "type").c_str()) != coordinate)
            out += xml.substr(setPos, blockEnd - setPos);
        pos = blockEnd;
    }
    return out;
}

} // namespace

// Removes the target coordinate's own <accessorySet type="N"> block from the
// card's moreAccessories XML. Used when the coordinate is replaced wholesale:
// its extras travel inside the copied/applied coordinate payload, so the
// target's stale XML block would double-append them on the next card load.
// Flat XML (no wrapper) is left untouched - the card loader never attributes
// those elements. Other coordinates' blocks are kept verbatim.
namespace {
void dropOutfitMoreAccessoriesXml(Card& card, int coordinate, std::vector<std::string>& notes) {
    if (!card.extended.isMap()) return;
    const Value& more = card.extended.at("moreAccessories").at(size_t(1)).at("additionalAccessories");
    if (!more.isStr()) return;
    const std::string cleaned = dropAccessorySetBlocks(more.asStr(), coordinate);
    if (cleaned == more.asStr()) return;
    for (auto& kv : card.extended.map()) {
        if (!kv.first.isStr() || kv.first.asStr() != "moreAccessories") continue;
        if (!kv.second.isArray() || kv.second.size() < 2) continue;
        const Value& payload = kv.second.at(size_t(1));
        if (!payload.isMap()) continue;
        Value version = kv.second.at(size_t(0));
        Value::Map np;
        for (const auto& pk : payload.asMap()) {
            if (pk.first.isStr() && pk.first.asStr() == "additionalAccessories") np.emplace_back(pk.first, Value(cleaned));
            else np.emplace_back(pk.first, pk.second);
        }
        kv.second = Value(Value::Array{Value(std::move(version)), Value(std::move(np))});
        notes.emplace_back("MoreAccessories: outfit " + std::to_string(coordinate) + "'s stale XML block dropped");
        return;
    }
}
} // namespace

static const char* kMeLists[] = {"RendererPropertyList", "MaterialFloatPropertyList", "MaterialColorPropertyList",
                                 "MaterialTexturePropertyList", "MaterialShaderList", "MaterialNamePropertyList",
                                 "MaterialKeywordPropertyList", "ProjectorPropertyList"};

std::string ClothingFile::displayName() const { return name; }

namespace {

bool isSunshineClothesMarker(const std::string& marker) {
    // 【KoiKatuClothesSun】 (and any other Sun variant): the container has not
    // been verified, like a Sunshine character.
    return marker.find("Sun") != std::string::npos || marker.find("SUN") != std::string::npos;
}

// MoreAccessories for one outfit (a clothing file has a single coordinate 0).
// Slots 20 + n are appended to the outfit's accessory parts, same as a card.
// The game's coordinate files write the flat shape (<additionalAccessories><accessory …/>
// with no <accessorySet> wrapper); cards write per-coordinate <accessorySet type="N">
// blocks. Both are accepted: with no wrapper present, every <accessory> child
// belongs to the one coordinate.
// `coordinateIndex` is the coordinate this outfit represents inside the source
// (0 for a real outfit file; the card's coordinate number when the outfit was
// synthesised from a character card, see ExtractClothingFromCard).
// `flatShapeCounts` controls the no-wrapper case: a real outfit file counts
// every element (its single coordinate is the file), while a card treats flat
// XML as unattributed and skips it (matching the card-side loader's rule).
void appendMoreAccessoriesToOutfit(Outfit& outfit, const Value& extended, int coordinateIndex = 0, bool flatShapeCounts = true) {
    const Value& more = extended.at("moreAccessories").at(size_t(1)).at("additionalAccessories");
    if (!more.isStr()) return;
    const std::string& xml = more.asStr();
    // The game double-writes extras in current coordinate files too (chunk and
    // XML). Only append as many as needed to bring the coordinate to 20 + XML count.
    bool anyAccessorySet = false;
    for (size_t p = 0; (p = xml.find("<accessorySet", p)) != std::string::npos; p += 13) { anyAccessorySet = true; break; }
    int coordinate = anyAccessorySet ? -1 : (flatShapeCounts ? coordinateIndex : -1);   // flat game shape: everything is coordinate 0
    int toAppend = 0;
    {
        int count = 0;
        if (anyAccessorySet) {
            // count elements inside the block of the coordinate being extracted
            size_t p = 0;
            bool inOutfit0 = false;
            while ((p = xml.find('<', p)) != std::string::npos) {
                const size_t end = xml.find('>', p);
                if (end == std::string::npos) break;
                const std::string tag = xml.substr(p, end - p);
                p = end + 1;
                if (tag.compare(0, 13, "<accessorySet") == 0) { inOutfit0 = std::atoi(xmlAttr(tag, "type").c_str()) == coordinateIndex; continue; }
                if (tag.rfind("<accessory", 0) == 0 && inOutfit0) ++count;
            }
        } else if (flatShapeCounts) {
            size_t p = 0;
            while ((p = xml.find("<accessory", p)) != std::string::npos) { ++count; p += 10; }
        }
        int current = 0;
        if (outfit.accessory.isMap()) current = std::max(0, static_cast<int>(outfit.accessory.at("parts").size()) - 20);
        toAppend = std::max(0, count - current);
    }
    int appended = 0;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos, end - pos);
        pos = end + 1;
        if (tag.compare(0, 13, "<accessorySet") == 0) { coordinate = std::atoi(xmlAttr(tag, "type").c_str()); continue; }
        if (tag.compare(0, 10, "<accessory") != 0 || tag.compare(0, 13, "<accessorySet") == 0) continue;
        if (coordinate != coordinateIndex) continue;   // this outfit's own block only
        if (appended >= toAppend) continue;   // the coordinate chunk already carries these
        ++appended;
        auto num = [&](const std::string& n, double def) { std::string v = xmlAttr(tag, n); return v.empty() ? def : std::atof(v.c_str()); };
        Value::Array colors;
        for (int k = 0; k < 4; ++k) {
            Value::Array rgba;
            const char* ch = "rgba";
            for (int j = 0; j < 4; ++j) rgba.emplace_back(num("color" + std::to_string(k) + ch[j], 1.0));
            colors.emplace_back(std::move(rgba));
        }
        Value::Array flat;
        for (int k = 0; k < 2; ++k)
            for (int r = 0; r < 3; ++r) {
                Value::Array v3;
                for (const char* axis : {"x", "y", "z"})
                    v3.emplace_back(num("addMove" + std::to_string(k) + std::to_string(r) + axis, r == 2 ? 1.0 : 0.0));
                flat.emplace_back(std::move(v3));
            }
        Value::Array addMove;
        addMove.emplace_back(int64_t{2});
        addMove.emplace_back(int64_t{3});
        addMove.emplace_back(std::move(flat));
        Value::Map part;
        part.emplace_back(Value(std::string("type")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "type").c_str()))));
        part.emplace_back(Value(std::string("id")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "id").c_str()))));
        part.emplace_back(Value(std::string("parentKey")), Value(xmlAttr(tag, "parentKey")));
        part.emplace_back(Value(std::string("color")), Value(std::move(colors)));
        part.emplace_back(Value(std::string("addMove")), Value(std::move(addMove)));
        part.emplace_back(Value(std::string("hideCategory")), Value(static_cast<int64_t>(std::atoi(xmlAttr(tag, "hideCategory").c_str()))));
        part.emplace_back(Value(std::string("noShake")), Value(xmlAttr(tag, "noShake") == "true"));
        Value& acc = outfit.accessory;
        if (!acc.isMap()) continue;
        for (auto& kv : acc.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts" && kv.second.isArray()) kv.second.array().emplace_back(Value(std::move(part)));
    }
}

bool containsLower(const std::string& s, const char* sub) {
    std::string h(s), n(sub);
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

bool isEmptyAcc(const Value& part) {
    if (!part.isMap()) return true;
    const int type = static_cast<int>(part.at("type").asInt(120));
    const int64_t id = part.at("id").asInt(0);
    return type == 120 || id == 0;
}

void setPartId(Value& part, int64_t id) {
    if (!part.isMap()) return;
    for (auto& kv : part.map())
        if (kv.first.isStr() && kv.first.asStr() == "id") { kv.second = Value(id); return; }
}

// An empty accessory slot to pad merged lists: a clone of a known empty part
// when there is one, else a minimal type-120 part (ItemsFromCard skips it).
Value emptyAccPart(const Value& a, const Value& b) {
    auto findEmpty = [](const Value& v) -> const Value* {
        if (!v.isMap()) return nullptr;
        const Value& parts = v.at("parts");
        if (!parts.isArray()) return nullptr;
        for (const auto& p : parts.asArray()) if (isEmptyAcc(p)) return &p;
        return nullptr;
    };
    if (const Value* p = findEmpty(a)) return *p;
    if (const Value* p = findEmpty(b)) return *p;
    // Minimal: type / id / parentKey are all a reader needs for an empty slot.
    Value::Array colors[4];
    Value::Array color;
    for (int k = 0; k < 4; ++k) { Value::Array rgba; for (int j = 0; j < 4; ++j) rgba.emplace_back(1.0); colors[k] = rgba; color.emplace_back(colors[k]); }
    Value::Array flat;
    for (int k = 0; k < 2; ++k) for (int r = 0; r < 3; ++r) { Value::Array v3; for (int j = 0; j < 3; ++j) v3.emplace_back(r == 2 ? 1.0 : 0.0); flat.emplace_back(v3); }
    Value::Array addMove{Value(int64_t{2}), Value(int64_t{3}), Value(std::move(flat))};
    Value::Map part;
    part.emplace_back(Value(std::string("type")), Value(int64_t{120}));
    part.emplace_back(Value(std::string("id")), Value(int64_t{0}));
    part.emplace_back(Value(std::string("parentKey")), Value(std::string("")));
    part.emplace_back(Value(std::string("color")), Value(color));
    part.emplace_back(Value(std::string("addMove")), Value(std::move(addMove)));
    return Value(std::move(part));
}

// Find in a raw map (string keys only); nullptr when absent or not a map.
const Value* mapFind(const Value::Map& map, const char* key) {
    for (const auto& kv : map)
        if (kv.first.isStr() && kv.first.asStr() == key) return &kv.second;
    return nullptr;
}
const Value* mapFind(const Value& map, const char* key) {
    if (!map.isMap()) return nullptr;
    for (const auto& kv : map.asMap())
        if (kv.first.isStr() && kv.first.asStr() == key) return &kv.second;
    return nullptr;
}


const Value* selectCoordinateEntry(const std::map<int64_t, Value>& entries, int srcCoordinate) {
    if (entries.empty()) return nullptr;
    if (srcCoordinate >= 0) {
        auto it = entries.find(static_cast<int64_t>(srcCoordinate));
        return it != entries.end() ? &it->second : nullptr;
    }
    return &entries.begin()->second;   // the file's only coordinate
}


// A real outfit file stores one coordinate verbatim, under whatever coordinate
// index the author saved it with (community downloads carry e.g. CoordinateIndex 3).
// Every coordinate-indexed payload in the file belongs to that one coordinate, so
// detect its index from the payloads that carry one: the MaterialEditor rows and the
// DynamicBoneEditor rows, plus the nested payloads keyed by coordinate (pushup bins,
// keyed Additional_Card_Info). The majority wins; 0 when absent. Flat payloads carry
// no index and are read shape-tolerantly regardless; ABMX per-coordinate lists use this value.
int detectClothingFileCoordinateIndex(const Value& extended) {
    std::map<int64_t, int64_t> counts;
    auto tallyMeOrDbe = [&counts](const Value& bin) {
        if (!bin.isBin() || bin.asBin().empty()) return;
        try {
            Value list = MsgPackParse(bin.asBin());
            if (!list.isArray()) return;
            int seen = 0;
            for (const auto& item : list.asArray()) {
                if (++seen > 64) break;
                if (!item.isMap()) continue;
                const Value& c = item.at("CoordinateIndex");
                if (c.isInt()) ++counts[c.asInt()];
            }
        } catch (const FormatError&) {}
    };
    auto tallyKeyed = [&counts](const Value& bin) {
        // nested {coordIdx: {...}}: a pure int map votes its keys (flat payloads have string keys)
        if (!bin.isBin() || bin.asBin().empty()) return;
        try {
            Value parsed = MsgPackParse(bin.asBin());
            if (!parsed.isMap()) return;
            bool keyed = true;
            for (const auto& kv : parsed.asMap()) if (!kv.first.isInt()) { keyed = false; break; }
            if (!keyed) return;
            for (const auto& kv : parsed.asMap()) ++counts[kv.first.asInt()];
        } catch (const FormatError&) {}
    };
    const Value& me = extended.at("com.deathweasel.bepinex.materialeditor").at(size_t(1));
    if (me.isMap())
        for (const char* key : kMeLists) tallyMeOrDbe(me.at(key));
    const Value& dbe = extended.at("com.deathweasel.bepinex.dynamicboneeditor").at(size_t(1));
    if (dbe.isMap()) tallyMeOrDbe(dbe.at("AccessoryDynamicBoneData"));
    const Value& pushup = extended.at("com.deathweasel.bepinex.pushup").at(size_t(1));
    if (pushup.isMap()) {
        static const char* kPushups[] = {"Pushup_TopData", "Pushup_BraData", "Pushup_BodyData"};
        for (const char* key : kPushups) tallyKeyed(pushup.at(key));
    }
    const Value& aci = extended.at("Additional_Card_Info").at(size_t(1));
    if (aci.isMap()) tallyKeyed(aci.at("CoordinateInfo"));
    if (counts.empty()) return 0;
    int best = -1;
    int64_t bestN = 0;
    for (const auto& kv : counts) if (kv.second > bestN) { bestN = kv.second; best = static_cast<int>(kv.first); }
    return best;
}

} // namespace

Card LoadCard(const std::string& path) {
    Card c;
    c.path = path;
    Bytes file = fileio::ReadFile(path);
    c.fileSize = file.size();
    size_t end = pngEnd(file);   // throws NotPng / TruncatedPng
    if (end >= file.size()) throw CardError(CardErrorKind::NoPayload, "PNG has no character data after the image");

    ByteReader r(file.data() + end, file.size() - end, false);
    std::string marker;
    try {
        c.productNo = r.i32();
        marker = r.dotnetString();
    } catch (const FormatError&) {
        throw CardError(CardErrorKind::UnknownMarker, "data after the PNG does not start like a card");
    }
    bool clothes = false;
    c.family = familyOf(marker, clothes);
    c.marker = marker;
    if (clothes) throw CardError(CardErrorKind::ClothesFile, "this is an outfit file (" + marker + "), not a character card");
    if (c.family == CardFamily::Other)
        throw CardError(CardErrorKind::UnknownMarker, "unrecognised card marker '" + marker + "'");

    try {
        c.formatVersion = r.dotnetString();
        int32_t faceLen = r.i32();
        if (faceLen < 0 || static_cast<size_t>(faceLen) > r.remaining()) throw FormatError("face image runs past the file");
        c.faceThumbnail = r.bytes(static_cast<size_t>(faceLen));
        int32_t hdrLen = r.i32();
        if (hdrLen < 0 || static_cast<size_t>(hdrLen) > r.remaining()) throw FormatError("block header runs past the file");
        Bytes hdrBytes = r.bytes(static_cast<size_t>(hdrLen));
        Value hdr = MsgPackParse(hdrBytes);
        int64_t dataSize = r.i64();
        size_t dataStart = r.pos();
        if (dataSize < 0 || static_cast<uint64_t>(dataSize) > r.remaining()) throw FormatError("block data shorter than the header says");
        for (const auto& bi : hdr.at("lstInfo").asArray()) {
            BlockInfo b;
            b.name = bi.at("name").asStr();
            b.version = bi.at("version").asStr();
            b.pos = static_cast<uint64_t>(bi.at("pos").asInt());
            b.size = static_cast<uint64_t>(bi.at("size").asInt());
            if (b.pos + b.size > static_cast<uint64_t>(dataSize)) throw FormatError("block '" + b.name + "' lies outside the data");
            c.blocks.push_back(b);
        }

        if (c.family == CardFamily::KoikatsuSunshine || c.family == CardFamily::EmotionCreators) {
            c.supported = false;
            c.supportNote = std::string("recognised as ") + FamilyName(c.family) + ", not converted yet (no matching game install to verify its asset bundles)";
            c.sha256 = Sha256Hex(file);
            return c;
        }

        // Blocks are parsed in place from the file bytes: copying each block
        // first held the whole card (and its embedded textures) twice more.
        auto blockReader = [&](const std::string& name) -> ByteReader {
            for (const auto& b : c.blocks)
                if (b.name == name)
                    return ByteReader(file.data() + end + dataStart + b.pos, static_cast<size_t>(b.size), false);
            return ByteReader();
        };
        auto hasBlock = [&](const std::string& name) {
            for (const auto& b : c.blocks) if (b.name == name && b.size > 0) return true;
            return false;
        };

        {
            ByteReader cr = blockReader("Custom");
            if (cr.size() == 0) throw FormatError("card has no Custom section");
            Value* parts[3] = {&c.face, &c.body, &c.hair};
            for (Value* p : parts) {
                int32_t n = cr.i32();
                if (n < 0 || static_cast<size_t>(n) > cr.remaining()) throw FormatError("Custom section runs past its data");
                ByteReader sub(cr.ptr(), static_cast<size_t>(n), false);
                *p = MsgPackRead(sub);
                cr.skip(static_cast<size_t>(n));
            }
        }

        {
            ByteReader coord = blockReader("Coordinate");
            if (coord.size() == 0) throw FormatError("card has no Coordinate section");
            Value arr = MsgPackRead(coord);
            for (const auto& el : arr.asArray()) {
                Outfit o;
                decodeCoordinate(el.asBin(), o);
                c.outfits.push_back(std::move(o));
            }
        }

        if (hasBlock("Parameter")) { ByteReader r = blockReader("Parameter"); c.parameter = MsgPackRead(r); }
        if (hasBlock("Status")) { ByteReader r = blockReader("Status"); c.status = MsgPackRead(r); }

        if (hasBlock("KKEx")) {
            ByteReader r = blockReader("KKEx");
            c.extended = MsgPackRead(r);
            const Value& uar = c.extended.at("com.bepis.sideloader.universalautoresolver");
            for (const auto& raw : uar.at(size_t(1)).at("info").asArray()) {
                Value row = MsgPackParse(raw.asBin());
                ResolveRow rr;
                rr.modId = row.at("ModID").asStr();
                rr.slot = row.at("Slot").asInt();
                rr.localSlot = row.at("LocalSlot").asInt();
                rr.property = row.at("Property").asStr();
                rr.category = static_cast<int>(row.at("CategoryNo").asInt());
                c.rows.push_back(std::move(rr));
            }
            appendMoreAccessories(c);
        }
    } catch (const CardError&) {
        throw;
    } catch (const FormatError& e) {
        throw CardError(CardErrorKind::CorruptPayload, std::string("damaged card data: ") + e.what());
    }

    c.supported = true;
    c.sha256 = Sha256Hex(file);
    return c;
}

// The accessory index encoded in a resolver property ("accessory8...."),
// or -1 when there is none.
int accessoryIndexOfProperty(const std::string& property) {
    const std::string key = "accessory";
    size_t at = property.find(key);
    if (at == std::string::npos) return -1;
    at += key.size();
    if (at >= property.size() || !std::isdigit(static_cast<unsigned char>(property[at]))) return -1;
    return std::atoi(property.c_str() + at);
}

std::string withOutfitPrefix(const std::string& property, int outfit) {
    if (property.rfind("outfit", 0) == 0) return property;
    return "outfit" + std::to_string(outfit) + "." + property;
}

// --- syncing the card's extended universalautoresolver payload with the merged rows ---
// LoadCard seeds card.rows from KKEx but never writes it back: after a merge, a
// re-export produced an outfit file with no rows even though rows existed.
// Here the extended UAR bin is brought in line with card.rows: rows for dstOutfit
// are dropped (clothes rows always, accessory rows the merge replaced) or, when the
// coordinate is replaced wholesale, dropped entirely; untouched rows keep their raw
// bins verbatim (their extra keys survive) and the rebased rows the merge added are
// appended.
void syncExtendedResolvedRows(Card& card, int dstOutfit, const std::vector<ResolveRow>& appended,
                              const std::set<int>* replacedAcc /*nullptr: drop ALL rows of the outfit*/) {
    const std::string prefix = "outfit" + std::to_string(dstOutfit) + ".";
    if (!card.extended.isMap()) card.extended = Value(Value::Map{});
    bool hasUar = false;
    int64_t uarVersion = 0;
    Value::Map uarPayload;
    for (const auto& kv : card.extended.map()) {
        if (!kv.first.isStr() || kv.first.asStr() != "com.bepis.sideloader.universalautoresolver") continue;
        hasUar = kv.second.isArray() && kv.second.size() >= 2;
        if (hasUar) {
            uarVersion = kv.second.at(size_t(0)).asInt(0);
            if (kv.second.at(size_t(1)).isMap()) uarPayload = kv.second.at(size_t(1)).asMap();
        }
        break;
    }
    Value::Array outInfo;
    if (hasUar) {
        const Value* info = nullptr;
        for (const auto& kv : uarPayload)
            if (kv.first.isStr() && kv.first.asStr() == "info") { info = &kv.second; break; }
        if (info != nullptr && info->isArray()) {
            for (const auto& raw : info->asArray()) {
                if (!raw.isBin()) continue;
                Value row;
                try { row = MsgPackParse(raw.asBin()); } catch (const FormatError&) { outInfo.emplace_back(raw); continue; }   // unparseable: keep verbatim
                const std::string prop = row.at("Property").asStr();
                bool drop = false;
                if (prop.rfind(prefix, 0) == 0) {
                    if (prop.find("ChaFileClothes.") != std::string::npos) drop = true;
                    else if (prop.find("ChaFileAccessory.") != std::string::npos &&
                             (!replacedAcc || replacedAcc->count(accessoryIndexOfProperty(prop)) != 0)) drop = true;
                }
                if (!drop) outInfo.emplace_back(raw);   // verbatim: extra keys, other coordinates
            }
        }
    } else {
        // The target has no UAR payload: seed one from the merged rows so a re-export carries them.
        // CardBridge's row model round-trips exactly these five keys (LoadCard drops the rest), so nothing
        // is lost that this path can see.
        if (card.rows.empty()) return;
    }
    for (const auto& rr : appended) {
        Value::Map row;
        row.emplace_back(Value(std::string("ModID")), Value(rr.modId));
        row.emplace_back(Value(std::string("Slot")), Value(rr.slot));
        row.emplace_back(Value(std::string("LocalSlot")), Value(rr.localSlot));
        row.emplace_back(Value(std::string("Property")), Value(rr.property));
        row.emplace_back(Value(std::string("CategoryNo")), Value(static_cast<int64_t>(rr.category)));
        outInfo.emplace_back(Value(MsgPackPack(Value(std::move(row)))));
    }
    // Nothing to write back when the UAR was absent, no rows were appended and there was nothing to merge either.
    if (!hasUar && card.rows.empty()) return;
    bool found = false;
    for (auto& kv : card.extended.map()) {
        if (!kv.first.isStr() || kv.first.asStr() != "com.bepis.sideloader.universalautoresolver") continue;
        Value::Map payload2 = std::move(uarPayload);
        bool keptInfo = false;
        for (auto& pkv : payload2)
            if (pkv.first.isStr() && pkv.first.asStr() == "info") { pkv.second = Value(std::move(outInfo)); keptInfo = true; }
        if (!keptInfo) payload2.emplace_back(Value(std::string("info")), Value(std::move(outInfo)));
        kv.second = Value(Value::Array{Value(uarVersion), Value(std::move(payload2))});
        found = true;
        break;
    }
    if (!found) {
        Value::Map payload2;
        payload2.emplace_back(Value(std::string("info")), Value(std::move(outInfo)));
        card.extended.map().emplace_back(Value(std::string("com.bepis.sideloader.universalautoresolver")),
                                         Value(Value::Array{Value(uarVersion), Value(std::move(payload2))}));
    }
}

// --- merging the outfit file's plugin payloads (ABMX, MaterialEditor) ---
// The game's coordinate load applies the file's bone mods and material edits
// for the outfit; without them the outfit shows its prefab look and fits
// without its skirt scaling (the Nurse+skirt test: the white shirt overtook
// the skirt at the waist and the stockings baked opaque).

// One inner MaterialEditor list (a bin of an array of maps) with each row's
// CoordinateIndex remapped 0->target and accessory slots remapped through
// the merge, plus texture ids remapped through texRemap. `srcCoordinate` is
// the coordinate of the source payload the rows come from (0 for an outfit
// file; the card's coordinate number for a synthesised source). Returns the
// encoded bin, or empty when the source list is absent.
Bytes mergeMeList(const Value& clothingPayload, const char* key, int target,
                  const std::vector<int>& clothToMerged, const std::map<int64_t, int64_t>& texRemap,
                  int srcCoordinate = 0) {
    const Value& srcBin = clothingPayload.at(key);
    if (!srcBin.isBin() || srcBin.asBin().empty()) return Bytes{};
    Value list;
    try { list = MsgPackParse(srcBin.asBin()); } catch (const FormatError&) { return Bytes{}; }
    if (!list.isArray()) return Bytes{};
    Value::Array rows;
    for (const auto& row : list.asArray()) {
        if (!row.isMap()) continue;
        const int64_t coord = row.at("CoordinateIndex").asInt(0);
        // srcCoordinate >= 0: only that source coordinate's rows travel (a card coordinates).
        // srcCoordinate < 0: the source is one coordinate (an outfit file) — all its rows belong to it.
        if (srcCoordinate >= 0 && coord != static_cast<int64_t>(srcCoordinate)) continue;
        Value nr = row;
        auto setKeyInt = [&](const char* k, int64_t v) {
            for (auto& kv : nr.map()) if (kv.first.isStr() && kv.first.asStr() == k) { kv.second = Value(v); return; }
            nr.map().emplace_back(Value(std::string(k)), Value(v));
        };
        setKeyInt("CoordinateIndex", target);
        const int64_t objectType = row.at("ObjectType").asInt(-1);
        const int64_t slot = row.at("Slot").asInt(0);
        if (objectType == 2 && slot >= 0 && static_cast<size_t>(slot) < clothToMerged.size()) {
            const int merged = clothToMerged[static_cast<size_t>(slot)];
            if (merged < 0) continue;   // the outfit slot was empty: nothing to edit
            setKeyInt("Slot", merged);
        }
        if (std::string(key) == "MaterialTexturePropertyList" && row.has("TexID")) {
            const Value& t = row.at("TexID");
            if (t.isInt()) {
                auto it = texRemap.find(t.asInt());
                if (it != texRemap.end()) setKeyInt("TexID", it->second);
            }
        }
        rows.emplace_back(std::move(nr));
    }
    if (rows.empty()) return Bytes{};
    return MsgPackPack(Value(std::move(rows)));
}

// Drops the character's rows for the replaced objects: the outfit's clothes
// replaced the target coordinate's wholesale, and the accessories the outfit
// replaced took with them whatever MaterialEditor paint was on them (keeping
// it would repaint the new garments with the old ones' textures). Rows for
// other coordinates and surviving accessories stay. `dropAllAccRows` drops
// every accessory row of the target coordinate regardless of `replacedAcc`
// (used by a wholesale coordinate import). Returns the filtered list as
// bytes, or an empty vector when the value is not a bin array.
Bytes dropReplacedMeRows(const Value& listBin, int outfit, const std::set<int>& replacedAcc, bool* droppedAny = nullptr, bool dropAllAccRows = false) {
    if (droppedAny) *droppedAny = false;
    if (!listBin.isBin() || listBin.asBin().empty()) return Bytes{};
    Value list;
    try { list = MsgPackParse(listBin.asBin()); } catch (const FormatError&) { return Bytes{}; }
    if (!list.isArray()) return Bytes{};
    Value::Array kept;
    bool dropped = false;
    for (const auto& row : list.asArray()) {
        if (!row.isMap()) { kept.emplace_back(row); continue; }
        const int64_t coord = row.at("CoordinateIndex").asInt(-1);
        if (coord != static_cast<int64_t>(outfit)) { kept.emplace_back(row); continue; }
        const int64_t objectType = row.at("ObjectType").asInt(-1);
        const int64_t slot = row.at("Slot").asInt(0);
        // Clothing (type 1) went wholesale; accessories (type 2) went where the outfit replaced them.
        if (objectType == 1 || (objectType == 2 && (dropAllAccRows || replacedAcc.count(static_cast<int>(slot))))) { dropped = true; continue; }
        kept.emplace_back(row);
    }
    if (droppedAny) *droppedAny = dropped;
    return dropped ? MsgPackPack(Value(std::move(kept))) : listBin.asBin();
}

// Appends decoded clothing rows to a character list (a bin of an array) and
// re-encodes it. When the character list is not a bin array, the clothing
// rows become it (handled by the caller seeding the character value).
Value appendMeRows(const Value& charBin, const Bytes& clothingRows) {
    if (clothingRows.empty()) return charBin;
    if (!charBin.isBin()) return charBin;
    Value list;
    try { list = MsgPackParse(charBin.asBin()); } catch (const FormatError&) { return charBin; }
    Value clothing;
    try { clothing = MsgPackParse(clothingRows); } catch (const FormatError&) { return charBin; }
    if (!list.isArray() || !clothing.isArray()) return charBin;
    Value::Array merged = list.asArray();
    for (const auto& e : clothing.asArray()) merged.emplace_back(e);
    return Value(MsgPackPack(Value(std::move(merged))));
}

// --- shared source-plugin merging (ABMX / MaterialEditor / KCOX) ---------------
// One implementation serves the clothing-file path (source coordinate 0) and a
// Coordinate import (source coordinate N). The game's coordinate load applies
// the source's bone mods and material edits for the coordinate; without them
// an imported outfit shows its prefab look and fits without its skirt scaling.

// `srcCoordinate` is the coordinate of the source table the merge reads (0 for
// a real outfit file, the card's coordinate number for a synthesised source).
// `dstOutfit` is the outfit of the target that is being replaced, and
// `dstOutfits` its total outfit count (an ABMX mod list longer than 1 is
// coordinate-indexed). `coordinateMode` selects the rebase for a Coordinate
// import: the source coordinate's effective entry replaces only the target
// coordinate's slot in the shared bone's list (other coordinates of the
// target keep theirs, and a target without a KKEx block gets one created).
// The clothing-file path clears the destination coordinate-specific slot and
// then lets the source win the whole shared bone, as before (except it no
// longer keeps the target's stale destination-coordinate-specific mods).
//
// Rollback: CARDBRIDGE_ABMX_LEGACY_MERGE=1 restores the pre-fix behaviour exactly
// (no destination-slot clear anywhere, and the v2-dictionary ABMX format is ignored),
// reproducing byte-identical output. Both paths are kept separately so the legacy
// behaviour can never drift from what was shipped.
static bool abmxLegacyMerge() { return std::getenv("CARDBRIDGE_ABMX_LEGACY_MERGE") != nullptr; }

// Writes the table back into the target's KKEx block (creating it when missing) and appends `note`.
static void writeAbmxBack(Card& card, const AbmxTable& table, std::vector<std::string>& notes, const char* note) {
    Bytes bin = PackAbmxBin(table);
    const auto setExtended = [&](const char* key, const Value& entry) {
        bool found = false;
        if (card.extended.isMap())
            for (auto& kv : card.extended.map())
                if (kv.first.isStr() && kv.first.asStr() == key) { kv.second = entry; found = true; }
        if (!found && card.extended.isMap()) card.extended.map().emplace_back(Value(std::string(key)), entry);
    };
    const Value& existing = card.extended.at("KKABMPlugin.ABMData");
    Value::Map payload;
    if (existing.isArray() && existing.at(size_t(1)).isMap()) payload = existing.at(size_t(1)).asMap();
    bool hasBin = false;
    for (auto& kv : payload)
        if (kv.first.isStr() && kv.first.asStr() == "boneData") { kv.second = Value(std::move(bin)); hasBin = true; }
    if (!hasBin) payload.emplace_back(Value(std::string("boneData")), Value(std::move(bin)));
    const int64_t version = existing.isArray() ? existing.at(size_t(0)).asInt(3) : 3;
    Value::Array entry{Value(version), Value(std::move(payload))};
    setExtended("KKABMPlugin.ABMData", Value(std::move(entry)));
    notes.emplace_back(note);
}

// The coordinate-import rebase: the source coordinate's effective modifier replaces only the target
// coordinate's slot in the shared bone's list. Shared by the legacy and the current path.
static void rebaseCoordinateSlot(AbmxTable& charAbmx, const AbmxTable& srcAbmx, int srcCoordinate, int dstOutfit, int dstOutfits) {
    const int m = dstOutfits > 0 && dstOutfit >= 0 && dstOutfit < dstOutfits ? dstOutfit : 0;
    for (const auto& kv : srcAbmx.bones) {
        const AbmxMod* srcMod = srcAbmx.find(kv.first, srcCoordinate);
        if (!srcMod) continue;   // this source has no effective mod for that bone on this coordinate
        if (srcMod->empty()) continue;
        auto it = charAbmx.bones.find(kv.first);
        if (it == charAbmx.bones.end()) {
            // The target had nothing for the bone: a full-length list with identity entries everywhere except the
            // replaced coordinate, so the source's mod applies only to that outfit (as it did on the source).
            if (dstOutfits <= 0 || m >= dstOutfits) { charAbmx.bones[kv.first] = {*srcMod}; continue; }
            std::vector<AbmxMod> list(static_cast<size_t>(dstOutfits), AbmxMod{});
            list[static_cast<size_t>(m)] = *srcMod;
            charAbmx.bones[kv.first] = std::move(list);
            continue;
        }
        auto& list = it->second;
        if (list.size() == 1) {
            // A blanket mod shared by every coordinate: expand it, replace only the target coordinate's entry.
            if (dstOutfits > 1 && m < dstOutfits) {
                const AbmxMod blanket = list[0];
                std::vector<AbmxMod> full(static_cast<size_t>(dstOutfits), blanket);
                full[static_cast<size_t>(m)] = *srcMod;
                list = std::move(full);
            } else {
                list[0] = *srcMod;
            }
            continue;
        }
        if (list.size() <= static_cast<size_t>(m)) list.resize(static_cast<size_t>(m) + 1, AbmxMod{});
        list[static_cast<size_t>(m)] = *srcMod;   // other coordinates of the target keep their entries
    }
}

// Pre-fix behaviour, frozen verbatim behind CARDBRIDGE_ABMX_LEGACY_MERGE=1. Do not edit.
static void mergeAbmxPayloadsLegacy(Card& card, const Value& srcExtended, std::vector<std::string>& notes,
                                    int srcCoordinate, int dstOutfit, int dstOutfits, bool coordinateMode) {
    if (!srcExtended.isMap()) return;
    AbmxTable charAbmx = LoadAbmx(card.extended);
    AbmxTable srcAbmx = LoadAbmx(srcExtended);
    if (srcAbmx.empty()) return;
    // A KKEx block is created when the target has none (vanilla cards carry no KKEx until a plugin writes one).
    if (!card.extended.isMap()) card.extended = Value(Value::Map{});
    if (!coordinateMode) {
        for (const auto& kv : srcAbmx.bones) charAbmx.bones[kv.first] = kv.second;   // the source wins the whole shared bone
    } else {
        rebaseCoordinateSlot(charAbmx, srcAbmx, srcCoordinate, dstOutfit, dstOutfits);
    }
    writeAbmxBack(card, charAbmx, notes, "ABMX: source's bone mods merged (skirt fit)");
}

// The ABMX import merge, matching the game's coordinate load (BoneController.OnCoordinateBeingLoaded):
// every coordinate-specific modifier already on the character gets the destination coordinate's slot
// cleared — whether or not the source carries ABMX data — and the source's modifiers replace that same
// slot. Other coordinates keep their entries. A regular clothing file keeps the original rule that the
// source wins the whole shared bone, applied to the destination outfit as usual.
void mergeAbmxPayloads(Card& card, const Value& srcExtended, std::vector<std::string>& notes,
                       int srcCoordinate, int dstOutfit, int dstOutfits, bool coordinateMode) {
    if (abmxLegacyMerge()) { mergeAbmxPayloadsLegacy(card, srcExtended, notes, srcCoordinate, dstOutfit, dstOutfits, coordinateMode); return; }
    AbmxTable charAbmx = LoadAbmx(card.extended);
    const AbmxTable srcAbmx = srcExtended.isMap() ? LoadAbmx(srcExtended) : AbmxTable{};
    const int m = dstOutfits > 0 && dstOutfit >= 0 && dstOutfit < dstOutfits ? dstOutfit : 0;
    bool cleared = false;
    for (auto& kv : charAbmx.bones) {
        auto& list = kv.second;
        if (list.size() <= 1 || m >= static_cast<int>(list.size())) continue;
        AbmxMod& slot = list[static_cast<size_t>(m)];
        if (slot.empty()) continue;   // already the empty modifier: nothing to reset
        slot = AbmxMod{};             // BoneModifierData.Clear(): scale 1, rotation 0, position 0, length 1
        cleared = true;
    }
    if (!cleared && srcAbmx.empty()) return;   // nothing to clear and nothing to merge: the target is left untouched
    // A KKEx block is created when the target has none (vanilla cards carry no KKEx until a plugin writes one):
    // the destination-slot clear and the merged source mods must be stored or they are silently lost.
    if (!card.extended.isMap()) card.extended = Value(Value::Map{});
    if (!coordinateMode) {
        for (const auto& kv : srcAbmx.bones) charAbmx.bones[kv.first] = kv.second;   // the source wins the whole shared bone
    } else {
        rebaseCoordinateSlot(charAbmx, srcAbmx, srcCoordinate, dstOutfit, dstOutfits);
    }
    writeAbmxBack(card, charAbmx, notes,
                  srcAbmx.empty() ? "ABMX: outfit's coordinate-specific bone mods cleared"
                                  : "ABMX: source's bone mods merged (skirt fit)");
}

// `clothToMerged[i]` is the merged slot holding source accessory slot i
// (-1 when the source slot was empty - for files, ApplyClothing computes this;
// for a Coordinate import it is the identity with empties marked -1).
// `replacedAcc` names the merged slots whose contents were dropped for the
// target's old accessories; ignored when `dropAllAccRows` drops the target
// coordinate's accessory rows wholesale. A source without MaterialEditor data
// leaves the target untouched on the file path (as before); on the
// Coordinate-import path the stale paint of the replaced coordinate is still
// dropped.
void mergeMaterialEditorPayloads(Card& card, const Value& srcExtended, int dstOutfit,
                                 const std::vector<int>& clothToMerged, int srcCoordinate,
                                 const std::set<int>& replacedAcc, bool dropAllAccRows,
                                 std::vector<std::string>& notes) {
    const Value& srcMe = srcExtended.isMap() ? srcExtended.at("com.deathweasel.bepinex.materialeditor").at(size_t(1)) : Value::nil();
    if (!srcMe.isMap() && !dropAllAccRows) return;
    const Value& charMe = card.extended.at("com.deathweasel.bepinex.materialeditor").at(size_t(1));
    // Texture ids: fresh ids for collisions so the source's images never
    // overwrite the character's.
    std::map<int64_t, int64_t> texRemap;
    std::map<int64_t, Bytes> charTex;
    if (charMe.isMap() && charMe.at("TextureDictionary").isBin()) {
        try {
            Value dict = MsgPackParse(charMe.at("TextureDictionary").asBin());
            if (dict.isMap())
                for (const auto& kv : dict.asMap())
                    if (kv.first.isInt() && kv.second.isBin()) charTex[kv.first.asInt()] = kv.second.asBin();
        } catch (const FormatError&) {}
    }
    std::map<int64_t, Bytes> srcTex;
    if (srcMe.isMap()) {
        try {
            Value dict = MsgPackParse(srcMe.at("TextureDictionary").asBin());
            if (dict.isMap())
                for (const auto& kv : dict.asMap())
                    if (kv.first.isInt() && kv.second.isBin()) srcTex[kv.first.asInt()] = kv.second.asBin();
        } catch (const FormatError&) {}
    }
    int64_t nextId = 0;
    for (const auto& kv : charTex) nextId = std::max(nextId, kv.first + 1);
    for (const auto& kv : srcTex) {
        auto it = charTex.find(kv.first);
        if (it == charTex.end()) { charTex[kv.first] = kv.second; continue; }
        if (it->second == kv.second) continue;   // the same image: share
        const int64_t fresh = nextId++;
        texRemap[kv.first] = fresh;
        charTex[fresh] = kv.second;
    }
    Value::Map charPayload;
    if (charMe.isMap()) charPayload = charMe.asMap();
    bool anyDropped = false;
    auto setPayloadBin = [&](const char* key, const Bytes& bin) {
        bool found = false;
        for (auto& kv : charPayload)
            if (kv.first.isStr() && kv.first.asStr() == key) { kv.second = Value(bin); found = true; }
        if (!found) charPayload.emplace_back(Value(std::string(key)), Value(bin));
    };
    if (srcMe.isMap()) {
        Value::Map dict;
        for (const auto& kv : charTex) dict.emplace_back(Value(kv.first), Value(kv.second));
        setPayloadBin("TextureDictionary", MsgPackPack(Value(std::move(dict))));
    }
    for (const char* key : kMeLists) {
        const Bytes rows = srcMe.isMap() ? mergeMeList(srcMe, key, dstOutfit, clothToMerged, texRemap, srcCoordinate) : Bytes{};
        const Value& cur = charMe.isMap() ? charMe.at(key) : Value::nil();
        // The coordinate's own paint belonged to the outfit that was replaced:
        // drop it before merging the source's, or the new garments are repainted with the old ones' textures.
        bool paintDropped = false;
        const Bytes kept = dropReplacedMeRows(cur, dstOutfit, replacedAcc, &paintDropped, dropAllAccRows);
        const Value keptValue = kept.empty() ? cur : Value(std::move(kept));
        if (rows.empty()) {
            if (paintDropped && keptValue.isBin()) { anyDropped = true; setPayloadBin(key, keptValue.asBin()); }
            continue;
        }
        anyDropped = anyDropped || paintDropped;
        Value mergedRows;
        if (keptValue.isBin() && !keptValue.asBin().empty()) mergedRows = appendMeRows(keptValue, rows);
        else mergedRows = Value(rows);
        setPayloadBin(key, mergedRows.isBin() ? mergedRows.asBin() : MsgPackPack(mergedRows));
    }
    if (!srcMe.isMap() && !anyDropped) return;   // nothing to write back
    bool found = false;
    if (card.extended.isMap())
        for (auto& kv : card.extended.map())
            if (kv.first.isStr() && kv.first.asStr() == "com.deathweasel.bepinex.materialeditor") {
                const int64_t version = kv.second.isArray() ? kv.second.at(size_t(0)).asInt(0) : 0;
                kv.second = Value(Value::Array{Value(version), Value(std::move(charPayload))});
                found = true;
            }
    if (!found && card.extended.isMap()) {
        Value::Array entry{Value(int64_t{0}), Value(std::move(charPayload))};
        card.extended.map().emplace_back(Value(std::string("com.deathweasel.bepinex.materialeditor")), Value(std::move(entry)));
    }
    if (srcMe.isMap()) notes.emplace_back("MaterialEditor: source's edits merged");
    else notes.emplace_back("MaterialEditor: replaced outfit's edits cleared");
}

// The KoiClothesOverlayX garment paint for one coordinate of a source. A
// clothing file writes the overlays as {garment -> [png, replace]} with no
// coordinate level (read only for coordinate 0 - the one coordinate a file
// carries); a character card stores {coordinate -> {...}}.
std::map<std::string, std::pair<Bytes, bool>> parseCoordinateOverlays(const Value& ext, int coordinateIndex) {
    std::map<std::string, std::pair<Bytes, bool>> out;
    const Value& kcox = ext.at("KCOX").at(size_t(1));
    if (!kcox.isMap() || !kcox.at("Overlays").isBin()) return out;
    Value overlays;
    try { overlays = MsgPackParse(kcox.at("Overlays").asBin()); } catch (const FormatError&) { return out; }
    if (!overlays.isMap()) return out;
    for (const auto& kv : overlays.asMap()) {
        if (kv.first.isInt()) {
            if (coordinateIndex >= 0 && kv.first.asInt() != coordinateIndex) continue;   // card mode: this coordinate only
            if (!kv.second.isMap()) continue;
            for (const auto& g : kv.second.asMap())
                if (g.first.isStr() && g.second.at(size_t(0)).isBin() && g.second.at(size_t(0)).size() > 0)
                    out[g.first.asStr()] = {g.second.at(size_t(0)).asBin(), g.second.at(size_t(1)).asBool()};
            continue;
        }
        // Flat shape (a single-coordinate file): every overlay belongs to the one coordinate,
        // whatever index the author saved it under.
        if (kv.first.isStr() && kv.second.isArray() && kv.second.size() >= 2 &&
            kv.second.at(size_t(0)).isBin() && kv.second.at(size_t(0)).size() > 0) {
            out[kv.first.asStr()] = {kv.second.at(size_t(0)).asBin(), kv.second.at(size_t(1)).asBool()};
        }
    }
    return out;
}

// The target coordinate's garment paint is dropped (it belonged to the outfit
// that was replaced), all other coordinates keep theirs, and the source's
// paint is stored under the target outfit's index.
void mergeKcoxPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                       std::vector<std::string>& notes) {
    const auto srcOverlays = parseCoordinateOverlays(srcExtended, srcCoordinate);
    const bool srcHasKcox = srcExtended.at("KCOX").at(size_t(1)).isMap() && srcExtended.at("KCOX").at(size_t(1)).at("Overlays").isBin();
    Value::Map outer;
    Value::Map charPayload;
    int64_t kcoxVersion = 1;
    bool charHasKcox = false;
    if (card.extended.isMap()) {
        const Value& charKcox = card.extended.at("KCOX").at(size_t(1));
        if (charKcox.isMap() && charKcox.at("Overlays").isBin()) {
            charHasKcox = true;
            charPayload = charKcox.asMap();
            const Value& entry = card.extended.at("KCOX");
            if (entry.isArray()) kcoxVersion = entry.at(size_t(0)).asInt(1);
            Value prev;
            try { prev = MsgPackParse(charKcox.at("Overlays").asBin()); } catch (const FormatError&) {}
            if (prev.isMap()) {
                for (const auto& kv : prev.asMap()) {
                    // Character form is {coordinate: {...}}; a {garment: ...}
                    // form has no coordinate to keep, so it is dropped with the target.
                    if (kv.first.isInt() && kv.first.asInt() != static_cast<int64_t>(dstOutfit)) outer.emplace_back(kv.first, kv.second);
                }
            }
        }
    }
    if (!srcOverlays.empty()) {
        Value::Map garments;
        for (const auto& kv : srcOverlays) {
            Value::Array ga{Value(kv.second.first), Value(kv.second.second)};
            garments.emplace_back(Value(kv.first), Value(std::move(ga)));
        }
        outer.emplace_back(Value(static_cast<int64_t>(dstOutfit)), Value(std::move(garments)));
    }
    if (outer.empty() && !charHasKcox && srcOverlays.empty()) return;
    Bytes newBin = MsgPackPack(Value(std::move(outer)));
    bool found = false;
    for (auto& kv : charPayload)
        if (kv.first.isStr() && kv.first.asStr() == "Overlays") { kv.second = Value(newBin); found = true; }
    if (!found) charPayload.emplace_back(Value(std::string("Overlays")), Value(newBin));
    // When the source has no overlays, storing the emptied blob
    // clears the target coordinate (no old paint left over).
    bool entryFound = false;
    if (card.extended.isMap())
        for (auto& kv : card.extended.map())
            if (kv.first.isStr() && kv.first.asStr() == "KCOX") {
                kv.second = Value(Value::Array{Value(kcoxVersion), Value(std::move(charPayload))});
                entryFound = true;
            }
    if (!entryFound && card.extended.isMap() && (!srcOverlays.empty() || charHasKcox)) {
        Value::Array entryV{Value(kcoxVersion), Value(std::move(charPayload))};
        card.extended.map().emplace_back(Value(std::string("KCOX")), Value(std::move(entryV)));
    }
    if (!srcOverlays.empty())
        notes.emplace_back("KoiClothesOverlayX: source's garment paint merged");
    else if (srcHasKcox)
        notes.emplace_back("KoiClothesOverlayX: source's garment paint cleared (no overlays in the source)");
    else if (charHasKcox)
        notes.emplace_back("KoiClothesOverlayX: previous outfit's garment paint cleared (the source has no overlays)");
}

// --- source KKEx coordinate-scoped payloads ------------------------------
// Every helper below reads the source's KKEx payload for one coordinate and
// rebases it to the target outfit. The Coordinate-import path replaces the
// target coordinate wholesale; the clothing-file path merges around kept
// hair, remapping accessory slots through `clothToMerged`. Both share the
// same code so the two modes stay consistent.

// Writes `value` as `innerKey` of the [version, payload] plugin entry `key`,
// creating the entry (and an empty KKEx map) when absent. Version defaults to
// the existing entry's version, or `defaultVersion` for a new entry.
void writePluginPayload(Card& card, const char* key, const char* innerKey, const Value& value, int64_t defaultVersion) {
    if (!card.extended.isMap()) card.extended = Value(Value::Map{});
    bool found = false;
    for (auto& kv : card.extended.map()) {
        if (!kv.first.isStr() || kv.first.asStr() != key) continue;
        const int64_t version = kv.second.isArray() && kv.second.size() >= 2 ? kv.second.at(size_t(0)).asInt(defaultVersion) : defaultVersion;
        Value::Map payload;
        if (kv.second.isArray() && kv.second.size() >= 2 && kv.second.at(size_t(1)).isMap())
            payload = kv.second.at(size_t(1)).asMap();
        bool kfound = false;
        for (auto& pkv : payload)
            if (pkv.first.isStr() && pkv.first.asStr() == innerKey) { pkv.second = value; kfound = true; }
        if (!kfound) payload.emplace_back(Value(std::string(innerKey)), value);
        kv.second = Value(Value::Array{Value(version), Value(std::move(payload))});
        found = true;
        break;
    }
    if (!found) {
        Value::Map payload;
        payload.emplace_back(Value(std::string(innerKey)), value);
        card.extended.map().emplace_back(Value(std::string(key)),
                                         Value(Value::Array{Value(defaultVersion), Value(std::move(payload))}));
    }
}

// Accessory_States (KKEx "Accessory_States"): the full per-slot visibility /
// state-machine data for accessories, per coordinate. CoordinateData is a map
// of coordIndex -> {_slotinfo, _names, _clothnotdata, _forceclothnotupdate}.
// _slotinfo is slotIndex -> Slotdata {_binding (0-8 = ClothesKind, -1 unbound), _state (visible ranges
// of the bound state), _shoetype, _parented}; _names is slotIndex -> NameData.
// The clothing merge maps source slots through clothToMerged (the hair that
// stays on the target keeps its accessory-state entries; slots that did not
// travel are dropped); the Coordinate import replaces the whole coordinate's
// entry.
void mergeAccessoryStatesPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                                  const std::vector<int>& clothToMerged, bool coordinateImport,
                                  std::vector<std::string>& notes) {
    if (!srcExtended.isMap()) return;
    const Value& payload = srcExtended.at("Accessory_States").at(size_t(1));
    if (!payload.isMap()) return;
    const Value& bin = payload.at("CoordinateData");
    if (!bin.isBin() || bin.asBin().empty()) return;
    std::map<int64_t, Value> srcCoords;
    Value flatEntry;
    bool flat = false;
    try {
        Value parsed = MsgPackParse(bin.asBin());
        if (parsed.isMap()) {
            // flat: the one coordinate's raw entry (_slotinfo at top level); nested: {coordIdx: {...}}
            if (parsed.at("_slotinfo").isMap()) {
                flat = true;
                flatEntry = parsed;
            } else {
                for (const auto& kv : parsed.asMap())
                    if (kv.first.isInt()) srcCoords.emplace(kv.first.asInt(), kv.second);
            }
        }
    } catch (const FormatError&) {}
    Value srcEntryV;
    if (flat) srcEntryV = flatEntry;
    else {
        const Value* sel = selectCoordinateEntry(srcCoords, srcCoordinate);
        if (!sel || !sel->isMap()) return;   // nothing stored for this coordinate
        srcEntryV = *sel;
    }
    const Value& srcEntry = srcEntryV;

    std::map<int64_t, Value> dstCoords;
    {
        const Value& dpayload = card.extended.at("Accessory_States").at(size_t(1));
        if (dpayload.isMap()) {
            const Value& dbin = dpayload.at("CoordinateData");
            if (dbin.isBin() && !dbin.asBin().empty()) {
                try {
                    Value dparsed = MsgPackParse(dbin.asBin());
                    if (dparsed.isMap())
                        for (const auto& kv : dparsed.asMap())
                            if (kv.first.isInt()) dstCoords.emplace(kv.first.asInt(), kv.second);
                } catch (const FormatError&) {}
            }
        }
    }
    const Value& srcSlotinfo = srcEntry.at("_slotinfo");
    const Value& srcNames = srcEntry.at("_names");
    const Value& srcClothNot = srcEntry.at("_clothnotdata");
    const Value& srcForce = srcEntry.at("_forceclothnotupdate");
    Value::Map entry;
    if (coordinateImport) {
        if (srcSlotinfo.isMap()) entry.emplace_back(Value(std::string("_slotinfo")), srcSlotinfo);
        if (srcNames.isMap()) entry.emplace_back(Value(std::string("_names")), srcNames);
        if (srcClothNot.isArray()) entry.emplace_back(Value(std::string("_clothnotdata")), srcClothNot);
        if (srcForce.isBool()) entry.emplace_back(Value(std::string("_forceclothnotupdate")), srcForce);
    } else {
        // Keep the target's surviving entries (slots not re-mapped to a travelled source slot), then rewrite the travelled ones.
        Value::Map mergedSlotinfo, mergedNames;
        const Value& dstSlotinfo = dstCoords.count(dstOutfit) && dstCoords[dstOutfit].isMap() ? dstCoords[dstOutfit].at("_slotinfo") : Value::nil();
        const Value& dstNames = dstCoords.count(dstOutfit) && dstCoords[dstOutfit].isMap() ? dstCoords[dstOutfit].at("_names") : Value::nil();
        std::set<int> used;
        if (srcSlotinfo.isMap()) {
            for (const auto& kv : srcSlotinfo.asMap()) {
                if (!kv.first.isInt()) continue;
                const int s = static_cast<int>(kv.first.asInt());
                const int m = s >= 0 && static_cast<size_t>(s) < clothToMerged.size() ? clothToMerged[static_cast<size_t>(s)] : -1;
                if (m < 0) continue;
                used.insert(m);
            }
        }
        if (dstSlotinfo.isMap()) for (const auto& kv : dstSlotinfo.asMap()) if (kv.first.isInt() && !used.count(static_cast<int>(kv.first.asInt()))) mergedSlotinfo.emplace_back(kv.first, kv.second);
        if (dstNames.isMap()) for (const auto& kv : dstNames.asMap()) if (kv.first.isInt() && !used.count(static_cast<int>(kv.first.asInt()))) mergedNames.emplace_back(kv.first, kv.second);
        auto remapMap = [&](const Value& src, Value::Map& into) {
            if (!src.isMap()) return;
            for (const auto& kv : src.asMap()) {
                if (!kv.first.isInt()) continue;
                const int s = static_cast<int>(kv.first.asInt());
                const int m = s >= 0 && static_cast<size_t>(s) < clothToMerged.size() ? clothToMerged[static_cast<size_t>(s)] : -1;
                if (m < 0) continue;
                bool found = false;
                for (auto& ekv : into)
                    if (ekv.first.isInt() && static_cast<int>(ekv.first.asInt()) == m) { ekv.second = kv.second; found = true; break; }
                if (!found) into.emplace_back(Value(static_cast<int64_t>(m)), kv.second);
            }
        };
        remapMap(srcSlotinfo, mergedSlotinfo);
        remapMap(srcNames, mergedNames);
        entry.emplace_back(Value(std::string("_slotinfo")), Value(std::move(mergedSlotinfo)));
        entry.emplace_back(Value(std::string("_names")), Value(std::move(mergedNames)));
        if (srcClothNot.isArray()) entry.emplace_back(Value(std::string("_clothnotdata")), srcClothNot);
        if (srcForce.isBool()) entry.emplace_back(Value(std::string("_forceclothnotupdate")), srcForce);
    }
    dstCoords[dstOutfit] = Value(std::move(entry));
    Value::Map outMap;
    for (const auto& kv : dstCoords) outMap.emplace_back(Value(kv.first), kv.second);
    writePluginPayload(card, "Accessory_States", "CoordinateData",
                       Value(MsgPackPack(Value(std::move(outMap)))), 2);
    notes.emplace_back(coordinateImport ? "Accessory_States: the source coordinate's accessory states imported"
                                        : "Accessory_States: source's accessory states merged (slots remapped)");
}

// DynamicBoneEditor (KKEx "com.deathweasel.bepinex.dynamicboneeditor"): rows of
// per-accessory bone modifiers, keyed by CoordinateIndex and Slot. Coordinate
// import replaces the coordinate's rows wholesale (they belong to the
// replaced accessories); the clothing merge keeps rows for surviving
// accessories and maps rows through clothToMerged.
void mergeDynamicBonePayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                              const std::vector<int>& clothToMerged, bool coordinateImport,
                              const std::set<int>& replacedAcc,
                              std::vector<std::string>& notes) {
    if (!srcExtended.isMap()) return;
    const Value& payload = srcExtended.at("com.deathweasel.bepinex.dynamicboneeditor").at(size_t(1));
    if (!payload.isMap()) return;
    const Value& srcBin = payload.at("AccessoryDynamicBoneData");
    if (!srcBin.isBin() || srcBin.asBin().empty()) return;
    std::vector<Value> rows;
    {
        const Value& dpayload = card.extended.at("com.deathweasel.bepinex.dynamicboneeditor").at(size_t(1));
        const Value& dbin = dpayload.at("AccessoryDynamicBoneData");
        if (dpayload.isMap() && dbin.isBin() && !dbin.asBin().empty()) {
            try {
                Value list = MsgPackParse(dbin.asBin());
                if (list.isArray()) {
                    for (const auto& row : list.asArray()) {
                        if (!row.isMap()) { rows.emplace_back(row); continue; }
                        const int64_t coord = row.at("CoordinateIndex").asInt(-1);
                        const int64_t slot = row.at("Slot").asInt(0);
                        if (coord == static_cast<int64_t>(dstOutfit) &&
                            (coordinateImport || replacedAcc.count(static_cast<int>(slot)))) continue;
                        rows.emplace_back(row);
                    }
                }
            } catch (const FormatError&) {}
        }
    }
    int added = 0;
    try {
        Value list = MsgPackParse(srcBin.asBin());
        if (list.isArray()) {
            for (const auto& row : list.asArray()) {
                if (!row.isMap()) continue;
                const int64_t coord = row.at("CoordinateIndex").asInt(-1);
                if (srcCoordinate >= 0 && coord != static_cast<int64_t>(srcCoordinate)) continue;   // -1: a single-coordinate file, all its rows travel
                Value nr = row;
                const int64_t slot = row.at("Slot").asInt(-1);
                int mapped = static_cast<int>(slot);
                if (slot >= 0) {
                    if (static_cast<size_t>(slot) < clothToMerged.size()) mapped = clothToMerged[static_cast<size_t>(slot)];
                    else if (!coordinateImport) continue;   // out of range: only the coordinate path passes a 1:1 map for every slot
                    if (mapped < 0) continue;
                    const auto setKeyInt = [&](const char* k, int64_t v) {
                        for (auto& kv : nr.map()) if (kv.first.isStr() && kv.first.asStr() == k) { kv.second = Value(v); return; }
                        nr.map().emplace_back(Value(std::string(k)), Value(v));
                    };
                    setKeyInt("Slot", mapped);
                }
                const auto setKeyIntCI = [&](const char* k, int64_t v) {
                    for (auto& kv : nr.map()) if (kv.first.isStr() && kv.first.asStr() == k) { kv.second = Value(v); return; }
                    nr.map().emplace_back(Value(std::string(k)), Value(v));
                };
                setKeyIntCI("CoordinateIndex", dstOutfit);
                rows.emplace_back(std::move(nr));
                ++added;
            }
        }
    } catch (const FormatError&) {}
    writePluginPayload(card, "com.deathweasel.bepinex.dynamicboneeditor", "AccessoryDynamicBoneData",
                       Value(MsgPackPack(Value(std::move(rows)))), 0);
    if (added > 0 || coordinateImport)
        notes.emplace_back(coordinateImport ? "DynamicBoneEditor: the source coordinate's accessory bone mods imported (" + std::to_string(added) + " row(s))"
                                            : "DynamicBoneEditor: source's accessory bone mods merged (" + std::to_string(added) + " row(s))");
}

// HairAccessoryCustomizer (KKEx "com.deathweasel.bepinex.hairaccessorycustomizer"):
// per-coordinate, per-slot hair-accessory look, a map of coordIndex -> slotIndex -> settings.
void mergeHairAccessoryPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                                const std::vector<int>& clothToMerged, bool coordinateImport,
                                const std::set<int>& /*unused*/,
                                std::vector<std::string>& notes) {
    if (!srcExtended.isMap()) return;
    const Value& payload = srcExtended.at("com.deathweasel.bepinex.hairaccessorycustomizer").at(size_t(1));
    if (!payload.isMap()) return;
    const Value* srcBinP = mapFind(payload, "HairAccessories");
    if (!srcBinP || !srcBinP->isBin() || srcBinP->asBin().empty()) srcBinP = mapFind(payload, "CoordinateHairAccessories");
    if (!srcBinP || !srcBinP->isBin() || srcBinP->asBin().empty()) return;
    const Value& srcBin = *srcBinP;
    // Both shapes are seen in the wild: nested {coordIdx: {slotIdx: settings}} (cards) and
    // the flat {slotIdx: settings} (outfit files written verbatim from the maker).
    Value parsed;
    try { parsed = MsgPackParse(srcBin.asBin()); } catch (const FormatError&) {}
    if (!parsed.isMap()) return;
    int shape = 0;   // 1 flat, 2 nested
    {
        bool sawStrVal = false, sawIntVal = false;
        for (const auto& kv : parsed.asMap()) {
            if (!kv.first.isInt() || !kv.second.isMap()) { shape = 0; return; }
            bool valHasInt = false, valHasStr = false;
            for (const auto& kv2 : kv.second.asMap()) {
                if (kv2.first.isInt()) valHasInt = true; else valHasStr = true;
            }
            if (valHasInt) sawIntVal = true;
            if (valHasStr) sawStrVal = true;
        }
        if (sawIntVal && !sawStrVal) shape = 2;
        else if (sawStrVal) shape = 1;
    }
    Value srcEntry;
    if (shape == 1) {
        srcEntry = parsed;   // the file's one coordinate, as-is
    } else {
        std::map<int64_t, Value> srcCoords;
        for (const auto& kv : parsed.asMap()) if (kv.first.isInt()) srcCoords.emplace(kv.first.asInt(), kv.second);
        const Value* sel = selectCoordinateEntry(srcCoords, srcCoordinate);
        if (!sel || !sel->isMap()) return;
        srcEntry = *sel;
    }
    // The target always stores nested {coordIdx: {slotIdx: settings}} (cards are nested), whichever key it used -
    // read both key names and both shapes; the flat shape is stored keyed at dstOutfit on write.
    std::map<int64_t, Value> dstCoords;
    {
        const Value& dpayload = card.extended.at("com.deathweasel.bepinex.hairaccessorycustomizer").at(size_t(1));
        const Value* dbinP = dpayload.isMap() ? mapFind(dpayload, "HairAccessories") : nullptr;
        if ((!dbinP || !dbinP->isBin() || dbinP->asBin().empty()) && dpayload.isMap()) dbinP = mapFind(dpayload, "CoordinateHairAccessories");
        if (dbinP && dbinP->isBin() && !dbinP->asBin().empty()) {
            try {
                Value dparsed = MsgPackParse(dbinP->asBin());
                if (dparsed.isMap()) {
                    bool dstFlat = true;
                    for (const auto& kv : dparsed.asMap()) {
                        if (!kv.first.isInt() || !kv.second.isMap()) { dstFlat = false; break; }
                        bool valHasStr = false;
                        for (const auto& kv2 : kv.second.asMap()) if (!kv2.first.isInt()) { valHasStr = true; break; }
                        if (!valHasStr) { dstFlat = false; break; }
                    }
                    if (dstFlat) dstCoords.emplace(static_cast<int64_t>(dstOutfit), dparsed);
                    else for (const auto& kv : dparsed.asMap()) if (kv.first.isInt()) dstCoords.emplace(kv.first.asInt(), kv.second);
                }
            } catch (const FormatError&) {}
        }
    }
    Value::Map merged;
    if (coordinateImport) {
        for (const auto& kv : srcEntry.asMap()) merged.emplace_back(kv.first, kv.second);
    } else {
        // keep target slots not touched by the merge, then rewrite the travelled ones
        std::set<int> used;
        for (const auto& kv : srcEntry.asMap()) {
            if (!kv.first.isInt()) continue;
            const int s = static_cast<int>(kv.first.asInt());
            const int m = s >= 0 && static_cast<size_t>(s) < clothToMerged.size() ? clothToMerged[static_cast<size_t>(s)] : -1;
            if (m < 0) continue;
            used.insert(m);
        }
        const Value& dstEntry = dstCoords.count(dstOutfit) ? dstCoords[dstOutfit] : Value::nil();
        if (dstEntry.isMap()) for (const auto& kv : dstEntry.asMap()) if (kv.first.isInt() && !used.count(static_cast<int>(kv.first.asInt()))) merged.emplace_back(kv.first, kv.second);
        for (const auto& kv : srcEntry.asMap()) {
            if (!kv.first.isInt()) { merged.emplace_back(kv.first, kv.second); continue; }
            const int s = static_cast<int>(kv.first.asInt());
            const int m = s >= 0 && static_cast<size_t>(s) < clothToMerged.size() ? clothToMerged[static_cast<size_t>(s)] : -1;
            if (m < 0) continue;
            bool found = false;
            for (auto& ekv : merged)
                if (ekv.first.isInt() && static_cast<int>(ekv.first.asInt()) == m) { ekv.second = kv.second; found = true; break; }
            if (!found) merged.emplace_back(Value(static_cast<int64_t>(m)), kv.second);
        }
    }
    dstCoords[dstOutfit] = Value(std::move(merged));
    Value::Map outMap;
    for (const auto& kv : dstCoords) outMap.emplace_back(Value(kv.first), kv.second);
    writePluginPayload(card, "com.deathweasel.bepinex.hairaccessorycustomizer", "HairAccessories",
                       Value(MsgPackPack(Value(std::move(outMap)))), 0);
    notes.emplace_back(coordinateImport ? "HairAccessoryCustomizer: the source coordinate's hair accessory settings imported"
                                        : "HairAccessoryCustomizer: source's hair accessory settings merged");
}

// pushup (KKEx "com.deathweasel.bepinex.pushup"): per-coordinate garment physics,
// a map of coordIndex -> parameters. The body pushup (Pushup_BodyData) is tied
// to the wearer's own body and is not ported; the top/bra (Pushup_TopData /
// Pushup_BraData) are tuned to the garments, so they travel with the outfit.
void mergePushupPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                         std::vector<std::string>& notes) {
    if (!srcExtended.isMap()) return;
    const char* kKeys[] = {"Pushup_TopData", "Pushup_BraData"};
    for (const char* key : kKeys) {
        const Value& payload = srcExtended.at("com.deathweasel.bepinex.pushup").at(size_t(1));
        if (!payload.isMap()) return;
        const Value& srcBin = payload.at(key);
        if (!srcBin.isBin() || srcBin.asBin().empty()) continue;
        std::map<int64_t, Value> coords;
        try {
            Value parsed = MsgPackParse(srcBin.asBin());
            if (parsed.isMap())
                for (const auto& kv : parsed.asMap())
                    if (kv.first.isInt()) coords.emplace(kv.first.asInt(), kv.second);
        } catch (const FormatError&) {}
        const auto it = coords.find(static_cast<int64_t>(srcCoordinate));
        if (it == coords.end()) continue;
        const Value& dpayload = card.extended.at("com.deathweasel.bepinex.pushup").at(size_t(1));
        std::map<int64_t, Value> dstCoords;
        if (dpayload.isMap()) {
            const Value& dbin = dpayload.at(key);
            if (dbin.isBin() && !dbin.asBin().empty()) {
                try {
                    Value dparsed = MsgPackParse(dbin.asBin());
                    if (dparsed.isMap())
                        for (const auto& kv : dparsed.asMap())
                            if (kv.first.isInt()) dstCoords.emplace(kv.first.asInt(), kv.second);
                } catch (const FormatError&) {}
            }
        }
        dstCoords[dstOutfit] = it->second;
        Value::Map outMap;
        for (const auto& kv : dstCoords) outMap.emplace_back(Value(kv.first), kv.second);
        // Preserve the entry's version (0) and any sibling keys we do not manage (Pushup_BodyData, BraHigherPriority).
        if (card.extended.isMap()) {
            bool found = false;
            for (auto& kv : card.extended.map()) {
                if (!kv.first.isStr() || kv.first.asStr() != "com.deathweasel.bepinex.pushup") continue;
                const int64_t version = kv.second.isArray() && kv.second.size() >= 2 ? kv.second.at(size_t(0)).asInt(0) : 0;
                Value::Map payload2;
                if (kv.second.isArray() && kv.second.size() >= 2 && kv.second.at(size_t(1)).isMap()) payload2 = kv.second.at(size_t(1)).asMap();
                bool kfound = false;
                for (auto& pkv : payload2)
                    if (pkv.first.isStr() && pkv.first.asStr() == key) { pkv.second = Value(MsgPackPack(Value(std::move(outMap)))); kfound = true; }
                if (!kfound) payload2.emplace_back(Value(std::string(key)), Value(MsgPackPack(Value(std::move(outMap)))));
                kv.second = Value(Value::Array{Value(version), Value(std::move(payload2))});
                found = true;
                break;
            }
            if (!found) {
                Value::Map payload2;
                payload2.emplace_back(Value(std::string(key)), Value(MsgPackPack(Value(std::move(outMap)))));
                card.extended.map().emplace_back(Value(std::string("com.deathweasel.bepinex.pushup")),
                                                 Value(Value::Array{Value(int64_t{0}), Value(std::move(payload2))}));
            }
        } else {
            card.extended = Value(Value::Map{});
            Value::Map payload2;
            payload2.emplace_back(Value(std::string(key)), Value(MsgPackPack(Value(std::move(outMap)))));
            card.extended.map().emplace_back(Value(std::string("com.deathweasel.bepinex.pushup")),
                                             Value(Value::Array{Value(int64_t{0}), Value(std::move(payload2))}));
        }
    }
    notes.emplace_back("pushup: the source garments' pushup settings imported (body pushup left with the character)");
}

// Additional_Card_Info schema reconnaissance (both generations found in the wild, see docs/COORDINATE_IMPORT.md):
//   v0 ({CoordinateInfo: bin({coordIdx: {...}}), CardInfo: bin({...})}) and
//   v1 (flat map, per-coordinate keys are 7-element arrays over the outfits).
// CardInfo/Personal_Clothing_Save/MakeUpKeep/Cosplay_Academy_Ready are
// character-scoped and never travel; only the coordinate's own entry does:
// the full entry in a Coordinate import, the accessory keep flags
// (_acckeep/_hairkeep / AccKeep/HairAcc) in a clothing import (a clothing file
// carries exactly those).
bool aciEntry(const Value& ext, int64_t& versionOut, Value::Map& payloadOut) {
    if (!ext.isMap()) return false;
    const Value& entry = ext.at("Additional_Card_Info");
    if (!entry.isArray() || entry.size() < 2 || !entry.at(size_t(1)).isMap()) return false;
    versionOut = entry.at(size_t(0)).asInt(0);
    payloadOut = entry.at(size_t(1)).asMap();
    return true;
}

// The source coordinate's own gender-restriction flag from Additional_Card_Info
// (v0 CoordinateInfo entries / v1 flat GenderType array), or -1 when absent.
// The maker shows such a coordinate only to matching characters; the flag is a
// restriction the author set, not something we can enforce here.
int aciGenderRestriction(const Value& ext, int srcCoordinate, int srcOutfits) {
    int64_t version = 0;
    Value::Map payload;
    if (!aciEntry(ext, version, payload)) return -1;
    const int64_t n = srcCoordinate < 0 ? 0 : static_cast<int64_t>(srcCoordinate);   // a real file: its one coordinate is index 0
    const Value* ci = mapFind(payload, "CoordinateInfo");
    if (ci && ci->isBin() && !ci->asBin().empty()) {
        try {
            Value parsed = MsgPackParse(ci->asBin());
            if (!parsed.isMap()) return -1;
            if (srcCoordinate < 0) {
                // the flat per-coordinate entry itself (a real outfit file carries just its one coordinate)
                const Value& ri = parsed.at("_restrictioninfo");
                if (!ri.isMap()) return -1;
                const Value& g = ri.at("_gender");
                return g.isNumber() ? static_cast<int>(g.asInt()) : -1;
            }
            for (const auto& kv : parsed.asMap()) {
                if (!kv.first.isInt() || kv.first.asInt() != n) continue;
                if (!kv.second.isMap()) return -1;
                const Value& ri = kv.second.at("_restrictioninfo");
                if (!ri.isMap()) return -1;
                const Value& g = ri.at("_gender");
                return g.isNumber() ? static_cast<int>(g.asInt()) : -1;
            }
            return -1;
        } catch (const FormatError&) { return -1; }
    }
    // v1: GenderType is an array over the outfits.
    const Value* gtp = mapFind(payload, "GenderType");
    Value arr;
    if (gtp && gtp->isBin() && !gtp->asBin().empty()) {
        try { arr = MsgPackParse(gtp->asBin()); } catch (const FormatError&) { return -1; }
    } else if (gtp && gtp->isArray()) {
        arr = *gtp;
    } else {
        return -1;
    }
    if (!arr.isArray() || srcOutfits <= 0 || static_cast<size_t>(n) >= arr.size()) return -1;
    const Value& g = arr.at(static_cast<size_t>(n));
    return g.isNumber() ? static_cast<int>(g.asInt()) : -1;
}
void writeAci(Card& card, int64_t version, const Value::Map& payload) {
    if (!card.extended.isMap()) card.extended = Value(Value::Map{});
    Value entry(Value::Array{Value(version), Value(payload)});
    for (auto& kv : card.extended.map()) {
        if (!kv.first.isStr() || kv.first.asStr() != "Additional_Card_Info") continue;
        kv.second = std::move(entry);
        return;
    }
    card.extended.map().emplace_back(Value(std::string("Additional_Card_Info")), std::move(entry));
}

// Warns when a coordinate from a different-sex source is being applied; names what resolves fine (mod rows by GUID,
// base rows by id) and what the maker would flag (the source coordinate's own gender restriction). Never discards anything.
void sexMismatchNotes(const Card& src, const Card& dst, int srcCoordinate, std::vector<std::string>& notes) {
    const Value& sSex = src.parameter.at("sex");
    const Value& dSex = dst.parameter.at("sex");
    const int64_t ss = sSex.isNumber() ? sSex.asInt() : -1;
    const int64_t ds = dSex.isNumber() ? dSex.asInt() : -1;
    if (ss < 0 || ds < 0 || ss == ds) return;
    const char* sName = ss == 1 ? "female" : ss == 0 ? "male" : "unknown";
    const char* dName = ds == 1 ? "female" : ds == 0 ? "male" : "unknown";
    int modRows = 0;
    for (const auto& rr : src.rows)
        if (!rr.modId.empty() && rr.modId != "コイカツ" && rr.modId != "abdata") ++modRows;
    const int genderFlag = src.extended.isMap() ? aciGenderRestriction(src.extended, srcCoordinate, static_cast<int>(src.outfits.size())) : -1;
    notes.emplace_back(
        std::string("sex mismatch: the source is a ") + sName + " card, the target " + dName +
        " - mod items resolve by GUID regardless of sex (" + std::to_string(modRows) +
        " mod row(s)) and base items by id, so the items themselves are unaffected; "
        "garments authored for one sex may clash with the target's body shape (fit/textures). "
        "Nothing is discarded; items with no matching row are reported missing by the resolver.");
    if (genderFlag > 0)
        notes.emplace_back("the source coordinate carries its own gender restriction flag (raw value " +
                           std::to_string(genderFlag) +
                           "), which the maker honours when browsing coordinates - it is imported with the coordinate, not enforced here");
}

// Copies the whole coordinate's ACI entry (coordinate import) or its accessory keep flags (clothing import)
// from the source to the target's outfit slot `dstOutfit`. Character-scoped keys never travel.
void mergeAdditionalCardInfo(Card& card, const Value& srcExtended, int srcCoordinate, int srcOutfits, int dstOutfit,
                             int dstOutfits, bool coordinateImport, std::vector<std::string>& notes) {
    if (!srcExtended.isMap()) return;
    int64_t srcVersion = 0;
    Value::Map srcPayload;
    if (!aciEntry(srcExtended, srcVersion, srcPayload)) return;
    int64_t dstVersion = 0;
    Value::Map dstPayload;
    const bool dstHad = card.extended.isMap() && aciEntry(card.extended, dstVersion, dstPayload);
    const Value* srcCoordInfo = mapFind(srcPayload, "CoordinateInfo");
    const bool srcIsV0 = srcCoordInfo != nullptr && srcCoordInfo->isBin();
    bool ported = false;
    if (srcIsV0) {
        Value srcInfo;
        try { srcInfo = MsgPackParse(srcCoordInfo->asBin()); } catch (const FormatError&) {}
        std::map<int64_t, Value> srcEntries;
        if (srcInfo.isMap()) {
            bool anyInt = false;
            for (const auto& kv : srcInfo.asMap()) if (kv.first.isInt()) anyInt = true;
            if (anyInt) {
                for (const auto& kv : srcInfo.asMap()) if (kv.first.isInt()) srcEntries.emplace(kv.first.asInt(), kv.second);
            } else {
                // the flat per-coordinate entry itself (a real outfit file carries just its one coordinate)
                srcEntries.emplace(static_cast<int64_t>(srcCoordinate >= 0 ? srcCoordinate : 0), srcInfo);
            }
        }
        std::map<int64_t, Value> dstEntries;
        if (dstHad) {
            const Value* dbin = mapFind(dstPayload, "CoordinateInfo");
            if (dbin != nullptr && dbin->isBin() && !dbin->asBin().empty()) {
                try {
                    Value dparsed = MsgPackParse(dbin->asBin());
                    if (dparsed.isMap())
                        for (const auto& kv : dparsed.asMap())
                            if (kv.first.isInt()) dstEntries.emplace(kv.first.asInt(), kv.second);
                } catch (const FormatError&) {}
            }
        }
        const Value* sel = selectCoordinateEntry(srcEntries, srcCoordinate);
        if (sel != nullptr && sel->isMap()) {
            const Value& srcEntry = *sel;
            if (coordinateImport) {
                dstEntries[dstOutfit] = srcEntry;   // the coordinate, as saved, including restrictions and keep flags
                ported = true;
            } else {
                // Clothing import: only the accessory keep flags travel (what a clothing file carries).
                Value dstEntry = dstEntries.count(dstOutfit) ? dstEntries[dstOutfit] : Value(Value::Map{});
                if (!dstEntry.isMap()) dstEntry = Value(Value::Map{});
                Value::Map em;
                if (dstEntry.isMap()) em = dstEntry.asMap();
                auto poke = [&](const char* key) {
                    const Value& fv = srcEntry.at(key);
                    bool found = false;
                    for (auto& kv : em)
                        if (kv.first.isStr() && kv.first.asStr() == key) { kv.second = fv; found = true; }
                    if (!found) em.emplace_back(Value(std::string(key)), fv);
                };
                if (srcEntry.at("_acckeep").isArray()) poke("_acckeep");
                if (srcEntry.at("_hairkeep").isArray()) poke("_hairkeep");
                dstEntries[dstOutfit] = Value(std::move(em));
                ported = true;
            }
        }
        Value::Map outInfo;
        for (const auto& kv : dstEntries) outInfo.emplace_back(Value(kv.first), kv.second);
        // CardInfo (character-scoped) stays whatever the target has; when the target has no ACI at all, create only what travels.
        Value::Map outPayload = dstHad ? dstPayload : Value::Map{};
        bool ciFound = false;
        for (auto& kv : outPayload)
            if (kv.first.isStr() && kv.first.asStr() == "CoordinateInfo") { kv.second = Value(MsgPackPack(Value(std::move(outInfo)))); ciFound = true; }
        if (!ciFound) outPayload.emplace_back(Value(std::string("CoordinateInfo")), Value(MsgPackPack(Value(std::move(outInfo)))));
        int64_t version = dstHad ? dstVersion : srcVersion;
        writeAci(card, version, outPayload);
        if (ported) notes.emplace_back(coordinateImport ? "Additional_Card_Info: the source coordinate's metadata imported (keep flags, restrictions)"
                                                        : "Additional_Card_Info: accessory keep flags imported");
        return;
    }
    // v1: per-coordinate keys are arrays (usually msgpack bins) spanning the source's outfits; copy index n -> m.
    // Character-scoped keys never travel: bools, non-array bins, and arrays whose size differs from the
    // source's outfit count (the character-scoped keeps are length 9, Cosplay_Academy_Ready a bare bool).
    // A real outfit file stores only its one coordinate, so index 0 is read regardless of the array's span.
    const int64_t nV1 = srcCoordinate < 0 ? 0 : static_cast<int64_t>(srcCoordinate);
    const bool srcIsFile = srcCoordinate < 0;
    if (dstOutfits <= 0) return;
    if (dstOutfits < static_cast<int>(dstOutfit) + 1) return;   // nothing sane to write into
    const auto v1Decode = [](const Value& v) -> Value {
        if (v.isBin() && !v.asBin().empty()) {
            try { return MsgPackParse(v.asBin()); } catch (const FormatError&) { return Value::nil(); }
        }
        return v;
    };
    Value::Map outPayload = dstHad ? dstPayload : Value::Map{};
    int portedCount = 0;
    for (const auto& kv : srcPayload) {
        const std::string key = kv.first.isStr() ? kv.first.asStr() : std::string();
        if (key.empty()) continue;
        if (key == "CoordinateInfo" || key == "CardInfo") continue;   // v0 leftovers in a v1 payload, if any
        const Value decoded = v1Decode(kv.second);
        if (!decoded.isArray() || decoded.size() == 0) continue;
        if (!srcIsFile && static_cast<int>(decoded.size()) != srcOutfits) continue;   // character-scoped, or outfit-shape we cannot map
        if (static_cast<size_t>(nV1) >= decoded.size()) continue;
        const Value& srcAtN = decoded.at(static_cast<size_t>(nV1));
        if (!coordinateImport) {
            if (key != "AccKeep" && key != "HairAcc") continue;   // clothing import: accessory keep flags only
        }
        const Value* curBin = mapFind(outPayload, key.c_str());
        const Value cur = curBin ? v1Decode(*curBin) : Value(Value::Array{});
        Value::Array arr;
        if (cur.isArray()) arr = cur.asArray();
        while (static_cast<int>(arr.size()) < dstOutfits) arr.emplace_back(Value(Value::Array{}));
        arr[static_cast<size_t>(dstOutfit)] = srcAtN;
        const Value encoded = Value(MsgPackPack(Value(std::move(arr))));
        bool found = false;
        for (auto& ekv : outPayload)
            if (ekv.first.isStr() && ekv.first.asStr() == key) { ekv.second = encoded; found = true; }
        if (!found) outPayload.emplace_back(Value(std::string(key)), encoded);
        ++portedCount;
    }
    if (portedCount) {
        int64_t version = dstHad ? dstVersion : srcVersion;
        writeAci(card, version, outPayload);
        notes.emplace_back(coordinateImport ? ("Additional_Card_Info: the source coordinate's metadata imported (" + std::to_string(portedCount) + " key(s))")
                                            : ("Additional_Card_Info: accessory keep flags imported (" + std::to_string(portedCount) + " key(s))"));
    }
}

ClothingFile LoadClothingFile(const std::string& path) {
    ClothingFile c;
    c.path = path;
    Bytes file = fileio::ReadFile(path);
    c.fileSize = file.size();
    size_t end = pngEnd(file);   // throws NotPng / TruncatedPng
    if (end >= file.size()) throw CardError(CardErrorKind::NoPayload, "PNG has no outfit data after the image");

    ByteReader r(file.data() + end, file.size() - end, false);
    std::string marker;
    try {
        c.productNo = r.i32();
        marker = r.dotnetString();
    } catch (const FormatError&) {
        throw CardError(CardErrorKind::UnknownMarker, "data after the PNG does not start like a card");
    }
    c.marker = marker;
    bool isClothes = marker.rfind("\xE3\x80\x90KoiKatuClothes", 0) == 0;
    if (!isClothes)
        throw CardError(CardErrorKind::UnknownMarker, "not an outfit file (marker '" + marker + "')");

    try {
        c.formatVersion = r.dotnetString();
        c.name = r.dotnetString();
        if (isSunshineClothesMarker(marker)) {
            c.supported = false;
            c.supportNote = "recognised as a Sunshine outfit file, not converted yet (no matching game install to verify its asset bundles)";
            c.sha256 = Sha256Hex(file);
            return c;
        }
        int32_t coordLen = r.i32();
        if (coordLen < 0 || static_cast<size_t>(coordLen) > r.remaining()) throw FormatError("outfit coordinate runs past the file");
        Bytes coordBytes = r.bytes(static_cast<size_t>(coordLen));
        decodeCoordinate(coordBytes, c.outfit);

        // Extended data: a named block ("KKEx"), a version/size pair, then the
        // same MessagePack map a character card's KKEx block carries. Some
        // files write only a size; accept either by checking the next byte.
        if (r.remaining() > 0) {
            std::string extName;
            try { extName = r.dotnetString(); } catch (const FormatError&) { extName.clear(); }
            if (!extName.empty()) {
                if (extName != "KKEx") throw FormatError("unexpected extended block '" + extName + "' in outfit file");
                if (r.remaining() < 4) throw FormatError("extended data is truncated");
                size_t saved = r.pos();
                int32_t a = r.i32();
                bool hasVersion = false;
                int32_t size = a;
                if (r.remaining() >= 4) {
                    int32_t b = r.i32();
                    // Version + size: the byte after the second int opens a map.
                    if (r.remaining() > 0) {
                        uint8_t next = *r.ptr();
                        bool mapHeader = (next >= 0x80 && next <= 0x8f) || next == 0xde || next == 0xdf;
                        if (mapHeader && b >= 0 && static_cast<size_t>(b) == r.remaining()) { hasVersion = true; size = b; }
                    }
                }
                if (!hasVersion) r.seek(saved), size = r.i32();
                if (size < 0 || static_cast<size_t>(size) > r.remaining()) throw FormatError("extended data size runs past the file");
                Bytes extBytes = r.bytes(static_cast<size_t>(size));
                c.extended = MsgPackParse(extBytes);
                const Value& uar = c.extended.at("com.bepis.sideloader.universalautoresolver");
                if (uar.isArray()) {
                    for (const auto& raw : uar.at(size_t(1)).at("info").asArray()) {
                        if (!raw.isBin()) continue;
                        Value row = MsgPackParse(raw.asBin());
                        ResolveRow rr;
                        rr.modId = row.at("ModID").asStr();
                        rr.slot = row.at("Slot").asInt();
                        rr.localSlot = row.at("LocalSlot").asInt();
                        rr.property = row.at("Property").asStr();
                        rr.category = static_cast<int>(row.at("CategoryNo").asInt());
                        c.rows.push_back(std::move(rr));
                    }
                }
                // Additional_Card_Info keep flags (slot indices the file asks to preserve).
                const Value& ci = c.extended.at("Additional_Card_Info").at(size_t(1)).at("CoordinateInfo");
                if (ci.isBin() && !ci.asBin().empty()) {
                    try {
                        Value info = MsgPackParse(ci.asBin());
                        auto readKeep = [](const Value& v, std::vector<int64_t>& into) {
                            if (!v.isArray()) return;
                            for (const auto& e : v.asArray()) into.push_back(e.asInt());
                        };
                        readKeep(info.at("_acckeep"), c.accKeep);
                        readKeep(info.at("_hairkeep"), c.hairKeep);
                    } catch (const FormatError&) { /* optional: ignore malformed info */ }
                }
                appendMoreAccessoriesToOutfit(c.outfit, c.extended);
            }
        }
    } catch (const CardError&) {
        throw;
    } catch (const FormatError& e) {
        throw CardError(CardErrorKind::CorruptPayload, std::string("damaged outfit data: ") + e.what());
    }

    // A real outfit file carries one coordinate verbatim, under whatever index
    // the author saved it with; read that payload for the merges instead of assuming 0.
    if (c.extended.isMap() || c.extended.isNil()) {
        c.singleCoordinateSource = true;
        c.srcCoordinate = detectClothingFileCoordinateIndex(c.extended);
    }

    c.supported = true;
    c.sha256 = Sha256Hex(file);
    return c;
}

ClothingFile ExtractClothingFromCard(const Card& src, int coordinate) {
    if (!src.supported)
        throw FormatError(std::string("the coordinate source is not supported: ") +
                          (src.supportNote.empty() ? std::string("unsupported card") : src.supportNote));
    if (src.outfits.empty()) throw FormatError("the source card has no coordinates");
    int n = coordinate;
    if (n < 0) {
        n = src.status.isMap() ? static_cast<int>(src.status.at("coordinateType").asInt(0)) : 0;
        if (n < 0 || static_cast<size_t>(n) >= src.outfits.size()) n = 0;
    }
    if (n < 0 || static_cast<size_t>(n) >= src.outfits.size())
        throw FormatError("the source coordinate index is out of range");
    ClothingFile cl;
    cl.path = src.path;
    cl.fileSize = src.fileSize;
    cl.sha256 = src.sha256;
    cl.marker = src.marker;
    cl.formatVersion = src.formatVersion;
    cl.productNo = src.productNo;
    cl.supported = true;
    cl.srcCoordinate = n;
    cl.srcOutfitCount = static_cast<int>(src.outfits.size());
    cl.outfit = src.outfits[static_cast<size_t>(n)];
    // The full map, with every reader aimed at coordinate n: nothing of the
    // source's identity (Custom/Parameter/Status, hair, other coordinates,
    // coordinate-agnostic plugins) is consulted by the merge.
    cl.extended = src.extended;
    const std::string who = src.displayName();
    cl.name = who.empty() ? src.outfitName(n) : who + " (" + src.outfitName(n) + ")";
    // Resolver rows for this coordinate only, rebased to the prefix-free shape
    // a file stores. Card coordinates carry author slot ids, so the merge's
    // local->slot translation is disabled by clearing the local slots.
    const std::string want = "outfit" + std::to_string(n) + ".";
    for (const auto& rr : src.rows) {
        if (rr.property.rfind(want, 0) != 0) continue;
        ResolveRow nr = rr;
        nr.property = rr.property.substr(want.size());
        nr.localSlot = 0;
        cl.rows.push_back(std::move(nr));
    }
    // MoreAccessories extras beyond slot 20 for this coordinate (the card
    // loader already appended what the XML holds; this only tops up gaps).
    appendMoreAccessoriesToOutfit(cl.outfit, cl.extended, n, /*flatShapeCounts=*/false);
    return cl;
}

OutfitSourceReport ApplyOutfitSource(Card& card, const std::string& path, const ApplyClothingOptions& opt) {
    OutfitSourceReport out;
    const ImageSourceKind kind = DetectImageSource(path);
    if (kind == ImageSourceKind::Coordinate) {
        // A Koikatsu Coordinate PNG (a character card used as an outfit
        // source): extract the outfit only, then run it through the same
        // merge a regular outfit file takes. A damaged or unsupported source
        // throws before anything is merged - never a silent fallback.
        Card src = LoadCard(path);
        if (!src.supported)
            throw FormatError(std::string("the coordinate source is not supported: ") + src.supportNote);
        if (opt.coordinateImport) {
            // The wholesale coordinate semantics (wholesale replace incl.
            // accessories, conflicts resolved with an identity slot map): the
            // source coordinate replaces the slot exactly as saved.
            ApplyCoordinateOptions copt;
            copt.srcOutfit = opt.srcOutfit;
            copt.dstOutfit = opt.outfit;
            const ApplyCoordinateReport rep = ApplyCoordinate(card, src, copt);
            out.fromCoordinate = true;
            out.srcOutfit = rep.srcOutfit;
            out.sourceName = rep.sourceName;
            out.apply.outfit = rep.dstOutfit;
            out.apply.clothesReplaced = rep.clothesReplaced;
            out.apply.accessoriesAdded = rep.accessoriesReplaced;
            out.apply.accessoriesRelocated = 0;
            out.apply.hairPreserved = 0;   // coordinate import: hair accessories are replaced, not kept
            out.apply.hairSlotsPreserved = 0;
            if (card.hair.isMap() && card.hair.at("parts").isArray())
                for (const auto& p : card.hair.at("parts").asArray())
                    if (p.at("id").asInt(0) != 0) ++out.apply.hairSlotsPreserved;
            out.apply.notes = rep.notes;
            return out;
        }
        ClothingFile cl = ExtractClothingFromCard(src, opt.srcOutfit);
        out.fromCoordinate = true;
        out.srcOutfit = cl.srcCoordinate;
        out.sourceName = cl.name;
        out.apply = ApplyClothing(card, cl, opt);
        sexMismatchNotes(src, card, cl.srcCoordinate, out.apply.notes);
        return out;
    }
    ClothingFile cl = LoadClothingFile(path);
    if (!cl.supported)
        throw FormatError(cl.supportNote.empty() ? std::string("outfit file not supported") : cl.supportNote);
    out.sourceName = cl.name.empty() ? path : cl.name;
    out.apply = ApplyClothing(card, cl, opt);
    return out;
}

ApplyCoordinateReport ApplyCoordinate(Card& card, const Card& src, const ApplyCoordinateOptions& opt) {
    if (!src.supported)
        throw FormatError(std::string("the coordinate source is not supported: ") +
                          (src.supportNote.empty() ? std::string("unsupported card") : src.supportNote));
    if (card.outfits.empty()) throw FormatError("the character has no outfits");
    if (src.outfits.empty()) throw FormatError("the source card has no coordinates");
    int n = opt.srcOutfit;
    if (n < 0) {
        n = src.status.isMap() ? static_cast<int>(src.status.at("coordinateType").asInt(0)) : 0;
        if (n < 0 || static_cast<size_t>(n) >= src.outfits.size()) n = 0;
    }
    if (n < 0 || static_cast<size_t>(n) >= src.outfits.size())
        throw FormatError("the source coordinate index is out of range");
    int m = opt.dstOutfit;
    if (m < 0) {
        m = card.status.isMap() ? static_cast<int>(card.status.at("coordinateType").asInt(0)) : 0;
        if (m < 0 || static_cast<size_t>(m) >= card.outfits.size()) m = 0;
    }
    if (m < 0 || static_cast<size_t>(m) >= card.outfits.size())
        throw FormatError("the target outfit index is out of range");
    ApplyCoordinateReport rep;
    rep.srcOutfit = n;
    rep.dstOutfit = m;
    rep.sourceName = src.displayName();
    const Outfit& so = src.outfits[static_cast<size_t>(n)];
    Outfit& d = card.outfits[static_cast<size_t>(m)];

    // --- clothes: wholesale replacement -------------------------------------
    if (so.clothes.isMap() && so.clothes.at("parts").isArray()) {
        const Value& oldParts = d.clothes.at("parts");
        const Value& newParts = so.clothes.at("parts");
        for (size_t i = 0; i < newParts.size() && i < 9; ++i) {
            const int64_t o = oldParts.isArray() && i < oldParts.size() ? oldParts.at(i).at("id").asInt(0) : 0;
            if (newParts.at(i).at("id").asInt(0) != o) ++rep.clothesReplaced;
        }
    }
    d.clothes = so.clothes;

    // --- accessories: wholesale replacement ---------------------------------
    // Positioning, rotation, scale, parenting, colors and hide flags travel
    // verbatim with the parts, including slots the source leaves empty
    // (which clear the target's, hair-type included - a clothing import
    // preserves hair; a coordinate import imports the coordinate as stored).
    {
        const Value& oldParts = d.accessory.at("parts");
        const Value& newParts = so.accessory.at("parts");
        if (oldParts.isArray() || newParts.isArray()) {
            const size_t cnt = std::max(oldParts.isArray() ? oldParts.size() : 0, newParts.isArray() ? newParts.size() : 0);
            for (size_t i = 0; i < cnt; ++i) {
                const Value& o = oldParts.isArray() && i < oldParts.size() ? oldParts.at(i) : Value::nil();
                const Value& s = newParts.isArray() && i < newParts.size() ? newParts.at(i) : Value::nil();
                const bool oEmpty = !o.isMap() || isEmptyAcc(o);
                const bool sEmpty = !s.isMap() || isEmptyAcc(s);
                if (oEmpty == sEmpty && o.at("type").asInt(-1) == s.at("type").asInt(-1) && o.at("id").asInt(0) == s.at("id").asInt(0)) continue;
                ++rep.accessoriesReplaced;
            }
        }
    }
    d.accessory = so.accessory;

    // --- makeup follows the source coordinate --------------------------------
    d.makeup = so.makeup;
    d.enableMakeup = so.enableMakeup;
    rep.makeupApplied = so.enableMakeup;

    // --- accessory visibility flags travel with the coordinate ---------------
    {
        const Value& sa = src.status.at("showAccessory");
        const Value& da = card.status.at("showAccessory");
        if (sa.isArray() && da.isArray() && sa.size() == da.size() && sa.size() >= 20) {
            bool ok = true;
            for (const auto& e : sa.asArray()) ok = ok && e.isBool();
            if (ok) {
                for (auto& kv : card.status.map())
                    if (kv.first.isStr() && kv.first.asStr() == "showAccessory") { kv.second = sa; break; }
                rep.notes.emplace_back("Status: accessory visibility flags copied from the source coordinate");
            }
        }
    }
    // --- clothes toggle state: the source's current screen state travels with the coordinate
    // (a character card carries exactly one Status; the coordinate shown at save time is
    // what was on screen. byte[9] indexed by ClothesKind: 0=on, 1=half-off, 2=half-off#2, 3=off) ---
    {
        const Value& srcCs = src.status.at("clothesState");
        const Value& dstCs = card.status.at("clothesState");
        auto writeClothesState = [&](const Value& bytes) {
            bool found = false;
            for (auto& kv : card.status.map())
                if (kv.first.isStr() && kv.first.asStr() == "clothesState") { kv.second = bytes; found = true; }
            if (!found && card.status.isMap())
                card.status.map().emplace_back(Value(std::string("clothesState")), bytes);
        };
        if (srcCs.isBin() && !srcCs.asBin().empty()) {
            bool ok = true;
            for (uint8_t b : srcCs.asBin()) ok = ok && b <= 3;
            if (!ok) {
                rep.notes.emplace_back("Status: source clothes toggle state not copied (unexpected byte values; kept the target's)");
            } else if (srcCs.asBin().size() == 9) {
                writeClothesState(srcCs);
                rep.notes.emplace_back("Status: clothes toggle state (0=on, 1=half-off, 2=half-off#2, 3=off) copied from the source");
            } else if (dstCs.isBin() && srcCs.asBin().size() == dstCs.asBin().size()) {
                writeClothesState(srcCs);
                rep.notes.emplace_back("Status: clothes toggle state copied from the source (length " + std::to_string(srcCs.asBin().size()) + ")");
            } else {
                rep.notes.emplace_back("Status: source clothes toggle state not copied (unexpected length " +
                                       std::to_string(srcCs.asBin().size()) + "; kept the target's)");
            }
        }
    }

    // --- sex mismatch (source and target of different sex) -------------------
    sexMismatchNotes(src, card, n, rep.notes);

    // --- resolver rows: the target coordinate's rows are dropped, the source's rebased, 1:1
    const std::string dstPrefix = "outfit" + std::to_string(m) + ".";
    {
        std::vector<ResolveRow> kept;
        kept.reserve(card.rows.size());
        for (const auto& rr : card.rows)
            if (rr.property.rfind(dstPrefix, 0) != 0) kept.push_back(rr);
        card.rows = std::move(kept);
    }
    const std::string srcPrefix = "outfit" + std::to_string(n) + ".";
    std::vector<ResolveRow> appended;
    for (const auto& rr : src.rows) {
        if (rr.property.rfind(srcPrefix, 0) != 0) continue;   // only this coordinate's rows travel
        ResolveRow nr = rr;
        nr.property = dstPrefix + rr.property.substr(srcPrefix.size());
        bool exists = false;
        for (const auto& e : card.rows) if (e.property == nr.property && e.slot == nr.slot) { exists = true; break; }
        if (exists) continue;
        appended.push_back(nr);
        card.rows.push_back(std::move(nr));
    }
    rep.notes.emplace_back("resolver rows: " + std::to_string(appended.size()) + " rebased to outfit " + std::to_string(m));
    // Keep the extended universalautoresolver payload consistent with the replaced coordinate's rows.
    syncExtendedResolvedRows(card, m, appended, /*replacedAcc=*/nullptr);

    // --- coordinate-scoped plugin payloads, rebased n -> m -------------------
    mergeAbmxPayloads(card, src.extended, rep.notes, n, m, static_cast<int>(card.outfits.size()), /*coordinateMode=*/true);
    {
        std::vector<int> identity;
        if (so.accessory.isMap() && so.accessory.at("parts").isArray()) {
            const Value& parts = so.accessory.at("parts");
            identity.resize(parts.size());
            for (size_t i = 0; i < parts.size(); ++i)
                identity[i] = isEmptyAcc(parts.at(i)) ? -1 : static_cast<int>(i);
        }
        std::set<int> unused;
        mergeMaterialEditorPayloads(card, src.extended, m, identity, n, unused, /*dropAllAccRows=*/true, rep.notes);
        mergeDynamicBonePayloads(card, src.extended, n, m, identity, /*coordinateImport=*/true, unused, rep.notes);
        mergeHairAccessoryPayloads(card, src.extended, n, m, identity, /*coordinateImport=*/true, unused, rep.notes);
        mergeAccessoryStatesPayloads(card, src.extended, n, m, identity, /*coordinateImport=*/true, rep.notes);
        mergePushupPayloads(card, src.extended, n, m, rep.notes);
        mergeAdditionalCardInfo(card, src.extended, n, static_cast<int>(src.outfits.size()), m,
                                static_cast<int>(card.outfits.size()), /*coordinateImport=*/true, rep.notes);
    }
    mergeKcoxPayloads(card, src.extended, n, m, rep.notes);

    // --- MoreAccessories: the target coordinate's stale XML block is dropped -
    // (its extras travel inside the coordinate payload; other coordinates keep theirs)
    dropOutfitMoreAccessoriesXml(card, m, rep.notes);

    // --- provenance: the viewer/exporter list this under "Added outfit" ------
    card.hasClothing = true;
    card.clothingName = (rep.sourceName.empty() ? std::string("Coordinate") : rep.sourceName) +
                        " (" + src.outfitName(n) + ")";
    card.clothingPath = src.path;
    card.clothingOutfit = m;
    card.clothingPartIds.clear();
    {
        static const char* kSlot[9] = {"top", "bottom", "bra", "shorts", "gloves", "pantyhose", "socks", "shoes_inner", "shoes_outer"};
        const Value& cp = d.clothes.at("parts");
        if (cp.isArray())
            for (size_t i = 0; i < cp.size() && i < 9; ++i) {
                if (cp.at(i).at("id").asInt(0) == 0) continue;
                card.clothingPartIds.insert("outfit" + std::to_string(m) + "." + kSlot[i]);
            }
        if (so.accessory.isMap() && so.accessory.at("parts").isArray()) {
            for (size_t i = 0; i < so.accessory.at("parts").size(); ++i) {
                if (isEmptyAcc(so.accessory.at("parts").at(i))) continue;
                char num[8];
                std::snprintf(num, sizeof num, "%02d", static_cast<int>(i));
                card.clothingPartIds.insert("outfit" + std::to_string(m) + ".acc" + num);
            }
        }
    }
    return rep;
}

bool IsHairAccessoryPart(const Value& part, const std::vector<ResolveRow>& rows, int outfit, int accIndex) {
    if (part.isMap()) {
        const int type = static_cast<int>(part.at("type").asInt(0));
        if (type == 122 || type == 123) return true;
        const int64_t id = part.at("id").asInt(0);
        if (id != 0) {
            const std::string want = "outfit" + std::to_string(outfit) + ".accessory" + std::to_string(accIndex) + ".ChaFileAccessory.PartsInfo.id";
            for (const auto& rr : rows) {
                if (rr.property == want && rr.slot == id) {
                    if (rr.category == 122 || rr.category == 123) return true;
                    if (containsLower(rr.modId, "hair") || containsLower(rr.modId, "acchair")) return true;
                }
            }
        }
    }
    return false;
}

ApplyClothingReport ApplyClothing(Card& card, const ClothingFile& clothes, const ApplyClothingOptions& opt) {
    if (!clothes.supported)
        throw FormatError(std::string("outfit file not supported: ") + clothes.supportNote);
    if (card.outfits.empty()) throw FormatError("the character has no outfits");
    int outfit = opt.outfit;
    if (outfit < 0) outfit = card.status.isMap() ? static_cast<int>(card.status.at("coordinateType").asInt()) : 0;
    if (outfit < 0 || static_cast<size_t>(outfit) >= card.outfits.size()) outfit = 0;

    ApplyClothingReport rep;
    rep.outfit = outfit;

    // Hair slots are never touched: a clothing file has no hair, so the
    // back/front/side/extension the card carries always stay.
    {
        rep.hairSlotsPreserved = 0;
        if (card.hair.isMap() && card.hair.at("parts").isArray())
            for (const auto& p : card.hair.at("parts").asArray())
                if (p.at("id").asInt(0) != 0) ++rep.hairSlotsPreserved;
    }

    // Clothing files store mod ids as session-local ids (the resolver rows'
    // LocalSlot); character cards store the author's Slot. Translate so the
    // resolver finds each item by its property path.
    std::map<int64_t, int64_t> localToSlot;
    for (const auto& rr : clothes.rows)
        if (rr.localSlot != 0) localToSlot[rr.localSlot] = rr.slot;
    auto translate = [&](int64_t id) -> int64_t {
        auto it = localToSlot.find(id);
        return it == localToSlot.end() ? id : it->second;
    };

    Outfit& target = card.outfits[static_cast<size_t>(outfit)];

    // --- clothes: replace wholesale ---------------------------------------
    Value newClothes = clothes.outfit.clothes;
    {
        const Value& oldParts = target.clothes.at("parts");
        if (newClothes.isMap() && newClothes.at("parts").isArray()) {
            for (auto& kv : newClothes.map())
                if (kv.first.isStr() && kv.first.asStr() == "parts" && kv.second.isArray()) {
                    for (size_t i = 0; i < kv.second.array().size(); ++i) {
                        Value& part = kv.second.array()[i];
                        const int64_t oldId = part.at("id").asInt(0);
                        const int64_t newId = translate(oldId);
                        if (newId != oldId) setPartId(part, newId);
                        // Each garment's four colour slots also carry a pattern id, stored the
                        // same session-local way (the outfit's resolver rows name it
                        // "ChaFileClothes.<Slot>Pattern0" etc.). Without translating it the
                        // resolver never finds the pattern, the pattern mask stays empty and
                        // the garment loses its pattern colours (the gold trim baked as the
                        // base colour instead: the Wiz H bug).
                        if (part.isMap())
                            for (auto& pk : part.map())
                                if (pk.first.isStr() && pk.first.asStr() == "colorInfo" && pk.second.isArray())
                                    for (auto& ci : pk.second.array())
                                        if (ci.isMap())
                                            for (auto& ck : ci.map())
                                                if (ck.first.isStr() && ck.first.asStr() == "pattern") {
                                                    const int64_t newPat = translate(ck.second.asInt(0));
                                                    if (newPat != ck.second.asInt(0)) ck.second = Value(newPat);
                                                    break;
                                                }
                        const Value& oldP = oldParts.isArray() && i < oldParts.size() ? oldParts.at(i) : Value::nil();
                        if (oldP.at("id").asInt(0) != (newId)) ++rep.clothesReplaced;
                    }
                }
            for (auto& kv : newClothes.map())
                if (kv.first.isStr() && kv.first.asStr() == "subPartsId" && kv.second.isArray())
                    for (auto& e : kv.second.array()) e = Value(translate(e.asInt()));
        }
    }
    target.clothes = newClothes;

    // --- makeup: follow the file's own switch -----------------------------
    if (clothes.outfit.enableMakeup) {
        target.enableMakeup = true;
        target.makeup = clothes.outfit.makeup;
    }

    // --- accessories: merge, never deleting hair --------------------------
    const Value& charAcc = target.accessory;
    const Value& clothAcc = clothes.outfit.accessory;
    Value::Array charParts, clothParts;
    if (charAcc.isMap() && charAcc.at("parts").isArray()) charParts = charAcc.at("parts").asArray();
    if (clothAcc.isMap() && clothAcc.at("parts").isArray()) clothParts = clothAcc.at("parts").asArray();
    // Translate the clothing accessory ids first (local -> author slot).
    for (auto& p : clothParts)
        if (p.isMap() && p.at("id").asInt(0) != 0) setPartId(p, translate(p.at("id").asInt()));

    Value::Array merged;
    std::vector<int> clothToMerged;
    const Value empty = emptyAccPart(charAcc, clothAcc);
    if (opt.coordinateImport) {
        // Whole-coordinate import: the file's slots are the truth, verbatim -
        // positioning, rotation, parenting, hide flags. Slots the file leaves
        // empty clear the target's; hair-type accessories are replaced too
        // (that is the difference from a clothing import). Slots beyond the
        // file (a vanilla file declares 20; the target may hold MoreOutfits
        // extras below 20+100) are the character's own and stay.
        const size_t nCloth = clothParts.size();
        merged.reserve(std::max(charParts.size(), nCloth));
        for (size_t i = 0; i < nCloth; ++i) merged.push_back(clothParts[i]);
        for (size_t i = nCloth; i < charParts.size(); ++i) merged.push_back(charParts[i]);
        clothToMerged.assign(nCloth, -1);
        rep.accessoriesRelocated = 0;
        rep.accessoriesAdded = 0;
        for (size_t i = 0; i < nCloth; ++i) {
            if (isEmptyAcc(clothParts[i])) continue;   // empty file slot: clears the target's; not an "added" accessory
            clothToMerged[i] = static_cast<int>(i);
            ++rep.accessoriesAdded;
        }
        rep.hairPreserved = 0;   // wholesale: the coordinate replaces the target's
    } else {
    merged = charParts;
    const size_t nCloth = clothParts.size();
    while (merged.size() < nCloth) merged.push_back(empty);
    clothToMerged.assign(nCloth, -1);
    for (size_t i = 0; i < nCloth; ++i) {
        if (isEmptyAcc(clothParts[i])) continue;   // an empty outfit slot never clears the character
        const bool charEmpty = i >= charParts.size() || isEmptyAcc(charParts[i]);
        if (charEmpty) {
            merged[i] = clothParts[i];
            clothToMerged[i] = static_cast<int>(i);
            ++rep.accessoriesAdded;
            continue;
        }
        // The slot is taken. Hair stays; anything else is replaced.
        if (opt.keepHairAccessories && IsHairAccessoryPart(charParts[i], card.rows, outfit, static_cast<int>(i))) {
            ++rep.hairPreserved;
            size_t j = merged.size();
            for (size_t k = 0; k < merged.size(); ++k)
                if (isEmptyAcc(merged[k])) { j = k; break; }
            if (j == merged.size()) merged.push_back(empty);
            merged[j] = clothParts[i];
            clothToMerged[i] = static_cast<int>(j);
            ++rep.accessoriesAdded;
            ++rep.accessoriesRelocated;
            continue;
        }
        merged[i] = clothParts[i];
        clothToMerged[i] = static_cast<int>(i);
        ++rep.accessoriesAdded;
    }
    // Count every character hair accessory that survived (kept or relocated around).
    {
        rep.hairPreserved = 0;
        for (size_t i = 0; i < charParts.size(); ++i)
            if (!isEmptyAcc(charParts[i]) && IsHairAccessoryPart(charParts[i], card.rows, outfit, static_cast<int>(i))) ++rep.hairPreserved;
    }
    }   // end of the merge (non-coordinate) path
    // Write the merged list into the target outfit, keeping its version.
    if (target.accessory.isMap()) {
        for (auto& kv : target.accessory.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts") kv.second = Value(merged);
    } else {
        Value::Map acc;
        acc.emplace_back(Value(std::string("version")), clothAcc.at("version"));
        acc.emplace_back(Value(std::string("parts")), Value(merged));
        target.accessory = Value(std::move(acc));
    }

    // --- resolver rows: drop what was replaced, add the outfit's ----------
    const std::string prefix = "outfit" + std::to_string(outfit) + ".";
    std::set<int> replacedAcc;
    if (opt.coordinateImport) {
        // Every slot the file declares counts as replaced (including ones the file leaves empty - they clear the target's):
        // rows must not survive a coordinate that is gone wholesale.
        for (size_t i = 0; i < clothToMerged.size() && i < merged.size(); ++i) replacedAcc.insert(static_cast<int>(i));
    } else {
        for (size_t i = 0; i < charParts.size() && i < merged.size(); ++i) {
            const bool same = (isEmptyAcc(charParts[i]) && isEmptyAcc(merged[i])) ||
                              (!isEmptyAcc(charParts[i]) && !isEmptyAcc(merged[i]) &&
                               charParts[i].at("type").asInt() == merged[i].at("type").asInt() &&
                               charParts[i].at("id").asInt() == merged[i].at("id").asInt());
            if (!same) replacedAcc.insert(static_cast<int>(i));
        }
    }
    std::vector<ResolveRow> kept;
    for (const auto& rr : card.rows) {
        bool drop = false;
        if (rr.property.rfind(prefix, 0) == 0 && rr.property.find("ChaFileClothes.") != std::string::npos) drop = true;
        if (!drop && rr.property.rfind(prefix, 0) == 0 && rr.property.find("ChaFileAccessory.") != std::string::npos) {
            const int idx = accessoryIndexOfProperty(rr.property);
            if (idx >= 0 && replacedAcc.count(idx)) drop = true;
        }
        if (!drop) kept.push_back(rr);
    }
    card.rows = std::move(kept);
    std::vector<ResolveRow> appended;
    for (const auto& rr : clothes.rows) {
        const bool isClothesRow = rr.property.find("ChaFileClothes.") != std::string::npos;
        const bool isAccRow = rr.property.find("ChaFileAccessory.") != std::string::npos;
        if (!isClothesRow && !isAccRow) continue;
        ResolveRow nr = rr;
        if (isClothesRow) {
            nr.property = withOutfitPrefix(rr.property, outfit);
        } else {
            const int orig = accessoryIndexOfProperty(rr.property);
            if (orig < 0 || static_cast<size_t>(orig) >= clothToMerged.size() || clothToMerged[static_cast<size_t>(orig)] < 0) continue;
            nr.property = prefix + "accessory" + std::to_string(clothToMerged[static_cast<size_t>(orig)]) + ".ChaFileAccessory.PartsInfo.id";
        }
        bool exists = false;
        for (const auto& e : card.rows) if (e.property == nr.property && e.slot == nr.slot) { exists = true; break; }
        if (!exists) {
            appended.push_back(nr);
            card.rows.push_back(std::move(nr));
        }
    }
    // Keep the extended universalautoresolver payload consistent with the merged rows (writes the rows a re-export carries).
    syncExtendedResolvedRows(card, outfit, appended, &replacedAcc);
    // --- plugin payloads: the source's bone mods and material edits for the outfit ---
    {
        const bool coordinateMode = opt.coordinateImport;   // the file's coordinate is the outfit wholesale
        // A real outfit file IS its one coordinate: its coordinate-indexed payloads are read regardless of the index the author saved them under.
        const int srcC = clothes.singleCoordinateSource ? -1 : clothes.srcCoordinate;
        mergeAbmxPayloads(card, clothes.extended, rep.notes, clothes.srcCoordinate, outfit,
                          static_cast<int>(card.outfits.size()), /*coordinateMode=*/coordinateMode);
        mergeMaterialEditorPayloads(card, clothes.extended, outfit, clothToMerged, srcC, replacedAcc,
                                    /*dropAllAccRows=*/coordinateMode, rep.notes);
        mergeKcoxPayloads(card, clothes.extended, srcC, outfit, rep.notes);
        mergeDynamicBonePayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                 /*coordinateImport=*/coordinateMode, replacedAcc, rep.notes);
        mergeHairAccessoryPayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                   /*coordinateImport=*/coordinateMode, replacedAcc, rep.notes);
        mergeAccessoryStatesPayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                     /*coordinateImport=*/coordinateMode, rep.notes);
        mergePushupPayloads(card, clothes.extended, srcC, outfit, rep.notes);
        mergeAdditionalCardInfo(card, clothes.extended, srcC, clothes.srcOutfitCount, outfit,
                                static_cast<int>(card.outfits.size()), /*coordinateImport=*/coordinateMode, rep.notes);
    }
    // A coordinate import replaces the outfit wholesale: the target's own
    // MoreAccessories block for this outfit is stale (the file's extras
    // travel in the file's parts), so it is dropped like a Coordinate import does.
    if (opt.coordinateImport) dropOutfitMoreAccessoriesXml(card, outfit, rep.notes);
    // Added-clothing provenance: which assembled parts came from the file.
    // Their ScenePart.group stays Outfit/Accessories (the exporter's Take-off
    // and mask logic key on it); the fromClothing flag is what the viewer
    // lists separately under "Added outfit".
    card.hasClothing = true;
    card.clothingName = clothes.name.empty() ? clothes.path : clothes.name;
    card.clothingPath = clothes.path;
    card.clothingOutfit = outfit;
    card.clothingPartIds.clear();
    {
        static const char* kSlot[9] = {"top", "bottom", "bra", "shorts", "gloves", "pantyhose", "socks", "shoes_inner", "shoes_outer"};
        const Value& cp = target.clothes.at("parts");
        if (cp.isArray())
            for (size_t i = 0; i < cp.size() && i < 9; ++i) {
                if (cp.at(i).at("id").asInt(0) == 0) continue;
                card.clothingPartIds.insert("outfit" + std::to_string(outfit) + "." + kSlot[i]);
            }
        for (size_t i = 0; i < clothToMerged.size(); ++i) {
            const int merged = clothToMerged[i];
            if (merged < 0) continue;
            char num[8];
            std::snprintf(num, sizeof num, "%02d", merged);
            card.clothingPartIds.insert("outfit" + std::to_string(outfit) + ".acc" + num);
        }
    }
    // The in-memory merge is all the viewer and the exporter read (Resolver
    // uses rows, not the extended bin); the file on disk is untouched.
    return rep;
}

} // namespace cardbridge::card
