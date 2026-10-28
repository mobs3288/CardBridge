#include "card/MaterialEdits.h"

#include "core/MsgPack.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace cardbridge::card {

namespace {

// Each list is stored as a MessagePack blob inside the plugin's payload map.
Value inner(const Value& payload, const char* key) {
    const Value& v = payload.at(key);
    return v.isBin() ? MsgPackParse(v.asBin()) : Value{};
}

} // namespace

int MaterialEdits::mainTexFor(int coordinate, int objectType, int slot, const std::string& material) const {
    for (const auto& m : mainTex)
        if (m.coordinate == coordinate && m.objectType == objectType && m.slot == slot && m.material == material) return m.texId;
    return -1;
}

namespace {

bool decodeImage(const Bytes& bytes, MaterialEdits::Image& out) {
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

} // namespace

bool MaterialEdits::decode(int texId, Image& out) const {
    auto it = encoded.find(texId);
    return it != encoded.end() && decodeImage(it->second, out);
}

bool MaterialEdits::decodeSkinOverlay(int type, Image& out) const {
    auto it = skinOverlays.find(type);
    return it != skinOverlays.end() && decodeImage(it->second, out);
}

MaterialEdits LoadMaterialEdits(const Value& extended, int coordinate) {
    MaterialEdits me;

    // KSOX: Lookup maps coordinate -> {overlay type -> texture id}; the images sit beside it as "_TextureID_<id>".
    const Value& ksox = extended.at("KSOX").at(size_t(1));
    if (ksox.isMap() && ksox.at("Lookup").isBin()) {
        Value lookup = MsgPackParse(ksox.at("Lookup").asBin());
        const Value* row = nullptr;
        for (const auto& kv : lookup.asMap())
            if (kv.first.isInt() && kv.first.asInt() == coordinate) row = &kv.second;
        if (!row && !lookup.asMap().empty()) row = &lookup.asMap().front().second;
        if (row)
            for (const auto& kv : row->asMap()) {
                if (!kv.first.isInt()) continue;
                const Value& img = ksox.at("_TextureID_" + std::to_string(kv.second.asInt()));
                if (img.isBin() && img.size() > 0) me.skinOverlays[static_cast<int>(kv.first.asInt())] = img.asBin();
            }
    }

    const Value& entry = extended.at("com.deathweasel.bepinex.materialeditor");
    const Value& payload = entry.at(size_t(1));
    if (!payload.isMap()) return me;

    Value textures = inner(payload, "TextureDictionary");
    for (const auto& kv : textures.asMap())
        if (kv.first.isInt() && kv.second.isBin()) me.encoded[static_cast<int>(kv.first.asInt())] = kv.second.asBin();

    Value list = inner(payload, "MaterialTexturePropertyList");
    for (const auto& row : list.asArray()) {
        if (row.at("Property").asStr() != "MainTex") continue;
        if (row.at("TexID").isNil()) continue;
        MaterialEdits::MainTex m;
        m.coordinate = static_cast<int>(row.at("CoordinateIndex").asInt());
        m.objectType = static_cast<int>(row.at("ObjectType").asInt());
        m.slot = static_cast<int>(row.at("Slot").asInt());
        m.material = row.at("MaterialName").asStr();
        m.texId = static_cast<int>(row.at("TexID").asInt());
        me.mainTex.push_back(std::move(m));
    }
    return me;
}

} // namespace cardbridge::card
