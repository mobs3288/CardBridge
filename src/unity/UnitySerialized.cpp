#include "unity/UnitySerialized.h"

#include <cstring>

namespace cardbridge::unity {

#include "unity/CommonStrings.inc"

namespace {

std::string commonString(uint32_t offset) {
    if (offset >= sizeof(kCommonStrings)) return "?" + std::to_string(offset);
    return std::string(kCommonStrings + offset);
}

void readTypeTree(ByteReader& r, int version, SerializedType& t) {
    if (version < 12 && version != 10) throw FormatError("old (pre-12) type trees are not supported");
    int32_t count = r.i32();
    int32_t strSize = r.i32();
    if (count < 0 || strSize < 0 || count > 200000) throw FormatError("corrupt type tree header");
    struct Raw { uint32_t typeOff, nameOff; };
    std::vector<Raw> raws(static_cast<size_t>(count));
    t.nodes.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        r.u16(); // node version
        TypeNode& n = t.nodes[static_cast<size_t>(i)];
        n.level = r.u8();
        n.isArray = (r.u8() & 1) != 0;
        raws[static_cast<size_t>(i)].typeOff = r.u32();
        raws[static_cast<size_t>(i)].nameOff = r.u32();
        n.byteSize = r.i32();
        r.i32(); // index
        n.metaFlags = r.i32();
        if (version >= 19) r.u64(); // ref type hash
    }
    Bytes strs = r.bytes(static_cast<size_t>(strSize));
    auto str = [&](uint32_t off) -> std::string {
        if (off & 0x80000000u) return commonString(off & 0x7fffffffu);
        if (off >= strs.size()) throw FormatError("type tree string offset out of range");
        return std::string(reinterpret_cast<const char*>(strs.data()) + off);
    };
    // Parent/child links from the depth-first level numbers.
    std::vector<int> stack;
    for (int i = 0; i < count; ++i) {
        TypeNode& n = t.nodes[static_cast<size_t>(i)];
        n.type = str(raws[static_cast<size_t>(i)].typeOff);
        n.name = str(raws[static_cast<size_t>(i)].nameOff);
        while (!stack.empty() && t.nodes[static_cast<size_t>(stack.back())].level >= n.level) stack.pop_back();
        if (!stack.empty()) t.nodes[static_cast<size_t>(stack.back())].children.push_back(i);
        stack.push_back(i);
    }
}

} // namespace

SerializedFile::SerializedFile(std::shared_ptr<UnityBundle> bundle, const UnityBundle::Node& node)
    : bundle_(std::move(bundle)) {
    data_ = bundle_->nodeData(node);
    size_ = static_cast<size_t>(node.size);
    ByteReader r(data_, size_, true);
    uint32_t metadataSize = r.u32();
    uint32_t fileSize = r.u32();
    version_ = static_cast<int>(r.u32());
    dataOffset_ = r.u32();
    (void)metadataSize; (void)fileSize;
    if (version_ < 13 || version_ > 21)
        throw FormatError("unsupported serialized file version " + std::to_string(version_));
    bigEndian_ = r.u8() != 0;
    r.skip(3);
    r.setBigEndian(bigEndian_);
    unityVersion_ = r.cstr();
    r.i32(); // target platform
    bool hasTree = r.u8() != 0;

    int32_t typeCount = r.i32();
    if (typeCount < 0 || typeCount > 100000) throw FormatError("corrupt type count");
    types_.resize(static_cast<size_t>(typeCount));
    for (auto& t : types_) {
        t.classId = r.i32();
        if (version_ >= 16) r.u8();       // stripped
        if (version_ >= 17) r.i16();      // script type index
        if (version_ >= 13) {
            if ((version_ < 16 && t.classId < 0) || (version_ >= 16 && t.classId == 114)) r.skip(16);
            r.skip(16);
        }
        if (hasTree) {
            readTypeTree(r, version_, t);
            if (version_ >= 21) {
                int32_t n = r.i32();
                if (n < 0 || static_cast<size_t>(n) * 4 > r.remaining()) throw FormatError("corrupt type dependencies");
                r.skip(static_cast<size_t>(n) * 4);
            }
        }
    }

    int32_t objCount = r.i32();
    if (objCount < 0 || objCount > 5000000) throw FormatError("corrupt object count");
    objects_.reserve(static_cast<size_t>(objCount));
    for (int32_t i = 0; i < objCount; ++i) {
        ObjectInfo o;
        r.align(4);   // version >= 14: 64-bit ids, aligned
        o.pathId = r.i64();
        o.byteStart = r.u32();
        o.byteSize = r.u32();
        int32_t typeId = r.i32();
        if (version_ < 16) {
            o.classId = r.u16();
            o.typeIndex = -1;
            for (size_t k = 0; k < types_.size(); ++k)
                if (types_[k].classId == typeId) { o.typeIndex = static_cast<int>(k); break; }
        } else {
            if (typeId < 0 || static_cast<size_t>(typeId) >= types_.size()) throw FormatError("object type index out of range");
            o.typeIndex = typeId;
            o.classId = types_[static_cast<size_t>(typeId)].classId;
        }
        if (version_ >= 11 && version_ < 17) r.i16();   // script type index
        if (version_ == 15 || version_ == 16) r.u8();   // stripped
        byId_[o.pathId] = objects_.size();
        objects_.push_back(o);
    }
    if (version_ >= 11) {
        int32_t scripts = r.i32();
        for (int32_t i = 0; i < scripts; ++i) {
            r.i32();
            r.align(4);
            r.i64();
        }
    }
    int32_t extCount = r.i32();
    for (int32_t i = 0; i < extCount; ++i) {
        if (version_ >= 6) r.cstr();
        if (version_ >= 5) { r.skip(16); r.i32(); }
        externals_.push_back(r.cstr());
    }
}

