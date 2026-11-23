#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace cardbridge {
namespace image {

// Area-weighted resample of interleaved samples (spp <= 4), up or down, with fractional
// pixel overlap. `mask`, when given, marks which source pixels are valid: only those
// contribute, so an island's colour never bleeds out of its own coverage. `outMask`,
// when given, receives 1 where at least one valid source pixel contributed.
//
// The unmasked float path is deliberately bit-for-bit the algorithm previously in
// OutfitIcon.cpp::resample (float accumulation, no rounding) so that icon output is
// unchanged. The byte path rounds to nearest on write.
template <typename T>
inline void areaResample(const T* src, int sw, int sh, int spp,
                         T* dst, int dw, int dh,
                         const uint8_t* mask = nullptr, uint8_t* outMask = nullptr) {
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || !src || !dst) return;
    constexpr int kMaxChannels = 4;
    const int channels = std::min(spp, kMaxChannels);
    for (int y = 0; y < dh; ++y) {
        const float y0 = static_cast<float>(y) * sh / dh;
        const float y1 = static_cast<float>(y + 1) * sh / dh;
        const int syFirst = std::max(0, static_cast<int>(std::floor(y0)));
        const int syLast = std::min(sh - 1, static_cast<int>(std::ceil(y1)) - 1);
        for (int x = 0; x < dw; ++x) {
            const float x0 = static_cast<float>(x) * sw / dw;
            const float x1 = static_cast<float>(x + 1) * sw / dw;
            const int sxFirst = std::max(0, static_cast<int>(std::floor(x0)));
            const int sxLast = std::min(sw - 1, static_cast<int>(std::ceil(x1)) - 1);
            float acc[kMaxChannels] = {0, 0, 0, 0};
            float wsum = 0.0f;
            for (int sy = syFirst; sy <= syLast; ++sy) {
                const float wy = std::min(y1, sy + 1.0f) - std::max(y0, static_cast<float>(sy));
                if (wy <= 0.0f) continue;
                for (int sx = sxFirst; sx <= sxLast; ++sx) {
                    if (mask && !mask[static_cast<size_t>(sy) * sw + sx]) continue;
                    const float wx = std::min(x1, sx + 1.0f) - std::max(x0, static_cast<float>(sx));
                    if (wx <= 0.0f) continue;
                    const float w = wx * wy;
                    const T* s = src + (static_cast<size_t>(sy) * sw + sx) * spp;
                    for (int c = 0; c < channels; ++c) acc[c] += static_cast<float>(s[c]) * w;
                    wsum += w;
                }
            }
            T* d = dst + (static_cast<size_t>(y) * dw + x) * spp;
            if (outMask) outMask[static_cast<size_t>(y) * dw + x] = wsum > 0.0f ? 1 : 0;
            const float inv = wsum > 0.0f ? 1.0f / wsum : 0.0f;
            for (int c = 0; c < channels; ++c) {
                const float v = acc[c] * inv;
                if constexpr (std::is_same_v<T, uint8_t>)
                    d[c] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
                else
                    d[c] = static_cast<T>(v);
            }
        }
    }
}

} // namespace image
} // namespace cardbridge
