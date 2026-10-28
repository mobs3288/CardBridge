#include "card/CardFile.h"

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
