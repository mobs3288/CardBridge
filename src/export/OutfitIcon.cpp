// cardbridge - a faceless, bald, flat-grey mannequin wearing exactly what the
// exported clothing file carries, as a front-view PNG icon.
// The same convention Koikatsu uses for coordinate files: the game ships a
// mannequin in the outfit, not the character.

#include "export/OutfitIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <stb_image_write.h>

#include "card/CardFile.h"
#include "card/MaterialEdits.h"
#include "export/OutfitTemplate.h"
#include "export/Preview.h"

namespace cardbridge::exporter {

namespace {

// Tiny local copy of the card module's read of Status.showAccessory (showAccessory is the 20
// flags of the accessory list; 0 = the maker wears nothing there). A missing or short array
// hides nothing, same as everywhere else.
std::vector<int8_t> parseShowAccessory(const Value& status) {
    std::vector<int8_t> flags;
    const Value& v = status.at("showAccessory");
    if (v.isBin() && v.asBin().size() >= 20)
        for (size_t i = 0; i < v.asBin().size(); ++i) flags.push_back(v.asBin()[i] ? 1 : 0);
    else if (v.isArray())
        for (const auto& e : v.asArray()) flags.push_back(static_cast<int8_t>(e.asInt(0) ? 1 : 0));
    return flags;
}

// "outfit0.acc03" -> 3; something else -> -1.
int accessorySlotOf(const std::string& id) {
    const size_t at = id.rfind(".acc");
    if (at == std::string::npos) return -1;
    const std::string tail = id.substr(at + 4);
    if (tail.empty()) return -1;
    bool digits = true;
    for (unsigned char c : tail) if (!std::isdigit(c)) { digits = false; break; }
    if (!digits) return -1;
    return std::atoi(tail.c_str());
}

bool isOutfitEmptyPart(const Value& part) {
    if (!part.isMap()) return true;
    const int type = static_cast<int>(part.at("type").asInt(120));
    const int64_t id = part.at("id").asInt(0);
    return type == 120 || id == 0;
}

bool isHairAcc(const card::Card& card, int outfit, int slot) {
    const Value& acc = card.outfits[static_cast<size_t>(outfit)].accessory;
    if (!acc.isMap()) return false;
    const Value& parts = acc.at("parts");
    if (!parts.isArray()) return false;
    if (slot >= static_cast<int>(parts.size())) return false;
    const Value& p = parts.at(static_cast<size_t>(slot));
    if (isOutfitEmptyPart(p)) return false;             // the slot is empty: nothing to hide
    return card::IsHairAccessoryPart(p, card.rows, outfit, slot);
}

} // namespace

std::vector<char> OutfitIconHiddenParts(const assemble::Scene& scene, const card::Card& card, int outfit,
                                        const std::vector<char>& base, bool withHair) {
    std::vector<char> flags(scene.parts.size(), 0);
    const std::vector<int8_t> showAccessory = parseShowAccessory(card.status);
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        const auto& p = scene.parts[pi];
        if (pi < base.size() && base[pi] == 1) flags[pi] = 1;   // the caller's untick: hidden in the view and in the file
        if (p.group == "Hair") { flags[pi] = withHair ? base[pi] : 1; continue; }   // bald by default; the hair is the character's, not the outfit's
        if (p.group == "Accessories") {
            const int slot = accessorySlotOf(p.id);
            if (slot >= 0) {
                if (!withHair && isHairAcc(card, outfit, slot)) { flags[pi] = 1; continue; }   // the file omits these by default
                if (showAccessory.size() > static_cast<size_t>(slot) && showAccessory[static_cast<size_t>(slot)] == 0) { flags[pi] = 1; continue; }   // hidden in the maker's saved state
                if (base.size() > pi && base[pi] == 1) { flags[pi] = 1; continue; }         // the caller's untick
            }
            continue;
        }
        if (p.underPiece) flags[pi] = 2;   // the garment masks cut the skin away; the mannequin shows it whole
        // Body, Head, Outfit keep their own flags from `base`.
    }
    return flags;
}

// ---- compositing helpers (float RGBA 0..1) --------------------------------------------------

