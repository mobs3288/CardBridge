#pragma once

#include "core/Bytes.h"

namespace cardbridge {

// LZ4 block format decoder (what Unity calls LZ4 and LZ4HC - the HC variant
// only changes the encoder). `uncompressedSize` is known from the bundle's
// block table; the output must come out to exactly that size.
Bytes Lz4DecompressBlock(const uint8_t* src, size_t srcSize, size_t uncompressedSize);
// Decompresses into an existing buffer of uncompressedSize bytes (the caller
// owns it), avoiding a temporary Bytes copy for large bundle blocks.
void Lz4DecompressBlockTo(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t uncompressedSize);

} // namespace cardbridge
