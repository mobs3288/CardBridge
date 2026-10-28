#pragma once

#include "shader/Dxbc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::shader {

// An interpreter for Direct3D 10/11 Shader Model 4 programs (the DXBC token stream), enough for every Koikatsu / Shader Forge
// pixel and vertex program: float and the few integer / bit operations they use, sampling, discard and if/else. It runs one
// invocation at a time and is exact to the bytecode (no recompiling, no approximation of what the shader does).

struct Sm4Operand {
    uint8_t type = 0;          // D3D10_SB_OPERAND_TYPE: 0 temp, 1 input, 2 output, 4 immediate, 8 constant buffer ...
    uint8_t selMode = 0;       // 0 mask, 1 swizzle, 2 select one component
    uint8_t mask = 0xF;        // write mask (destinations)
    uint8_t swz[4] = {0, 1, 2, 3};
    uint8_t modifier = 0;      // 0 none, 1 negate, 2 abs, 3 negated abs
    uint8_t numComponents = 4; // 1 or 4 (immediates)
    int32_t index0 = 0, index1 = 0;
    int32_t relReg = -1, relComp = 0;   // register-relative index (temp rN.c), or -1
    uint32_t imm[4] = {0, 0, 0, 0};
};

struct Sm4Instr {
    uint16_t op = 0;
    bool saturate = false;
    bool testNonZero = true;   // if_nz / if_z, discard_nz / discard_z
    std::vector<Sm4Operand> operands;
    int jump = -1;             // for if: index of the matching else/endif; for else: index of its endif
};

struct Sm4Program {
    std::vector<Sm4Instr> code;
    uint32_t tempCount = 0;
    uint32_t major = 4, minor = 0;
    bool isPixel = true;
};

bool DecodeSm4(const std::vector<uint32_t>& tokens, Sm4Program& out, std::string* why);

// Texture access for the program: coordinates are the shader's own (already scaled), results are RGBA.
struct ISm4Textures {
    virtual ~ISm4Textures() = default;
    // lod < 0 means "pick from derivatives" (the caller decides how); cmp is the reference for sample_c.
    virtual void Sample(int texture, int sampler, const float coord[4], float lod, float out[4]) = 0;
};

struct Sm4Io {
    float input[32][4] = {};
    float output[32][4] = {};
    // Constant buffers by slot, each a flat array of float4 elements (size / 16 entries).
    std::vector<const float*> constantBuffers;
    ISm4Textures* textures = nullptr;
    bool frontFace = true;      // SV_IsFrontFace input is read as an integer (0xFFFFFFFF front) through input[reg].x
    bool discarded = false;
    bool trace = false;         // print every instruction's destination (debugging)
};

// Runs a decoded program once. Returns false (with `why`) on an operation the interpreter does not know.
bool RunSm4(const Sm4Program& prog, Sm4Io& io, std::string* why);

} // namespace cardbridge::shader