struct FloatImage {
    int w = 0, h = 0, spp = 4;
    std::vector<float> px;
    FloatImage() = default;
    FloatImage(int width, int height, int channels) : w(width), h(height), spp(channels) { px.assign((size_t)w * (size_t)h * channels, 0.0f); }
    float& operator()(int x, int y, int c) { return px[((size_t)y * (size_t)w + x) * spp + c]; }
    const float& operator()(int x, int y, int c) const { return px[((size_t)y * (size_t)w + x) * spp + c]; }
    static FloatImage fromBytes(const std::vector<uint8_t>& rgba, int width, int height, int channels) {
        FloatImage out(width, height, channels);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = rgba[i] / 255.0f;
        return out;
    }
};

bool decodePngToFloat(const Bytes& png, FloatImage& out) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!card::DecodeImageBytes(png, w, h, rgba)) return false;
    if (w <= 0 || h <= 0 || rgba.empty()) return false;
    out = FloatImage::fromBytes(rgba, w, h, 4);
    return true;
}

// Area-filter resample (down and up alike). `src` is spp floats per pixel; dst is pre-sized.
void resample(const float* src, int sw, int sh, int spp, FloatImage& dst) {
    const int dw = dst.w, dh = dst.h;
    for (int y = 0; y < dh; ++y) {
        const float y0 = y * (float)sh / (float)dh, y1 = (y + 1) * (float)sh / (float)dh;
        const int syFirst = std::max(0, (int)std::floor(y0));
        const int syLast = std::min(sh - 1, (int)std::ceil(y1) - 1);
        for (int x = 0; x < dw; ++x) {
            const float x0 = x * (float)sw / (float)dw, x1 = (x + 1) * (float)sw / (float)dw;
            const int sxFirst = std::max(0, (int)std::floor(x0));
            const int sxLast = std::min(sw - 1, (int)std::ceil(x1) - 1);
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0;
            for (int sy = syFirst; sy <= syLast; ++sy) {
                const float wy = std::min(y1, sy + 1.0f) - std::max(y0, (float)sy);
                if (wy <= 0) continue;
                for (int sx = sxFirst; sx <= sxLast; ++sx) {
                    const float wx = std::min(x1, sx + 1.0f) - std::max(x0, (float)sx);
                    if (wx <= 0) continue;
                    const float w = wx * wy;
                    const float* s = src + ((size_t)sy * (size_t)sw + sx) * spp;
                    for (int c = 0; c < spp; ++c) acc[c] += s[c] * w;
                    wsum += w;
                }
            }
            float* d = &dst(x, y, 0);
            const float inv = wsum > 0 ? 1.0f / wsum : 0.0f;
            for (int c = 0; c < spp; ++c) d[c] = acc[c] * inv;
        }
    }
}

// One horizontal + one vertical box pass over an image in place (weights 1/(2r+1), clamped edges).
void boxBlurPass(FloatImage& img, int radius) {
    if (radius < 1) return;
    const int w = img.w, h = img.h, spp = img.spp;
    std::vector<float> tmp(img.px.size(), 0.0f);
    const float norm = 1.0f / (2 * radius + 1);
    std::vector<float> pre(static_cast<size_t>(std::max(w, h)) + 1);
    for (int y = 0; y < h; ++y) {
        for (int c = 0; c < spp; ++c) {
            pre[0] = 0;
            for (int x = 0; x < w; ++x) pre[x + 1] = pre[x] + img(x, y, c);
            for (int x = 0; x < w; ++x) {
                const int a = std::max(0, x - radius), b = std::min(w, x + radius + 1);
                tmp[((size_t)y * w + x) * spp + c] = (pre[b] - pre[a]) * norm;
            }
        }
    }
    img.px.swap(tmp);
    for (int x = 0; x < w; ++x) {
        for (int c = 0; c < spp; ++c) {
            pre[0] = 0;
            for (int y = 0; y < h; ++y) pre[y + 1] = pre[y] + img(x, y, c);
            for (int y = 0; y < h; ++y) {
                const int a = std::max(0, y - radius), b = std::min(h, y + radius + 1);
                tmp[((size_t)y * w + x) * spp + c] = (pre[b] - pre[a]) * norm;
            }
        }
    }
    img.px.swap(tmp);
}

FloatImage blurGaussian(const FloatImage& src, int radius) {
    FloatImage out = src;   // copy
    boxBlurPass(out, radius);
    boxBlurPass(out, radius);
    return out;
}

