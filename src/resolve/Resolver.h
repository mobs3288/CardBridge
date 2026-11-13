#pragma once

#include "card/CardFile.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "unity/UnityScene.h"

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    // A text file a mod ships (e.g. a Stiletto heel "BepInEx/Stiletto/<Name>.txt"): bytes of the
    // entry `<path>` inside the mod's zip (case-insensitive). Nullopt when absent, empty value
    // when present-but-empty (Stiletto defaults all to 0, i.e. flat). No caching: heel configs
    // are a few hundred bytes and read once per shoe.
    std::optional<Bytes> openModText(const ResolvedItem& item, const std::string& path);
    std::shared_ptr<unity::UnityFile> openByCab(const std::string& externalName, std::string* why);
    // Drops every cached bundle and loose texture. Callers that hold a
    // shared_ptr to a bundle keep it alive, so this only releases the
    // resolver's own references: the next open re-reads the file. Used when a
    // new character is loaded and before the bake/export preview stages, so
    // the previous character's bundles do not sit beside the new scene.
    void clearCaches();
    // Decompressed bytes currently held by the bundle cache (diagnostics).
    size_t cacheBytes() const;

    // Decoded-texture cache: one entry per (source bundle, pathId), keyed by identity + the source file's
    // size and last-write ticks, so re-reading a card re-uses the previous decode instead of running the
    // LZ4/Block decoder again. Outfit switches in the viewer cost the mesh work, not the texture decodes.
    // Bounded by CARDBRIDGE_TEXTURE_CACHE_MB (default 384 MB, 0 disables); evicted least-recently-used first.
    // The identity strings are set on the UnityFile by the open*Bundle functions. Misses and hits on a file
    // that changed are indistinguishable from a brand-new texture — the stat is part of the key.
    std::shared_ptr<unity::TextureData> cachedDecodedTexture(const std::string& source, const std::string& statPath, int64_t pathId);
    void cacheDecodedTexture(const std::string& source, const std::string& statPath, int64_t pathId, const std::shared_ptr<unity::TextureData>& tex);
    size_t decodedCacheBytes() const;
    // Test/debug aid: drop the decoded-texture cache (the bundle cache is dropped by clearCaches).
    void clearDecodedCache();

private:
    struct CacheEntry {
        std::string key;
        std::shared_ptr<unity::UnityFile> file;
        size_t bytes = 0;
    };
    // Finds a cached bundle and makes it most-recently-used; null when absent.
    std::shared_ptr<unity::UnityFile> cacheFind(const std::string& key);
    void cachePut(const std::string& key, const std::shared_ptr<unity::UnityFile>& file);
    void cacheEvictLocked();

    const ModIndex& mods_;
    ListTable& lists_;
    std::string abdata_;
    std::map<std::string, bool> modListsLoaded_;
    // Bounded LRU of decoded textures, most-recent first. Shares the bundle cache's lock. Key: "<source>|<size>|<mtime>#<pathId>".
    std::list<std::pair<std::string, std::shared_ptr<unity::TextureData>>> textureCache_;
    std::map<std::string, std::list<std::pair<std::string, std::shared_ptr<unity::TextureData>>>::iterator> textureCacheByKey_;
    size_t textureCacheBytes_ = 0;
    // Bounded LRU of opened bundles, most-recent first. Previously this was an
    // unbounded map: every bundle a character touched (the library's largest
    // is 132 MB on disk) was decompressed once and kept for the life of the
    // resolver - i.e. for the whole viewer session, across every character -
    // so memory grew monotonically and never returned after a conversion.
    std::list<CacheEntry> cache_;
    std::map<std::string, std::list<CacheEntry>::iterator> cacheByKey_;
    size_t cacheBytes_ = 0;
    // Loose textures are held weakly: a caller that needs one holds the
    // shared_ptr for the duration of its use, and the decoded pixels are
    // freed as soon as it does not. The zip entry can be re-read - these are
    // the small images mods ship loose, not bundles.
    std::map<std::string, std::weak_ptr<unity::TextureData>> looseTextures_;
    mutable std::mutex cacheMutex_;
    std::map<std::string, std::vector<std::string>> providers_;   // lower-case bundle path -> zipmods of this character that contain it
    std::map<std::string, std::string> cabIndex_;   // lower-case CAB name -> library-relative bundle path
    bool cabIndexBuilt_ = false;
};

} // namespace cardbridge::resolve
