#include "export/MorphCatalog.h"

#include "export/MmdNames.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::exporter {

namespace {

// Panel by the names the game gives its face meshes and shapes (the same keywords AddExpressionMorphs uses): mayuge = brows,
// eye / namida = eyes, kuti / tooth / tang = mouth; teeth are a mouth thing, the nose line belongs to the eyes.
int panelFor(const std::string& full) {
    auto has = [&](const char* s) { return full.find(s) != std::string::npos; };
    return has("mayu") ? 1 : (has("eye") || has("namida") || has("gag")) ? 2 : (has("kuti") || has("tooth") || has("tang") || has("mouth")) ? 3 : 4;
}

// The white and the iris of an eye: the game builds an eye as two meshes, the white (cf_Ohitomi_L / _R) and the iris one (cf_Ohitomi_L02 / _R02).
// A mesh whose name carries the eye's own name followed by digits is the iris.
LiveMorphEyes findEye(const assemble::Scene& scene, const char* key) {
    LiveMorphEyes out;
    const size_t keyLen = std::char_traits<char>::length(key);
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const std::string& n = scene.meshes[mi].name;
        const size_t at = n.find(key);
        if (at == std::string::npos) continue;
        const size_t next = at + keyLen;
        const bool iris = next < n.size() && std::isdigit(static_cast<unsigned char>(n[next]));   // cf_Ohitomi_L02: the iris
        auto& mesh = iris ? out.irisMesh : out.whiteMesh;
        if (mesh >= 0) continue;   // first match wins
        mesh = static_cast<int>(mi);
        auto& verts = iris ? out.irisVerts : out.whiteVerts;
        for (const auto& sub : scene.meshes[mi].submeshes)
            verts.insert(verts.end(), sub.indices.begin(), sub.indices.end());
        std::sort(verts.begin(), verts.end());
        verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    }
    return out;
}

} // namespace

