#pragma once

#include "core/Bytes.h"
#include "core/Value.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::card {

// What the MaterialEditor plugin stored in the card (KKEx "com.deathweasel.bepinex.materialeditor"): per-material
// texture replacements, with the replacement images embedded in the card itself. Nothing here needs the game or
// the mod: the card carries the pixels.
struct MaterialEdits {
    // MaterialEditor's ObjectType.
    enum ObjectType { StudioItem = 0, Clothing = 1, Accessory = 2, Hair = 3, Character = 4 };

    struct MainTex {
        int coordinate = 0;
        int objectType = 0;
        int slot = 0;             // clothing kind / accessory index / hair kind
        std::string material;     // material name as in the prefab (copies carry a ".MECopyN" or "_Copy" suffix)
        int texId = 0;
    };
    struct Image {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;   // top row first
    };

    std::vector<MainTex> mainTex;
    std::map<int, Bytes> encoded;    // TexID -> PNG/JPEG bytes as stored in the card

    // KoiSkinOverlayX (KKEx "KSOX"): painted layers over the skin, stored as images in the card, keyed by overlay type.
    // Seen in cards: 3 = body skin, 4 = face skin, 8 / 10 = the two eyes' iris paint (types follow KSOX's own numbering).
    std::map<int, Bytes> skinOverlays;
    enum SkinOverlayType { OverlayBody = 3, OverlayFace = 4, OverlayEyeL = 8, OverlayEyeR = 10 };
    bool decodeSkinOverlay(int type, Image& out) const;

    bool empty() const { return mainTex.empty() && skinOverlays.empty(); }

    // The embedded texture replacing `material`'s main texture, or -1.
    int mainTexFor(int coordinate, int objectType, int slot, const std::string& material) const;

    // Decodes an embedded image. False when the id is unknown or the bytes are not a readable image.
    bool decode(int texId, Image& out) const;
};

MaterialEdits LoadMaterialEdits(const Value& extended, int coordinate);

} // namespace cardbridge::card
