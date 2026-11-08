#include "card/Abmx.h"

#include "core/Lz4.h"
#include "core/MsgPack.h"

namespace cardbridge::card {

const AbmxMod* AbmxTable::find(const std::string& bone, int coordinate) const {
    auto it = bones.find(bone);
    if (it == bones.end() || it->second.empty()) return nullptr;
    if (it->second.size() == 1) return &it->second[0];
    return coordinate >= 0 && static_cast<size_t>(coordinate) < it->second.size() ? &it->second[static_cast<size_t>(coordinate)] : nullptr;
}

namespace {

// A MessagePack integer at `p`; returns the number of bytes read, 0 if it is not an integer.
size_t readInt(const uint8_t* p, size_t n, int64_t& out) {
    if (n < 1) return 0;
    uint8_t m = p[0];
    auto be = [&](size_t bytes, bool sign) -> size_t {
        if (n < 1 + bytes) return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < bytes; ++i) v = (v << 8) | p[1 + i];
        if (sign && bytes < 8 && (v >> (bytes * 8 - 1))) v |= ~uint64_t{0} << (bytes * 8);
        out = static_cast<int64_t>(v);
        return 1 + bytes;
    };
    if (m <= 0x7f) { out = m; return 1; }
    if (m >= 0xe0) { out = static_cast<int8_t>(m); return 1; }
    switch (m) {
        case 0xcc: return be(1, false);
        case 0xcd: return be(2, false);
        case 0xce: return be(4, false);
        case 0xcf: return be(8, false);
        case 0xd0: return be(1, true);
        case 0xd1: return be(2, true);
        case 0xd2: return be(4, true);
        case 0xd3: return be(8, true);
    }
    return 0;
}

Vec3 vec3Of(const Value& v, Vec3 def) {
    if (!v.isArray() || v.size() < 3) return def;
    return {static_cast<float>(v.at(size_t(0)).asFloat(def.x)), static_cast<float>(v.at(size_t(1)).asFloat(def.y)), static_cast<float>(v.at(size_t(2)).asFloat(def.z))};
}

} // namespace

AbmxTable LoadAbmx(const Value& extended) {
    AbmxTable t;
    const Value& entry = extended.at("KKABMPlugin.ABMData").at(size_t(1));
    if (!entry.isMap() || !entry.at("boneData").isBin()) return t;
    const Bytes& bd = entry.at("boneData").asBin();
    if (bd.empty()) return t;
    Value list;
    try {
        size_t o = 0;
        if (bd[0] == 0xc7 || bd[0] == 0xc8 || bd[0] == 0xc9) {   // ext: type 99 = LZ4 block (MessagePack-CSharp); payload = original length, block
            size_t lenBytes = bd[0] == 0xc7 ? 1 : bd[0] == 0xc8 ? 2 : 4;
            o = 1 + lenBytes + 1;   // marker, length, type
            if (bd.size() <= o || bd[1 + lenBytes] != 99) return t;
            int64_t ulen = 0;
            size_t used = readInt(bd.data() + o, bd.size() - o, ulen);
            if (used == 0 || ulen <= 0 || ulen > (64 << 20)) return t;
            o += used;
            Bytes raw = Lz4DecompressBlock(bd.data() + o, bd.size() - o, static_cast<size_t>(ulen));
            list = MsgPackParse(raw);
        } else {
            list = MsgPackParse(bd);
        }
    } catch (const std::exception&) {
        return t;
    }
    for (const auto& e : list.asArray()) {
        if (!e.isArray() || e.size() < 2 || !e.at(size_t(0)).isStr()) continue;
        std::vector<AbmxMod> mods;
        for (const auto& m : e.at(size_t(1)).asArray()) {
            AbmxMod a;
            a.scale = vec3Of(m.at(size_t(0)), a.scale);
            a.length = static_cast<float>(m.at(size_t(1)).asFloat(1.0));
            if (m.size() > 2) a.position = vec3Of(m.at(size_t(2)), a.position);
            if (m.size() > 3) a.rotation = vec3Of(m.at(size_t(3)), a.rotation);
            mods.push_back(a);
        }
        if (!mods.empty()) t.bones[e.at(size_t(0)).asStr()] = std::move(mods);
    }
    return t;
}

Value EncodeAbmx(const AbmxTable& t) {
    Value::Array list;
    // std::map order is file order's sort; the plugin reads by name, so order does not matter.
    for (const auto& kv : t.bones) {
        Value::Array mods;
        for (const auto& a : kv.second) {
            Value::Array mod;
            mod.emplace_back(Value::Array{Value(a.scale.x), Value(a.scale.y), Value(a.scale.z)});
            mod.emplace_back(Value(static_cast<double>(a.length)));
            mod.emplace_back(Value::Array{Value(a.position.x), Value(a.position.y), Value(a.position.z)});
            mod.emplace_back(Value::Array{Value(a.rotation.x), Value(a.rotation.y), Value(a.rotation.z)});
            mods.emplace_back(Value(std::move(mod)));
        }
        Value::Array entry;
        entry.emplace_back(Value(kv.first));
        entry.emplace_back(Value(std::move(mods)));
        list.emplace_back(Value(std::move(entry)));
    }
    return Value(std::move(list));
}

Bytes PackAbmxBin(const AbmxTable& t) {
    const Bytes raw = MsgPackPack(EncodeAbmx(t));
    const Bytes comp = Lz4CompressBlock(raw.data(), raw.size());
    Bytes bin;
    bin.push_back(0xc9);
    const uint32_t payloadLen = static_cast<uint32_t>(1 + MsgPackPack(Value(static_cast<int64_t>(raw.size()))).size() + comp.size());
    for (int k = 3; k >= 0; --k) bin.push_back(static_cast<uint8_t>(payloadLen >> (8 * k)));
    bin.push_back(99);
    const Bytes ulen = MsgPackPack(Value(static_cast<int64_t>(raw.size())));
    bin.insert(bin.end(), ulen.begin(), ulen.end());
    bin.insert(bin.end(), comp.begin(), comp.end());
    return bin;
}

} // namespace cardbridge::card
