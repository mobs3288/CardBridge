#pragma once

#include "assemble/Scene.h"
#include "core/Bytes.h"

#include <cstdint>
#include <memory>
#include <vector>

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

// ---- Interactive view (the CardBridge viewer): one orthographic camera that orbits the model.
struct ViewCamera {
    float yawDeg = 0, pitchDeg = 0;   // yaw about Y, then pitch about X
    float zoom = 1;                    // 1 = whole model fits the window
    float panX = 0, panY = 0;          // fractions of the window height
};

struct ViewState;                      // caches that survive between frames (texture mip chains)
struct ViewStateDeleter { void operator()(ViewState*) const; };
using ViewStatePtr = std::unique_ptr<ViewState, ViewStateDeleter>;
ViewStatePtr NewViewState();

struct ViewOptions {
    bool textured = true;
    const std::vector<char>* hiddenParts = nullptr;   // per scene part: 1 = do not draw (on top of visibleInPreview)
    int step = 1;                                      // 2 = every other pixel in each direction (fast, while dragging)
    bool shaded = false;                               // run the game's own compiled shaders per pixel (materials without one keep the plain look)
};

// Draws into a top-down BGRA buffer (width * height uint32, 0xAARRGGBB).
void RenderView(const assemble::Scene& scene, const ViewCamera& cam, int width, int height, const ViewOptions& opt,
                ViewState& state, std::vector<uint32_t>& bgra);

} // namespace cardbridge::exporter
