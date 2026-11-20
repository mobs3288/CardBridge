#pragma once

#include "core/Math.h"
#include "core/Value.h"

#include <array>
#include <cstdint>
#include <map>

namespace cardbridge::card {

// What DeathWeasel's HairAccessoryCustomizer stores in KKEx:
// "com.deathweasel.bepinex.hairaccessorycustomizer" -> [version, { HairAccessories: <MessagePack blob> }].
// The blob is a map of coordinate index -> accessory slot -> settings. A flat
// slot->settings shape also occurs in outfit files written by the maker; those
// entries are stored under coordinate -1 (a wildcard) and are only used when no
// exact coordinate entry exists.
//
// `valid` is false for a missing, malformed or incomplete setting: callers must
// then keep CardBridge's existing accessory behaviour instead of inventing
// defaults.
struct HairAccessoryCustomizerSettings {
    bool hairGloss = false;
    bool colorMatch = false;
    Vec4 outlineColor{0, 0, 0, 1};
    Vec4 accessoryColor{1, 1, 1, 1};
    Vec4 glossColor{0, 0, 0, 1};
    float hairLength = 0.0f;
    bool valid = false;
};

struct HairAccessoryCoordinateSettings {
    std::map<int, HairAccessoryCustomizerSettings> slots;
};

struct HairAccessorySettings {
    std::map<int, HairAccessoryCoordinateSettings> coordinates;

    const HairAccessoryCustomizerSettings* find(int coordinate, int slot) const;
    bool empty() const { return coordinates.empty(); }
};

// Decodes the plugin entry from a KKEx map. Never throws; a damaged or absent
// payload yields an empty result. Uses the shared MessagePack reader.
HairAccessorySettings LoadHairAccessoryCustomizer(const Value& extended);

// Colour resolution used by the accessory pipeline when a hair accessory has
// HAC settings. Hair materials (rendHair of ChaCustomHairComponent) use the
// character's three hair colours when ColorMatch is on; accessory renderers
// use the stored AccessoryColor. Both return `fallback` when the setting is
// absent or invalid.
std::array<Vec4, 3> HacHairColors(const HairAccessoryCustomizerSettings& s,
                                  const Vec4 hair[3], const Vec4 fallback[3]);
std::array<Vec4, 3> HacAccessoryColors(const HairAccessoryCustomizerSettings& s, const Vec4 fallback[3]);

} // namespace cardbridge::card
