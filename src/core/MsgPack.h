#pragma once

#include "core/Value.h"

namespace cardbridge {

// MessagePack decoder (the subset the card and list tables use: nil, bool,
// ints, floats, str, bin, array, map; ext types are skipped as Nil).
// Reads one document starting at `r`; trailing bytes are left for the caller.
Value MsgPackRead(ByteReader& r);
Value MsgPackParse(const uint8_t* data, size_t size);   // whole buffer must be one document
inline Value MsgPackParse(const Bytes& b) { return MsgPackParse(b.data(), b.size()); }

// MessagePack encoder for the same subset (ints as the smallest fitting form,
// floats as float32, as the game writes them). Used to merge plugin payloads
// (ABMX, MaterialEditor) when a clothing file is applied: without it the
// outfit would show its prefab look and fit without its bone mods.
Bytes MsgPackPack(const Value& v);

} // namespace cardbridge
