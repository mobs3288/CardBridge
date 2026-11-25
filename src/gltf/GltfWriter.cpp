#include "gltf/GltfWriter.h"
#include "export/FormatCommon.h"
#include "core/FileIo.h"
#include "core/Version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>

#ifndef CARDBRIDGE_VERSION
#define CARDBRIDGE_VERSION "unknown"
#endif

namespace cardbridge::gltf {

namespace {

using json = nlohmann::json;
using namespace cardbridge::exportcommon;

constexpr size_t kNoView = std::numeric_limits<size_t>::max();

struct BufferView { size_t offset = 0, length = 0; int target = 0; };
struct Accessor {
    size_t bufferView = kNoView;
    size_t byteOffset = 0;
    int componentType = 5126;
    std::string type = "SCALAR";
    size_t count = 0;
    std::vector<double> min, max;
    bool sparse = false;
    size_t sparseIndices = 0, sparseValues = 0, sparseCount = 0;
    int sparseIndexType = 5125;
};

struct TargetData {
    std::string name;
    std::vector<uint32_t> indices;
    std::vector<Vec3> deltas;
};

json Number(float v) {
    if (!std::isfinite(v)) return json(0.0f);
    return json(v);
}

json Vec4Json(const Vec4& v) { return json::array({Number(v.x), Number(v.y), Number(v.z), Number(v.w)}); }

void PutU32(Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 255));
    out.push_back(static_cast<uint8_t>((v >> 8) & 255));
    out.push_back(static_cast<uint8_t>((v >> 16) & 255));
    out.push_back(static_cast<uint8_t>((v >> 24) & 255));
}

class Document {
public:
    Document(const assemble::Scene& scene, const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result, bool embedded)
        : scene_(scene), opt_(opt), result_(result), embedded_(embedded),
          textures_(scene, opt.outDir, opt.exportTextures, opt.overwrite) {
        doc_["asset"] = {{"version", "2.0"}, {"generator", std::string("CardBridge ") + CARDBRIDGE_VERSION}};
        doc_["scene"] = 0;
        doc_["scenes"] = json::array({json{{"nodes", json::array()}}});
        doc_["nodes"] = json::array();
        doc_["meshes"] = json::array();
        doc_["materials"] = json::array();
        doc_["accessors"] = json::array();
        doc_["bufferViews"] = json::array();
        doc_["buffers"] = json::array();
        doc_["images"] = json::array();
        doc_["textures"] = json::array();
        doc_["samplers"] = json::array();
    }

    void Build() {
        CheckCancelled(opt_.cancelled);
        const float scale = opt_.scale > 0.0f && std::isfinite(opt_.scale) ? opt_.scale : 1.0f;

        // Samplers: clamp for ramp-like textures, repeat for everything else.
        samplerClamp_ = AddSampler(true);
        samplerRepeat_ = AddSampler(false);

        // Materials are created only for submeshes that are actually written.
        std::vector<int> materialMap(scene_.materials.size(), -1);
        std::vector<char> materialUsed(scene_.materials.size(), 0);
        for (const auto& mesh : scene_.meshes)
            for (const auto& sm : mesh.submeshes)
                if (sm.material >= 0 && static_cast<size_t>(sm.material) < scene_.materials.size())
                    materialUsed[static_cast<size_t>(sm.material)] = 1;
        for (size_t i = 0; i < scene_.materials.size(); ++i)
            if (materialUsed[i]) materialMap[i] = AddMaterial(scene_.materials[i]);

        BuildMeshes(scale, materialMap);
        BuildSkeleton(scale);

        doc_["buffers"].push_back(json{{"byteLength", bin_.size()}});
        if (!extensionsUsed_.empty()) doc_["extensionsUsed"] = extensionsUsed_;

        textures_.Flush();
        result_.textures += textures_.Entries().size();
        result_.materials = doc_["materials"].size();
        result_.bones = skinJointCount_;
        for (const auto& part : scene_.parts)
            if (part.includeInExport) ++result_.parts;
    }

