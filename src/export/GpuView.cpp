#include "export/GpuView.h"

#include "export/AdapterSelect.h"
#include "export/ColourGrade.h"
#include "export/Exporter.h"

#include "shader/UnityShader.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <tuple>

#include <windows.h>

namespace cardbridge::exporter {

namespace detail {   // Preview.cpp: the constant-buffer filler and tangents the software renderer uses too
void FillShaderConstants(const shader::UnityProgram& prog, const assemble::SceneMaterial& mat, const float lightDir[3], const float camPos[3],
                         std::vector<std::vector<float>>& data, const std::vector<std::pair<std::string, Vec4>>* globals);
std::vector<Vec3> ComputeMeshTangents(const assemble::SceneMesh& m);
bool LinearLight();
float ToLinear(float c);
assemble::SceneMaterial LinearColours(const assemble::SceneMaterial& mat);
int RenderQueueOf(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
bool IsOverlayMaterial(const assemble::Scene& scene, const assemble::SceneMaterial& mat);
}
using detail::LinearLight;
using detail::ToLinear;

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct GVert {
    float pos[3], nrm[3], tan[4];
    float uv[2], col[4], uv1[2], uv2[2], uv3[2];
    int boneIdx[4];
    float boneW[4];
};   // 120 bytes; boneIdx/boneW are read only by the skinning compute shader, the game shaders never see them

struct GpuSub { ID3D11Buffer* ib = nullptr; UINT count = 0; int material = 0; };
struct GpuMesh {
    ID3D11Buffer* vb = nullptr;        // the buffer drawn (rest when there is no pose, posed when there is); a borrowed plain vertex buffer
    ID3D11Buffer* restBuf = nullptr;   // plain vertex buffer holding the rest vertices, drawn when there is no pose
    ID3D11Buffer* posedBuf = nullptr;  // plain vertex buffer holding the skinned copy, drawn when there is a pose
    ID3D11Buffer* skinInBuf = nullptr;  // structured copy of the rest vertices the compute shader reads
    ID3D11Buffer* skinOutBuf = nullptr; // structured buffer the compute shader writes, copied into posedBuf before drawing
    ID3D11ShaderResourceView* restSrv = nullptr;
    ID3D11UnorderedAccessView* posedUav = nullptr;
    std::vector<GpuSub> subs;
    std::vector<GVert> rest;   // the vertices as loaded, for re-skinning
};

struct GpuShader {
    std::vector<shader::UnityProgram> progs;
    size_t pi = 0, vi = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    bool ok = false;
};

struct GpuTex { ID3D11ShaderResourceView* srv = nullptr; ID3D11Texture2D* tex = nullptr; };

UINT Round16(UINT n) { return (n + 15u) & ~15u; }

} // namespace

struct GpuView::Impl {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    GpuKind kind = GpuKind::Software;   // set by Init: hardware GPU, WARP (CPU) or software (no device)
    std::string adapterName;            // what Init created the device on
    std::string wantSkin = "gpu";       // "gpu" asks for the compute shader, "cpu" forces the CPU skin, "auto" picks per mesh
    size_t skinThresholdVerts = 0;      // "auto" mode: meshes with fewer vertices use the CPU loop
    // Skinning compute shader: rest vertices + per-bone matrices -> posed vertices, all on the GPU. When it is absent
    // (no d3dcompiler, a feature level below 11, or the user asked for the CPU skin) SetSkin keeps its CPU loop and the
    // whole view still works; same picture, slower pose edits.
    ID3D11ComputeShader* skCs = nullptr;
    ID3D11Buffer* skBones = nullptr;    // structured buffer of bone matrix rows (4 float4 per bone)
    ID3D11ShaderResourceView* skBonesSrv = nullptr;
    bool skinOnGpu = false;
    // Benchmark timing queries (created lazily, used only when SetBenchQueries(true)).
    // Timestamp/disjoint give pure GPU compute time when the driver reports a
    // non-zero frequency; otherwise an event query gives GPU completion time
    // (submission + GPU, waited for, never submission alone).
    bool benchQueries = false;
    ID3D11Query* benchTsStart = nullptr;
    ID3D11Query* benchTsEnd = nullptr;
    ID3D11Query* benchTsDisjoint = nullptr;
    ID3D11Query* benchEvent = nullptr;
    mutable double benchLastMs = -1.0;
    mutable bool benchPending = false;
    mutable LARGE_INTEGER benchStartQpc{};
    const assemble::Scene* scene = nullptr;
    std::vector<GpuMesh> meshes;
    std::vector<GpuShader> shaders;
    std::vector<GpuTex> textures;                // by scene texture
    GpuTex defaults[4];                          // white, black, gray, bump
    ID3D11SamplerState* samplerWrap = nullptr;
    ID3D11SamplerState* samplerClamp = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11RasterizerState* rasterCull = nullptr;   // back faces culled: what the game and MikuMikuDance do for a single-sided material
    ID3D11DepthStencilState* depth = nullptr;
    ID3D11DepthStencilState* depthOverlay = nullptr;   // depth test without write, for the highlight tint
    ID3D11BlendState* blend = nullptr;
    ID3D11BlendState* blendOverlay = nullptr;          // ordinary src-alpha blending, for the highlight
    // Per-frame constant buffers, by (material, stage, slot).
    std::map<std::tuple<int, int, int>, ID3D11Buffer*> cbufs;
    // Render targets, recreated on a size change.
    int rw = 0, rh = 0;
    ID3D11Texture2D *msaa = nullptr, *resolve = nullptr, *staging = nullptr, *depthTex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int skipped = 0;
    std::vector<Mat4> skinCopy;    // the skin SetSkin was last given (empty = rest), kept for Refresh
    float lastM[16] = {};          // world -> clip of the last Render, for Project()
    int lastW = 0, lastH = 0;
    bool plain = false;                          // see GpuView::SetPlain
    std::vector<MmdLook> looks;                  // plain mode: each scene material as the PMX writes it (made on the first plain Render)
    std::vector<char> cutout;                    // per scene material: cut to a shape by its texture (HasCutout), so no outline ring
    std::map<std::string, GpuTex> spheres;       // plain mode: the looks' sphere maps, by MmdSphereImage name
    std::vector<ID3D11ShaderResourceView*> lookSphere;   // per look: its sphere map's view (owned by `spheres`), null for none
    // Plain mode's self-shadow map, made on first use.
    static constexpr int kShadowSize = 2048;
    ID3D11Texture2D* smTex = nullptr;
    ID3D11DepthStencilView* smDsv = nullptr;
    ID3D11ShaderResourceView* smSrv = nullptr;
    ID3D11SamplerState* smCmp = nullptr;

