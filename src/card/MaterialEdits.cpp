#include "card/MaterialEdits.h"

#include "core/MsgPack.h"
#include <cstdlib>

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

std::vector<const MaterialEdits::Edit*> MaterialEdits::editsFor(const std::vector<Edit>& list, int coordinate, int objectType, int slot, const std::string& material) const {
    std::vector<const Edit*> out;
    for (const auto& e : list)
        if (e.coordinate == coordinate && e.objectType == objectType && e.slot == slot && e.material == material) out.push_back(&e);
    return out;
}

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

bool DecodeImageBytes(const Bytes& bytes, int& width, int& height, std::vector<uint8_t>& rgba) {
    MaterialEdits::Image img;
    if (!decodeImage(bytes, img)) return false;
    width = img.width;
    height = img.height;
    rgba = std::move(img.rgba);
    return true;
}

bool MaterialEdits::decode(int texId, Image& out) const {
    auto it = encoded.find(texId);
    return it != encoded.end() && decodeImage(it->second, out);
}

bool MaterialEdits::decodeSkinOverlay(int type, Image& out) const {
    auto it = skinOverlays.find(type);
    return it != skinOverlays.end() && decodeImage(it->second, out);
}

bool MaterialEdits::decodeClothesOverlay(const std::string& garment, Image& out, bool& replace) const {
    auto it = clothesOverlays.find(garment);
    if (it == clothesOverlays.end() || !decodeImage(it->second.png, out)) return false;
    replace = it->second.replace;
    return true;
}

MaterialEdits LoadMaterialEdits(const Value& extended, int coordinate) {
    MaterialEdits me;

    // KCOX: "Overlays" is a MessagePack blob of coordinate -> {garment object name -> [png bytes, replace]}.
    const Value& kcox = extended.at("KCOX").at(size_t(1));
    if (kcox.isMap() && kcox.at("Overlays").isBin()) {
        Value overlays = MsgPackParse(kcox.at("Overlays").asBin());
        for (const auto& kv : overlays.asMap()) {
            if (!kv.first.isInt() || kv.first.asInt() != coordinate) continue;
            for (const auto& g : kv.second.asMap())
                if (g.first.isStr() && g.second.at(size_t(0)).isBin() && g.second.at(size_t(0)).size() > 0)
                    me.clothesOverlays[g.first.asStr()] = {g.second.at(size_t(0)).asBin(), g.second.at(size_t(1)).asBool()};
        }
    }

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

    auto readList = [&](const char* key, std::vector<MaterialEdits::Edit>& into, const char* nameKey, auto&& fill) {
        const Value rows = inner(payload, key);   // kept alive for the loop
        for (const auto& row : rows.asArray()) {
            MaterialEdits::Edit e;
            e.coordinate = static_cast<int>(row.at("CoordinateIndex").asInt());
            e.objectType = static_cast<int>(row.at("ObjectType").asInt());
            e.slot = static_cast<int>(row.at("Slot").asInt());
            e.material = row.at(nameKey).asStr();
            fill(row, e);
            into.push_back(std::move(e));
        }
    };
    readList("MaterialFloatPropertyList", me.floats, "MaterialName", [](const Value& row, MaterialEdits::Edit& e) {
        e.property = row.at("Property").asStr();
        const Value& v = row.at("Value");
        e.value[0] = v.isStr() ? static_cast<float>(std::atof(v.asStr().c_str())) : static_cast<float>(v.asFloat());
    });
    readList("MaterialColorPropertyList", me.colors, "MaterialName", [](const Value& row, MaterialEdits::Edit& e) {
        e.property = row.at("Property").asStr();
        const Value& v = row.at("Value");
        for (size_t k = 0; k < 4; ++k) e.value[k] = static_cast<float>(v.at(k).asFloat(1.0));
    });
    readList("MaterialShaderList", me.shaders, "MaterialName", [](const Value& row, MaterialEdits::Edit& e) { e.property = row.at("ShaderName").asStr(); });
    readList("RendererPropertyList", me.rendererEnabled, "RendererName", [](const Value& row, MaterialEdits::Edit& e) {
        e.property = std::to_string(row.at("Property").asInt());   // 0 = Enabled
        const Value& v = row.at("Value");
        e.value[0] = v.isStr() ? static_cast<float>(std::atof(v.asStr().c_str())) : static_cast<float>(v.asFloat());
    });
    return me;
}

} // namespace cardbridge::card
