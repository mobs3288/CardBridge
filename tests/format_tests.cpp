// Independent serializer checks for the glTF/GLB and OBJ/MTL export backends.
// No game files or Blender needed: the writers are fed a synthetic canonical Scene
// and their output is parsed back with a small, independent reader.

#include "export/Exporter.h"
#include "export/ModelExport.h"
#include "core/Bytes.h"
#include "core/FileIo.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#define FormatPid() _getpid()
#else
#include <unistd.h>
#define FormatPid() getpid()
#endif

#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace cardbridge;

int gRun = 0, gFailed = 0;

#define CHECK(cond) do { \
    ++gRun; \
    if (!(cond)) { ++gFailed; std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

void Test(const char* name, const std::function<void()>& body) {
    std::printf("[ RUN ] %s\n", name);
    try { body(); }
    catch (const std::exception& e) { ++gFailed; std::printf("  FAIL exception: %s\n", e.what()); }
}

std::string TempDir(const char* name) {
    const char* root = std::getenv("CARDBRIDGE_TEST_OUT");
    if (!root) root = std::getenv("TEMP");
    if (!root) root = ".";
    const std::string dir = std::string(root) + "/cardbridge_formats_" + name + "_" + std::to_string(FormatPid());
    fileio::CreateDirectories(dir);
    return dir;
}

uint32_t U32(const Bytes& b, size_t at) {
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

assemble::Scene MakeScene() {
    assemble::Scene s;
    s.name = "Test Character";

    assemble::SceneBone root;
    root.name = "root";
    root.parent = -1;
    root.pos = {0, 0, 0};
    root.world = Mat4::TRS({0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1});
    s.bones.push_back(root);
    assemble::SceneBone spine;
    spine.name = "spine";
    spine.parent = 0;
    spine.pos = {0, 1, 0};
    spine.world = Mat4::TRS({0, 1, 0}, {0, 0, 0, 1}, {1, 1, 1});
    s.bones.push_back(spine);

    assemble::SceneTexture tex;
    tex.name = "body";
    tex.width = 2;
    tex.height = 2;
    tex.rgba = {255, 200, 180, 255, 255, 200, 180, 255, 255, 200, 180, 255, 255, 200, 180, 255};
    tex.srgb = true;
    tex.pixelHash = Fnv1a64(tex.rgba.data(), tex.rgba.size());
    s.textures.push_back(tex);

    assemble::SceneMaterial mat;
    mat.name = "skin";
    mat.texture = 0;
    mat.diffuse = {1, 1, 1, 1};
    mat.specularPower = 5.0f;
    s.materials.push_back(mat);

    assemble::SceneMesh mesh;
    mesh.name = "body";
    mesh.part = 0;
    const Vec3 positions[4] = {{-1, 0, 0}, {1, 0, 0}, {-1, 2, 0}, {1, 2, 0}};
    const Vec2 uvs[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        assemble::SceneVertex v;
        v.pos = positions[i];
        v.normal = {0, 0, 1};
        v.uv = uvs[i];
        v.uv1 = v.uv; v.uv2 = v.uv; v.uv3 = v.uv;
        v.bone[0] = 0; v.weight[0] = 0.6f;
        v.bone[1] = 1; v.weight[1] = 0.4f;
        mesh.vertices.push_back(v);
    }
    assemble::SceneSubmesh sm;
    sm.material = 0;
    sm.indices = {0, 1, 2, 1, 3, 2};
    mesh.submeshes.push_back(sm);

    assemble::SceneBlendShape shape;
    shape.name = "smile";
    shape.rest = 0.0f;
    shape.offsets.push_back({2, {0, 0.1f, 0}});
    mesh.shapes.push_back(shape);
    s.meshes.push_back(mesh);

    assemble::SceneExpression ex;
    ex.name = "Joy";
    ex.panel = 3;
    ex.shape = "smile";
    ex.targets.push_back({0, 0, 100.0f});
    s.expressions.push_back(ex);

    assemble::ScenePart part;
    part.id = "body";
    part.label = "Body";
    part.includeInExport = true;
    part.meshes = {0};
    s.parts.push_back(part);
    return s;
}

exporter::ModelExportOptions Options(exporter::ExportFormat format, const std::string& dir) {
    exporter::ModelExportOptions o;
    o.format = format;
    o.outDir = dir;
    o.baseName = "testchar";
    return o;
}

struct GlbParts {
    json doc;
    Bytes bin;
};

GlbParts ReadGlb(const std::string& path) {
    const Bytes b = fileio::ReadFile(path);
    CHECK(b.size() >= 20);
    CHECK(U32(b, 0) == 0x46546C67);
    CHECK(U32(b, 4) == 2);
    CHECK(U32(b, 8) == b.size());
    const uint32_t jsonLen = U32(b, 12);
    CHECK(U32(b, 16) == 0x4E4F534A);
    CHECK(b.size() >= 20 + jsonLen + 8);
    GlbParts out;
    out.doc = json::parse(std::string(reinterpret_cast<const char*>(b.data() + 20), jsonLen));
    const size_t binHeader = 20 + jsonLen;
    const uint32_t binLen = U32(b, binHeader);
    CHECK(U32(b, binHeader + 4) == 0x004E4942);
    CHECK(b.size() >= binHeader + 8 + binLen);
    out.bin.assign(b.begin() + binHeader + 8, b.begin() + binHeader + 8 + binLen);
    return out;
}

int AccessorCount(const json& doc, size_t index) {
    return doc["accessors"].at(index).at("count").get<int>();
}

void TestGlb() {
    const std::string dir = TempDir("glb");
    auto scene = MakeScene();
    auto result = exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Glb, dir));
    CHECK(result.vertices == 4);
    CHECK(result.triangles == 2);
    CHECK(result.bones == 2);
    CHECK(result.textures == 1);
    CHECK(result.morphs >= 1);
    CHECK(fileio::Exists(fileio::Join(dir, "testchar.glb")));
    CHECK(fileio::Exists(fileio::Join(dir, "cardbridge-export.json")));

    const GlbParts g = ReadGlb(fileio::Join(dir, "testchar.glb"));
    CHECK(g.doc["asset"]["version"] == "2.0");
    CHECK(g.doc.contains("scene") && !g.doc["scenes"].empty());
    CHECK(g.doc["buffers"].size() == 1);
    CHECK(!g.doc["buffers"][0].contains("uri"));
    CHECK(g.doc["buffers"][0]["byteLength"].get<size_t>() <= g.bin.size());
    CHECK(g.doc["accessors"].size() >= 8);
    CHECK(!g.doc.contains("animations"));   // never fabricate animation data

    const auto& prim = g.doc["meshes"][0]["primitives"][0];
    CHECK(prim["mode"] == 4);
    CHECK(AccessorCount(g.doc, prim["attributes"]["POSITION"].get<size_t>()) == 4);
    CHECK(AccessorCount(g.doc, prim["indices"].get<size_t>()) == 6);
    CHECK(g.doc["accessors"][prim["attributes"]["POSITION"].get<size_t>()]["type"] == "VEC3");
    CHECK(g.doc["accessors"][prim["attributes"]["TANGENT"].get<size_t>()]["type"] == "VEC4");
    CHECK(g.doc["accessors"][prim["attributes"]["JOINTS_0"].get<size_t>()]["count"] == 4);
    CHECK(g.doc["accessors"][prim["attributes"]["WEIGHTS_0"].get<size_t>()]["count"] == 4);
    CHECK(g.doc["textures"].size() == 1);
    CHECK(g.doc["images"][0]["mimeType"] == "image/png");
    CHECK(g.doc["images"][0].contains("bufferView"));
    CHECK(g.doc["skins"].size() == 1);
    CHECK(g.doc["skins"][0]["joints"].size() == 2);
    CHECK(AccessorCount(g.doc, g.doc["skins"][0]["inverseBindMatrices"].get<size_t>()) == 2);
    CHECK(g.doc["nodes"].size() >= 4);
    CHECK(prim.contains("targets"));
    CHECK(g.doc["meshes"][0]["extras"]["targetNames"].size() >= 1);
    CHECK(g.doc["meshes"][0]["weights"].size() == prim["targets"].size());

    // The X mirror must be applied to geometry: original x range [-1,1] stays [-1,1]
    // but the per-vertex mapping is negated; check a corner explicitly via min/max and the first vertex.
    const auto& posAcc = g.doc["accessors"][prim["attributes"]["POSITION"].get<size_t>()];
    CHECK(posAcc["min"][0] == -1.0 && posAcc["max"][0] == 1.0);
    CHECK(posAcc["min"][1] == 0.0 && posAcc["max"][1] == 2.0);
    const auto& posView = g.doc["bufferViews"][posAcc["bufferView"].get<size_t>()];
    const size_t off = posView["byteOffset"].get<size_t>();
    float firstX = 0.0f;
    std::memcpy(&firstX, g.bin.data() + off, 4);
    CHECK(firstX > 0.9f);   // scene x=-1 becomes glTF x=+1
}

