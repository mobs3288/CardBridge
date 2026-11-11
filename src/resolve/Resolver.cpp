#include "resolve/Resolver.h"

#include "core/FileIo.h"
#include "card/MaterialEdits.h"
#include "core/ZipArchive.h"
#include <nlohmann/json.hpp>

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
            {
                add("outfit" + std::to_string(outfit) + "." + slot[i], slot[i], "Outfit",
                    "outfit" + std::to_string(outfit) + ".ChaFileClothes." + prop[i], cat[i], cp[i].at("id").asInt());
                out.back().slot = static_cast<int>(i);
                out.back().outfit = outfit;
            }
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
            out.back().accIndex = static_cast<int>(i);
            out.back().outfit = outfit;
        }
    }
    return out;
}

Resolver::Resolver(const ModIndex& mods, ListTable& lists, std::string abdataRoot)
    : mods_(mods), lists_(lists), abdata_(std::move(abdataRoot)) {}

ResolvedItem Resolver::resolve(const card::Card& card, const ItemRef& ref) {
    // In the viewer, Reshape (UI thread) can resolve while a card load (worker thread) is assembling. Both walk
    // the shared modlists map and mutate it, so serialize them with the bundle-cache lock.
    std::lock_guard<std::mutex> lk(cacheMutex_);
    ResolvedItem r;
    r.ref = ref;
    // id 0 means "none" for hair, clothing and accessories - but for the head it is the first real head.
    // Jacket/sailor sub-parts (categories 200-212) are the same: id 0 there is a real shirt or a listed "none".
    bool zeroIsItem = ref.category == 100 || (ref.category >= 200 && ref.category <= 212);
    // Face textures (eyebrow, eyelines, nose, eye white / pupil / highlight rows) are the same where the base lists have a row 0: it is the
    // first real texture (cw_t_mayuge_000, cw_t_eyeline_down_000, cw_t_noseline_000), not "none".
    if (ref.category >= 406 && ref.category <= 414 && lists_.findBase(ref.category, 0)) zeroIsItem = true;
    if (ref.id == 0 && !zeroIsItem) { r.status = ItemStatus::Empty; return r; }

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

    // The item by its raw list id among the installed mods' lists. A card saved without Sideloader's resolve information names its mod
    // items this way, and Sideloader falls back to it when the named mod (or its installed version) does not define the slot.
    // Several installed files can claim the id (typically two versions of the same GUID): Sideloader's unpacker copies them over
    // each other in path order, so the last one the game sees wins and we take the newest by path, as the named branch below does.
    auto byRawId = [&]() {
        auto owners = mods_.findByListId(ref.category, ref.id);
        if (owners.empty()) return false;
        const ModEntry& m = **std::max_element(owners.begin(), owners.end(), [](const ModEntry* a, const ModEntry* b) { return a->path < b->path; });
        if (!modListsLoaded_[m.path]) {
            modListsLoaded_[m.path] = true;
            lists_.addMod(m.path, m.guid, nullptr);
        }
        const ListItem* ml = lists_.findMod(m.guid, ref.category, ref.id);
        if (!ml) return false;
        r.modGuid = m.guid;
        r.modName = m.name; r.modVersion = m.version; r.modAuthor = m.author; r.modPath = m.path;
        r.ambiguous = r.ambiguous || owners.size() > 1;
        r.listItem = *ml;
        r.status = ItemStatus::ResolvedMod;
        r.detail = "resolved by raw list id";
        return true;
    };

    if (row) {
        r.modGuid = row->modId;
        const auto* files = mods_.find(row->modId);
        if (!files || files->empty()) {
            if (byRawId()) return r;
            r.status = ItemStatus::MissingMod;
            r.detail = "mod '" + row->modId + "' is not installed under the mods folder";
            return r;
        }
        // Several installed files can carry one GUID (versions of a pack); take one whose lists define the item, the newest by path.
        const ModEntry* pick = &(*files)[0];
        bool pickHas = false;
        for (const ModEntry& cand : *files) {
            bool has = false;
            for (size_t k = 0; k + 1 < cand.listIds.size() && !has; k += 2) has = cand.listIds[k] == ref.category && cand.listIds[k + 1] == ref.id;
            if ((has && !pickHas) || (has == pickHas && cand.path > pick->path)) { pick = &cand; pickHas = has; }
        }
        const ModEntry& m = *pick;
        r.modName = m.name; r.modVersion = m.version; r.modAuthor = m.author; r.modPath = m.path;
        if (files->size() > 1) r.ambiguous = true;
        if (!modListsLoaded_[m.path]) {
            modListsLoaded_[m.path] = true;
            lists_.addMod(m.path, row->modId, nullptr);
        }
        const ListItem* li = lists_.findMod(row->modId, ref.category, ref.id);
        if (!li) {
            if (byRawId()) return r;
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
        if (byRawId()) return r;
        r.status = ItemStatus::MissingListEntry;
        r.detail = "id " + std::to_string(ref.id) + " is not in the base game lists and the card names no mod for it";
        return r;
    }
    r.listItem = *li;
    r.status = ItemStatus::ResolvedBase;
    return r;
}

namespace {
// Bundles in this library reach 132 MB on disk. Keeping a few of them is the
// difference between decompressing the same bundle for every item and
// keeping hundreds of megabytes for the whole session. 128 MB holds the
// shared skeleton, face and create-material bundles that every character
// reuses (each a few MB to ~10 MB decompressed) and evicts the rest
// (re-read on demand): a single 132 MB bundle is kept while it is the only
// entry, but it does not sit beside every other large bundle for the whole
// conversion. With the scene's own textures this bounds the bundle cache's
// contribution to the peak instead of letting it grow with every item.
constexpr size_t kBundleCacheCap = 128ull * 1024 * 1024;
size_t bundleBytes(const std::shared_ptr<unity::UnityFile>& f) {
    // The decompressed blob dominates; the serialized file (type trees, object
    // table) is accounted roughly on top so the cap is not understated.
    return f ? f->serialized().bundle().dataSize() + f->serialized().objects().size() * 32 + 65536 : 0;
}
} // namespace

std::shared_ptr<unity::UnityFile> Resolver::cacheFind(const std::string& key) {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    auto it = cacheByKey_.find(key);
    if (it == cacheByKey_.end()) return nullptr;
    cache_.splice(cache_.begin(), cache_, it->second);
    return it->second->file;
}

void Resolver::cacheEvictLocked() {
    while (cacheBytes_ > kBundleCacheCap && cache_.size() > 1) {
        auto& back = cache_.back();
        cacheBytes_ -= back.bytes;
        cacheByKey_.erase(back.key);
        cache_.pop_back();
    }
}

void Resolver::cachePut(const std::string& key, const std::shared_ptr<unity::UnityFile>& file) {
    if (!file) return;
    std::lock_guard<std::mutex> lk(cacheMutex_);
    auto old = cacheByKey_.find(key);
    if (old != cacheByKey_.end()) {
        cacheBytes_ -= old->second->bytes;
        cache_.erase(old->second);
        cacheByKey_.erase(old);
    }
    CacheEntry e;
    e.key = key;
    e.file = file;
    e.bytes = bundleBytes(file);
    cache_.push_front(e);
    cacheByKey_[key] = cache_.begin();
    cacheBytes_ += cache_.front().bytes;
    cacheEvictLocked();
}

void Resolver::clearCaches() {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    cache_.clear();
    cacheByKey_.clear();
    cacheBytes_ = 0;
    looseTextures_.clear();
    providers_.clear();   // per-card "which mod ships this bundle path": a new character registers its own, so the previous one's must not stay
}

size_t Resolver::cacheBytes() const {
    std::lock_guard<std::mutex> lk(cacheMutex_);
    return cacheBytes_;
}

std::shared_ptr<unity::UnityFile> Resolver::openBaseBundle(const std::string& rel, std::string* why) {
    std::string key = "base:" + rel;
    if (auto hit = cacheFind(key)) return hit;
    std::string path = fileio::Join(abdata_, rel);
    if (!fileio::Exists(path)) { if (why) *why = "bundle not found: " + path; return nullptr; }
    try {
        auto b = unity::UnityBundle::Open(fileio::ReadFile(path), rel);
        auto uf = std::make_shared<unity::UnityFile>(b);
        cachePut(key, uf);
        return uf;
    } catch (const std::exception& e) {
        if (why) *why = std::string(e.what());
        return nullptr;
    }
}

std::shared_ptr<unity::UnityFile> Resolver::openByCab(const std::string& externalName, std::string* why) {
    std::string cab = externalName;
    for (char& c : cab) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto slash = cab.find_last_of('/');
    if (slash != std::string::npos) cab = cab.substr(slash + 1);
    if (!cabIndexBuilt_) {
        cabIndexBuilt_ = true;
        const std::string cachePath = fileio::Join(abdata_, "../cab_index.json");
        try {
            auto b = fileio::ReadFile(cachePath);
            auto j = nlohmann::json::parse(std::string(b.begin(), b.end()));
            for (auto it = j.begin(); it != j.end(); ++it) cabIndex_[it.key()] = it.value().get<std::string>();
        } catch (...) {}
        if (cabIndex_.empty()) {
            std::string root = fileio::Join(abdata_, "chara");
            for (const auto& f : fileio::ListFiles(root, {".unity3d"})) {
                try {
                    for (const auto& n : unity::UnityBundle::NodeNames(fileio::ReadFile(f))) {
                        if (n.rfind("CAB-", 0) != 0 || n.find('.') != std::string::npos) continue;
                        std::string key = n;
                        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        std::string rel = f.substr(abdata_.size());
                        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
                        cabIndex_[key] = rel;
                    }
                } catch (...) {}
            }
            try {
                nlohmann::json j = nlohmann::json::object();
                for (const auto& kv : cabIndex_) j[kv.first] = kv.second;
                fileio::WriteTextFile(cachePath, j.dump());
            } catch (...) {}
        }
    }
    auto it = cabIndex_.find(cab);
    if (it == cabIndex_.end()) { if (why) *why = "no library bundle holds " + externalName; return nullptr; }
    return openBaseBundle(it->second, why);
}

std::shared_ptr<unity::UnityFile> Resolver::openBundle(const ResolvedItem& item, std::string* why) {
    std::string rel = item.bundleRel();
    if (rel.empty()) { if (why) *why = "list entry has no MainAB"; return nullptr; }
    return openItemBundle(item, rel, why);
}

namespace {
std::string lowerOf(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return s; }
}

void Resolver::registerBundleProvider(const ResolvedItem& item) {
    if (item.status != ItemStatus::ResolvedMod || item.modPath.empty()) return;
    std::string rel = item.bundleRel();
    if (rel.empty()) return;
    try {
        ZipArchive z(item.modPath);
        if (!z.find("abdata/" + rel)) {
            bool any = false;
            std::string want = lowerOf("abdata/" + rel);
            for (const auto& e : z.entries()) if (lowerOf(e.name) == want) { any = true; break; }
            if (!any) return;
        }
    } catch (...) { return; }
    std::lock_guard<std::mutex> lk(cacheMutex_);
    auto& v = providers_[lowerOf(rel)];
    if (std::find(v.begin(), v.end(), item.modPath) == v.end()) v.push_back(item.modPath);
}

std::shared_ptr<unity::UnityFile> Resolver::openItemBundle(const ResolvedItem& item, const std::string& rel, std::string* why) {
    if (item.status == ItemStatus::ResolvedBase) return openBaseBundle(rel, why);

    std::string modPath = item.modPath;
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        if (auto pr = providers_.find(lowerOf(rel)); pr != providers_.end() && pr->second.size() > 1)
            modPath = *std::max_element(pr->second.begin(), pr->second.end(), [](const std::string& a, const std::string& b) { return lowerOf(a) < lowerOf(b); });
    }
    std::string key = modPath + "|" + rel;
    if (auto hit = cacheFind(key)) return hit;
    try {
        ZipArchive z(modPath);
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
            if (base) { cachePut(key, base); return base; }
            if (why) *why = "bundle '" + want + "' is not inside " + item.modPath + " and not in the library (" + whyBase + ")";
            return nullptr;
        }
        auto b = unity::UnityBundle::Open(z.read(*found), rel);
        auto uf = std::make_shared<unity::UnityFile>(b);
        cachePut(key, uf);
        return uf;
    } catch (const std::exception& e) {
        if (why) *why = std::string(e.what());
        return nullptr;
    }
}

