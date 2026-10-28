#include "unity/UnityScene.h"

#include "unity/TextureDecode.h"

#include <algorithm>
#include <cstring>

namespace cardbridge::unity {

namespace {

PPtr toPPtr(const Value& v) {
    PPtr p;
    p.fileId = static_cast<int32_t>(v.at("m_FileID").asInt());
    p.pathId = v.at("m_PathID").asInt();
    return p;
}

Vec3 toVec3(const Value& v) {
    return {static_cast<float>(v.at("x").asFloat()), static_cast<float>(v.at("y").asFloat()), static_cast<float>(v.at("z").asFloat())};
}

// Field names for fixed arrays differ between Unity versions: "weight[0]" in the
// type tree, "weight_0_" in some dumps.
const Value& indexed(const Value& v, const char* base, int i) {
    std::string a = std::string(base) + "[" + std::to_string(i) + "]";
    if (v.has(a)) return v.at(a);
    return v.at(std::string(base) + "_" + std::to_string(i) + "_");
}

size_t channelFormatSize(int format) {
    switch (format) {
    case 0: return 4;   // float32
    case 1: return 2;   // float16
    case 2: return 1;   // colour, 4 x uint8
    case 3: return 1;   // byte
    case 4: return 4;   // uint32
    default: throw FormatError("unknown vertex channel format " + std::to_string(format));
    }
}

float halfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f;
    if (e == 0) f = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
    return s ? -f : f;
}

} // namespace

UnityFile::UnityFile(std::shared_ptr<UnityBundle> bundle) : bundle_(std::move(bundle)) {
    const auto* node = bundle_->serializedNode();
    if (!node) throw FormatError(bundle_->name() + ": bundle has no serialized file");
    sf_ = std::make_unique<SerializedFile>(bundle_, *node);

    for (const auto* o : sf_->ofClass(cls::GameObject)) {
        Value v = sf_->read(*o);
        goNames_[o->pathId] = v.at("m_Name").asStr();
        auto& comps = goComponents_[o->pathId];
        for (const auto& c : v.at("m_Component").asArray()) {
            const Value& pp = c.has("component") ? c.at("component") : c.at("second");
            PPtr p = toPPtr(pp);
            if (!p.isLocal() || p.isNull()) continue;
            const ObjectInfo* co = sf_->find(p.pathId);
            if (co) comps.emplace_back(co->classId, p.pathId);
        }
    }
    for (const auto* o : sf_->ofClass(cls::Transform)) {
        Value v = sf_->read(*o);
        Xform x;
        x.id = o->pathId;
        x.goId = toPPtr(v.at("m_GameObject")).pathId;
        auto it = goNames_.find(x.goId);
        x.name = it == goNames_.end() ? std::string() : it->second;
        x.pos = toVec3(v.at("m_LocalPosition"));
        const Value& q = v.at("m_LocalRotation");
        x.rot = {static_cast<float>(q.at("x").asFloat()), static_cast<float>(q.at("y").asFloat()),
                 static_cast<float>(q.at("z").asFloat()), static_cast<float>(q.at("w").asFloat())};
        x.scl = toVec3(v.at("m_LocalScale"));
        x.parent = toPPtr(v.at("m_Father")).pathId;
        for (const auto& c : v.at("m_Children").asArray()) x.children.push_back(toPPtr(c).pathId);
        goToXform_[x.goId] = x.id;
        xforms_[x.id] = std::move(x);
    }
}

const Xform* UnityFile::xform(int64_t id) const {
    auto it = xforms_.find(id);
    return it == xforms_.end() ? nullptr : &it->second;
}

std::vector<int64_t> UnityFile::findXformsByName(const std::string& goName) const {
    std::vector<int64_t> out;
    for (const auto& kv : xforms_)
        if (kv.second.name == goName) out.push_back(kv.first);
    return out;
}

Mat4 UnityFile::worldMatrix(int64_t xformId, const std::map<int64_t, Mat4>* localOverride) const {
    std::vector<const Xform*> chain;
    for (const Xform* x = xform(xformId); x && chain.size() < 4096; x = xform(x->parent)) chain.push_back(x);
    Mat4 m;
    for (size_t i = chain.size(); i-- > 0;) {
        if (localOverride) {
            auto it = localOverride->find(chain[i]->id);
            if (it != localOverride->end()) { m = m * it->second; continue; }
        }
        m = m * Mat4::TRS(chain[i]->pos, chain[i]->rot, chain[i]->scl);
    }
    return m;
}