void TestGltf() {
    const std::string dir = TempDir("gltf");
    auto scene = MakeScene();
    auto result = exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Gltf, dir));
    const std::string model = fileio::Join(dir, "testchar.gltf");
    CHECK(fileio::Exists(model));
    CHECK(fileio::Exists(result.primaryPath));
    const Bytes modelBytes = fileio::ReadFile(model);
    const std::string text(reinterpret_cast<const char*>(modelBytes.data()), modelBytes.size());
    const json doc = json::parse(text);
    CHECK(doc["asset"]["version"] == "2.0");
    CHECK(doc["buffers"][0].contains("uri"));
    const std::string binName = doc["buffers"][0]["uri"].get<std::string>();
    const std::string binPath = fileio::Join(dir, binName);
    CHECK(fileio::Exists(binPath));
    CHECK(fileio::FileSize(binPath) >= doc["buffers"][0]["byteLength"].get<uint64_t>());
    CHECK(doc["images"][0].contains("uri"));
    CHECK(!doc["images"][0].contains("bufferView"));
    const std::string uri = doc["images"][0]["uri"].get<std::string>();
    CHECK(uri.find("textures/") == 0);
    const std::string texPath = fileio::Join(dir, uri);
    CHECK(fileio::Exists(texPath));
    int w = 0, h = 0, channels = 0;
    const Bytes png = fileio::ReadFile(texPath);
    CHECK(stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels) != 0);
    CHECK(w == 2 && h == 2);
}

