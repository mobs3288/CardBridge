#include "card/CardFile.h"
#include <cstdlib>

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
        Bytes b = r.bytes(static_cast<size_t>(n));
        return MsgPackParse(b);
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

        auto blockData = [&](const std::string& name) -> Bytes {
            for (const auto& b : c.blocks)
                if (b.name == name) {
                    ByteReader br(file.data() + end + dataStart + b.pos, static_cast<size_t>(b.size), false);
                    return br.bytes(static_cast<size_t>(b.size));
                }
            return {};
        };

        Bytes custom = blockData("Custom");
        if (custom.empty()) throw FormatError("card has no Custom section");
        ByteReader cr(custom, false);
        Value* parts[3] = {&c.face, &c.body, &c.hair};
        for (Value* p : parts) {
            int32_t n = cr.i32();
            if (n < 0 || static_cast<size_t>(n) > cr.remaining()) throw FormatError("Custom section runs past its data");
            Bytes b = cr.bytes(static_cast<size_t>(n));
            *p = MsgPackParse(b);
        }

        Bytes coord = blockData("Coordinate");
        if (coord.empty()) throw FormatError("card has no Coordinate section");
        Value arr = MsgPackParse(coord);
        for (const auto& el : arr.asArray()) {
            Outfit o;
            decodeCoordinate(el.asBin(), o);
            c.outfits.push_back(std::move(o));
        }

        Bytes param = blockData("Parameter");
        if (!param.empty()) c.parameter = MsgPackParse(param);
        Bytes status = blockData("Status");
        if (!status.empty()) c.status = MsgPackParse(status);

        Bytes kkex = blockData("KKEx");
        if (!kkex.empty()) {
            c.extended = MsgPackParse(kkex);
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

} // namespace cardbridge::card
