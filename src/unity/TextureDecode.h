#pragma once

#include "core/Bytes.h"
#include "core/Math.h"

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

// Tells an achromatic "white / grey eyeball skin" iris texture apart from a coloured one. Measured on the base
// game: every shipped hitomi is strongly coloured (chroma mean 66..182, achromatic fraction <= 0.31), while the
// mod's blank fill sits at 0.66. Thresholds are set well inside both groups.
struct FillSummary {
    float opaqueFraction = 1.0f;    // 0 when every pixel is (near-)transparent
    Vec3 avg{1, 1, 1};              // RGB mean of opaque pixels, 0..1
    float value = 1.0f;             // mean of the brightest channel per opaque pixel, 0..1
    float achromatic = 0.0f;        // fraction of opaque pixels with chroma <= 30/255
    float chromaMean = 0.0f;        // mean chroma per opaque pixel, 0..255
};
FillSummary AnalyzeFill(const uint8_t* rgba, int width, int height);

} // namespace cardbridge::unity
