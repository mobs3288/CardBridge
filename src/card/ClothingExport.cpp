// cardbridge - writes a Koikatsu *outfit* file (【KoiKatuClothes】 .png) from a
// character card's coordinate. Inverse of LoadClothingFile / ApplyClothing.
// Design and rationale: docs/CLOTHING_EXPORT.md.

#include "card/ClothingExport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <map>
#include <set>
#include <unordered_map>

#include "card/Abmx.h"
#include "core/FileIo.h"
#include "core/MsgPack.h"
#include "core/Sha256.h"

namespace cardbridge::card {

namespace {

constexpr int kFixedAccessorySlots = 20;
constexpr const char* kUarKey = "com.bepis.sideloader.universalautoresolver";
constexpr const char* kMeKey = "com.deathweasel.bepinex.materialeditor";
constexpr const char* kKcoxKey = "KCOX";
constexpr const char* kMoreAccKey = "moreAccessories";
constexpr const char* kInfoKey = "Additional_Card_Info";
constexpr const char* kAbmxKey = "KKABMPlugin.ABMData";

// ---- bytes ------------------------------------------------------------------

void putInt(std::vector<uint8_t>& out, int32_t v) {
    for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>(v & 0xff)); v >>= 8; }
}
void putU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void putDotnetString(std::vector<uint8_t>& out, const std::string& s) {
    // .NET BinaryReader.ReadString: 7-bit-encoded varint length, then UTF-8.
    uint32_t n = static_cast<uint32_t>(s.size());
    while (n > 0x7f) { out.push_back(static_cast<uint8_t>((n & 0x7f) | 0x80)); n >>= 7; }
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), s.begin(), s.end());
}
void putBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& b) {
    out.insert(out.end(), b.begin(), b.end());
}
void putChunk(std::vector<uint8_t>& out, const Bytes& b) { putInt(out, static_cast<int32_t>(b.size())); putBytes(out, b); }

// Offset of the byte after the PNG's IEND chunk; SIZE_MAX when invalid.
size_t pngEndOffset(const std::vector<uint8_t>& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (b.size() < 8 || std::memcmp(b.data(), sig, 8) != 0) return static_cast<size_t>(-1);
    size_t r = 8;
    while (true) {
        if (r + 12 > b.size()) return static_cast<size_t>(-1);
        uint32_t len = 0;
        for (int i = 0; i < 4; ++i) len = (len << 8) | b[r + i];   // PNG is big-endian
        std::string type(reinterpret_cast<const char*>(b.data() + r + 4), 4);
        size_t next = r + 12 + static_cast<size_t>(len);
        if (next > b.size()) return static_cast<size_t>(-1);
        r = next;
        if (type == "IEND") return r;
    }
}

std::string fullPath(const std::string& p) {
    char buf[_MAX_PATH];
    if (_fullpath(buf, p.c_str(), _MAX_PATH)) return buf;
    return p;
}

// ---- accessories -------------------------------------------------------------

bool isEmptyPart(const Value& part) {
    if (!part.isMap()) return true;
    const int type = static_cast<int>(part.at("type").asInt(120));
    const int64_t id = part.at("id").asInt(0);
    return type == 120 || id == 0;
}

Value makeEmptyPart() {
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
    part.emplace_back(Value(std::string("color")), Value(std::move(color)));
    part.emplace_back(Value(std::string("addMove")), Value(std::move(addMove)));
    return Value(std::move(part));
}

// 20 showAccessory flags out of Status: 0 = hidden. A missing or odd
// array hides nothing.
std::vector<int8_t> parseShowAccessory(const Value& status) {
    std::vector<int8_t> flags;
    const Value& v = status.at("showAccessory");
    if (v.isBin() && v.asBin().size() >= static_cast<size_t>(kFixedAccessorySlots))
        for (size_t i = 0; i < v.asBin().size(); ++i) flags.push_back(v.asBin()[i] ? 1 : 0);
    else if (v.isArray())
        for (const auto& e : v.asArray()) flags.push_back(static_cast<int8_t>(e.asInt(0) ? 1 : 0));
    return flags;
}

// ---- id re-localisation ------------------------------------------------------

