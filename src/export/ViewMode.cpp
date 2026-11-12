#include "export/ViewMode.h"

#include <string_view>

namespace cardbridge::exporter {

const char* ViewModeName(ViewMode m) {
    switch (m) {
    case ViewMode::Lit: return "lit";
    case ViewMode::Unlit: return "unlit";
    case ViewMode::BaseColor: return "basecolor";
    case ViewMode::Wireframe: return "wireframe";
    case ViewMode::Normal: return "normal";
    case ViewMode::Roughness: return "roughness";
    case ViewMode::Metallic: return "metallic";
    case ViewMode::Opacity: return "opacity";
    case ViewMode::UVChecker: return "uvchecker";
    case ViewMode::Transmission: return "transmission";
    case ViewMode::MaterialID: return "materialid";
    case ViewMode::SkinAO: return "skin-ao";
    default: return "lit";
    }
}

const char* ViewModeLabel(ViewMode m) {
    switch (m) {
    case ViewMode::Lit: return "Lit";
    case ViewMode::Unlit: return "Unlit";
    case ViewMode::BaseColor: return "Base";
    case ViewMode::Wireframe: return "Wire";
    case ViewMode::Normal: return "Normal";
    case ViewMode::Roughness: return "Rough";
    case ViewMode::Metallic: return "Metal";
    case ViewMode::Opacity: return "Opacity";
    case ViewMode::UVChecker: return "UV";
    case ViewMode::Transmission: return "Trans";
    case ViewMode::MaterialID: return "M-ID";
    case ViewMode::SkinAO: return "SkinAO";
    default: return "Lit";
    }
}

const char* ViewModeTooltip(ViewMode m) {
    switch (m) {
    case ViewMode::Lit: return "the game's shader (Lit) — the default view";
    case ViewMode::Unlit: return "texture x colour, unlit";
    case ViewMode::BaseColor: return "flat diffuse colour, no texture";
    case ViewMode::Wireframe: return "triangle edges only";
    case ViewMode::Normal: return "world normal direction as colour";
    case ViewMode::Roughness: return "interpreted roughness (gray)";
    case ViewMode::Metallic: return "interpreted metallic (gray)";
    case ViewMode::Opacity: return "alpha as gray";
    case ViewMode::UVChecker: return "UV checkerboard";
    case ViewMode::Transmission: return "wrap-light fabric transmission (back light)";
    case ViewMode::MaterialID: return "one colour per material";
    case ViewMode::SkinAO: return "the Koikatsu AO baked into the body skin — white = untouched, dark = darkened";
    default: return "the game's shader";
    }
}

ViewMode ViewModeFromName(const char* s) {
    if (!s) return ViewMode::Lit;
    for (int m = 0; m < static_cast<int>(ViewMode::Count); ++m)
        if (std::string_view{ViewModeName(static_cast<ViewMode>(m))} == s)
            return static_cast<ViewMode>(m);
    return ViewMode::Lit;
}

} // namespace exporter
