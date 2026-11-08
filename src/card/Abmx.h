#pragma once

#include "core/Math.h"
#include "core/Value.h"

#include <map>
#include <string>
#include <vector>

namespace cardbridge::card {

// KKABMX (Advanced Bonemod, KKEx "KKABMPlugin.ABMData"): per-bone scale, length, position and rotation modifiers the plugin applies on top
// of the game's own shape sliders. A bone with one entry uses it for every coordinate; with several, entry n is coordinate n.
struct AbmxMod {
    Vec3 scale{1, 1, 1};
    float length = 1.0f;
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};   // Euler degrees
    bool hasScale() const { return scale.x != 1.0f || scale.y != 1.0f || scale.z != 1.0f; }
    bool hasLength() const { return length != 1.0f; }
    bool hasPosition() const { return position.x != 0.0f || position.y != 0.0f || position.z != 0.0f; }
    bool hasRotation() const { return rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f; }
    bool empty() const { return !hasScale() && !hasLength() && !hasPosition() && !hasRotation(); }
};

struct AbmxTable {
    std::map<std::string, std::vector<AbmxMod>> bones;
    // The modifier of `bone` for the coordinate, or null.
    const AbmxMod* find(const std::string& bone, int coordinate) const;
    bool empty() const { return bones.empty(); }
};

// Reads the table from a card's extended data (an LZ4-compressed MessagePack list of [bone name, [modifiers]]); empty when absent.
AbmxTable LoadAbmx(const Value& extended);

// The table as MessagePack (the list LoadAbmx reads), and the full stored
// form (the LZ4 ext-99 blob the `boneData` bin holds). Used to merge an outfit
// file's bone mods into the character when the outfit is applied.
Value EncodeAbmx(const AbmxTable& t);
Bytes PackAbmxBin(const AbmxTable& t);

} // namespace cardbridge::card
