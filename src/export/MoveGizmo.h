#pragma once

#include "assemble/Scene.h"
#include "export/GpuView.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// The translate gizmo for an accessory: three axis arrows (X red, Y green, Z blue) drawn over a finished picture of GpuView::Render,
// anchored at `origin` (model space). Hovering or dragging an axis brightens it; the other two dim while one is dragged. Screen-space
// size: the arrows are ~64 px regardless of zoom. The returned state says where each axis landed in pixels, for picking; x < 0 in it
// means the gizmo was not drawn (the view has no picture yet).
struct MoveGizmoState {
    float ox = -1.0f, oy = -1.0f;   // the origin in pixels (x < 0: not drawn)
    float tipX[3] = {}, tipY[3] = {};
    float dirX[3] = {}, dirY[3] = {};        // unit screen direction of each axis
    float axisLen[3] = {};                   // projected axis length in pixels (< 12: too edge-on to grab)
    float worldPerPixel[3] = {};             // world length of one pixel along the axis
};

// Draws over the frame. `hoverAxis` / `activeAxis`: -1 none, 0 X, 1 Y, 2 Z (active = the one being dragged).
MoveGizmoState DrawMoveGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                             Vec3 origin, int hoverAxis, int activeAxis);

// The axis nearest to a point within `radius` pixels (measured point-to-segment); -1 for none. Axes seen edge-on are not pickable.
int PickMoveGizmoAxis(const MoveGizmoState& state, float x, float y, float radius);

} // namespace cardbridge::exporter
