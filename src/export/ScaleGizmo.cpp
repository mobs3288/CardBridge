#include "export/ScaleGizmo.h"
#include "export/OverlayDraw.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::exporter {

namespace {

using namespace overlay;

constexpr uint32_t kAxisColour[3] = {0xFFE05555, 0xFF55E055, 0xFF5E8CF0};   // X red, Y green, Z blue
constexpr uint32_t kOutline = 0xFF161616, kWhite = 0xFFFFFFFF;
const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

} // namespace

ScaleGizmoState DrawScaleGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                               Vec3 origin, int hoverAxis, int activeAxis) {
    ScaleGizmoState st;
    if (bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return st;
    float ox, oy, depth;
    if (!view.Project(origin, ox, oy, depth)) return st;
    // Model units per pixel: the longest projected unit axis (orthographic, so exact unless edge-on).
    float pxPerM = 0.0f;
    for (int a = 0; a < 3; ++a) {
        float tx, ty, td;
        const Vec3 tip{origin.x + kAxisDir[a].x, origin.y + kAxisDir[a].y, origin.z + kAxisDir[a].z};
        if (view.Project(tip, tx, ty, td))
            pxPerM = std::max(pxPerM, std::sqrt((tx - ox) * (tx - ox) + (ty - oy) * (ty - oy)));
    }
    if (pxPerM < 1e-3f) pxPerM = 1e-3f;
    const float len = std::clamp(66.0f / pxPerM, 0.02f, 4.0f);   // ~66 px handles whatever the zoom
    struct Handle {
        float tx, ty, dirX, dirY, axisLen;
        bool ok;
    } h[3];
    for (int a = 0; a < 3; ++a) {
        const Vec3 tipW{origin.x + kAxisDir[a].x * len, origin.y + kAxisDir[a].y * len, origin.z + kAxisDir[a].z * len};
        float tx, ty, td;
        h[a].ok = view.Project(tipW, tx, ty, td);
        if (!h[a].ok) { h[a] = {0, 0, 1, 0, 0, false}; continue; }
        const float dx = tx - ox, dy = ty - oy;
        const float sl = std::sqrt(dx * dx + dy * dy);
        h[a].tx = tx; h[a].ty = ty; h[a].axisLen = sl;
        h[a].dirX = sl > 1e-4f ? dx / sl : 1.0f;
        h[a].dirY = sl > 1e-4f ? dy / sl : 0.0f;
    }
    st.ox = ox; st.oy = oy;
    for (int a = 0; a < 3; ++a) {
        st.tipX[a] = h[a].tx; st.tipY[a] = h[a].ty;
        st.dirX[a] = h[a].dirX; st.dirY[a] = h[a].dirY;
        st.axisLen[a] = h[a].axisLen;
    }
    // Far axes first: dim, hover, dragged.
    const auto orderOf = [&](int a) { return a == activeAxis ? 2 : (a == hoverAxis ? 1 : 0); };
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return orderOf(a) < orderOf(b); });
    for (int oi = 0; oi < 3; ++oi) {
        const int a = order[oi];
        if (!h[a].ok || h[a].axisLen < 2.0f) continue;
        const bool drag = a == activeAxis, hot = a == hoverAxis || drag;
        uint32_t col = kAxisColour[a];
        if (activeAxis >= 0 && !drag) col = mixToward(col, 0xFF3A3A3A, 0.55f);
        else if (hot) col = mixToward(col, kWhite, drag ? 0.45f : 0.25f);
        thickLine(bgra, width, height, ox, oy, h[a].tx, h[a].ty, col, kOutline, drag ? 2.0f : 1.25f);
        // A square at the tip marks the resize handle (an arrow would suggest a move).
        const float px = -h[a].dirY, py = h[a].dirX, half = drag ? 7.0f : 5.5f;
        const float bx = h[a].tx, by = h[a].ty;
        const float c[4][2] = {{bx + h[a].dirX * half + px * half, by + h[a].dirY * half + py * half},
                               {bx - h[a].dirX * half + px * half, by - h[a].dirY * half + py * half},
                               {bx - h[a].dirX * half - px * half, by - h[a].dirY * half - py * half},
                               {bx + h[a].dirX * half - px * half, by + h[a].dirY * half - py * half}};
        for (int k = 0; k < 4; ++k) {
            const float* p0 = c[k], *p1 = c[(k + 1) % 4];
            thickLine(bgra, width, height, p0[0], p0[1], p1[0], p1[1], col, kOutline, 1.25f);
        }
    }
    // The centre: a small white dot ringed in dark (the pivot the resize is about; not a drag target).
    disc(bgra, width, height, ox, oy, 5, kOutline);
    disc(bgra, width, height, ox, oy, 3, kWhite);
    return st;
}

int PickScaleGizmoAxis(const ScaleGizmoState& state, float x, float y, float radius) {
    if (state.ox < 0.0f) return -1;
    int best = -1;
    float bestD = radius * radius;
    for (int a = 0; a < 3; ++a) {
        if (state.axisLen[a] < 12.0f) continue;
        const float ax = state.ox, ay = state.oy, bx = state.tipX[a], by = state.tipY[a];
        const float abx = bx - ax, aby = by - ay;
        const float len2 = abx * abx + aby * aby;
        float t = len2 > 1e-6f ? ((x - ax) * abx + (y - ay) * aby) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float dx = x - (ax + abx * t), dy = y - (ay + aby * t);
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD) { bestD = d2; best = a; }
    }
    return best;
}

} // namespace cardbridge::exporter
