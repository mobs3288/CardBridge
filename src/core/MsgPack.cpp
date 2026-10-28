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

} // namespace cardbridge
