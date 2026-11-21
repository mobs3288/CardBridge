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
// KKS resource records use this as the sampler word when a texture has no sampler state of its own (a cube map that
// reuses another binding's sampler). It is a sentinel, not a shader register: passing it to PSSetSamplers is an
// out-of-range access in the D3D11 driver.
constexpr uint32_t kNoSampler = 0xFFFFFFFFu;

struct TextureBinding {
    std::string name;        // Unity property name, e.g. "_MainTex"
    uint32_t reg = 0;        // t register
    uint32_t sampler = 0;    // s register (meaningless when hasSampler is false)
    uint32_t dimension = 2;  // 2 = 2D, 4 = 3D, 5 = cube
    bool hasSampler = true;  // false = the shader record carries no sampler; keep whatever sampler is already bound
};

// D3D11 caps (D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT / _SAMPLER_SLOT_COUNT) without making the shader module
// depend on a graphics header. The GPU view still validates against the real D3D11 constants before binding.
inline bool ValidTextureRegister(const TextureBinding& tb) { return tb.reg < 128u; }
inline bool ValidSamplerRegister(const TextureBinding& tb) { return !tb.hasSampler || tb.sampler < 16u; }
inline bool ValidTextureBinding(const TextureBinding& tb) { return ValidTextureRegister(tb) && ValidSamplerRegister(tb); }

struct UnityProgram {
    Dxbc code;
    Bytes dxbc;              // the container itself, as Direct3D 11 takes it
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