LiveMorphCatalog BuildLiveMorphs(const assemble::Scene& scene) {
    LiveMorphCatalog cat;
    if (scene.meshes.empty()) return cat;

    cat.eyes[0] = findEye(scene, "cf_Ohitomi_L");   // the left eye (+x in the as-built scene)
    cat.eyes[1] = findEye(scene, "cf_Ohitomi_R");

    auto add = [&](std::string name, std::string nameEn, int panel, std::vector<LiveMorph::Recipe> recipe) {
        if (recipe.empty()) return;
        LiveMorph lm;
        lm.name = std::move(name);
        lm.nameEn = std::move(nameEn);
        lm.panel = panel;
        lm.recipe = std::move(recipe);
        cat.morphs.push_back(std::move(lm));
    };
    std::map<std::string, std::string> expressionShapes;   // expression morph name -> the pattern's blend shape name ("a_l_op")
    // The game's FaceBlendShape patterns, in the order the export writes them: brow / eye / mouth, closed and open, relative to the resting
    // expression built into the vertices.
    for (const auto& ex : scene.expressions) {
        std::vector<LiveMorph::Recipe> recipe;
        for (const auto& t : ex.targets) {
            if (t.mesh < 0 || static_cast<size_t>(t.mesh) >= scene.meshes.size()) continue;
            const auto& shapes = scene.meshes[static_cast<size_t>(t.mesh)].shapes;
            if (t.shape < 0 || static_cast<size_t>(t.shape) >= shapes.size()) continue;
            if (shapes[static_cast<size_t>(t.shape)].offsets.empty()) continue;   // a pattern the head does not carry: nothing to move, like the PMX
            const float w = (t.weight - shapes[static_cast<size_t>(t.shape)].rest) * 0.01f;   // scene units
            if (std::fabs(w) > 1e-7f) recipe.push_back({t.mesh, t.shape, w});
        }
        if (recipe.empty()) continue;
        add(ex.name, "", ex.panel, std::move(recipe));
        if (!ex.shape.empty()) expressionShapes[ex.name] = ex.shape;
    }
    // One morph per raw blend shape, at full weight.
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const auto& mesh = scene.meshes[mi];
        for (size_t si = 0; si < mesh.shapes.size(); ++si) {
            const auto& sh = mesh.shapes[si];
            if (sh.offsets.empty()) continue;
            const float w = (100.0f - sh.rest) * 0.01f;   // the full shape, relative to the resting weight built into the vertices
            if (std::fabs(w) < 1e-7f) continue;
            const std::string full = mesh.name + "." + sh.name;
            add(full, "", panelFor(full), {{static_cast<int>(mi), static_cast<int>(si), w}});
        }
    }
    // The MMD standard ones: each a copy of the game morph of the same meaning, found by the pattern's own blend shape name, or (for a head
    // without the pattern) of the plain morph the fallback names, scaled.
    auto findMorph = [&](const std::string& name) {
        for (size_t i = 0; i < cat.morphs.size(); ++i) if (cat.morphs[i].name == name) return static_cast<int>(i);
        return -1;
    };
    auto byShape = [&](const char* keys, int panel) {
        std::string list = keys;
        for (size_t at = 0; at <= list.size();) {
            const size_t end = std::min(list.find(',', at), list.size());
            const std::string key = list.substr(at, end - at);
            at = end + 1;
            for (size_t i = 0; i < cat.morphs.size(); ++i) {
                auto it = expressionShapes.find(cat.morphs[i].name);
                if (it != expressionShapes.end() && it->second == key && cat.morphs[i].panel == panel && !cat.morphs[i].recipe.empty()) return static_cast<int>(i);
            }
        }
        return -1;
    };
    auto scaledCopy = [&](int src, float gain) {
        std::vector<LiveMorph::Recipe> recipe;
        if (src < 0) return recipe;
        for (const auto& e : cat.morphs[static_cast<size_t>(src)].recipe) recipe.push_back({e.mesh, e.shape, e.weight * gain});
        return recipe;
    };
    for (const auto& s : MmdStandardMorphs()) {
        if (findMorph(s.mmd) >= 0) continue;   // made already
        int src = byShape(s.keys, s.panel);
        float gain = 1.0f;
        if (src < 0 && s.fallback) { src = findMorph(s.fallback); gain = s.gain; }
        if (src < 0) continue;
        add(s.mmd, s.en, s.panel, scaledCopy(src, gain));
    }
    // The one-eyed versions: the closed-eye shapes cut at the face's middle. ウィンク closes the character's left eye: the side the rig's left
    // eye bone is on (the model is still as built here, so the sign of x alone decides).
    float leftSign = 1.0f;
    for (const auto& b : scene.bones)
        if (b.name == "cf_J_hitomi_tx_L" || b.name == "左目") { leftSign = b.pos.x < 0.0f ? -1.0f : 1.0f; break; }
    for (const auto& w : MmdWinkMorphs()) {
        const int src = findMorph(w.from);
        if (src < 0) continue;
        // The mean of the source morph's vertex positions is the face's middle: the cut goes there (the left eye first).
        double sum = 0;
        size_t n = 0;
        for (const auto& e : cat.morphs[static_cast<size_t>(src)].recipe) {
            const auto& mesh = scene.meshes[static_cast<size_t>(e.mesh)];
            const auto& shapes = mesh.shapes;
            if (e.shape < 0 || static_cast<size_t>(e.shape) >= shapes.size()) continue;
            for (const auto& [vi, d] : shapes[static_cast<size_t>(e.shape)].offsets) {
                if (vi >= mesh.vertices.size()) continue;
                sum += mesh.vertices[vi].pos.x;
                ++n;
            }
        }
        if (n == 0) continue;
        const float mid = static_cast<float>(sum / static_cast<double>(n));
        for (int side = 0; side < 2; ++side) {
            // side 0 = the left eye (ウィンク): keep the vertices on the rig's left side of the middle
            LiveMorph lm;
            lm.name = side == 0 ? w.left : w.right;
            lm.nameEn = side == 0 ? w.leftEn : w.rightEn;
            lm.panel = 2;
            for (const auto& e : cat.morphs[static_cast<size_t>(src)].recipe)
                lm.recipe.push_back({e.mesh, e.shape, e.weight});
            lm.winkSide = side;
            lm.winkMid = mid;
            lm.winkSign = leftSign;
            cat.morphs.push_back(std::move(lm));
        }
    }
    return cat;
}

