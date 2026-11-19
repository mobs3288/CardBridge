#pragma once

// Viewer overlay pixel helpers shared by the gizmos drawn over a finished picture (MoveGizmo, RotateGizmo): filled
// triangles by scanline, thick outlined lines, discs and colour mixing, all writing into the picture's BGRA buffer.
// Self-contained (no GpuView), deliberately minimal; the skeleton overlay does its own thing.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cardbridge::exporter::overlay {

inline void put(std::vector<uint32_t>& px, int w, int h, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
}

// Filled triangle by scanline (barycentric signs).
inline void fillTriangle(std::vector<uint32_t>& px, int w, int h, float ax, float ay, float bx, float by, float cx, float cy, uint32_t c) {
    const int minX = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
    const int maxX = std::min(w - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
    const int maxY = std::min(h - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
    const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::fabs(d) < 1e-6f) return;
    const float inv = 1.0f / d;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x) {
            const float a = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) * inv;
            const float b = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) * inv;
            const float cc = 1.0f - a - b;
            if (a >= 0 && b >= 0 && cc >= 0) put(px, w, h, x, y, c);
        }
}

// A filled quad (a thick line): two triangles. Falls back to a dot when the two ends coincide.
inline void quadStrip(std::vector<uint32_t>& px, int w, int h, float x0, float y0, float x1, float y1, float halfWidth, uint32_t c) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) {
        fillTriangle(px, w, h, x0, y0, x0 + 1, y0, x0, y0 + 1, c);
        return;
    }
    const float nx = -dy / len * halfWidth, ny = dx / len * halfWidth;
    fillTriangle(px, w, h, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    fillTriangle(px, w, h, x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

// An outlined thick line: the dark quad first, the coloured one over it.
inline void thickLine(std::vector<uint32_t>& px, int w, int h, float x0, float y0, float x1, float y1, uint32_t c, uint32_t outline, float halfWidth) {
    quadStrip(px, w, h, x0, y0, x1, y1, halfWidth + 1.5f, outline);
    quadStrip(px, w, h, x0, y0, x1, y1, halfWidth, c);
}

inline void disc(std::vector<uint32_t>& px, int w, int h, float cx, float cy, int r, uint32_t c) {
    const int icx = static_cast<int>(std::lround(cx)), icy = static_cast<int>(std::lround(cy));
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            if (i * i + j * j <= r * r) put(px, w, h, icx + i, icy + j, c);
}

inline uint32_t mixToward(uint32_t a, uint32_t b, float t) {
    const auto ca = [&](int shift) { return (a >> shift) & 0xFF; };
    const auto cb = [&](int shift) { return (b >> shift) & 0xFF; };
    const auto ch = [&](int shift) { return static_cast<uint32_t>(ca(shift) + static_cast<int>((cb(shift) - ca(shift)) * t)) & 0xFF; };
    return 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

} // namespace cardbridge::exporter::overlay