    bool MakeShadowMap() {
        if (smSrv) return true;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = kShadowSize; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_TYPELESS; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &smTex))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;   // outside the map: lit
        if (FAILED(dev->CreateDepthStencilView(smTex, &dv, &smDsv)) || FAILED(dev->CreateShaderResourceView(smTex, &sv, &smSrv)) ||
            FAILED(dev->CreateSamplerState(&sd, &smCmp))) { Rel(smDsv); Rel(smSrv); Rel(smCmp); Rel(smTex); return false; }
        return true;
    }
    // A plain textured, toon-lit shader for materials whose game shader is not in the bundle; compiled at run time by the
    // system's d3dcompiler (absent: such materials are left out).
    ID3D11VertexShader* fbVs = nullptr;
    ID3D11PixelShader* fbPs = nullptr;
    ID3D11InputLayout* fbIl = nullptr;
    ID3D11Buffer* fbCb = nullptr;
    // Outline: the material's silhouette ring (_LineWidthS, _LineColor) drawn as a copy of the mesh pushed out along its normals in
    // screen space and slightly back in depth, so only the part beyond the surface shows.
    ID3D11VertexShader* olVs = nullptr;
    ID3D11PixelShader* olPs = nullptr;
    ID3D11InputLayout* olIl = nullptr;
    ID3D11Buffer* olCb = nullptr;
    // Extra shading layers (ShaderLayer.h).
    struct Layer {
        bool ok = false, enabled = false;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ID3D11InputLayout* il = nullptr;
        ID3D11BlendState* blend = nullptr;
        ID3D11Buffer* cb = nullptr;
    };
    std::vector<Layer> layers;

    void ClearLayers() {
        for (auto& l : layers) { Rel(l.vs); Rel(l.ps); Rel(l.il); Rel(l.blend); Rel(l.cb); }
        layers.clear();
    }

    bool MakeOutline() {
        typedef HRESULT(WINAPI * CompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!dll) return false;
        auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")));
        if (!compile) return false;
        static const char src[] = R"HLSL(
cbuffer C : register(b0) { float4 r0, r1, r2, r3, color, param; };   // param: ring half width in clip units (x, y), depth push (z)
struct VI { float3 p : POSITION; float3 n : NORMAL; };
float4 vs(VI i) : SV_Position {
    float4 p = float4(i.p, 1);
    float4 o = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p));
    float2 n = float2(dot(r0.xyz, i.n), dot(r1.xyz, i.n));
    float l = length(n);
    if (l > 1e-5) o.xy += n / l * param.xy * o.w;
    o.z += param.z * o.w;
    return o;
}
float4 ps() : SV_Target { return color; }
)HLSL";
        ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
        if (FAILED(compile(src, sizeof(src) - 1, "outline", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, &err)) || !vb) { Rel(err); return false; }
        if (FAILED(compile(src, sizeof(src) - 1, "outline", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, &err)) || !pb) { Rel(vb); Rel(err); return false; }
        bool ok = SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &olVs)) &&
                  SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &olPs));
        const D3D11_INPUT_ELEMENT_DESC el[2] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ok = ok && SUCCEEDED(dev->CreateInputLayout(el, 2, vb->GetBufferPointer(), vb->GetBufferSize(), &olIl));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 6 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &olCb));
        Rel(vb); Rel(pb); Rel(err);
        return ok;
    }

    bool MakeFallback() {
        typedef HRESULT(WINAPI * CompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!dll) return false;
        auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")));
        if (!compile) return false;
        static const char src[] = R"HLSL(
cbuffer C : register(b0) { float4 r0, r1, r2, r3, diffuse, shadow, light; float4 ramp[32]; float4 l0, l1, l2, mmd, sph; };   // shadow.w = 2: shade through ramp (a material's toon ramp, top = lit)
// sph.x: the PMX sphere map in t2, 1 multiplies, 2 adds (looked up by the normal's screen x and y, as MMD does).
// MikuMikuDance's look (the "as exported" view): mmd.x = 1 the material takes self-shadow (l0..l2: world -> shadow map, z = depth),
// mmd.y = 1 the ground shadow pass (the mesh flattened along the light onto the floor at height mmd.z, drawn in `diffuse`), mmd.w the model's radius.
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
Texture2D<float> sm : register(t1); SamplerComparisonState sc : register(s1);
Texture2D t2 : register(t2);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_POSITION; float3 n : TEXCOORD1; float2 uv : TEXCOORD0; float3 w : TEXCOORD2; float2 vn : TEXCOORD3; };
VO vs(VI i) {
    VO o; float4 p = float4(i.p, 1);
    if (mmd.y > 0.5) p.xyz -= light.xyz * ((p.y - mmd.z) / max(light.y, 0.05));
    o.pos = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p)); o.n = i.n; o.uv = i.uv; o.w = p.xyz;
    o.vn = float2(dot(normalize(r0.xyz), i.n), dot(normalize(r1.xyz), i.n));
    return o;
}
float4 ps(VO i) : SV_Target {
    if (mmd.y > 0.5) return diffuse;
    float4 c = t0.Sample(s0, i.uv) * diffuse;
    if (sph.x > 0.5) {
        float2 vn = i.vn / max(length(i.vn), 1.0);
        float3 s = t2.Sample(s0, float2(vn.x * 0.5 + 0.5, 0.5 - vn.y * 0.5)).rgb;
        c.rgb = sph.x > 1.5 ? c.rgb + s : c.rgb * s;
    }
    float d = dot(normalize(i.n), light.xyz);
    if (shadow.w > 1.5 && mmd.x > 0.5) {   // MMD's self-shadow shading: the lit colour, or it times the ramp's bottom row, by N.L * 3 and the shadow map
        float4 w = float4(i.w + normalize(i.n) * 0.006 * mmd.w, 1);
        float3 lp = float3(dot(l0, w), dot(l1, w), dot(l2, w));
        float vis = sm.SampleCmpLevelZero(sc, float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5), lp.z - 0.0015);
        c.rgb *= lerp(ramp[31].rgb, float3(1, 1, 1), min(saturate(d * 3.0), vis));
        return c;
    }
    if (shadow.w > 1.5) {
        float f = saturate(0.5 - 0.5 * d) * 32.0 - 0.5;
        int a = clamp((int)floor(f), 0, 31), b = min(a + 1, 31);
        c.rgb *= lerp(ramp[a].rgb, ramp[b].rgb, saturate(f - a));
        return c;
    }
    float lit = saturate((saturate(d) - 0.1) / 0.3);
    c.rgb *= lerp(shadow.rgb, float3(1, 1, 1), lit);
    return c;
}
)HLSL";
        ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
        if (FAILED(compile(src, sizeof(src) - 1, "fallback", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, &err)) || !vb) { Rel(err); return false; }
        if (FAILED(compile(src, sizeof(src) - 1, "fallback", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, &err)) || !pb) { Rel(vb); Rel(err); return false; }
        bool ok = SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &fbVs)) &&
                  SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &fbPs));
        const D3D11_INPUT_ELEMENT_DESC el[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GVert, uv), D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ok = ok && SUCCEEDED(dev->CreateInputLayout(el, 3, vb->GetBufferPointer(), vb->GetBufferSize(), &fbIl));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (7 + 32 + 5) * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &fbCb));
        Rel(vb); Rel(pb); Rel(err);
        return ok;
    }

    // Skinning compute shader: the pose lives in a structured buffer of bone matrix rows (4 float4 per bone, row-major as
    // Mat4 stores them), the rest vertices in a structured buffer, and one thread per vertex blends its four bones and
    // writes the posed vertex. The game shaders never see boneIdx/boneW, so the input layouts do not change.
    bool MakeSkinCompute() {
        if (skCs) return true;
        typedef HRESULT(WINAPI * CompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!dll) return false;
        auto compile = reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile")));
        if (!compile) return false;
        static const char src[] = R"HLSL(
struct SVVert { float3 pos; float3 nrm; float4 tan; float2 uv; float4 col; float2 uv1; float2 uv2; float2 uv3; int4 boneIdx; float4 boneW; };
StructuredBuffer<SVVert> g_in : register(t0);
StructuredBuffer<float4> g_bones : register(t1);
RWStructuredBuffer<SVVert> g_out : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint count, stride; g_in.GetDimensions(count, stride);
    if (dtid.x >= count) return;
    SVVert v = g_in[dtid.x];
    uint boneRows, boneStride; g_bones.GetDimensions(boneRows, boneStride);
    uint numBones = boneRows / 4u;
    float3 p = 0, n = 0, t = 0;
    float sum = 0;
    for (int k = 0; k < 4; ++k) {
        float w = v.boneW[k];
        int b = v.boneIdx[k];
        if (b < 0 || (uint)b >= numBones) continue;
        if (!(w > 0.0) || isinf(w) || isnan(w)) continue;
        uint r = (uint)b * 4u;
        float4 r0 = g_bones[r], r1 = g_bones[r + 1], r2 = g_bones[r + 2];
        p += w * float3(dot(r0, float4(v.pos, 1)), dot(r1, float4(v.pos, 1)), dot(r2, float4(v.pos, 1)));
        n += w * float3(dot(r0.xyz, v.nrm), dot(r1.xyz, v.nrm), dot(r2.xyz, v.nrm));
        t += w * float3(dot(r0.xyz, v.tan.xyz), dot(r1.xyz, v.tan.xyz), dot(r2.xyz, v.tan.xyz));
        sum += w;
    }
    if (sum > 1e-6 && isfinite(sum)) {
        float3 pp = p / sum;
        if (isfinite(pp.x) && isfinite(pp.y) && isfinite(pp.z)) {
            v.pos = pp;
            float nl = length(n);
            if (nl > 1e-8 && isfinite(nl)) { float3 nn = n / nl; if (isfinite(nn.x) && isfinite(nn.y) && isfinite(nn.z)) v.nrm = nn; }
            float tl = length(t);
            if (tl > 1e-8 && isfinite(tl)) { float3 tt = t / tl; if (isfinite(tt.x) && isfinite(tt.y) && isfinite(tt.z)) v.tan.xyz = tt; }
        }
    }
    g_out[dtid.x] = v;
}
)HLSL";
        ID3DBlob* cb = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT chr = compile(src, sizeof(src) - 1, "skinning", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cb, &err);
        if (FAILED(chr) || !cb) {
            if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "skin cs compile hr=%lx %s\n", (long)chr, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            Rel(cb); Rel(err); return false;
        }
        bool ok = SUCCEEDED(dev->CreateComputeShader(cb->GetBufferPointer(), cb->GetBufferSize(), nullptr, &skCs));
        if (!ok && std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "skin cs create failed\n");
        Rel(cb); Rel(err);
        if (!ok) { Rel(skCs); return false; }
        return true;
    }

    // The bone matrices as rows, for the skinning shader. Recreated when the bone count changes.
    bool UploadBones(const std::vector<Mat4>* skin) {
        if (!skin || skin->empty()) return true;
        const size_t bytes = skin->size() * 4 * 16;
        if (!skBones || skBonesBytes != bytes) {
            Rel(skBonesSrv); Rel(skBones);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(bytes); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &skBones))) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; sv.BufferEx.NumElements = static_cast<UINT>(skin->size() * 4);
            if (FAILED(dev->CreateShaderResourceView(skBones, &sv, &skBonesSrv))) { Rel(skBones); return false; }
            skBonesBytes = bytes;
        }
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(ctx->Map(skBones, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return false;
        float* rows = static_cast<float*>(mp.pData);
        for (const auto& m : *skin) for (int i = 0; i < 16; ++i) *rows++ = m.m[i];
        ctx->Unmap(skBones, 0);
        return true;
    }
    size_t skBonesBytes = 0;

    bool EnsureBenchQueries() {
        if (benchTsStart) return true;
        D3D11_QUERY_DESC ts{};
        ts.Query = D3D11_QUERY_TIMESTAMP;
        D3D11_QUERY_DESC dj{};
        dj.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC ev{};
        ev.Query = D3D11_QUERY_EVENT;
        const HRESULT h1 = dev->CreateQuery(&ts, &benchTsStart);
        const HRESULT h2 = h1 == S_OK ? dev->CreateQuery(&ts, &benchTsEnd) : h1;
        const HRESULT h3 = h2 == S_OK ? dev->CreateQuery(&dj, &benchTsDisjoint) : h2;
        const HRESULT h4 = h3 == S_OK ? dev->CreateQuery(&ev, &benchEvent) : h3;
        if (FAILED(h1) || FAILED(h2) || FAILED(h3) || FAILED(h4)) {
            if (std::getenv("CARDBRIDGE_DEBUG_GPU")) std::fprintf(stderr, "bench queries create: %lx %lx %lx %lx\n", static_cast<long>(h1), static_cast<long>(h2), static_cast<long>(h3), static_cast<long>(h4));
            Rel(benchTsStart); Rel(benchTsEnd); Rel(benchTsDisjoint); Rel(benchEvent);
            return false;
        }
        return true;
    }
    bool DispatchSkin(GpuMesh& g) {
        if (!skCs || !g.restSrv || !g.posedUav || g.rest.empty()) return false;
        ID3D11ShaderResourceView* tv[2] = {g.restSrv, skBonesSrv};
        ID3D11UnorderedAccessView* tu[1] = {g.posedUav};
        const bool timed = benchQueries && EnsureBenchQueries();
        if (timed) {
            QueryPerformanceCounter(&benchStartQpc);
            ctx->End(benchTsDisjoint);
            ctx->End(benchTsStart);
        }
        ctx->CSSetShader(skCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, tv);
        ctx->CSSetUnorderedAccessViews(0, 1, tu, nullptr);
        ctx->Dispatch(static_cast<UINT>((g.rest.size() + 63) / 64), 1, 1);
        if (timed) { ctx->End(benchTsEnd); ctx->End(benchEvent); }
        ID3D11UnorderedAccessView* none[1] = {nullptr};
        ID3D11ShaderResourceView* nrv[2] = {nullptr, nullptr};
        ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
        ctx->CSSetShaderResources(0, 2, nrv);
        ctx->CSSetShader(nullptr, nullptr, 0);
        if (timed) benchPending = true;
        ctx->CopyResource(g.posedBuf, g.skinOutBuf);
        g.vb = g.posedBuf;
        return true;
    }

    void ReleaseTargets() { Rel(rtv); Rel(dsv); Rel(msaa); Rel(resolve); Rel(staging); Rel(depthTex); rw = rh = 0; }

    void ReleaseDevice() {
        ClearLayers();
        Rel(smDsv); Rel(smSrv); Rel(smCmp); Rel(smTex);
        Rel(fbVs); Rel(fbPs); Rel(fbIl); Rel(fbCb);
        Rel(olVs); Rel(olPs); Rel(olIl); Rel(olCb);
        Rel(skCs);
        Rel(benchTsStart); Rel(benchTsEnd); Rel(benchTsDisjoint); Rel(benchEvent);
        benchQueries = false; benchPending = false; benchLastMs = -1.0;
        Clear();
        Rel(samplerWrap); Rel(samplerClamp);
        Rel(raster); Rel(rasterCull);
        Rel(depth); Rel(depthOverlay);
        Rel(blend); Rel(blendOverlay);
        for (auto& d : defaults) { Rel(d.srv); Rel(d.tex); }
        ReleaseTargets();
        Rel(ctx); Rel(dev);
        scene = nullptr;
        skinCopy.clear();
        lastM[0] = 0;
        lastW = lastH = 0;
        kind = GpuKind::Software;
        adapterName = "software (CPU)";
    }

    ~Impl() { ReleaseDevice(); }

    void Clear() {
        // Quiesce queued skin dispatches / copies before releasing their buffers: without a flush the old card's
        // dispatches can still be in flight while the new card's Load reallocates the same buffers, drawing that
        // frame with garbage (the black/white leg rectangles after switching cards).
        if (dev && ctx) ctx->Flush();
        for (auto& m : meshes) {
            // vb borrows restBuf/posedBuf when the compute shader is in use; only a lone CPU vertex buffer owns itself.
            if (m.vb != m.restBuf && m.vb != m.posedBuf) Rel(m.vb);
            Rel(m.restBuf); Rel(m.posedBuf); Rel(m.skinInBuf); Rel(m.skinOutBuf); Rel(m.restSrv); Rel(m.posedUav); for (auto& s : m.subs) Rel(s.ib);
        }
        meshes.clear();
        Rel(skBonesSrv); Rel(skBones);
        skinOnGpu = false;
        for (auto& s : shaders) { Rel(s.vs); Rel(s.ps); Rel(s.il); }
        shaders.clear();
        for (auto& t : textures) { Rel(t.srv); Rel(t.tex); }
        textures.clear();
        for (auto& c : cbufs) Rel(c.second);
        cbufs.clear();
        looks.clear();
        for (auto& s : spheres) { Rel(s.second.srv); Rel(s.second.tex); }
        spheres.clear();
        lookSphere.clear();
        skipped = 0;
    }

    // srgb: the texels are sRGB-encoded colour data, so the sampler hands the shader linear values (and mips average in linear light).
    bool MakeTexture(int w, int h, const uint8_t* rgbaTopFirst, GpuTex& out, bool flip, bool srgb = false) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w); d.Height = static_cast<UINT>(h); d.MipLevels = 0; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &out.tex))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MostDetailedMip = 0; sv.Texture2D.MipLevels = static_cast<UINT>(-1);
        if (FAILED(dev->CreateShaderResourceView(out.tex, &sv, &out.srv))) return false;
        if (flip) {   // Unity's v runs up: the shaders expect the bottom row first
            std::vector<uint8_t> rows(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) std::memcpy(&rows[static_cast<size_t>(y) * w * 4], rgbaTopFirst + static_cast<size_t>(h - 1 - y) * w * 4, static_cast<size_t>(w) * 4);
            ctx->UpdateSubresource(out.tex, 0, nullptr, rows.data(), static_cast<UINT>(w) * 4, 0);
        } else {
            ctx->UpdateSubresource(out.tex, 0, nullptr, rgbaTopFirst, static_cast<UINT>(w) * 4, 0);
        }
        ctx->GenerateMips(out.srv);
        return true;
    }

    bool EnsureTargets(int w, int h) {
        if (w == rw && h == rh && rtv) return true;
        ReleaseTargets();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(w); d.Height = static_cast<UINT>(h); d.MipLevels = 1; d.ArraySize = 1;
        d.Format = LinearLight() ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &msaa))) return false;
        if (FAILED(dev->CreateRenderTargetView(msaa, nullptr, &rtv))) return false;
        D3D11_TEXTURE2D_DESC dd = d;
        dd.Format = DXGI_FORMAT_D32_FLOAT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&dd, nullptr, &depthTex))) return false;
        if (FAILED(dev->CreateDepthStencilView(depthTex, nullptr, &dsv))) return false;
        D3D11_TEXTURE2D_DESC rd = d;
        rd.SampleDesc.Count = 1;
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &resolve))) return false;
        rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&rd, nullptr, &staging))) return false;
        rw = w; rh = h;
        return true;
    }

    ID3D11Buffer* ConstantBuffer(int mat, int stage, int slot, UINT bytes) {
        auto key = std::make_tuple(mat, stage, slot);
        auto it = cbufs.find(key);
        if (it != cbufs.end()) return it->second;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = Round16(bytes); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* b = nullptr;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &b))) b = nullptr;
        cbufs[key] = b;
        return b;
    }

    // Texture bound to a shader's texture property for a material: the material's own, a global, else the shader's default.
    const GpuTex* TextureFor(const assemble::SceneMaterial& mat, const GpuShader& sh, const std::string& name, bool& clamp) {
        clamp = false;
        for (const auto& tb : mat.texProps)
            if (tb.name == name && tb.texture >= 0 && static_cast<size_t>(tb.texture) < textures.size() && textures[static_cast<size_t>(tb.texture)].srv) {
                clamp = scene->textures[static_cast<size_t>(tb.texture)].clamp;
                return &textures[static_cast<size_t>(tb.texture)];
            }
        for (const auto& tb : scene->globalTextures)
            if (tb.name == name && tb.texture >= 0 && static_cast<size_t>(tb.texture) < textures.size() && textures[static_cast<size_t>(tb.texture)].srv) {
                clamp = scene->textures[static_cast<size_t>(tb.texture)].clamp;
                return &textures[static_cast<size_t>(tb.texture)];
            }
        std::string def = "white";
        if (mat.shaderIndex >= 0 && static_cast<size_t>(mat.shaderIndex) < scene->shaders.size())
            for (const auto& d : scene->shaders[static_cast<size_t>(mat.shaderIndex)].texDefaults) if (d.first == name) def = d.second;
        (void)sh;
        return &defaults[def == "black" ? 1 : (def == "gray" || def == "grey") ? 2 : def == "bump" ? 3 : 0];
    }
};

