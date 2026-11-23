#pragma once

#include <algorithm>
#include <string>

namespace cardbridge {
namespace exporter {

// Quality settings for the shader bake. The defaults are exactly the historical
// behaviour (1024, one sample per texel, simple dilation) so an export made without
// the new options is byte-identical to a pre-change export.
struct BakeQuality {
    int maxSize = 1024;          // longer side of the baked texture written to the PMX
    int supersample = 1;         // internal samples per output texel (1 = no supersampling)
    bool islandPadding = false;  // dilate within a UV island only, radius scaled to resolution
    bool areaShaderMaps = false; // shader maps use area resampling at maxSize instead of a 1024 box
};

inline constexpr int kMinBakeSize = 64;
inline constexpr int kMaxBakeSize = 8192;
// Supersampling never renders a texture larger than this internally. Combined with
// the preset sizes this keeps Ultra from spending 16x the time on already-huge sources.
inline constexpr int kMaxSupersampleSide = 4096;

inline int ClampBakeSize(int n) {
    if (n < kMinBakeSize) return kMinBakeSize;
    if (n > kMaxBakeSize) return kMaxBakeSize;
    return n;
}

// The baked texture's dimensions: keep the source size, halve only to stay under maxSize,
// then apply the historical 64-texel floor. Never upscales a small source.
inline void BakedTextureSize(int srcW, int srcH, int maxSize, int& outW, int& outH) {
    outW = std::max(1, srcW);
    outH = std::max(1, srcH);
    const int cap = ClampBakeSize(maxSize);
    while (std::max(outW, outH) > cap) {
        outW = std::max(1, outW / 2);
        outH = std::max(1, outH / 2);
    }
    outW = std::max(outW, kMinBakeSize);
    outH = std::max(outH, kMinBakeSize);
}

// Resolve a --bake-quality preset. Returns false for an unknown name (out untouched).
inline bool ResolveBakePreset(const std::string& name, BakeQuality& out) {
    if (name == "standard" || name.empty()) {
        out = BakeQuality{};
        return true;
    }
    if (name == "high") {
        out = BakeQuality{2048, 2, true, true};
        return true;
    }
    if (name == "ultra") {
        out = BakeQuality{4096, 2, true, true};
        return true;
    }
    return false;
}

// Uniform internal scale: never downscale below the output, and never render a side larger
// than kMaxSupersampleSide. Keeping one scale for both axes preserves texel density.
inline float SupersampleScale(int w, int h, int supersample) {
    if (supersample <= 1 || w <= 0 || h <= 0) return 1.0f;
    return std::max(1.0f, std::min(static_cast<float>(supersample),
                                   static_cast<float>(kMaxSupersampleSide) / static_cast<float>(std::max(w, h))));
}

inline int SupersampleSide(int side, float scale) {
    return std::max(1, static_cast<int>(static_cast<float>(side) * scale + 0.5f));
}

} // namespace exporter
} // namespace cardbridge
