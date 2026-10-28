#pragma once

#include "assemble/Scene.h"
#include "core/Bytes.h"

namespace cardbridge::exporter {

struct PreviewOptions {
    int viewWidth = 560;
    int viewHeight = 640;
    float yawDegrees[3] = {0.0f, 90.0f, 180.0f};   // front, side, back (about Y)
    int views = 3;
    float yTop = 1.0f, yBottom = 0.0f;   // vertical window as fractions of the model height (1 = top); a close-up when narrower
};

// A small headless software renderer: orthographic, z-buffered, nearest-texel
// sampling, a headlight and flat ambient. It draws the parts with includeInExport
// set, so what it shows is what the exporter writes. It exists so a conversion can
// be looked at without a PMX viewer (and as the seed of the Milestone 2 viewport).
// Returns a PNG of the views side by side.
Bytes RenderPreviewPng(const assemble::Scene& scene, const PreviewOptions& opt = {});

} // namespace cardbridge::exporter
