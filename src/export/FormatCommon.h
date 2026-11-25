#pragma once

#include "assemble/Scene.h"
#include "core/Bytes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cardbridge::exportcommon {

// ---- Coordinate conversion -------------------------------------------------
// The canonical scene is left-handed Y-up; glTF and OBJ are right-handed Y-up.
// One mirror of X, applied to geometry, normals, bone matrices and morph deltas
// together. `scale` is metres per scene unit (1.0 by default for both formats).
Vec3 Point(const Vec3& p, float scale);
Vec3 Direction(const Vec3& n);
Vec3 Delta(const Vec3& d, float scale);

// M * W * M for the X mirror, then a uniform scale. Row-major, like core/Math.h.
Mat4 WorldMatrix(const Mat4& world, float scale);
// Local = inverse(parentWorld') * world'. The caller passes already-converted worlds.
Mat4 LocalMatrix(const Mat4& convertedParentWorld, const Mat4& convertedWorld);
// glTF matrices are column-major; core::Mat4 is row-major.
void ToColumnMajor(const Mat4& m, float out[16]);

// Tangents are not in the canonical scene. Derive them from converted positions,
// normals and UV0 (Gram-Schmidt) so normal maps work in Blender.
struct Tangent { Vec4 t; };
std::vector<Tangent> ComputeTangents(const std::vector<Vec3>& positions,
                                     const std::vector<Vec3>& normals,
                                     const std::vector<Vec2>& uvs,
                                     const std::vector<uint32_t>& indices);

// ---- Materials -------------------------------------------------------------
enum class AlphaMode { Opaque, Mask, Blend };
// Deterministic classification shared by both writers. Uses CardBridge's existing
// material-semantics helpers so the new formats agree with the viewer/PMX on what is
// solid, clipped or translucent. `cutoff` is filled for Mask materials.
AlphaMode ClassifyAlpha(const assemble::Scene& scene, const assemble::SceneMaterial& mat, float& cutoff);
float RoughnessFromSpecular(float specularPower);

// ---- Textures --------------------------------------------------------------
struct TextureEntry {
    int sceneIndex = -1;
    std::string name;
    std::string file;             // relative, forward slashes
    std::vector<uint8_t> png;
};

// Encodes only the textures a material actually references, deduplicated by pixel hash.
// `textures/` is created on demand. Nothing is written until Flush(), so a failed
// export never leaves a half-written texture set behind from this writer.
class TextureWriter {
public:
    TextureWriter(const assemble::Scene& scene, const std::string& outDir, bool enabled, bool overwrite);
    int Use(int sceneIndex);      // returns index into Entries(), or -1 when textures are off/invalid
    void Flush();
    const std::vector<TextureEntry>& Entries() const { return entries_; }
private:
    const assemble::Scene& scene_;
    std::string outDir_;
    bool enabled_, overwrite_;
    std::vector<TextureEntry> entries_;
    std::vector<int> byScene_;    // scene texture index -> entry index
    bool flushed_ = false;
};

Bytes EncodePng(const assemble::SceneTexture& t);

// ---- Files -----------------------------------------------------------------
// Write to "<path>.tmp" through the UTF-8-safe FileIo layer, then atomically
// replace the target. A failure removes the temp file; an existing valid file is
// never truncated. `overwrite` guards an existing target.
void WriteAtomic(const std::string& path, const uint8_t* data, size_t size, bool overwrite);
void WriteAtomicText(const std::string& path, const std::string& text, bool overwrite);

std::string SafeStem(const std::string& name, const std::string& fallback = "character");
// OBJ/MTL token (no whitespace), preserving UTF-8 bytes.
std::string Token(const std::string& name);

void CheckCancelled(const std::function<bool()>& cancelled);
void ReportProgress(const std::function<void(double, const std::string&)>& progress, double f, const std::string& stage);
void ReportLog(const std::function<void(const std::string&)>& log, const std::string& message);

} // namespace cardbridge::exportcommon
