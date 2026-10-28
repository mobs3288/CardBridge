#pragma once

#include "core/Bytes.h"

namespace cardbridge {

// Raw DEFLATE (RFC 1951) decoder, as stored in .zip / .zipmod entries.
// `expectedSize` is the entry's uncompressed size from the zip directory.
Bytes InflateRaw(const uint8_t* src, size_t srcSize, size_t expectedSize);

} // namespace cardbridge
