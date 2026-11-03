#pragma once

#include "assemble/Scene.h"
#include "core/Bytes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
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
    const std::vector<char>* hiddenParts = nullptr;   // per scene part: 1 = do not draw (on top of visibleInPreview), 2 = draw even when visibleInPreview is false
    int step = 1;                                      // 2 = every other pixel in each direction (fast, while dragging)
    bool shaded = false;                               // run the game's own compiled shaders per pixel (materials without one keep the plain look)
    int highlightPart = -1;                            // scene part drawn with a warm tint (the viewer's hovered part), -1 = none
    // The pose: per bone the matrix from the rest pose to the posed one (assemble::SkinMatrices, with the physics sim's chain bones on top).
    // Every vertex is skinned by it before it is drawn; the framing stays that of the rest pose so the picture does not jump while a bone
    // moves. nullptr or empty = the rest pose.
    const std::vector<Mat4>* skin = nullptr;
};

// Runs each material's compiled shader over its own UV layout (lit from the front, no cast shadow) and replaces the material's
// texture with the result, so the PMX carries the colour the game's shader produces. Materials without a shader are left alone.
// maxSize caps the baked texture's longer side. lit = run on the real mesh with the front view's light and camera, so the texture
// carries the toon shading too (the material is then unlit); otherwise the material is a flat sheet and only its colour is baked.
// grade = pass the baked colours through the game's colour grading (ColourGrade.h, when it is loaded and on), which the preview
// applies to every frame; a PMX has no such step, so this is what makes its colours match the preview.
// toon = also measure each material's shade as a function of N.L (SceneMaterial::toon): the shader is run over the flat sheet with the
// light swept from straight on to behind, and the result relative to the baked colour is the ramp a PMX toon texture carries. Ignored
// with lit (the shading is in the texture then).
void BakeShaderTextures(assemble::Scene& scene, int maxSize = 1024, bool lit = true, std::string* report = nullptr,
                        const std::function<void(const std::string&)>& progress = {}, bool grade = false, bool toon = false);

// Runs every pending Scene::createJobs: the game's "create" materials (eye iris and white, face and body skin) drawn over one quad
// into their placeholder textures. Called once, at the end of assembly.
void RunCreateJobs(assemble::Scene& scene);

// The hidden flags the renderers take (ViewOptions::hiddenParts), from the part checkboxes `hidden` (1 = unticked, one per scene part).
// A body (or bra) triangle a garment's mask hides is kept as an "under piece" part (ScenePart::underPiece): a piece is drawn (flag 2)
// when the part it was cut from is shown and either the "Nude body" part (ScenePart::nudeSwitch) is ticked or none of the garments over
// it is; a garment the game hides itself ("" in coveredBy) keeps covering. Every other part keeps its own flag.
std::vector<char> EffectiveHiddenParts(const assemble::Scene& scene, const std::vector<char>& hidden);

// Makes the exported PMX start the way the viewer shows the checkboxes: under pieces and unticked outfit, accessory and hair parts get
// ScenePart::startHidden (their PMX materials start transparent; the PMX morphs bring them back).
void ApplyPartStateForExport(assemble::Scene& scene, const std::vector<char>& hidden);

// Draws into a top-down BGRA buffer (width * height uint32, 0xAARRGGBB).
void RenderView(const assemble::Scene& scene, const ViewCamera& cam, int width, int height, const ViewOptions& opt,
                ViewState& state, std::vector<uint32_t>& bgra);

} // namespace cardbridge::exporter
