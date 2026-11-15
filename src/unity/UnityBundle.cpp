#include "unity/UnityBundle.h"

#include "core/Lz4.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

namespace cardbridge::unity {

namespace {

Bytes decompressBlock(const uint8_t* src, size_t srcSize, size_t outSize, uint32_t mode, const std::string& what) {
    switch (mode & 0x3f) {
    case 0:
        if (srcSize != outSize) throw FormatError(what + ": stored block size mismatch");
        return Bytes(src, src + srcSize);
    case 2:
    case 3:
        return Lz4DecompressBlock(src, srcSize, outSize);
    case 1:
        throw FormatError(what + ": LZMA-compressed bundles are not supported yet");
    default:
        throw FormatError(what + ": unknown bundle compression " + std::to_string(mode & 0x3f));
    }
}

// Decompresses one block straight into `dst` (outSize bytes), with no
// temporary Bytes for the block: a large bundle otherwise holds its file
// bytes, a per-block copy and the whole decompressed blob at once.
void decompressBlockTo(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t outSize, uint32_t mode, const std::string& what) {
    switch (mode & 0x3f) {
    case 0:
        if (srcSize != outSize) throw FormatError(what + ": stored block size mismatch");
        if (outSize) std::memcpy(dst, src, outSize);
        return;
    case 2:
    case 3:
        Lz4DecompressBlockTo(src, srcSize, dst, outSize);
        return;
    case 1:
        throw FormatError(what + ": LZMA-compressed bundles are not supported yet");
    default:
        throw FormatError(what + ": unknown bundle compression " + std::to_string(mode & 0x3f));
    }
}

} // namespace

std::shared_ptr<UnityBundle> UnityBundle::Open(Bytes file, const std::string& debugName) {
    auto b = std::make_shared<UnityBundle>();
    b->name_ = debugName;
    ByteReader r(file, true);
    std::string sig = r.cstr();
    if (sig != "UnityFS") throw FormatError(debugName + ": not a UnityFS bundle (signature '" + sig + "')");
    uint32_t version = r.u32();
    r.cstr(); // player version ("5.x.x")
    b->engineVersion_ = r.cstr();
    if (version < 6 || version > 7) throw FormatError(debugName + ": unsupported UnityFS version " + std::to_string(version));
    r.u64(); // total size
    uint32_t cbiSize = r.u32();
    uint32_t ubiSize = r.u32();
    uint32_t flags = r.u32();
    if (version >= 7) r.align(16);

    Bytes cbi;
    if (flags & 0x80) {   // block info at the end of the file
        if (cbiSize > file.size()) throw FormatError(debugName + ": bad block info size");
        cbi.assign(file.end() - cbiSize, file.end());
    } else {
        cbi = r.bytes(cbiSize);
    }
    Bytes info = decompressBlock(cbi.data(), cbi.size(), ubiSize, flags, debugName + " block info");

    ByteReader ir(info, true);
    ir.skip(16); // data hash
    struct Block { uint32_t usize, csize; uint16_t flags; };
    std::vector<Block> blocks(static_cast<size_t>(ir.i32()));
    for (auto& bl : blocks) { bl.usize = ir.u32(); bl.csize = ir.u32(); bl.flags = ir.u16(); }
    int32_t nodeCount = ir.i32();
    for (int32_t i = 0; i < nodeCount; ++i) {
        Node n;
        n.offset = ir.u64();
        n.size = ir.u64();
        n.flags = ir.u32();
        n.path = ir.cstr();
        b->nodes_.push_back(std::move(n));
    }

    uint64_t total = 0;
    for (const auto& bl : blocks) total += bl.usize;
    b->data_.resize(static_cast<size_t>(total));
    // Per-block offsets, so the blocks can be decompressed concurrently: every block writes its own
    // destination range (and LZ4 is a pure function), which makes this one of the few perfectly parallel steps.
    std::vector<uint64_t> dstOff(blocks.size()), srcOff(blocks.size());
    {
        uint64_t w = 0, s = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            dstOff[i] = w; srcOff[i] = s; w += blocks[i].usize; s += blocks[i].csize;
        }
    }
    const size_t dataStart = static_cast<size_t>(r.ptr() - file.data());
    std::exception_ptr blockErr;
    std::mutex blockErrMu;
    auto blockTask = [&](size_t wi, unsigned hw) {
        try {
            for (size_t i = wi; i < blocks.size(); i += hw) {
                const Block& bl = blocks[i];
                if (blocks[i].csize > file.size() - (dataStart + srcOff[i])) throw FormatError(debugName + ": truncated block data");
                const uint8_t* srcp = file.data() + dataStart + srcOff[i];
                decompressBlockTo(srcp, bl.csize, b->data_.data() + dstOff[i], bl.usize, bl.flags, debugName);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lk(blockErrMu);
            if (!blockErr) blockErr = std::current_exception();
        }
    };
    const unsigned parallelThreads = std::max(1u, std::min(std::thread::hardware_concurrency(), static_cast<unsigned>(blocks.size())));
    const bool parallel = blocks.size() > 1 && total >= 8ull * 1024 * 1024;
    if (parallel) {
        std::vector<std::thread> pool;
        pool.reserve(parallelThreads - 1);
        for (unsigned w = 1; w < parallelThreads; ++w) pool.emplace_back(blockTask, static_cast<size_t>(w), parallelThreads);
        blockTask(0, parallelThreads);
        for (auto& th : pool) th.join();
        if (blockErr) std::rethrow_exception(blockErr);
    } else {
        blockTask(0, 1);
        if (blockErr) std::rethrow_exception(blockErr);
    }
    for (const auto& n : b->nodes_)
        if (n.offset + n.size > b->data_.size()) throw FormatError(debugName + ": node " + n.path + " outside the data");
    return b;
}

std::vector<std::string> UnityBundle::NodeNames(const Bytes& file) {
    ByteReader r(file, true);
    if (r.cstr() != "UnityFS") throw FormatError("not a UnityFS bundle");
    uint32_t version = r.u32();
    r.cstr();
    r.cstr();
    r.u64();
    uint32_t cbiSize = r.u32();
    uint32_t ubiSize = r.u32();
    uint32_t flags = r.u32();
    if (version >= 7) r.align(16);
    Bytes cbi;
    if (flags & 0x80) {
        if (cbiSize > file.size()) throw FormatError("bad block info size");
        cbi.assign(file.end() - cbiSize, file.end());
    } else {
        cbi = r.bytes(cbiSize);
    }
    Bytes info = decompressBlock(cbi.data(), cbi.size(), ubiSize, flags, "block info");
    ByteReader ir(info, true);
    ir.skip(16);
    int32_t blocks = ir.i32();
    for (int32_t i = 0; i < blocks; ++i) { ir.u32(); ir.u32(); ir.u16(); }
    std::vector<std::string> names;
    int32_t nodeCount = ir.i32();
    for (int32_t i = 0; i < nodeCount; ++i) { ir.u64(); ir.u64(); ir.u32(); names.push_back(ir.cstr()); }
    return names;
}

const UnityBundle::Node* UnityBundle::findNode(const std::string& pathOrBasename) const {
    std::string base = pathOrBasename;
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    for (const auto& n : nodes_)
        if (n.path == base) return &n;
    return nullptr;
}

const UnityBundle::Node* UnityBundle::serializedNode() const {
    for (const auto& n : nodes_) {
        const std::string& p = n.path;
        bool res = p.size() >= 5 && (p.compare(p.size() - 5, 5, ".resS") == 0 || p.compare(p.size() - 4, 4, ".res") == 0);
        if (!res) return &n;
    }
    return nullptr;
}

} // namespace cardbridge::unity
