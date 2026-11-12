// cardbridge - a faceless, bald, flat-grey mannequin wearing exactly what the
// exported clothing file carries, as a front-view PNG icon.
// The same convention Koikatsu uses for coordinate files: the game ships a
// mannequin in the outfit, not the character.

#include "export/OutfitIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "card/CardFile.h"
#include "export/Preview.h"

namespace cardbridge::exporter {

namespace {

// Tiny local copy of the card module's read of Status.showAccessory (showAccessory is the 20
// flags of the accessory list; 0 = the maker wears nothing there). A missing or short array
// hides nothing, same as everywhere else.
std::vector<int8_t> parseShowAccessory(const Value& status) {
    std::vector<int8_t> flags;
    const Value& v = status.at("showAccessory");
    if (v.isBin() && v.asBin().size() >= 20)
        for (size_t i = 0; i < v.asBin().size(); ++i) flags.push_back(v.asBin()[i] ? 1 : 0);
    else if (v.isArray())
        for (const auto& e : v.asArray()) flags.push_back(static_cast<int8_t>(e.asInt(0) ? 1 : 0));
    return flags;
}

// "outfit0.acc03" -> 3; something else -> -1.
int accessorySlotOf(const std::string& id) {
    const size_t at = id.rfind(".acc");
    if (at == std::string::npos) return -1;
    const std::string tail = id.substr(at + 4);
    if (tail.empty()) return -1;
    bool digits = true;
    for (unsigned char c : tail) if (!std::isdigit(c)) { digits = false; break; }
    if (!digits) return -1;
    return std::atoi(tail.c_str());
}

bool isOutfitEmptyPart(const Value& part) {
    if (!part.isMap()) return true;
    const int type = static_cast<int>(part.at("type").asInt(120));
    const int64_t id = part.at("id").asInt(0);
    return type == 120 || id == 0;
}

bool isHairAcc(const card::Card& card, int outfit, int slot) {
    const Value& acc = card.outfits[static_cast<size_t>(outfit)].accessory;
    if (!acc.isMap()) return false;
    const Value& parts = acc.at("parts");
    if (!parts.isArray()) return false;
    if (slot >= static_cast<int>(parts.size())) return false;
    const Value& p = parts.at(static_cast<size_t>(slot));
    if (isOutfitEmptyPart(p)) return false;             // the slot is empty: nothing to hide
    return card::IsHairAccessoryPart(p, card.rows, outfit, slot);
}

} // namespace

std::vector<char> OutfitIconHiddenParts(const assemble::Scene& scene, const card::Card& card, int outfit,
                                        const std::vector<char>& base, bool withHair) {
    std::vector<char> flags(scene.parts.size(), 0);
    const std::vector<int8_t> showAccessory = parseShowAccessory(card.status);
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& p = scene.parts[pi];
        if (pi < base.size() && base[pi] == 1) flags[pi] = 1;   // the caller's untick: hidden in the view and in the file
        if (p.group == "Hair") { flags[pi] = withHair ? base[pi] : 1; continue; }   // bald by default; the hair is the character's, not the outfit's
        if (p.group == "Accessories") {
            const int slot = accessorySlotOf(p.id);
            if (slot >= 0) {
                if (!withHair && isHairAcc(card, outfit, slot)) { flags[pi] = 1; continue; }   // the file omits these by default
                if (showAccessory.size() > static_cast<size_t>(slot) && showAccessory[static_cast<size_t>(slot)] == 0) { flags[pi] = 1; continue; }   // hidden in the maker's saved state
                if (base.size() > pi && base[pi] == 1) { flags[pi] = 1; continue; }         // the caller's untick
            }
            continue;
        }
        if (p.underPiece) flags[pi] = 2;   // the garment masks cut the skin away; the mannequin shows it whole
        // Body, Head, Outfit keep their own flags from `base`.
    }
    return flags;
}

Bytes RenderOutfitIcon(const assemble::Scene& scene, const OutfitIconOptions& opt) {
    assemble::Scene sc = scene;   // the caller's scene is not touched
    std::vector<char> flags;
    if (opt.hiddenParts) flags = *opt.hiddenParts;
    else flags.assign(sc.parts.size(), 0);

    // Collect the material indices referenced by kept Body/Head parts; they become flat grey.
    std::vector<char> collect(sc.materials.size(), 0);
    for (size_t pi = 0; pi < sc.parts.size(); ++pi) {
        const auto& p = sc.parts[pi];
        if (pi >= flags.size() || flags[pi] == 1) continue;   // not drawn (2 = drawn: under pieces are wanted)
        if (p.group != "Body" && p.group != "Head") continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= sc.meshes.size()) continue;
            for (const auto& sub : sc.meshes[static_cast<size_t>(mi)].submeshes) {
                if (sub.material >= 0 && static_cast<size_t>(sub.material) < collect.size()) collect[static_cast<size_t>(sub.material)] = 1;
            }
        }
    }
    for (size_t mi = 0; mi < sc.materials.size(); ++mi) {
        if (!collect[mi]) continue;
        auto& mat = sc.materials[mi];
        mat.texture = -1;                                  // no texture: flat colour only
        mat.diffuse = {0.72f, 0.71f, 0.72f, 1.0f};         // the mannequin grey
        mat.specular = {0.0f, 0.0f, 0.0f};                 // no sheen; a nurse cap is not glossy grey either, but skin never is on the mannequin
        mat.specularPower = 0.0f;
        mat.shadow = {0.72f, 0.71f, 0.72f};                // no toon shade (a flat figure has none)
        mat.outlineColor = {0.72f, 0.71f, 0.72f};
        mat.outlineWidth = 0.0f;
        mat.alpha = false;
        mat.translucent = false;
        mat.doubleSided = true;                            // the garment masks can leave holes otherwise
        mat.toon.clear();
    }

    PreviewOptions po{};
    po.viewWidth = opt.viewWidth;
    po.viewHeight = opt.viewHeight;
    po.views = 1;                        // front view only
    po.yawDegrees[0] = 0.0f;
    po.yTop = 1.0f;
    po.yBottom = 0.0f;                   // head to feet, all of it
    po.hiddenParts = &flags;
    Bytes png = RenderPreviewPng(sc, po);
    if (png.empty()) throw FormatError("the clothing icon render failed");
    return png;
}

} // namespace cardbridge::exporter