// slot -> LocalSlot for one outfit, per domain.
struct RowMaps {
    std::map<int64_t, int64_t> clothes;                    // any ChaFileClothes row for this outfit
    std::map<int, std::map<int64_t, int64_t>> acc;         // per accessory slot (any index)
};

RowMaps buildRowMaps(const Card& card, int outfit) {
    RowMaps rm;
    const std::string prefix = "outfit" + std::to_string(outfit) + ".";
    for (const auto& rr : card.rows) {
        if (rr.property.rfind(prefix, 0) != 0 || rr.localSlot == 0) continue;
        const std::string rest = rr.property.substr(prefix.size());   // "ChaFileClothes.ClothesTop" / "accessory3..."
        if (rest.rfind("ChaFileClothes.", 0) == 0) { rm.clothes.emplace(rr.slot, rr.localSlot); continue; }
        if (rest.rfind("accessory", 0) == 0) {
            size_t i = 9;
            if (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i]))) {
                const int slot = std::atoi(rest.c_str() + i);
                rm.acc[slot].emplace(rr.slot, rr.localSlot);           // first wins
            }
        }
    }
    return rm;
}

int64_t localise(const std::map<int64_t, int64_t>& m, int64_t id, bool* changed) {
    auto it = m.find(id);
    if (it == m.end()) { *changed = false; return id; }
    *changed = it->second != id;
    return it->second;
}

void setMapInt(Value& map, const char* key, int64_t v) {
    if (!map.isMap()) return;
    for (auto& kv : map.map())
        if (kv.first.isStr() && kv.first.asStr() == key) { kv.second = Value(v); return; }
}

// ---- MoreAccessories XML ------------------------------------------------------

// Attribute value `name="..."` of one tag text; empty when absent.
std::string xmlAttr(const std::string& tag, const char* name) {
    const std::string key = std::string(" ") + name + "=\"";
    size_t at = tag.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    size_t end = tag.find('"', at);
    return end == std::string::npos ? std::string() : tag.substr(at, end - at);
}

struct XmlElement {
    size_t begin = 0, end = 0;   // [begin, end) offsets into the source string
    int srcSlot = -1;            // positional slot index (20 + position)
};
struct MoreAccessories {
    bool present = false;       // an <additionalAccessories> root was found
    std::string version;        // root version attribute, empty when absent
    std::vector<XmlElement> elements;
};

// Extract this outfit's <accessory ...> elements from the card's
// additionalAccessories string. Handles the per-coordinate <accessorySet
// type="N"> wrapper (cards) and the flat positional form (coordinate files,
// which cards should not contain but are accepted defensively).
MoreAccessories extractMoreAccessories(const std::string& xml, int outfit) {
    MoreAccessories out;
    if (xml.empty()) return out;
    const size_t rootAt = xml.find("<additionalAccessories");
    if (rootAt == std::string::npos) return out;
    const size_t rootEnd = xml.find('>', rootAt);
    if (rootEnd == std::string::npos) return out;
    out.present = true;
    out.version = xmlAttr(xml.substr(rootAt, rootEnd - rootAt + 1), "version");

    // Gather all top-level tags.
    struct Span { size_t begin, end; };
    std::vector<Span> tags;
    for (size_t p = 0; (p = xml.find('<', p)) != std::string::npos;) {
        const size_t close = xml.find('>', p);
        if (close == std::string::npos) break;
        tags.push_back({p, close + 1});
        p = close + 1;
    }

    std::vector<Span> picked;
    const bool hasAccessorySet = xml.find("<accessorySet") != std::string::npos;
    if (hasAccessorySet) {
        int current = -1;
        for (const auto& t : tags) {
            const std::string tag = xml.substr(t.begin, t.end - t.begin);
            if (tag.compare(0, 13, "<accessorySet") == 0) { current = std::atoi(xmlAttr(tag, "type").c_str()); continue; }
            if (tag.rfind("</accessorySet", 0) == 0) { current = -1; continue; }
            if (tag.rfind("<accessory", 0) == 0 && current == outfit) picked.push_back(t);
        }
    } else {
        // Flat: positional; coordinate attribute (unverified) selects or the whole set is taken.
        bool sawCoordAttr = false;
        for (const auto& t : tags) {
            const std::string tag = xml.substr(t.begin, t.end - t.begin);
            if (tag.rfind("<accessory", 0) != 0) continue;
            const std::string c = xmlAttr(tag, "coordinate");
            if (!c.empty()) sawCoordAttr = true;
        }
        for (const auto& t : tags) {
            const std::string tag = xml.substr(t.begin, t.end - t.begin);
            if (tag.rfind("<accessory", 0) != 0) continue;
            const std::string c = xmlAttr(tag, "coordinate");
            if (!sawCoordAttr || c.empty() || std::atoi(c.c_str()) == outfit) picked.push_back(t);
        }
    }

    int slot = kFixedAccessorySlots;
    for (const auto& e : picked) out.elements.push_back({e.begin, e.end, slot++});
    return out;
}

