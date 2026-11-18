#include "assemble/CustomHairDiag.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace cardbridge::assemble {

namespace {
std::string fmt(const char* format, double value) {
    char buffer[128];
    std::snprintf(buffer, sizeof buffer, format, value);
    return buffer;
}
}

bool IsHeadAttachNode(const std::string& node) {
    if (node.empty()) return false;
    // The head sockets the game uses for head-attached items: a_n_head, a_n_headside, a_n_head_L/R, a_n_hair*, a_n_hair_pin.
    if (node.rfind("a_n_head", 0) == 0) return true;
    if (node.rfind("a_n_hair", 0) == 0) return true;
    return false;
}

CustomRigVerdict AnalyzeImportedRig(const ImportedRigReport& r) {
    CustomRigVerdict v;
    // A custom skeleton: the item carries bones the shared rig does not know. That is the reliable,
    // name-independent signature of an imported item (a game's own rig, whatever it calls its bones).
    // Scale alone does not count, because the game bakes scale into some vanilla prefabs on purpose.
    // A head-attached skinned custom skeleton IS hair-like, whether or not it carries DynamicBone chains:
    // that distinction (chains or no chains) only changes the physics report, not what the item is.
    v.importedRig = r.customBones > 0 && r.skinnedMeshes > 0;
    v.hairLike = v.importedRig && (IsHeadAttachNode(r.attachNode) || r.group == "Hair");
    if (!v.importedRig) return v;   // an ordinary item: nothing to say
    const bool scaledRoot = std::fabs(1.0f - r.rootScale) > 0.01f;

    // What is this item.
    if (v.hairLike) {
        if (r.chains == 0) v.headline = "imported custom-skeleton hair: skinned to " + std::to_string(r.customBones) + " of its own bones, no DynamicBone chains (rigid as authored)";
        else v.headline = "imported custom-skeleton hair: skinned to " + std::to_string(r.customBones) + " of its own bones, " + std::to_string(r.chains) + " DynamicBone chain(s)";
    } else v.headline = "imported custom-skeleton item: skinned to " + std::to_string(r.customBones) + " of its own bones";

    // Where it hangs from.
    if (r.group == "Hair")
        v.infos.push_back("hung on the head's hair parent, as the game parents its hair items");
    else if (IsHeadAttachNode(r.attachNode))
        v.infos.push_back("attached to '" + r.attachNode + "' (its list row's attach socket)");

    // The import scale: kept exactly as authored. PMX has no bone scale, so the vertex positions are baked
    // with it; nothing is rescaled "to fit", because rescaling would be destroying the asset's own design.
    if (scaledRoot) v.infos.push_back("prefab root carries " + fmt("%.4f", r.rootScale) +
                                      " scale (kept as authored; vertices are baked with it, bones are positions only)");
    if (r.minScale < 1e-6f)
        v.warnings.push_back("a transform in the import has a near-zero scale; the game would render it degenerate too");

    // Bind pose verification: the inverse bind matrices must be consistent with the hierarchy they came from.
    if (r.bindScaleErr > 0.25f)
        v.warnings.push_back("bind pose scale does not match the imported hierarchy (off by " + fmt("%.0f", r.bindScaleErr * 100.0) +
                              "%): the mesh is baked with its bind pose as-is; expect limbs/strands to render wrong; the asset needs re-exporting");
    else if (r.bindScaleErr > 0.05f)
        v.infos.push_back("bind pose scale is close to, but not exactly the hierarchy's (off by " + fmt("%.1f", r.bindScaleErr * 100.0) + "%); kept as authored");

    // How far the strands are, and what the physics says about them. The converter reports, it does not change.
    if (r.strandDrop > 0.25f) {
        if (v.hairLike)
            v.infos.push_back("the strands reach " + fmt("%.0f", r.strandDrop * 100.0) +
                              " cm below the attachment point; that is this asset's design, not a conversion artefact; geometry is never shortened");
        else
            v.infos.push_back("this item's own geometry reaches " + fmt("%.0f", r.strandDrop * 100.0) +
                              " cm below the attachment point; kept exactly as authored");
    }
    if (v.hairLike) {
        if (r.chains == 0) v.infos.push_back("no DynamicBone chains; the strands are rigid as authored");
        else if (r.droppedChains == 0) v.infos.push_back(std::to_string(r.chains) + " DynamicBone chain(s) converted to PMX rigid bodies and joints with the component's damping, elasticity and stiffness");
    }

    // Chains that could not be resolved: the converter drops them silently otherwise, which is a real loss.
    if (r.droppedChains > 0)
        v.warnings.push_back(std::to_string(r.droppedChains) +
                             " DynamicBone chain(s) cover fewer than two bones that resolve to the scene; the chain(s) were skipped and nothing else changed");
    // The rest pose: the authored pose is kept. The maker may reposition/recolour/re-orient the item at will.
    return v;
}

} // namespace cardbridge::assemble
