#pragma once

#include "assemble/Scene.h"
#include "export/GpuView.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// The rotate gizmo for a scene part: one ring per axis (X red, Y green, Z blue) drawn over a finished picture of
// GpuView::Render, centred at `origin` (model space). Hovering or dragging a ring brightens it; the other two dim while one is
// dragged. Screen-space size: the rings are ~46 px in radius regardless of zoom. The returned state says where each ring's
// polyline landed in pixels, for picking; x < 0 in it means the gizmo was not drawn (the view has no picture yet).
struct RotateGizmoState {
    static constexpr int kRingSamples = 64;   // points per ring (closed loop)
    float ox = -1.0f, oy = -1.0f;   // the centre in pixels (x < 0: not drawn)
    float uX[3] = {}, uY[3] = {};   // projected unit in-plane basis vector U of axis a (pixels per world unit)
    float vX[3] = {}, vY[3] = {};   // projected unit in-plane basis vector V of axis a (pixels per world unit)
    float det[3] = {};              // u x v: the signed face-on measure of the ring (0 = seen edge-on)
    float ringPx[3] = {};           // projected mean ring radius in pixels (informational; the layout dump reports it)
    float minorPx[3] = {};          // the minor semi-axis of the projected ellipse in pixels (< 12: too edge-on to grab)
    float ringWorld = 0.0f;         // the ring's radius in world units
    std::vector<std::array<float, 2>> ring[3];   // the projected ring polygons in pixels (absolute)
};

// Draws over the frame. `hoverAxis` / `activeAxis`: -1 none, 0 X, 1 Y, 2 Z (active = the one being dragged).
RotateGizmoState DrawRotateGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                                 Vec3 origin, int hoverAxis, int activeAxis);

// The ring nearest to a point within `radius` pixels (measured point-to-segment over the projected polygon); -1 for none.
// Rings seen edge-on are not pickable.
int PickRotateGizmoAxis(const RotateGizmoState& state, float x, float y, float radius);

// The pointer's angle around axis `axis` in the ring plane (radians, the same convention for every call), from the exact
// inverse of the orthographic projection: the ring maps to the unit circle in (U, V) plane coordinates and the pointer maps
// with it. False when the ring is too edge-on to invert, or the pointer sits too close to the centre. The drag accumulates the
// wrapped difference between two angles.
bool RotateGizmoPointerAngle(const RotateGizmoState& state, int axis, float x, float y, float& angle);

} // namespace cardbridge::exporter
