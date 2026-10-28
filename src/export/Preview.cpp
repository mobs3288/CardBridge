#include "export/Preview.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace cardbridge::exporter {

namespace {

struct Pt { float x, y, z; };

// Box-filtered mip levels, so a 4096 atlas drawn a few hundred pixels tall does not alias into speckle.
struct Mips {
    std::vector<std::vector<uint8_t>> rgba;
    std::vector<int> w, h;
};

Mips BuildMips(const assemble::SceneTexture& t) {
    Mips m;
    m.rgba.push_back(t.rgba); m.w.push_back(t.width); m.h.push_back(t.height);
    while (m.w.back() > 1 && m.h.back() > 1) {
        const auto& src = m.rgba.back();
        int sw = m.w.back(), sh = m.h.back(), dw = sw / 2, dh = sh / 2;
        std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) sum += src[(static_cast<size_t>(2 * y + dy) * sw + 2 * x + dx) * 4 + c];
                    dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
        m.rgba.push_back(std::move(dst)); m.w.push_back(dw); m.h.push_back(dh);
    }
    return m;
}

// Bilinear, wrapping.
void SampleBilinear(const Mips& m, int lvl, float u, float v, float out[4]) {
    int w = m.w[lvl], h = m.h[lvl];
    float fx = u * w - 0.5f, fy = (1.0f - v) * h - 0.5f;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0, ty = fy - y0;
    auto at = [&](int x, int y, int c) { x = ((x % w) + w) % w; y = ((y % h) + h) % h; return m.rgba[lvl][(static_cast<size_t>(y) * w + x) * 4 + c] / 255.0f; };
    for (int c = 0; c < 4; ++c)
        out[c] = (at(x0, y0, c) * (1 - tx) + at(x0 + 1, y0, c) * tx) * (1 - ty) + (at(x0, y0 + 1, c) * (1 - tx) + at(x0 + 1, y0 + 1, c) * tx) * ty;
}

} // namespace

