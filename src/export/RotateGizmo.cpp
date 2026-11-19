#include "export/RotateGizmo.h"
#include "export/OverlayDraw.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::exporter {

namespace {

using namespace overlay;

constexpr uint32_t kAxisColour[3] = {0xFFE05555, 0xFF55E055, 0xFF5E8CF0};   // X red, Y green, Z blue
constexpr uint32_t kOutline = 0xFF161616, kWhite = 0xFFFFFFFF;
// The in-plane basis of each ring (unit, world), chosen right-handed with the axis (U x V = axis), so a positive angle
// from U to V is a positive rotation about the axis.
const Vec3 kRingU[3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
const Vec3 kRingV[3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
const Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
constexpr float kTwoPi = 6.28318530718f;
constexpr float kEdgeOnPx = 12.0f;   // minor semi-axis below this: too edge-on to grab honestly

// The projected ring's minor semi-axis in pixels (from the singular values of the 2x2 projected-basis matrix; the ellipse's
// foreshortening, independent of zoom).
float minorAxisPx(float uX, float uY, float vX, float vY, float ringWorld) {
    const float tu = uX * uX + uY * uY, tv = vX * vX + vY * vY;
    const float det = uX * vY - uY * vX;
    const float trace = tu + tv;
    const float disc = std::max(0.0f, trace * trace - 4.0f * det * det);
    const float sMin2 = (trace - std::sqrt(disc)) * 0.5f;
    return sMin2 > 0.0f ? std::sqrt(sMin2) * ringWorld : 0.0f;
}

} // namespace

RotateGizmoState DrawRotateGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                                 Vec3 origin, int hoverAxis, int activeAxis) {
    RotateGizmoState st;
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
    const float radius = std::clamp(46.0f / pxPerM, 0.02f, 4.0f);   // ~46 px rings whatever the zoom
    st.ox = ox; st.oy = oy; st.ringWorld = radius;
    struct Ring {
        float uX, uY, vX, vY, det, ringPx, minorPx;
        bool ok;
    } rg[3];
    for (int a = 0; a < 3; ++a) {
        const Vec3 U = kRingU[a], V = kRingV[a];
        const Vec3 tipU{origin.x + U.x, origin.y + U.y, origin.z + U.z};
        const Vec3 tipV{origin.x + V.x, origin.y + V.y, origin.z + V.z};
        float ux, uy, ud, vx, vy, vd;
        rg[a].ok = view.Project(tipU, ux, uy, ud) && view.Project(tipV, vx, vy, vd);
        if (!rg[a].ok) { rg[a] = {0, 0, 0, 0, 0, 0, 0, false}; continue; }
        rg[a].uX = ux - ox; rg[a].uY = uy - oy; rg[a].vX = vx - ox; rg[a].vY = vy - oy;
        rg[a].det = rg[a].uX * rg[a].vY - rg[a].uY * rg[a].vX;
        rg[a].ringPx = radius * 0.5f * (std::sqrt(rg[a].uX * rg[a].uX + rg[a].uY * rg[a].uY) + std::sqrt(rg[a].vX * rg[a].vX + rg[a].vY * rg[a].vY));
        rg[a].minorPx = minorAxisPx(rg[a].uX, rg[a].uY, rg[a].vX, rg[a].vY, radius);
        // The projected polygon: sample the world circle and project every point (exact even edge-on, where it degenerates to a line).
        st.ring[a].clear();
        st.ring[a].reserve(RotateGizmoState::kRingSamples);
        for (int k = 0; k < RotateGizmoState::kRingSamples; ++k) {
            const float t = kTwoPi * static_cast<float>(k) / static_cast<float>(RotateGizmoState::kRingSamples);
            const float c = std::cos(t), s = std::sin(t);
            const Vec3 w{origin.x + radius * (c * U.x + s * V.x),
                         origin.y + radius * (c * U.y + s * V.y),
                         origin.z + radius * (c * U.z + s * V.z)};
            float px, py, pd;
            if (view.Project(w, px, py, pd)) st.ring[a].push_back({px, py});
        }
    }
    for (int a = 0; a < 3; ++a) {
        st.uX[a] = rg[a].uX; st.uY[a] = rg[a].uY; st.vX[a] = rg[a].vX; st.vY[a] = rg[a].vY;
        st.det[a] = rg[a].det; st.ringPx[a] = rg[a].ringPx; st.minorPx[a] = rg[a].minorPx;
    }
    // The far ring is drawn first so the active one sits on top: dim ones, then hover, then the dragged one.
    const auto orderOf = [&](int a) { return a == activeAxis ? 2 : (a == hoverAxis ? 1 : 0); };
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return orderOf(a) < orderOf(b); });
    for (int oi = 0; oi < 3; ++oi) {
        const int a = order[oi];
        if (!rg[a].ok || st.ring[a].size() < 8) continue;
        const bool drag = a == activeAxis, hot = a == hoverAxis || drag;
        uint32_t col = kAxisColour[a];
        if (activeAxis >= 0 && !drag) col = mixToward(col, 0xFF3A3A3A, 0.55f);   // one ring is being dragged: the others dim
        else if (hot) col = mixToward(col, kWhite, drag ? 0.45f : 0.25f);
        const float hw = drag ? 2.0f : 1.25f;
        const size_t n = st.ring[a].size();
        for (size_t k = 0; k < n; ++k) {
            const std::array<float, 2>& p0 = st.ring[a][k], &p1 = st.ring[a][(k + 1) % n];
            thickLine(bgra, width, height, p0[0], p0[1], p1[0], p1[1], col, kOutline, hw);
        }
    }
    // The centre: a small white dot ringed in dark (it marks the anchor; it is not a drag target).
    disc(bgra, width, height, ox, oy, 5, kOutline);
    disc(bgra, width, height, ox, oy, 3, kWhite);
    return st;
}