void TestObj() {
    const std::string dir = TempDir("obj");
    auto scene = MakeScene();
    auto result = exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Obj, dir));
    CHECK(result.vertices == 4);
    CHECK(result.triangles == 2);
    CHECK(result.materials == 1);
    CHECK(result.textures == 1);
    CHECK(!result.warnings.empty());   // static format must report what it cannot carry

    const std::string path = fileio::Join(dir, "testchar.obj");
    CHECK(fileio::Exists(path));
    CHECK(fileio::Exists(fileio::Join(dir, "testchar.mtl")));
    const Bytes objBytes = fileio::ReadFile(path);
    const std::string text(reinterpret_cast<const char*>(objBytes.data()), objBytes.size());
    std::istringstream in(text);
    std::string line;
    int v = 0, vt = 0, vn = 0, f = 0, maxIndex = 0;
    bool hasMtlLib = false, hasUseMtl = false, hasGroup = false;
    while (std::getline(in, line)) {
        if (line.rfind("v ", 0) == 0) ++v;
        else if (line.rfind("vt ", 0) == 0) ++vt;
        else if (line.rfind("vn ", 0) == 0) ++vn;
        else if (line.rfind("f ", 0) == 0) {
            ++f;
            std::istringstream fs(line);
            std::string tok;
            fs >> tok;
            while (fs >> tok) {
                const size_t slash = tok.find('/');
                CHECK(slash != std::string::npos);
                const int index = std::atoi(tok.substr(0, slash).c_str());
                maxIndex = std::max(maxIndex, index);
            }
        } else if (line.rfind("mtllib ", 0) == 0) hasMtlLib = true;
        else if (line.rfind("usemtl ", 0) == 0) hasUseMtl = true;
        else if (line.rfind("g ", 0) == 0) hasGroup = true;
    }
    CHECK(v == 4 && vt == 4 && vn == 4 && f == 2);
    CHECK(maxIndex > 0 && maxIndex <= v);
    CHECK(hasMtlLib && hasUseMtl && hasGroup);

    const Bytes mtlBytes = fileio::ReadFile(fileio::Join(dir, "testchar.mtl"));
    const std::string mtlText(reinterpret_cast<const char*>(mtlBytes.data()), mtlBytes.size());
    CHECK(mtlText.find("newmtl skin") != std::string::npos);
    CHECK(mtlText.find("map_Kd textures/") != std::string::npos);
}

