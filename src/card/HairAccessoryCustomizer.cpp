#include "card/HairAccessoryCustomizer.h"

#include "core/MsgPack.h"

namespace cardbridge::card {

namespace {

constexpr const char* kPlugin = "com.deathweasel.bepinex.hairaccessorycustomizer";

bool looksLikeSettings(const Value& v) {
    if (!v.isMap()) return false;
    return v.has("ColorMatch") || v.has("HairGloss") || v.has("AccessoryColor") ||
           v.has("OutlineColor") || v.has("GlossColor") || v.has("HairLength");
}

Vec4 colorOf(const Value& v, bool& ok) {
    if (!v.isArray() || v.size() < 3) { ok = false; return {}; }
    return Vec4{static_cast<float>(v.at(size_t(0)).asFloat()),
                static_cast<float>(v.at(size_t(1)).asFloat()),
                static_cast<float>(v.at(size_t(2)).asFloat()),
                static_cast<float>(v.size() > 3 ? v.at(size_t(3)).asFloat() : 1.0)};
}

HairAccessoryCustomizerSettings decodeSettings(const Value& v) {
    HairAccessoryCustomizerSettings out;
    if (!looksLikeSettings(v)) return out;
    const bool allPresent = v.has("ColorMatch") && v.has("HairGloss") && v.has("AccessoryColor") &&
                            v.has("OutlineColor") && v.has("GlossColor") && v.has("HairLength");
    if (!allPresent) return out;   // never turn a partial record into a valid override
    bool ok = true;
    out.colorMatch = v.at("ColorMatch").asBool();
    out.hairGloss = v.at("HairGloss").asBool();
    out.outlineColor = colorOf(v.at("OutlineColor"), ok);
    out.accessoryColor = colorOf(v.at("AccessoryColor"), ok);
    out.glossColor = colorOf(v.at("GlossColor"), ok);
    out.hairLength = static_cast<float>(v.at("HairLength").asFloat());
    out.valid = ok;
    return out;
}

} // namespace

const HairAccessoryCustomizerSettings* HairAccessorySettings::find(int coordinate, int slot) const {
    auto c = coordinates.find(coordinate);
    if (c != coordinates.end()) {
        auto s = c->second.slots.find(slot);
        if (s != c->second.slots.end()) return &s->second;
    }
    c = coordinates.find(-1);   // flat shape from an outfit file
    if (c != coordinates.end()) {
        auto s = c->second.slots.find(slot);
        if (s != c->second.slots.end()) return &s->second;
    }
    return nullptr;
}

HairAccessorySettings LoadHairAccessoryCustomizer(const Value& extended) {
    HairAccessorySettings out;
    if (!extended.isMap()) return out;
    const Value& entry = extended.at(kPlugin);
    if (!entry.isArray() || entry.size() < 2) return out;
    const Value& payload = entry.at(size_t(1));
    Value blob = payload;
    if (payload.isMap()) {
        if (payload.has("HairAccessories")) blob = payload.at("HairAccessories");
        else if (payload.has("CoordinateHairAccessories")) blob = payload.at("CoordinateHairAccessories");
    }
    if (!blob.isBin() || blob.asBin().empty()) return out;

    Value parsed;
    try {
        parsed = MsgPackParse(blob.asBin());
    } catch (const FormatError&) {
        return out;
    } catch (const std::exception&) {
        return out;
    }
    if (!parsed.isMap()) return out;

    for (const auto& coordKv : parsed.asMap()) {
        if (!coordKv.first.isInt()) continue;
        const int coord = static_cast<int>(coordKv.first.asInt());
        const Value& coordValue = coordKv.second;
        HairAccessoryCoordinateSettings dest;
        if (looksLikeSettings(coordValue)) {
            // Flat slot -> settings shape (an outfit file): store as a wildcard coordinate.
            dest.slots.emplace(coord, decodeSettings(coordValue));
            out.coordinates[-1].slots.emplace(coord, decodeSettings(coordValue));
            continue;
        }
        if (!coordValue.isMap()) continue;
        for (const auto& slotKv : coordValue.asMap()) {
            if (!slotKv.first.isInt() || !looksLikeSettings(slotKv.second)) continue;
            dest.slots.emplace(static_cast<int>(slotKv.first.asInt()), decodeSettings(slotKv.second));
        }
        if (!dest.slots.empty()) out.coordinates.emplace(coord, std::move(dest));
    }
    return out;
}

std::array<Vec4, 3> HacHairColors(const HairAccessoryCustomizerSettings& s, const Vec4 hair[3], const Vec4 fallback[3]) {
    if (!s.valid) return {fallback[0], fallback[1], fallback[2]};
    if (s.colorMatch) return {hair[0], hair[1], hair[2]};
    // With ColorMatch off the game keeps the standard accessory colour on hair
    // renderers; the plugin only writes AccessoryColor to rendAccessory.
    return {fallback[0], fallback[1], fallback[2]};
}

std::array<Vec4, 3> HacAccessoryColors(const HairAccessoryCustomizerSettings& s, const Vec4 fallback[3]) {
    if (!s.valid) return {fallback[0], fallback[1], fallback[2]};
    return {s.accessoryColor, s.accessoryColor, s.accessoryColor};
}

} // namespace cardbridge::card
