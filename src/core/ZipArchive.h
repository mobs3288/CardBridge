#pragma once

#include "core/Bytes.h"

#include <map>
#include <string>
#include <vector>

namespace cardbridge {

// Read-only .zip / .zipmod. Only the central directory is read on open; an
// entry's bytes are read (and inflated if needed) on request, so opening a
// 60 MB mod to look at its manifest costs a few KB of IO. Zip64 and encrypted
// entries are reported as unsupported rather than guessed at.
class ZipArchive {
public:
    struct Entry {
        std::string name;        // as stored, forward slashes
        uint16_t method = 0;     // 0 stored, 8 deflate
        uint32_t compressedSize = 0;
        uint32_t size = 0;
        uint32_t localHeaderOffset = 0;
    };

    explicit ZipArchive(const std::string& path);   // throws FormatError
    // A zip that starts `base` bytes into the file and is `size` bytes long (appended to an exe): every offset in it is relative to `base`.
    ZipArchive(const std::string& path, uint64_t base, uint64_t size);   // throws FormatError

    const std::string& path() const { return path_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& name) const;   // exact, case-sensitive
    bool contains(const std::string& name) const { return find(name) != nullptr; }
    Bytes read(const std::string& name) const;          // throws if missing or unsupported
    Bytes read(const Entry& e) const;

private:
    std::string path_;
    uint64_t base_ = 0;
    std::vector<Entry> entries_;
    std::map<std::string, size_t> byName_;
};

} // namespace cardbridge