GpuView::GpuView() : impl_(new Impl()) {}
GpuView::~GpuView() = default;
bool GpuView::ready() const { return impl_->dev != nullptr; }
void GpuView::Shutdown() { impl_->ReleaseDevice(); }
GpuKind GpuView::kind() const { return impl_->kind; }
std::string GpuView::adapter() const { return impl_->adapterName; }
void GpuView::SetSkinMode(const std::string& mode) {
    impl_->wantSkin = (mode == "cpu" || mode == "auto") ? mode : "gpu";
    impl_->skinOnGpu = false;
}
void GpuView::SetSkinThreshold(size_t verts) { impl_->skinThresholdVerts = verts; }
SkinMemoryStats GpuView::SkinMemory() const {
    SkinMemoryStats s;
    const Impl& I = *impl_;
    for (const auto& m : I.meshes) {
        const size_t bytes = m.rest.size() * sizeof(GVert);
        if (m.restBuf) s.restBytes += bytes;
        if (m.skinInBuf) s.skinInBytes += bytes;
        if (m.skinOutBuf) s.skinOutBytes += bytes;
        if (m.posedBuf) s.posedBytes += bytes;
        s.cpuMirrorBytes += bytes;
        s.vertexCount += m.rest.size();
        if (!m.rest.empty()) s.bufferCount += (m.restBuf ? 1 : 0) + (m.skinInBuf ? 1 : 0) + (m.skinOutBuf ? 1 : 0) + (m.posedBuf ? 1 : 0);
    }
    if (I.skBones) s.boneBytes = I.skBonesBytes;
    s.boneCount = I.skinCopy.size();
    s.totalGpuBytes = s.restBytes + s.skinInBytes + s.skinOutBytes + s.posedBytes + s.boneBytes;
    s.bufferCount += I.skBones ? 1 : 0;
    // Transient scratch for one CPU-skin update (the largest mesh's posedVerts).
    size_t largest = 0;
    for (const auto& m : I.meshes) largest = std::max(largest, m.rest.size());
    s.cpuStagingBytes = largest * sizeof(GVert);
    return s;
}
bool GpuView::gpuSkin() const { return impl_->skinOnGpu; }
void GpuView::SetBenchQueries(bool on) { impl_->benchQueries = on; }
bool GpuView::BenchQueries() const { return impl_->benchQueries; }
double GpuView::LastGpuDispatchMs() const {
    Impl& I = *impl_;
    if (!I.ctx || !I.benchPending) return -1.0;
    I.ctx->Flush();
    // Try timestamp/disjoint first (pure GPU compute time). Some drivers
    // report a zero frequency; then fall back to the event (GPU completion
    // time from dispatch start, waited for). Either way the result accounts
    // for asynchronous execution; CPU submission time alone is never returned.
    if (I.benchTsStart && I.benchTsEnd && I.benchTsDisjoint) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        bool gotDisjoint = false;
        for (int i = 0; i < 1000; ++i) {
            if (I.ctx->GetData(I.benchTsDisjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) { gotDisjoint = true; break; }
            Sleep(0);
        }
        if (gotDisjoint && !dj.Disjoint && dj.Frequency != 0) {
            uint64_t t0 = 0, t1 = 0;
            bool gotTimes = false;
            for (int i = 0; i < 1000; ++i) {
                const HRESULT hs = I.ctx->GetData(I.benchTsStart, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                const HRESULT he = I.ctx->GetData(I.benchTsEnd, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (hs == S_OK && he == S_OK && t1 >= t0) { gotTimes = true; break; }
                Sleep(0);
            }
            if (gotTimes) {
                I.benchPending = false;
                I.benchLastMs = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
                return I.benchLastMs;
            }
        } else if (std::getenv("CARDBRIDGE_DEBUG_GPU")) {
            static bool logged = false;
            if (!logged) { logged = true; std::fprintf(stderr, "bench timestamps unusable (got %d freq %llu disjoint %d), using event timing\n", gotDisjoint, static_cast<unsigned long long>(dj.Frequency), dj.Disjoint); }
        }
    }
    if (I.benchEvent) {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        bool signaled = false;
        LARGE_INTEGER now{};
        for (int i = 0; i < 100000; ++i) {
            if (I.ctx->GetData(I.benchEvent, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) { signaled = true; break; }
            if ((i & 1023) == 0) Sleep(0);
        }
        I.benchPending = false;
        if (!signaled) return -1.0;
        QueryPerformanceCounter(&now);
        I.benchLastMs = static_cast<double>(now.QuadPart - I.benchStartQpc.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        return I.benchLastMs;
    }
    I.benchPending = false;
    return -1.0;
}
bool GpuView::ReadbackPosed(std::vector<std::array<float, 3>>& outPos, std::vector<std::array<float, 3>>* outNrm) {
    Impl& I = *impl_;
    if (!I.ctx) return false;
    outPos.clear();
    if (outNrm) outNrm->clear();
    // Concatenate posed positions across meshes in scene order (mesh order matches the scene).
    // When a mesh was dispatched its posedBuf holds the result; otherwise its vb holds the CPU pose.
    for (const auto& g : I.meshes) {
        if (!g.vb) continue;
        const size_t n = g.rest.size();
        if (n == 0) continue;
        const UINT bytes = static_cast<UINT>(n * sizeof(GVert));
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* stage = nullptr;
        if (FAILED(I.dev->CreateBuffer(&sd, nullptr, &stage))) return false;
        I.ctx->CopyResource(stage, g.vb);
        I.ctx->Flush();
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(I.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) { Rel(stage); return false; }
        const GVert* p = static_cast<const GVert*>(mp.pData);
        for (size_t v = 0; v < n; ++v) {
            outPos.push_back({p[v].pos[0], p[v].pos[1], p[v].pos[2]});
            if (outNrm) outNrm->push_back({p[v].nrm[0], p[v].nrm[1], p[v].nrm[2]});
        }
        I.ctx->Unmap(stage, 0);
        Rel(stage);
    }
    return !outPos.empty();
}
int GpuView::skippedMaterials() const { return impl_->skipped; }
void GpuView::SetPlain(bool on) { impl_->plain = on; }

void GpuView::SetSkin(const std::vector<Mat4>* skin) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene) return;
    if (skin && skin != &I.skinCopy) I.skinCopy = *skin;
    else if (!skin) I.skinCopy.clear();
    if (skin == &I.skinCopy) skin = I.skinCopy.empty() ? nullptr : &I.skinCopy;
    const bool posed = skin && !skin->empty();
    // GPU path: one compute dispatch per mesh, no CPU vertex work. Needs the shader, the bone matrices on the GPU and a
    // rest/posed buffer pair per mesh. Anything missing falls through to the CPU loop below, same picture.
    // In "auto" mode small meshes stay on the CPU (dispatch + sync overhead outweighs the compute saving below the
    // benchmark threshold); the threshold is per mesh so a small accessory does not pay GPU overhead for a big body.
    size_t th = I.skinThresholdVerts;
    if (th == 0) {
        const char* env = std::getenv("CARDBRIDGE_SKIN_THRESHOLD");
        // Benchmark crossover for the preview path (CPU SkinPoint vs GPU
        // submission) sits below the smallest measured workload (10k verts,
        // GPU 30x faster on submission); the sync+readback crossover sits
        // near 50k. The preview never reads back, so the default stays low,
        // just above dispatch fixed overhead (~a few hundred verts).
        th = env ? static_cast<size_t>(std::atoll(env)) : 2000;
    }
    auto wantGpu = [&](const GpuMesh& m) {
        if (!posed || I.wantSkin == "cpu") return false;
        if (I.wantSkin == "auto" && m.rest.size() < th) return false;
        return true;
    };
    const bool gpuReady = posed && I.wantSkin != "cpu" && I.MakeSkinCompute() && I.UploadBones(skin);
    auto useGpu = [&](const GpuMesh& m) { return gpuReady && wantGpu(m) && m.posedBuf != nullptr; };
    I.skinOnGpu = false;
    if (gpuReady) {
        for (auto& g : I.meshes) if (!g.rest.empty() && useGpu(g)) { I.DispatchSkin(g); I.skinOnGpu = true; }
    }
    std::vector<GVert> posedVerts;
    for (size_t mi = 0; mi < I.meshes.size() && mi < I.scene->meshes.size(); ++mi) {
        GpuMesh& g = I.meshes[mi];
        if (!g.vb && !g.restBuf) continue;
        if (g.rest.empty()) continue;
        if (useGpu(g)) continue;   // already dispatched above; nothing to upload
        const auto& verts = I.scene->meshes[mi].vertices;
        const std::vector<GVert>* src = &g.rest;
        if (posed) {
            posedVerts = g.rest;
            for (size_t v = 0; v < posedVerts.size() && v < verts.size(); ++v) {
                // the vertex's blended skin matrix, applied to its position, normal and tangent.
                // Bounds and weight checks match SkinPoint and the compute shader.
                Mat4 m;
                for (float& x : m.m) x = 0;
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    const float w = verts[v].weight[k];
                    const int b = verts[v].bone[k];
                    if (b < 0 || static_cast<size_t>(b) >= skin->size()) continue;
                    if (!(w > 0.0f) || !std::isfinite(w)) continue;
                    for (int e = 0; e < 16; ++e) m.m[e] += (*skin)[static_cast<size_t>(b)].m[e] * w;
                    sum += w;
                }
                if (!(sum > 1e-6f) || !std::isfinite(sum)) continue;
                for (float& x : m.m) x /= sum;
                GVert& o = posedVerts[v];
                const Vec3 p = m.point({o.pos[0], o.pos[1], o.pos[2]});
                const Vec3 n = normalize(m.dir({o.nrm[0], o.nrm[1], o.nrm[2]}));
                const Vec3 t = normalize(m.dir({o.tan[0], o.tan[1], o.tan[2]}));
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
                o.pos[0] = p.x; o.pos[1] = p.y; o.pos[2] = p.z;
                if (std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z)) { o.nrm[0] = n.x; o.nrm[1] = n.y; o.nrm[2] = n.z; }
                if (std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z)) { o.tan[0] = t.x; o.tan[1] = t.y; o.tan[2] = t.z; }
            }
            src = &posedVerts;
        }
        if (g.posedBuf) {
            I.ctx->UpdateSubresource(g.posedBuf, 0, nullptr, src->data(), 0, 0);
            g.vb = g.posedBuf;
        } else if (g.restBuf && !posed) {
            g.vb = g.restBuf;
        } else if (posed) {
            I.ctx->UpdateSubresource(g.vb, 0, nullptr, src->data(), 0, 0);
        } else if (g.restBuf) {
            g.vb = g.restBuf;
        } else {
            I.ctx->UpdateSubresource(g.vb, 0, nullptr, g.rest.data(), 0, 0);
        }
    }
}

