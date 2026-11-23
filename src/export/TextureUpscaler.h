#pragma once

#include <string>

namespace cardbridge {
namespace exporter {

// Optional post-export texture upscaling through waifu2x-ncnn-vulkan (MIT, github.com/nihui/waifu2x-ncnn-vulkan).
// It is a plain external program invoked on the PNGs after they are written; the PMX needs no change because
// PMX texture references carry no dimensions. Nothing here is used unless the caller opts in.

// Finds waifu2x-ncnn-vulkan.exe and its models directory. `exe` may be an explicit path; otherwise the
// CARDBRIDGE_UPSCALER env var, tools/upscaler/, and PATH are tried. Returns false with a reason in err.
bool ResolveUpscaler(std::string& exe, std::string& modelDir, std::string* err);

// Upscales one PNG in place (writes a temporary file, then replaces the original). The anime-style
// model interpolates alpha too, which antialiases cutout edges; interior alpha stays at 255.
bool UpscalePngInPlace(const std::string& exe, const std::string& modelDir, const std::string& pngPath,
                       int scale, std::string* err);

// Toon/sphere maps are data, not colour: upscaling them would change MMD's lighting response.
bool IsUpscalableTexture(const std::string& fileName);

} // namespace exporter
} // namespace cardbridge
