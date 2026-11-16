// Body-geometry suppression: reproduce Koikatsu's clothing-mask behaviour when the mask data is present, and
// suppress body triangles spatially when it is not.
//
// The game hides the skin under opaque clothing with an alpha mask in the covered mesh's UV space
// (the outfit list's OverBodyMask/OverBraMask). A garment whose mask is missing or unresolved hides nothing in the
// game either — the body shows through. CardBridge therefore has two layers, sharing one mechanism:
//
//   1. Masks (assemble time, uv-based) — handled in Assembler.cpp's mask pass, using the mask texture directly.
//      The triangles it takes, and the ones the coverage pass below takes, land on "body.under.*" under-pieces:
//      kept so the nude switch (and the PMX 'Nude body' morph) can show the skin again.
//   2. Spatial coverage (assemble time, geometry-based) — the covered triangles are those within `eps` of an
//      OPAQUE triangle of any worn Outfit/Accessory part. This catches exactly the case the mask cannot cover:
//      missing, mis-authored or undeclared coverage data.
//   3. KokanHide — the bottoms' list row key KokanHide=1 hides the crotch skin (the game hides the pubic region
//      the same way). Done geometrically around the rig's cf_d_kokan / cf_j_kokan bone (no mask texture exists for this
//      flag in the source assets).
//
// Preservation rules, applied everywhere:
//   - translucent garments suppress nothing (the skin shows through them on purpose), matching the mask path;
//   - triangles only move to under pieces — no body material is deleted, no mesh is removed;
//   - layered clothing: a triangle covered by several garments is tagged with every coverer (the same rule as masks);
//   - the exposed skin is untouched: neck, arms, legs outside clothing, faces, hands, feet outside the shoes;
//   - incomplete input never fails the export: a missing kokan bone warns and hides nothing.
#pragma once

#include "assemble/Scene.h"
#include "assemble/MeshQueries.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::assemble {

struct BodySuppressionStats {
    size_t coverageTraits = 0;    // body triangles hidden by the spatial coverage pass (kept as under pieces)
    size_t coverageTotal = 0;     // body triangles examined by that pass
    size_t kokanTraits = 0;       // body triangles hidden by the KokanHide pass
    size_t providers = 0;         // opaque garment triangles that were tested as coverage providers
};

// The cover (garment part ids) -> mesh -> material -> triangle indices of a set of covered triangles.
struct UnderPieceSet {
    std::map<std::vector<std::string>, std::map<int, std::map<int, std::vector<uint32_t>>>> byCover;
    bool empty() const { return byCover.empty(); }
};

// True when every id in `cover` is a garment whose materials are all translucent (such a garment masks nothing: the skin is meant to
// show through it). An empty id (a garment the game itself hides) always counts as a cover — the game hides the skin the same way.
// `allTranslucent` maps part id -> true when every material on it blends.
bool AllCoverTranslucent(const std::map<std::string, bool>& allTranslucent, const std::vector<std::string>& cover);

// Maps part id -> true when every material on it is authored translucent (blends). Under pieces, hidden parts are skipped.
std::map<std::string, bool> TranslucencyByPart(const Scene& scene);

// Moves the triangles gathered in `pieces` (built from the meshes of part `pi`) onto new "<part id>.under.*" parts — the skin kept so
// the nude body can be shown, in the viewer and through PMX morphs. `what` names the mechanism in the part's detail string.
// `tagOverride`, when non-empty, is used as the piece tag instead of the derived coverer list (the coverer list still lands in
// coveredBy, so per-garment skin morphs group correctly). If the derived piece id already exists in the scene, a counter is
// appended rather than producing two parts with the same id.
void EmitUnderPieces(Scene& scene, const std::string& what, size_t pi, const UnderPieceSet& pieces, const std::string& tagOverride = "");

// Layer 2 (coverage): hides the body triangles that opaque clothing covers — within `eps` (scene units; default 4 mm) of an opaque
// triangle of any worn Outfit/Accessory part (hidden ones count too). Layer 3 (KokanHide): hides the crotch triangles when a bottom
// item with KokanHide=1 is worn (`kokanHiders` names its part ids; empty = the pass does nothing). Both keep the hidden triangles on
// under pieces tagged with every coverer, so per-garment "show the skin under X" morphs and the all-clothes-off morph work.
// Call after the mask passes (the triangles the masks already removed are not seen here).
BodySuppressionStats SuppressBodyUnderGarments(Scene& scene, const std::vector<std::string>& kokanHiders, float eps = 0.004f);

} // namespace cardbridge::assemble