    void WriteGlbFile(const std::string& path) {
        json doc = doc_;
        doc["buffers"][0].erase("uri");
        std::string text = doc.dump(2);
        const size_t jsonPad = (4 - (text.size() & 3)) & 3;
        text.append(jsonPad, ' ');
        std::vector<uint8_t> bin = bin_;
        const size_t binPad = (4 - (bin.size() & 3)) & 3;
        bin.insert(bin.end(), binPad, 0);

        Bytes out;
        out.reserve(12 + 8 + text.size() + 8 + bin.size());
        PutU32(out, 0x46546C67);                    // "glTF"
        PutU32(out, 2);
        PutU32(out, static_cast<uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
        PutU32(out, static_cast<uint32_t>(text.size()));
        PutU32(out, 0x4E4F534A);                    // JSON
        out.insert(out.end(), text.begin(), text.end());
        PutU32(out, static_cast<uint32_t>(bin.size()));
        PutU32(out, 0x004E4942);                    // BIN
        out.insert(out.end(), bin.begin(), bin.end());
        WriteAtomic(path, out.data(), out.size(), opt_.overwrite);
    }

    void WriteGltfFiles(const std::string& stem) {
        const std::string binName = stem + ".bin";
        doc_["buffers"][0]["uri"] = binName;
        WriteAtomic(fileio::Join(opt_.outDir, binName), bin_.data(), bin_.size(), opt_.overwrite);
        WriteAtomicText(fileio::Join(opt_.outDir, stem + ".gltf"), doc_.dump(2), opt_.overwrite);
    }

    const Bytes& Binary() const { return bin_; }
    json& DocumentJson() { return doc_; }

private:
    size_t Align() {
        while (bin_.size() & 3) bin_.push_back(0);
        return bin_.size();
    }

    size_t AddView(const uint8_t* data, size_t size, int target = 0) {
        const size_t offset = Align();
        BufferView v;
        v.offset = offset;
        v.length = size;
        v.target = target;
        if (data && size) bin_.insert(bin_.end(), data, data + size);
        doc_["bufferViews"].push_back(json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", size}});
        if (target) doc_["bufferViews"].back()["target"] = target;
        return doc_["bufferViews"].size() - 1;
    }

    size_t AddFloats(const std::vector<float>& values, int components, const char* type, int target, bool bounds) {
        if (components == 0 || values.empty()) return std::string::npos;
        const size_t view = AddView(reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(float), target);
        Accessor a;
        a.bufferView = view;
        a.componentType = 5126;
        a.type = type;
        a.count = values.size() / static_cast<size_t>(components);
        if (bounds) {
            a.min.assign(static_cast<size_t>(components), std::numeric_limits<double>::infinity());
            a.max.assign(static_cast<size_t>(components), -std::numeric_limits<double>::infinity());
            for (size_t i = 0; i < a.count; ++i)
                for (int c = 0; c < components; ++c) {
                    const double v = values[i * static_cast<size_t>(components) + static_cast<size_t>(c)];
                    a.min[static_cast<size_t>(c)] = std::min(a.min[static_cast<size_t>(c)], v);
                    a.max[static_cast<size_t>(c)] = std::max(a.max[static_cast<size_t>(c)], v);
                }
            for (auto& v : a.min) if (!std::isfinite(v)) v = 0.0;
            for (auto& v : a.max) if (!std::isfinite(v)) v = 0.0;
        }
        return PushAccessor(a);
    }

    size_t AddIndices(const std::vector<uint32_t>& indices, bool use16) {
        if (indices.empty()) return kNoView;
        size_t view = kNoView;
        Accessor a;
        if (use16) {
            std::vector<uint16_t> small(indices.size());
            for (size_t i = 0; i < indices.size(); ++i) small[i] = static_cast<uint16_t>(indices[i]);
            view = AddView(reinterpret_cast<const uint8_t*>(small.data()), small.size() * sizeof(uint16_t), 34963);
            a.componentType = 5123;
        } else {
            view = AddView(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(uint32_t), 34963);
            a.componentType = 5125;
        }
        a.bufferView = view;
        a.type = "SCALAR";
        a.count = indices.size();
        return PushAccessor(a);
    }

    // JOINTS_0 is a VEC4 of unsigned indices per vertex (not four SCALARs).
    size_t AddJoints(const std::vector<uint32_t>& joints, bool use16) {
        if (joints.empty()) return kNoView;
        size_t view = kNoView;
        Accessor a;
        if (use16) {
            std::vector<uint16_t> small(joints.size());
            for (size_t i = 0; i < joints.size(); ++i) small[i] = static_cast<uint16_t>(joints[i]);
            view = AddView(reinterpret_cast<const uint8_t*>(small.data()), small.size() * sizeof(uint16_t), 34962);
            a.componentType = 5123;
        } else {
            view = AddView(reinterpret_cast<const uint8_t*>(joints.data()), joints.size() * sizeof(uint32_t), 34962);
            a.componentType = 5125;
        }
        a.bufferView = view;
        a.type = "VEC4";
        a.count = joints.size() / 4;
        return PushAccessor(a);
    }

    size_t AddSparse(const TargetData& target, size_t vertexCount) {
        Accessor a;
        a.componentType = 5126;
        a.type = "VEC3";
        a.count = vertexCount;
        a.sparse = true;
        a.sparseCount = target.indices.size();
        a.sparseIndexType = vertexCount > 65535 ? 5125 : 5123;
        if (a.sparseIndexType == 5123) {
            std::vector<uint16_t> small(target.indices.size());
            for (size_t i = 0; i < target.indices.size(); ++i) small[i] = static_cast<uint16_t>(target.indices[i]);
            a.sparseIndices = AddView(reinterpret_cast<const uint8_t*>(small.data()), small.size() * sizeof(uint16_t));
        } else {
            a.sparseIndices = AddView(reinterpret_cast<const uint8_t*>(target.indices.data()), target.indices.size() * sizeof(uint32_t));
        }
        std::vector<float> values;
        values.reserve(target.deltas.size() * 3);
        // POSITION accessors must declare bounds, including morph targets. A sparse
        // accessor is zero everywhere else, so zero is always part of the range.
        a.min = {0.0, 0.0, 0.0};
        a.max = {0.0, 0.0, 0.0};
        for (const auto& d : target.deltas) {
            values.push_back(Number(d.x).get<float>());
            values.push_back(Number(d.y).get<float>());
            values.push_back(Number(d.z).get<float>());
            const double xyz[3] = {d.x, d.y, d.z};
            for (int c = 0; c < 3; ++c) {
                a.min[static_cast<size_t>(c)] = std::min(a.min[static_cast<size_t>(c)], xyz[c]);
                a.max[static_cast<size_t>(c)] = std::max(a.max[static_cast<size_t>(c)], xyz[c]);
            }
        }
        a.sparseValues = AddView(reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(float));
        return PushAccessor(a);
    }

    size_t PushAccessor(const Accessor& a) {
        json j = {{"componentType", a.componentType}, {"type", a.type}, {"count", a.count}};
        if (a.bufferView != kNoView) {
            j["bufferView"] = a.bufferView;
            if (a.byteOffset) j["byteOffset"] = a.byteOffset;
        }
        if (!a.min.empty()) { j["min"] = a.min; j["max"] = a.max; }
        if (a.sparse) {
            j["sparse"] = {{"count", a.sparseCount},
                           {"indices", {{"bufferView", a.sparseIndices}, {"componentType", a.sparseIndexType}}},
                           {"values", {{"bufferView", a.sparseValues}}}};
        }
        doc_["accessors"].push_back(std::move(j));
        return doc_["accessors"].size() - 1;
    }

    int AddSampler(bool clamp) {
        doc_["samplers"].push_back(json{{"magFilter", 9729}, {"minFilter", 9729},
                                        {"wrapS", clamp ? 33071 : 10497}, {"wrapT", clamp ? 33071 : 10497}});
        return static_cast<int>(doc_["samplers"].size() - 1);
    }

    int AddImage(int entry) {
        const auto& e = textures_.Entries()[static_cast<size_t>(entry)];
        json img;
        if (embedded_) {
            const size_t view = AddView(e.png.data(), e.png.size());
            img = {{"bufferView", view}, {"mimeType", "image/png"}};
        } else {
            img = {{"uri", e.file}};
        }
        const auto& src = scene_.textures[static_cast<size_t>(e.sceneIndex)];
        img["name"] = e.name;
        doc_["images"].push_back(std::move(img));
        const int imageIndex = static_cast<int>(doc_["images"].size() - 1);
        doc_["textures"].push_back(json{{"sampler", src.clamp ? samplerClamp_ : samplerRepeat_}, {"source", imageIndex}});
        return imageIndex;
    }

    int FindBinding(const assemble::SceneMaterial& mat, int texture, const char* preferred) const {
        int found = -1;
        for (const auto& b : mat.texProps) {
            if (b.texture != texture || b.texture < 0) continue;
            if (preferred && b.name == preferred) return static_cast<int>(&b - mat.texProps.data());
            if (found < 0) found = static_cast<int>(&b - mat.texProps.data());
        }
        return found;
    }

    int AddMaterial(const assemble::SceneMaterial& mat) {
        json m;
        m["name"] = mat.name.empty() ? "material" : mat.name;
        json pbr = {{"baseColorFactor", Vec4Json(mat.diffuse)}, {"metallicFactor", 0.0},
                    {"roughnessFactor", Number(RoughnessFromSpecular(mat.specularPower))}};

        int baseTex = mat.texture;
        if (baseTex < 0) {
            for (const auto& b : mat.texProps)
                if (b.name == "_MainTex" && b.texture >= 0) { baseTex = b.texture; break; }
        }
        int texIndex = textures_.Use(baseTex);
        if (texIndex >= 0) {
            const int image = AddImage(texIndex);
            const int coord = mat.mainUvFromUv1 ? 1 : 0;
            json base = {{"index", image}, {"texCoord", coord}};

            const int binding = FindBinding(mat, baseTex, "_MainTex");
            if (binding >= 0) {
                const auto& st = mat.texProps[static_cast<size_t>(binding)].st;
                if (std::fabs(st.x - 1.0f) > 1e-6f || std::fabs(st.y - 1.0f) > 1e-6f ||
                    std::fabs(st.z) > 1e-6f || std::fabs(st.w) > 1e-6f) {
                    // UVs are exported with v flipped; transform v accordingly.
                    base["extensions"] = {{"KHR_texture_transform",
                                           {{"offset", json::array({Number(st.z), Number(st.y + st.w)})},
                                            {"scale", json::array({Number(st.x), Number(-st.y)})}}}};
                    UseExtension("KHR_texture_transform");
                }
            }
            pbr["baseColorTexture"] = std::move(base);
        }
        m["pbrMetallicRoughness"] = std::move(pbr);

        float cutoff = 0.5f;
        const AlphaMode alpha = ClassifyAlpha(scene_, mat, cutoff);
        m["alphaMode"] = alpha == AlphaMode::Opaque ? "OPAQUE" : alpha == AlphaMode::Mask ? "MASK" : "BLEND";
        if (alpha == AlphaMode::Mask) m["alphaCutoff"] = Number(cutoff);
        m["doubleSided"] = mat.doubleSided;

        // Only emit an emissive factor when the authored material actually has the property.
        for (const auto& [name, value] : mat.props) {
            if (name == "_EmissionColor") {
                const float k = std::max({value.x, value.y, value.z});
                if (k > 1e-5f)
                    m["emissiveFactor"] = json::array({Number(value.x / k), Number(value.y / k), Number(value.z / k)});
                break;
            }
        }
        for (const auto& b : mat.texProps) {
            const std::string lower = ToLower(b.name);
            if ((lower.find("bump") != std::string::npos || lower.find("normal") != std::string::npos) && b.texture >= 0) {
                const int tex = textures_.Use(b.texture);
                if (tex >= 0) m["normalTexture"] = {{"index", AddImage(tex)}, {"texCoord", 0}};
                break;
            }
        }
        doc_["materials"].push_back(std::move(m));
        return static_cast<int>(doc_["materials"].size() - 1);
    }

    static std::string ToLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
    void UseExtension(const char* name) {
        if (std::find(extensionsUsed_.begin(), extensionsUsed_.end(), name) == extensionsUsed_.end())
            extensionsUsed_.push_back(name);
    }

    std::vector<TargetData> BuildTargets(size_t mi, float scale) const {
        const auto& mesh = scene_.meshes[mi];
        std::vector<TargetData> out;
        for (const auto& ex : scene_.expressions) {
            std::map<uint32_t, Vec3> acc;
            for (const auto& t : ex.targets) {
                if (t.mesh < 0 || static_cast<size_t>(t.mesh) != mi) continue;
                if (t.shape < 0 || static_cast<size_t>(t.shape) >= mesh.shapes.size()) continue;
                const auto& sh = mesh.shapes[static_cast<size_t>(t.shape)];
                const float k = (t.weight - sh.rest) * 0.01f;
                for (const auto& [vi, d] : sh.offsets) {
                    if (vi >= mesh.vertices.size()) continue;
                    acc[vi] = acc[vi] + d * k;
                }
            }
            TargetData target;
            target.name = ex.name.empty() ? "expression" : ex.name;
            for (const auto& [vi, d] : acc) {
                target.indices.push_back(vi);
                target.deltas.push_back(Delta(d, scale));
            }
            if (!target.indices.empty()) out.push_back(std::move(target));
        }
        for (const auto& sh : mesh.shapes) {
            if (sh.offsets.empty()) continue;
            TargetData target;
            target.name = mesh.name + "." + sh.name;
            const float k = (100.0f - sh.rest) * 0.01f;
            for (const auto& [vi, d] : sh.offsets) {
                if (vi >= mesh.vertices.size()) continue;
                target.indices.push_back(vi);
                target.deltas.push_back(Delta(d * k, scale));
            }
            if (!target.indices.empty()) out.push_back(std::move(target));
        }
        return out;
    }

    void BuildMeshes(float scale, const std::vector<int>& materialMap) {
        for (size_t mi = 0; mi < scene_.meshes.size(); ++mi) {
            CheckCancelled(opt_.cancelled);
            const auto& mesh = scene_.meshes[mi];
            if (mesh.vertices.empty()) continue;
            bool hasTriangles = false;
            for (const auto& sm : mesh.submeshes) hasTriangles = hasTriangles || sm.indices.size() >= 3;
            if (!hasTriangles) continue;

            const size_t n = mesh.vertices.size();
            std::vector<Vec3> pos(n), nrm(n);
            std::vector<Vec2> uv0(n), uv1(n), uv2(n), uv3(n);
            std::vector<float> joints(n * 4), weights(n * 4);
            for (size_t v = 0; v < n; ++v) {
                const auto& sv = mesh.vertices[v];
                pos[v] = Point(sv.pos, scale);
                nrm[v] = Direction(sv.normal);
                uv0[v] = {sv.uv.x, 1.0f - sv.uv.y};
                uv1[v] = {sv.uv1.x, 1.0f - sv.uv1.y};
                uv2[v] = {sv.uv2.x, 1.0f - sv.uv2.y};
                uv3[v] = {sv.uv3.x, 1.0f - sv.uv3.y};
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    int bone = sv.bone[k];
                    float w = sv.weight[k];
                    if (!std::isfinite(w) || w <= 0.0f || bone < 0 || static_cast<size_t>(bone) >= scene_.bones.size()) {
                        w = 0.0f;
                        bone = 0;
                    }
                    joints[v * 4 + static_cast<size_t>(k)] = static_cast<float>(bone);
                    weights[v * 4 + static_cast<size_t>(k)] = w;
                    sum += w;
                }
                if (sum > 1e-8f)
                    for (int k = 0; k < 4; ++k) weights[v * 4 + static_cast<size_t>(k)] /= sum;
                else
                    weights[v * 4] = 1.0f;
            }

            std::vector<uint32_t> allIndices;
            for (const auto& sm : mesh.submeshes)
                for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                    // The X mirror reverses orientation; swap two corners to keep normals outward.
                    allIndices.push_back(sm.indices[i]);
                    allIndices.push_back(sm.indices[i + 2]);
                    allIndices.push_back(sm.indices[i + 1]);
                }
            const bool use16 = n <= 65535;
            auto tangents = ComputeTangents(pos, nrm, uv0, allIndices);

            auto Floats3 = [&](const std::vector<Vec3>& src) {
                std::vector<float> out(src.size() * 3);
                for (size_t v = 0; v < src.size(); ++v) {
                    out[v * 3] = Number(src[v].x).get<float>();
                    out[v * 3 + 1] = Number(src[v].y).get<float>();
                    out[v * 3 + 2] = Number(src[v].z).get<float>();
                }
                return out;
            };
            auto Floats2 = [&](const std::vector<Vec2>& src) {
                std::vector<float> out(src.size() * 2);
                for (size_t v = 0; v < src.size(); ++v) {
                    out[v * 2] = Number(src[v].x).get<float>();
                    out[v * 2 + 1] = Number(src[v].y).get<float>();
                }
                return out;
            };

            json attributes = {
                {"POSITION", AddFloats(Floats3(pos), 3, "VEC3", 34962, true)},
                {"NORMAL", AddFloats(Floats3(nrm), 3, "VEC3", 34962, false)},
                {"TANGENT", AddFloats([&] {
                    std::vector<float> t(n * 4);
                    for (size_t v = 0; v < n; ++v) t[v * 4 + 0] = Number(tangents[v].t.x).get<float>(),
                        t[v * 4 + 1] = Number(tangents[v].t.y).get<float>(),
                        t[v * 4 + 2] = Number(tangents[v].t.z).get<float>(),
                        t[v * 4 + 3] = Number(tangents[v].t.w).get<float>();
                    return t;
                }(), 4, "VEC4", 34962, false)},
                {"TEXCOORD_0", AddFloats(Floats2(uv0), 2, "VEC2", 34962, false)},
                {"TEXCOORD_1", AddFloats(Floats2(uv1), 2, "VEC2", 34962, false)},
                {"TEXCOORD_2", AddFloats(Floats2(uv2), 2, "VEC2", 34962, false)},
                {"TEXCOORD_3", AddFloats(Floats2(uv3), 2, "VEC2", 34962, false)},
            };
            if (opt_.exportSkinning && opt_.exportArmature && !scene_.bones.empty()) {
                std::vector<uint32_t> jointU32(n * 4);
                for (size_t v = 0; v < n; ++v)
                    for (int k = 0; k < 4; ++k) jointU32[v * 4 + static_cast<size_t>(k)] = static_cast<uint32_t>(joints[v * 4 + static_cast<size_t>(k)]);
                attributes["JOINTS_0"] = AddJoints(jointU32, scene_.bones.size() <= 65535);
                attributes["WEIGHTS_0"] = AddFloats(weights, 4, "VEC4", 34962, false);
            }

            std::vector<TargetData> targets;
            if (opt_.exportMorphs) targets = BuildTargets(mi, scale);
            json primitiveTargets = json::array();
            for (const auto& t : targets) {
                const size_t acc = AddSparse(t, n);
                // glTF morph targets are a flat attribute dictionary, NOT an
                // {"attributes": {...}} wrapper (that is only for the primitive itself).
                primitiveTargets.push_back(json{{"POSITION", acc}});
            }

            json primitives = json::array();
            for (const auto& sm : mesh.submeshes) {
                if (sm.indices.size() < 3) continue;
                std::vector<uint32_t> tri;
                tri.reserve(sm.indices.size());
                for (size_t i = 0; i + 2 < sm.indices.size(); i += 3) {
                    tri.push_back(sm.indices[i]);
                    tri.push_back(sm.indices[i + 2]);
                    tri.push_back(sm.indices[i + 1]);
                }
                json prim = {{"attributes", attributes}, {"indices", AddIndices(tri, use16)}, {"mode", 4}};
                if (sm.material >= 0 && static_cast<size_t>(sm.material) < materialMap.size() && materialMap[static_cast<size_t>(sm.material)] >= 0)
                    prim["material"] = materialMap[static_cast<size_t>(sm.material)];
                if (!primitiveTargets.empty()) prim["targets"] = primitiveTargets;
                primitives.push_back(std::move(prim));
                result_.triangles += tri.size() / 3;
            }
            if (primitives.empty()) continue;

            json meshJson = {{"name", mesh.name.empty() ? ("mesh_" + std::to_string(mi)) : mesh.name},
                             {"primitives", std::move(primitives)}};
            if (!targets.empty()) {
                meshJson["weights"] = json::array();
                for (size_t t = 0; t < targets.size(); ++t) meshJson["weights"].push_back(0);
                json names = json::array();
                for (const auto& t : targets) names.push_back(t.name);
                meshJson["extras"] = {{"targetNames", std::move(names)}};
                result_.morphs += targets.size();
            }
            doc_["meshes"].push_back(std::move(meshJson));

            json node = {{"name", mesh.name.empty() ? ("mesh_" + std::to_string(mi)) : mesh.name},
                         {"mesh", doc_["meshes"].size() - 1}};
            meshNodes_.push_back(static_cast<int>(doc_["nodes"].size()));
            doc_["nodes"].push_back(std::move(node));
            doc_["scenes"][0]["nodes"].push_back(meshNodes_.back());
            result_.vertices += n;
        }
    }

