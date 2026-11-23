#include "export/TextureUpscaler.h"

#include "core/FileIo.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

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

#ifdef _WIN32
std::string searchPath(const std::string& name) {
    char buf[MAX_PATH] = {};
    const DWORD n = SearchPathA(nullptr, name.c_str(), nullptr, sizeof buf, buf, nullptr);
    return n > 0 && n < sizeof buf ? std::string(buf) : std::string();
}
#endif

} // namespace

bool IsUpscalableTexture(const std::string& fileName) {
    const std::string s = lower(fileName);
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".png") == 0 &&
           s.find("toon") == std::string::npos && s.find("sphere") == std::string::npos;
}

bool ResolveUpscaler(std::string& exe, std::string& modelDir, std::string* err) {
#ifdef _WIN32
    std::string found;
    if (!exe.empty()) found = exe;
    if (found.empty()) {
        const char* env = std::getenv("CARDBRIDGE_UPSCALER");
        if (env && *env) found = env;
    }
    if (found.empty()) found = "tools/upscaler/waifu2x-ncnn-vulkan.exe";
    if (!exists(found)) found = searchPath("waifu2x-ncnn-vulkan.exe");
    if (found.empty() || !exists(found)) {
        if (err) *err = "waifu2x-ncnn-vulkan.exe not found (pass --upscaler <exe>, set CARDBRIDGE_UPSCALER, "
                        "or put it in tools/upscaler/ - see docs/TEXTURE_QUALITY_AND_RESOLUTION.md)";
        return false;
    }
    exe = found;
    const size_t slash = found.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string(".") : found.substr(0, slash);
    std::string model = dir + "/models-upconv_7_anime_style_art_rgb";
    if (!exists(model)) model = dir + "/models-cunet";
    if (!exists(model)) {
        if (err) *err = "waifu2x models not found beside " + found + " (expected models-upconv_7_anime_style_art_rgb or models-cunet)";
        return false;
    }
    modelDir = model;
    return true;
#else
    if (err) *err = "texture upscaling requires Windows (waifu2x-ncnn-vulkan)";
    return false;
#endif
}

bool UpscalePngInPlace(const std::string& exe, const std::string& modelDir, const std::string& pngPath,
                       int scale, std::string* err) {
#ifdef _WIN32
    if (scale < 2 || scale > 8) { if (err) *err = "upscale factor must be 2..8"; return false; }
    const std::string tmp = pngPath + ".upscaled.png";
    const std::string cmd = "\"" + exe + "\" -i \"" + pngPath + "\" -o \"" + tmp +
                            "\" -s " + std::to_string(scale) + " -n -1 -m \"" + modelDir + "\"";
    STARTUPINFOA si{}; PROCESS_INFORMATION pi{};
    si.cb = sizeof si;
    std::string mutableCmd = cmd;
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        if (err) *err = "could not start the upscaler";
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0 || !exists(tmp)) {
        if (err) *err = "waifu2x failed (exit " + std::to_string(code) + ")";
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
    (void)exe; (void)modelDir; (void)pngPath; (void)scale;
    if (err) *err = "texture upscaling requires Windows";
    return false;
#endif
}

} // namespace exporter
} // namespace cardbridge
