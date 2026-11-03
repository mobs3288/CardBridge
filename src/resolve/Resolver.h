#pragma once

#include "card/CardFile.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "unity/UnityScene.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::resolve {

// One thing in the card that should become geometry: a head, a hair part, a
// clothing slot, an accessory. Built from the card alone (no files touched).
struct ItemRef {
    std::string partId;       // stable, e.g. "outfit0.top", "hair.back", "outfit0.acc03"
    std::string label;        // display name
    std::string group;        // "Head", "Hair", "Outfit", "Accessories"
    std::string property;     // as the card's resolver data names it
    int category = 0;         // ChaListDefine.CategoryNo
    int64_t id = 0;
    std::string attachNode;   // accessories: the body node they hang on (e.g. a_n_headside)
    int slot = -1;            // clothing slot index (0 top ... 8 outer shoes), or -1
    int accIndex = -1;        // accessory slot index, or -1
    int subKind = -1;         // top sub-part A/B/C (0..2) of a jacket/sailor top, or -1
    int outfit = -1;
};

enum class ItemStatus {
    Empty,            // id 0 / "none": nothing to load
    ResolvedBase,     // found in the base game's lists
    ResolvedMod,      // found in an installed mod
    MissingMod,       // the card names a mod that is not installed
    MissingListEntry, // mod found but it has no list row for this slot / base lists have no such id
    MissingBundle,    // list row found, but its asset bundle is not in the mod / game folder
};
const char* StatusName(ItemStatus s);

struct ResolvedItem {
    ItemRef ref;
    ItemStatus status = ItemStatus::Empty;
    std::string detail;       // human-readable reason for a failure
    ListItem listItem;        // valid when status is Resolved*
    std::string modGuid, modName, modVersion, modAuthor, modPath;
    bool ambiguous = false;   // several files claim the same mod GUID
    std::string bundleRel() const { return listItem.get("MainAB"); }
    std::string assetName() const { return listItem.get("MainData"); }
    bool ok() const { return status == ItemStatus::ResolvedBase || status == ItemStatus::ResolvedMod; }
};

// Items a card uses for one outfit (head and hair are outfit-independent).
std::vector<ItemRef> ItemsFromCard(const card::Card& card, int outfitIndex);

class Resolver {
public:
    // `abdataRoot` is <game>/abdata. The mod index and list table outlive the resolver.
    Resolver(const ModIndex& mods, ListTable& lists, std::string abdataRoot);

    ResolvedItem resolve(const card::Card& card, const ItemRef& ref);

    // Opens (and caches) the bundle that holds the item's prefab.
    std::shared_ptr<unity::UnityFile> openBundle(const ResolvedItem& item, std::string* why);
    // Same lookup for another bundle the item's list row names (its texture bundle):
    // inside the mod's zip for a mod item, else the library.
    std::shared_ptr<unity::UnityFile> openItemBundle(const ResolvedItem& item, const std::string& rel, std::string* why);
    // Sideloader keeps one bundle per bundle path: when two installed mods ship the same path (a newer copy of the same asset pack under
    // another GUID), the one loaded last wins. Items of the character call this so openItemBundle picks the mod the game would.
    void registerBundleProvider(const ResolvedItem& item);
    // A base-game bundle by its abdata-relative path, e.g. "chara/oo_base.unity3d".
    std::shared_ptr<unity::UnityFile> openBaseBundle(const std::string& rel, std::string* why);
    // The library bundle that holds an object a material points at in "another file" (a PPtr's external name, e.g.
    // "archive:/CAB-xxxx/CAB-xxxx"): the first call indexes which bundle holds which CAB (cached in library/cab_index.json).
    // A row of the base-game lists (the library's abdata/list), or null.
    const ListItem* baseRow(int category, int64_t id) const { return lists_.findBase(category, id); }
    // A texture a mod's list row names that is not in any bundle: Sideloader serves loose images the zipmod ships at
    // abdata/<bundle path without .unity3d>/<texture name>.png (e.g. abdata/sideloader/mt_nose_00/01.png for MainAB "sideloader/mt_nose_00.unity3d",
    // MainTex "01"). Null when the mod has no such file.
    std::shared_ptr<unity::TextureData> openLooseTexture(const ResolvedItem& item, const std::string& ab, const std::string& name, std::string* why);
    std::shared_ptr<unity::UnityFile> openByCab(const std::string& externalName, std::string* why);

private:
    const ModIndex& mods_;
    ListTable& lists_;
    std::string abdata_;
    std::map<std::string, bool> modListsLoaded_;
    std::map<std::string, std::shared_ptr<unity::UnityFile>> cache_;
    std::map<std::string, std::shared_ptr<unity::TextureData>> looseTextures_;
    std::map<std::string, std::vector<std::string>> providers_;   // lower-case bundle path -> zipmods of this character that contain it
    std::map<std::string, std::string> cabIndex_;   // lower-case CAB name -> library-relative bundle path
    bool cabIndexBuilt_ = false;
};

} // namespace cardbridge::resolve
