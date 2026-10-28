#include "resolve/Resolver.h"

#include "core/FileIo.h"
#include "core/ZipArchive.h"

#include <algorithm>
#include <cstdio>

namespace cardbridge::resolve {

const char* StatusName(ItemStatus s) {
    switch (s) {
    case ItemStatus::Empty: return "empty";
    case ItemStatus::ResolvedBase: return "resolved-base";
    case ItemStatus::ResolvedMod: return "resolved-mod";
    case ItemStatus::MissingMod: return "missing-mod";
    case ItemStatus::MissingListEntry: return "missing-list-entry";
    default: return "missing-bundle";
    }
}

std::vector<ItemRef> ItemsFromCard(const card::Card& card, int outfit) {
    std::vector<ItemRef> out;
    auto add = [&](const std::string& id, const std::string& label, const std::string& group, const std::string& prop,
                   int category, int64_t itemId, const std::string& attach = {}) {
        ItemRef r;
        r.partId = id; r.label = label; r.group = group; r.property = prop;
        r.category = category; r.id = itemId; r.attachNode = attach;
        out.push_back(std::move(r));
    };

    add("head", "Head", "Head", "ChaFileFace.headId", 100, card.face.at("headId").asInt());

    static const char* hairNames[4] = {"back", "front", "side", "extension"};
    static const char* hairProps[4] = {"ChaFileHair.HairBack", "ChaFileHair.HairFront", "ChaFileHair.HairSide", "ChaFileHair.HairExtension"};
    const auto& hp = card.hair.at("parts").asArray();
    for (size_t i = 0; i < hp.size() && i < 4; ++i)
        add(std::string("hair.") + hairNames[i], std::string("Hair (") + hairNames[i] + ")", "Hair", hairProps[i],
            101 + static_cast<int>(i), hp[i].at("id").asInt());

    if (outfit >= 0 && static_cast<size_t>(outfit) < card.outfits.size()) {
        const auto& oc = card.outfits[static_cast<size_t>(outfit)];
        static const char* slot[9] = {"top", "bottom", "bra", "shorts", "gloves", "pantyhose", "socks", "shoes_inner", "shoes_outer"};
        static const char* prop[9] = {"ClothesTop", "ClothesBot", "ClothesBra", "ClothesShorts", "ClothesGloves",
                                      "ClothesPants", "ClothesSocks", "ClothesShoesInner", "ClothesShoesOuter"};
        static const int cat[9] = {105, 106, 107, 108, 109, 110, 111, 112, 112};
        const auto& cp = oc.clothes.at("parts").asArray();
        for (size_t i = 0; i < cp.size() && i < 9; ++i)
            add("outfit" + std::to_string(outfit) + "." + slot[i], slot[i], "Outfit",
                "outfit" + std::to_string(outfit) + ".ChaFileClothes." + prop[i], cat[i], cp[i].at("id").asInt());
        const auto& ap = oc.accessory.at("parts").asArray();
        for (size_t i = 0; i < ap.size(); ++i) {
            int type = static_cast<int>(ap[i].at("type").asInt());
            int64_t id = ap[i].at("id").asInt();
            if (type == 120 || id == 0) continue;   // 120 = "none" slot
            char num[8];
            std::snprintf(num, sizeof num, "%02zu", i);
            add("outfit" + std::to_string(outfit) + ".acc" + num, std::string("Accessory ") + num, "Accessories",
                "outfit" + std::to_string(outfit) + ".accessory" + std::to_string(i) + ".ChaFileAccessory.PartsInfo.id",
                type, id, ap[i].at("parentKey").asStr());
        }
    }
    return out;
}

Resolver::Resolver(const ModIndex& mods, ListTable& lists, std::string abdataRoot)
    : mods_(mods), lists_(lists), abdata_(std::move(abdataRoot)) {}

ResolvedItem Resolver::resolve(const card::Card& card, const ItemRef& ref) {
    ResolvedItem r;
    r.ref = ref;
    // id 0 means "none" for hair, clothing and accessories - but for the head it is the first real head.
    if (ref.id == 0 && ref.category != 100) { r.status = ItemStatus::Empty; return r; }

    // The card's resolver row for this item: by property path first, then by
    // (category, slot) - the pair identifies a mod item even if a property
    // name differs between plugin versions.
    const card::ResolveRow* row = nullptr;
    for (const auto& rr : card.rows)
        if (rr.property == ref.property && rr.slot == ref.id) { row = &rr; break; }
    if (!row) {
        std::vector<const card::ResolveRow*> hits;
        for (const auto& rr : card.rows)
            if (rr.category == ref.category && rr.slot == ref.id) hits.push_back(&rr);
        if (!hits.empty()) {
            row = hits[0];
            for (auto* h : hits) if (h->modId != row->modId) { r.ambiguous = true; break; }
        }
    }

    if (row) {
        r.modGuid = row->modId;
        const auto* files = mods_.find(row->modId);
        if (!files || files->empty()) {
            r.status = ItemStatus::MissingMod;
            r.detail = "mod '" + row->modId + "' is not installed under the mods folder";
            return r;
        }
        const ModEntry& m = (*files)[0];
        r.modName = m.name; r.modVersion = m.version; r.modAuthor = m.author; r.modPath = m.path;
        if (files->size() > 1) r.ambiguous = true;
        if (!modListsLoaded_[m.path]) {
            modListsLoaded_[m.path] = true;
            lists_.addMod(m.path, row->modId, nullptr);
        }
        const ListItem* li = lists_.findMod(row->modId, ref.category, ref.id);
        if (!li) {
            r.status = ItemStatus::MissingListEntry;
            r.detail = "mod '" + row->modId + "' has no list entry for category " + std::to_string(ref.category) + " slot " + std::to_string(ref.id);
            return r;
        }
        r.listItem = *li;
        r.status = ItemStatus::ResolvedMod;
        return r;
    }

    const ListItem* li = lists_.findBase(ref.category, ref.id);
    if (!li) {
        r.status = ItemStatus::MissingListEntry;
        r.detail = "id " + std::to_string(ref.id) + " is not in the base game lists and the card names no mod for it";
        return r;
    }
    r.listItem = *li;
    r.status = ItemStatus::ResolvedBase;
    return r;
}

std::shared_ptr<unity::UnityFile> Resolver::openBaseBundle(const std::string& rel, std::string* why) {
    std::string key = "base:" + rel;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    std::string path = fileio::Join(abdata_, rel);
    if (!fileio::Exists(path)) { if (why) *why = "bundle not found: " + path; return nullptr; }
    try {
        auto b = unity::UnityBundle::Open(fileio::ReadFile(path), rel);
        auto uf = std::make_shared<unity::UnityFile>(b);
        cache_[key] = uf;
        return uf;
    } catch (const std::exception& e) {
        if (why) *why = std::string(e.what());
        return nullptr;
    }
}

std::shared_ptr<unity::UnityFile> Resolver::openBundle(const ResolvedItem& item, std::string* why) {
    std::string rel = item.bundleRel();
    if (rel.empty()) { if (why) *why = "list entry has no MainAB"; return nullptr; }
    return openItemBundle(item, rel, why);
}

std::shared_ptr<unity::UnityFile> Resolver::openItemBundle(const ResolvedItem& item, const std::string& rel, std::string* why) {
    if (item.status == ItemStatus::ResolvedBase) return openBaseBundle(rel, why);

    std::string key = item.modPath + "|" + rel;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    try {
        ZipArchive z(item.modPath);
        std::string want = "abdata/" + rel;
        std::string lowerWant = want;
        std::transform(lowerWant.begin(), lowerWant.end(), lowerWant.begin(), [](unsigned char c) { return std::tolower(c); });
        const ZipArchive::Entry* found = z.find(want);
        if (!found)
            for (const auto& e : z.entries()) {
                std::string l = e.name;
                std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
                if (l == lowerWant) { found = &e; break; }
            }
        if (!found) {
            // Some mods point at a base-game bundle (a "no hair" item uses chara/etc.unity3d): use the library's.
            std::string whyBase;
            auto base = openBaseBundle(rel, &whyBase);
            if (base) { cache_[key] = base; return base; }
            if (why) *why = "bundle '" + want + "' is not inside " + item.modPath + " and not in the library (" + whyBase + ")";
            return nullptr;
        }
        auto b = unity::UnityBundle::Open(z.read(*found), rel);
        auto uf = std::make_shared<unity::UnityFile>(b);
        cache_[key] = uf;
        return uf;
    } catch (const std::exception& e) {
        if (why) *why = std::string(e.what());
        return nullptr;
    }
}

} // namespace cardbridge::resolve
