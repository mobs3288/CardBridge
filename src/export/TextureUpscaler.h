#pragma once

#include <string>

namespace cardbridge {
namespace exporter {

// Optional post-export texture upscaling through open-source Vulkan upscalers. It is a plain
// external program invoked on the PNGs after they are written; PMX stores no texture dimensions,
// so no PMX change is needed. Nothing here is used unless the caller opts in.

// "waifu2x" (github.com/nihui/waifu2x-ncnn-vulkan, MIT) or "realesrgan" (github.com/xinntao/
// Real-ESRGAN-ncnn-vulkan, MIT, x4plus-anime model). Case-insensitive; empty means waifu2x.
bool IsKnownUpscaler(const std::string& model);

// Finds the executable and its models directory. `exe` may be an explicit path; otherwise
// CARDBRIDGE_UPSCALER, tools/upscaler/ and PATH are tried. Returns false with a reason in err.
bool ResolveUpscaler(const std::string& model, std::string& exe, std::string& modelDir, std::string* err);

// Upscales one PNG in place (temporary file, then replace). Both models interpolate alpha,
// which antialiases cutout edges. The realesrgan path first dilates RGB into fully transparent
// texels so the model cannot bleed their stored black into the visible edge.
bool UpscalePngInPlace(const std::string& model, const std::string& exe, const std::string& modelDir,
                       const std::string& pngPath, int scale, std::string* err);

// Toon/sphere maps are data, not colour: upscaling them would change MMD's lighting response.
bool IsUpscalableTexture(const std::string& fileName);

} // namespace exporter
} // namespace cardbridge