int xmlElementId(const std::string& xml, const XmlElement& e) {
    std::string tag = xml.substr(e.begin, e.end - e.begin);
    std::string v = xmlAttr(tag, "id");
    if (v.empty()) return -1;
    return std::atoi(v.c_str());
}

// Rewrite the numeric `id="..."` attribute of one element in place (in the
// source string). Only used when building the output from a copied tag, so the
// offsets stay valid.
std::string xmlElementRewrittenId(const std::string& xml, const XmlElement& e, int64_t newId) {
    std::string tag = xml.substr(e.begin, e.end - e.begin);
    size_t idAt = tag.find(" id=\"");
    if (idAt == std::string::npos) return tag;
    idAt += 5;
    size_t end = tag.find('"', idAt);
    if (end == std::string::npos) return tag;
    char nbuf[32];
    std::snprintf(nbuf, sizeof nbuf, "%lld", static_cast<long long>(newId));
    tag.replace(idAt, end - idAt, nbuf);
    return tag;
}

// ---- the export context --------------------------------------------------------

struct ExportContext {
    const Card* card = nullptr;
    const ClothingExportOptions& opt;
    int outfit = -1;
    ClothingExportReport rep;
    RowMaps rows;
    std::vector<int8_t> showAccessory;
    std::set<int> omitted;                 // accessory indices omitted
    std::map<int64_t, int64_t> accRenumber;  // old index -> new index (only for slots >= 20)
    ExportContext(const Card& c, const ClothingExportOptions& o) : card(&c), opt(o) {}
};

void decideOmissions(ExportContext& ctx, const std::vector<Value>& parts) {
    const int total = static_cast<int>(parts.size());
    for (int i = 0; i < total; ++i) {
        bool omit = false;
        bool hair = false;
        const Value& p = parts[static_cast<size_t>(i)];
        if (!isEmptyPart(p)) hair = IsHairAccessoryPart(p, ctx.card->rows, ctx.outfit, i);
        const bool hiddenByCard = i < kFixedAccessorySlots && i < static_cast<int>(ctx.showAccessory.size())
                                  && ctx.showAccessory[static_cast<size_t>(i)] == 0;
        if (!ctx.opt.includeHidden && hiddenByCard) omit = true;
        if (ctx.opt.hiddenSlots.count(i)) omit = true;                 // user-specified: wins over nothing
        if (!ctx.opt.withHairAccessories && hair) omit = true;
        if (omit) ctx.omitted.insert(i);
        if (hair && omit) ++ctx.rep.hairAccessoriesDropped;
        else if (omit) ++ctx.rep.hiddenAccessoriesDropped;
    }
    int ni = kFixedAccessorySlots;
    for (int i = kFixedAccessorySlots; i < total; ++i) {
        if (ctx.omitted.count(i)) continue;
        ctx.accRenumber[i] = ni++;
    }
}

int compactedIndex(const ExportContext& ctx, int slot) {
    auto it = ctx.accRenumber.find(slot);
    return it == ctx.accRenumber.end() ? slot : static_cast<int>(it->second);
}

}   // namespace

