#include "core/Lz4.h"

namespace cardbridge {

void Lz4DecompressBlockTo(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t uncompressedSize) {
    uint8_t* out = dst;
    size_t ip = 0, op = 0;
    while (ip < srcSize) {
        uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= srcSize) throw FormatError("LZ4: truncated literal length");
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > srcSize - ip || lit > uncompressedSize - op) throw FormatError("LZ4: literal run out of range");
        for (size_t i = 0; i < lit; ++i) out[op + i] = src[ip + i];
        ip += lit;
        op += lit;
        if (ip >= srcSize) break; // the last sequence is literals only
        if (srcSize - ip < 2) throw FormatError("LZ4: truncated match offset");
        size_t offset = src[ip] | (static_cast<size_t>(src[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op) throw FormatError("LZ4: bad match offset");
        size_t len = (token & 15);
        if (len == 15) {
            uint8_t b;
            do {
                if (ip >= srcSize) throw FormatError("LZ4: truncated match length");
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (len > uncompressedSize - op) throw FormatError("LZ4: match runs past the output");
        // Byte by byte: matches may overlap their own output (offset < len).
        for (size_t i = 0; i < len; ++i) out[op + i] = out[op - offset + i];
        op += len;
    }
    if (op != uncompressedSize) throw FormatError("LZ4: output size mismatch");
}

Bytes Lz4DecompressBlock(const uint8_t* src, size_t srcSize, size_t uncompressedSize) {
    Bytes out(uncompressedSize);
    Lz4DecompressBlockTo(src, srcSize, out.data(), uncompressedSize);
    return out;
}

} // namespace cardbridge