Bytes RenderPreviewPng(const assemble::Scene& scene, const PreviewOptions& opt) {
    // Bounds over everything that will be drawn.
    Pt mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const auto& part : scene.parts) {
        if (!part.includeInExport) continue;
        for (int mi : part.meshes)
            for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                mn = {std::min(mn.x, v.pos.x), std::min(mn.y, v.pos.y), std::min(mn.z, v.pos.z)};
                mx = {std::max(mx.x, v.pos.x), std::max(mx.y, v.pos.y), std::max(mx.z, v.pos.z)};
            }
    }
    int W = opt.viewWidth * opt.views, H = opt.viewHeight;
    std::vector<uint8_t> img(static_cast<size_t>(W) * H * 4, 0);
    for (size_t i = 0; i < img.size(); i += 4) { img[i] = 226; img[i + 1] = 228; img[i + 2] = 232; img[i + 3] = 255; }
    if (mn.x > mx.x) {
        Bytes out;
        stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n); },
                               &out, W, H, 4, img.data(), W * 4);
        return out;
    }
    // One scale for all views: the largest extent of the model, so front and side match.
    float cx = (mn.x + mx.x) * 0.5f, cz = (mn.z + mx.z) * 0.5f;
    float radius = std::max({mx.x - mn.x, mx.z - mn.z}) * 0.5f;
    float spanY = mx.y - mn.y;
    float y0 = mn.y + opt.yBottom * spanY, y1 = mn.y + opt.yTop * spanY;
    bool closeUp = (opt.yTop - opt.yBottom) < 0.9f;
    if (closeUp) {   // centre a close-up on the geometry inside its window, not on the whole model
        Pt wn{1e30f, 1e30f, 1e30f}, wx{-1e30f, -1e30f, -1e30f};
        for (const auto& part : scene.parts) {
            if (!part.includeInExport) continue;
            for (int mi : part.meshes)
                for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                    if (v.pos.y < y0 || v.pos.y > y1) continue;
                    wn = {std::min(wn.x, v.pos.x), 0, std::min(wn.z, v.pos.z)};
                    wx = {std::max(wx.x, v.pos.x), 0, std::max(wx.z, v.pos.z)};
                }
        }
        if (wn.x <= wx.x) { cx = (wn.x + wx.x) * 0.5f; cz = (wn.z + wx.z) * 0.5f; }
    }
    float scale = closeUp ? static_cast<float>(opt.viewHeight) / (y1 - y0)
                          : std::min(static_cast<float>(opt.viewHeight) * 0.96f / spanY, static_cast<float>(opt.viewWidth) * 0.96f / (2.0f * radius));

    std::vector<Mips> mips(scene.textures.size());
    std::vector<bool> mipsBuilt(scene.textures.size(), false);
    for (int view = 0; view < opt.views; ++view) {
        float yaw = opt.yawDegrees[view] * 3.14159265f / 180.0f;
        float cs = std::cos(yaw), sn = std::sin(yaw);
        int ox = view * opt.viewWidth;
        std::vector<float> zbuf(static_cast<size_t>(opt.viewWidth) * opt.viewHeight, -1e30f);

        auto project = [&](const assemble::SceneVertex& v, float& sx, float& sy, float& sz, float& nz) {
            float x = v.pos.x - cx, z = v.pos.z - cz;
            // Camera on the +Z side looking back at the model (the model faces +Z): screen x is -x.
            float rx = x * cs + z * sn, rz = -x * sn + z * cs;
            sx = opt.viewWidth * 0.5f - rx * scale;
            sy = closeUp ? opt.viewHeight - (v.pos.y - y0) * scale : opt.viewHeight - 0.02f * opt.viewHeight - (v.pos.y - mn.y) * scale;
            sz = rz;
            float nx = v.normal.x * cs + v.normal.z * sn, nzz = -v.normal.x * sn + v.normal.z * cs;
            (void)nx;
            nz = nzz;
        };

        for (const auto& part : scene.parts) {
            if (!part.includeInExport) continue;
            for (int mi : part.meshes) {
                const auto& mesh = scene.meshes[static_cast<size_t>(mi)];
                std::vector<Pt> P(mesh.vertices.size());
                std::vector<float> NZ(mesh.vertices.size());
                for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                    float sx, sy, sz, nz;
                    project(mesh.vertices[i], sx, sy, sz, nz);
                    P[i] = {sx, sy, sz};
                    NZ[i] = nz;
                }
                for (const auto& sub : mesh.submeshes) {
                    const auto& mat = scene.materials[static_cast<size_t>(sub.material)];
                    const assemble::SceneTexture* tex = mat.texture >= 0 ? &scene.textures[static_cast<size_t>(mat.texture)] : nullptr;
                    for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                        uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                        const Pt &a = P[i0], &b = P[i1], &c = P[i2];
                        float minx = std::floor(std::min({a.x, b.x, c.x})), maxx = std::ceil(std::max({a.x, b.x, c.x}));
                        float miny = std::floor(std::min({a.y, b.y, c.y})), maxy = std::ceil(std::max({a.y, b.y, c.y}));
                        int x0 = std::max(0, static_cast<int>(minx)), x1 = std::min(opt.viewWidth - 1, static_cast<int>(maxx));
                        int y0 = std::max(0, static_cast<int>(miny)), y1 = std::min(opt.viewHeight - 1, static_cast<int>(maxy));
                        float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                        if (std::fabs(den) < 1e-8f) continue;
                        int lod = 0;
                        const Mips* mp = nullptr;
                        if (tex) {
                            size_t ti = static_cast<size_t>(mat.texture);
                            if (!mipsBuilt[ti]) { mips[ti] = BuildMips(*tex); mipsBuilt[ti] = true; }
                            mp = &mips[ti];
                            const auto &t0 = mesh.vertices[i0].uv, &t1 = mesh.vertices[i1].uv, &t2 = mesh.vertices[i2].uv;
                            float uvArea = std::fabs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y)) * 0.5f * tex->width * tex->height;
                            float scrArea = std::fabs(den) * 0.5f;
                            if (scrArea > 1e-6f && uvArea > scrArea)
                                lod = std::min(static_cast<int>(mp->w.size()) - 1, static_cast<int>(std::floor(0.5f * std::log2(uvArea / scrArea) + 0.5f)));
                        }
                        for (int y = y0; y <= y1; ++y)
                            for (int x = x0; x <= x1; ++x) {
                                float px = x + 0.5f, py = y + 0.5f;
                                float l0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / den;
                                float l1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / den;
                                float l2 = 1.0f - l0 - l1;
                                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                                float z = l0 * a.z + l1 * b.z + l2 * c.z;
                                size_t zi = static_cast<size_t>(y) * opt.viewWidth + x;
                                if (z <= zbuf[zi]) continue;
                                float r = mat.diffuse.x, g = mat.diffuse.y, bl = mat.diffuse.z, al = 1.0f;
                                if (tex) {
                                    const auto& u0 = mesh.vertices[i0].uv; const auto& u1 = mesh.vertices[i1].uv; const auto& u2 = mesh.vertices[i2].uv;
                                    float u = l0 * u0.x + l1 * u1.x + l2 * u2.x, v = l0 * u0.y + l1 * u1.y + l2 * u2.y;
                                    u -= std::floor(u); v -= std::floor(v);
                                    float tc[4];
                                    SampleBilinear(*mp, lod, u, v, tc);
                                    r *= tc[0]; g *= tc[1]; bl *= tc[2]; al = tc[3];
                                }
                                if (al < 0.5f) continue;
                                float nz = std::fabs(l0 * NZ[i0] + l1 * NZ[i1] + l2 * NZ[i2]);
                                float shade = 0.55f + 0.45f * std::min(1.0f, nz);
                                zbuf[zi] = z;
                                uint8_t* o = &img[(static_cast<size_t>(y) * W + ox + x) * 4];
                                o[0] = static_cast<uint8_t>(std::min(255.0f, r * shade * 255.0f));
                                o[1] = static_cast<uint8_t>(std::min(255.0f, g * shade * 255.0f));
                                o[2] = static_cast<uint8_t>(std::min(255.0f, bl * shade * 255.0f));
                            }
                    }
                }
            }
        }
    }
    Bytes out;
    stbi_write_png_to_func([](void* c, void* d, int n) { auto* b = static_cast<Bytes*>(c); b->insert(b->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n); },
                           &out, W, H, 4, img.data(), W * 4);
    return out;
}

} // namespace cardbridge::exporter
