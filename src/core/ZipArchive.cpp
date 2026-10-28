#include "core/ZipArchive.h"

#include "core/FileIo.h"
#include "core/Inflate.h"

#include <algorithm>

namespace cardbridge {

ZipArchive::ZipArchive(const std::string& path) : path_(path) {
    uint64_t fileSize = fileio::FileSize(path);
    if (fileSize < 22) throw FormatError("not a zip (too small): " + path);
    size_t tailSize = static_cast<size_t>(std::min<uint64_t>(fileSize, 22 + 65535));
    Bytes tail = fileio::ReadRange(path, fileSize - tailSize, tailSize);

    // End of central directory record: scan backwards for its signature.
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) throw FormatError("not a zip (no end record): " + path);

    ByteReader er(tail.data() + eocd + 4, tail.size() - eocd - 4, false);
    er.skip(4); // disk numbers
    uint16_t entriesThisDisk = er.u16();
    uint16_t totalEntries = er.u16();
    uint32_t cdSize = er.u32();
    uint32_t cdOffset = er.u32();
    if (totalEntries == 0xffff || cdSize == 0xffffffffu || cdOffset == 0xffffffffu)
        throw FormatError("zip64 archives are not supported: " + path);
    (void)entriesThisDisk;
    if (static_cast<uint64_t>(cdOffset) + cdSize > fileSize) throw FormatError("corrupt zip directory: " + path);

    Bytes cd = fileio::ReadRange(path, cdOffset, cdSize);
    ByteReader r(cd, false);
    entries_.reserve(totalEntries);
    for (uint16_t i = 0; i < totalEntries; ++i) {
        if (r.u32() != 0x02014b50) throw FormatError("corrupt zip directory entry: " + path);
        r.skip(4); // version made by / needed
        uint16_t flags = r.u16();
        Entry e;
        e.method = r.u16();
        r.skip(4); // time, date
        r.skip(4); // crc
        e.compressedSize = r.u32();
        e.size = r.u32();
        uint16_t nameLen = r.u16(), extraLen = r.u16(), commentLen = r.u16();
        r.skip(8); // disk, internal attrs, external attrs
        e.localHeaderOffset = r.u32();
        e.name = r.str(nameLen);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        r.skip(static_cast<size_t>(extraLen) + commentLen);
        if (flags & 1) e.method = 0xfffe; // encrypted: reported on read
        byName_[e.name] = entries_.size();
        entries_.push_back(std::move(e));
    }
}

const ZipArchive::Entry* ZipArchive::find(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &entries_[it->second];
}

Bytes ZipArchive::read(const std::string& name) const {
    const Entry* e = find(name);
    if (!e) throw FormatError("zip entry not found: " + name + " in " + path_);
    return read(*e);
}

Bytes ZipArchive::read(const Entry& e) const {
    if (e.method != 0 && e.method != 8)
        throw FormatError("zip entry " + e.name + " uses an unsupported method/encryption");
    Bytes hdr = fileio::ReadRange(path_, e.localHeaderOffset, 30);
    ByteReader r(hdr, false);
    if (r.u32() != 0x04034b50) throw FormatError("corrupt local header for " + e.name);
    r.skip(22);
    uint16_t nameLen = r.u16(), extraLen = r.u16();
    uint64_t dataOffset = static_cast<uint64_t>(e.localHeaderOffset) + 30 + nameLen + extraLen;
    Bytes raw = fileio::ReadRange(path_, dataOffset, e.compressedSize);
    if (e.method == 0) {
        if (raw.size() != e.size) throw FormatError("stored entry size mismatch: " + e.name);
        return raw;
    }
    return InflateRaw(raw.data(), raw.size(), e.size);
}

} // namespace cardbridge
