#pragma once

#include "core/Bytes.h"

#include <memory>
#include <string>
#include <vector>

namespace cardbridge::unity {

// A UnityFS asset bundle (format version 6 - what Unity 5.6 writes, verified
// on the 297 bundles in abdata\chara: all LZ4HC, blocks-info combined with the
// directory). Opening a bundle decompresses it fully into memory; the bundles
// in this game range from ~0.1 MB to 132 MB on disk (library_full\abdata\chara:
// co_top_00 is 132 MB), so a bundle's decompressed data is a large allocation
// and is accounted for by the resolver's bounded cache. LZMA blocks are
// reported as unsupported, not guessed at.
class UnityBundle {
public:
    struct Node {
        std::string path;   // "CAB-xxxx" (the serialized file) or "CAB-xxxx.resS" (streamed data)
        uint64_t offset = 0;
        uint64_t size = 0;
        uint32_t flags = 0;
    };

    static std::shared_ptr<UnityBundle> Open(Bytes fileBytes, const std::string& debugName);   // throws FormatError
    // The node names (CAB-xxxx ...) of a bundle file without decompressing its data: enough to index which bundle holds which CAB.
    static std::vector<std::string> NodeNames(const Bytes& fileBytes);   // throws FormatError

    const std::string& name() const { return name_; }
    const std::string& engineVersion() const { return engineVersion_; }
    const std::vector<Node>& nodes() const { return nodes_; }
    const Node* findNode(const std::string& pathOrBasename) const;   // "archive:/CAB-x/CAB-x.resS" or "CAB-x.resS"
    // The node's bytes, inside the decompressed blob (valid while the bundle lives).
    const uint8_t* nodeData(const Node& n) const { return data_.data() + n.offset; }
    // The serialized-file node (the one that is not a .resS), or null.
    const Node* serializedNode() const;
    // Size of the decompressed data blob: the bundle's main cost, used to
    // bound the resolver's bundle cache.
    size_t dataSize() const { return data_.size(); }

private:
    std::string name_;
    std::string engineVersion_;
    std::vector<Node> nodes_;
    Bytes data_;
};

} // namespace cardbridge::unity
