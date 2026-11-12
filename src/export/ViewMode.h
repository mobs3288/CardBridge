#pragma once

// View modes for the material workbench's viewport diagnostics (spec S13,
// ARCHITECTURE_DECISIONS.md ADR-003 / ADR-008). One enum drives both renderers:
// the software rasterizer switches per pixel, the GPU path uses a dedicated
// runtime-compiled fallback shader and never touches the game's own shaders.
//
// Lit is the untouched game shader on both paths (the default view). Every
// mode is a *read-only diagnostic*: it never writes back to the scene.

#include <string>
#include <string_view>
#include <vector>

namespace cardbridge::exporter {

enum class ViewMode {
    Lit = 0,        // the game's shader (GPU) or the same look in software — current behaviour
    Unlit,          // texture x diffuse colour, no shading
    BaseColor,      // diffuse colour alone (no texture)
    Wireframe,      // triangle edges only (software: barycentric edge test; GPU: wireframe rasterizer)
    Normal,         // view-independent normal direction as RGB
    Roughness,      // interpreted roughness as gray
    Metallic,       // interpreted metallic as gray
    Opacity,        // alpha as gray
    UVChecker,      // UV checkerboard
    Transmission,   // wrap-light fabric transmission from a back light (ADR-004)
    MaterialID,     // one deterministic colour per material
    SkinAO,         // the baked Koikatsu AO contribution on the body skin (white = untouched, dark = darkened)
    Count
};

const char* ViewModeName(ViewMode m);     // settings token, stable
const char* ViewModeLabel(ViewMode m);     // short label for the viewport strip
const char* ViewModeTooltip(ViewMode m);   // one line for the footer
ViewMode ViewModeFromName(const char* s);  // ViewMode::Lit on anything unknown

// Per-material interpreted parameters the diagnostic modes read. Filled by
// the caller (the viewer's Material Workbench EffectiveMaterial); never
// persisted. Index == scene.materials index; entries may be missing
// (diagnostics stay zeroed).
struct DiagnosticMaterial {
    float roughness = 0.7f;             // 0 smooth .. 1 rough
    float metallic = 0.0f;              // 0..1
    float transmissionStrength = 0.0f;  // 0..1
    float transmissionThickness = 1.0f; // 0 thin .. 2 thick; exp(-thickness) attenuation
    float transmissionColor[3] = {1.0f, 1.0f, 1.0f};
    bool doubleSided = false;           // flip the wrap light for back faces when true
};

} // namespace exporter
