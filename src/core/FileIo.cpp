#include "core/FileIo.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

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
    std::ofstream f(ToPath(path), std::ios::binary | std::ios::trunc);
    if (!f) throw FormatError("cannot create " + path);
    if (size) f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f) throw FormatError("cannot write " + path);
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

} // namespace cardbridge::fileio
