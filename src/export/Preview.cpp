#include "export/Preview.h"

#include "assemble/Pose.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <thread>
#include <map>
#include <memory>

#include "export/ColourGrade.h"
#include "shader/Sm4.h"
#include "shader/UnityShader.h"
#include <vector>

namespace cardbridge::exporter {

namespace detail {   // shared with GpuView.cpp
// The game runs in linear light: sRGB textures are decoded when sampled, colour properties reach the shader as linear values and
// the frame is encoded when it is written. CARDBRIDGE_GAMMA=1 combines everything as stored instead (the old look, for comparing).
bool LinearLight() { static const bool on = !std::getenv("CARDBRIDGE_GAMMA"); return on; }

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float ToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

// Colour-typed material properties (Unity converts these, and only these, to linear): _Color.., _ShadowColor, _LineColor, _overcolorN, ...
static bool IsColourProp(const std::string& n) {
    if (n.size() > 3 && n.compare(n.size() - 3, 3, "_ST") == 0) return false;
    return n.rfind("_Color", 0) == 0 || n.rfind("_overcolor", 0) == 0 || n == "_ShadowColor" || n == "_LineColor" || n == "_SpecularColor" ||
           n == "_EmissionColor" || n == "_ambientshadow";
}

// The material as the shader sees it in linear light.
assemble::SceneMaterial LinearColours(const assemble::SceneMaterial& mat) {
    assemble::SceneMaterial m = mat;
    for (auto& pr : m.props)
        if (IsColourProp(pr.first)) { pr.second.x = ToLinear(pr.second.x); pr.second.y = ToLinear(pr.second.y); pr.second.z = ToLinear(pr.second.z); }
    return m;
}
} // namespace detail

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
void SampleBilinear(const Mips& m, int lvl, float u, float v, float out[4], bool clamp = false) {
    int w = m.w[lvl], h = m.h[lvl];
    float fx = u * w - 0.5f, fy = (1.0f - v) * h - 0.5f;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    float tx = fx - x0, ty = fy - y0;
    auto at = [&](int x, int y, int c) { if (clamp) { x = std::min(w - 1, std::max(0, x)); y = std::min(h - 1, std::max(0, y)); } else { x = ((x % w) + w) % w; y = ((y % h) + h) % h; } return m.rgba[lvl][(static_cast<size_t>(y) * w + x) * 4 + c] / 255.0f; };
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
                                // Two-tone toon: lit side full colour, shade side tinted by the material's shadow colour.
                                float lit = std::min(1.0f, std::max(0.0f, (nz - 0.35f) / 0.2f));
                                float shade = 1.0f;
                                r *= (mat.shadow.x + (1.0f - mat.shadow.x) * lit);
                                g *= (mat.shadow.y + (1.0f - mat.shadow.y) * lit);
                                bl *= (mat.shadow.z + (1.0f - mat.shadow.z) * lit);
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


// A scene shader ready to run: its forward-pass programs decoded.
struct ShaderRuntime {
    bool ok = false;
    std::vector<shader::UnityProgram> programs;
    size_t pi = 0, vi = 0;
    shader::Sm4Program ps, vs;
    // vertex output register -> pixel input register, per pixel input element
    struct Link { uint32_t vsReg, psReg, mask; };
    std::vector<Link> links;
};

struct ViewState {
    std::vector<Mips> mips;
    std::vector<bool> built;
    std::vector<std::unique_ptr<ShaderRuntime>> shaders;   // per Scene::shaders
};

namespace {

const ShaderRuntime* GetShader(const assemble::Scene& scene, ViewState& st, int index) {
    if (index < 0 || static_cast<size_t>(index) >= scene.shaders.size()) return nullptr;
    if (st.shaders.size() != scene.shaders.size()) st.shaders.resize(scene.shaders.size());
    auto& slot = st.shaders[static_cast<size_t>(index)];
    if (!slot) {
        slot = std::make_unique<ShaderRuntime>();
        auto& sr = *slot;
        Bytes blob(scene.shaders[static_cast<size_t>(index)].blob.begin(), scene.shaders[static_cast<size_t>(index)].blob.end());
        sr.programs = shader::ParseUnityShaderBlob(blob, nullptr);
        if (shader::PickForwardPass(sr.programs, sr.pi, sr.vi) && shader::DecodeSm4(sr.programs[sr.pi].code.tokens, sr.ps, nullptr) &&
            shader::DecodeSm4(sr.programs[sr.vi].code.tokens, sr.vs, nullptr)) {
            for (const auto& in : sr.programs[sr.pi].code.inputs)
                for (const auto& o : sr.programs[sr.vi].code.outputs)
                    if (in.semantic == o.semantic && in.index == o.index && in.semantic != "SV_IsFrontFace")
                        sr.links.push_back({o.reg, in.reg, in.mask & o.mask});
            sr.ok = true;
        }
    }
    return slot->ok ? slot.get() : nullptr;
}

// Fills a constant buffer from the material (colours, floats, texture scale/offset) and the fixed scene lighting.
struct CbSet {
    std::vector<std::vector<float>> data;   // by slot
    std::vector<const float*> ptr;
};
void FillConstants(const shader::UnityProgram& prog, const assemble::SceneMaterial& mat, const float lightDir[3], const float camPos[3], CbSet& out,
                   const std::vector<std::pair<std::string, Vec4>>* globals = nullptr) {
    out.data.assign(8, {});
    for (const auto& cb : prog.cbuffers) {
        if (cb.slot < 0 || cb.slot >= 8) continue;
        auto& d = out.data[static_cast<size_t>(cb.slot)];
        d.assign(cb.size / 4 + 16, 0.0f);
        for (const auto& pm : cb.params) {
            float* f = &d[pm.offset / 4];
            if (pm.isMatrix) {   // unity_ObjectToWorld / WorldToObject / MatrixVP: identity (the scene is already in world space)
                for (uint32_t i = 0; i < 4; ++i) f[i * 4 + i] = 1.0f;
                continue;
            }
            if (pm.name == "_LightColor0") {
                static const float lightColour = std::getenv("CARDBRIDGE_LIGHT") ? static_cast<float>(std::atof(std::getenv("CARDBRIDGE_LIGHT"))) : 1.0f;
                f[0] = f[1] = f[2] = f[3] = lightColour;
                continue;
            }
            if (pm.name == "_WorldSpaceLightPos0") { f[0] = lightDir[0]; f[1] = lightDir[1]; f[2] = lightDir[2]; f[3] = 0.0f; continue; }
            if (pm.name == "_WorldSpaceCameraPos") { f[0] = camPos[0]; f[1] = camPos[1]; f[2] = camPos[2]; continue; }
            bool found = false;
            for (const auto& pr : mat.props) if (pr.first == pm.name) { f[0] = pr.second.x; f[1] = pr.second.y; f[2] = pr.second.z; f[3] = pr.second.w; found = true; }
            if (!found && globals)
                for (const auto& pr : *globals) if (pr.first == pm.name) { f[0] = pr.second.x; f[1] = pr.second.y; f[2] = pr.second.z; f[3] = pr.second.w; found = true; }
            if (found) continue;
            if (std::getenv("CARDBRIDGE_DEBUG_CB") && !(pm.name.size() > 3 && pm.name.compare(pm.name.size() - 3, 3, "_ST") == 0))
                std::fprintf(stderr, "cb-missing %s: %s\n", mat.name.c_str(), pm.name.c_str());
            if (pm.name.size() > 3 && pm.name.compare(pm.name.size() - 3, 3, "_ST") == 0) {
                std::string tex = pm.name.substr(0, pm.name.size() - 3);
                f[0] = 1; f[1] = 1;
                for (const auto& tb : mat.texProps) if (tb.name == tex) { f[0] = tb.st.x; f[1] = tb.st.y; f[2] = tb.st.z; f[3] = tb.st.w; }
            }
        }
    }
    if (std::getenv("CARDBRIDGE_DEBUG_CBV") && mat.name == std::getenv("CARDBRIDGE_DEBUG_CBV") && prog.isPixel())
        for (const auto& cb : prog.cbuffers) {
            if (cb.slot < 0 || cb.slot >= 8) continue;
            for (const auto& pm : cb.params) { const float* f = &out.data[static_cast<size_t>(cb.slot)][pm.offset / 4]; std::fprintf(stderr, "cbv cb%d[%u] %s = %.3f %.3f %.3f %.3f\n", cb.slot, pm.offset / 16, pm.name.c_str(), f[0], f[1], f[2], f[3]); }
        }
    out.ptr.assign(8, nullptr);
    for (size_t i = 0; i < 8; ++i) if (!out.data[i].empty()) out.ptr[i] = out.data[i].data();
}

struct MatTextures : shader::ISm4Textures {
    const assemble::Scene* scene = nullptr;
    ViewState* st = nullptr;
    const shader::UnityProgram* prog = nullptr;
    const assemble::SceneMaterial* mat = nullptr;
    float lod = 0;
    bool trace = false;
    bool decodeSrgb = false;   // sRGB-flagged textures give linear values (a create job run in linear light)
    void Sample(int t, int, const float coord[4], float lodIn, float out[4]) override {
        SampleImpl(t, coord, lodIn, out);
        if (trace) {
            const std::string* nm = nullptr;
            for (const auto& b : prog->textures) if (static_cast<int>(b.reg) == t) nm = &b.name;
            std::fprintf(stderr, "  sample t%d %s uv(%.3f %.3f) -> %.3f %.3f %.3f %.3f\n", t, nm ? nm->c_str() : "?", coord[0], coord[1], out[0], out[1], out[2], out[3]);
        }
    }
    void SampleImpl(int t, const float coord[4], float lodIn, float out[4]) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        const std::string* name = nullptr;
        for (const auto& b : prog->textures) if (static_cast<int>(b.reg) == t) name = &b.name;
        if (!name) return;
        static const char* const drop = std::getenv("CARDBRIDGE_DROP_TEX");
        if (drop && *name == drop) return;   // debug: treat this texture as missing (white)
        for (const auto& tb : mat->texProps)
            if (tb.name == *name && tb.texture >= 0) {
                size_t ti = static_cast<size_t>(tb.texture);
                if (st->built.size() != scene->textures.size()) { st->mips.assign(scene->textures.size(), Mips{}); st->built.assign(scene->textures.size(), false); }
                if (!st->built[ti]) { st->mips[ti] = BuildMips(scene->textures[ti]); st->built[ti] = true; }
                const Mips& m = st->mips[ti];
                int l = std::min(static_cast<int>(m.w.size()) - 1, std::max(0, static_cast<int>(lodIn >= 0 ? lodIn : lod)));
                bool cl = scene->textures[ti].clamp;
                float u = cl ? coord[0] : coord[0] - std::floor(coord[0]), v = cl ? coord[1] : coord[1] - std::floor(coord[1]);
                SampleBilinear(m, l, u, v, out, cl);
                if (decodeSrgb && scene->textures[ti].srgb)
                    for (int c = 0; c < 3; ++c) out[c] = out[c] <= 0.04045f ? out[c] / 12.92f : std::pow((out[c] + 0.055f) / 1.055f, 2.4f);
                return;
            }
        for (const auto& tb : scene->globalTextures)
            if (tb.name == *name && tb.texture >= 0) {
                size_t ti = static_cast<size_t>(tb.texture);
                if (st->built.size() != scene->textures.size()) { st->mips.assign(scene->textures.size(), Mips{}); st->built.assign(scene->textures.size(), false); }
                if (!st->built[ti]) { st->mips[ti] = BuildMips(scene->textures[ti]); st->built[ti] = true; }
                bool cl = scene->textures[ti].clamp;
                SampleBilinear(st->mips[ti], 0, cl ? coord[0] : coord[0] - std::floor(coord[0]), cl ? coord[1] : coord[1] - std::floor(coord[1]), out, cl);
                return;
            }
        // A texture the material leaves empty: the shader's declared default (Unity's built-in white / black / gray / bump).
        std::string def = "white";
        if (mat->shaderIndex >= 0 && static_cast<size_t>(mat->shaderIndex) < scene->shaders.size())
            for (const auto& d : scene->shaders[static_cast<size_t>(mat->shaderIndex)].texDefaults) if (d.first == *name) def = d.second;
        if (def == "black") { out[0] = out[1] = out[2] = out[3] = 0.0f; }
        else if (def == "gray" || def == "grey") { out[0] = out[1] = out[2] = out[3] = 0.5f; }
        else if (def == "bump") { out[0] = 0.5f; out[1] = 0.5f; out[2] = 1.0f; out[3] = 0.5f; }   // flat normal in Unity's packed form (x in alpha)
    }
};

} // namespace

void ViewStateDeleter::operator()(ViewState* v) const { delete v; }
ViewStatePtr NewViewState() { return ViewStatePtr(new ViewState()); }

namespace {

struct PsIn { float r[8][4]; };

// Per-vertex tangents from the UV layout (the shaders build their normal-map frame from them).
std::vector<Vec3> ComputeTangents(const assemble::SceneMesh& m) {
    std::vector<Vec3> tan(m.vertices.size(), Vec3{0, 0, 0});
    for (const auto& sub : m.submeshes)
        for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
            const auto &v0 = m.vertices[sub.indices[t]], &v1 = m.vertices[sub.indices[t + 1]], &v2 = m.vertices[sub.indices[t + 2]];
            float e1x = v1.pos.x - v0.pos.x, e1y = v1.pos.y - v0.pos.y, e1z = v1.pos.z - v0.pos.z;
            float e2x = v2.pos.x - v0.pos.x, e2y = v2.pos.y - v0.pos.y, e2z = v2.pos.z - v0.pos.z;
            float du1 = v1.uv.x - v0.uv.x, dv1 = v1.uv.y - v0.uv.y, du2 = v2.uv.x - v0.uv.x, dv2 = v2.uv.y - v0.uv.y;
            float det = du1 * dv2 - du2 * dv1;
            if (std::fabs(det) < 1e-12f) continue;
            float k = 1.0f / det;
            Vec3 tg{(e1x * dv2 - e2x * dv1) * k, (e1y * dv2 - e2y * dv1) * k, (e1z * dv2 - e2z * dv1) * k};
            for (int c = 0; c < 3; ++c) { auto& d = tan[sub.indices[t + static_cast<size_t>(c)]]; d = {d.x + tg.x, d.y + tg.y, d.z + tg.z}; }
        }
    for (size_t i = 0; i < tan.size(); ++i) {
        const Vec3& n = m.vertices[i].normal;
        Vec3 t = tan[i];
        float d = t.x * n.x + t.y * n.y + t.z * n.z;
        t = {t.x - n.x * d, t.y - n.y * d, t.z - n.z * d};
        float len = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
        tan[i] = len > 1e-8f ? Vec3{t.x / len, t.y / len, t.z / len} : Vec3{1, 0, 0};
    }
    return tan;
}

// Runs the vertex program for every vertex of a mesh and keeps what the pixel program receives.
bool RunVertexStage(const ShaderRuntime& sr, const assemble::SceneMaterial& mat, const assemble::SceneMesh& mesh, const std::vector<Vec3>& tangents,
                    const std::vector<std::pair<std::string, Vec4>>& globalProps,
                    const float lightDir[3], const float camPos[3], std::vector<PsIn>& out) {
    const auto& vp = sr.programs[sr.vi];
    CbSet cbs;
    FillConstants(vp, mat, lightDir, camPos, cbs, &globalProps);
    out.assign(mesh.vertices.size(), PsIn{});
    shader::Sm4Io io;
    io.constantBuffers = cbs.ptr;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto& v = mesh.vertices[i];
        for (const auto& in : vp.code.inputs) {
            if (in.reg >= 32) continue;
            float* r = io.input[in.reg];
            if (in.semantic == "POSITION") { r[0] = v.pos.x; r[1] = v.pos.y; r[2] = v.pos.z; r[3] = 1; }
            else if (in.semantic == "NORMAL") { r[0] = v.normal.x; r[1] = v.normal.y; r[2] = v.normal.z; r[3] = 0; }
            else if (in.semantic == "TANGENT") { r[0] = tangents[i].x; r[1] = tangents[i].y; r[2] = tangents[i].z; r[3] = 1; }
            else if (in.semantic == "TEXCOORD") { const Vec2& t = in.index == 1 ? v.uv1 : in.index == 2 ? v.uv2 : v.uv; r[0] = t.x; r[1] = t.y; r[2] = 0; r[3] = 0; }
            else if (in.semantic == "COLOR") { r[0] = r[1] = r[2] = r[3] = 1; }
        }
        io.discarded = false;
        std::string why;
        if (!shader::RunSm4(sr.vs, io, &why)) return false;
        for (const auto& l : sr.links)
            if (l.vsReg < 32 && l.psReg < 8) for (int c = 0; c < 4; ++c) if (l.mask & (1u << c)) out[i].r[l.psReg][c] = io.output[l.vsReg][c];   // registers are packed: TEXCOORD0 is xy, TEXCOORD1 zw
    }
    return true;
}

} // namespace