std::vector<int64_t> UnityFile::subtree(int64_t rootId) const {
    std::vector<int64_t> out;
    std::vector<int64_t> stack{rootId};
    while (!stack.empty() && out.size() < 1000000) {
        int64_t id = stack.back();
        stack.pop_back();
        const Xform* x = xform(id);
        if (!x) continue;
        out.push_back(id);
        for (auto it = x->children.rbegin(); it != x->children.rend(); ++it) stack.push_back(*it);
    }
    return out;
}

std::string UnityFile::pathOf(int64_t xformId) const {
    std::string s;
    for (const Xform* x = xform(xformId); x; x = xform(x->parent)) s = x->name + (s.empty() ? "" : "/" + s);
    return s;
}

std::vector<RendererInfo> UnityFile::renderersUnder(int64_t rootId) const {
    std::vector<RendererInfo> out;
    for (int64_t xid : subtree(rootId)) {
        const Xform* x = xform(xid);
        auto cit = goComponents_.find(x->goId);
        if (cit == goComponents_.end()) continue;
        PPtr filterMesh;
        for (auto& [cid, pid] : cit->second) {
            if (cid == cls::MeshFilter) filterMesh = toPPtr(sf_->read(*sf_->find(pid)).at("m_Mesh"));
        }
        for (auto& [cid, pid] : cit->second) {
            if (cid != cls::SkinnedMeshRenderer && cid != cls::MeshRenderer) continue;
            Value v = sf_->read(*sf_->find(pid));
            RendererInfo r;
            r.skinned = cid == cls::SkinnedMeshRenderer;
            r.enabled = v.at("m_Enabled").asBool(true);
            r.goId = x->goId;
            r.xformId = xid;
            r.componentId = pid;
            r.name = x->name;
            r.mesh = r.skinned ? toPPtr(v.at("m_Mesh")) : filterMesh;
            for (const auto& m : v.at("m_Materials").asArray()) r.materials.push_back(toPPtr(m));
            if (r.skinned)
                for (const auto& b : v.at("m_Bones").asArray()) {
                    PPtr p = toPPtr(b);
                    r.boneXforms.push_back(p.isLocal() ? p.pathId : 0);
                }
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<MonoInfo> UnityFile::monoBehavioursUnder(int64_t rootId) const {
    std::vector<MonoInfo> out;
    std::map<int64_t, std::string> scriptNames;
    for (int64_t xid : subtree(rootId)) {
        const Xform* x = xform(xid);
        auto cit = goComponents_.find(x->goId);
        if (cit == goComponents_.end()) continue;
        for (auto& [cid, pid] : cit->second) {
            if (cid != cls::MonoBehaviour) continue;
            Value v = sf_->read(*sf_->find(pid));
            PPtr sp = toPPtr(v.at("m_Script"));
            std::string cname;
            if (sp.isLocal() && !sp.isNull()) {
                auto it = scriptNames.find(sp.pathId);
                if (it == scriptNames.end()) {
                    std::string n;
                    if (const ObjectInfo* so = sf_->find(sp.pathId)) n = sf_->read(*so).at("m_ClassName").asStr();
                    it = scriptNames.emplace(sp.pathId, n).first;
                }
                cname = it->second;
            }
            out.push_back({cname, x->goId, std::move(v)});
        }
    }
    return out;
}

PPtr UnityFile::findAsset(const std::string& name) const {
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; };
    std::string want = lower(name);
    for (const auto* o : sf_->ofClass(142)) {   // AssetBundle
        Value v = sf_->read(*o);
        for (const auto& e : v.at("m_Container").asArray()) {
            std::string path = lower(e.at("first").asStr());
            std::string stem = path;
            auto sl = stem.find_last_of('/');
            if (sl != std::string::npos) stem = stem.substr(sl + 1);
            auto dot = stem.find_last_of('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            if (stem == want || path == want) return toPPtr(e.at("second").at("asset"));
        }
    }
    return {};
}

PPtr UnityFile::findTexture(const std::string& name) const {
    for (const auto* o : sf_->ofClass(cls::Texture2D))
        if (sf_->peekName(*o) == name) {
            PPtr p;
            p.pathId = o->pathId;
            return p;
        }
    return {};
}

bool UnityFile::isUnderGameObject(int64_t xformId, const std::vector<int64_t>& goIds) const {
    for (const Xform* x = xform(xformId); x; x = xform(x->parent))
        if (std::find(goIds.begin(), goIds.end(), x->goId) != goIds.end()) return true;
    return false;
}

std::string UnityFile::externalName(const PPtr& p) const {
    const auto& ex = sf_->externals();
    if (p.fileId > 0 && static_cast<size_t>(p.fileId) <= ex.size()) return ex[static_cast<size_t>(p.fileId) - 1];
    return "external#" + std::to_string(p.fileId);
}

std::optional<MeshData> UnityFile::readMesh(const PPtr& p, std::string* why) const {
    auto fail = [&](const std::string& m) -> std::optional<MeshData> { if (why) *why = m; return std::nullopt; };
    if (p.isNull()) return fail("no mesh assigned");
    if (!p.isLocal()) return fail("mesh is in another bundle (" + externalName(p) + ")");
    const ObjectInfo* o = sf_->find(p.pathId);
    if (!o || o->classId != cls::Mesh) return fail("mesh object not found");
    Value v = sf_->read(*o);

    if (v.at("m_MeshCompression").asInt() != 0) return fail("compressed meshes are not supported");
    MeshData m;
    m.name = v.at("m_Name").asStr();

    const Value& vd = v.at("m_VertexData");
    size_t count = static_cast<size_t>(vd.at("m_VertexCount").asInt());
    const Bytes& data = vd.at("m_DataSize").asBin();
    const auto& chans = vd.at("m_Channels").asArray();
    struct Ch { int stream = 0, offset = 0, format = 0, dim = 0; };
    std::vector<Ch> ch;
    int streamCount = 0;
    for (const auto& c : chans) {
        Ch k{static_cast<int>(c.at("stream").asInt()), static_cast<int>(c.at("offset").asInt()),
             static_cast<int>(c.at("format").asInt()), static_cast<int>(c.at("dimension").asInt()) & 0xf};
        ch.push_back(k);
        if (k.dim > 0) streamCount = std::max(streamCount, k.stream + 1);
    }
    std::vector<size_t> sOff(static_cast<size_t>(streamCount)), sStride(static_cast<size_t>(streamCount));
    try {
        size_t off = 0;
        for (int s = 0; s < streamCount; ++s) {
            size_t stride = 0;
            for (const auto& k : ch)
                if (k.stream == s && k.dim > 0) stride += static_cast<size_t>(k.dim) * channelFormatSize(k.format);
            sOff[static_cast<size_t>(s)] = off;
            sStride[static_cast<size_t>(s)] = stride;
            off += count * stride;
            off = (off + 15) & ~static_cast<size_t>(15);
        }
    } catch (const FormatError& e) {
        return fail(e.what());
    }

    auto readChannel = [&](size_t idx, int wantDim, std::vector<float>& out) -> bool {
        if (idx >= ch.size() || ch[idx].dim == 0) return false;
        const Ch& k = ch[idx];
        size_t csz = channelFormatSize(k.format);
        size_t base = sOff[static_cast<size_t>(k.stream)] + static_cast<size_t>(k.offset);
        size_t stride = sStride[static_cast<size_t>(k.stream)];
        if (count && base + (count - 1) * stride + static_cast<size_t>(k.dim) * csz > data.size())
            throw FormatError("vertex data shorter than its channels imply");
        out.assign(count * static_cast<size_t>(wantDim), 0.0f);
        for (size_t i = 0; i < count; ++i)
            for (int d = 0; d < std::min(k.dim, wantDim); ++d) {
                const uint8_t* src = data.data() + base + i * stride + static_cast<size_t>(d) * csz;
                float f = 0;
                switch (k.format) {
                case 0: std::memcpy(&f, src, 4); break;
                case 1: { uint16_t h; std::memcpy(&h, src, 2); f = halfToFloat(h); break; }
                case 2: case 3: f = *src / 255.0f; break;
                default: { uint32_t u; std::memcpy(&u, src, 4); f = static_cast<float>(u); }
                }
                out[i * static_cast<size_t>(wantDim) + static_cast<size_t>(d)] = f;
            }
        return true;
    };
    try {
        std::vector<float> tmp;
        if (readChannel(0, 3, tmp)) {
            m.positions.resize(count);
            for (size_t i = 0; i < count; ++i) m.positions[i] = {tmp[i * 3], tmp[i * 3 + 1], tmp[i * 3 + 2]};
        }
        if (readChannel(1, 3, tmp)) {
            m.normals.resize(count);
            for (size_t i = 0; i < count; ++i) m.normals[i] = {tmp[i * 3], tmp[i * 3 + 1], tmp[i * 3 + 2]};
        }
        if (readChannel(3, 2, tmp)) {
            m.uv0.resize(count);
            for (size_t i = 0; i < count; ++i) m.uv0[i] = {tmp[i * 2], tmp[i * 2 + 1]};
        }
    } catch (const FormatError& e) {
        return fail(e.what());
    }

    for (const auto& s : v.at("m_Skin").asArray()) {
        BoneInfluence b;
        for (int i = 0; i < 4; ++i) {
            b.idx[i] = static_cast<int>(indexed(s, "boneIndex", i).asInt());
            b.w[i] = static_cast<float>(indexed(s, "weight", i).asFloat());
        }
        m.skin.push_back(b);
    }
    if (!m.skin.empty() && m.skin.size() != count) m.skin.clear();

    for (const auto& bp : v.at("m_BindPose").asArray()) {
        Mat4 mt;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                mt.m[r * 4 + c] = static_cast<float>(bp.at("e" + std::to_string(r) + std::to_string(c)).asFloat());
        m.bindPoses.push_back(mt);
    }

    const Bytes& ib = v.at("m_IndexBuffer").asBin();
    bool use16 = !v.has("m_Use16BitIndices") || v.at("m_Use16BitIndices").asInt() != 0;
    size_t isz = use16 ? 2 : 4;
    auto indexAt = [&](size_t i) -> uint32_t {
        size_t o = i * isz;
        if (o + isz > ib.size()) throw FormatError("index buffer shorter than a submesh needs");
        return use16 ? static_cast<uint32_t>(ib[o] | (ib[o + 1] << 8))
                     : static_cast<uint32_t>(ib[o] | (ib[o + 1] << 8) | (ib[o + 2] << 16) | (static_cast<uint32_t>(ib[o + 3]) << 24));
    };
    try {
        for (const auto& sm : v.at("m_SubMeshes").asArray()) {
            size_t first = static_cast<size_t>(sm.at("firstByte").asInt()) / isz;
            size_t n = static_cast<size_t>(sm.at("indexCount").asInt());
            int topo = static_cast<int>(sm.at("topology").asInt());
            std::vector<uint32_t> tris;
            if (topo == 0) {
                for (size_t i = 0; i + 3 <= n; i += 3) {
                    tris.push_back(indexAt(first + i));
                    tris.push_back(indexAt(first + i + 1));
                    tris.push_back(indexAt(first + i + 2));
                }
            } else if (topo == 1) {
                for (size_t i = 0; i + 2 < n; ++i) {
                    uint32_t a = indexAt(first + i), b = indexAt(first + i + 1), c = indexAt(first + i + 2);
                    if (a == b || b == c || a == c) continue;
                    if (i & 1) std::swap(a, b);
                    tris.push_back(a); tris.push_back(b); tris.push_back(c);
                }
            } else {
                return fail("submesh topology " + std::to_string(topo) + " is not triangles");
            }
            for (uint32_t t : tris)
                if (t >= count) return fail("triangle index outside the vertex buffer");
            m.subMeshTriangles.push_back(std::move(tris));
        }
    } catch (const FormatError& e) {
        return fail(e.what());
    }
    return m;
}

std::optional<MaterialData> UnityFile::readMaterial(const PPtr& p, std::string* why) const {
    auto fail = [&](const std::string& m) -> std::optional<MaterialData> { if (why) *why = m; return std::nullopt; };
    if (p.isNull()) return fail("no material assigned");
    if (!p.isLocal()) return fail("material is in another bundle (" + externalName(p) + ")");
    const ObjectInfo* o = sf_->find(p.pathId);
    if (!o || o->classId != cls::Material) return fail("material object not found");
    Value v = sf_->read(*o);
    MaterialData m;
    m.name = v.at("m_Name").asStr();
    PPtr sp = toPPtr(v.at("m_Shader"));
    if (sp.isLocal() && !sp.isNull()) {
        if (const ObjectInfo* so = sf_->find(sp.pathId)) {
            Value sv = sf_->read(*so);
            m.shaderName = sv.at("m_ParsedForm").at("m_Name").asStr();
            if (m.shaderName.empty()) m.shaderName = sv.at("m_Name").asStr();
        }
    } else if (!sp.isNull()) {
        m.shaderName = "(external shader " + externalName(sp) + ")";
    }
    const Value& props = v.at("m_SavedProperties");
    for (const auto& e : props.at("m_TexEnvs").asArray())
        m.textures.emplace_back(e.at("first").asStr(), toPPtr(e.at("second").at("m_Texture")));
    for (const auto& e : props.at("m_Colors").asArray()) {
        const Value& c = e.at("second");
        m.colors.emplace_back(e.at("first").asStr(),
                              Vec4{static_cast<float>(c.at("r").asFloat()), static_cast<float>(c.at("g").asFloat()),
                                   static_cast<float>(c.at("b").asFloat()), static_cast<float>(c.at("a").asFloat())});
    }
    for (const auto& e : props.at("m_Floats").asArray())
        m.floats.emplace_back(e.at("first").asStr(), static_cast<float>(e.at("second").asFloat()));
    return m;
}

std::optional<TextureData> UnityFile::readTexture(const PPtr& p, std::string* why) const {
    auto fail = [&](const std::string& m) -> std::optional<TextureData> { if (why) *why = m; return std::nullopt; };
    if (p.isNull()) return fail("no texture assigned");
    if (!p.isLocal()) return fail("texture is in another bundle (" + externalName(p) + ")");
    const ObjectInfo* o = sf_->find(p.pathId);
    if (!o || o->classId != cls::Texture2D) return fail("texture object not found");
    Value v = sf_->read(*o);
    TextureData t;
    t.name = v.at("m_Name").asStr();
    t.width = static_cast<int>(v.at("m_Width").asInt());
    t.height = static_cast<int>(v.at("m_Height").asInt());
    t.unityFormat = static_cast<int>(v.at("m_TextureFormat").asInt());
    const uint8_t* src = nullptr;
    size_t size = 0;
    const Bytes& inl = v.at("image data").asBin();
    if (!inl.empty()) {
        src = inl.data();
        size = inl.size();
    } else {
        const Value& sd = v.at("m_StreamData");
        const auto* node = bundle_->findNode(sd.at("path").asStr());
        uint64_t off = static_cast<uint64_t>(sd.at("offset").asInt()), sz = static_cast<uint64_t>(sd.at("size").asInt());
        if (!node || off + sz > node->size) return fail("streamed texture data not found");
        src = bundle_->nodeData(*node) + off;
        size = static_cast<size_t>(sz);
    }
    try {
        std::vector<uint8_t> px = DecodeTexture(t.unityFormat, t.width, t.height, src, size);
        // Unity stores rows bottom-first; PNG and PMX viewers want top-first.
        size_t row = static_cast<size_t>(t.width) * 4;
        t.rgba.resize(px.size());
        for (int y = 0; y < t.height; ++y)
            std::memcpy(&t.rgba[static_cast<size_t>(y) * row], &px[static_cast<size_t>(t.height - 1 - y) * row], row);
    } catch (const FormatError& e) {
        return fail(e.what());
    }
    return t;
}

std::vector<std::pair<std::string, Bytes>> UnityFile::readTextAssets() const {
    std::vector<std::pair<std::string, Bytes>> out;
    for (const auto* o : sf_->ofClass(cls::TextAsset)) {
        Value v = sf_->read(*o);
        const std::string& s = v.at("m_Script").asStr();
        out.emplace_back(v.at("m_Name").asStr(), Bytes(s.begin(), s.end()));
    }
    return out;
}

} // namespace cardbridge::unity
