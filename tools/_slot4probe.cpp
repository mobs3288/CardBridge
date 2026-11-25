// Scratch diagnostic: trace clothing skinning / bone mappings and pose response.
//   usage: slot4probe <card.png> <library> [mods] [outfit]
#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
std::string Slashes(const char* s) { std::string o = s; for (auto& c : o) if (c == '\\') c = '/'; return o; }
float dist(cardbridge::Vec3 a, cardbridge::Vec3 b) {
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return std::sqrt(x * x + y * y + z * z);
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: slot4probe <card.png> <library> [mods] [outfit]\n"); return 1; }
    const std::string cardPath = Slashes(argv[1]), library = Slashes(argv[2]);
    const std::string mods = argc >= 4 ? Slashes(argv[3]) : std::string();
    int outfitOverride = argc >= 5 ? std::atoi(argv[4]) : -1;
    using namespace cardbridge;

    card::Card c;
    try { c = card::LoadCard(cardPath); }
    catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::printf("CARD family=%s supported=%d outfits=%zu\n", card::FamilyName(c.family), c.supported ? 1 : 0, c.outfits.size());
    for (const auto& b : c.blocks) std::printf("  block %-14s v%s\n", b.name.c_str(), b.version.c_str());

    resolve::ModIndex mindex;
    if (!mods.empty()) {
        const std::string cache = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "/slot4probe_modcache.json";
        std::vector<std::string> iss;
        mindex = resolve::ModIndex::Build(mods, cache, &iss, nullptr, [](size_t, size_t) {});
    }
    const auto chk = library::CheckLibrary(library, false);
    if (!chk.ok) { std::fprintf(stderr, "library problems\n"); return 3; }
    resolve::ListTable lists;
    std::vector<std::string> iss;
    lists.loadBaseGame(library::AbdataRoot(library), &iss);
    auto resolver = std::make_unique<resolve::Resolver>(mindex, lists, library::AbdataRoot(library));

    assemble::AssembleOptions ao;
    ao.outfit = outfitOverride;
    // Resolve the upper eyelash row the same way faceTextureFor does.
    {
        int64_t up = c.face.has("eyelineUpId") ? c.face.at("eyelineUpId").asInt() : -1;
        std::printf("card face.eyelineUpId=%lld\n", (long long)up);
        resolve::ItemRef fr; fr.partId = "head.EyelineUpTex"; fr.group = "Face";
        fr.property = "ChaFileFace.eyelineUpId"; fr.category = 412; fr.id = up;
        auto fi = resolver->resolve(c, fr);
        std::printf("  resolve status=%s name='%s' EyelineUpTex='%s' MainAB='%s' detail='%s'\n",
                    resolve::StatusName(fi.status), fi.listItem.get("Name").c_str(), fi.listItem.get("EyelineUpTex").c_str(),
                    fi.listItem.get("MainAB").c_str(), fi.detail.c_str()); std::printf("  modGuid=%s\\n", fi.modGuid.c_str());
        auto owners = mindex.findByListId(412, up);
        std::printf("  mods claiming cat412 id %lld: %zu\n", (long long)up, owners.size());
        for (const auto* o : owners) std::printf("    owner guid=%s name=%s path=%s\n", o->guid.c_str(), o->name.c_str(), o->path.c_str());
        for (const auto& rr : c.rows)
            if (rr.property == fr.property || (rr.category == 412 && rr.slot == up))
                std::printf("  card row prop='%s' cat=%d slot=%lld modId=%s\n", rr.property.c_str(), rr.category, (long long)rr.slot, rr.modId.c_str());
    }
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);
    std::printf("SCENE bones=%zu parts=%zu meshes=%zu\n\n", scene.bones.size(), scene.parts.size(), scene.meshes.size());

    // ---- all items in this outfit -------------------------------------------------
    int outfit = outfitOverride;
    if (outfit < 0) outfit = static_cast<int>(c.status.at("coordinateType").asInt());
    if (outfit < 0 || (size_t)outfit >= c.outfits.size()) outfit = 0;
    std::printf("=== ITEMS (outfit %d) ===\n", outfit);
    for (const auto& it : resolve::ItemsFromCard(c, outfit))
        std::printf("  %-26s group=%-12s cat=%5d id=%lld slot=%d acc=%d\n", it.partId.c_str(), it.group.c_str(), it.category,
                    (long long)it.id, it.slot, it.accIndex);
    std::printf("\n");

    // ---- pose response: A-pose with the arms down, using the same skin path as the viewer ----
    assemble::Pose pose;
    bool ap = assemble::MakeAPose(scene, 40.0f, pose.base);
    std::printf("A-pose available=%d\n", ap ? 1 : 0);
    std::vector<cardbridge::Mat4> skin = assemble::SkinMatrices(scene, pose);
    std::vector<cardbridge::Mat4> rest = assemble::SkinMatrices(scene, assemble::Pose{});

    std::printf("\n=== PARTS / MESHES ===\n");
    for (const auto& part : scene.parts) {
        std::printf("PART %-26s group=%-12s cat=%5d meshes=%zu status=%s\n", part.id.c_str(), part.group.c_str(), part.category,
                    part.meshes.size(), part.status.c_str());
        for (int mi : part.meshes) {
            if (mi < 0 || (size_t)mi >= scene.meshes.size()) continue;
            const auto& m = scene.meshes[(size_t)mi];
            std::map<std::string, float> boneW;
            std::map<std::string, int> boneN;
            size_t rootVerts = 0, zeroW = 0;
            float maxDisp = 0;
            for (const auto& v : m.vertices) {
                float wsum = 0;
                bool rootStrong = false;
                for (int k = 0; k < 4; ++k) {
                    if (!(v.weight[k] > 0)) continue;
                    int bi = v.bone[k];
                    const std::string nm = (bi >= 0 && (size_t)bi < scene.bones.size()) ? scene.bones[(size_t)bi].name : "<?>";
                    boneW[nm] += v.weight[k];
                    ++boneN[nm];
                    wsum += v.weight[k];
                    if (bi == 0) rootStrong = true;
                }
                if (wsum <= 1e-6f) ++zeroW;
                if (rootStrong) ++rootVerts;
                cardbridge::Vec3 a = assemble::SkinPoint(v, rest), b = assemble::SkinPoint(v, skin);
                maxDisp = std::max(maxDisp, dist(a, b));
            }
            std::printf("  MESH %d '%-30s' verts=%6zu poseMaxDisp=%.4f rootVerts=%zu zeroW=%zu\n", mi, m.name.c_str(),
                        m.vertices.size(), maxDisp, rootVerts, zeroW);
            std::vector<std::pair<std::string, float>> top(boneW.begin(), boneW.end());
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
            size_t shown = 0;
            for (const auto& [nm, w] : top) {
                if (shown++ >= 8) break;
                std::printf("      %-28s wsum=%9.1f verts=%6d\n", nm.c_str(), w, boneN[nm]);
            }
        }
    }

    // ---- eyebrow / face trace -----------------------------------------------------
    // ---- unskinned garment: does its geometry coincide with the body mesh? --------
    std::printf("\n=== UNSKINNED GARMENT vs BODY GEOMETRY ===\n");
    const assemble::SceneMesh* bodyMesh = nullptr;
    for (const auto& p : scene.parts) if (p.id == "body") for (int mi : p.meshes)
        if (mi >= 0 && (size_t)mi < scene.meshes.size() && scene.meshes[(size_t)mi].name.find("o_body_a") != std::string::npos)
            bodyMesh = &scene.meshes[(size_t)mi];
    if (bodyMesh) {
        for (const auto& part : scene.parts)
            for (int mi : part.meshes) {
                if (mi < 0 || (size_t)mi >= scene.meshes.size()) continue;
                const auto& m = scene.meshes[(size_t)mi];
                bool allRoot = !m.vertices.empty();
                for (const auto& v : m.vertices) for (int k = 0; k < 4; ++k)
                    if (v.weight[k] > 0 && v.bone[k] != 0) allRoot = false;
                if (!allRoot) continue;
                std::printf("  UNWEIGHTED %-24s mesh %d '%s' verts=%zu\n", part.id.c_str(), mi, m.name.c_str(),
                            m.vertices.size());
                size_t close[4] = {};
                double sum = 0; float worst = 0;
                for (const auto& v : m.vertices) {
                    float best = 1e9f;
                    for (const auto& bv : bodyMesh->vertices) best = std::min(best, dist(v.pos, bv.pos));
                    worst = std::max(worst, best); sum += best;
                    if (best < 0.001f) ++close[0];
                    if (best < 0.01f) ++close[1];
                    if (best < 0.05f) ++close[2];
                    if (best < 0.10f) ++close[3];
                }
                std::printf("      nearest body vertex: <1mm=%zu <1cm=%zu <5cm=%zu <10cm=%zu mean=%.4f worst=%.4f\n",
                            close[0], close[1], close[2], close[3], sum / m.vertices.size(), worst);
            }
    }

    std::printf("\n=== EYEBROW / FACE TRACE ===\n");
    if (c.face.has("eyebrowId")) std::printf("  card face.eyebrowId=%lld\n", (long long)c.face.at("eyebrowId").asInt());
    if (c.face.has("eyebrowColor")) {
        const auto& ec = c.face.at("eyebrowColor");
        std::printf("  card face.eyebrowColor=[");
        for (size_t i = 0; i < ec.size(); ++i) std::printf("%s%.3f", i ? " " : "", ec.at(i).asFloat());
        std::printf("]\n");
    }
    if (c.status.has("eyebrowPtn")) std::printf("  card status.eyebrowPtn=%lld\n", (long long)c.status.at("eyebrowPtn").asInt());
    if (c.status.has("eyebrowOpenMax")) std::printf("  card status.eyebrowOpenMax=%.3f\n", c.status.at("eyebrowOpenMax").asFloat());
    for (const auto& p : scene.parts)
        if (has(p.id, "brow") || has(p.id, "mayu") || has(p.id, "face") || has(p.id, "head") || has(p.id, "eyeline")) {
            std::printf("  PART %-24s meshes=%zu\n", p.id.c_str(), p.meshes.size());
            for (int mi : p.meshes)
                if (mi >= 0 && (size_t)mi < scene.meshes.size()) {
                    const auto& m = scene.meshes[(size_t)mi];
                    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                    for (const auto& v : m.vertices) {
                        lo[0] = std::min(lo[0], v.pos.x); hi[0] = std::max(hi[0], v.pos.x);
                        lo[1] = std::min(lo[1], v.pos.y); hi[1] = std::max(hi[1], v.pos.y);
                        lo[2] = std::min(lo[2], v.pos.z); hi[2] = std::max(hi[2], v.pos.z);
                    }
                    std::printf("      mesh %d '%s' shapes=%zu bounds x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]\n",
                                mi, m.name.c_str(), m.shapes.size(), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
                    if (has(m.name, "mayuge")) {
                        double nz = 0; size_t nn = 0;
                        for (const auto& v : m.vertices) { nz += v.normal.z; ++nn; }
                        std::printf("        avg normal.z=%.3f (%zu verts)\n", nn ? nz / nn : 0, nn);
                        double facez = 0; size_t nf = 0;
                        for (const auto& sub : m.submeshes)
                            for (size_t i = 0; i + 2 < sub.indices.size(); i += 3) {
                                const auto& a = m.vertices[sub.indices[i]].pos;
                                const auto& b = m.vertices[sub.indices[i+1]].pos;
                                const auto& c = m.vertices[sub.indices[i+2]].pos;
                                facez += (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x); ++nf;
                            }
                        std::printf("        avg triangle winding z=%.5f (%zu faces) -> %s\n", nf ? facez/nf : 0, nf, (nf && facez > 0) ? "CCW (front +Z)" : "CW (front -Z)");
                    }
                    for (size_t s = 0; s < m.shapes.size(); ++s)
                        if (m.shapes[s].rest > 0 || (has(m.name, "mayuge") && s < 6))
                            std::printf("        shape %zu '%s' rest=%.2f offsets=%zu\n", s, m.shapes[s].name.c_str(), m.shapes[s].rest, m.shapes[s].offsets.size());
                }
        }
    for (size_t i = 0; i < scene.textures.size(); ++i)
        if (i >= 30 && i <= 40) {
            const auto& t = scene.textures[i];
            size_t op = 0, pt = 0, cl = 0;
            for (size_t p = 3; p < t.rgba.size(); p += 4) { if (t.rgba[p] > 250) ++op; else if (t.rgba[p] < 5) ++cl; else ++pt; }
            std::printf("  TEXALL %zu '%s' %dx%d opaque=%zu partial=%zu clear=%zu\n", i, t.name.c_str(), t.width, t.height, op, pt, cl);
        }
    for (size_t i = 0; i < scene.textures.size(); ++i)
        if (has(scene.textures[i].name, "mayu") || has(scene.textures[i].name, "brow") || has(scene.textures[i].name, "eyeline")) {
            const auto& t = scene.textures[i];
            size_t opaque = 0, partial = 0, clear = 0;
            for (size_t p = 3; p < t.rgba.size(); p += 4) {
                if (t.rgba[p] > 250) ++opaque; else if (t.rgba[p] < 5) ++clear; else ++partial;
            }
            float ulo = 9, uhi = -9, vlo = 9, vhi = -9;
            for (int y = 0; y < t.height; ++y) for (int x = 0; x < t.width; ++x) {
                if (t.rgba[((size_t)y * t.width + x) * 4 + 3] < 250) continue;
                ulo = std::min(ulo, (float)x / t.width); uhi = std::max(uhi, (float)x / t.width);
                vlo = std::min(vlo, (float)y / t.height); vhi = std::max(vhi, (float)y / t.height);
            }
            std::printf("  TEX %zu '%s' %dx%d clamp=%d srgb=%d alpha opaque=%zu partial=%zu clear=%zu opaqueUV u[%.3f %.3f] v[%.3f %.3f]\n", i,
                        t.name.c_str(), t.width, t.height, t.clamp ? 1 : 0, t.srgb ? 1 : 0, opaque, partial, clear, ulo, uhi, vlo, vhi);
        }
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& mat = scene.materials[i];
        if (has(mat.name, "mayu") || has(mat.name, "brow") || has(mat.name, "eyeline")) {
            std::printf("  MAT %zu '%s' shader=%s shaderIndex=%d (%s) texture=%d hidden=%d alpha=%d doubleSided=%d queue=%d bakedLit=%d toon=%zu\n",
                        i, mat.name.c_str(), mat.shader.c_str(), mat.shaderIndex,
                        mat.shaderIndex >= 0 && (size_t)mat.shaderIndex < scene.shaders.size() ? scene.shaders[(size_t)mat.shaderIndex].name.c_str() : "<none>",
                        mat.texture, mat.hidden ? 1 : 0, mat.alpha ? 1 : 0, mat.doubleSided ? 1 : 0, mat.renderQueue, mat.bakedLit ? 1 : 0, mat.toon.size());
            for (const auto& tb : mat.texProps)
                std::printf("      texprop %-20s -> %d st=(%.3f %.3f %.3f %.3f)\n", tb.name.c_str(), tb.texture, tb.st.x, tb.st.y, tb.st.z, tb.st.w);
            for (const auto& pr : mat.props)
                std::printf("      prop %-20s = %.3f %.3f %.3f %.3f\n", pr.first.c_str(), pr.second.x, pr.second.y, pr.second.z, pr.second.w);
        }
    }
    std::printf("\n=== SCENE ISSUES (%zu) ===\n", scene.issues.size());
    for (const auto& i : scene.issues)
        std::printf("  [%s] %s: %s\n", i.severity.c_str(), i.part.c_str(), i.message.c_str());

    std::printf("\nDONE\n");
    return 0;
}






