#pragma once

#include "core/Bytes.h"

#include <string>
#include <vector>

namespace cardbridge::unity {

// Decodes the top mip of a Unity Texture2D to RGBA8, rows bottom-first as
// Unity stores them (the caller flips for PNG). Supported TextureFormat ids:
// 1 Alpha8, 3 RGB24, 4 RGBA32, 5 ARGB32, 10 DXT1, 12 DXT5, 14 BGRA32 -
// everything seen in the sampled bundles. Others throw FormatError naming the
// format so the material can be reported untextured.
std::vector<uint8_t> DecodeTexture(int unityFormat, int width, int height, const uint8_t* data, size_t size);

// The format string recorded on SceneTexture::format for a Unity format id.
// Unknown ids keep their raw form ("UnityFormat 42") so provenance survives.
std::string FormatName(int unityFormat);

} // namespace cardbridge::unity
