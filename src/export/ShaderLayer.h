#pragma once

#include <string>
#include <vector>

namespace cardbridge::exporter {

// An extra shading layer drawn by the Direct3D viewer on top of the game's own shaders (which always run). A layer is one HLSL file
// in a layers folder, so another shader is added by dropping a file there. The file starts with comment lines that describe it:
//
//     // layer: Rim light            the name shown in the viewer's Look tab
//     // blend: add                  add | multiply | alpha: how its colour is combined with what the game's shader drew
//     // default: off                on | off: the state when the viewer starts
//
// and defines `float4 ps(VO i) : SV_Target`. The prelude (ShaderLayerPrelude) is put in front of it: it declares the frame constants
// (the matrix, the light, the view direction, the material's colours, the time), the material's main texture `mainTex` with sampler `samp`,
// the vertex output `VO` (clip position, world position, world normal, uv) and the vertex shader. Layers are not exported to PMX.
struct ShaderLayer {
    std::string name, file, blend = "add";
    bool defaultOn = false;
    std::string source;    // the file's text
};

// Every *.hlsl in the given folders (missing folders are skipped), in file-name order.
std::vector<ShaderLayer> LoadShaderLayers(const std::vector<std::string>& folders);

// The HLSL that goes in front of a layer's file (exposed so the docs and tests can show it).
const char* ShaderLayerPrelude();

} // namespace cardbridge::exporter