void GpuView::Refresh(bool tangents) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene) return;
    for (size_t mi = 0; mi < I.meshes.size() && mi < I.scene->meshes.size(); ++mi) {
        GpuMesh& g = I.meshes[mi];
        const auto& m = I.scene->meshes[mi];
        if ((!g.vb && !g.restBuf) || g.rest.size() != m.vertices.size()) continue;
        std::vector<Vec3> tan;
        if (tangents) tan = detail::ComputeMeshTangents(m);
        for (size_t v = 0; v < g.rest.size(); ++v) {
            const auto& sv = m.vertices[v];
            g.rest[v].pos[0] = sv.pos.x; g.rest[v].pos[1] = sv.pos.y; g.rest[v].pos[2] = sv.pos.z;
            g.rest[v].nrm[0] = sv.normal.x; g.rest[v].nrm[1] = sv.normal.y; g.rest[v].nrm[2] = sv.normal.z;
            if (tangents) { g.rest[v].tan[0] = tan[v].x; g.rest[v].tan[1] = tan[v].y; g.rest[v].tan[2] = tan[v].z; }
        }
        if (g.restBuf) I.ctx->UpdateSubresource(g.restBuf, 0, nullptr, g.rest.data(), 0, 0);
        if (g.skinInBuf) I.ctx->UpdateSubresource(g.skinInBuf, 0, nullptr, g.rest.data(), 0, 0);
    }
    SetSkin(I.skinCopy.empty() ? nullptr : &I.skinCopy);
}

bool GpuView::Project(Vec3 world, float& sx, float& sy, float& depth) const {
    const Impl& I = *impl_;
    if (I.lastW <= 0) return false;
    const float* M = I.lastM;
    const float cx = M[0] * world.x + M[1] * world.y + M[2] * world.z + M[3];
    const float cy = M[4] * world.x + M[5] * world.y + M[6] * world.z + M[7];
    depth = M[8] * world.x + M[9] * world.y + M[10] * world.z + M[11];
    sx = (cx + 1.0f) * 0.5f * static_cast<float>(I.lastW);
    sy = (1.0f - cy) * 0.5f * static_cast<float>(I.lastH);
    return true;
}
bool GpuView::plain() const { return impl_->plain; }
size_t GpuView::layerCount() const { return impl_->layers.size(); }
bool GpuView::layerEnabled(size_t i) const { return i < impl_->layers.size() && impl_->layers[i].enabled; }
bool GpuView::layerOk(size_t i) const { return i < impl_->layers.size() && impl_->layers[i].ok; }
void GpuView::SetLayerEnabled(size_t i, bool on) { if (i < impl_->layers.size() && impl_->layers[i].ok) impl_->layers[i].enabled = on; }

bool GpuView::SetLayers(const std::vector<ShaderLayer>& defs, std::string* report) {
    Impl& I = *impl_;
    I.ClearLayers();
    if (!I.dev) return false;
    typedef HRESULT(WINAPI * CompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<CompileFn>(reinterpret_cast<void*>(GetProcAddress(dll, "D3DCompile"))) : nullptr;
    bool any = false;
    for (const ShaderLayer& d : defs) {
        Impl::Layer L;
        L.enabled = d.defaultOn;
        std::string problem;
        if (!compile) {
            problem = "d3dcompiler_47.dll is not available";
        } else {
            const std::string src = std::string(ShaderLayerPrelude()) + "\n#line 1\n" + d.source;
            ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
            auto message = [&](ID3DBlob* e) { return e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : std::string("compile failed"); };
            if (FAILED(compile(src.data(), src.size(), d.file.c_str(), nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, &err)) || !vb) {
                problem = message(err);
            } else {
                Rel(err);
                if (FAILED(compile(src.data(), src.size(), d.file.c_str(), nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, &err)) || !pb) problem = message(err);
            }
            Rel(err);
            if (problem.empty()) {
                const D3D11_INPUT_ELEMENT_DESC el[3] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GVert, nrm), D3D11_INPUT_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GVert, uv), D3D11_INPUT_PER_VERTEX_DATA, 0}};
                D3D11_BLEND_DESC bd{};
                auto& rt = bd.RenderTarget[0];
                rt.BlendEnable = TRUE;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
                rt.DestBlendAlpha = D3D11_BLEND_ONE;
                rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
                if (d.blend == "multiply") { rt.SrcBlend = D3D11_BLEND_DEST_COLOR; rt.DestBlend = D3D11_BLEND_ZERO; }
                else if (d.blend == "alpha") { rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; }
                else { rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_ONE; }
                D3D11_BUFFER_DESC cbd{};
                cbd.ByteWidth = 9 * 16;
                cbd.Usage = D3D11_USAGE_DEFAULT;
                cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                const bool made = SUCCEEDED(I.dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &L.vs)) &&
                                  SUCCEEDED(I.dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &L.ps)) &&
                                  SUCCEEDED(I.dev->CreateInputLayout(el, 3, vb->GetBufferPointer(), vb->GetBufferSize(), &L.il)) &&
                                  SUCCEEDED(I.dev->CreateBlendState(&bd, &L.blend)) && SUCCEEDED(I.dev->CreateBuffer(&cbd, nullptr, &L.cb));
                if (!made) problem = "Direct3D refused the shader";
            }
            Rel(vb);
            Rel(pb);
        }
        L.ok = problem.empty();
        if (!L.ok) {
            Rel(L.vs); Rel(L.ps); Rel(L.il); Rel(L.blend); Rel(L.cb);
            L.enabled = false;
            if (report) *report += d.file + ": " + problem + "\n";
        }
        any = any || L.ok;
        I.layers.push_back(L);
    }
    return any;
}


bool GpuView::Init(std::string* why) { return Init(GpuOptions{}, why); }

