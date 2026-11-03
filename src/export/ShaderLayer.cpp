#include "export/ShaderLayer.h"

#include "core/FileIo.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace cardbridge::exporter {

const char* ShaderLayerPrelude() {
    return R"HLSL(
cbuffer Frame : register(b0) {
    float4 r0, r1, r2, r3;   // world -> clip, row by row
    float4 lightDir;         // xyz: unit vector towards the light, world space
    float4 view;             // xyz: unit vector towards the camera, world space (the camera is orthographic: the same for every pixel)
    float4 diffuse;          // the material's colour (linear when info.y is 1), w = alpha
    float4 shadow;           // the material's shade colour
    float4 info;             // x = seconds, y = 1 when the frame is written in linear light
};
Texture2D mainTex : register(t0);
SamplerState samp : register(s0);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_POSITION; float3 world : TEXCOORD2; float3 n : TEXCOORD1; float2 uv : TEXCOORD0; };
VO vs(VI i) {
    VO o;
    float4 p = float4(i.p, 1);
    o.pos = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p));
    o.pos.z -= 3e-4 * o.pos.w;   // a hair in front of the surface the game's shader drew
    o.world = i.p;
    o.n = i.n;
    o.uv = i.uv;
    return o;
}
)HLSL";
}

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    return s.substr(a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<ShaderLayer> LoadShaderLayers(const std::vector<std::string>& folders) {
    std::vector<ShaderLayer> out;
    std::set<std::string> seen;
    for (const std::string& folder : folders) {
        if (folder.empty() || !fileio::Exists(folder)) continue;
        std::vector<std::string> files = fileio::ListFiles(folder, {".hlsl"});
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) {
            const std::string file = fileio::FileName(f);
            if (!seen.insert(lower(file)).second) continue;   // an earlier folder wins
            Bytes raw = fileio::ReadFile(f);
            ShaderLayer layer;
            layer.file = file;
            layer.name = fileio::Stem(f);
            layer.source.assign(raw.begin(), raw.end());
            // header comments: "// key: value", read until the first line that is neither a comment nor blank
            size_t at = 0;
            while (at < layer.source.size()) {
                size_t end = layer.source.find('\n', at);
                if (end == std::string::npos) end = layer.source.size();
                std::string line = trim(layer.source.substr(at, end - at));
                at = end + 1;
                if (line.empty()) continue;
                if (line.rfind("//", 0) != 0) break;
                line = trim(line.substr(2));
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
                if (key == "layer" && !value.empty()) layer.name = value;
                else if (key == "blend") { value = lower(value); if (value == "add" || value == "multiply" || value == "alpha") layer.blend = value; }
                else if (key == "default") layer.defaultOn = lower(value) == "on";
            }
            out.push_back(std::move(layer));
        }
    }
    return out;
}

} // namespace cardbridge::exporter
