#include "card/CardFile.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

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

void appendMoreAccessories(Card& c) {
    const Value& more = c.extended.at("moreAccessories").at(size_t(1)).at("additionalAccessories");
    if (!more.isStr()) return;
    const std::string& xml = more.asStr();
    int coordinate = -1;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos, end - pos);
        pos = end + 1;
        if (tag.compare(0, 13, "<accessorySet") == 0) { coordinate = std::atoi(xmlAttr(tag, "type").c_str()); continue; }
        if (tag.compare(0, 10, "<accessory") != 0 || tag.compare(0, 13, "<accessorySet") == 0) continue;
        if (coordinate < 0 || static_cast<size_t>(coordinate) >= c.outfits.size()) continue;
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
        Value& acc = c.outfits[static_cast<size_t>(coordinate)].accessory;
        if (!acc.isMap()) continue;
        for (auto& kv : acc.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts" && kv.second.isArray()) kv.second.array().emplace_back(Value(std::move(part)));
    }
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

std::string ClothingFile::displayName() const { return name; }

namespace {

bool isSunshineClothesMarker(const std::string& marker) {
    // 【KoiKatuClothesSun】 (and any other Sun variant): the container has not
    // been verified, like a Sunshine character.
    return marker.find("Sun") != std::string::npos || marker.find("SUN") != std::string::npos;
}

// MoreAccessories for one outfit (a clothing file has a single coordinate 0).
// Slots 20 + n are appended to the outfit's accessory parts, same as a card.
void appendMoreAccessoriesToOutfit(Outfit& outfit, const Value& extended) {
    const Value& more = extended.at("moreAccessories").at(size_t(1)).at("additionalAccessories");
    if (!more.isStr()) return;
    const std::string& xml = more.asStr();
    int coordinate = -1;
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos, end - pos);
        pos = end + 1;
        if (tag.compare(0, 13, "<accessorySet") == 0) { coordinate = std::atoi(xmlAttr(tag, "type").c_str()); continue; }
        if (tag.compare(0, 10, "<accessory") != 0 || tag.compare(0, 13, "<accessorySet") == 0) continue;
        if (coordinate != 0) continue;   // a clothing file's single outfit is coordinate 0
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

} // namespace

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

    c.supported = true;
    c.sha256 = Sha256Hex(file);
    return c;
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

    const Value empty = emptyAccPart(charAcc, clothAcc);
    Value::Array merged = charParts;
    const size_t nCloth = clothParts.size();
    while (merged.size() < nCloth) merged.push_back(empty);
    std::vector<int> clothToMerged(nCloth, -1);
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
    for (size_t i = 0; i < charParts.size() && i < merged.size(); ++i) {
        const bool same = isEmptyAcc(charParts[i]) && isEmptyAcc(merged[i]) ||
                          (!isEmptyAcc(charParts[i]) && !isEmptyAcc(merged[i]) &&
                           charParts[i].at("type").asInt() == merged[i].at("type").asInt() &&
                           charParts[i].at("id").asInt() == merged[i].at("id").asInt());
        if (!same) replacedAcc.insert(static_cast<int>(i));
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
        if (!exists) card.rows.push_back(std::move(nr));
    }
    // The in-memory merge is all the viewer and the exporter read (Resolver
    // uses rows, not the extended bin); the file on disk is untouched.
    return rep;
}

} // namespace cardbridge::card
