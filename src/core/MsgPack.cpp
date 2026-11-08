#include "core/MsgPack.h"

namespace cardbridge {

namespace {

constexpr int kMaxDepth = 64;

Value readValue(ByteReader& r, int depth);

Value readArray(ByteReader& r, size_t n, int depth) {
    // Each element is at least one byte, so a count beyond what is left is corrupt.
    if (n > r.remaining()) throw FormatError("msgpack: array longer than the data");
    Value::Array a;
    a.reserve(n);
    for (size_t i = 0; i < n; ++i) a.push_back(readValue(r, depth + 1));
    return Value(std::move(a));
}

Value readMap(ByteReader& r, size_t n, int depth) {
    if (n > r.remaining()) throw FormatError("msgpack: map longer than the data");
    Value::Map m;
    m.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Value k = readValue(r, depth + 1);
        Value v = readValue(r, depth + 1);
        m.emplace_back(std::move(k), std::move(v));
    }
    return Value(std::move(m));
}

Value readValue(ByteReader& r, int depth) {
    if (depth > kMaxDepth) throw FormatError("msgpack: nested too deep");
    bool oldBig = r.bigEndian();
    r.setBigEndian(true); // MessagePack is big-endian whatever the caller's reader was set to
    struct Restore { ByteReader& r; bool b; ~Restore() { r.setBigEndian(b); } } restore{r, oldBig};

    uint8_t t = r.u8();
    if (t <= 0x7f) return Value(static_cast<int64_t>(t));
    if (t >= 0xe0) return Value(static_cast<int64_t>(static_cast<int8_t>(t)));
    if ((t & 0xe0) == 0xa0) return Value(r.str(t & 0x1f));
    if ((t & 0xf0) == 0x90) return readArray(r, t & 0x0f, depth);
    if ((t & 0xf0) == 0x80) return readMap(r, t & 0x0f, depth);
    switch (t) {
    case 0xc0: return Value();
    case 0xc2: return Value(false);
    case 0xc3: return Value(true);
    case 0xc4: return Value(r.bytes(r.u8()));
    case 0xc5: return Value(r.bytes(r.u16()));
    case 0xc6: return Value(r.bytes(r.u32()));
    case 0xca: return Value(static_cast<double>(r.f32()));
    case 0xcb: return Value(r.f64());
    case 0xcc: return Value(static_cast<int64_t>(r.u8()));
    case 0xcd: return Value(static_cast<int64_t>(r.u16()));
    case 0xce: return Value(static_cast<int64_t>(r.u32()));
    case 0xcf: return Value(static_cast<int64_t>(r.u64()));
    case 0xd0: return Value(static_cast<int64_t>(r.i8()));
    case 0xd1: return Value(static_cast<int64_t>(r.i16()));
    case 0xd2: return Value(static_cast<int64_t>(r.i32()));
    case 0xd3: return Value(r.i64());
    case 0xd9: return Value(r.str(r.u8()));
    case 0xda: return Value(r.str(r.u16()));
    case 0xdb: return Value(r.str(r.u32()));
    case 0xdc: return readArray(r, r.u16(), depth);
    case 0xdd: return readArray(r, r.u32(), depth);
    case 0xde: return readMap(r, r.u16(), depth);
    case 0xdf: return readMap(r, r.u32(), depth);
    case 0xc7: { size_t n = r.u8(); r.skip(1 + n); return Value(); }   // ext 8
    case 0xc8: { size_t n = r.u16(); r.skip(1 + n); return Value(); }  // ext 16
    case 0xc9: { size_t n = r.u32(); r.skip(1 + n); return Value(); }  // ext 32
    case 0xd4: r.skip(2); return Value();
    case 0xd5: r.skip(3); return Value();
    case 0xd6: r.skip(5); return Value();
    case 0xd7: r.skip(9); return Value();
    case 0xd8: r.skip(17); return Value();
    default: throw FormatError("msgpack: unknown type byte");
    }
}

} // namespace

Value MsgPackRead(ByteReader& r) { return readValue(r, 0); }

Value MsgPackParse(const uint8_t* data, size_t size) {
    ByteReader r(data, size, true);
    Value v = readValue(r, 0);
    if (r.remaining() != 0) throw FormatError("msgpack: trailing bytes after document");
    return v;
}

