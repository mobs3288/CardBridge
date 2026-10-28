#pragma once

#include "core/Bytes.h"
#include "shader/Dxbc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::shader {

// One compiled sub-program of a Unity 5.6 (DX11) shader, as stored in the Shader asset's program blob: the DXBC code plus
// what Unity writes around it - the keywords it was compiled for and the binding tables that name the constant-buffer
// variables, textures and samplers (the DXBC itself has no reflection data).
struct CbParam {
    std::string name;
    uint32_t offset = 0;     // bytes from the start of the constant buffer
    uint32_t rows = 1, cols = 4;
    bool isMatrix = false;
};
struct CbLayout {
    std::string name;
    uint32_t size = 0;       // bytes
    std::vector<CbParam> params;
    int slot = -1;           // constant-buffer register (cbN)
};
struct TextureBinding {
    std::string name;        // Unity property name, e.g. "_MainTex"
    uint32_t reg = 0;        // t register
    uint32_t sampler = 0;    // s register
    uint32_t dimension = 2;  // 2 = 2D, 4 = 3D, 5 = cube
};

struct UnityProgram {
    Dxbc code;
    std::vector<std::string> keywords;
    std::vector<CbLayout> cbuffers;
    std::vector<TextureBinding> textures;

    bool isPixel() const { return code.programType == 0; }
    bool isVertex() const { return code.programType == 1; }
    bool hasKeyword(const std::string& k) const {
        for (const auto& s : keywords) if (s == k) return true;
        return false;
    }
    const CbLayout* cbufferBySlot(int slot) const {
        for (const auto& c : cbuffers) if (c.slot == slot) return &c;
        return nullptr;
    }
};

// Splits the decompressed program blob of a Shader asset (see tools/extract_shaders.py) into its programs. Programs that
// cannot be parsed are skipped; `why` collects the reasons.
std::vector<UnityProgram> ParseUnityShaderBlob(const Bytes& blob, std::vector<std::string>* why = nullptr);

// Picks the main forward pass of a shader: the largest pixel program that needs no shadow-map input, and the vertex program
// whose outputs feed it. Returns false when the blob has no such pair.
bool PickForwardPass(const std::vector<UnityProgram>& programs, size_t& pixelIndex, size_t& vertexIndex);

} // namespace cardbridge::shader