    void BuildSkeleton(float scale) {
        if (!opt_.exportArmature || scene_.bones.empty() || meshNodes_.empty()) return;
        CheckCancelled(opt_.cancelled);

        const size_t nb = scene_.bones.size();
        std::vector<Mat4> worlds(nb);
        std::vector<int> parent(nb, -1);
        for (size_t i = 0; i < nb; ++i) {
            worlds[i] = WorldMatrix(scene_.bones[i].world, scale);
            const int p = scene_.bones[i].parent;
            parent[i] = (p >= 0 && static_cast<size_t>(p) < nb) ? p : -1;
        }

        const int container = static_cast<int>(doc_["nodes"].size());
        doc_["nodes"].push_back(json{{"name", "Skeleton"}, {"children", json::array()}});
        doc_["scenes"][0]["nodes"].push_back(container);
        std::vector<int> jointNodes(nb, -1);
        std::vector<std::vector<int>> children(nb);
        for (size_t i = 0; i < nb; ++i) {
            jointNodes[i] = static_cast<int>(doc_["nodes"].size());
            doc_["nodes"].push_back(json::object());   // filled below (children need all indices)
            if (parent[i] < 0) doc_["nodes"][static_cast<size_t>(container)]["children"].push_back(jointNodes[i]);
            else children[static_cast<size_t>(parent[i])].push_back(jointNodes[i]);
        }
        for (size_t i = 0; i < nb; ++i) {
            const Mat4 local = parent[i] < 0 ? worlds[i] : LocalMatrix(worlds[static_cast<size_t>(parent[i])], worlds[i]);
            float matrix[16];
            ToColumnMajor(local, matrix);
            json node = {{"name", scene_.bones[i].name.empty() ? ("bone_" + std::to_string(i)) : scene_.bones[i].name}};
            // The spec asks importers not to be given the identity matrix explicitly.
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            bool isIdentity = true;
            for (int k = 0; k < 16; ++k)
                if (std::fabs(matrix[k] - identity[k]) > 1e-7f) { isIdentity = false; break; }
            if (!isIdentity) {
                node["matrix"] = json::array();
                for (float v : matrix) node["matrix"].push_back(Number(v));
            }
            if (!children[i].empty()) node["children"] = children[i];
            doc_["nodes"][static_cast<size_t>(jointNodes[i])] = std::move(node);
        }

        if (!opt_.exportSkinning) return;
        std::vector<float> ibm(nb * 16);
        for (size_t i = 0; i < nb; ++i) {
            float m[16];
            ToColumnMajor(inverse(worlds[i]), m);
            for (int k = 0; k < 16; ++k) ibm[i * 16 + static_cast<size_t>(k)] = m[k];
        }
        const size_t ibmAcc = AddFloats(ibm, 16, "MAT4", 0, false);
        json skin = {{"joints", jointNodes}, {"inverseBindMatrices", ibmAcc}, {"skeleton", container}};
        doc_["skins"] = json::array({std::move(skin)});
        skinJointCount_ = nb;
        for (int node : meshNodes_) doc_["nodes"][static_cast<size_t>(node)]["skin"] = 0;
    }

