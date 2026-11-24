#pragma once

#include "library/Library.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cardbridge::resolve {

struct ModEntry {
    std::string guid;
    std::string name, version, author;
    std::string path;        // UTF-8, absolute, forward slashes
    uint64_t size = 0;
    int64_t mtime = 0;
    // The (category, id) of every row in the mod's character lists, flattened: category, id, category, id ...
    std::vector<int64_t> listIds;
    // MaterialEditor shader registrations from the manifest (<MaterialEditor><Shader Name="..." AssetBundle="...">):
    // name -> bundle (relative to abdata/). The game's Shader.Find resolves these; CardBridge does too, so an
    // outfit authored against a mod shader (IBL_Shader_alpha & friends) renders with the shader it was made for
    // instead of a fallback with the same blending mode.
    std::vector<std::pair<std::string, std::string>> shaders;
    // <game> tags in manifest.xml. Empty means a legacy/untagged mod, which
    // Sideloader still loads in both games.
    std::vector<std::string> games;
    bool compatibleWith(const library::GameInfo& g) const {
        if (g.game == library::SourceGame::Unknown || games.empty()) return true;
        if (g.game == library::SourceGame::KoikatsuSunshine)
            for (const auto& x : games) if (x == "Koikatsu Sunshine") return true;
        if (g.game == library::SourceGame::Koikatsu)
            for (const auto& x : games) if (x == "Koikatsu") return true;
        return false;
    }
};

// Index of the Sideloader mods under a folder: GUID -> .zipmod/.zip files.
// Reading a mod's manifest.xml needs only its zip directory, but a modpack has
// thousands of them, so the result is cached in a JSON file keyed by path +
// size + last-write time; only changed or new files are re-opened.
class ModIndex {
public:
    struct Stats { size_t zips = 0, reused = 0, opened = 0, failed = 0; };

    // `cachePath` may be empty (no cache). Files that cannot be read go into `issues`.
    // `cancelled`: when it returns true the build stops early and throws
    // cardbridge::Cancelled (from the calling thread, after the pool joins).
    static ModIndex Build(const std::string& modsRoot, const std::string& cachePath,
                          std::vector<std::string>* issues, Stats* stats = nullptr,
                          const std::function<void(size_t, size_t)>& progress = {},
                          const std::function<bool()>& cancelled = {});

    // Restrict lookups to mods compatible with the imported library's game.
    // Unknown (older libraries) keeps every mod, preserving the KK behaviour.
    void setGame(const library::GameInfo& g) { game_ = g; }
    const library::GameInfo& game() const { return game_; }

    const std::vector<ModEntry>* find(const std::string& guid) const;
    // The mods whose lists define (category, id). A card saved without Sideloader's resolve info names its mod items by this raw id.
    std::vector<const ModEntry*> findByListId(int category, int64_t id) const;
    // The mod (path + its entry) that declares a shader of this exact Unity name in its manifest's
    // <MaterialEditor><Shader> table, or null. Exact name match; a shader without a declaration is not found.
    const ModEntry* findShader(const std::string& name, std::string* bundle /* abdata-relative, when found */) const;
    size_t guidCount() const { return byGuid_.size(); }
    size_t zipCount() const { return count_; }

private:
    std::map<std::string, std::vector<ModEntry>> byGuid_;
    std::map<std::pair<int, int64_t>, std::vector<std::pair<std::string, size_t>>> byListId_;   // -> (guid, index in byGuid_[guid])
    std::map<std::string, std::pair<std::string, size_t>> byShaderName_;                        // shader name -> (guid, index in byGuid_[guid])
    library::GameInfo game_;
    size_t count_ = 0;
};

// A cheap change-detection key for a mods directory: the sorted path + size +
// last-write time of every .zip/.zipmod. Two equal fingerprints mean the mod
// source is unchanged, so an already-built ModIndex can be reused. Reading
// manifest.xml is *not* part of this (that is the expensive part Build skips
// via its on-disk cache).
std::string ModSourceFingerprint(const std::string& root, std::vector<std::string>* issues = nullptr);

// Owns the in-memory mod database independently of the card being loaded.
//
// The old viewer built the index inside EnsureResolver, whose validity key was
// the resolver's library+mods pair. Switching a KK card for a KKS card changes
// the library path (a card-specific context), which was mistaken for an
// indexing dependency and rebuilt the whole database. Here the only rebuild
// triggers are: no index yet, a different mod source/configuration, mod files
// added/removed/edited, or an explicit invalidate(). The card's game is a
// lookup filter (setGame), never a build key. get() serialises builds, so a
// concurrent card load or profile switch cannot start a second indexing job.
class ModIndexCache {
public:
    using Progress = std::function<void(size_t, size_t)>;
    using Cancelled = std::function<bool()>;

    // Returns the index for `modsRoot`, rebuilding only when needed. An empty
    // root yields an empty index without touching disk. Concurrent callers
    // block and reuse the same build. `stats` describes the build (all zeros
    // on reuse); the game filter of the current index is preserved across a
    // rebuild.
    const ModIndex& get(const std::string& modsRoot, const std::string& cachePath,
                        std::vector<std::string>* issues, ModIndex::Stats* stats = nullptr,
                        const Progress& progress = {}, const Cancelled& cancelled = {});

    // Force the next get() to rebuild. `clearDiskCache` also deletes the JSON
    // per-file manifest cache so edited/removed mods are re-read from disk.
    void invalidate(bool clearDiskCache = false, const std::string& cachePath = {});

    // Card-specific game context: affects which indexed mods are visible to
    // lookups, never whether the index is rebuilt.
    void setGame(const library::GameInfo& g) { index_.setGame(g); }

    const ModIndex& index() const { return index_; }
    bool hasIndex() const { return valid_; }
    size_t buildCount() const { return builds_; }   // telemetry: how many actual builds happened
    const std::string& root() const { return root_; }

private:
    ModIndex index_;
    std::string root_, cachePath_, fingerprint_;
    bool valid_ = false;
    size_t builds_ = 0;
    std::mutex mu_;   // one build at a time; a waiter sees the finished index
};

} // namespace cardbridge::resolve
