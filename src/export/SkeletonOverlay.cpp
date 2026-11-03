#include "export/SkeletonOverlay.h"

#include "export/BoneLabels.h"

#include <cmath>
#include <cstdlib>

namespace cardbridge::exporter {

namespace {

void put(std::vector<uint32_t>& px, int w, int h, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
}

void line(std::vector<uint32_t>& px, int w, int h, int x0, int y0, int x1, int y1, uint32_t c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 8192; ++guard) {
        put(px, w, h, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void dot(std::vector<uint32_t>& px, int w, int h, int x, int y, int r, uint32_t c) {
    for (int j = -r; j <= r; ++j) for (int i = -r; i <= r; ++i) put(px, w, h, x + i, y + j, c);
}

} // namespace

bool DrawSkeletonOverlay(const assemble::Scene& scene, const std::vector<Vec3>& boneWorld, const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                         int selectedBone, bool showHelpers, std::vector<std::pair<float, float>>* screen) {
    const size_t n = scene.bones.size();
    std::vector<std::pair<float, float>> pos(n, {-1.0f, -1.0f});
    if (bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) { if (screen) *screen = pos; return false; }
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        if (!showHelpers && IsHelperBone(scene.bones[i].name)) continue;
        const Vec3 at = boneWorld.size() == n ? boneWorld[i] : scene.bones[i].pos;
        float sx, sy, depth;
        if (view.Project(at, sx, sy, depth)) { pos[i] = {sx, sy}; any = true; }
    }
    const uint32_t lineColour = 0xFF2F6FE0, hot = 0xFFFFC21E, white = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i) {   // the line to the nearest drawn ancestor
        if (pos[i].first < 0) continue;
        int a = scene.bones[i].parent;
        while (a >= 0 && static_cast<size_t>(a) < n && pos[static_cast<size_t>(a)].first < 0)
            a = scene.bones[static_cast<size_t>(a)].parent == a ? -1 : scene.bones[static_cast<size_t>(a)].parent;
        if (a < 0 || static_cast<size_t>(a) >= n) continue;
        line(bgra, width, height, static_cast<int>(pos[static_cast<size_t>(a)].first), static_cast<int>(pos[static_cast<size_t>(a)].second),
             static_cast<int>(pos[i].first), static_cast<int>(pos[i].second), lineColour);
    }
    for (size_t i = 0; i < n; ++i) {
        if (pos[i].first < 0) continue;
        const bool sel = static_cast<int>(i) == selectedBone;
        const int x = static_cast<int>(pos[i].first), y = static_cast<int>(pos[i].second);
        dot(bgra, width, height, x, y, sel ? 4 : 2, sel ? hot : lineColour);
        dot(bgra, width, height, x, y, sel ? 2 : 1, white);
    }
    if (screen) *screen = std::move(pos);
    return any;
}

int PickBone(const std::vector<std::pair<float, float>>& screen, float x, float y, float radius) {
    int best = -1;
    float bestD = radius * radius;
    for (size_t i = 0; i < screen.size(); ++i) {
        if (screen[i].first < 0) continue;
        const float dx = screen[i].first - x, dy = screen[i].second - y;
        if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = static_cast<int>(i); }
    }
    return best;
}

} // namespace cardbridge::exporter