void TestErrorsAndCancellation() {
    // A malformed texture must fail before any output file appears.
    {
        const std::string dir = TempDir("bad");
        auto scene = MakeScene();
        scene.textures[0].width = 0;
        bool threw = false;
        try { exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Glb, dir)); }
        catch (const FormatError&) { threw = true; }
        CHECK(threw);
        CHECK(!fileio::Exists(fileio::Join(dir, "testchar.glb")));
        CHECK(!fileio::Exists(fileio::Join(dir, "testchar.glb.tmp")));
    }
    // Existing output is never replaced unless overwrite is requested.
    {
        const std::string dir = TempDir("overwrite");
        auto scene = MakeScene();
        exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Glb, dir));
        bool threw = false;
        try { exporter::ExportModel(scene, {}, Options(exporter::ExportFormat::Glb, dir)); }
        catch (const FormatError&) { threw = true; }
        CHECK(threw);
        auto opt = Options(exporter::ExportFormat::Glb, dir);
        opt.overwrite = true;
        exporter::ExportModel(scene, {}, opt);   // succeeds on the second attempt
    }
    // Cancellation is cooperative and must throw the dedicated type.
    {
        const std::string dir = TempDir("cancel");
        auto scene = MakeScene();
        auto opt = Options(exporter::ExportFormat::Glb, dir);
        opt.cancelled = [] { return true; };
        bool threw = false;
        try { exporter::ExportModel(scene, {}, opt); }
        catch (const Cancelled&) { threw = true; }
        CHECK(threw);
    }
}

void TestFormatNames() {
    exporter::ExportFormat f = exporter::ExportFormat::Pmx;
    CHECK(exporter::ParseExportFormat("GLB", f) && f == exporter::ExportFormat::Glb);
    CHECK(exporter::ParseExportFormat("gltf", f) && f == exporter::ExportFormat::Gltf);
    CHECK(exporter::ParseExportFormat("obj", f) && f == exporter::ExportFormat::Obj);
    CHECK(exporter::ParseExportFormat("pmx", f) && f == exporter::ExportFormat::Pmx);
    CHECK(!exporter::ParseExportFormat("fbx", f));
    CHECK(std::string(exporter::FormatExtension(exporter::ExportFormat::Gltf)) == ".gltf");
}

} // namespace

int main() {
    Test("glb serializer round-trip", TestGlb);
    Test("gltf external resources", TestGltf);
    Test("obj/mtl serializer round-trip", TestObj);
    Test("format errors, overwrite and cancellation", TestErrorsAndCancellation);
    Test("format names", TestFormatNames);
    std::printf("\n%d checks, %d failed\n", gRun, gFailed);
    return gFailed ? 1 : 0;
}