int PickRotateGizmoAxis(const RotateGizmoState& state, float x, float y, float radius) {
    if (state.ox < 0.0f) return -1;
    int best = -1;
    float bestD = radius * radius;
    for (int a = 0; a < 3; ++a) {
        if (state.minorPx[a] < kEdgeOnPx) continue;   // too edge-on to grab honestly
        const std::vector<std::array<float, 2>>& ring = state.ring[a];
        const size_t n = ring.size();
        if (n < 3) continue;
        for (size_t k = 0; k < n; ++k) {
            const float ax = ring[k][0], ay = ring[k][1], bx = ring[(k + 1) % n][0], by = ring[(k + 1) % n][1];
            const float abx = bx - ax, aby = by - ay;
            const float len2 = abx * abx + aby * aby;
            float t = len2 > 1e-6f ? ((x - ax) * abx + (y - ay) * aby) / len2 : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float dx = x - (ax + abx * t), dy = y - (ay + aby * t);
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestD) { bestD = d2; best = a; }
        }
    }
    return best;
}

bool RotateGizmoPointerAngle(const RotateGizmoState& state, int axis, float x, float y, float& angle) {
    if (state.ox < 0.0f || axis < 0 || axis > 2) return false;
    if (state.minorPx[axis] < kEdgeOnPx) return false;   // too edge-on to invert honestly
    const float uX = state.uX[axis], uY = state.uY[axis], vX = state.vX[axis], vY = state.vY[axis];
    const float det = uX * vY - uY * vX;
    if (std::fabs(det) < 1e-6f) return false;
    const float qx = x - state.ox, qy = y - state.oy;
    const float a = (qx * vY - qy * vX) / det;
    const float b = (uX * qy - uY * qx) / det;
    if (a * a + b * b < 1e-4f) return false;   // too close to the centre: the angle is noise there
    angle = std::atan2(b, a);
    return true;
}

} // namespace cardbridge::exporter