const ObjectInfo* SerializedFile::find(int64_t pathId) const {
    auto it = byId_.find(pathId);
    return it == byId_.end() ? nullptr : &objects_[it->second];
}

std::vector<const ObjectInfo*> SerializedFile::ofClass(int classId) const {
    std::vector<const ObjectInfo*> out;
    for (const auto& o : objects_)
        if (o.classId == classId) out.push_back(&o);
    return out;
}

Value SerializedFile::read(const ObjectInfo& o) const {
    if (o.typeIndex < 0) throw FormatError("object " + std::to_string(o.pathId) + " has no type information");
    const SerializedType& t = types_[static_cast<size_t>(o.typeIndex)];
    if (t.nodes.empty()) throw FormatError("serialized file has no type tree for class " + std::to_string(o.classId));
    uint64_t start = static_cast<uint64_t>(dataOffset_) + o.byteStart;
    if (start + o.byteSize > size_) throw FormatError("object data outside the serialized file");
    ByteReader r(data_ + start, o.byteSize, bigEndian_);
    return readNode(r, t, 0);
}

std::string SerializedFile::peekName(const ObjectInfo& o) const {
    uint64_t start = static_cast<uint64_t>(dataOffset_) + o.byteStart;
    if (start + 4 > size_ || o.byteSize < 4) return {};
    ByteReader r(data_ + start, o.byteSize, bigEndian_);
    int32_t len = r.i32();
    if (len < 0 || static_cast<size_t>(len) > r.remaining()) return {};
    return r.str(static_cast<size_t>(len));
}

Value SerializedFile::readNode(ByteReader& r, const SerializedType& t, int idx) const {
    const TypeNode& n = t.nodes[static_cast<size_t>(idx)];
    const std::string& ty = n.type;
    Value out;
    bool handled = true;

    if (ty == "SInt8") out = Value(static_cast<int64_t>(r.i8()));
    else if (ty == "UInt8" || ty == "char") out = Value(static_cast<int64_t>(r.u8()));
    else if (ty == "short" || ty == "SInt16") out = Value(static_cast<int64_t>(r.i16()));
    else if (ty == "unsigned short" || ty == "UInt16") out = Value(static_cast<int64_t>(r.u16()));
    else if (ty == "int" || ty == "SInt32") out = Value(static_cast<int64_t>(r.i32()));
    else if (ty == "unsigned int" || ty == "UInt32" || ty == "Type*") out = Value(static_cast<int64_t>(r.u32()));
    else if (ty == "long long" || ty == "SInt64") out = Value(r.i64());
    else if (ty == "unsigned long long" || ty == "UInt64") out = Value(static_cast<int64_t>(r.u64()));
    else if (ty == "float") out = Value(static_cast<double>(r.f32()));
    else if (ty == "double") out = Value(r.f64());
    else if (ty == "bool") out = Value(r.u8() != 0);
    else if (ty == "string") {
        int32_t len = r.i32();
        if (len < 0) throw FormatError("negative string length");
        out = Value(r.str(static_cast<size_t>(len)));
        r.align(4);
    } else if (ty == "TypelessData") {
        int32_t len = r.i32();
        if (len < 0) throw FormatError("negative data length");
        out = Value(r.bytes(static_cast<size_t>(len)));
    } else {
        handled = false;
    }

    if (!handled) {
        if (n.isArray) {
            if (n.children.size() < 2) throw FormatError("malformed array node in type tree");
            int32_t count = r.i32();
            if (count < 0 || static_cast<size_t>(count) > r.remaining() + 1)
                throw FormatError("array longer than the object data");
            const TypeNode& el = t.nodes[static_cast<size_t>(n.children[1])];
            if (el.type == "UInt8" || el.type == "char" || el.type == "SInt8") {
                out = Value(r.bytes(static_cast<size_t>(count)));
            } else {
                Value::Array a;
                a.reserve(static_cast<size_t>(count));
                for (int32_t i = 0; i < count; ++i) a.push_back(readNode(r, t, n.children[1]));
                out = Value(std::move(a));
            }
        } else if (n.children.size() == 1 && t.nodes[static_cast<size_t>(n.children[0])].isArray) {
            out = readNode(r, t, n.children[0]);   // vector<T> / map<K,V>: the array itself
        } else {
            Value::Map m;
            m.reserve(n.children.size());
            for (int c : n.children)
                m.emplace_back(Value(t.nodes[static_cast<size_t>(c)].name), readNode(r, t, c));
            out = Value(std::move(m));
        }
    }
    if (n.metaFlags & 0x4000) r.align(4);
    return out;
}

} // namespace cardbridge::unity
