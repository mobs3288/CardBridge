#include "export/MorphCatalog.h"

#include "export/MmdNames.h"
#include "export/PmxPhysics.h"
#include "pmx/PmxWriter.h"

#include <algorithm>
#include <map>
#include <vector>

namespace cardbridge::exporter {

namespace {

// The scratch model exists to reuse the export's own morph builders verbatim (AddExpressionMorphs, AddMmdMorphs and
// the eye pass), so the catalog cannot drift from what the file carries. Only positions, indices, material names and
// bone names are filled - the winks split by vertex x, the eye pass fits the white and rebinds it, the morph compensation
// adds the white's mean offset to the iris - none of which reads normals, uvs or weights.
struct MeshBase {
    std::vector<int> base;   // PMX vertex index of each scene mesh's first vertex (-1 never happens: the scratch exports everything)
    uint32_t total = 0;
};

MeshBase meshBaseOf(const assemble::Scene& scene) {
    MeshBase mb;
    mb.base.resize(scene.meshes.size(), -1);
    uint32_t base = 0;
    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        mb.base[mi] = static_cast<int>(base);
        base += static_cast<uint32_t>(scene.meshes[mi].vertices.size());
    }
    mb.total = base;
    return mb;
}

} // namespace

std::vector<LiveMorph> BuildLiveMorphs(const assemble::Scene& scene) {
    std::vector<LiveMorph> out;
    if (scene.meshes.empty()) return out;

    const MeshBase mb = meshBaseOf(scene);
    pmx::Model model;

    // positions
    model.vertices.resize(mb.total);
    {
        uint32_t base = 0;
        for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
            const auto& m = scene.meshes[mi];
            for (size_t v = 0; v < m.vertices.size(); ++v) {
                pmx::Vertex& pv = model.vertices[base + v];
                pv.position = m.vertices[v].pos;   // scene units, copied at rest (the PMX scales, the viewer does not)
                pv.bone[0] = 0;
            }
            base += static_cast<uint32_t>(m.vertices.size());
        }
    }
    // indices and one material per submesh, with the names the exporter writes (part/mesh#sub): the eye pass looks for
    // the mesh's own name inside them to tell the white ("cf_Ohitomi_L") from the iris ("cf_Ohitomi_L02"). The scene's submesh
    // indices are mesh-local; the PMX (and this scratch model) is one vertex array, so they are offset by the mesh's base.
    {
        for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
            const auto& m = scene.meshes[mi];
            const uint32_t base = static_cast<uint32_t>(mb.base[mi]);
            for (size_t si = 0; si < m.submeshes.size(); ++si) {
                const auto& sm = m.submeshes[si];
                pmx::Material pm;
                if (sm.material >= 0 && static_cast<size_t>(sm.material) < scene.materials.size())
                    // The scene material's own name (the shader property material, e.g. "cf_Ohitomi_L02"): the substring the
                    // eye pass searches for. Falling back to the submesh ordinal keeps a missing material index harmless.
                    pm.name = scene.materials[static_cast<size_t>(sm.material)].name;
                if (pm.name.empty()) pm.name = "part/mesh#" + std::to_string(si);
                pm.indexCount = static_cast<uint32_t>(sm.indices.size());
                for (uint32_t idx : sm.indices) model.indices.push_back(base + idx);
                model.materials.push_back(std::move(pm));
            }
        }
    }
    // bones, parallel to scene.bones: rig names (ApplyMmdBoneNames renames in place, as it does for the PMX) and the
    // rig's rest positions (the winks split the eye's vertices on the left eye's side by x).
    model.bones.resize(scene.bones.size());
    for (size_t i = 0; i < scene.bones.size(); ++i) {
        pmx::Bone& pb = model.bones[i];
        pb.name = scene.bones[i].name;
        pb.position = scene.bones[i].pos;
        pb.parent = scene.bones[i].parent;
        pb.flags = 0x0006;   // helper; the scratch model is discarded, never written
    }

    // The morphs, exactly in the export's order: the expressions (and, with them, one morph per raw blend shape),
    // then the MMD standard ones (copies of the game's pattern with the standard name), then the bone-name pass,
    // which is also what compensates the lid morphs so the iris slides with the white (the game moves the white and
    // lids through the face blend shapes; the iris would stay in front of the lid without it).
    std::map<std::string, std::string> expressionShapes;   // expression morph name -> the pattern's blend shape name ("a_l_op")
    AddExpressionMorphs(scene, model, mb.base, 1.0f, &expressionShapes);
    AddMmdMorphs(model, expressionShapes);
    ApplyMmdBoneNames(scene, model, 1.0f);

    // global vertex indices -> per-mesh lists
    out.reserve(model.morphs.size());
    for (const auto& m : model.morphs) {
        if (m.vertices.empty()) continue;   // the outfit and nude toggles are material morphs; group morphs have no vertex work
        LiveMorph lm;
        lm.name = m.name;
        lm.nameEn = m.nameEn;
        lm.panel = m.panel >= 1 && m.panel <= 4 ? m.panel : 4;
        lm.byMesh.resize(scene.meshes.size());
        for (const auto& [vi, d] : m.vertices) {
            if (vi < 0 || static_cast<size_t>(vi) >= mb.total) continue;
            // meshBase is cumulative and sorted; duplicates (an empty mesh) resolve to the mesh that owns the vertex.
            const auto it = std::upper_bound(mb.base.begin(), mb.base.end(), vi);
            const size_t mi = it == mb.base.begin() ? 0 : static_cast<size_t>(it - mb.base.begin() - 1);
            lm.byMesh[mi].emplace_back(static_cast<uint32_t>(vi - mb.base[mi]), d);
        }
        out.push_back(std::move(lm));
    }
    return out;
}

} // namespace cardbridge::exporter
