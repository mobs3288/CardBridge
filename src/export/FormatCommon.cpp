#include "export/FormatCommon.h"
#include "export/Exporter.h"
#include "core/FileIo.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

namespace cardbridge::exportcommon {

Vec3 Point(const Vec3& p, float scale) { return {-p.x * scale, p.y * scale, p.z * scale}; }
Vec3 Direction(const Vec3& n) { return {-n.x, n.y, n.z}; }
Vec3 Delta(const Vec3& d, float scale) { return {-d.x * scale, d.y * scale, d.z * scale}; }

Mat4 WorldMatrix(const Mat4& world, float scale) {
    Mat4 out;
    const float s[4] = {-1.0f, 1.0f, 1.0f, 1.0f};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            // M * W * M with M = diag(-1,1,1,1), then uniform scale on the affine part.
            float v = world.m[row * 4 + col] * s[row] * s[col];
            if (row < 3) v *= scale;   // upper 3x3 and translation column
            out.m[row * 4 + col] = v;
        }
    }
    return out;
}

Mat4 LocalMatrix(const Mat4& convertedParentWorld, const Mat4& convertedWorld) {
    return inverse(convertedParentWorld) * convertedWorld;
}

void ToColumnMajor(const Mat4& m, float out[16]) {
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            out[col * 4 + row] = m.m[row * 4 + col];
}

std::vector<Tangent> ComputeTangents(const std::vector<Vec3>& positions,
                                     const std::vector<Vec3>& normals,
                                     const std::vector<Vec2>& uvs,
                                     const std::vector<uint32_t>& indices) {
    const size_t n = positions.size();
    std::vector<float> tan(3 * n, 0.0f), bitan(3 * n, 0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;
        const float du1 = uvs[b].x - uvs[a].x, dv1 = uvs[b].y - uvs[a].y;
        const float du2 = uvs[c].x - uvs[a].x, dv2 = uvs[c].y - uvs[a].y;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f) continue;
        const Vec3 e1{positions[b].x - positions[a].x, positions[b].y - positions[a].y, positions[b].z - positions[a].z};
        const Vec3 e2{positions[c].x - positions[a].x, positions[c].y - positions[a].y, positions[c].z - positions[a].z};
        const float inv = 1.0f / det;
        const Vec3 t{(dv2 * e1.x - dv1 * e2.x) * inv, (dv2 * e1.y - dv1 * e2.y) * inv, (dv2 * e1.z - dv1 * e2.z) * inv};
        const Vec3 bt{(du1 * e2.x - du2 * e1.x) * inv, (du1 * e2.y - du2 * e1.y) * inv, (du1 * e2.z - du2 * e1.z) * inv};
        for (uint32_t v : {a, b, c}) {
            tan[v * 3] += t.x; tan[v * 3 + 1] += t.y; tan[v * 3 + 2] += t.z;
            bitan[v * 3] += bt.x; bitan[v * 3 + 1] += bt.y; bitan[v * 3 + 2] += bt.z;
        }
    }
    std::vector<Tangent> out(n);
    for (size_t v = 0; v < n; ++v) {
        Vec3 t{tan[v * 3], tan[v * 3 + 1], tan[v * 3 + 2]};
        const Vec3& nn = normals[v];
        const float dot = t.x * nn.x + t.y * nn.y + t.z * nn.z;
        t = {t.x - nn.x * dot, t.y - nn.y * dot, t.z - nn.z * dot};
        t = normalize(t);
        const Vec3 b{bitan[v * 3], bitan[v * 3 + 1], bitan[v * 3 + 2]};
        const Vec3 crossNnT{nn.y * t.z - nn.z * t.y, nn.z * t.x - nn.x * t.z, nn.x * t.y - nn.y * t.x};
        const float w = (b.x * crossNnT.x + b.y * crossNnT.y + b.z * crossNnT.z) >= 0.0f ? 1.0f : -1.0f;
        out[v].t = {t.x, t.y, t.z, w};
    }
    return out;
}

AlphaMode ClassifyAlpha(const assemble::Scene& scene, const assemble::SceneMaterial& mat, float& cutoff) {
    cutoff = 0.5f;
    for (const auto& [name, value] : mat.props)
        if (name == "_Cutoff") cutoff = std::clamp(value.x, 0.0f, 1.0f);

    const assemble::SceneTexture* tex = nullptr;
    if (mat.texture >= 0 && static_cast<size_t>(mat.texture) < scene.textures.size())
        tex = &scene.textures[static_cast<size_t>(mat.texture)];
    const bool opaqueTex = tex && assemble::TextureIsFullyOpaque(*tex);

    // Translucent compositing is a blend regardless of the texture's alpha bytes.
    if (mat.translucent || (mat.renderQueue >= 3000 && !mat.shaderClipsAlpha)) return AlphaMode::Blend;
    // An alpha-test shader discards texels; the game's own cutout verdicts are shared with PMX.
    if (mat.shaderClipsAlpha || mat.fullyCutByAlpha ||
        (!opaqueTex && (exporter::HasCutout(scene, mat) || exporter::HasAlphaCutout(scene, mat))))
        return AlphaMode::Mask;
    if (mat.alpha && tex && !opaqueTex) return AlphaMode::Blend;
    return AlphaMode::Opaque;
}