void RenderView(const assemble::Scene& scene, const ViewCamera& cam, int W, int H, const ViewOptions& opt, ViewState& st,
                std::vector<uint32_t>& bgra) {
    bgra.assign(static_cast<size_t>(W) * H, 0xFFE2E4E8u);
    if (st.mips.size() != scene.textures.size()) { st.mips.assign(scene.textures.size(), Mips{}); st.built.assign(scene.textures.size(), false); }
    // Bounds of everything drawable, once per frame (cheap next to the raster).
    Pt mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    auto visible = [&](size_t pi) {
        const auto& part = scene.parts[pi];
        const char flag = opt.hiddenParts && pi < opt.hiddenParts->size() ? (*opt.hiddenParts)[pi] : 0;
        if (!part.includeInExport) return false;
        if (flag == 2) return true;   // forced on, even for a part that is hidden by default (a nude under piece)
        return part.visibleInPreview && !flag;
    };
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        if (!visible(pi)) continue;
        for (int mi : scene.parts[pi].meshes)
            for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                mn = {std::min(mn.x, v.pos.x), std::min(mn.y, v.pos.y), std::min(mn.z, v.pos.z)};
                mx = {std::max(mx.x, v.pos.x), std::max(mx.y, v.pos.y), std::max(mx.z, v.pos.z)};
            }
    }
    if (mn.x > mx.x) return;
    const float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f, cz = (mn.z + mx.z) * 0.5f;
    const float extent = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z});
    const float scale = H * 0.9f / extent * cam.zoom;
    const float yaw = cam.yawDeg * 3.14159265f / 180.0f, pit = cam.pitchDeg * 3.14159265f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    std::vector<float> zbuf(static_cast<size_t>(W) * H, -1e30f);
    // Scene lighting for the game's shaders: a directional light just above the camera axis (the camera looks at the model
    // from toCamera; the scene is in metres, the orthographic camera is placed a few metres away).
    const float toCam[3] = {-sn * cp, sp, cs * cp};
    float L[3] = {toCam[0], toCam[1], toCam[2]};   // the character maker's light shines straight along the view (its Directional Light faces -Z)
    { float n = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); for (float& c : L) c /= n; }
    const float camPos[3] = {cx + toCam[0] * 5.0f, cy + toCam[1] * 5.0f, cz + toCam[2] * 5.0f};

    auto project = [&](const assemble::SceneVertex& v, Pt& p, float& nz) {
        float x = v.pos.x - cx, y = v.pos.y - cy, z = v.pos.z - cz;
        float rx = x * cs + z * sn, rz = -x * sn + z * cs;
        float ry = y * cp - rz * sp, rz2 = y * sp + rz * cp;
        p = {W * 0.5f - rx * scale + cam.panX * H, H * 0.5f - ry * scale + cam.panY * H, rz2};
        float nzz = -v.normal.x * sn + v.normal.z * cs;
        nz = v.normal.y * sp + nzz * cp;
    };

    const int step = std::max(1, opt.step);
    auto highlightTint = [](float& r, float& g, float& b) {   // the same warm mix GpuView uses for its hover tint
        r = r * 0.62f + 0.98f * 0.38f;
        g = g * 0.62f + 0.52f * 0.38f;
        b = b * 0.62f + 0.10f * 0.38f;
    };
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        if (!visible(pi)) continue;
        const bool highlight = static_cast<int>(pi) == opt.highlightPart;
        for (int mi : scene.parts[pi].meshes) {
            const auto& restMesh = scene.meshes[static_cast<size_t>(mi)];
            assemble::SceneMesh posedMesh;   // the vertices moved by the pose, when there is one (submeshes and shapes are not needed to draw)
            const bool posed = opt.skin && opt.skin->size() >= scene.bones.size() && !opt.skin->empty();
            if (posed) {
                posedMesh.name = restMesh.name;
                posedMesh.vertices = restMesh.vertices;
                posedMesh.submeshes = restMesh.submeshes;
                for (auto& v : posedMesh.vertices) {
                    v.pos = assemble::SkinPoint(v, *opt.skin);
                    v.normal = normalize(assemble::SkinDirection(v, *opt.skin));
                }
            }
            const assemble::SceneMesh& mesh = posed ? posedMesh : restMesh;
            std::vector<Pt> P(mesh.vertices.size());
            std::vector<float> NZ(mesh.vertices.size());
            for (size_t i = 0; i < mesh.vertices.size(); ++i) project(mesh.vertices[i], P[i], NZ[i]);
            std::vector<Vec3> tangents;
            for (const auto& sub : mesh.submeshes) {
                const auto& mat = scene.materials[static_cast<size_t>(sub.material)];
                const assemble::SceneTexture* tex = (opt.textured && mat.texture >= 0) ? &scene.textures[static_cast<size_t>(mat.texture)] : nullptr;
                const ShaderRuntime* sr = opt.shaded ? GetShader(scene, st, mat.shaderIndex) : nullptr;
                std::vector<PsIn> vsOut;
                CbSet psCb;
                MatTextures mt;
                const bool linear = detail::LinearLight();
                const assemble::SceneMaterial linearMat = (sr && linear) ? detail::LinearColours(mat) : assemble::SceneMaterial{};
                if (sr) {
                    if (tangents.empty()) tangents = ComputeTangents(mesh);
                    if (!RunVertexStage(*sr, mat, mesh, tangents, scene.globalProps, L, camPos, vsOut)) sr = nullptr;
                    else {
                        FillConstants(sr->programs[sr->pi], linear ? linearMat : mat, L, camPos, psCb, &scene.globalProps);
                        mt.scene = &scene; mt.st = &st; mt.prog = &sr->programs[sr->pi]; mt.mat = &mat;
                        mt.decodeSrgb = linear;
                    }
                }
                for (size_t t = 0; t + 2 < sub.indices.size(); t += 3) {
                    uint32_t i0 = sub.indices[t], i1 = sub.indices[t + 1], i2 = sub.indices[t + 2];
                    const Pt &a = P[i0], &b = P[i1], &c = P[i2];
                    float minx = std::min({a.x, b.x, c.x}), maxx = std::max({a.x, b.x, c.x});
                    float miny = std::min({a.y, b.y, c.y}), maxy = std::max({a.y, b.y, c.y});
                    if (maxx < 0 || maxy < 0 || minx >= W || miny >= H) continue;
                    int x0 = std::max(0, static_cast<int>(std::floor(minx))), x1 = std::min(W - 1, static_cast<int>(std::ceil(maxx)));
                    int y0 = std::max(0, static_cast<int>(std::floor(miny))), y1 = std::min(H - 1, static_cast<int>(std::ceil(maxy)));
                    float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                    if (std::fabs(den) < 1e-8f) continue;
                    int lod = 0;
                    const Mips* mp = nullptr;
                    if (tex) {
                        size_t ti = static_cast<size_t>(mat.texture);
                        if (!st.built[ti]) { st.mips[ti] = BuildMips(*tex); st.built[ti] = true; }
                        mp = &st.mips[ti];
                        const auto &t0 = mesh.vertices[i0].uv, &t1 = mesh.vertices[i1].uv, &t2 = mesh.vertices[i2].uv;
                        float uvArea = std::fabs((t1.x - t0.x) * (t2.y - t0.y) - (t2.x - t0.x) * (t1.y - t0.y)) * 0.5f * tex->width * tex->height;
                        float scrArea = std::fabs(den) * 0.5f;
                        if (scrArea > 1e-6f && uvArea > scrArea)
                            lod = std::min(static_cast<int>(mp->w.size()) - 1, static_cast<int>(std::floor(0.5f * std::log2(uvArea / scrArea) + 0.5f)));
                    }
                    for (int y = y0 - (y0 % step); y <= y1; y += step)
                        for (int x = x0 - (x0 % step); x <= x1; x += step) {
                            if (x < 0 || y < 0) continue;
                            float px = x + 0.5f, py = y + 0.5f;
                            float l0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / den;
                            float l1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / den;
                            float l2 = 1.0f - l0 - l1;
                            if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                            float z = l0 * a.z + l1 * b.z + l2 * c.z;
                            size_t zi = static_cast<size_t>(y) * W + x;
                            if (z <= zbuf[zi]) continue;
                            float r = mat.diffuse.x, g = mat.diffuse.y, bl = mat.diffuse.z, al = 1.0f;
                            if (sr) {
                                shader::Sm4Io io;
                                io.constantBuffers = psCb.ptr;
                                io.textures = &mt;
                                mt.lod = static_cast<float>(lod);
                                for (const auto& l : sr->links)
                                    for (int ch = 0; ch < 4; ++ch)
                                        io.input[l.psReg][ch] = l0 * vsOut[i0].r[l.psReg][ch] + l1 * vsOut[i1].r[l.psReg][ch] + l2 * vsOut[i2].r[l.psReg][ch];
                                for (const auto& in : sr->programs[sr->pi].code.inputs) {
                                    if (in.reg >= 32) continue;
                                    if (in.semantic == "SV_POSITION") { io.input[in.reg][0] = px; io.input[in.reg][1] = py; io.input[in.reg][2] = z; io.input[in.reg][3] = 1; }
                                    else if (in.semantic == "SV_IsFrontFace") {
                                        float nzf = l0 * NZ[i0] + l1 * NZ[i1] + l2 * NZ[i2];
                                        uint32_t bits = nzf >= 0 ? 0xFFFFFFFFu : 0u;
                                        std::memcpy(&io.input[in.reg][0], &bits, 4);
                                    }
                                }
                                std::string why;
                                static const char* traceMat = std::getenv("CARDBRIDGE_TRACE_MAT");
                                static bool traced = false;
                                {
                                    static const char* tpx = std::getenv("CARDBRIDGE_TRACE_PX");
                                    int wx = -1, wy = -1;
                                    if (tpx) std::sscanf(tpx, "%d,%d", &wx, &wy);
                                    mt.trace = traceMat && !traced && mat.name == traceMat && (!tpx || (x == wx && y == wy));
                                }
                                if (mt.trace) std::fprintf(stderr, "trace %s (shader %s) at %d,%d\n", mat.name.c_str(), mat.shader.c_str(), x, y);
                                if (mt.trace) for (uint32_t vi2 : {i0, i1, i2}) std::fprintf(stderr, "  vertex pos %.3f %.3f %.3f normal %.3f %.3f %.3f uv %.3f %.3f\n", mesh.vertices[vi2].pos.x, mesh.vertices[vi2].pos.y, mesh.vertices[vi2].pos.z, mesh.vertices[vi2].normal.x, mesh.vertices[vi2].normal.y, mesh.vertices[vi2].normal.z, mesh.vertices[vi2].uv.x, mesh.vertices[vi2].uv.y);
                                if (mt.trace) for (const auto& in : sr->programs[sr->pi].code.inputs) if (in.reg < 32) std::fprintf(stderr, "  in %s%u r%u = %.3f %.3f %.3f %.3f\n", in.semantic.c_str(), in.index, in.reg, io.input[in.reg][0], io.input[in.reg][1], io.input[in.reg][2], io.input[in.reg][3]);
                                io.trace = mt.trace && std::getenv("CARDBRIDGE_TRACE_INSTR");
                                bool okRun = shader::RunSm4(sr->ps, io, &why);
                                if (mt.trace) { std::fprintf(stderr, "  result %s %.3f %.3f %.3f %.3f %s\n", okRun ? "ok" : why.c_str(), io.output[0][0], io.output[0][1], io.output[0][2], io.output[0][3], io.discarded ? "DISCARDED" : ""); traced = true; mt.trace = false; }
                                if (!okRun || io.discarded) continue;
                                r = io.output[0][0]; g = io.output[0][1]; bl = io.output[0][2]; al = io.output[0][3];
                                if (al < 0.5f) continue;
                                if (highlight) highlightTint(r, g, bl);
                                zbuf[zi] = z;
                                auto q2 = [linear](float f) { f = std::min(1.0f, std::max(0.0f, f)); if (linear) f = detail::ToSrgb(f); return static_cast<uint32_t>(f * 255.0f + 0.5f); };
                                uint32_t shadedPx = 0xFF000000u | (q2(r) << 16) | (q2(g) << 8) | q2(bl);
                                for (int dy = 0; dy < step && y + dy < H; ++dy)
                                    for (int dx = 0; dx < step && x + dx < W; ++dx) {
                                        size_t zj = static_cast<size_t>(y + dy) * W + (x + dx);
                                        bgra[zj] = shadedPx;
                                        zbuf[zj] = z;
                                    }
                                continue;
                            }
                            if (mp) {
                                const auto& u0 = mesh.vertices[i0].uv; const auto& u1 = mesh.vertices[i1].uv; const auto& u2 = mesh.vertices[i2].uv;
                                float u = l0 * u0.x + l1 * u1.x + l2 * u2.x, v = l0 * u0.y + l1 * u1.y + l2 * u2.y;
                                u -= std::floor(u); v -= std::floor(v);
                                float tc[4];
                                SampleBilinear(*mp, lod, u, v, tc);
                                r *= tc[0]; g *= tc[1]; bl *= tc[2]; al = tc[3];
                            }
                            if (al < 0.5f) continue;
                            float nz = std::fabs(l0 * NZ[i0] + l1 * NZ[i1] + l2 * NZ[i2]);
                            if (mat.toon.size() == static_cast<size_t>(assemble::kToonRows) && !mat.bakedLit) {   // N.L taken as the facing of the surface
                                const Vec3& rr = mat.toon[static_cast<size_t>(std::min(assemble::kToonRows - 1, static_cast<int>((1.0f - nz) * 0.5f * assemble::kToonRows)))];
                                r *= rr.x; g *= rr.y; bl *= rr.z;
                            } else {
                                float lit = mat.bakedLit ? 1.0f : std::min(1.0f, std::max(0.0f, (nz - 0.35f) / 0.2f));
                                r *= mat.shadow.x + (1.0f - mat.shadow.x) * lit;
                                g *= mat.shadow.y + (1.0f - mat.shadow.y) * lit;
                                bl *= mat.shadow.z + (1.0f - mat.shadow.z) * lit;
                            }
                            if (highlight) highlightTint(r, g, bl);
                            zbuf[zi] = z;
                            auto q = [](float f) { return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, f * 255.0f))); };
                            uint32_t px32 = 0xFF000000u | (q(r) << 16) | (q(g) << 8) | q(bl);
                            for (int dy = 0; dy < step && y + dy < H; ++dy)
                                for (int dx = 0; dx < step && x + dx < W; ++dx) {
                                    size_t zj = static_cast<size_t>(y + dy) * W + (x + dx);
                                    bgra[zj] = px32;
                                    zbuf[zj] = z;
                                }
                        }
                }
            }
        }
    }
    if (opt.shaded) ApplyColourGrade(bgra, 0xFFE2E4E8u);   // the game's own colour grading, like the Direct3D view
}