bool GpuView::Init(const GpuOptions& opt, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    Impl& I = *impl_;
    if (I.dev) return true;
    I.wantSkin = opt.skin;
    if (const char* env = std::getenv("CARDBRIDGE_SKIN")) I.wantSkin = env;
    I.skinThresholdVerts = opt.skinThresholdVerts;
    if (const char* env = std::getenv("CARDBRIDGE_SKIN_THRESHOLD")) I.skinThresholdVerts = static_cast<size_t>(std::atoll(env));
    std::string pref = opt.preference;
    if (const char* env = std::getenv("CARDBRIDGE_GPU")) pref = env;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    auto utf8 = [](const wchar_t* w) {
        std::string s;
        if (!w || !*w) return s;
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return s;
        s.resize(static_cast<size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    if (pref == "software") {
        I.kind = GpuKind::Software;
        I.adapterName = "software (CPU)";
        return fail("software renderer requested");
    }
    if (pref == "warp") {
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &I.dev, &got, &I.ctx);
        if (FAILED(hr)) { I.dev = nullptr; I.ctx = nullptr; I.kind = GpuKind::Software; return fail("no WARP device"); }
        I.kind = GpuKind::Warp;
        I.adapterName = "WARP (CPU)";
    } else {
        // Enumerate the adapters and prefer the hardware adapter best suited to CardBridge (AdapterSelect.h):
        // software/virtual adapters are excluded, dedicated memory and the dedicated share order the rest, and
        // vendor is only an equal tie-breaker, so the default (nullptr) never leaves a discrete card idle for an
        // integrated one, and Intel Arc discrete competes like any other discrete card.
        const bool debugGpu = std::getenv("CARDBRIDGE_DEBUG_GPU") != nullptr;
        int wanted = -1;
        if (const char* env = std::getenv("CARDBRIDGE_GPU_ADAPTER")) wanted = std::atoi(env);
        struct Pick { IDXGIAdapter1* a = nullptr; AdapterDesc desc; };
        std::vector<Pick> picks;
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
            for (UINT n = 0;; ++n) {
                IDXGIAdapter1* a = nullptr;
                if (FAILED(f->EnumAdapters1(n, &a)) || !a) break;
                Pick p; p.a = a;
                DXGI_ADAPTER_DESC1 d{};
                a->GetDesc1(&d);
                p.desc.vendorId = d.VendorId;
                p.desc.deviceId = d.DeviceId;
                p.desc.description = utf8(d.Description);
                p.desc.dedicatedVideoMemory = d.DedicatedVideoMemory;
                p.desc.dedicatedSystemMemory = d.DedicatedSystemMemory;
                p.desc.sharedSystemMemory = d.SharedSystemMemory;
                p.desc.flags = d.Flags;
                p.desc.index = static_cast<int>(n);
                if (debugGpu) std::fprintf(stderr, "gpu adapter [%d]: %s\n", p.desc.index, DescribeAdapter(p.desc).c_str());
                picks.push_back(p);
            }
        }
        if (f) Rel(f);
        // CARDBRIDGE_GPU_ADAPTER names an adapter by its enumeration index, before any ordering.
        if (wanted >= 0) {
            Pick keep;
            bool found = false;
            for (const auto& p : picks) if (p.desc.index == wanted) { keep = p; found = true; break; }
            IDXGIAdapter1* keepPtr = found ? keep.a : nullptr;
            for (auto& p : picks) if (p.a != keepPtr) Rel(p.a);
            picks.clear();
            if (found) picks.push_back(keep);
        } else {
            // Order by the shared scoring policy, best first.
            std::sort(picks.begin(), picks.end(), [](const Pick& x, const Pick& y) {
                const int sx = ScoreAdapter(x.desc).score, sy = ScoreAdapter(y.desc).score;
                if (sx != sy) return sx > sy;
                if (x.desc.dedicatedVideoMemory != y.desc.dedicatedVideoMemory) return x.desc.dedicatedVideoMemory > y.desc.dedicatedVideoMemory;
                return x.desc.index < y.desc.index;
            });
        }
        bool made = false;
        for (auto& p : picks) {
            const AdapterScore s = ScoreAdapter(p.desc);
            if (!s.suitable) continue;   // software / virtual / remote: hardware selection skips them
            D3D_FEATURE_LEVEL got{};
            // CARDBRIDGE_D3D_DEBUG=1 asks for a debug device when the SDK is installed, so invalid buffer access
            // (out-of-range bone indices, bad SRV/UAV bindings) is caught by the D3D11 debug layer.
            const UINT flags = std::getenv("CARDBRIDGE_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_BGRA_SUPPORT : D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            if (SUCCEEDED(D3D11CreateDevice(p.a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &I.dev, &got, &I.ctx))) {
                I.kind = GpuKind::Hardware;
                I.adapterName = p.desc.description;
                if (debugGpu) std::fprintf(stderr, "gpu selected [%d]: %s (feature level 0x%lx, hardware)\n", p.desc.index, DescribeAdapter(p.desc).c_str(), static_cast<long>(got));
                made = true;
                break;
            } else if (debugGpu) {
                std::fprintf(stderr, "gpu adapter [%d] device creation failed, trying next\n", p.desc.index);
            }
        }
        for (auto& p : picks) Rel(p.a);
        if (!made) {
            if (pref == "hardware") { I.kind = GpuKind::Software; return fail("no hardware GPU found"); }
            D3D_FEATURE_LEVEL got{};
            const UINT flags = std::getenv("CARDBRIDGE_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG | D3D11_CREATE_DEVICE_BGRA_SUPPORT : D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &I.dev, &got, &I.ctx);
            if (FAILED(hr)) { I.dev = nullptr; I.ctx = nullptr; I.kind = GpuKind::Software; return fail("no Direct3D 11 device"); }
            I.kind = GpuKind::Warp;
            I.adapterName = "WARP (CPU)";
            if (debugGpu) std::fprintf(stderr, "gpu selected: WARP (CPU) (no suitable hardware adapter, software fallback)\n");
        }
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8; sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    I.dev->CreateSamplerState(&sd, &I.samplerWrap);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    I.dev->CreateSamplerState(&sd, &I.samplerClamp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    I.dev->CreateRasterizerState(&rd, &I.raster);
    rd.CullMode = D3D11_CULL_BACK; rd.FrontCounterClockwise = FALSE;   // Unity's convention: a face toward the camera winds clockwise on screen
    I.dev->CreateRasterizerState(&rd, &I.rasterCull);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    I.dev->CreateDepthStencilState(&dd, &I.depth);
    D3D11_DEPTH_STENCIL_DESC od = dd;
    od.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; od.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    I.dev->CreateDepthStencilState(&od, &I.depthOverlay);
    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable = TRUE;   // the shaders' alpha cut-out (eyelines, lashes, hair tips) without sorting
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    I.dev->CreateBlendState(&bd, &I.blend);
    D3D11_BLEND_DESC ob{};
    ob.RenderTarget[0].BlendEnable = TRUE;
    ob.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ob.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ob.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ob.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ob.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ob.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ob.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    I.dev->CreateBlendState(&ob, &I.blendOverlay);
    const uint8_t px[4][4] = {{255, 255, 255, 255}, {0, 0, 0, 0}, {128, 128, 128, 128}, {128, 128, 255, 128}};
    for (int i = 0; i < 4; ++i) I.MakeTexture(1, 1, px[i], I.defaults[i], false);
    I.MakeFallback();
    I.MakeOutline();
    return true;
}

void GpuView::Unload() {
    Impl& I = *impl_;
    I.Clear();
    I.scene = nullptr;
    I.skinCopy.clear();
    I.lastM[0] = 0;
    I.lastW = I.lastH = 0;
}

bool GpuView::UpdateTextures(const std::vector<int>& textureIndices) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene) return false;
    bool ok = true;
    for (int ti : textureIndices) {
        if (ti < 0 || static_cast<size_t>(ti) >= I.scene->textures.size()) continue;
        if (I.textures.size() < I.scene->textures.size()) I.textures.resize(I.scene->textures.size());
        GpuTex& g = I.textures[static_cast<size_t>(ti)];
        Rel(g.srv);
        Rel(g.tex);
        const auto& t = I.scene->textures[static_cast<size_t>(ti)];
        if (t.width > 0 && t.height > 0 && t.rgba.size() >= static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4) {
            if (!I.MakeTexture(t.width, t.height, t.rgba.data(), g, true, LinearLight() && t.srgb)) {
                Rel(g.srv);
                Rel(g.tex);
                ok = false;
            }
        }
    }
    return ok;
}

bool GpuView::UploadTexture(int textureIndex) { return UpdateTextures(std::vector<int>{textureIndex}); }

bool GpuView::EnsureTexture(int textureIndex) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene) return false;
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= I.scene->textures.size()) return false;
    const size_t ti = static_cast<size_t>(textureIndex);
    if (I.textures.size() > ti && I.textures[ti].srv && I.textures[ti].tex) return true;
    return UpdateTextures(std::vector<int>{textureIndex});
}

