#pragma once

#include "pmx/PmxWriter.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cardbridge::pmx {

// Native PMX importer. Reads PMX 2.0 and 2.1, both text encodings, all index
// sizes, every vertex deform type and every morph type. Anything after the
// joints (PMX 2.1 soft bodies and later sections) is kept verbatim in
// Model::format.trailing, so import -> export reproduces it exactly.
// Malformed input throws FormatError with the section and byte offset.

Model ReadPmx(const uint8_t* data, size_t size);
inline Model ReadPmx(const Bytes& bytes) { return ReadPmx(bytes.data(), bytes.size()); }
Model ReadPmxFile(const std::string& path);

} // namespace cardbridge::pmx
