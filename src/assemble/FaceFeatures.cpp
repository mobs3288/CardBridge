#include "assemble/FaceFeatures.h"
#include "assemble/Assembler.h"   // SoftenNoseLineAlpha (the existing line feather, reused verbatim)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace cardbridge::assemble {
namespace {

struct Comp {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int pixels = 0;
    float sumX = 0.0f, sumY = 0.0f;
    float cx() const { return pixels ? sumX / pixels : 0.0f; }
    float cy() const { return pixels ? sumY / pixels : 0.0f; }
    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
};

// Connected components of the (thresholded) alpha mask, 4-connected. Small isolated texels from lossy compression are
// skipped by the caller's threshold; this is only about the shapes the game paints.
std::vector<Comp> Components(const std::vector<uint8_t>& alpha, int w, int h, uint8_t threshold) {
    std::vector<uint8_t> seen(static_cast<size_t>(w) * h, 0);
    std::vector<Comp> out;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (seen[i] || alpha[i] < threshold) continue;
            Comp c;
            c.x0 = x; c.y0 = y; c.x1 = x; c.y1 = y;
            seen[i] = 1;
            stack.clear();
            stack.push_back(i);
            while (!stack.empty()) {
                const size_t k = stack.back();
                stack.pop_back();
                const int kx = static_cast<int>(k % w), ky = static_cast<int>(k / w);
                c.x0 = std::min(c.x0, kx); c.x1 = std::max(c.x1, kx);
                c.y0 = std::min(c.y0, ky); c.y1 = std::max(c.y1, ky);
                ++c.pixels;
                c.sumX += kx; c.sumY += ky;
                auto push = [&](int nx, int ny) {
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
                    const size_t n = static_cast<size_t>(ny) * w + nx;
                    if (seen[n] || alpha[n] < threshold) return;
                    seen[n] = 1;
                    stack.push_back(n);
                };
                push(kx - 1, ky); push(kx + 1, ky); push(kx, ky - 1); push(kx, ky + 1);
            }
            if (c.pixels >= 3) out.push_back(c);
        }
    return out;
}

std::vector<uint8_t> AlphaOf(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> a(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < a.size(); ++i) a[i] = rgba[i * 4 + 3];
    return a;
}

// The stock nose-line rows draw a near-opaque, very tall thin stroke down the centre of the atlas. Nostril rows draw
// short wide arcs (or two dots) near the bottom. Use shape ratios rather than absolute pixel positions so every
// resolution and every modded atlas is judged the same way.
bool IsLine(const Comp& c, int w, int h) {
    if (c.pixels < 6) return false;
    const float centre = w * 0.5f;
    const bool middle = std::fabs(c.cx() - centre) <= w * 0.14f;
    return middle && c.height() >= 4 && static_cast<float>(c.height()) >= c.width() * 2.2f &&
           static_cast<float>(c.height()) >= h * 0.06f;
}

bool IsNostril(const Comp& c, int w, int h) {
    (void)h;
    if (c.pixels < 3) return false;
    // Short and wide (an arc or a dot pair's half), not a tall centre stroke.
    return static_cast<float>(c.width()) >= c.height() * 0.9f && c.width() >= 3;
}

} // namespace

NoseFeatures DetectNoseFeatures(const std::vector<uint8_t>& rgba, int w, int h) {
    NoseFeatures f;
    if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return f;
    const std::vector<uint8_t> alpha = AlphaOf(rgba, w, h);
    uint8_t peak = 0;
    for (uint8_t a : alpha) peak = std::max(peak, a);
    if (peak == 0) return f;
    // Work relative to the texture's own peak: a mod whose nostrils top out at alpha 146 is just as authored as a
    // base-game nose line that reaches 255. A quarter of the peak keeps anti-aliased fringes out of the components.
    const uint8_t threshold = static_cast<uint8_t>(std::max(4, static_cast<int>(peak) / 4));
    for (const Comp& c : Components(alpha, w, h, threshold)) {
        if (IsLine(c, w, h)) f.line = true;
        else if (IsNostril(c, w, h)) f.nostrils = true;
    }
    return f;
}

void SoftenNoseLineOnly(std::vector<uint8_t>& rgba, int w, int h, const NoseFeatures& features) {
    if (!features.line || w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return;
    const std::vector<uint8_t> alpha = AlphaOf(rgba, w, h);
    uint8_t peak = 0;
    for (uint8_t a : alpha) peak = std::max(peak, a);
    if (peak == 0) return;
    const uint8_t threshold = static_cast<uint8_t>(std::max(4, static_cast<int>(peak) / 4));
    std::vector<uint8_t> lineMask(static_cast<size_t>(w) * h, 0);
    for (const Comp& c : Components(alpha, w, h, threshold)) {
        if (!IsLine(c, w, h)) continue;
        for (int y = c.y0; y <= c.y1; ++y)
            for (int x = c.x0; x <= c.x1; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (alpha[i] >= threshold) lineMask[i] = 1;
            }
    }
    bool anyLine = false;
    for (uint8_t v : lineMask) anyLine |= v != 0;
    if (!anyLine) return;

    // Reuse the existing feather exactly on a copy of the alpha, then put back ONLY the line's new alpha. Everything
    // outside the line components - nostrils included - keeps its original bytes, so its cutout/banding never changes.
    std::vector<uint8_t> soft = rgba;
    SoftenNoseLineAlpha(soft, w, h);
    for (size_t i = 0; i < lineMask.size(); ++i)
        if (lineMask[i]) rgba[i * 4 + 3] = soft[i * 4 + 3];
}

LipLineFeatures DetectLipLineFeatures(const std::vector<uint8_t>& rgba, int w, int h) {
    LipLineFeatures f;
    if (w <= 0 || h <= 0 || rgba.size() < static_cast<size_t>(w) * h * 4) return f;
    const std::vector<uint8_t> alpha = AlphaOf(rgba, w, h);
    uint8_t peak = 0;
    for (uint8_t a : alpha) peak = std::max(peak, a);
    if (peak == 0) return f;
    const uint8_t threshold = static_cast<uint8_t>(std::max(4, static_cast<int>(peak) / 3));
    auto comps = Components(alpha, w, h, threshold);
    // The upper lip line sits above the lower one. Two separate strokes -> both present; one stroke -> just that one.
    // Classify by the component's centre row relative to the atlas, not by card data.
    for (const Comp& c : comps) {
        const bool wide = static_cast<float>(c.width()) >= c.height() * 1.5f;
        if (!wide) continue;
        if (c.cy() < h * 0.5f) f.upper = true;
        else f.lower = true;
    }
    if (comps.size() == 1 && !f.upper && !f.lower) f.upper = true;
    return f;
}

} // namespace cardbridge::assemble