bool GpuView::Load(const assemble::Scene& scene, std::string* why,
                   const std::function<void(double, const std::string&)>& progress,
                   const std::function<bool()>& cancelled) {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    auto report = [&](double frac, const std::string& stage) { if (progress) progress(frac, stage); };
    auto isCancelled = [&]() { return cancelled && cancelled(); };
    Impl& I = *impl_;
    if (!I.dev) return fail("no device");
    if (isCancelled()) { I.Clear(); I.scene = nullptr; return fail("cancelled by user"); }
    I.Clear();
    I.scene = &scene;

    // Shaders: the forward pass of each, straight from the bundle's DXBC.
    I.shaders.resize(scene.shaders.size());
    for (size_t i = 0; i < scene.shaders.size(); ++i) {
        if (isCancelled()) { I.Clear(); I.scene = nullptr; return fail("cancelled by user"); }
        report(0.05 + 0.15 * (scene.shaders.empty() ? 1.0 : static_cast<double>(i + 1) / static_cast<double>(scene.shaders.size())),
               "shaders " + std::to_string(i + 1) + " / " + std::to_string(scene.shaders.size()));
        auto& g = I.shaders[i];
        Bytes blob(scene.shaders[i].blob.begin(), scene.shaders[i].blob.end());
        g.progs = shader::ParseUnityShaderBlob(blob, nullptr);
        if (!shader::PickForwardPass(g.progs, g.pi, g.vi)) continue;
        const auto& vp = g.progs[g.vi];
        const auto& pp = g.progs[g.pi];
        if (vp.dxbc.empty() || pp.dxbc.empty()) continue;
        if (FAILED(I.dev->CreateVertexShader(vp.dxbc.data(), vp.dxbc.size(), nullptr, &g.vs))) continue;
        if (FAILED(I.dev->CreatePixelShader(pp.dxbc.data(), pp.dxbc.size(), nullptr, &g.ps))) { Rel(g.vs); continue; }
        std::vector<D3D11_INPUT_ELEMENT_DESC> el;
        std::vector<std::string> names;
        for (const auto& in : vp.code.inputs) names.push_back(in.semantic);
        for (size_t k = 0; k < vp.code.inputs.size(); ++k) {
            const auto& in = vp.code.inputs[k];
            D3D11_INPUT_ELEMENT_DESC e{};
            e.SemanticName = names[k].c_str(); e.SemanticIndex = in.index; e.InputSlot = 0; e.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            if (in.semantic == "POSITION") { e.Format = DXGI_FORMAT_R32G32B32_FLOAT; e.AlignedByteOffset = offsetof(GVert, pos); }
            else if (in.semantic == "NORMAL") { e.Format = DXGI_FORMAT_R32G32B32_FLOAT; e.AlignedByteOffset = offsetof(GVert, nrm); }
            else if (in.semantic == "TANGENT") { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = offsetof(GVert, tan); }
            else if (in.semantic == "COLOR") { e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; e.AlignedByteOffset = offsetof(GVert, col); }
            else { e.Format = DXGI_FORMAT_R32G32_FLOAT; e.AlignedByteOffset = in.index == 1 ? offsetof(GVert, uv1) : in.index == 2 ? offsetof(GVert, uv2) : in.index == 3 ? offsetof(GVert, uv3) : offsetof(GVert, uv); }   // TEXCOORD1..3 are the extra UV sets, the rest read the first
            el.push_back(e);
        }
        if (FAILED(I.dev->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), vp.dxbc.data(), vp.dxbc.size(), &g.il))) { Rel(g.vs); Rel(g.ps); continue; }
        g.ok = true;
    }

    // Textures the shaders can read. A failed upload must fail the Load
    // (the caller falls back to the software renderer) instead of leaving the
    // view null and drawing the white/black shader defaults as rectangles.
    I.textures.assign(scene.textures.size(), GpuTex{});
    bool texFailed = false;
    auto upload = [&](int ti) {
        if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size() || I.textures[static_cast<size_t>(ti)].srv) return;
        const auto& t = scene.textures[static_cast<size_t>(ti)];
        if (t.width > 0 && t.height > 0 && t.rgba.size() >= static_cast<size_t>(t.width) * t.height * 4) {
            if (!I.MakeTexture(t.width, t.height, t.rgba.data(), I.textures[static_cast<size_t>(ti)], true, LinearLight() && t.srgb))
                texFailed = true;
        }
    };
    for (const auto& m : scene.materials) {
        if (isCancelled()) { I.Clear(); I.scene = nullptr; return fail("cancelled by user"); }
        for (const auto& tb : m.texProps) upload(tb.texture);
        upload(m.texture);
    }
    for (const auto& tb : scene.globalTextures) upload(tb.texture);
    report(0.5, "textures");
    if (isCancelled()) { I.Clear(); I.scene = nullptr; return fail("cancelled by user"); }
    if (texFailed) { I.Clear(); I.scene = nullptr; return fail("a texture upload failed"); }

    I.cutout.assign(scene.materials.size(), 0);
    for (size_t m = 0; m < scene.materials.size(); ++m) I.cutout[m] = HasCutout(scene, scene.materials[m]) ? 1 : 0;

    // Geometry, with the tangents the shaders build their normal-map frame from. When the skinning compute shader is
    // available, each mesh gets a rest buffer (read by the skinning shader and drawn when there is no pose) and a posed
    // buffer (written by it). Without it, there is only the old single vertex buffer that SetSkin rewrites on the CPU.
    const bool dual = I.wantSkin != "cpu" && I.MakeSkinCompute();
    I.meshes.resize(scene.meshes.size());
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        if (isCancelled()) { I.Clear(); I.scene = nullptr; return fail("cancelled by user"); }
        if (mi % 8 == 0 && !scene.meshes.empty())
            report(0.5 + 0.5 * (static_cast<double>(mi) / static_cast<double>(scene.meshes.size())),
                   "meshes " + std::to_string(mi + 1) + " / " + std::to_string(scene.meshes.size()));
        const auto& m = scene.meshes[mi];
        auto& g = I.meshes[mi];
        if (m.vertices.empty()) continue;
        std::vector<Vec3> tan = detail::ComputeMeshTangents(m);
        std::vector<GVert> vb(m.vertices.size());
        for (size_t v = 0; v < vb.size(); ++v) {
            const auto& s = m.vertices[v];
            vb[v] = {{s.pos.x, s.pos.y, s.pos.z}, {s.normal.x, s.normal.y, s.normal.z}, {tan[v].x, tan[v].y, tan[v].z, 1.0f}, {s.uv.x, s.uv.y}, {1, 1, 1, 1}, {s.uv1.x, s.uv1.y}, {s.uv2.x, s.uv2.y}, {s.uv3.x, s.uv3.y}, {s.bone[0], s.bone[1], s.bone[2], s.bone[3]}, {s.weight[0], s.weight[1], s.weight[2], s.weight[3]}};
        }
        const UINT bytes = static_cast<UINT>(vb.size() * sizeof(GVert));
        if (dual) {
            // A structured buffer cannot also be a vertex buffer, so the rest vertices live twice: once in a plain vertex
            // buffer that is drawn when there is no pose, once in a structured buffer the skinning shader reads. The
            // skinned result is written to a structured buffer too and copied into a plain posed vertex buffer for drawing.
            // (Verified on the test hardware: BIND_VERTEX_BUFFER | BIND_SHADER_RESOURCE with
            // D3D11_RESOURCE_MISC_BUFFER_STRUCTURED is rejected by D3D11CreateBuffer, so the duplication is required
            // while the shader uses structured buffers. A ByteAddressBuffer rewrite could halve it; see
            // docs/GPU_SKINNING_HARDENING.md for why that rewrite was investigated but not taken here.)
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA sr{vb.data(), 0, 0};
            if (FAILED(I.dev->CreateBuffer(&bd, &sr, &g.restBuf))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "rest buf create failed\n"); Rel(g.restBuf); continue; }
            D3D11_BUFFER_DESC pd{};
            pd.ByteWidth = bytes; pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            if (FAILED(I.dev->CreateBuffer(&pd, &sr, &g.posedBuf))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "posed buf create failed\n"); Rel(g.posedBuf); Rel(g.restBuf); continue; }
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sd.StructureByteStride = sizeof(GVert);
            D3D11_SUBRESOURCE_DATA ssr{vb.data(), 0, 0};
            if (FAILED(I.dev->CreateBuffer(&sd, &ssr, &g.skinInBuf))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "skin in create failed\n"); Rel(g.skinInBuf); Rel(g.posedBuf); Rel(g.restBuf); continue; }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; sv.BufferEx.NumElements = static_cast<UINT>(vb.size());
            if (FAILED(I.dev->CreateShaderResourceView(g.skinInBuf, &sv, &g.restSrv))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "rest srv create failed\n"); Rel(g.restSrv); Rel(g.skinInBuf); Rel(g.posedBuf); Rel(g.restBuf); continue; }
            D3D11_BUFFER_DESC od{};
            od.ByteWidth = bytes; od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; od.StructureByteStride = sizeof(GVert);
            D3D11_SUBRESOURCE_DATA osr{vb.data(), 0, 0};
            if (FAILED(I.dev->CreateBuffer(&od, &osr, &g.skinOutBuf))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "skin out create failed\n"); Rel(g.skinOutBuf); Rel(g.restSrv); Rel(g.skinInBuf); Rel(g.posedBuf); Rel(g.restBuf); continue; }
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = static_cast<UINT>(vb.size());
            if (FAILED(I.dev->CreateUnorderedAccessView(g.skinOutBuf, &uv, &g.posedUav))) { if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) std::fprintf(stderr, "posed uav create failed\n"); Rel(g.posedUav); Rel(g.skinOutBuf); Rel(g.restSrv); Rel(g.skinInBuf); Rel(g.posedBuf); Rel(g.restBuf); continue; }
            g.vb = g.restBuf;
        } else {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = bytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;   // default, not immutable: SetSkin rewrites it
            D3D11_SUBRESOURCE_DATA sr{vb.data(), 0, 0};
            if (FAILED(I.dev->CreateBuffer(&bd, &sr, &g.vb))) continue;
        }
        g.rest = std::move(vb);
        for (const auto& sub : m.submeshes) {
            GpuSub gs;
            gs.material = sub.material; gs.count = static_cast<UINT>(sub.indices.size());
            if (gs.count == 0) continue;
            D3D11_BUFFER_DESC ib{};
            ib.ByteWidth = gs.count * 4; ib.Usage = D3D11_USAGE_IMMUTABLE; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA is{sub.indices.data(), 0, 0};
            if (FAILED(I.dev->CreateBuffer(&ib, &is, &gs.ib))) continue;
            g.subs.push_back(gs);
        }
    }
    if (std::getenv("CARDBRIDGE_DEBUG_SKIN")) {
        const SkinMemoryStats m = SkinMemory();
        std::fprintf(stderr, "skin buffers: %zu verts, rest %zuKB skin-in %zuKB skin-out %zuKB posed %zuKB bones %zuKB total GPU %zuKB CPU mirror %zuKB (%zu buffers)\n",
                     m.vertexCount, m.restBytes / 1024, m.skinInBytes / 1024, m.skinOutBytes / 1024, m.posedBytes / 1024, m.boneBytes / 1024, m.totalGpuBytes / 1024, m.cpuMirrorBytes / 1024, m.bufferCount);
    }
    report(1.0, "done");
    return true;
}