Bytes BuildClothingFileBytes(const Card& card, const ClothingExportOptions& opt, ClothingExportReport* report) {
    if (report) *report = ClothingExportReport{};
    if (!card.supported) throw FormatError("this card is not a convertible Koikatsu card (" + card.supportNote + ")");
    if (card.outfits.empty()) throw FormatError("the card has no outfits");
    int outfit = opt.outfit;
    if (outfit < 0) outfit = card.status.isMap() ? static_cast<int>(card.status.at("coordinateType").asInt()) : 0;
    if (outfit < 0 || outfit >= static_cast<int>(card.outfits.size()))
        throw FormatError("outfit " + std::to_string(outfit) + " is out of range; the card has " + std::to_string(card.outfits.size()) + " outfit(s)");

    ExportContext ctx(card, opt);
    ctx.outfit = outfit;
    ctx.rows = buildRowMaps(card, outfit);
    ctx.showAccessory = parseShowAccessory(card.status);

    std::string name = opt.name;
    if (name.empty()) {
        name = card.displayName();
        const std::string slot = card.outfitName(outfit);
        name = name.empty() ? slot : name + " - " + slot;
    }
    ctx.rep.outfitName = name;
    ctx.rep.outfit = outfit;

    const auto& srcOutfit = card.outfits[static_cast<size_t>(outfit)];
    std::vector<Value> parts;
    if (srcOutfit.accessory.isMap()) parts = srcOutfit.accessory.at("parts").asArray();
    decideOmissions(ctx, parts);

    // ---- coordinates ----------------------------------------------------------
    Bytes coord;
    // Clothes: re-localise garment / pattern / sub-part / emblem ids.
    Value clothes = srcOutfit.clothes;
    if (clothes.isMap()) {
        for (auto& kv : clothes.map()) {
            if (!kv.first.isStr()) continue;
            const std::string& key = kv.first.asStr();
            if (key == "parts" && kv.second.isArray()) {
                for (auto& part : kv.second.array()) {
                    if (!part.isMap()) continue;
                    bool changed = false;
                    const int64_t id = part.at("id").asInt(0);
                    if (id != 0) {
                        int64_t nl = localise(ctx.rows.clothes, id, &changed);
                        if (changed) { setMapInt(part, "id", nl); ++ctx.rep.clothesIdsLocalized; }
                    }
                    // colourInfo pattern ids (the pattern rows are clothes-domain).
                    for (auto& pk : part.map()) {
                        if (!pk.first.isStr()) continue;
                        if (pk.first.asStr() == "colorInfo" && pk.second.isArray()) {
                            for (auto& ci : pk.second.array()) {
                                if (!ci.isMap()) continue;
                                for (auto& ck : ci.map()) {
                                    if (!ck.first.isStr() || ck.first.asStr() != "pattern") continue;
                                    int64_t pat = ck.second.asInt(0);
                                    bool chg = false;
                                    int64_t nl = localise(ctx.rows.clothes, pat, &chg);
                                    if (chg) { ck.second = Value(nl); ++ctx.rep.clothesIdsLocalized; }
                                }
                            }
                        } else if ((pk.first.asStr() == "emblemeId" || pk.first.asStr() == "emblemeId2") && pk.second.isInt()) {
                            int64_t cur = pk.second.asInt(0);
                            if (cur != 0) {
                                bool chg = false;
                                int64_t nl = localise(ctx.rows.clothes, cur, &chg);
                                if (chg) { pk.second = Value(nl); ++ctx.rep.clothesIdsLocalized; }
                            }
                        }
                    }
                }
            } else if (key == "subPartsId" && kv.second.isArray()) {
                for (auto& e : kv.second.array()) {
                    int64_t cur = e.asInt(0);
                    bool chg = false;
                    int64_t nl = localise(ctx.rows.clothes, cur, &chg);
                    if (chg) { e = Value(nl); ++ctx.rep.subPartsLocalized; }
                }
            }
        }
    }

    // Accessory: slots 0-19 stay (omitted ones become empty parts); slots 20+
    // are carried only by the MoreAccessories XML.
    std::vector<Value> keptParts;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (static_cast<int>(i) >= kFixedAccessorySlots) break;
        if (ctx.omitted.count(static_cast<int>(i))) { keptParts.push_back(makeEmptyPart()); continue; }
        Value p = parts[i];
        const int64_t id = p.at("id").asInt(0);
        if (id != 0 && p.isMap()) {
            auto it = ctx.rows.acc.find(static_cast<int>(i));
            if (it != ctx.rows.acc.end()) {
                auto it2 = it->second.find(id);
                if (it2 != it->second.end() && it2->second != id) {
                    setMapInt(p, "id", it2->second);
                    ++ctx.rep.accessoryIdsLocalized;
                }
            }
        }
        keptParts.push_back(std::move(p));
    }
    while (keptParts.size() < static_cast<size_t>(kFixedAccessorySlots)) keptParts.push_back(makeEmptyPart());

    Value accessory = srcOutfit.accessory;
    bool accessoryWritten = false;
    if (accessory.isMap()) {
        for (auto& kv : accessory.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts") { kv.second = Value(std::move(keptParts)); accessoryWritten = true; }
    }
    if (!accessoryWritten) {
        Value::Map accMap;
        accMap.emplace_back(Value(std::string("version")), Value(int64_t{1}));
        accMap.emplace_back(Value(std::string("parts")), Value(std::move(keptParts)));
        accessory = Value(std::move(accMap));
    }

    Value makeup = srcOutfit.makeup;
    const bool enableMakeup = srcOutfit.enableMakeup;

    putChunk(coord, MsgPackPack(clothes));
    putChunk(coord, MsgPackPack(accessory));
    putU8(coord, enableMakeup ? 1 : 0);
    putChunk(coord, MsgPackPack(makeup));

    // ---- extras (MoreAccessories) ----------------------------------------------
    std::string extrasBody;      // contents of <additionalAccessories> (or empty)
    std::string extrasVersion;   // source version attribute, empty when absent
    bool emitMoreAccessories = false;
    {
        std::string src;
        const Value& more = card.extended.at(kMoreAccKey);
        if (more.isArray() && more.size() >= 2 && more.at(size_t(1)).isMap()) {
            const Value& s = more.at(size_t(1)).at("additionalAccessories");
            if (s.isStr()) src = s.asStr();
        }
        MoreAccessories mx = extractMoreAccessories(src, outfit);
        int extrasNonEmpty = 0;
        for (size_t i = kFixedAccessorySlots; i < parts.size(); ++i)
            if (!isEmptyPart(parts[i])) ++extrasNonEmpty;
        if (extrasNonEmpty > 0 && mx.elements.size() < static_cast<size_t>(extrasNonEmpty)) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "the card claims %d non-empty accessory slot(s) beyond 20 but its MoreAccessories XML has only %zu (corrupt or hand-edited card)", extrasNonEmpty, mx.elements.size());
            throw FormatError(buf);
        }
        extrasVersion = mx.version;
        for (const auto& e : mx.elements) {
            if (ctx.omitted.count(e.srcSlot)) continue;
            const int64_t elemId = xmlElementId(src, e);
            std::string tag = src.substr(e.begin, e.end - e.begin);
            if (elemId > 0) {
                auto it = ctx.rows.acc.find(e.srcSlot);
                if (it != ctx.rows.acc.end()) {
                    auto it2 = it->second.find(elemId);
                    if (it2 != it->second.end() && it2->second != elemId) {
                        tag = xmlElementRewrittenId(src, e, it2->second);
                        ++ctx.rep.accessoryIdsLocalized;
                    }
                }
            }
            extrasBody += tag;
            ++ctx.rep.extraAccessoriesKept;
        }
        if (ctx.rep.extraAccessoriesKept > 0) emitMoreAccessories = true;
        else if (mx.present && extrasNonEmpty > 0) emitMoreAccessories = false;   // everything omitted: nothing to carry
        else if (mx.present) emitMoreAccessories = true;                        // self-closing block keeps the plugin state consistent
    }
    // assemble the moreAccessories entry from extrasBody + version
    Value moreAccessoriesEntry;
    if (emitMoreAccessories) {
        std::string xml;
        if (extrasBody.empty()) {
            xml = "<additionalAccessories";
            if (!extrasVersion.empty()) xml += " version=\"" + extrasVersion + "\"";
            xml += " />";
        } else {
            xml = "<additionalAccessories";
            if (!extrasVersion.empty()) xml += " version=\"" + extrasVersion + "\"";
            xml += ">" + extrasBody + "</additionalAccessories>";
        }
        const Value& more = card.extended.at(kMoreAccKey);
        const int64_t maVersion = more.isArray() && more.size() > 0 ? more.at(size_t(0)).asInt(2) : 2;
        Value::Map morePayload;
        morePayload.emplace_back(Value(std::string("additionalAccessories")), Value(std::move(xml)));
        moreAccessoriesEntry = Value(Value::Array{Value(maVersion), Value(std::move(morePayload))});
    }

    // ---- KKEx -------------------------------------------------------------------
    Value::Map kkex;

    // UAR: keep rows for this outfit, strip the prefix, re-number compacted extras.
    {
        const Value& uar = card.extended.at(kUarKey);
        if (uar.isArray() && uar.size() >= 2 && uar.at(size_t(1)).isMap()) {
            const int64_t ver = uar.at(size_t(0)).asInt(0);
            const Value& payload = uar.at(size_t(1));
            const Value& info = payload.at("info");
            const std::string prefix = "outfit" + std::to_string(outfit) + ".";
            Value::Array rows;
            auto accessoryIndexOf = [](const std::string& p) -> int {
                const std::string key = "accessory";
                size_t at = p.find(key);
                if (at == std::string::npos) return -1;
                at += key.size();
                if (at >= p.size() || !std::isdigit(static_cast<unsigned char>(p[at]))) return -1;
                return std::atoi(p.c_str() + at);
            };
            for (const auto& raw : info.asArray()) {
                if (!raw.isBin()) continue;
                Value row;
                try { row = MsgPackParse(raw.asBin()); } catch (const FormatError&) { continue; }
                if (!row.isMap()) continue;
                const std::string prop = row.at("Property").asStr();
                if (prop.rfind(prefix, 0) != 0) continue;
                const int accIdx = accessoryIndexOf(prop);
                if (accIdx >= 0 && ctx.omitted.count(accIdx)) continue;
                std::string stripped = prop.substr(prefix.size());
                if (accIdx >= kFixedAccessorySlots) {
                    char nbuf[64];
                    std::snprintf(nbuf, sizeof nbuf, "accessory%d.ChaFileAccessory.PartsInfo.id", compactedIndex(ctx, accIdx));
                    stripped = nbuf;
                }
                for (auto& kv : row.map())
                    if (kv.first.isStr() && kv.first.asStr() == "Property") kv.second = Value(std::move(stripped));
                rows.emplace_back(Value(MsgPackPack(row)));
                ++ctx.rep.uarRowsKept;
            }
            if (!rows.empty()) {
                Value::Map payloadOut = payload.asMap();
                bool found = false;
                for (auto& kv : payloadOut)
                    if (kv.first.isStr() && kv.first.asStr() == "info") { kv.second = Value(std::move(rows)); found = true; }
                if (!found) payloadOut.emplace_back(Value(std::string("info")), Value(std::move(rows)));
                kkex.emplace_back(Value(std::string(kUarKey)), Value(Value::Array{Value(ver), Value(std::move(payloadOut))}));
            }
        }
    }

    // MaterialEditor: rows for this coordinate only, rebased to 0; subset the texture dictionary.
    {
        const Value& me = card.extended.at(kMeKey);
        if (me.isArray() && me.size() >= 2 && me.at(size_t(1)).isMap()) {
            const int64_t ver = me.at(size_t(0)).asInt(0);
            Value::Map payload = me.at(size_t(1)).asMap();
            bool keptAnything = false;
            std::set<int64_t> usedTex;

            static const char* kMeLists[] = {"RendererPropertyList", "MaterialFloatPropertyList", "MaterialColorPropertyList",
                                             "MaterialTexturePropertyList", "MaterialShaderList", "MaterialNamePropertyList",
                                             "MaterialKeywordPropertyList", "ProjectorPropertyList", "MaterialCopyList"};
            for (const char* key : kMeLists) {
                Value listBin;
                for (const auto& kv : payload) if (kv.first.isStr() && kv.first.asStr() == key) { listBin = kv.second; break; }
                if (!listBin.isBin() || listBin.asBin().empty()) continue;
                Value list;
                try { list = MsgPackParse(listBin.asBin()); } catch (const FormatError&) { continue; }   // keep verbatim on parse failure
                if (!list.isArray()) continue;
                Value::Array keptRows;
                for (const auto& rowConst : list.asArray()) {
                    if (!rowConst.isMap()) { keptRows.emplace_back(rowConst); continue; }   // keep unparseable rows untouched
                    Value row = rowConst;   // mutation copy
                    const int64_t coordIdx = row.at("CoordinateIndex").asInt(-1);
                    if (coordIdx == -1) { keptRows.emplace_back(std::move(row)); continue; }   // no coordinate: verbatim (e.g. MaterialCopyList may be flat)
                    if (coordIdx != static_cast<int64_t>(outfit)) continue;
                    const int64_t objectType = row.at("ObjectType").asInt(-1);
                    if (objectType != -1 && objectType != 1 && objectType != 2) continue;   // hair/character/studio rows are not garment paint
                    const int64_t slot = row.at("Slot").asInt(-1);
                    if (objectType == 2 && slot >= 0) {
                        if (ctx.omitted.count(static_cast<int>(slot))) continue;
                        if (slot >= kFixedAccessorySlots) {
                            const int ni = compactedIndex(ctx, static_cast<int>(slot));
                            setMapInt(row, "Slot", static_cast<int64_t>(ni));
                        }
                    }
                    setMapInt(row, "CoordinateIndex", 0);
                    int64_t texId = -1;
                    if (std::string(key) == "MaterialTexturePropertyList") {
                        const Value& t = row.at("TexID");
                        if (t.isInt() && t.asInt() >= 0) texId = t.asInt();
                    }
                    keptRows.emplace_back(std::move(row));
                    ++ctx.rep.meListRows;
                    keptAnything = true;
                    if (texId >= 0) usedTex.insert(texId);
                }
                for (auto& kv : payload)
                    if (kv.first.isStr() && kv.first.asStr() == key) kv.second = Value(MsgPackPack(Value(std::move(keptRows))));
            }

            // TextureDictionary: keep only entries referenced by kept texture rows; drop the key when none.
            Value dictBin;
            for (const auto& kv : payload) if (kv.first.isStr() && kv.first.asStr() == "TextureDictionary") { dictBin = kv.second; break; }
            if (dictBin.isBin() && !dictBin.asBin().empty()) {
                if (usedTex.empty()) {
                    payload.erase(std::remove_if(payload.begin(), payload.end(), [](const auto& kv) { return kv.first.isStr() && kv.first.asStr() == "TextureDictionary"; }), payload.end());
                } else {
                    try {
                        Value td = MsgPackParse(dictBin.asBin());
                        if (td.isMap()) {
                            Value::Map keptDict;
                            for (const auto& kv : td.map()) {
                                if (!kv.first.isInt()) continue;
                                if (usedTex.count(kv.first.asInt())) { keptDict.emplace_back(kv.first, kv.second); ++ctx.rep.meTexturesKept; }
                            }
                            for (auto& kv : payload)
                                if (kv.first.isStr() && kv.first.asStr() == "TextureDictionary") kv.second = Value(MsgPackPack(Value(std::move(keptDict))));
                        }
                    } catch (const FormatError&) {}
                }
            } else if (!dictBin.isBin() || dictBin.asBin().empty()) {
                payload.erase(std::remove_if(payload.begin(), payload.end(), [](const auto& kv) { return kv.first.isStr() && kv.first.asStr() == "TextureDictionary"; }), payload.end());
            }

            if (keptAnything)
                kkex.emplace_back(Value(std::string(kMeKey)), Value(Value::Array{Value(ver), Value(std::move(payload))}));
        }
    }

    // KCOX: overlays for this coordinate, written as the bare {garment -> [png, replace]} form.
    {
        const Value& kcox = card.extended.at(kKcoxKey);
        if (kcox.isArray() && kcox.size() >= 2 && kcox.at(size_t(1)).isMap()) {
            const int64_t ver = kcox.at(size_t(0)).asInt(1);
            const Value& overlays = kcox.at(size_t(1)).at("Overlays");
            if (overlays.isBin()) {
                Value parsed;
                try { parsed = MsgPackParse(overlays.asBin()); } catch (const FormatError&) {}
                if (parsed.isMap()) {
                    Value::Map garments;
                    auto add = [&](const std::string& n, const Value& arr) {
                        garments.emplace_back(Value(std::string(n)), Value(arr));
                        ++ctx.rep.kcoxGarments;
                    };
                    for (const auto& kv : parsed.asMap()) {
                        if (kv.first.isInt()) {
                            if (kv.first.asInt() != static_cast<int64_t>(outfit)) continue;
                            if (kv.second.isMap())
                                for (const auto& g : kv.second.asMap())
                                    if (g.first.isStr() && g.second.isArray()) add(g.first.asStr(), g.second);
                        } else if (kv.first.isStr() && kv.second.isArray() && kv.second.size() >= 2) {
                            add(kv.first.asStr(), kv.second);
                        }
                    }
                    if (!garments.empty()) {
                        Value::Map payloadOut;
                        payloadOut.emplace_back(Value(std::string("Overlays")), Value(MsgPackPack(Value(std::move(garments)))));
                        kkex.emplace_back(Value(std::string(kKcoxKey)), Value(Value::Array{Value(ver), Value(std::move(payloadOut))}));
                    }
                }
            }
        }
    }

    if (emitMoreAccessories) kkex.emplace_back(Value(std::string(kMoreAccKey)), std::move(moreAccessoriesEntry));

    {
        const Value& info = card.extended.at(kInfoKey);
        if (info.isArray() && info.size() >= 2) kkex.emplace_back(Value(std::string(kInfoKey)), Value(info));
    }
    if (ctx.opt.withAbmx) {
        const Value& abmx = card.extended.at(kAbmxKey);
        if (abmx.isArray() && abmx.size() >= 2) {
            kkex.emplace_back(Value(std::string(kAbmxKey)), Value(abmx));
            ctx.rep.warnings.emplace_back("KKABMPlugin.ABMData carried verbatim (the character's whole bone-mod table; applying this outfit re-applies all of it)");
        }
    }
    for (const auto& kv : card.extended.asMap()) {
        if (!kv.first.isStr()) continue;
        const std::string& k = kv.first.asStr();
        if (k == kUarKey || k == kMeKey || k == kKcoxKey || k == kMoreAccKey || k == kInfoKey) continue;
        if (k == kAbmxKey && ctx.opt.withAbmx) continue;   // consumed, not dropped
        ctx.rep.droppedPlugins.push_back(k);
    }

    // ---- assemble ---------------------------------------------------------------
    Bytes thumbnail;
    if (!opt.thumbnailPngPath.empty()) {
        thumbnail = fileio::ReadFile(opt.thumbnailPngPath);
        if (pngEndOffset(thumbnail) == static_cast<size_t>(-1))
            throw FormatError("--thumbnail is not a complete PNG (signature + IEND): " + opt.thumbnailPngPath);
    } else {
        thumbnail = card.faceThumbnail;
        if (thumbnail.empty() || pngEndOffset(thumbnail) == static_cast<size_t>(-1))
            throw FormatError("the card has no face thumbnail to reuse; pass --thumbnail <file.png>");
    }

    std::vector<uint8_t> out = std::move(thumbnail);
    putInt(out, card.productNo != 0 ? card.productNo : 100);
    putDotnetString(out, "【KoiKatuClothes】");
    putDotnetString(out, card.formatVersion.empty() ? "0.0.0" : card.formatVersion);
    putDotnetString(out, name);
    putInt(out, static_cast<int32_t>(coord.size()));
    putBytes(out, coord);
    if (!kkex.empty()) {
        Bytes kkexBin = MsgPackPack(Value(std::move(kkex)));
        putDotnetString(out, "KKEx");
        const int64_t kkexVersion = 3;   // what the game writes for this outfit file (verified, Phase 0)
        putInt(out, static_cast<int32_t>(kkexVersion));
        putInt(out, static_cast<int32_t>(kkexBin.size()));
        putBytes(out, kkexBin);
    }
    if (report) *report = ctx.rep;
    return out;
}

ClothingExportReport ExportClothingToFile(const Card& card, const std::string& outPath,
                                          const ClothingExportOptions& opt, bool overwrite) {
    if (!overwrite && fileio::Exists(outPath))
        throw FormatError("output exists already (use --overwrite to replace it): " + outPath);
    const std::string src = fullPath(card.path);
    const std::string dst = fullPath(outPath);
#ifdef _WIN32
    if (_stricmp(src.c_str(), dst.c_str()) == 0)
#else
    if (src == dst)
#endif
        throw FormatError("the output path is the source card itself; refusing to overwrite it");
    ClothingExportReport rep;
    Bytes bytes = BuildClothingFileBytes(card, opt, &rep);
    const size_t lastSlash = outPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) fileio::CreateDirectories(outPath.substr(0, lastSlash));
    fileio::WriteFile(outPath, bytes.data(), bytes.size());
    return rep;
}

} // namespace cardbridge::card
