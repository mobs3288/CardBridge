#pragma once

#include "assemble/Scene.h"
#include "export/GpuView.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// The resize gizmo for a scene part: three axis handles (X red, Y green, Z blue) drawn over a finished picture of
// GpuView::Render, anchored at `origin` (model space). Each handle is a square at the end of a ~66 px axis; dragging one
// scales the part along that axis about its bind-space centre. Hovering or dragging brightens it; the others dim while one
// is dragged. The returned state says where each handle landed in pixels, for picking; x < 0 means it was not drawn.
struct ScaleGizmoState {
    float ox = -1.0f, oy = -1.0f;   // the origin in pixels (x < 0: not drawn)
    float tipX[3] = {}, tipY[3] = {};
    float dirX[3] = {}, dirY[3] = {};        // unit screen direction of each axis
    float axisLen[3] = {};                   // projected handle length in pixels (< 12: too edge-on to grab)
};

// Draws over the frame. `hoverAxis` / `activeAxis`: -1 none, 0 X, 1 Y, 2 Z (active = the one being dragged).
ScaleGizmoState DrawScaleGizmo(const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                               Vec3 origin, int hoverAxis, int activeAxis);

// The axis handle nearest to a point within `radius` pixels (measured point-to-segment); -1 for none.
int PickScaleGizmoAxis(const ScaleGizmoState& state, float x, float y, float radius);

} // namespace cardbridge::exporter
