#pragma once

#include "assemble/Scene.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::exporter {

enum class GpuKind { Unknown, Hardware, Warp, Software };

struct GpuOptions {
    std::string preference = "auto";   // "auto", "hardware", "warp", "software"; CARDBRIDGE_GPU overrides it when set
    std::string skin = "gpu";          // "gpu" (compute shader when a device can run it), "cpu" (always the CPU skin)
};

// The game's own compiled shaders run on the GPU: Direct3D 11 takes the DXBC programs straight out of the asset bundles (the
// same bytecode the software interpreter runs), the material properties fill their constant buffers, and the scene is drawn
// with the same orthographic framing as RenderView. Windows only (the whole tool is). Materials whose shader is not in the
// bundle are not drawn; skippedMaterials() says how many were left out.
//
// When more than one adapter exists (a discrete NVIDIA/AMD card beside an integrated one), Init enumerates them and prefers
// a discrete hardware adapter, so the viewer and the export bake use the real GPU instead of the integrated one wherever
// it is. WARP (Direct3D running on the CPU) is the fallback when there is no usable hardware device, and when that is
// gone too the callers keep the software interpreter renderer. That CPU fallback is the supported path on a machine
// without a GPU; it is not a degraded second implementation.
class GpuView {
public:
    GpuView();
    ~GpuView();
    GpuView(const GpuView&) = delete;
    GpuView& operator=(const GpuView&) = delete;

    // Creates the Direct3D device: by default the best hardware adapter (discrete before integrated), else WARP on the CPU.
    // False with `why` when none works; kind() then says Software and the caller falls back to RenderView. The preference
    // may be overridden by CARDBRIDGE_GPU (auto/hardware/warp/software) and CARDBRIDGE_GPU_ADAPTER=N (adapter index, 0-based
    // over the enumerated adapters before any ordering).
    bool Init(std::string* why = nullptr);
    bool Init(const GpuOptions& opt, std::string* why = nullptr);
    // Tears the Direct3D device down completely, so the next Init builds a new one on a different adapter (the viewer hot-swaps
    // the device from the Setup tab without restarting). After Shutdown the view is not ready until Init succeeds again, and
    // any loaded scene is gone. Safe to call when no device exists.
    void Shutdown();
    bool ready() const;
    GpuKind kind() const;              // Hardware, Warp (CPU) or Software after Init
    std::string adapter() const;       // short device name, e.g. "NVIDIA GeForce ..." or "WARP (CPU)"
    void SetSkinMode(const std::string& mode);   // "gpu" asks for the compute shader, "cpu" forces the CPU skin (applies to the next Load)
    bool gpuSkin() const;              // the current pose is skinned by the GPU compute shader (not the CPU loop)

    // Uploads the scene's meshes, textures and shader programs. Call again whenever the Scene changes; the Scene must outlive
    // the next Render calls.
    bool Load(const assemble::Scene& scene, std::string* why = nullptr);
    // Releases every uploaded mesh, texture, shader and the CPU mirror of the
    // vertices (GpuMesh::rest), keeping the device and its shared states for
    // the next Load. Call when the loaded scene is discarded so the previous
    // character's GPU copy does not stay resident beside the new one.
    void Unload();

    // Draws into a top-down BGRA buffer (0xAARRGGBB), framing the model the way RenderView does. hiddenParts as in ViewOptions (1 hides, 2 forces a default-hidden part on).
    // highlightPart gets a warm tint and a bright silhouette ring (the viewer's hovered component), -1 = none.
    bool Render(const ViewCamera& cam, int width, int height, const std::vector<char>* hiddenParts, std::vector<uint32_t>& bgra,
                int highlightPart = -1);

    int skippedMaterials() const;

    // Re-skins the loaded meshes with per-bone matrices (assemble::SkinMatrices): a pose edit shows at once, no new Load. nullptr or empty
    // goes back to the rest pose. The camera framing stays that of the rest pose so the picture does not jump while a bone moves.
    void SetSkin(const std::vector<Mat4>* skin);

    // The scene's vertices changed in place (a re-shape): uploads the new positions and normals, keeping the current skin. Tangents (the normal
    // map frame) are recomputed only when asked; leave them while a slider is being dragged and ask once it is released.
    void Refresh(bool tangents);

    // Where a model-space point lands in the picture of the last Render (pixels from the top left) and its depth (smaller = nearer the camera).
    // False before the first Render.
    bool Project(Vec3 world, float& sx, float& sy, float& depth) const;

    // Plain mode draws the scene the way MikuMikuDance shows the exported PMX when its textures are baked (BakeShaderTextures, colour
    // only): each material as the PMX writes it (MmdMaterialLook: colours, toon ramp, edge), MMD's default light, its self-shadow
    // (the lit colour or the ramp's bottom row, by N.L and a shadow map) and its ground shadow, and no colour grading. Off by default.
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