namespace {

// A mesh laid flat: every vertex goes to its own UV position on a 1 m plane facing +Z, so the shader sees the material as a flat
// sheet in front of the light and the camera, with the tangent frame of the texture itself.
assemble::SceneMesh FlattenToUv(const assemble::SceneMesh& m) {
    assemble::SceneMesh f = m;
    for (auto& v : f.vertices) { v.pos = {v.uv.x - 0.5f, v.uv.y - 0.5f, 0.0f}; v.normal = {0, 0, 1}; }
    return f;
}

void BuildTexMips(const assemble::Scene& scene, ViewState& st, int ti) {
    if (ti < 0 || st.built[static_cast<size_t>(ti)]) return;
    st.mips[static_cast<size_t>(ti)] = BuildMips(scene.textures[static_cast<size_t>(ti)]);
    st.built[static_cast<size_t>(ti)] = true;
}

} // namespace

std::vector<char> EffectiveHiddenParts(const assemble::Scene& scene, const std::vector<char>& hiddenIn) {
    std::vector<char> hidden = hiddenIn;
    hidden.resize(scene.parts.size(), 0);
    std::vector<char> eff = hidden;
    auto shown = [&](size_t i) { return scene.parts[i].visibleInPreview && !hidden[i]; };
    std::map<std::string, size_t> byId;
    bool nude = false;
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        byId[scene.parts[i].id] = i;
        if (scene.parts[i].nudeSwitch && !hidden[i]) nude = true;
    }
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const auto& p = scene.parts[i];
        if (!p.underPiece) continue;
        auto parent = byId.find(p.underOf);
        bool show = parent != byId.end() && shown(parent->second);
        if (show && !nude)
            for (const auto& id : p.coveredBy) {
                auto g = byId.find(id);
                if (id.empty() || (g != byId.end() && shown(g->second))) { show = false; break; }
            }
        eff[i] = show ? 2 : 1;
    }
    return eff;
}

