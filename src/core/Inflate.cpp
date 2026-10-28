#include "core/Inflate.h"

#include <array>

namespace cardbridge {

namespace {

struct BitReader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;

    uint32_t bits(int need) {
        while (bitCnt < need) {
            if (pos >= n) throw FormatError("inflate: unexpected end of data");
            bitBuf |= static_cast<uint32_t>(p[pos++]) << bitCnt;
            bitCnt += 8;
        }
        uint32_t v = bitBuf & ((1u << need) - 1u);
        bitBuf >>= need;
        bitCnt -= need;
        return v;
    }
};

// Canonical Huffman table decoded bit by bit (count per length + sorted symbols).
// Slow-ish but short and obviously correct; zipmod lists and manifests are tiny
// and the big bundles are stored, not deflated.
struct Huffman {
    std::array<uint16_t, 16> count{};
    std::array<uint16_t, 288> symbol{};

    void build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; ++i) count[lengths[i]]++;
        count[0] = 0;
        std::array<uint16_t, 16> offs{};
        for (int i = 1; i < 15; ++i) offs[i + 1] = static_cast<uint16_t>(offs[i] + count[i]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= static_cast<int>(br.bits(1));
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw FormatError("inflate: bad Huffman code");
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                    6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflateBlock(BitReader& br, Bytes& out, const Huffman& lit, const Huffman& dist, size_t limit) {
    while (true) {
        int sym = lit.decode(br);
        if (sym < 256) {
            if (out.size() >= limit) throw FormatError("inflate: output larger than expected");
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw FormatError("inflate: bad length symbol");
            size_t len = kLenBase[sym] + br.bits(kLenExtra[sym]);
            int ds = dist.decode(br);
            if (ds >= 30) throw FormatError("inflate: bad distance symbol");
            size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (d > out.size()) throw FormatError("inflate: distance too far back");
            if (out.size() + len > limit) throw FormatError("inflate: output larger than expected");
            for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
        }
    }
}

} // namespace

Bytes InflateRaw(const uint8_t* src, size_t srcSize, size_t expectedSize) {
    BitReader br{src, srcSize};
    Bytes out;
    out.reserve(expectedSize);
    bool last = false;
    while (!last) {
        last = br.bits(1) != 0;
        uint32_t type = br.bits(2);
        if (type == 0) {
            br.bitBuf = 0;
            br.bitCnt = 0;
            if (br.pos + 4 > br.n) throw FormatError("inflate: truncated stored block");
            uint16_t len = static_cast<uint16_t>(src[br.pos] | (src[br.pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(src[br.pos + 2] | (src[br.pos + 3] << 8));
            br.pos += 4;
            if (static_cast<uint16_t>(~nlen) != len) throw FormatError("inflate: stored block length check failed");
            if (br.pos + len > br.n) throw FormatError("inflate: truncated stored block");
            if (out.size() + len > expectedSize) throw FormatError("inflate: output larger than expected");
            out.insert(out.end(), src + br.pos, src + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            uint8_t l[288];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            Huffman lit, dist;
            lit.build(l, 288);
            uint8_t dl[30];
            for (int i = 0; i < 30; ++i) dl[i] = 5;
            dist.build(dl, 30);
            inflateBlock(br, out, lit, dist, expectedSize);
        } else if (type == 2) {
            int nlen = static_cast<int>(br.bits(5)) + 257;
            int ndist = static_cast<int>(br.bits(5)) + 1;
            int ncode = static_cast<int>(br.bits(4)) + 4;
            if (nlen > 286 || ndist > 30) throw FormatError("inflate: bad dynamic header");
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman cl;
            cl.build(lengths, 19);
            uint8_t all[320] = {};
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = cl.decode(br);
                if (sym < 16) {
                    all[idx++] = static_cast<uint8_t>(sym);
                } else {
                    int rep, val = 0;
                    if (sym == 16) {
                        if (idx == 0) throw FormatError("inflate: repeat with no previous length");
                        val = all[idx - 1];
                        rep = 3 + static_cast<int>(br.bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(br.bits(3));
                    } else {
                        rep = 11 + static_cast<int>(br.bits(7));
                    }
                    if (idx + rep > nlen + ndist) throw FormatError("inflate: too many code lengths");
                    while (rep--) all[idx++] = static_cast<uint8_t>(val);
                }
            }
            Huffman lit, dist;
            lit.build(all, nlen);
            dist.build(all + nlen, ndist);
            inflateBlock(br, out, lit, dist, expectedSize);
        } else {
            throw FormatError("inflate: bad block type");
        }
    }
    if (out.size() != expectedSize) throw FormatError("inflate: output size mismatch");
    return out;
}

} // namespace cardbridge