bool GpuView::Render(const ViewCamera& cam, int W, int H, const std::vector<char>* hiddenParts, std::vector<uint32_t>& bgra,
                     int highlightPart) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene || W < 8 || H < 8) return false;
    const assemble::Scene& scene = *I.scene;
    if (!I.EnsureTargets(W, H)) return false;

    // Same framing as RenderView: bounds of what is drawn, an orthographic camera orbiting its centre.
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    auto visible = [&](size_t pi) {
        const auto& part = scene.parts[pi];
        const char flag = hiddenParts && pi < hiddenParts->size() ? (*hiddenParts)[pi] : 0;
        if (!part.includeInExport) return false;
        if (flag == 2) return true;   // forced on, even for a part that is hidden by default (a nude under piece)
        return part.visibleInPreview && !flag;
    };
    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        if (!visible(pi)) continue;
        for (int mi : scene.parts[pi].meshes)
            for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
                mn[0] = std::min(mn[0], v.pos.x); mn[1] = std::min(mn[1], v.pos.y); mn[2] = std::min(mn[2], v.pos.z);
                mx[0] = std::max(mx[0], v.pos.x); mx[1] = std::max(mx[1], v.pos.y); mx[2] = std::max(mx[2], v.pos.z);
            }
    }
    bgra.assign(static_cast<size_t>(W) * H, 0xFFE2E4E8u);
    if (mn[0] > mx[0]) return true;
    const float cx = (mn[0] + mx[0]) * 0.5f, cy = (mn[1] + mx[1]) * 0.5f, cz = (mn[2] + mx[2]) * 0.5f;
    const float extent = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
    const float scale = H * 0.9f / extent * cam.zoom;
    const float yaw = cam.yawDeg * 3.14159265f / 180.0f, pit = cam.pitchDeg * 3.14159265f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const float toCam[3] = {-sn * cp, sp, cs * cp};
    float L[3] = {toCam[0], toCam[1], toCam[2]};   // the character maker's light shines straight along the view (its Directional Light faces -Z)
    if (I.plain) { L[0] = -0.5f; L[1] = 1.0f; L[2] = 0.5f; }   // MikuMikuDance's default light (direction -0.5, -1, 0.5), for the PMX turned to face -Z
    if (const char* ld = std::getenv("CARDBRIDGE_LIGHTDIR")) std::sscanf(ld, "%f,%f,%f", &L[0], &L[1], &L[2]);   // test aid: world-space direction towards the light
    { float n = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); for (float& c : L) c /= n; }
    const float camPos[3] = {cx + toCam[0] * 5.0f, cy + toCam[1] * 5.0f, cz + toCam[2] * 5.0f};

    // World -> clip, row by row (the software projection written as a matrix).
    const float a = 2.0f * scale / W, c = 2.0f * scale / H, bx = 2.0f * cam.panX * H / W, by = -2.0f * cam.panY, zk = 0.5f / extent;
    float M[4][4] = {
        {-a * cs, 0, -a * sn, 0},
        {c * sp * sn, c * cp, -c * sp * cs, 0},
        {zk * cp * sn, -zk * sp, -zk * cp * cs, 0},
        {0, 0, 0, 1}};
    for (int r = 0; r < 3; ++r) M[r][3] = -(M[r][0] * cx + M[r][1] * cy + M[r][2] * cz);
    M[0][3] += bx; M[1][3] += by; M[2][3] += 0.5f;
    for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) I.lastM[r * 4 + col] = M[r][col];
    I.lastW = W; I.lastH = H;

    ID3D11DeviceContext* ctx = I.ctx;
    float bg[4] = {0xE2 / 255.0f, 0xE4 / 255.0f, 0xE8 / 255.0f, 1.0f};
    if (LinearLight()) for (int i = 0; i < 3; ++i) bg[i] = ToLinear(bg[i]);   // the target encodes what is written
    ctx->OMSetRenderTargets(1, &I.rtv, I.dsv);
    ctx->ClearRenderTargetView(I.rtv, bg);
    ctx->ClearDepthStencilView(I.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(I.raster);
    ctx->OMSetDepthStencilState(I.depth, 0);
    const float bf[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(I.blend, bf, 0xFFFFFFFFu);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(GVert), offset = 0;
    // The silhouette ring: the mesh drawn again pushed out along its normals in screen space, in one colour.
    auto ring = [&](const GpuMesh& gm, const GpuSub& gs, const Vec3& col, float px) {
        ctx->RSSetState(I.raster);
        float cbv[24];
        for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) cbv[r * 4 + k] = M[r][k];
        const float colr[4] = {LinearLight() ? ToLinear(col.x) : col.x, LinearLight() ? ToLinear(col.y) : col.y, LinearLight() ? ToLinear(col.z) : col.z, 1.0f};
        const float prm[4] = {px * 2.0f / W, px * 2.0f / H, 0.004f, 0.0f};
        std::memcpy(cbv + 16, colr, 16);
        std::memcpy(cbv + 20, prm, 16);
        ctx->UpdateSubresource(I.olCb, 0, nullptr, cbv, 0, 0);
        ctx->IASetInputLayout(I.olIl);
        ctx->VSSetShader(I.olVs, nullptr, 0);
        ctx->PSSetShader(I.olPs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &I.olCb);
        ctx->PSSetConstantBuffers(0, 1, &I.olCb);
        ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
        ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(gs.count, 0, 0);
    };
    // Each visible submesh, opaque materials first and then the alpha-flagged ones.
    auto eachSub = [&](const std::function<void(const GpuMesh&, const GpuSub&, const assemble::SceneMaterial&)>& fn) {
        for (int pass = 0; pass < 2; ++pass)
            for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
                if (!visible(pi)) continue;
                for (int mi : scene.parts[pi].meshes) {
                    const GpuMesh& gm = I.meshes[static_cast<size_t>(mi)];
                    if (!gm.vb) continue;
                    for (const auto& gs : gm.subs) {
                        const auto& mat = scene.materials[static_cast<size_t>(gs.material)];
                        if ((mat.alpha ? 1 : 0) == pass) fn(gm, gs, mat);
                    }
                }
            }
    };

    // Plain mode is MikuMikuDance's look: every material as the PMX writes it (MmdMaterialLook), the self-shadow its casters throw
    // along the light (a depth map) and the ground shadow, the flagged meshes flattened along the light onto the floor.
    const float radius = 0.5f * std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) + (mx[2] - mn[2]) * (mx[2] - mn[2])) * 1.05f;
    float LS[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}};   // world -> shadow map (x, y in -1..1, depth 0..1)
    bool selfShadow = false;
    const bool fallbackOk = I.fbVs && I.fbPs && I.fbIl && I.fbCb;
    if (I.plain && fallbackOk) {
        if (I.looks.size() != scene.materials.size()) {
            I.looks.clear();
            for (const auto& m : scene.materials) {
                I.looks.push_back(MmdMaterialLook(scene, m));
                if (I.looks.back().sphere == 0) continue;
                const assemble::SceneTexture t = MmdSphereImage(I.looks.back());
                if (!I.spheres.count(t.name)) I.MakeTexture(t.width, t.height, t.rgba.data(), I.spheres[t.name], false, LinearLight());
            }
            I.lookSphere.assign(I.looks.size(), nullptr);
            for (size_t k = 0; k < I.looks.size(); ++k)
                if (I.looks[k].sphere > 0) I.lookSphere[k] = I.spheres[MmdSphereImage(I.looks[k]).name].srv;
        }
        const float f[3] = {-L[0], -L[1], -L[2]};   // along the light
        float x[3] = {f[2], 0.0f, -f[0]};           // up x f
        float xn = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        if (xn < 1e-4f) { x[0] = 1.0f; x[2] = 0.0f; xn = 1.0f; }
        x[0] /= xn; x[2] /= xn;
        const float y[3] = {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
        const float ctr[3] = {cx, cy, cz};
        const float* axes[3] = {x, y, f};
        for (int r = 0; r < 3; ++r) {
            const float k = r == 2 ? 0.5f / radius : 1.0f / radius;
            for (int col = 0; col < 3; ++col) LS[r][col] = axes[r][col] * k;
            LS[r][3] = -(LS[r][0] * ctr[0] + LS[r][1] * ctr[1] + LS[r][2] * ctr[2]) + (r == 2 ? 0.5f : 0.0f);
        }
        float cbv[(7 + 32 + 5) * 4] = {};
        ctx->IASetInputLayout(I.fbIl);
        ctx->VSSetShader(I.fbVs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &I.fbCb);
        ctx->PSSetConstantBuffers(0, 1, &I.fbCb);
        auto draw = [&](const GpuMesh& gm, const GpuSub& gs) {
            ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
            ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(gs.count, 0, 0);
        };
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(1, 1, &none);
        if (I.MakeShadowMap()) {
            selfShadow = true;
            for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = LS[r][col];
            ctx->UpdateSubresource(I.fbCb, 0, nullptr, cbv, 0, 0);
            ctx->OMSetRenderTargets(0, nullptr, I.smDsv);
            ctx->ClearDepthStencilView(I.smDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            const D3D11_VIEWPORT sv{0, 0, static_cast<float>(Impl::kShadowSize), static_cast<float>(Impl::kShadowSize), 0, 1};
            ctx->RSSetViewports(1, &sv);
            ctx->PSSetShader(nullptr, nullptr, 0);
            eachSub([&](const GpuMesh& gm, const GpuSub& gs, const assemble::SceneMaterial&) {
                if (I.looks[static_cast<size_t>(gs.material)].drawFlags & 0x04) draw(gm, gs);
            });
            ctx->OMSetRenderTargets(1, &I.rtv, I.dsv);
            ctx->RSSetViewports(1, &vp);
            ctx->PSSetShaderResources(1, 1, &I.smSrv);
            ctx->PSSetSamplers(1, 1, &I.smCmp);
        }
        // The ground shadow, under everything (depth tested, not written): one solid shade of the background, so overlaps do not darken twice.
        for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
        for (int i = 0; i < 3; ++i) cbv[16 + i] = LinearLight() ? ToLinear(0.7f * (i == 0 ? 0xE2 : i == 1 ? 0xE4 : 0xE8) / 255.0f) : 0.7f * (i == 0 ? 0xE2 : i == 1 ? 0xE4 : 0xE8) / 255.0f;
        cbv[19] = 1.0f;
        cbv[24] = L[0]; cbv[25] = L[1]; cbv[26] = L[2];
        cbv[(7 + 32 + 3) * 4 + 1] = 1.0f; cbv[(7 + 32 + 3) * 4 + 2] = mn[1]; cbv[(7 + 32 + 3) * 4 + 3] = radius;
        ctx->UpdateSubresource(I.fbCb, 0, nullptr, cbv, 0, 0);
        ctx->PSSetShader(I.fbPs, nullptr, 0);
        ctx->OMSetDepthStencilState(I.depthOverlay, 0);
        eachSub([&](const GpuMesh& gm, const GpuSub& gs, const assemble::SceneMaterial&) {
            if (I.looks[static_cast<size_t>(gs.material)].drawFlags & 0x02) draw(gm, gs);
        });
        ctx->OMSetDepthStencilState(I.depth, 0);
    }

    I.skipped = 0;
    std::vector<char> matDone(scene.materials.size(), 0), matSkipped(scene.materials.size(), 0);
    std::vector<std::vector<std::vector<float>>> matVs(scene.materials.size()), matPs(scene.materials.size());
    for (int pass = 0; pass < 3; ++pass)   // opaque first, then the no-depth-write paint layers (like the game's transparent queue), then the alpha-flagged ones
        for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
            if (!visible(pi)) continue;
            for (int mi : scene.parts[pi].meshes) {
                const GpuMesh& gm = I.meshes[static_cast<size_t>(mi)];
                if (!gm.vb) continue;
                for (const auto& gs : gm.subs) {
                    const auto& mat = scene.materials[static_cast<size_t>(gs.material)];
                    const int bucket = mat.alpha ? 2 : (!I.plain && detail::IsOverlayMaterial(scene, mat) ? 1 : 0);
                    if (bucket != pass) continue;
                    if (bucket == 1) {   // the game's transparent compositing: tested, not written, alpha-blended
                        ctx->OMSetDepthStencilState(I.depthOverlay, 0);
                        ctx->OMSetBlendState(I.blendOverlay, bf, 0xFFFFFFFFu);
                    } else {
                        ctx->OMSetDepthStencilState(I.depth, 0);
                        ctx->OMSetBlendState(I.blend, bf, 0xFFFFFFFFu);
                    }
                    const GpuShader* sh = (!I.plain && mat.shaderIndex >= 0 && static_cast<size_t>(mat.shaderIndex) < I.shaders.size()) ? &I.shaders[static_cast<size_t>(mat.shaderIndex)] : nullptr;
                    // A single-sided material's back faces are not drawn, as in the game and MikuMikuDance (drawn, the inside of a shell showed through).
                    const bool twoSided = I.plain && static_cast<size_t>(gs.material) < I.looks.size() ? (I.looks[static_cast<size_t>(gs.material)].drawFlags & 0x01) != 0 : mat.doubleSided;
                    ctx->RSSetState(twoSided || !I.rasterCull ? I.raster : I.rasterCull);
                    if (!sh || !sh->ok) {
                        if (!I.fbVs || !I.fbPs || !I.fbIl || !I.fbCb) { if (!matSkipped[static_cast<size_t>(gs.material)]) { matSkipped[static_cast<size_t>(gs.material)] = 1; ++I.skipped; } continue; }
                        float cbv[(7 + 32 + 5) * 4] = {};
                        for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
                        const float lit[4] = {L[0], L[1], L[2], 0.0f};
                        const auto lin = [](float c) { return LinearLight() ? ToLinear(c) : c; };
                        const MmdLook* look = I.plain && gs.material >= 0 && static_cast<size_t>(gs.material) < I.looks.size() ? &I.looks[static_cast<size_t>(gs.material)] : nullptr;
                        if (look) {   // MMD's lit colour: diffuse times its light (154/255) plus ambient, at most 1; no two-step shade without a ramp
                            const auto amb = [](float d, float a) { return std::min(1.0f, d * 154.0f / 255.0f + a); };
                            const float dif4[4] = {lin(amb(look->diffuse.x, look->ambient.x)), lin(amb(look->diffuse.y, look->ambient.y)), lin(amb(look->diffuse.z, look->ambient.z)), look->diffuse.w};
                            std::memcpy(cbv + 16, dif4, 16);
                            cbv[20] = cbv[21] = cbv[22] = cbv[23] = 1.0f;
                            for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) cbv[(7 + 32 + r) * 4 + col] = LS[r][col];
                            cbv[(7 + 32 + 3) * 4] = selfShadow && (look->drawFlags & 0x08) ? 1.0f : 0.0f;
                            cbv[(7 + 32 + 3) * 4 + 3] = radius;
                            if (ID3D11ShaderResourceView* sp = I.lookSphere[static_cast<size_t>(gs.material)]) {
                                cbv[(7 + 32 + 4) * 4] = look->sphere == 1 ? 1.0f : 2.0f;
                                ctx->PSSetShaderResources(2, 1, &sp);
                            }
                        } else {
                            const float dif4[4] = {lin(mat.diffuse.x), lin(mat.diffuse.y), lin(mat.diffuse.z), mat.diffuse.w};
                            std::memcpy(cbv + 16, dif4, 16);
                            const float sh4[4] = {lin(mat.shadow.x), lin(mat.shadow.y), lin(mat.shadow.z), 1.0f};
                            std::memcpy(cbv + 20, sh4, 16);
                        }
                        std::memcpy(cbv + 24, lit, 16);
                        if (mat.toon.size() == static_cast<size_t>(assemble::kToonRows)) {   // the game shader's shade ramp instead of the two-step shadow tint
                            cbv[23] = 2.0f;
                            for (size_t k = 0; k < mat.toon.size(); ++k) {
                                cbv[28 + k * 4] = lin(mat.toon[k].x); cbv[29 + k * 4] = lin(mat.toon[k].y); cbv[30 + k * 4] = lin(mat.toon[k].z);
                            }
                        }
                        ctx->UpdateSubresource(I.fbCb, 0, nullptr, cbv, 0, 0);
                        ctx->IASetInputLayout(I.fbIl);
                        ctx->VSSetShader(I.fbVs, nullptr, 0);
                        ctx->PSSetShader(I.fbPs, nullptr, 0);
                        ctx->VSSetConstantBuffers(0, 1, &I.fbCb);
                        ctx->PSSetConstantBuffers(0, 1, &I.fbCb);
                        const GpuTex* ft = (mat.texture >= 0 && static_cast<size_t>(mat.texture) < I.textures.size() && I.textures[static_cast<size_t>(mat.texture)].srv) ? &I.textures[static_cast<size_t>(mat.texture)] : &I.defaults[0];
                        ctx->PSSetShaderResources(0, 1, &ft->srv);
                        ID3D11SamplerState* fs = I.samplerWrap;
                        ctx->PSSetSamplers(0, 1, &fs);
                        ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
                        ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
                        ctx->DrawIndexed(gs.count, 0, 0);
                        if (look && (look->drawFlags & 0x10) && look->edgeSize > 0.0f && I.olVs && I.olPs && I.olIl && I.olCb)   // the PMX edge
                            ring(gm, gs, {look->edgeColor.x, look->edgeColor.y, look->edgeColor.z}, 1.5f * look->edgeSize * H / 800.0f);
                        continue;
                    }
                    const size_t m = static_cast<size_t>(gs.material);
                    if (!matDone[m]) {   // constants once per material per frame
                        matDone[m] = 1;
                        for (int stage = 0; stage < 2; ++stage) {
                            const auto& prog = sh->progs[stage == 0 ? sh->vi : sh->pi];
                            auto& data = stage == 0 ? matVs[m] : matPs[m];
                            detail::FillShaderConstants(prog, LinearLight() ? detail::LinearColours(mat) : mat, L, camPos, data, &scene.globalProps);
                            for (const auto& cb : prog.cbuffers) {
                                if (cb.slot < 0 || cb.slot >= 8 || data[static_cast<size_t>(cb.slot)].empty()) continue;
                                auto& d = data[static_cast<size_t>(cb.slot)];
                                for (const auto& pm : cb.params)
                                    if (pm.isMatrix && (pm.name == "unity_MatrixVP" || pm.name == "glstate_matrix_mvp" || pm.name == "unity_MatrixMVP"))
                                        for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) d[pm.offset / 4 + static_cast<size_t>(col * 4 + r)] = M[r][col];   // column j of the matrix in float4 j
                                if (ID3D11Buffer* b = I.ConstantBuffer(static_cast<int>(m), stage, cb.slot, cb.size)) ctx->UpdateSubresource(b, 0, nullptr, d.data(), 0, 0);
                            }
                        }
                    }
                    ctx->IASetInputLayout(sh->il);
                    ctx->VSSetShader(sh->vs, nullptr, 0);
                    ctx->PSSetShader(sh->ps, nullptr, 0);
                    for (int stage = 0; stage < 2; ++stage) {
                        const auto& prog = sh->progs[stage == 0 ? sh->vi : sh->pi];
                        for (const auto& cb : prog.cbuffers) {
                            if (cb.slot < 0 || cb.slot >= 8) continue;
                            auto it = I.cbufs.find(std::make_tuple(static_cast<int>(m), stage, cb.slot));
                            if (it == I.cbufs.end() || !it->second) continue;
                            if (stage == 0) ctx->VSSetConstantBuffers(static_cast<UINT>(cb.slot), 1, &it->second);
                            else ctx->PSSetConstantBuffers(static_cast<UINT>(cb.slot), 1, &it->second);
                        }
                    }
                    for (const auto& tb : sh->progs[sh->pi].textures) {
                        bool clamp = false;
                        const GpuTex* t = I.TextureFor(mat, *sh, tb.name, clamp);
                        ctx->PSSetShaderResources(tb.reg, 1, &t->srv);
                        ID3D11SamplerState* ss = clamp ? I.samplerClamp : I.samplerWrap;
                        ctx->PSSetSamplers(tb.sampler, 1, &ss);
                    }
                    ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
                    ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
                    ctx->DrawIndexed(gs.count, 0, 0);
                    if (mat.outlineWidth > 0.0f && !mat.alpha && !mat.doubleSided && !I.cutout[m] && I.olVs && I.olPs && I.olIl && I.olCb && !std::getenv("CARDBRIDGE_NO_OUTLINE"))
                        ring(gm, gs, mat.outlineColor, 1.2f * std::min(2.0f, mat.outlineWidth) * H / 800.0f);   // ring width in pixels
                }
            }
        }

    ctx->RSSetState(I.raster);
    // Extra shading layers over what the game's shaders drew: the visible opaque submeshes again with the layer's pixel shader,
    // blended, depth-tested against the finished frame and never written.
    for (const auto& layer : I.layers) {
        if (!layer.ok || !layer.enabled) continue;
        ctx->OMSetDepthStencilState(I.depthOverlay, 0);
        ctx->OMSetBlendState(layer.blend, bf, 0xFFFFFFFFu);
        ctx->IASetInputLayout(layer.il);
        ctx->VSSetShader(layer.vs, nullptr, 0);
        ctx->PSSetShader(layer.ps, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &layer.cb);
        ctx->PSSetConstantBuffers(0, 1, &layer.cb);
        ID3D11SamplerState* ls = I.samplerWrap;
        ctx->PSSetSamplers(0, 1, &ls);
        const float seconds = static_cast<float>(GetTickCount64() % 1000000ull) / 1000.0f;
        for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
            if (!visible(pi)) continue;
            for (int mi : scene.parts[pi].meshes) {
                const GpuMesh& gm = I.meshes[static_cast<size_t>(mi)];
                if (!gm.vb) continue;
                ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
                for (const auto& gs : gm.subs) {
                    const auto& mat = scene.materials[static_cast<size_t>(gs.material)];
                    if (mat.alpha || !gs.ib || !gs.count) continue;
                    float cbv[36];
                    for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
                    const float lit[4] = {L[0], L[1], L[2], 0.0f}, vw[4] = {toCam[0], toCam[1], toCam[2], 0.0f};
                    const auto lin = [](float c) { return LinearLight() ? ToLinear(c) : c; };
                    const float dif4[4] = {lin(mat.diffuse.x), lin(mat.diffuse.y), lin(mat.diffuse.z), mat.diffuse.w};
                    const float sh4[4] = {lin(mat.shadow.x), lin(mat.shadow.y), lin(mat.shadow.z), 1.0f};
                    const float info[4] = {seconds, LinearLight() ? 1.0f : 0.0f, 0.0f, 0.0f};
                    std::memcpy(cbv + 16, lit, 16);
                    std::memcpy(cbv + 20, vw, 16);
                    std::memcpy(cbv + 24, dif4, 16);
                    std::memcpy(cbv + 28, sh4, 16);
                    std::memcpy(cbv + 32, info, 16);
                    ctx->UpdateSubresource(layer.cb, 0, nullptr, cbv, 0, 0);
                    const GpuTex* lt = (mat.texture >= 0 && static_cast<size_t>(mat.texture) < I.textures.size() && I.textures[static_cast<size_t>(mat.texture)].srv) ? &I.textures[static_cast<size_t>(mat.texture)] : &I.defaults[0];
                    ctx->PSSetShaderResources(0, 1, &lt->srv);
                    ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
                    ctx->DrawIndexed(gs.count, 0, 0);
                }
            }
        }
    }

    // Hovered component: tint its visible surface, then draw the same silhouette ring the game's outlines use in a warm
    // colour so the part pops out of the model. Depth-tested against the finished frame, never written.
    if (highlightPart >= 0 && static_cast<size_t>(highlightPart) < scene.parts.size() && visible(static_cast<size_t>(highlightPart)) &&
        I.olVs && I.olPs && I.olIl && I.olCb && I.depthOverlay && I.blendOverlay) {
        const auto& part = scene.parts[static_cast<size_t>(highlightPart)];
        ctx->OMSetDepthStencilState(I.depthOverlay, 0);
        ctx->OMSetBlendState(I.blendOverlay, bf, 0xFFFFFFFFu);
        ctx->IASetInputLayout(I.olIl);
        ctx->VSSetShader(I.olVs, nullptr, 0);
        ctx->PSSetShader(I.olPs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &I.olCb);
        ctx->PSSetConstantBuffers(0, 1, &I.olCb);
        for (int pass = 0; pass < 2; ++pass) {   // 0: translucent tint over the surface, 1: opaque ring outside it
            float cbv[24];
            for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
            const float colr[4] = {1.0f, LinearLight() ? ToLinear(0.62f) : 0.62f, LinearLight() ? ToLinear(0.16f) : 0.16f, pass == 0 ? 0.38f : 0.95f};
            const float px = 2.2f * H / 800.0f;
            const float prm[4] = {pass == 0 ? 0.0f : px * 2.0f / W, pass == 0 ? 0.0f : px * 2.0f / H,
                                  pass == 0 ? 0.0f : 0.004f, 0.0f};
            std::memcpy(cbv + 16, colr, 16);
            std::memcpy(cbv + 20, prm, 16);
            ctx->UpdateSubresource(I.olCb, 0, nullptr, cbv, 0, 0);
            for (int mi : part.meshes) {
                const GpuMesh& gm = I.meshes[static_cast<size_t>(mi)];
                if (!gm.vb) continue;
                ctx->IASetVertexBuffers(0, 1, &gm.vb, &stride, &offset);
                for (const auto& gs : gm.subs) {
                    if (!gs.ib || !gs.count) continue;
                    ctx->IASetIndexBuffer(gs.ib, DXGI_FORMAT_R32_UINT, 0);
                    ctx->DrawIndexed(gs.count, 0, 0);
                }
            }
        }
    }

    ctx->ResolveSubresource(I.resolve, 0, I.msaa, 0, LinearLight() ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM);
    ctx->CopyResource(I.staging, I.resolve);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(I.staging, 0, D3D11_MAP_READ, 0, &mp))) return false;
    for (int y = 0; y < H; ++y) std::memcpy(&bgra[static_cast<size_t>(y) * W], static_cast<const uint8_t*>(mp.pData) + static_cast<size_t>(y) * mp.RowPitch, static_cast<size_t>(W) * 4);
    ctx->Unmap(I.staging, 0);
    for (auto& px : bgra) px |= 0xFF000000u;
    if (!I.plain) ApplyColourGrade(bgra, 0xFFE2E4E8u);
    return true;
}

} // namespace cardbridge::exporter
