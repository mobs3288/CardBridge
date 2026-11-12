#include "unity/TextureDecode.h"

#include "core/Bytes.h"

#include <algorithm>

#include <bc7decomp.h>   // third_party/bc7decomp (Richard Geldreich, MIT or public domain)

namespace cardbridge::unity {

namespace {

struct Rgba { uint8_t r, g, b, a; };

Rgba from565(uint16_t c) {
    uint8_t r = static_cast<uint8_t>((c >> 11) & 31), g = static_cast<uint8_t>((c >> 5) & 63), b = static_cast<uint8_t>(c & 31);
    return {static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)),
            static_cast<uint8_t>((b << 3) | (b >> 2)), 255};
}

// Colour half of a DXT1/DXT5 block: two 565 endpoints and 2-bit indices.
void decodeColorBlock(const uint8_t* b, Rgba out[16], bool dxt1) {
    uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
    Rgba p[4];
    p[0] = from565(c0);
    p[1] = from565(c1);
    static constexpr uint8_t Rgba::* channels[3] = {&Rgba::r, &Rgba::g, &Rgba::b};
    if (!dxt1 || c0 > c1) {
        for (auto ch : channels) {
            p[2].*ch = static_cast<uint8_t>((2 * p[0].*ch + p[1].*ch) / 3);
            p[3].*ch = static_cast<uint8_t>((p[0].*ch + 2 * p[1].*ch) / 3);
        }
        p[2].a = p[3].a = 255;
    } else {
        for (auto ch : channels) {
            p[2].*ch = static_cast<uint8_t>((p[0].*ch + p[1].*ch) / 2);
            p[3].*ch = 0;
        }
        p[2].a = 255;
        p[3].a = 0;   // DXT1 one-bit alpha: transparent black
    }
    uint32_t idx = static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
                   (static_cast<uint32_t>(b[6]) << 16) | (static_cast<uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) out[i] = p[(idx >> (2 * i)) & 3];
}

void decodeAlphaBlock(const uint8_t* b, uint8_t out[16]) {
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i) a[1 + i] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
    } else {
        for (int i = 1; i < 5; ++i) a[1 + i] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = a[(bits >> (3 * i)) & 7];
}

std::vector<uint8_t> decodeDxt(int width, int height, const uint8_t* data, size_t size, bool dxt5) {
    int bw = (width + 3) / 4, bh = (height + 3) / 4;
    size_t blockBytes = dxt5 ? 16 : 8;
    if (size < static_cast<size_t>(bw) * bh * blockBytes) throw FormatError("texture data shorter than its size implies");
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (static_cast<size_t>(by) * bw + bx) * blockBytes;
            Rgba px[16];
            uint8_t al[16];
            if (dxt5) {
                decodeAlphaBlock(b, al);
                decodeColorBlock(b + 8, px, false);
            } else {
                decodeColorBlock(b, px, true);
            }
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    int X = bx * 4 + x, Y = by * 4 + y;
                    if (X >= width || Y >= height) continue;
                    Rgba c = px[y * 4 + x];
                    if (dxt5) c.a = al[y * 4 + x];
                    uint8_t* o = &out[(static_cast<size_t>(Y) * width + X) * 4];
                    o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
                }
        }
    return out;
}

std::vector<uint8_t> decodeBc7(int width, int height, const uint8_t* data, size_t size) {
    int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (size < static_cast<size_t>(bw) * bh * 16) throw FormatError("texture data shorter than its size implies");
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            bc7decomp::color_rgba px[16];
            if (!bc7decomp::unpack_bc7(data + (static_cast<size_t>(by) * bw + bx) * 16, px)) throw FormatError("invalid BC7 block");
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    int X = bx * 4 + x, Y = by * 4 + y;
                    if (X >= width || Y >= height) continue;
                    uint8_t* o = &out[(static_cast<size_t>(Y) * width + X) * 4];
                    o[0] = px[y * 4 + x].r; o[1] = px[y * 4 + x].g; o[2] = px[y * 4 + x].b; o[3] = px[y * 4 + x].a;
                }
        }
    return out;
}

} // namespace

