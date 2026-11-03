#pragma once

#include "assemble/Scene.h"
#include "export/GpuView.h"

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// Draws the skeleton over a finished picture of GpuView::Render: a line from each bone to its nearest shown ancestor and a dot on every
// bone, the selected one larger and warm coloured. `boneWorld` are the bones' positions as drawn (the posed ones; empty = the scene's rest
// positions). Helper bones (driven, shape, attach nodes) are left out unless `showHelpers`. `screen` gets, per bone, where it landed in
// pixels, or x < 0 for a bone that was not drawn, for picking. Returns false when the view has no picture yet.
bool DrawSkeletonOverlay(const assemble::Scene& scene, const std::vector<Vec3>& boneWorld, const GpuView& view, std::vector<uint32_t>& bgra, int width, int height,
                         int selectedBone, bool showHelpers, std::vector<std::pair<float, float>>* screen = nullptr);

// The bone nearest to a point in the picture within `radius` pixels, among those DrawSkeletonOverlay drew; -1 for none.
int PickBone(const std::vector<std::pair<float, float>>& screen, float x, float y, float radius);

} // namespace cardbridge::exporter
