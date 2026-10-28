#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace cardbridge {

using Bytes = std::vector<uint8_t>;

// Every format error in the library is one of these, with a message that says
// which file or section it was and what was wrong - the CLI prints it and the
// report records it. Nothing here asserts on input from a file.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A bounds-checked cursor over a byte range. Truncated or corrupt data throws
// FormatError instead of reading past the end.
class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const uint8_t* data, size_t size, bool bigEndian = false)
        : data_(data), size_(size), big_(bigEndian) {}
    explicit ByteReader(const Bytes& b, bool bigEndian = false)
        : ByteReader(b.data(), b.size(), bigEndian) {}

    size_t pos() const { return pos_; }
    size_t size() const { return size_; }
    size_t remaining() const { return size_ - pos_; }
    bool bigEndian() const { return big_; }
    void setBigEndian(bool b) { big_ = b; }
    void seek(size_t p) {
        if (p > size_) throw FormatError("seek past end of data");
        pos_ = p;
    }
    void skip(size_t n) { need(n); pos_ += n; }
    void align(size_t a) { size_t r = pos_ % a; if (r) skip(a - r); }
    const uint8_t* ptr() const { return data_ + pos_; }

    uint8_t u8() { need(1); return data_[pos_++]; }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t u16() { return static_cast<uint16_t>(num(2)); }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() { return static_cast<uint32_t>(num(4)); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64() { return num(8); }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64() { uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }

    Bytes bytes(size_t n) {
        need(n);
        Bytes out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }
    std::string str(size_t n) {
        need(n);
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    // NUL-terminated string (Unity bundles and serialized files).
    std::string cstr() {
        size_t start = pos_;
        while (true) {
            need(1);
            if (data_[pos_++] == 0) break;
        }
        return std::string(reinterpret_cast<const char*>(data_ + start), pos_ - 1 - start);
    }
    // .NET BinaryReader.ReadString: 7-bit-encoded length, then UTF-8.
    std::string dotnetString() {
        uint32_t n = 0;
        int shift = 0;
        while (true) {
            uint8_t b = u8();
            n |= static_cast<uint32_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 28) throw FormatError("bad 7-bit string length");
        }
        return str(n);
    }

private:
    void need(size_t n) const {
        if (n > size_ - pos_) throw FormatError("unexpected end of data");
    }
    uint64_t num(int n) {
        need(static_cast<size_t>(n));
        uint64_t v = 0;
        if (big_) for (int i = 0; i < n; ++i) v = (v << 8) | data_[pos_ + i];
        else for (int i = n - 1; i >= 0; --i) v = (v << 8) | data_[pos_ + i];
        pos_ += static_cast<size_t>(n);
        return v;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool big_ = false;
};

// FNV-1a 64: the pixel/data hash the golden tests compare (cheap, no dependency).
inline uint64_t Fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace cardbridge
