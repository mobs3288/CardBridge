#include "export/ColourGrade.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "core/FileIo.h"

namespace cardbridge::exporter {

namespace {

constexpr int kSize = 32;
std::vector<float> g_lut;   // [b][g][r][3], 0..255
bool g_on = !std::getenv("CARDBRIDGE_NO_GRADE");

}

bool LoadColourGrade(const std::string& libraryDir) {
    g_lut.clear();
    const std::string path = libraryDir + "/colorgrade/lut_amount.png";
    if (!fileio::Exists(path)) return false;
    Bytes bytes = fileio::ReadFile(path);
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &n, 3);
    if (!px) return false;
    if (w != kSize * kSize || h != kSize) { stbi_image_free(px); return false; }
    g_lut.resize(static_cast<size_t>(kSize) * kSize * kSize * 3);
    // The strip holds one square per blue slice, left to right; inside a square red runs along x and green up (row 0 is the top).
    for (int b = 0; b < kSize; ++b)
        for (int g = 0; g < kSize; ++g)
            for (int r = 0; r < kSize; ++r)
                for (int c = 0; c < 3; ++c)
                    g_lut[((static_cast<size_t>(b) * kSize + g) * kSize + r) * 3 + c] = px[(static_cast<size_t>(kSize - 1 - g) * w + b * kSize + r) * 3 + c];
    stbi_image_free(px);
    return true;
}

bool ColourGradeLoaded() { return !g_lut.empty(); }
void SetColourGradeEnabled(bool on) { g_on = on; }
bool ColourGradeEnabled() { return g_on && !g_lut.empty(); }

void ApplyColourGrade(std::vector<uint32_t>& bgra, uint32_t skip) {
    if (!ColourGradeEnabled()) return;
    const float k = (kSize - 1) / 255.0f;
    auto work = [&](size_t from, size_t to) {
        for (size_t i = from; i < to; ++i) {
            const uint32_t p = bgra[i];
            if (p == skip) continue;
            const float v[3] = {((p >> 16) & 255) * k, ((p >> 8) & 255) * k, (p & 255) * k};   // r, g, b in table units
            int i0[3];
            float f[3];
            for (int c = 0; c < 3; ++c) { i0[c] = std::min(static_cast<int>(v[c]), kSize - 2); f[c] = v[c] - i0[c]; }
            float out[3] = {0, 0, 0};
            for (int db = 0; db < 2; ++db)
                for (int dg = 0; dg < 2; ++dg)
                    for (int dr = 0; dr < 2; ++dr) {
                        const float w = (dr ? f[0] : 1 - f[0]) * (dg ? f[1] : 1 - f[1]) * (db ? f[2] : 1 - f[2]);
                        const float* t = &g_lut[((static_cast<size_t>(i0[2] + db) * kSize + (i0[1] + dg)) * kSize + (i0[0] + dr)) * 3];
                        for (int c = 0; c < 3; ++c) out[c] += w * t[c];
                    }
            auto q = [](float x) { return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, x + 0.5f))); };
            bgra[i] = (p & 0xFF000000u) | (q(out[0]) << 16) | (q(out[1]) << 8) | q(out[2]);
        }
    };
    const unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    const size_t chunk = (bgra.size() + hw - 1) / hw;
    for (unsigned t = 0; t < hw; ++t) pool.emplace_back(work, t * chunk, std::min(bgra.size(), (t + 1) * chunk));
    for (auto& t : pool) t.join();
}

} // namespace cardbridge::exporter
