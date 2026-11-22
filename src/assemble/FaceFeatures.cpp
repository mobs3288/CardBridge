#include "assemble/FaceFeatures.h"
#include "assemble/Assembler.h"   // SoftenNoseLineAlpha (the existing line feather, reused verbatim)
#include "assemble/Scene.h"
#include "core/Bytes.h"

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

float DetectNoseRidge(const std::vector<uint8_t>& rgba, int w, int h, std::vector<float>& weight) {
    weight.assign(static_cast<size_t>(w) * h, 0.0f);
    if (w < 32 || h < 32 || rgba.size() < static_cast<size_t>(w) * h * 4) return 0.0f;
    const int radius = std::max(3, w / 90);
    float peak = 0.0f;
    // The face is laid out symmetrically in its atlas. A ridge is a narrow vertical band near the centre column whose
    // pixels differ from the average of their horizontal neighbours. Work per row so the broad face shading cancels.
    for (int y = h / 8; y < h * 7 / 8; ++y) {
        for (int x = w / 2 - w / 14; x <= w / 2 + w / 14; ++x) {
            if (x - radius < 0 || x + radius >= w) continue;
            const size_t c = static_cast<size_t>(y) * w + x;
            double avg[3] = {0, 0, 0};
            for (int k = -radius; k <= radius; ++k) {
                if (k == 0) continue;
                const size_t n = static_cast<size_t>(y) * w + (x + k);
                for (int ch = 0; ch < 3; ++ch) avg[ch] += rgba[n * 4 + ch];
            }
            const double div = 2 * radius;
            float diff = 0.0f;
            for (int ch = 0; ch < 3; ++ch) {
                const double d = std::fabs(rgba[c * 4 + ch] - avg[ch] / div);
                diff = std::max(diff, static_cast<float>(d));
            }
            // Only a consistent darkening (a shading ridge), not a bright seam or a coloured painted line.
            bool darker = true;
            for (int ch = 0; ch < 3; ++ch)
                if (rgba[c * 4 + ch] > avg[ch] / div + 2.0) darker = false;
            const float v = darker ? std::min(1.0f, diff / 18.0f) : 0.0f;
            weight[c] = v;
            peak = std::max(peak, v);
        }
    }
    // A single speck is not a ridge: keep only bands that continue over many rows.
    int strongCols = 0;
    for (int x = w / 2 - w / 14; x <= w / 2 + w / 14; ++x) {
        int rows = 0;
        for (int y = h / 8; y < h * 7 / 8; ++y) if (weight[static_cast<size_t>(y) * w + x] > 0.35f) ++rows;
        if (rows > (h * 3 / 4) / 8) ++strongCols;
    }
    if (strongCols < 2) { weight.assign(static_cast<size_t>(w) * h, 0.0f); return 0.0f; }
    return peak;
}

void ApplyNoseRidge(std::vector<uint8_t>& rgba, int w, int h, const std::vector<float>& /*weight*/, float strength) {
    if (w < 32 || h < 32 || rgba.size() < static_cast<size_t>(w) * h * 4) return;
    // Amplifying a normal/detail map far past its authored values clips into white highlights, so the "more visible"
    // end is deliberately gentle; 2 is the strongest useful setting, not a doubled texture.
    const float k = std::min(1.0f, std::max(-0.25f, 1.0f - strength));   // 0 = authored, 1 = removed
    if (std::fabs(k) < 1e-4f) return;
    const int radius = std::max(3, w / 90);
    std::vector<uint8_t> src = rgba;   // sample the untouched image, so repeated applies cannot drift
    for (int y = h / 8; y < h * 7 / 8; ++y)
        for (int x = w / 2 - w / 14; x <= w / 2 + w / 14; ++x) {
            if (x - radius < 0 || x + radius >= w) continue;
            const size_t c = static_cast<size_t>(y) * w + x;
            for (int ch = 0; ch < 3; ++ch) {
                double avg = 0.0;
                for (int j = -radius; j <= radius; ++j) {
                    if (j == 0) continue;
                    avg += src[(static_cast<size_t>(y) * w + x + j) * 4 + ch];
                }
                avg /= 2 * radius;
                const double v = src[c * 4 + ch] * (1.0 - k) + avg * k;
                rgba[c * 4 + ch] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, v)) + 0.5);
            }
        }
}

void SetNoseRidgeStrength(Scene& scene, float strength) {
    NoseRidge& nr = scene.noseRidge;
    nr.strength = std::min(2.0f, std::max(0.0f, strength));
    if (nr.texture < 0 || static_cast<size_t>(nr.texture) >= scene.textures.size()) return;
    auto& tex = scene.textures[static_cast<size_t>(nr.texture)];
    if (nr.original.empty()) return;
    tex.rgba = nr.original;
    ApplyNoseRidge(tex.rgba, nr.width, nr.height, nr.mask, nr.strength);
    tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
}

} // namespace cardbridge::assemble
