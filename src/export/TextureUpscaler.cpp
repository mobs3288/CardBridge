#include "export/TextureUpscaler.h"

#include "core/FileIo.h"
#include "card/MaterialEdits.h"

#include "core/Bytes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

// Decoding goes through the project's shared helper (it owns the stb_image implementation);
// only the writer is declared here, as in Exporter.cpp.
#include <stb_image_write.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cardbridge {
namespace exporter {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool exists(const std::string& p) { return fileio::Exists(p); }

std::string normalizedModel(const std::string& model) {
    const std::string s = lower(model);
    if (s == "realesrgan" || s == "real-esrgan" || s == "realesrgan-x4plus-anime") return "realesrgan";
    return "waifu2x";
}

#ifdef _WIN32
std::string searchPath(const std::string& name) {
    char buf[MAX_PATH] = {};
    const DWORD n = SearchPathA(nullptr, name.c_str(), nullptr, sizeof buf, buf, nullptr);
    return n > 0 && n < sizeof buf ? std::string(buf) : std::string();
}
#endif

// Fill the RGB of fully transparent texels from their nearest covered neighbour. A cutout atlas
// stores black there; a learned colour model would otherwise smear that black over the edge.
bool FillTransparentRgb(const std::string& inPath, const std::string& outPath) {
    const Bytes file = fileio::ReadFile(inPath);
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!card::DecodeImageBytes(file, w, h, px) || w <= 0 || h <= 0) return false;
    std::vector<uint8_t> cov(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < cov.size(); ++i) cov[i] = px[i * 4 + 3] > 0 ? 1 : 0;
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                if (cov[i]) continue;
                size_t from = static_cast<size_t>(-1);
                for (int d = 0; d < 4 && from == static_cast<size_t>(-1); ++d) {
                    const int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t ni = static_cast<size_t>(ny) * w + nx;
                    if (cov[ni]) from = ni;
                }
                if (from == static_cast<size_t>(-1)) continue;
                for (int c = 0; c < 3; ++c) px[i * 4 + c] = px[from * 4 + c];
                cov[i] = 1;
                changed = true;
            }
        if (!changed) break;
    }
    return stbi_write_png(outPath.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

} // namespace

bool IsKnownUpscaler(const std::string& model) {
    const std::string s = lower(model);
    return s.empty() || s == "waifu2x" || s == "realesrgan" || s == "real-esrgan" ||
           s == "realesrgan-x4plus-anime";
}

bool IsUpscalableTexture(const std::string& fileName) {
    const std::string s = lower(fileName);
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".png") == 0 &&
           s.find("toon") == std::string::npos && s.find("sphere") == std::string::npos;
}

bool ResolveUpscaler(const std::string& model, std::string& exe, std::string& modelDir, std::string* err) {
#ifdef _WIN32
    const std::string which = normalizedModel(model);
    const char* wanted = which == "realesrgan" ? "realesrgan-ncnn-vulkan.exe" : "waifu2x-ncnn-vulkan.exe";
    std::string found;
    if (!exe.empty()) found = exe;
    if (found.empty()) {
        const char* env = std::getenv("CARDBRIDGE_UPSCALER");
        if (env && *env) found = env;
    }
    if (found.empty()) found = std::string("tools/upscaler/") + wanted;
    if (found.empty() || !exists(found)) found = searchPath(wanted);
    if (found.empty() || !exists(found)) {
        if (err) *err = std::string(wanted) + " not found (pass --upscaler <exe>, set CARDBRIDGE_UPSCALER, "
                        "or put it in tools/upscaler/ - see docs/TEXTURE_QUALITY_AND_RESOLUTION.md)";
        return false;
    }
    exe = found;
    const size_t slash = found.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : found.substr(0, slash);
    if (which == "realesrgan") {
        modelDir = dir + "/models-realesrgan-anime";
        if (!exists(modelDir + "/realesrgan-x4plus-anime.param")) {
            if (err) *err = "realesrgan-x4plus-anime model not found beside " + found;
            return false;
        }
    } else {
        std::string m = dir + "/models-upconv_7_anime_style_art_rgb";
        if (!exists(m)) m = dir + "/models-cunet";
        if (!exists(m)) {
            if (err) *err = "waifu2x models not found beside " + found;
            return false;
        }
        modelDir = m;
    }
    return true;
#else
    (void)model; (void)exe; (void)modelDir;
    if (err) *err = "texture upscaling requires Windows (Vulkan upscaler)";
    return false;
#endif
}

bool UpscalePngInPlace(const std::string& model, const std::string& exe, const std::string& modelDir,
                       const std::string& pngPath, int scale, std::string* err) {
#ifdef _WIN32
    if (scale < 2 || scale > 8) { if (err) *err = "upscale factor must be 2..8"; return false; }
    const bool esrgan = normalizedModel(model) == "realesrgan";
    const std::string prepared = esrgan ? pngPath + ".prepared.png" : pngPath;
    if (esrgan && !FillTransparentRgb(pngPath, prepared)) {
        if (err) *err = "could not prepare the cutout atlas for Real-ESRGAN";
        fileio::RemoveFile(prepared);
        return false;
    }
    const std::string tmp = pngPath + ".upscaled.png";
    const std::string args = esrgan
        ? " -n realesrgan-x4plus-anime -m \"" + modelDir + "\""
        : " -n -1 -m \"" + modelDir + "\"";
    const std::string cmd = "\"" + exe + "\" -i \"" + prepared + "\" -o \"" + tmp +
                            "\" -s " + std::to_string(scale) + args;
    STARTUPINFOA si{}; PROCESS_INFORMATION pi{};
    si.cb = sizeof si;
    std::string mutableCmd = cmd;
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        if (err) *err = "could not start the upscaler";
        if (esrgan) fileio::RemoveFile(prepared);
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (esrgan) fileio::RemoveFile(prepared);
    if (code != 0 || !exists(tmp)) {
        if (err) *err = "upscaler failed (exit " + std::to_string(code) + ")";
        fileio::RemoveFile(tmp);
        return false;
    }
    fileio::RemoveFile(pngPath);
    if (std::rename(tmp.c_str(), pngPath.c_str()) != 0) {
        if (err) *err = "could not replace the upscaled PNG";
        return false;
    }
    return true;
#else
    (void)model; (void)exe; (void)modelDir; (void)pngPath; (void)scale;
    if (err) *err = "texture upscaling requires Windows";
    return false;
#endif
}

} // namespace exporter
} // namespace cardbridge
