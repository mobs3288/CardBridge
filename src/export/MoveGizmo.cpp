#include "export/MoveGizmo.h"
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

MoveGizmoState DrawMoveGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                             Vec3 origin, int hoverAxis, int activeAxis) {
    MoveGizmoState st;
    if (bgra.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return st;
    float ox, oy, depth;
    if (!view.Project(origin, ox, oy, depth)) return st;
    // The model-units per pixel: project the three unit axes and take the longest (orthographic, so any is exact unless it is edge-on).
    float pxPerM = 0.0f;
    for (int a = 0; a < 3; ++a) {
        float tx, ty, td;
        const Vec3 tip{origin.x + kAxisDir[a].x, origin.y + kAxisDir[a].y, origin.z + kAxisDir[a].z};
        if (view.Project(tip, tx, ty, td)) {
            const float d = std::sqrt((tx - ox) * (tx - ox) + (ty - oy) * (ty - oy));
            pxPerM = std::max(pxPerM, d);
        }
    }
    if (pxPerM < 1e-3f) pxPerM = 1e-3f;
    const float len = std::clamp(66.0f / pxPerM, 0.02f, 4.0f);   // ~66 px arrows whatever the zoom
    struct Axis {
        float tx, ty, dirX, dirY, axisLen, wpp;
        bool ok;
    } ax[3];
    for (int a = 0; a < 3; ++a) {
        const Vec3 tipW{origin.x + kAxisDir[a].x * len, origin.y + kAxisDir[a].y * len, origin.z + kAxisDir[a].z * len};
        float tx, ty, td;
        ax[a].ok = view.Project(tipW, tx, ty, td);
        if (!ax[a].ok) { ax[a].tx = ax[a].ty = ax[a].dirX = ax[a].dirY = 0.0f; ax[a].axisLen = ax[a].wpp = 0.0f; continue; }
        const float dx = tx - ox, dy = ty - oy;
        const float sl = std::sqrt(dx * dx + dy * dy);
        ax[a].tx = tx; ax[a].ty = ty;
        ax[a].axisLen = sl;
        ax[a].dirX = sl > 1e-4f ? dx / sl : 1.0f;
        ax[a].dirY = sl > 1e-4f ? dy / sl : 0.0f;
        ax[a].wpp = sl > 1e-4f ? len / sl : 0.0f;
    }
    st.ox = ox; st.oy = oy;
    for (int a = 0; a < 3; ++a) {
        st.tipX[a] = ax[a].tx; st.tipY[a] = ax[a].ty;
        st.dirX[a] = ax[a].dirX; st.dirY[a] = ax[a].dirY;
        st.axisLen[a] = ax[a].axisLen; st.worldPerPixel[a] = ax[a].wpp;
    }
    // The far axis is drawn first so the active one sits on top: dim ones, then hover, then the dragged one.
    const auto orderOf = [&](int a) { return a == activeAxis ? 2 : (a == hoverAxis ? 1 : 0); };
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return orderOf(a) < orderOf(b); });
    for (int oi = 0; oi < 3; ++oi) {
        const int a = order[oi];
        if (!ax[a].ok || ax[a].axisLen < 2.0f) continue;
        const bool drag = a == activeAxis, hot = a == hoverAxis || drag;
        uint32_t col = kAxisColour[a];
        if (activeAxis >= 0 && !drag) col = mixToward(col, 0xFF3A3A3A, 0.55f);   // one axis is being dragged: the others dim
        else if (hot) col = mixToward(col, kWhite, drag ? 0.45f : 0.25f);
        thickLine(bgra, width, height, ox, oy, ax[a].tx, ax[a].ty, col, kOutline, drag ? 2.0f : 1.25f);
        const float dx = ax[a].dirX, dy = ax[a].dirY, px = -dy, py = dx;
        const float headLen = drag ? 15.0f : 12.0f, half = drag ? 6.5f : 5.0f;
        fillTriangle(bgra, width, height,
                     ax[a].tx + dx * (headLen + 1.5f), ax[a].ty + dy * (headLen + 1.5f),
                     ax[a].tx + px * (half + 2.0f), ax[a].ty + py * (half + 2.0f),
                     ax[a].tx - px * (half + 2.0f), ax[a].ty - py * (half + 2.0f), kOutline);
        fillTriangle(bgra, width, height,
                     ax[a].tx + dx * headLen, ax[a].ty + dy * headLen,
                     ax[a].tx + px * half, ax[a].ty + py * half,
                     ax[a].tx - px * half, ax[a].ty - py * half, col);
    }
    // The centre: a small white dot ringed in dark (it marks the anchor; it is not a drag target).
    disc(bgra, width, height, ox, oy, 5, kOutline);
    disc(bgra, width, height, ox, oy, 3, kWhite);
    return st;
}

int PickMoveGizmoAxis(const MoveGizmoState& state, float x, float y, float radius) {
    if (state.ox < 0.0f) return -1;
    int best = -1;
    float bestD = radius * radius;
    for (int a = 0; a < 3; ++a) {
        if (state.axisLen[a] < 12.0f) continue;   // too edge-on to grab honestly
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
