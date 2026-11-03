#include "export/GpuView.h"

#include "export/ColourGrade.h"

#include "shader/UnityShader.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
}
using detail::LinearLight;
using detail::ToLinear;

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct GVert { float pos[3], nrm[3], tan[4], uv[2], col[4], uv1[2]; };   // 72 bytes

struct GpuSub { ID3D11Buffer* ib = nullptr; UINT count = 0; int material = 0; };
struct GpuMesh { ID3D11Buffer* vb = nullptr; std::vector<GpuSub> subs; std::vector<GVert> rest; };   // rest: the vertices as loaded, for re-skinning

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
    const assemble::Scene* scene = nullptr;
    std::vector<GpuMesh> meshes;
    std::vector<GpuShader> shaders;
    std::vector<GpuTex> textures;                // by scene texture
    GpuTex defaults[4];                          // white, black, gray, bump
    ID3D11SamplerState* samplerWrap = nullptr;
    ID3D11SamplerState* samplerClamp = nullptr;
    ID3D11RasterizerState* raster = nullptr;
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
    float lastM[16] = {};          // world -> clip of the last Render, for Project()
    int lastW = 0, lastH = 0;
    bool plain = false;                          // see GpuView::SetPlain
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
cbuffer C : register(b0) { float4 r0, r1, r2, r3, diffuse, shadow, light; };
Texture2D t0 : register(t0); SamplerState s0 : register(s0);
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VO { float4 pos : SV_POSITION; float3 n : TEXCOORD1; float2 uv : TEXCOORD0; };
VO vs(VI i) { VO o; float4 p = float4(i.p, 1); o.pos = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p)); o.n = i.n; o.uv = i.uv; return o; }
float4 ps(VO i) : SV_Target {
    float4 c = t0.Sample(s0, i.uv) * diffuse;
    float nl = saturate(dot(normalize(i.n), light.xyz));
    float lit = saturate((nl - 0.1) / 0.3);
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
        bd.ByteWidth = 7 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &fbCb));
        Rel(vb); Rel(pb); Rel(err);
        return ok;
    }

    ~Impl() { ClearLayers(); Rel(fbVs); Rel(fbPs); Rel(fbIl); Rel(fbCb); Rel(olVs); Rel(olPs); Rel(olIl); Rel(olCb); Clear(); Rel(samplerWrap); Rel(samplerClamp); Rel(raster); Rel(depth); Rel(depthOverlay); Rel(blend); Rel(blendOverlay); for (auto& d : defaults) { Rel(d.srv); Rel(d.tex); } ReleaseTargets(); Rel(ctx); Rel(dev); }

    void ReleaseTargets() { Rel(rtv); Rel(dsv); Rel(msaa); Rel(resolve); Rel(staging); Rel(depthTex); rw = rh = 0; }

    void Clear() {
        for (auto& m : meshes) { Rel(m.vb); for (auto& s : m.subs) Rel(s.ib); }
        meshes.clear();
        for (auto& s : shaders) { Rel(s.vs); Rel(s.ps); Rel(s.il); }
        shaders.clear();
        for (auto& t : textures) { Rel(t.srv); Rel(t.tex); }
        textures.clear();
        for (auto& c : cbufs) Rel(c.second);
        cbufs.clear();
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
int GpuView::skippedMaterials() const { return impl_->skipped; }
void GpuView::SetPlain(bool on) { impl_->plain = on; }

void GpuView::SetSkin(const std::vector<Mat4>* skin) {
    Impl& I = *impl_;
    if (!I.dev || !I.scene) return;
    std::vector<GVert> posed;
    for (size_t mi = 0; mi < I.meshes.size() && mi < I.scene->meshes.size(); ++mi) {
        GpuMesh& g = I.meshes[mi];
        if (!g.vb || g.rest.empty()) continue;
        const auto& verts = I.scene->meshes[mi].vertices;
        const std::vector<GVert>* src = &g.rest;
        if (skin && !skin->empty()) {
            posed = g.rest;
            for (size_t v = 0; v < posed.size() && v < verts.size(); ++v) {
                // the vertex's blended skin matrix, applied to its position, normal and tangent
                Mat4 m;
                for (float& x : m.m) x = 0;
                float sum = 0;
                for (int k = 0; k < 4; ++k) {
                    const float w = verts[v].weight[k];
                    const int b = verts[v].bone[k];
                    if (w <= 0 || b < 0 || static_cast<size_t>(b) >= skin->size()) continue;
                    for (int e = 0; e < 16; ++e) m.m[e] += (*skin)[static_cast<size_t>(b)].m[e] * w;
                    sum += w;
                }
                if (sum <= 1e-6f) continue;
                for (float& x : m.m) x /= sum;
                GVert& o = posed[v];
                const Vec3 p = m.point({o.pos[0], o.pos[1], o.pos[2]});
                const Vec3 n = normalize(m.dir({o.nrm[0], o.nrm[1], o.nrm[2]}));
                const Vec3 t = normalize(m.dir({o.tan[0], o.tan[1], o.tan[2]}));
                o.pos[0] = p.x; o.pos[1] = p.y; o.pos[2] = p.z;
                o.nrm[0] = n.x; o.nrm[1] = n.y; o.nrm[2] = n.z;
                o.tan[0] = t.x; o.tan[1] = t.y; o.tan[2] = t.z;
            }
            src = &posed;
        }
        I.ctx->UpdateSubresource(g.vb, 0, nullptr, src->data(), 0, 0);
    }
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


bool GpuView::Init(std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    Impl& I = *impl_;
    if (I.dev) return true;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &I.dev, &got, &I.ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &I.dev, &got, &I.ctx);
    if (FAILED(hr)) { I.dev = nullptr; I.ctx = nullptr; return fail("no Direct3D 11 device"); }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8; sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    I.dev->CreateSamplerState(&sd, &I.samplerWrap);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    I.dev->CreateSamplerState(&sd, &I.samplerClamp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    I.dev->CreateRasterizerState(&rd, &I.raster);
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

bool GpuView::Load(const assemble::Scene& scene, std::string* why) {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    Impl& I = *impl_;
    if (!I.dev) return fail("no device");
    I.Clear();
    I.scene = &scene;

    // Shaders: the forward pass of each, straight from the bundle's DXBC.
    I.shaders.resize(scene.shaders.size());
    for (size_t i = 0; i < scene.shaders.size(); ++i) {
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
            else { e.Format = DXGI_FORMAT_R32G32_FLOAT; e.AlignedByteOffset = in.index == 1 ? offsetof(GVert, uv1) : offsetof(GVert, uv); }   // TEXCOORD1 is the second UV set, the rest read the first
            el.push_back(e);
        }
        if (FAILED(I.dev->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), vp.dxbc.data(), vp.dxbc.size(), &g.il))) { Rel(g.vs); Rel(g.ps); continue; }
        g.ok = true;
    }

    // Textures the shaders can read.
    I.textures.assign(scene.textures.size(), GpuTex{});
    auto upload = [&](int ti) {
        if (ti < 0 || static_cast<size_t>(ti) >= scene.textures.size() || I.textures[static_cast<size_t>(ti)].srv) return;
        const auto& t = scene.textures[static_cast<size_t>(ti)];
        if (t.width > 0 && t.height > 0 && t.rgba.size() >= static_cast<size_t>(t.width) * t.height * 4) I.MakeTexture(t.width, t.height, t.rgba.data(), I.textures[static_cast<size_t>(ti)], true, LinearLight() && t.srgb);
    };
    for (const auto& m : scene.materials) { for (const auto& tb : m.texProps) upload(tb.texture); upload(m.texture); }
    for (const auto& tb : scene.globalTextures) upload(tb.texture);

    // Geometry, with the tangents the shaders build their normal-map frame from.
    I.meshes.resize(scene.meshes.size());
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const auto& m = scene.meshes[mi];
        auto& g = I.meshes[mi];
        if (m.vertices.empty()) continue;
        std::vector<Vec3> tan = detail::ComputeMeshTangents(m);
        std::vector<GVert> vb(m.vertices.size());
        for (size_t v = 0; v < vb.size(); ++v) {
            const auto& s = m.vertices[v];
            vb[v] = {{s.pos.x, s.pos.y, s.pos.z}, {s.normal.x, s.normal.y, s.normal.z}, {tan[v].x, tan[v].y, tan[v].z, 1.0f}, {s.uv.x, s.uv.y}, {1, 1, 1, 1}, {s.uv1.x, s.uv1.y}};
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(vb.size() * sizeof(GVert)); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;   // default, not immutable: SetSkin rewrites it
        D3D11_SUBRESOURCE_DATA sr{vb.data(), 0, 0};
        if (FAILED(I.dev->CreateBuffer(&bd, &sr, &g.vb))) continue;
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

    I.skipped = 0;
    std::vector<char> matDone(scene.materials.size(), 0), matSkipped(scene.materials.size(), 0);
    std::vector<std::vector<std::vector<float>>> matVs(scene.materials.size()), matPs(scene.materials.size());
    UINT stride = sizeof(GVert), offset = 0;
    for (int pass = 0; pass < 2; ++pass)   // opaque materials first, then the alpha-flagged ones
        for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
            if (!visible(pi)) continue;
            for (int mi : scene.parts[pi].meshes) {
                const GpuMesh& gm = I.meshes[static_cast<size_t>(mi)];
                if (!gm.vb) continue;
                for (const auto& gs : gm.subs) {
                    const auto& mat = scene.materials[static_cast<size_t>(gs.material)];
                    if ((mat.alpha ? 1 : 0) != pass) continue;
                    const GpuShader* sh = (!I.plain && mat.shaderIndex >= 0 && static_cast<size_t>(mat.shaderIndex) < I.shaders.size()) ? &I.shaders[static_cast<size_t>(mat.shaderIndex)] : nullptr;
                    if (!sh || !sh->ok) {
                        if (!I.fbVs || !I.fbPs || !I.fbIl || !I.fbCb) { if (!matSkipped[static_cast<size_t>(gs.material)]) { matSkipped[static_cast<size_t>(gs.material)] = 1; ++I.skipped; } continue; }
                        float cbv[28];
                        for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
                        const float lit[4] = {L[0], L[1], L[2], 0.0f};
                        const auto lin = [](float c) { return LinearLight() ? ToLinear(c) : c; };
                        const float dif4[4] = {lin(mat.diffuse.x), lin(mat.diffuse.y), lin(mat.diffuse.z), mat.diffuse.w};
                        std::memcpy(cbv + 16, dif4, 16);
                        const float sh4[4] = {lin(mat.shadow.x), lin(mat.shadow.y), lin(mat.shadow.z), 1.0f};
                        std::memcpy(cbv + 20, sh4, 16);
                        std::memcpy(cbv + 24, lit, 16);
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
                    if (mat.outlineWidth > 0.0f && !mat.alpha && !mat.doubleSided && I.olVs && I.olPs && I.olIl && I.olCb && !std::getenv("CARDBRIDGE_NO_OUTLINE")) {
                        float cbv[24];
                        for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) cbv[r * 4 + col] = M[r][col];
                        const float colr[4] = {LinearLight() ? ToLinear(mat.outlineColor.x) : mat.outlineColor.x, LinearLight() ? ToLinear(mat.outlineColor.y) : mat.outlineColor.y,
                                               LinearLight() ? ToLinear(mat.outlineColor.z) : mat.outlineColor.z, 1.0f};
                        const float px = 1.2f * std::min(2.0f, mat.outlineWidth) * H / 800.0f;   // ring width in pixels
                        const float prm[4] = {px * 2.0f / W, px * 2.0f / H, 0.004f, 0.0f};
                        std::memcpy(cbv + 16, colr, 16);
                        std::memcpy(cbv + 20, prm, 16);
                        ctx->UpdateSubresource(I.olCb, 0, nullptr, cbv, 0, 0);
                        ctx->IASetInputLayout(I.olIl);
                        ctx->VSSetShader(I.olVs, nullptr, 0);
                        ctx->PSSetShader(I.olPs, nullptr, 0);
                        ctx->VSSetConstantBuffers(0, 1, &I.olCb);
                        ctx->PSSetConstantBuffers(0, 1, &I.olCb);
                        ctx->DrawIndexed(gs.count, 0, 0);
                    }
                }
            }
        }

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
