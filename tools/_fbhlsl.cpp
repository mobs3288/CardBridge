// Compiles the GpuView fallback HLSL strings with D3DCompile (no display needed).
#include <cstdio>
#include <string>
#include <windows.h>
#include <d3dcompiler.h>

int main() {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) { std::fprintf(stderr, "d3dcompiler_47.dll not found\n"); return 2; }
    auto compile = reinterpret_cast<HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**)>(GetProcAddress(dll, "D3DCompile"));
    if (!compile) { std::fprintf(stderr, "D3DCompile missing\n"); return 2; }

    // Extract the two HLSL strings from GpuView.cpp: MakeFallback and MakeDiagnostic are the R"HLSL( ... )HLSL" blocks.
    HANDLE f = CreateFileW(L"D:\\Project K\\cardbridge\\src\\export\\GpuView.cpp", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD fsz = GetFileSize(f, nullptr);
    std::string src(fsz, '\0');
    ReadFile(f, src.data(), fsz, nullptr, nullptr);
    CloseHandle(f);

    struct Entry { const char* name; };
    auto extract = [&](const char* tag) -> std::string {
        std::string begin = std::string("R\"HLSL(", 7);
        std::string::size_type a = src.find(tag);
        if (a == std::string::npos) return {};
        a = src.find("R\"HLSL(", a);
        if (a == std::string::npos) return {};
        a += 7;
        std::string::size_type b = src.find(")HLSL\"", a);
        if (b == std::string::npos) return {};
        return src.substr(a, b - a);
    };
    const std::string fallbackSrc = extract("static const char src[]");
    if (fallbackSrc.empty()) { std::fprintf(stderr, "extract failed\n"); return 2; }

    struct PS { const char* vs, *ps; };
    auto compileAll = [&](const char* what, const std::string& s) -> bool {
        bool ok = true;
        for (const char* kind : {"vs", "ps"}) {
            ID3DBlob *blob = nullptr, *err = nullptr;
            std::string target = std::string(kind) + std::string("_4_0");
            HRESULT hr = compile(s.data(), s.size(), what, nullptr, nullptr, kind, target.c_str(), 0, 0, &blob, &err);
            if (FAILED(hr)) {
                ok = false;
                std::fprintf(stderr, "[FAIL] %s %s: ", what, target.c_str());
                if (err && err->GetBufferPointer()) std::fprintf(stderr, "%s\n", (char*)err->GetBufferPointer()); else std::fprintf(stderr, "%lx\n", hr);
            } else {
                if (blob && blob->GetBufferSize()) std::fprintf(stderr, "[ok] %s %s: %zu bytes\n", what, target.c_str(), blob->GetBufferSize());
            }
            if (blob) blob->Release(); if (err) err->Release();
        }
        return ok;
    };
    bool ok = compileAll("fallback", fallbackSrc);
    return ok ? 0 : 1;
}