    void WriteManifest(const std::string& file) {
        json j = {
            {"schema_version", 1},
            {"export", {{"format", opt_.format == exporter::ExportFormat::Glb ? "glb" : "gltf"},
                        {"file", file}, {"axes", "y-up, right-handed"}, {"scale", opt_.scale},
                        {"uv", "v flipped to top-left origin"}, {"facing", "+z"},
                        {"texture_mode", opt_.format == exporter::ExportFormat::Glb ? "embedded" : "copy_required_textures"},
                        {"textures_dir", "textures"}}},
            {"counts", {{"vertices", result_.vertices}, {"triangles", result_.triangles},
                        {"materials", result_.materials}, {"bones", result_.bones},
                        {"textures", result_.textures}, {"morphs", result_.morphs}}},
            {"warnings", result_.warnings},
            {"limitations", json::array({
                "Koikatsu toon shading is baked into textures; glTF materials are a documented metallic-roughness approximation",
                "no skeletal animation source data exists in the canonical character; no animations are exported",
                "vertex colours and morph normals are not present in the canonical character"
            })}
        };
        WriteAtomicText(fileio::Join(opt_.outDir, "cardbridge-export.json"), j.dump(2) + "\n", opt_.overwrite);
        result_.manifestPath = fileio::Join(opt_.outDir, "cardbridge-export.json");
    }