namespace {

void packU16(Bytes& out, uint16_t v) { out.push_back(static_cast<uint8_t>(v >> 8)); out.push_back(static_cast<uint8_t>(v)); }
void packU32(Bytes& out, uint32_t v) { for (int k = 3; k >= 0; --k) out.push_back(static_cast<uint8_t>(v >> (8 * k))); }
void packU64(Bytes& out, uint64_t v) { for (int k = 7; k >= 0; --k) out.push_back(static_cast<uint8_t>(v >> (8 * k))); }

void packValue(Bytes& out, const Value& v, int depth) {
    if (depth > kMaxDepth) throw FormatError("msgpack: nested too deep to pack");
    if (v.isNil()) { out.push_back(0xc0); return; }
    if (v.isBool()) { out.push_back(v.asBool() ? 0xc3 : 0xc2); return; }
    if (v.isInt()) {
        const int64_t x = v.asInt();
        if (x >= 0) {
            if (x <= 0x7f) out.push_back(static_cast<uint8_t>(x));
            else if (x <= 0xff) { out.push_back(0xcc); out.push_back(static_cast<uint8_t>(x)); }
            else if (x <= 0xffff) { out.push_back(0xcd); packU16(out, static_cast<uint16_t>(x)); }
            else if (x <= 0xffffffffll) { out.push_back(0xce); packU32(out, static_cast<uint32_t>(x)); }
            else { out.push_back(0xcf); packU64(out, static_cast<uint64_t>(x)); }
        } else {
            if (x >= -32) out.push_back(static_cast<uint8_t>(x));
            else if (x >= -128) { out.push_back(0xd0); out.push_back(static_cast<uint8_t>(x)); }
            else if (x >= -32768) { out.push_back(0xd1); packU16(out, static_cast<uint16_t>(static_cast<int16_t>(x))); }
            else if (x >= -2147483648ll) { out.push_back(0xd2); packU32(out, static_cast<uint32_t>(static_cast<int32_t>(x))); }
            else { out.push_back(0xd3); packU64(out, static_cast<uint64_t>(x)); }
        }
        return;
    }
    if (v.isFloat()) {
        const float f = static_cast<float>(v.asFloat());
        uint32_t u;
        std::memcpy(&u, &f, 4);
        out.push_back(0xca);
        packU32(out, u);
        return;
    }
    if (v.isStr()) {
        const std::string& s = v.asStr();
        const size_t n = s.size();
        if (n < 32) out.push_back(static_cast<uint8_t>(0xa0 | n));
        else if (n <= 0xff) { out.push_back(0xd9); out.push_back(static_cast<uint8_t>(n)); }
        else if (n <= 0xffff) { out.push_back(0xda); packU16(out, static_cast<uint16_t>(n)); }
        else { out.push_back(0xdb); packU32(out, static_cast<uint32_t>(n)); }
        out.insert(out.end(), s.begin(), s.end());
        return;
    }
    if (v.isBin()) {
        const Bytes& b = v.asBin();
        const size_t n = b.size();
        if (n <= 0xff) { out.push_back(0xc4); out.push_back(static_cast<uint8_t>(n)); }
        else if (n <= 0xffff) { out.push_back(0xc5); packU16(out, static_cast<uint16_t>(n)); }
        else { out.push_back(0xc6); packU32(out, static_cast<uint32_t>(n)); }
        out.insert(out.end(), b.begin(), b.end());
        return;
    }
    if (v.isArray()) {
        const Value::Array& a = v.asArray();
        const size_t n = a.size();
        if (n < 16) out.push_back(static_cast<uint8_t>(0x90 | n));
        else if (n <= 0xffff) { out.push_back(0xdc); packU16(out, static_cast<uint16_t>(n)); }
        else { out.push_back(0xdd); packU32(out, static_cast<uint32_t>(n)); }
        for (const auto& e : a) packValue(out, e, depth + 1);
        return;
    }
    if (v.isMap()) {
        const Value::Map& m = v.asMap();
        const size_t n = m.size();
        if (n < 16) out.push_back(static_cast<uint8_t>(0x80 | n));
        else if (n <= 0xffff) { out.push_back(0xde); packU16(out, static_cast<uint16_t>(n)); }
        else { out.push_back(0xdf); packU32(out, static_cast<uint32_t>(n)); }
        for (const auto& kv : m) { packValue(out, kv.first, depth + 1); packValue(out, kv.second, depth + 1); }
        return;
    }
    throw FormatError("msgpack: cannot pack value");
}

} // namespace

Bytes MsgPackPack(const Value& v) {
    Bytes out;
    packValue(out, v, 0);
    return out;
}

} // namespace cardbridge
