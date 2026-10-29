#pragma once

#include "assemble/Scene.h"
#include "export/Preview.h"

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

    // Draws into a top-down BGRA buffer (0xAARRGGBB), framing the model the way RenderView does. hiddenParts as in ViewOptions.
    // highlightPart gets a warm tint and a bright silhouette ring (the viewer's hovered component), -1 = none.
    bool Render(const ViewCamera& cam, int width, int height, const std::vector<char>* hiddenParts, std::vector<uint32_t>& bgra,
                int highlightPart = -1);

    int skippedMaterials() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cardbridge::exporter