    const assemble::Scene& scene_;
    const exporter::ModelExportOptions& opt_;
    exporter::ModelExportResult& result_;
    bool embedded_ = false;
    TextureWriter textures_;
    json doc_;
    Bytes bin_;
    int samplerClamp_ = 0, samplerRepeat_ = 0;
    std::vector<std::string> extensionsUsed_;
    std::vector<int> meshNodes_;
    size_t skinJointCount_ = 0;

public:
    void Finish(const std::string& file) { WriteManifest(file); }
};

void Prepare(exporter::ModelExportResult& result, const exporter::ModelExportOptions& opt) {
    fileio::CreateDirectories(opt.outDir);
    result.folder = opt.outDir;
    if (opt.exportAnimation)
        result.warnings.push_back("no animation source data exists in the canonical character; --animation ignored");
}

} // namespace

void WriteGlb(const assemble::Scene& scene, const card::Card& card,
              const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result) {
    (void)card;
    Prepare(result, opt);
    Document doc(scene, opt, result, true);
    doc.Build();
    const std::string stem = SafeStem(opt.baseName.empty() ? scene.name : opt.baseName);
    const std::string file = stem + ".glb";
    doc.WriteGlbFile(fileio::Join(opt.outDir, file));
    doc.Finish(file);
    result.primaryPath = fileio::Join(opt.outDir, file);
}

void WriteGltf(const assemble::Scene& scene, const card::Card& card,
               const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result) {
    (void)card;
    Prepare(result, opt);
    Document doc(scene, opt, result, false);
    doc.Build();
    const std::string stem = SafeStem(opt.baseName.empty() ? scene.name : opt.baseName);
    const std::string file = stem + ".gltf";
    doc.WriteGltfFiles(stem);
    doc.Finish(file);
    result.primaryPath = fileio::Join(opt.outDir, file);
}

} // namespace cardbridge::gltf