void ApplyPartStateForExport(assemble::Scene& scene, const std::vector<char>& hiddenIn) {
    std::vector<char> hidden = hiddenIn;
    hidden.resize(scene.parts.size(), 0);
    const std::vector<char> eff = EffectiveHiddenParts(scene, hidden);
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        auto& p = scene.parts[i];
        if (p.underPiece) p.startHidden = eff[i] == 1;
        else if ((p.group == "Outfit" || p.group == "Accessories" || p.group == "Hair") && !p.nudeSwitch) p.startHidden = hidden[i] != 0;
    }
}

void BakeShaderTextures(assemble::Scene& scene, int maxSize, bool lit, std::string* report,
                        const std::function<void(const std::string&)>& progress, bool grade, bool toon) {
    grade = grade && ColourGradeEnabled();
    toon = toon && !lit;
    ViewState st;
    st.mips.assign(scene.textures.size(), Mips{});
    st.built.assign(scene.textures.size(), false);
    float L[3] = {0.0f, 0.4f, 0.9165f}, camPos[3] = {0.0f, 0.0f, 5.0f};
    if (lit) {   // the light and camera of the front view the shaded picture uses, so the texture holds the same shading
        Pt mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
        for (const auto& part : scene.parts) {
            if (!part.includeInExport || !part.visibleInPreview) continue;
            for (int me : part.meshes)
                for (const auto& v : scene.meshes[static_cast<size_t>(me)].vertices) {
                    mn = {std::min(mn.x, v.pos.x), std::min(mn.y, v.pos.y), std::min(mn.z, v.pos.z)};
                    mx = {std::max(mx.x, v.pos.x), std::max(mx.y, v.pos.y), std::max(mx.z, v.pos.z)};
                }
        }
        const float pit = 4.0f * 3.14159265f / 180.0f, toCam[3] = {0.0f, std::sin(pit), std::cos(pit)};
        float n = 0;
        for (int c = 0; c < 3; ++c) L[c] = toCam[c] + (c == 1 ? 0.45f : 0.0f);
        for (float c : L) n += c * c;
        for (float& c : L) c /= std::sqrt(n);
        const float ctr[3] = {(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
        for (int c = 0; c < 3; ++c) camPos[c] = ctr[c] + toCam[c] * 5.0f;
    }
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int baked = 0;
    const size_t originalTextures = scene.textures.size();
    for (size_t mi = 0; mi < scene.materials.size(); ++mi) {
        auto& mat = scene.materials[mi];
        const ShaderRuntime* sr = GetShader(scene, st, mat.shaderIndex);
        if (!sr) continue;
        // The submeshes of exported parts that use this material, each on a flattened copy of its mesh.
        struct Use { size_t flat; const std::vector<uint32_t>* indices; };
        std::vector<Use> uses;
        std::vector<assemble::SceneMesh> flats;
        for (const auto& part : scene.parts) {
            if (!part.includeInExport) continue;
            for (int me : part.meshes) {
                const auto& mesh = scene.meshes[static_cast<size_t>(me)];
                size_t flatIdx = static_cast<size_t>(-1);
                for (const auto& sub : mesh.submeshes) {
                    if (sub.material != static_cast<int>(mi)) continue;
                    if (flatIdx == static_cast<size_t>(-1)) { flats.push_back(lit ? mesh : FlattenToUv(mesh)); flatIdx = flats.size() - 1; }
                    uses.push_back({flatIdx, &sub.indices});
                }
            }
        }
        if (uses.empty()) continue;
        int W = 512, H = 512;
        if (mat.texture >= 0 && static_cast<size_t>(mat.texture) < originalTextures) { W = scene.textures[static_cast<size_t>(mat.texture)].width; H = scene.textures[static_cast<size_t>(mat.texture)].height; }
        while (std::max(W, H) > maxSize) { W = std::max(1, W / 2); H = std::max(1, H / 2); }
        W = std::max(W, 64); H = std::max(H, 64);

        // The textures the shader reads are built before the worker threads start (they only read from here on).
        for (const auto& tb : mat.texProps) BuildTexMips(scene, st, tb.texture);
        for (const auto& tb : scene.globalTextures) BuildTexMips(scene, st, tb.texture);

        // The shader over the uses' UV layouts at W x H with the light Lv: img gets rgba, cov the texels a triangle reached.
        auto render = [&](int W, int H, const float* Lv, std::vector<float>& img, std::vector<uint8_t>& cov) -> bool {
        CbSet psCb;
        FillConstants(sr->programs[sr->pi], mat, Lv, camPos, psCb, &scene.globalProps);
        std::vector<std::vector<PsIn>> vsOut(flats.size());
        bool ok = true;
        for (size_t f = 0; f < flats.size() && ok; ++f) ok = RunVertexStage(*sr, mat, flats[f], ComputeTangents(flats[f]), scene.globalProps, Lv, camPos, vsOut[f]);
        if (!ok) return false;

        img.assign(static_cast<size_t>(W) * H * 4, 0.0f);
        cov.assign(static_cast<size_t>(W) * H, 0);
        auto worker = [&](unsigned wi) {
            MatTextures mt;
            mt.scene = &scene; mt.st = &st; mt.prog = &sr->programs[sr->pi]; mt.mat = &mat; mt.lod = 0;
            for (const auto& use : uses) {
                const auto& fm = flats[use.flat];
                const auto& vo = vsOut[use.flat];
                for (size_t t = 0; t + 2 < use.indices->size(); t += 3) {
                    uint32_t i0 = (*use.indices)[t], i1 = (*use.indices)[t + 1], i2 = (*use.indices)[t + 2];
                    float ax = fm.vertices[i0].uv.x * W, ay = (1.0f - fm.vertices[i0].uv.y) * H;
                    float bx = fm.vertices[i1].uv.x * W, by = (1.0f - fm.vertices[i1].uv.y) * H;
                    float cx = fm.vertices[i2].uv.x * W, cy = (1.0f - fm.vertices[i2].uv.y) * H;
                    {   // UVs outside 0..1 repeat: the triangle lands in the tile its centre is in
                        float tu = std::floor((fm.vertices[i0].uv.x + fm.vertices[i1].uv.x + fm.vertices[i2].uv.x) / 3.0f);
                        float tv = std::floor((fm.vertices[i0].uv.y + fm.vertices[i1].uv.y + fm.vertices[i2].uv.y) / 3.0f);
                        ax -= tu * W; bx -= tu * W; cx -= tu * W;
                        ay += tv * H; by += tv * H; cy += tv * H;
                    }
                    float den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                    // A triangle with no UV area (a garment part coloured from one point of its texture, like a skirt's plain upper half)
                    // still samples that point in the PMX: the texel under its UV centre is shaded with the triangle's centre.
                    const bool point = std::fabs(den) < 1e-8f;
                    int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx})))), x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
                    int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy})))), y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
                    if (point) {
                        x0 = x1 = static_cast<int>(std::floor((ax + bx + cx) / 3.0f));
                        y0 = y1 = static_cast<int>(std::floor((ay + by + cy) / 3.0f));
                        if (x0 < 0 || y0 < 0 || x0 >= W || y0 >= H) continue;
                    }
                    for (int y = y0; y <= y1; ++y) {
                        if (static_cast<unsigned>(y) % hw != wi) continue;
                        for (int x = x0; x <= x1; ++x) {
                            float px = x + 0.5f, py = y + 0.5f;
                            float l0 = 1.0f / 3.0f, l1 = 1.0f / 3.0f, l2 = 1.0f / 3.0f;
                            if (!point) {
                                l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
                                l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
                                l2 = 1.0f - l0 - l1;
                                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                            }
                            size_t ti = static_cast<size_t>(y) * W + x;
                            if (cov[ti]) continue;   // overlapping UV islands: the first one wins
                            shader::Sm4Io io;
                            io.constantBuffers = psCb.ptr;
                            io.textures = &mt;
                            for (const auto& l : sr->links)
                                for (int ch = 0; ch < 4; ++ch)
                                    io.input[l.psReg][ch] = l0 * vo[i0].r[l.psReg][ch] + l1 * vo[i1].r[l.psReg][ch] + l2 * vo[i2].r[l.psReg][ch];
                            for (const auto& in : sr->programs[sr->pi].code.inputs) {
                                if (in.reg >= 32) continue;
                                if (in.semantic == "SV_POSITION") { io.input[in.reg][0] = px; io.input[in.reg][1] = py; io.input[in.reg][2] = 0.5f; io.input[in.reg][3] = 1; }
                                else if (in.semantic == "SV_IsFrontFace") { uint32_t bits = 0xFFFFFFFFu; std::memcpy(&io.input[in.reg][0], &bits, 4); }
                            }
                            std::string why;
                            if (!shader::RunSm4(sr->ps, io, &why)) continue;
                            cov[ti] = 1;
                            float a = io.discarded ? 0.0f : io.output[0][3];
                            if (!mat.alpha && a >= 0.5f) a = 1.0f;
                            for (int c = 0; c < 3; ++c) img[ti * 4 + static_cast<size_t>(c)] = io.output[0][c];
                            img[ti * 4 + 3] = a;
                        }
                    }
                }
            }
        };
        std::vector<std::thread> pool;
        for (unsigned w = 1; w < hw; ++w) pool.emplace_back(worker, w);
        worker(0);
        for (auto& th : pool) th.join();
        return true;
        };
        std::vector<float> img;
        std::vector<uint8_t> cov;
        if (!render(W, H, L, img, cov)) continue;

        if (toon) {   // the shade ramp: the mean colour at each light angle over the sheet, relative to the baked front light
            auto meanColour = [&](const float* Lv, float out[3]) {
                std::vector<float> ti;
                std::vector<uint8_t> tc;
                out[0] = out[1] = out[2] = 0.0f;
                if (!render(64, 64, Lv, ti, tc)) return false;
                double sum[3] = {0, 0, 0};
                size_t n = 0;
                for (size_t i = 0; i < tc.size(); ++i)
                    if (tc[i] && ti[i * 4 + 3] >= 0.5f) { for (int c = 0; c < 3; ++c) sum[c] += ti[i * 4 + static_cast<size_t>(c)]; ++n; }
                if (!n) return false;
                for (int c = 0; c < 3; ++c) out[c] = static_cast<float>(sum[c] / static_cast<double>(n));
                return true;
            };
            float ref[3];
            std::vector<Vec3> ramp;
            if (meanColour(L, ref)) {
                for (int k = 0; k < assemble::kToonRows; ++k) {
                    const float c = 1.0f - 2.0f * (static_cast<float>(k) + 0.5f) / static_cast<float>(assemble::kToonRows);   // N.L of this row
                    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
                    const float Lv[3] = {0.0f, s, c};
                    float m[3];
                    Vec3 row{1, 1, 1};
                    if (meanColour(Lv, m)) {
                        float* rc[3] = {&row.x, &row.y, &row.z};
                        for (int ch = 0; ch < 3; ++ch) *rc[ch] = ref[ch] > 1e-3f ? std::min(1.0f, std::max(0.0f, m[ch] / ref[ch])) : 1.0f;
                    }
                    ramp.push_back(row);
                }
                mat.toon = std::move(ramp);
            }
        }

        if (std::getenv("CARDBRIDGE_DEBUG_MAT")) {
            size_t covered = 0, solid = 0;
            for (size_t i = 0; i < cov.size(); ++i) if (cov[i]) { ++covered; if (img[i * 4 + 3] >= 0.5f) ++solid; }
            std::fprintf(stderr, "bake %s (%s): %dx%d, %zu texels covered, %zu with alpha >= 0.5, alpha flag %d\n", mat.name.c_str(), mat.shader.c_str(), W, H, covered, solid, mat.alpha ? 1 : 0);
        }
        // Pad the islands so bilinear filtering and mips do not pull in empty texels at the seams.
        for (int pass = 0; pass < 6; ++pass) {
            std::vector<uint8_t> cov2 = cov;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    size_t ti = static_cast<size_t>(y) * W + x;
                    if (cov[ti]) continue;
                    for (int d = 0; d < 4; ++d) {
                        int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        size_t ni = static_cast<size_t>(ny) * W + nx;
                        if (!cov[ni]) continue;
                        for (int c = 0; c < 4; ++c) img[ti * 4 + static_cast<size_t>(c)] = img[ni * 4 + static_cast<size_t>(c)];
                        cov2[ti] = 1;
                        break;
                    }
                }
            cov.swap(cov2);
        }
        assemble::SceneTexture bt;
        bt.name = "baked_" + mat.name;
        bt.source = "shader bake (" + mat.shader + ")";
        bt.width = W; bt.height = H;
        bt.rgba.resize(static_cast<size_t>(W) * H * 4);
        for (size_t i = 0; i < bt.rgba.size(); ++i) bt.rgba[i] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, img[i] * 255.0f + 0.5f)));
        if (grade) {   // the same look-up the preview applies to its frame; alpha stays
            std::vector<uint32_t> px(static_cast<size_t>(W) * H);
            for (size_t i = 0; i < px.size(); ++i)
                px[i] = (uint32_t(bt.rgba[i * 4 + 3]) << 24) | (uint32_t(bt.rgba[i * 4]) << 16) | (uint32_t(bt.rgba[i * 4 + 1]) << 8) | bt.rgba[i * 4 + 2];
            ApplyColourGrade(px, 0u);
            for (size_t i = 0; i < px.size(); ++i) { bt.rgba[i * 4] = (px[i] >> 16) & 255; bt.rgba[i * 4 + 1] = (px[i] >> 8) & 255; bt.rgba[i * 4 + 2] = px[i] & 255; }
        }
        scene.textures.push_back(std::move(bt));
        st.mips.push_back(Mips{});
        st.built.push_back(false);
        // The shader's colour is now in the texture: the PMX material multiplies by white and keeps its alpha.
        mat.texture = static_cast<int>(scene.textures.size()) - 1;
        mat.diffuse = {1.0f, 1.0f, 1.0f, mat.diffuse.w};
        mat.bakedLit = lit;
        ++baked;
        if (progress) progress("baked texture " + std::to_string(mi + 1) + "/" + std::to_string(scene.materials.size()) + ": " +
                               mat.name + " (" + std::to_string(W) + "x" + std::to_string(H) + ")");
    }
    if (report) *report = std::string(lit ? "baked (shaded, front light" : "baked (colour only") + (grade ? ", colour graded" : "") + (toon ? ", toon ramps) " : ") ") + std::to_string(baked) + " material texture(s) through the game's shaders";
}

