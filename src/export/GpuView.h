#pragma once

#include "assemble/Scene.h"
#include "export/Preview.h"
#include "export/ShaderLayer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cardbridge::exporter {

enum class GpuKind { Unknown, Hardware, Warp, Software };

struct GpuOptions {
    std::string preference = "auto";   // "auto", "hardware", "warp", "software"; CARDBRIDGE_GPU overrides it when set
    std::string skin = "gpu";          // "gpu" (compute shader when a device can run it), "cpu" (always the CPU skin),
                                       // "auto" (CPU below skinThresholdVerts, GPU above; CARDBRIDGE_SKIN overrides)
    size_t skinThresholdVerts = 0;     // "auto" mode only: meshes with fewer vertices use the CPU loop. 0 = follow the
                                       // benchmark suggestion (CARDBRIDGE_SKIN_THRESHOLD, else a conservative default).
};

// Calculated GPU resource allocation for skinning (bytes, from D3D11 buffer
// byte widths: element count * stride). Physical GPU memory (driver
// reservations, fragmentation) is not obtainable exactly through D3D11 and
// is not included; QueryVideoMemoryInfo when available reports the process
// budget separately. See AdapterSelect.h for the adapter policy and
// docs/GPU_SKINNING_HARDENING.md for the buffer layout.
struct SkinMemoryStats {
    size_t restBytes = 0;      // rest vertex buffers (plain VB, drawn at rest)
    size_t skinInBytes = 0;    // structured input buffers (shader SRV copy of the rest vertices)
    size_t skinOutBytes = 0;   // structured output buffers (shader UAV, copied to posed)
    size_t posedBytes = 0;     // posed vertex buffers (plain VB, drawn when posed)
    size_t boneBytes = 0;      // bone matrix structured buffer
    size_t totalGpuBytes = 0;  // rest + input + output + posed + bones
    size_t cpuMirrorBytes = 0; // CPU-side rest vertex copy (per vertex, kept for re-skinning)
    size_t cpuStagingBytes = 0; // transient CPU scratch for one CPU-skin update (posedVerts)
    size_t vertexCount = 0;
    size_t boneCount = 0;
    size_t bufferCount = 0;    // persistent GPU buffers (4 per mesh + bones)
};

// The game's own compiled shaders run on the GPU: Direct3D 11 takes the DXBC programs straight out of the asset bundles (the
// same bytecode the software interpreter runs), the material properties fill their constant buffers, and the scene is drawn
// with the same orthographic framing as RenderView. Windows only (the whole tool is). Materials whose shader is not in the
// bundle are not drawn; skippedMaterials() says how many were left out.
//
// When more than one adapter exists (a discrete card beside an integrated one), Init enumerates them and prefers
// the hardware adapter best suited to CardBridge's workload (see AdapterSelect.h: dedicated memory and capability,
// software/virtual adapters excluded, vendor only an equal tie-breaker), so the viewer and the export bake use the
// real GPU instead of the integrated one wherever it is. WARP (Direct3D running on the CPU) is the fallback when there
// is no usable hardware device, and when that is gone too the callers keep the software interpreter renderer. That CPU
// fallback is the supported path on a machine without a GPU; it is not a degraded second implementation.
class GpuView {
public:
    GpuView();
    ~GpuView();
    GpuView(const GpuView&) = delete;
    GpuView& operator=(const GpuView&) = delete;

    // Creates the Direct3D device: by default the best-suited hardware adapter (AdapterSelect.h), else WARP on the CPU.
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
    void SetSkinMode(const std::string& mode);   // "gpu" asks for the compute shader, "cpu" forces the CPU skin, "auto" picks per mesh
                                                 // by vertex count (applies to the next Load; SetSkin itself follows the mode at load time)
    void SetSkinThreshold(size_t verts);         // "auto" mode: meshes with fewer vertices use the CPU loop
    bool gpuSkin() const;              // the current pose is skinned by the GPU compute shader (not the CPU loop)
    SkinMemoryStats SkinMemory() const;  // calculated skinning allocations for the loaded scene (see SkinMemoryStats)

    // Benchmark hooks (off by default, no cost when off). With queries on, each skin dispatch is bracketed by
    // timestamp/disjoint queries; LastGpuDispatchMs() returns the GPU execution time of the last dispatch
    // (not CPU submission time). ReadbackPosed copies the posed buffer to the CPU for equivalence checks.
    void SetBenchQueries(bool on);
    bool BenchQueries() const;
    double LastGpuDispatchMs() const;  // -1 when unavailable (queries off, disjoint, or timed out)
    bool ReadbackPosed(std::vector<std::array<float, 3>>& outPos, std::vector<std::array<float, 3>>* outNrm = nullptr);

    // Uploads the scene's meshes, textures and shader programs. Call again whenever the Scene changes; the Scene must outlive
    // the next Render calls. `progress` (fraction 0..1, stage) and `cancelled` let the viewer drive its loading bar over the
    // upload; when cancelled fires the upload stops, the GPU copy is released, `why` is "cancelled by user" and Load returns false.
    bool Load(const assemble::Scene& scene, std::string* why = nullptr,
              const std::function<void(double, const std::string&)>& progress = {},
              const std::function<bool()>& cancelled = {});
    // Releases every uploaded mesh, texture, shader and the CPU mirror of the
    // vertices (GpuMesh::rest), keeping the device and its shared states for
    // the next Load. Call when the loaded scene is discarded so the previous
    // character's GPU copy does not stay resident beside the new one.
    void Unload();

    // Re-uploads scene textures whose pixels changed in place, and uploads new
    // ones (the Material Workbench's tint copies). The indices are
    // WorkbenchState::changedTextures. Only the named textures are touched: no
    // shader recompile, mesh upload or full Load. False when a texture could
    // not be created (the old view stays usable).
    bool UpdateTextures(const std::vector<int>& textureIndices);

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
