// Body suppression fallback: hide the body under opaque clothing when no body mask could be resolved for the outfit.
//
// Koikatsu hides the skin under clothing with a per-outfit mask in body UV space (red channel 255 = keep the skin).
// The game uses it unconditionally when the mask asset resolves; mods that ship no mask, ship broken ones, or point at non-texture
// assets leave the body fully visible under the clothing — the body bursts through the outfit (the extreme H 87/48/102 cards show this
// everywhere). The game has no automatic fallback in that case; we do, but only when the outfit genuinely declares no resolvable mask.
//
// This module implements that fallback. It's deliberately NOT a "distance-to-clothing" test: an exposed body region is
// within millimetres of opaque fabric (the dress slit is exactly that case), so proximity says nothing about coverage.
// The discriminator used here is occlusion: for each body vertex, cast a short ray along its outward normal; if an opaque
// garment triangle blocks it, the vertex is concealed by the clothing, and the triangle is suppressed. If the ray exits
// unobstructed the vertex is exposed and stays visible — the dress slit stays open, the neckline stays open, the hem stays open.
// This is the same silhouette logic the game's mask implements: a body triangle the clothing replaces from every direction
// leaves the part visible only where the clothing leaves an opening.
//
// Coverage decisions are per vertex; a body triangle is suppressed only when all three of its vertices are concealed
// (conservative: partial slivers are kept so no hidden skin disappears along garment edges unnecessarily).
//
// Exposed pieces stay on the body; suppressed triangles were moved to ".under.*" under pieces, kept for the nude morph.
// Translucent garments do not occlude (the skin is meant to show through them). Hidden (in- Preview) garments do occlude
// the same as the visible layer they replace. KokanHide is handled separately (a hard region hide, as the game's).
//
// The fallback is used only when the declared mask could not be resolved; a mask that resolves still drives the mask passes.
#pragma once

#include "assemble/Scene.h"
#include "assemble/MeshQueries.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::assemble {

struct BodySuppressionStats {
    size_t coverageTraits = 0;    // body triangles hidden by the occlusion fallback
    size_t coverageTotal = 0;     // body triangles examined
    size_t kokanTraits = 0;       // body triangles hidden by KokanHide
    size_t providers = 0;         // opaque garment triangles considered as occluders
    bool   fallbackUsed = false;  // true when any body triangle was suppressed by the fallback
};

// The cover (garment part ids) -> mesh -> material -> triangle indices of a set of concealed triangles.
struct UnderPieceSet {
    std::map<std::vector<std::string>, std::map<int, std::map<int, std::vector<uint32_t>>>> byCover;
    bool empty() const { return byCover.empty(); }
};

// True when every id in `cover` is a garment whose materials are all translucent (such a garment masks nothing) or the empty id.
bool AllCoverTranslucent(const std::map<std::string, bool>& allTranslucent, const std::vector<std::string>& cover);

std::map<std::string, bool> TranslucencyByPart(const Scene& scene);

// Moves triangles onto under pieces (kept for the nude morph).
void EmitUnderPieces(Scene& scene, const std::string& what, size_t pi, const UnderPieceSet& pieces, const std::string& tagOverride = "");

// Collects the opaque triangles of every worn Outfit/Accessory part (hidden ones included — a hidden garment covers the same as the visible
// layer it replaces). Each triangle is tagged with its owner's part id for under-piece tagging. Translucent materials don't occlude.
// With `excludeCutout`, a material whose faces sample a discard texel is not a provider either — a perforated shell
// (lace with real holes) does not conceal what is behind it. The default is off (historical assemble-time behaviour).
struct CoverageProvider {
    Tri tri;             // world-space triangle of the opaque garment
    std::string owner;   // part id
};
std::vector<CoverageProvider> GatherOpaqueProviders(const Scene& scene, bool excludeCutout = false);

// Classifies every body vertex: true when its outward-normal ray hits an opaque garment triangle within `maxDist` (default 0.4 m).
// `coverage` is sized like the body mesh's vertex count; use with `meshIndices` of the body part(s).
// `excludeOwner` (when non-null) names a part id whose providers never block — a shell never covers itself.
std::vector<char> ClassifyBodyOcclusion(const Scene& scene, const std::vector<int>& meshIndices, const std::vector<CoverageProvider>& providers, float maxDist = 0.4f, const std::string* excludeOwner = nullptr);

// Fallback pass: suppresses body triangles whose vertices are all concealed by opaque garments (ray test), and hides crotch triangles
// when a bottom with KokanHide=1 is worn. Body-only; the bra uses the mask passes. Call after the mask passes.
// `applyCoverage` gates the ray-based suppression layer alone (KokanHide still applies either way); keep it false when a body mask resolved.
BodySuppressionStats SuppressBodyUnderGarments(Scene& scene, const std::vector<std::string>& kokanHiders, bool applyCoverage = true);

} // namespace cardbridge::assemble
