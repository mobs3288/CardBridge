#pragma once

#include "core/Value.h"

namespace cardbridge {

// MessagePack decoder (the subset the card and list tables use: nil, bool,
// ints, floats, str, bin, array, map; ext types are skipped as Nil).
// Reads one document starting at `r`; trailing bytes are left for the caller.
Value MsgPackRead(ByteReader& r);
Value MsgPackParse(const uint8_t* data, size_t size);   // whole buffer must be one document
inline Value MsgPackParse(const Bytes& b) { return MsgPackParse(b.data(), b.size()); }

} // namespace cardbridge