std::vector<uint8_t> DecodeTexture(int fmt, int width, int height, const uint8_t* data, size_t size) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) throw FormatError("bad texture size");
    size_t px = static_cast<size_t>(width) * height;
    std::vector<uint8_t> out;
    auto need = [&](size_t n) { if (size < n) throw FormatError("texture data shorter than its size implies"); };
    switch (fmt) {
    case 1:   // Alpha8
        need(px);
        out.resize(px * 4);
        for (size_t i = 0; i < px; ++i) { out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 255; out[i * 4 + 3] = data[i]; }
        return out;
    case 3:   // RGB24
        need(px * 3);
        out.resize(px * 4);
        for (size_t i = 0; i < px; ++i) {
            out[i * 4] = data[i * 3]; out[i * 4 + 1] = data[i * 3 + 1]; out[i * 4 + 2] = data[i * 3 + 2]; out[i * 4 + 3] = 255;
        }
        return out;
    case 4:   // RGBA32
        need(px * 4);
        return std::vector<uint8_t>(data, data + px * 4);
    case 5:   // ARGB32
        need(px * 4);
        out.resize(px * 4);
        for (size_t i = 0; i < px; ++i) {
            out[i * 4] = data[i * 4 + 1]; out[i * 4 + 1] = data[i * 4 + 2]; out[i * 4 + 2] = data[i * 4 + 3]; out[i * 4 + 3] = data[i * 4];
        }
        return out;
    case 14:  // BGRA32
        need(px * 4);
        out.resize(px * 4);
        for (size_t i = 0; i < px; ++i) {
            out[i * 4] = data[i * 4 + 2]; out[i * 4 + 1] = data[i * 4 + 1]; out[i * 4 + 2] = data[i * 4]; out[i * 4 + 3] = data[i * 4 + 3];
        }
        return out;
    case 10: return decodeDxt(width, height, data, size, false);
    case 12: return decodeDxt(width, height, data, size, true);
    case 25: return decodeBc7(width, height, data, size);
    default:
        throw FormatError("unsupported Unity texture format " + std::to_string(fmt));
    }
}

FillSummary AnalyzeFill(const uint8_t* rgba, int width, int height) {
    FillSummary fs;
    if (!rgba || width <= 0 || height <= 0) return fs;
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    double sr = 0.0, sg = 0.0, sb = 0.0, smax = 0.0, sch = 0.0;
    size_t opaque = 0, achromatic = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] < 10) continue;
        ++opaque;
        const int r = p[0], g = p[1], b = p[2];
        const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
        sr += r; sg += g; sb += b; smax += hi; sch += hi - lo;
        if (hi - lo <= 30) ++achromatic;
    }
    fs.opaqueFraction = static_cast<float>(opaque) / static_cast<float>(n);
    if (opaque == 0) { fs.avg = Vec3{0, 0, 0}; return fs; }
    fs.avg = Vec3{static_cast<float>(sr / opaque / 255.0), static_cast<float>(sg / opaque / 255.0), static_cast<float>(sb / opaque / 255.0)};
    fs.value = static_cast<float>(smax / opaque / 255.0);
    fs.achromatic = static_cast<float>(achromatic) / static_cast<float>(opaque);
    fs.chromaMean = static_cast<float>(sch / opaque);
    return fs;
}

std::string FormatName(int unityFormat) {
    switch (unityFormat) {
    case 1: return "Alpha8";
    case 3: return "RGB24";
    case 4: return "RGBA32";
    case 5: return "ARGB32";
    case 10: return "DXT1";
    case 12: return "DXT5";
    case 14: return "BGRA32";
    case 25: return "BC7";
    default: return "UnityFormat " + std::to_string(unityFormat);
    }
}

} // namespace cardbridge::unity
