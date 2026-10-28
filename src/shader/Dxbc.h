#pragma once

#include "core/Bytes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::shader {

// A compiled Direct3D 10/11 shader (DXBC container), the form Koikatsu's Shader Forge shaders (and any mod shader built
// for Unity 5.6 on DX11) ship in. Only what an interpreter needs: the constant-buffer layout with variable names (so
// Unity material properties can be bound by name), the texture/sampler bindings, the in/out signatures and the token
// stream of the program.
struct CbVariable {
    std::string name;
    uint32_t offset = 0;   // bytes from the start of the constant buffer
    uint32_t size = 0;
};
struct ConstantBuffer {
    std::string name;
    uint32_t size = 0;     // bytes
    std::vector<CbVariable> vars;
};
struct ResourceBinding {
    std::string name;
    uint32_t type = 0;     // D3D_SHADER_INPUT_TYPE: 0 cbuffer, 2 texture, 3 sampler
    uint32_t bindPoint = 0;
    uint32_t dimension = 0;
};
struct SignatureElement {
    std::string semantic;
    uint32_t index = 0;
    uint32_t reg = 0;
    uint32_t mask = 0;     // xyzw bits
    uint32_t systemValue = 0;
};

struct Dxbc {
    uint32_t programType = 0;                // 0 pixel, 1 vertex, 2 geometry ...
    uint32_t major = 0, minor = 0;
    std::vector<ConstantBuffer> cbuffers;
    std::vector<ResourceBinding> resources;
    std::vector<SignatureElement> inputs, outputs;
    std::vector<uint32_t> tokens;            // SHDR/SHEX: version token, length token, then the instruction tokens

    const ConstantBuffer* cbuffer(const std::string& name) const {
        for (const auto& c : cbuffers) if (c.name == name) return &c;
        return nullptr;
    }
};

// Parses one container. Returns false (with `why`) when the bytes are not a DXBC container or lack the program chunk.
bool ParseDxbc(const uint8_t* data, size_t size, Dxbc& out, std::string* why);

// Splits a Unity shader sub-program blob into its DXBC containers.
std::vector<Bytes> CarveDxbc(const Bytes& blob);

} // namespace cardbridge::shader
