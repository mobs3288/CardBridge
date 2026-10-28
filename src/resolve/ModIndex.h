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
};

// Index of the Sideloader mods under a folder: GUID -> .zipmod/.zip files.
// Reading a mod's manifest.xml needs only its zip directory, but a modpack has
// thousands of them, so the result is cached in a JSON file keyed by path +
// size + last-write time; only changed or new files are re-opened.
class ModIndex {
public:
    struct Stats { size_t zips = 0, reused = 0, opened = 0, failed = 0; };

    // `cachePath` may be empty (no cache). Files that cannot be read go into `issues`.
    static ModIndex Build(const std::string& modsRoot, const std::string& cachePath,
                          std::vector<std::string>* issues, Stats* stats = nullptr,
                          const std::function<void(size_t, size_t)>& progress = {});

    const std::vector<ModEntry>* find(const std::string& guid) const;
    size_t guidCount() const { return byGuid_.size(); }
    size_t zipCount() const { return count_; }

private:
    std::map<std::string, std::vector<ModEntry>> byGuid_;
    size_t count_ = 0;
};

} // namespace cardbridge::resolve