void RunCreateJobs(assemble::Scene& scene) {
    if (scene.createJobs.empty()) return;
    ViewState st;
    st.mips.assign(scene.textures.size(), Mips{});
    st.built.assign(scene.textures.size(), false);
    const float L[3] = {0.0f, 0.0f, 1.0f}, camPos[3] = {0.0f, 0.0f, 5.0f};
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    for (auto& job : scene.createJobs) {
        auto& mat = job.material;
        const ShaderRuntime* sr = GetShader(scene, st, mat.shaderIndex);
        if (!sr) { scene.warn("", "create material '" + mat.name + "': its shader could not be run; the texture keeps its plain input"); continue; }
        // Graphics.Blit: one quad over the whole target, UV 0..1.
        assemble::SceneMesh quad;
        const Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (const Vec2& uv : uvs) {
            assemble::SceneVertex v;
            v.pos = {uv.x - 0.5f, uv.y - 0.5f, 0.0f};
            v.normal = {0, 0, 1};
            v.uv = uv;
            v.uv1 = uv;
            v.uv2 = uv;
            quad.vertices.push_back(v);
        }
        for (const auto& tb : mat.texProps) BuildTexMips(scene, st, tb.texture);
        for (const auto& tb : scene.globalTextures) BuildTexMips(scene, st, tb.texture);
        if (job.linear)   // colour properties arrive as linear values
            for (auto& pr : mat.props)
                if (pr.first.rfind("_Color", 0) == 0)
                    for (float* c : {&pr.second.x, &pr.second.y, &pr.second.z}) *c = *c <= 0.04045f ? *c / 12.92f : std::pow((*c + 0.055f) / 1.055f, 2.4f);
        CbSet psCb;
        FillConstants(sr->programs[sr->pi], mat, L, camPos, psCb, &scene.globalProps);
        std::vector<PsIn> vo;
        if (!RunVertexStage(*sr, mat, quad, ComputeTangents(quad), scene.globalProps, L, camPos, vo)) continue;
        const int W = job.width, H = job.height;
        std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4, 0);
        auto worker = [&](unsigned wi) {
            MatTextures mt;
            mt.scene = &scene; mt.st = &st; mt.prog = &sr->programs[sr->pi]; mt.mat = &mat; mt.lod = 0; mt.decodeSrgb = job.linear;
            for (int y = static_cast<int>(wi); y < H; y += static_cast<int>(hw))
                for (int x = 0; x < W; ++x) {
                    const float u = (x + 0.5f) / W, v = 1.0f - (y + 0.5f) / H;
                    // The quad is two triangles split on the diagonal u == v; both interpolate the varyings the same way.
                    const bool lower = v <= u;
                    const size_t i0 = 0, i1 = lower ? 1 : 2, i2 = lower ? 2 : 3;
                    const float l0 = lower ? 1.0f - u : 1.0f - v, l1 = lower ? u - v : u, l2 = lower ? v : v - u;
                    shader::Sm4Io io;
                    io.constantBuffers = psCb.ptr;
                    io.textures = &mt;
                    for (const auto& l : sr->links)
                        for (int ch = 0; ch < 4; ++ch)
                            io.input[l.psReg][ch] = l0 * vo[i0].r[l.psReg][ch] + l1 * vo[i1].r[l.psReg][ch] + l2 * vo[i2].r[l.psReg][ch];
                    for (const auto& in : sr->programs[sr->pi].code.inputs) {
                        if (in.reg >= 32) continue;
                        if (in.semantic == "SV_POSITION") { io.input[in.reg][0] = x + 0.5f; io.input[in.reg][1] = y + 0.5f; io.input[in.reg][2] = 0.5f; io.input[in.reg][3] = 1; }
                        else if (in.semantic == "SV_IsFrontFace") { uint32_t bits = 0xFFFFFFFFu; std::memcpy(&io.input[in.reg][0], &bits, 4); }
                    }
                    std::string why;
                    if (!shader::RunSm4(sr->ps, io, &why) || io.discarded) continue;
                    uint8_t* o = &rgba[(static_cast<size_t>(y) * W + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        float v = std::min(1.0f, std::max(0.0f, io.output[0][c]));
                        if (job.linear && c < 3) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                        o[c] = static_cast<uint8_t>(v * 255.0f + 0.5f);
                    }
                }
        };
        std::vector<std::thread> pool;
        for (unsigned w = 1; w < hw; ++w) pool.emplace_back(worker, w);
        worker(0);
        for (auto& th : pool) th.join();
        if (!job.overlay.empty() && job.overlayWidth > 0 && job.overlayHeight > 0) {
            if (job.overlayReplaces) std::fill(rgba.begin(), rgba.end(), uint8_t{0});
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const int ox = std::min(job.overlayWidth - 1, x * job.overlayWidth / W), oy = std::min(job.overlayHeight - 1, y * job.overlayHeight / H);
                    const uint8_t* o = &job.overlay[(static_cast<size_t>(oy) * job.overlayWidth + ox) * 4];
                    const float a = o[3] / 255.0f;
                    if (a <= 0.0f) continue;
                    uint8_t* d = &rgba[(static_cast<size_t>(y) * W + x) * 4];
                    const float da = d[3] / 255.0f, oa = a + da * (1.0f - a);
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((o[c] * a + d[c] * da * (1.0f - a)) / std::max(oa, 1e-6f));
                    d[3] = static_cast<uint8_t>(oa * 255.0f);
                }
        }
        auto& tex = scene.textures[static_cast<size_t>(job.texture)];
        tex.width = W;
        tex.height = H;
        tex.rgba = std::move(rgba);
        tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
        st.mips[static_cast<size_t>(job.texture)] = Mips{};
        st.built[static_cast<size_t>(job.texture)] = false;
    }
    scene.createJobs.clear();
}

namespace detail {   // used by GpuView.cpp
void FillShaderConstants(const shader::UnityProgram& prog, const assemble::SceneMaterial& mat, const float lightDir[3], const float camPos[3],
                         std::vector<std::vector<float>>& data, const std::vector<std::pair<std::string, Vec4>>* globals) {
    CbSet c;
    FillConstants(prog, mat, lightDir, camPos, c, globals);
    data = std::move(c.data);
}
std::vector<Vec3> ComputeMeshTangents(const assemble::SceneMesh& m) { return ComputeTangents(m); }
} // namespace detail

} // namespace cardbridge::exporter