std::shared_ptr<unity::TextureData> Resolver::openLooseTexture(const ResolvedItem& item, const std::string& ab, const std::string& name, std::string* why) {
    if (item.status != ItemStatus::ResolvedMod || item.modPath.empty()) { if (why) *why = "not a mod item"; return nullptr; }
    std::string stem = lowerOf(ab);
    if (stem.size() > 8 && stem.compare(stem.size() - 8, 8, ".unity3d") == 0) stem.resize(stem.size() - 8);
    std::string modPath = item.modPath;
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        if (auto pr = providers_.find(lowerOf(ab)); pr != providers_.end() && pr->second.size() > 1)
            modPath = *std::max_element(pr->second.begin(), pr->second.end(), [](const std::string& a, const std::string& b) { return lowerOf(a) < lowerOf(b); });
    }
    std::string key = "loose:" + modPath + "|" + stem + "/" + lowerOf(name);
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        auto hit = looseTextures_.find(key);
        if (hit != looseTextures_.end()) {
            if (auto live = hit->second.lock()) return live;
            looseTextures_.erase(hit);
        }
    }
    try {
        ZipArchive z(modPath);
        const std::string want = "abdata/" + stem + "/" + lowerOf(name) + ".png";
        for (const auto& e : z.entries()) {
            if (lowerOf(e.name) != want) continue;
            int w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (!card::DecodeImageBytes(z.read(e), w, h, rgba)) { if (why) *why = e.name + " is not a readable image"; return nullptr; }
            auto t = std::make_shared<unity::TextureData>();
            t->name = name;
            t->width = w;
            t->height = h;
            t->unityFormat = 4;   // RGBA32
            t->rgba = std::move(rgba);
            // Sideloader serves loose PNGs with the wrap mode picked from the file's path:
            //   "clamp" in it -> Clamp, "repeat" in it -> Repeat, otherwise Unity's default (Repeat).
            // (Sideloader.dll GetPng: pngPath.Contains("clamp") / Contains("repeat").)
            // Decal-style mods (tattoos, lip liners, nip textures) name their image _clamp because the
            // placement shader samples outside 0..1 and must see the transparent border, not a tile.
            const std::string pngPath = std::string("abdata/") + stem + "/" + lowerOf(name) + ".png";
            if (pngPath.find("clamp") != std::string::npos) t->wrapMode = 1;         // Clamp
            else if (pngPath.find("repeat") != std::string::npos) t->wrapMode = 0;   // Repeat
            else t->wrapMode = 0;                                                    // Unity's default for a texture loaded from bytes
            {
                std::lock_guard<std::mutex> lk(cacheMutex_);
                looseTextures_[key] = t;
            }
            return t;
        }
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return nullptr;
    }
    if (why) *why = "no loose image abdata/" + stem + "/" + name + ".png in " + modPath;
    return nullptr;
}

std::optional<Bytes> Resolver::openModText(const ResolvedItem& item, const std::string& path) {
    if (item.status != ItemStatus::ResolvedMod || item.modPath.empty()) return std::nullopt;
    try {
        ZipArchive z(item.modPath);
        std::string want = lowerOf(path);
        for (const auto& e : z.entries())
            if (lowerOf(e.name) == want) return z.read(e);
    } catch (...) {}
    return std::nullopt;
}

} // namespace cardbridge::resolve