void AccumulateLiveMorph(const assemble::Scene& scene, const LiveMorphCatalog& catalog, const LiveMorph& morph, float weight,
                         std::vector<std::vector<Vec3>>& dst) {
    if (weight == 0.0f) return;
    const size_t meshCount = scene.meshes.size();
    if (dst.size() < meshCount) dst.resize(meshCount);
    const auto meshVec = [&dst, &scene](int mi) -> std::vector<Vec3>& {
        std::vector<Vec3>& vec = dst[static_cast<size_t>(mi)];
        if (vec.empty()) vec.assign(scene.meshes[static_cast<size_t>(mi)].vertices.size(), Vec3{0, 0, 0});
        return vec;
    };
    for (const auto& e : morph.recipe) {
        if (e.mesh < 0 || static_cast<size_t>(e.mesh) >= meshCount) continue;
        const auto& shapes = scene.meshes[static_cast<size_t>(e.mesh)].shapes;
        if (e.shape < 0 || static_cast<size_t>(e.shape) >= shapes.size()) continue;
        const auto& offsets = shapes[static_cast<size_t>(e.shape)].offsets;
        if (offsets.empty()) continue;
        const float k = weight * e.weight;
        std::vector<Vec3>& vec = meshVec(e.mesh);
        for (const auto& [vi, d] : offsets) {
            if (vi >= vec.size()) continue;
            if (morph.winkSide >= 0) {   // the one-eyed versions: only the vertices on the rig's left / right of the middle
                const float side = (scene.meshes[static_cast<size_t>(e.mesh)].vertices[vi].pos.x - morph.winkMid) * morph.winkSign;
                if ((side > 0.0f) != (morph.winkSide == 0)) continue;
            }
            Vec3& a = vec[vi];
            a.x += d.x * k; a.y += d.y * k; a.z += d.z * k;
        }
    }
    // Iris follow: when the lid squeezes the white, the iris takes the mean offset of the white's (the PMX adds it to every morph).
    for (const auto& E : catalog.eyes) {
        if (E.whiteMesh < 0 || E.irisMesh < 0) continue;
        if (static_cast<size_t>(E.whiteMesh) >= dst.size() || static_cast<size_t>(E.irisMesh) >= dst.size()) continue;
        const std::vector<Vec3>& white = dst[static_cast<size_t>(E.whiteMesh)];
        if (white.empty()) continue;   // this morph did not move the white: the iris follow is the zero offset, nothing to do
        Vec3 sum{0, 0, 0};
        size_t n = 0;
        for (uint32_t v : E.whiteVerts) {
            if (v >= white.size()) continue;
            const Vec3& d = white[v];
            if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) continue;
            sum = {sum.x + d.x, sum.y + d.y, sum.z + d.z};
            ++n;
        }
        if (n == 0) continue;
        const Vec3 mean{sum.x / static_cast<float>(n), sum.y / static_cast<float>(n), sum.z / static_cast<float>(n)};
        std::vector<Vec3>& iris = meshVec(E.irisMesh);
        for (uint32_t v : E.irisVerts) {
            if (v >= iris.size()) continue;
            Vec3& a = iris[v];
            a.x += mean.x; a.y += mean.y; a.z += mean.z;
        }
    }
}

} // namespace cardbridge::exporter
