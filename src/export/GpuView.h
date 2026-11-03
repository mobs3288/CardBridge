#pragma once

#include "assemble/Scene.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// The game's own compiled shaders run on the GPU: Direct3D 11 takes the DXBC programs straight out of the asset bundles (the
// same bytecode the software interpreter runs), the material properties fill their constant buffers, and the scene is drawn
// with the same orthographic framing as RenderView. Windows only (the whole tool is). Materials whose shader is not in the
// bundle are not drawn; skippedMaterials() says how many were left out.
class GpuView {
public:
    GpuView();
    ~GpuView();
    GpuView(const GpuView&) = delete;
    GpuView& operator=(const GpuView&) = delete;

    // Creates the Direct3D device (hardware, else the software rasteriser WARP). False with `why` when none works.
    bool Init(std::string* why = nullptr);
    bool ready() const;

    // Uploads the scene's meshes, textures and shader programs. Call again whenever the Scene changes; the Scene must outlive
    // the next Render calls.
    bool Load(const assemble::Scene& scene, std::string* why = nullptr);

    // Draws into a top-down BGRA buffer (0xAARRGGBB), framing the model the way RenderView does. hiddenParts as in ViewOptions (1 hides, 2 forces a default-hidden part on).
    // highlightPart gets a warm tint and a bright silhouette ring (the viewer's hovered component), -1 = none.
    bool Render(const ViewCamera& cam, int width, int height, const std::vector<char>* hiddenParts, std::vector<uint32_t>& bgra,
                int highlightPart = -1);

    int skippedMaterials() const;

    // Re-skins the loaded meshes with per-bone matrices (assemble::SkinMatrices): a pose edit shows at once, no new Load. nullptr or empty
    // goes back to the rest pose. The camera framing stays that of the rest pose so the picture does not jump while a bone moves.
    void SetSkin(const std::vector<Mat4>* skin);

    // Where a model-space point lands in the picture of the last Render (pixels from the top left) and its depth (smaller = nearer the camera).
    // False before the first Render.
    bool Project(Vec3 world, float& sx, float& sy, float& depth) const;

    // Plain mode draws every material with its texture and diffuse colour under one headlight and no colour grading, the way a
    // PMX viewer shows the exported file when the scene's textures are baked (BakeShaderTextures, colour only). Off by default.
    void SetPlain(bool on);
    bool plain() const;

    // Extra HLSL shading layers (ShaderLayer.h) drawn over the game's shaders. SetLayers compiles them (replacing earlier ones) and
    // returns false when none compiled; `report` gets one line per layer that did not. Layers start in their default state.
    bool SetLayers(const std::vector<ShaderLayer>& layers, std::string* report = nullptr);
    size_t layerCount() const;
    bool layerEnabled(size_t i) const;
    void SetLayerEnabled(size_t i, bool on);
    bool layerOk(size_t i) const;   // false when it failed to compile

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cardbridge::exporter