float RoughnessFromSpecular(float specularPower) {
    if (!std::isfinite(specularPower)) return 1.0f;
    return std::clamp(1.0f - 0.01f * specularPower, 0.3f, 1.0f);
}

Bytes EncodePng(const assemble::SceneTexture& t) {
    Bytes out;
    if (t.width <= 0 || t.height <= 0 || t.rgba.size() < static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4)
        throw FormatError("texture '" + t.name + "' has no valid RGBA pixels");
    auto cb = [](void* ctx, void* data, int size) {
        auto* b = static_cast<Bytes*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        b->insert(b->end(), p, p + size);
    };
    if (!stbi_write_png_to_func(cb, &out, t.width, t.height, 4, t.rgba.data(), t.width * 4))
        throw FormatError("PNG encode failed for texture " + t.name);
    return out;
}

TextureWriter::TextureWriter(const assemble::Scene& scene, const std::string& outDir, bool enabled, bool overwrite)
    : scene_(scene), outDir_(outDir), enabled_(enabled), overwrite_(overwrite) {
    byScene_.assign(scene.textures.size(), -1);
}

int TextureWriter::Use(int sceneIndex) {
    if (!enabled_ || sceneIndex < 0 || static_cast<size_t>(sceneIndex) >= scene_.textures.size()) return -1;
    if (byScene_[static_cast<size_t>(sceneIndex)] >= 0) return byScene_[static_cast<size_t>(sceneIndex)];
    // Deduplicate by the hash CardBridge already computes from the pixels, so the
    // many atlas variants of one image do not each get a PNG file.
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& other = scene_.textures[static_cast<size_t>(entries_[i].sceneIndex)];
        if (other.pixelHash != 0 && other.pixelHash == scene_.textures[static_cast<size_t>(sceneIndex)].pixelHash) {
            byScene_[static_cast<size_t>(sceneIndex)] = static_cast<int>(i);
            return static_cast<int>(i);
        }
    }
    const auto& t = scene_.textures[static_cast<size_t>(sceneIndex)];
    TextureEntry e;
    e.sceneIndex = sceneIndex;
    e.name = t.name.empty() ? "texture" : t.name;
    e.file = "textures/" + SafeStem(e.name, "texture") + "_" + std::to_string(entries_.size()) + ".png";
    e.png = EncodePng(t);
    const int index = static_cast<int>(entries_.size());
    entries_.push_back(std::move(e));
    byScene_[static_cast<size_t>(sceneIndex)] = index;
    return index;
}

void TextureWriter::Flush() {
    if (flushed_ || !enabled_) return;
    if (!entries_.empty()) fileio::CreateDirectories(fileio::Join(outDir_, "textures"));
    for (const auto& e : entries_)
        WriteAtomic(fileio::Join(outDir_, e.file), e.png.data(), e.png.size(), overwrite_);
    flushed_ = true;
}

namespace {
bool ReplaceFileUtf8(const std::string& from, const std::string& to) {
#ifdef _WIN32
    auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::remove(to.c_str());
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}
} // namespace

void WriteAtomic(const std::string& path, const uint8_t* data, size_t size, bool overwrite) {
    if (!overwrite && fileio::Exists(path))
        throw FormatError("refusing to overwrite existing file: " + path + " (use overwrite)");
    const std::string tmp = path + ".tmp";
    fileio::WriteFile(tmp, data, size);
    if (!ReplaceFileUtf8(tmp, path)) {
        fileio::RemoveFile(tmp);
        throw FormatError("could not replace output file: " + path);
    }
}

void WriteAtomicText(const std::string& path, const std::string& text, bool overwrite) {
    WriteAtomic(path, reinterpret_cast<const uint8_t*>(text.data()), text.size(), overwrite);
}

std::string SafeStem(const std::string& name, const std::string& fallback) {
    return name.empty() ? fallback : fileio::SafeFileName(name);
}

std::string Token(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c <= ' ' || c == '#' || c == '\\' || c == '/' || c == ':') out.push_back('_');
        else out.push_back(static_cast<char>(c));
    }
    return out.empty() ? std::string("material") : out;
}

void CheckCancelled(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw Cancelled{};
}

void ReportProgress(const std::function<void(double, const std::string&)>& progress, double f, const std::string& stage) {
    if (progress) progress(f, stage);
}

void ReportLog(const std::function<void(const std::string&)>& log, const std::string& message) {
    if (log) log(message);
}

} // namespace cardbridge::exportcommon
