#pragma once

#include <cstdint>
#include <functional>
#include <map>
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
    size_t count_ = 0;
};

} // namespace cardbridge::resolve