void shiftDown(const FloatImage& src, int dy, FloatImage& dst) {
    dst = FloatImage(src.w, src.h, src.spp);
    for (int y = dy; y < src.h; ++y)
        std::memcpy(&dst(0, y, 0), &src(0, y - dy, 0), (size_t)src.w * src.spp * sizeof(float));
}

Bytes encodePngRgba(const std::vector<uint8_t>& rgba, int w, int h) {
    Bytes png;
    stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), (const uint8_t*)d, (const uint8_t*)d + n); },
                           &png, w, h, 4, rgba.data(), w * 4);
    return png;
}

Bytes RenderOutfitIcon(const assemble::Scene& scene, const OutfitIconOptions& opt) {
    assemble::Scene sc = scene;   // the caller's scene is not touched
    std::vector<char> flags;
    if (opt.hiddenParts) flags = *opt.hiddenParts;
    else flags.assign(sc.parts.size(), 0);

    // Collect the material indices referenced by kept Body/Head parts; they become flat grey.
    std::vector<char> collect(sc.materials.size(), 0);
    for (size_t pi = 0; pi < sc.parts.size(); ++pi) {
        const auto& p = sc.parts[pi];
        if (pi >= flags.size() || flags[pi] == 1) continue;   // not drawn (2 = drawn: under pieces are wanted)
        if (p.group != "Body" && p.group != "Head") continue;
        for (int mi : p.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= sc.meshes.size()) continue;
            for (const auto& sub : sc.meshes[static_cast<size_t>(mi)].submeshes) {
                if (sub.material >= 0 && static_cast<size_t>(sub.material) < collect.size()) collect[static_cast<size_t>(sub.material)] = 1;
            }
        }
    }
    for (size_t mi = 0; mi < sc.materials.size(); ++mi) {
        if (!collect[mi]) continue;
        auto& mat = sc.materials[mi];
        mat.texture = -1;                                  // no texture: flat colour only
        mat.diffuse = {0.72f, 0.71f, 0.72f, 1.0f};         // the mannequin grey
        mat.specular = {0.0f, 0.0f, 0.0f};                 // no sheen; a nurse cap is not glossy grey either, but skin never is on the mannequin
        mat.specularPower = 0.0f;
        mat.shadow = {0.46f, 0.45f, 0.46f};                // a real toon ramp: the mannequin reads as a figure, not a flat cutout
        mat.outlineColor = {0.72f, 0.71f, 0.72f};
        mat.outlineWidth = 0.0f;
        mat.alpha = false;
        mat.translucent = false;
        mat.doubleSided = true;                            // the garment masks can leave holes otherwise
        mat.toon.clear();
    }

    // The embedded stage template: pink tiled backdrop + filigree frame with the floor built in.
    FloatImage tpl;
    {
        int tw = 0, th = 0;
        std::vector<uint8_t> rgba;
        if (DecodeOutfitTemplate(rgba, tw, th) && !rgba.empty()) tpl = FloatImage::fromBytes(rgba, tw, th, 4);
    }

    if (tpl.w == 0) {   // a build problem, not a user problem: the icon still comes out, on the plain background
        if (opt.note) *opt.note = "the embedded icon template failed to decode; the icon was rendered on the plain background";
        PreviewOptions po{};
        po.viewWidth = opt.viewWidth;
        po.viewHeight = opt.viewHeight;
        po.views = 1;
        po.yawDegrees[0] = 0.0f;
        po.yTop = 1.0f;
        po.yBottom = 0.0f;
        po.hiddenParts = &flags;
        Bytes png = RenderPreviewPng(sc, po);
        if (png.empty()) throw FormatError("the clothing icon render failed");
        return png;
    }

    const int W = tpl.w, H = tpl.h;

    // Two renders of the same scene: the picture, and an identical render with everything hidden
    // (nothing drawn anywhere: the pure background). Their difference is the mannequin's coverage.
    PreviewOptions po{};
    po.viewWidth = W;
    po.viewHeight = H;
    po.views = 1;
    po.yawDegrees[0] = 0.0f;
    po.yTop = 1.0f;
    po.yBottom = 0.0f;   // head to feet, all of it
    po.hiddenParts = &flags;
    Bytes fullBytes = RenderPreviewPng(sc, po);
    if (fullBytes.empty()) throw FormatError("the clothing icon render failed");
    std::vector<char> noneFlags(sc.parts.size(), 1);
    PreviewOptions poEmpty = po;
    poEmpty.hiddenParts = &noneFlags;
    Bytes emptyBytes = RenderPreviewPng(sc, poEmpty);
    if (emptyBytes.empty()) throw FormatError("the clothing icon render failed");

    FloatImage full, empty;
    if (!decodePngToFloat(fullBytes, full) || !decodePngToFloat(emptyBytes, empty)) throw FormatError("the clothing icon render failed");

    // The coverage mask: pixels the picture drew, soft-edged by the resample below.
    FloatImage cov(W, H, 1);
    for (size_t i = 0; i < (size_t)W * (size_t)H; ++i) {
        const float d0 = std::fabs(full.px[i * 4] - empty.px[i * 4]);
        const float d1 = std::fabs(full.px[i * 4 + 1] - empty.px[i * 4 + 1]);
        const float d2 = std::fabs(full.px[i * 4 + 2] - empty.px[i * 4 + 2]);
        cov.px[i] = std::max({d0, d1, d2}) > (9.0f / 255.0f) ? 1.0f : 0.0f;
    }

    // The sprite is placed so the mannequin stands on the stage floor: its head-to-feet span is 0.81 of
    // the frame's height and the feet land at 0.895 (the centre of the floor band).
    const float kSpanFrac = 0.81f;
    const float kFeetY = 0.895f;
    const int sh = std::max(1, (int)std::lround(H * kSpanFrac / 0.96f));
    const int sw = std::max(1, (int)std::lround(sh * ((float)W / (float)H)));
    FloatImage sprite(sw, sh, 4);
    resample(full.px.data(), W, H, 4, sprite);
    FloatImage mask(sw, sh, 1);
    resample(cov.px.data(), W, H, 1, mask);

    const int sx = (W - sw) / 2;
    const int sy = std::clamp((int)std::lround(kFeetY * H) - (int)std::lround(0.98f * sh), 0, std::max(0, H - 1));

    // The silhouette pushed down onto the floor and blurred, twice (a tight contact line over a wide soft
    // fall-off), darkening the backdrop: the outfit file carries its own shadow, baked in.
    FloatImage placed(W, H, 1);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const int cx = x + sx, cy = y + sy;
            if (cx >= 0 && cx < W && cy >= 0 && cy < H) placed(cx, cy, 0) = mask(x, y, 0);
        }
    auto bakeShadow = [&](int dy, int radius, float gain) {
        FloatImage off(W, H, 1);
        shiftDown(placed, dy, off);
        FloatImage blurred = blurGaussian(off, radius);
        for (size_t i = 0; i < (size_t)W * (size_t)H; ++i) {
            const float s = std::min(0.72f, std::max(0.0f, blurred.px[i] * gain));
            const float keep = 1.0f - s;
            tpl.px[i * 4] *= keep;
            tpl.px[i * 4 + 1] *= keep;
            tpl.px[i * 4 + 2] *= keep;
        }
    };
    bakeShadow(std::max(2, H / 58), std::max(2, H / 120), 0.42f);   // the contact line
    bakeShadow(std::max(4, H / 30), std::max(4, H / 46), 0.30f);    // the wide fall-off

    // The mannequin itself, over the (already shadowed) backdrop.
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const int cx = x + sx, cy = y + sy;
            if (cx < 0 || cx >= W || cy < 0 || cy >= H) continue;
            const float a = std::min(1.0f, std::max(0.0f, mask(x, y, 0)));
            if (a <= 0.0f) continue;
            const float ia = 1.0f - a;
            float* d = &tpl(cx, cy, 0);
            const float* s = &sprite(x, y, 0);
            for (int c = 0; c < 3; ++c) d[c] = s[c] * a + d[c] * ia;
        }

    std::vector<uint8_t> rgba((size_t)W * (size_t)H * 4);
    for (size_t i = 0; i < (size_t)W * (size_t)H; ++i) {
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, tpl.px[i * 4 + c])) * 255.0f);
        rgba[i * 4 + 3] = 255;
    }
    return encodePngRgba(rgba, W, H);
}

} // namespace cardbridge::exporter
