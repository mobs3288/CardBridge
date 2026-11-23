#include "core/FileIo.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace cardbridge::fileio {

namespace {
fs::path ToPath(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}
std::string ToUtf8(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// Decodes one UTF-8 scalar. Malformed bytes (bad continuation, overlong form,
// surrogate, value above U+10FFFF) are rejected so a broken name cannot emit a
// broken path. `len` is the number of bytes consumed when the result is true.
bool NextUtf8(const std::string& s, size_t i, uint32_t& cp, size_t& len) {
    const auto at = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const auto cont = [&](size_t k) { return k < s.size() && (at(k) & 0xC0) == 0x80; };
    const uint8_t b0 = at(i);
    if (b0 < 0x80) { cp = b0; len = 1; return true; }
    if (b0 >= 0xC2 && b0 <= 0xDF && cont(i + 1)) {
        cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | (at(i + 1) & 0x3F);
        len = 2; return true;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF && cont(i + 1) && cont(i + 2)) {
        cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) | (static_cast<uint32_t>(at(i + 1) & 0x3F) << 6) | (at(i + 2) & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        len = 3; return true;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        cp = (static_cast<uint32_t>(b0 & 0x07) << 18) | (static_cast<uint32_t>(at(i + 1) & 0x3F) << 12) |
             (static_cast<uint32_t>(at(i + 2) & 0x3F) << 6) | (at(i + 3) & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return false;
        len = 4; return true;
    }
    return false;
}

// The characters Windows does not allow in a file name, plus the C0 controls
// (and DEL). Everything else - including spaces and all non-ASCII scalars -
// is a valid file name character.
bool InvalidFileNameChar(uint32_t cp) {
    return cp < 0x20 || cp == 0x7F ||
           cp == '"' || cp == '<' || cp == '>' || cp == ':' || cp == '/' || cp == '\\' ||
           cp == '|' || cp == '?' || cp == '*';
}
} // namespace

Bytes ReadFile(const std::string& path) {
    std::ifstream f(ToPath(path), std::ios::binary | std::ios::ate);
    if (!f) throw FormatError("cannot open " + path);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))
        throw FormatError("cannot read " + path);
    return out;
}

Bytes ReadRange(const std::string& path, uint64_t offset, size_t size) {
    std::ifstream f(ToPath(path), std::ios::binary);
    if (!f) throw FormatError("cannot open " + path);
    f.seekg(static_cast<std::streamoff>(offset));
    Bytes out(size);
    if (size && !f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))
        throw FormatError("short read from " + path);
    return out;
}

uint64_t FileSize(const std::string& path) {
    std::error_code ec;
    auto s = fs::file_size(ToPath(path), ec);
    if (ec) throw FormatError("cannot stat " + path);
    return s;
}

bool Exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(ToPath(path), ec);
}

int64_t LastWriteTicks(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(ToPath(path), ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

void WriteFile(const std::string& path, const uint8_t* data, size_t size) {
    // Windows can briefly hold a file that was just created or deleted (antivirus, the search
    // indexer, another process), so opening it with trunc may fail with a sharing violation even
    // though the path is valid. Exports write dozens of PNGs back to back and a re-export deletes
    // and rewrites the same names; retry for a fraction of a second before declaring failure.
    // Both the open and the write are retried: a stream that opened but failed part-way is simply
    // truncated and written again on the next attempt.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::ofstream f(ToPath(path), std::ios::binary | std::ios::trunc);
        if (!f) {
            if (attempt == 7) throw FormatError("cannot create " + path);
        } else {
            if (size) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (f) return;   // closes (and flushes) on destruction
            if (attempt == 7) throw FormatError("cannot write " + path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
    }
}

void WriteTextFile(const std::string& path, const std::string& text) {
    // No BOM: the project's data files are BOM-free (see the config.json rule).
    WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void CreateDirectories(const std::string& path) {
    std::error_code ec;
    fs::create_directories(ToPath(path), ec);
    if (ec) throw FormatError("cannot create folder " + path);
}

bool RemoveFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(ToPath(path), ec);
}

std::vector<std::string> ListFiles(const std::string& root, const std::vector<std::string>& exts,
                                   std::vector<std::string>* skipped) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(ToPath(root), fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        if (skipped) skipped->push_back(root);
        return out;
    }
    while (it != end) {
        const auto& entry = *it;
        std::error_code e2;
        if (entry.is_regular_file(e2)) {
            std::string ext = ToUtf8(entry.path().extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (std::find(exts.begin(), exts.end(), ext) != exts.end()) out.push_back(ToUtf8(entry.path()));
        }
        it.increment(ec);
        if (ec) {
            if (skipped) skipped->push_back(ToUtf8(entry.path()));
            ec.clear();
            break;
        }
    }
    return out;
}

std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string FileName(const std::string& path) {
    auto i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string Stem(const std::string& path) {
    std::string n = FileName(path);
    auto i = n.find_last_of('.');
    return i == std::string::npos ? n : n.substr(0, i);
}

std::string SafeFileName(const std::string& name) {
    constexpr size_t kMaxChars = 80;
    std::string out;
    size_t chars = 0;
    for (size_t i = 0; i < name.size() && chars < kMaxChars;) {
        uint32_t cp = 0;
        size_t len = 0;
        if (!NextUtf8(name, i, cp, len)) {
            // A bad byte is replaced on its own; the next byte may start a good scalar.
            out.push_back('_');
            ++chars;
            ++i;
            continue;
        }
        if (InvalidFileNameChar(cp)) out.push_back('_');
        else out.append(name, i, len);   // keep the original bytes: no normalization, no transliteration
        ++chars;
        i += len;
    }
    if (out.empty()) out = "unnamed";
    return out;
}

} // namespace cardbridge::fileio
