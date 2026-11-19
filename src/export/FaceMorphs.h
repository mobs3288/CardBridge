#pragma once

// The extended facial morph system: semantic names, categories, the facial effects (Eye Roll, Heart Eyes, Sweat, Sweat Heavy)
// and the MMD display frames. Everything here is data-driven from a central registry (the FaceStemLabels table in the .cpp),
// so a new morph is registered, not threaded through the exporter.
//
// What the exporter produces, before this module runs (see PmxPhysics.cpp / Exporter.cpp):
//   - the game's FaceBlendShape patterns as vertex morphs named "Brow N open|closed" / "Eyes N open|closed" / "Mouth N open|closed"
//     (panel 1 brows, 2 eyes, 3 mouth), the pattern's blend shape key kept in `expressionShapes` by AddExpressionMorphs;
//   - one raw vertex morph per blend shape, named "<mesh>.<shape>" (e.g. "cf_O_face.eye_face.f00_def_cl");
//   - MikuMikuDance's standard morphs (あ, まばたき, ...) as copies of the pattern morphs of the same meaning (AddMmdMorphs).
// This module renames the first two kinds to descriptive, human-readable English names (keeping the old names as the English
// name, for the logs and for tools that want the source identifier), adds display frames per MMD category, and adds the
// extended effects as their own morphs.
//
// Nothing here modifies textures or vertex normals of existing meshes: the effects are separate overlay geometry and material
// morphs, invisible until their morph is driven.

#include "assemble/Scene.h"
#include "export/Exporter.h"
#include "pmx/PmxWriter.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// ---- semantic naming --------------------------------------------------------

// How a raw Koikatsu face shape key parses. Case-sensitive for the side tag (a trailing capital L or R marks the left or the right
// side, as the game's brow shapes use, e.g. mayu00_sianL_cl); everything else compares case-insensitively.
struct FaceStemParts {
    std::string word;      // the core word after the prefix, the state suffix, the set number, the size tag and the side tag are stripped ("a", "gyu", "uresi")
    std::string size;      // "small" (_s) | "smaller" (_ss) | "large" (_l) | "" (none)
    int set = 0;           // the trailing set number (ikari02 -> 2, bisyou_op1 -> 1), 0 when none
    int side = 0;          // +1 left, -1 right, 0 both/unknown
    std::string state;     // "op" | "cl" | "" (no state marker on the key)
    std::string prefix;    // the mesh prefix that was stripped ("f00", "mayu00", "elu00", "sL00", ...), "" when none
    bool known = false;    // `word` is in the registry
};

FaceStemParts ParseFaceStem(const std::string& key);
// The word of an unknown key is empty; the label table returns "" for it.
const std::map<std::string, std::string>& FaceStemLabels();

// Applies semantic names to every morph the exporter generated from the face (expression morphs and raw blend shape morphs; the
// outfit/nude material morphs and MikuMikuDance's own standard morphs are left alone). The previous name is kept in the English name.
// `expressionShapes` maps the expression morph's current name to the key of the pattern it plays (see AddExpressionMorphs). Returns the number of morphs renamed
// (the "before -> after" lines are appended to `renames` when given, for the log).
size_t ApplySemanticNames(pmx::Model& model, const std::map<std::string, std::string>& expressionShapes,
                          std::vector<std::string>* renames = nullptr);

// English-name group morphs for the MMD-standard vowels and the English-name morphs English-speaking users look for, made only when their Japanese source morph exists
// (never a re-invented shape; the geometry is the same vertices): あ->Mouth Ah, い->Mouth Ee, う->Mouth Oo, ん->Mouth Mm, 口角上げ->Mouth Smile, 笑い->Eye Happy.
size_t AddFaceAliases(pmx::Model& model);

// ---- extended facial effects --------------------------------------------------

struct FaceEffectsResult {
    int eyeRoll = -1;       // index of the "Eye Roll" UV morph, -1 when no iris vertices were found
    int heartEyes = -1;     // index of the "Heart Eyes" material morph
    int sweat = -1;         // index of the "Sweat" morph
    int sweatHeavy = -1;    // index of the "Sweat Heavy" morph
    size_t overlayVertices = 0;   // vertices the overlays (heart pupils + sweat droplets) added
};

// Adds the extended effects to the model, as overlay geometry + material morphs (+ one UV morph). Must run after every part and every
// existing morph is in the model, and before TurnAboutY / the display frames are built. `meshBase[i]` is the PMX index of scene mesh i's
// first vertex (as AddExpressionMorphs takes), `headBone` the scene/PMX index of the head bone (1:1 with scene bones), `fullOutDir` the
// export folder (the effect textures are written to its textures/ folder, like every other texture) and `scale` the model scale (scene metres to MMD units).
// Every failure that is not fatal to the export (no iris meshes, no face mesh for the droplet anchors, a missing head bone) leaves that
// one effect out and logs a line; nothing else is disturbed.
FaceEffectsResult AddFaceEffects(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase,
                                 const std::string& outDir, float scale,
                                 const std::function<void(const std::string&)>& log);

// The four facial display frames (Eye, Brow, Mouth, Other), one panel each, appended after the Exp frame by the caller. MikuMikuDance keeps the
// number of morphs ALL display frames list in one byte (each listed item fills a 46-byte record), so the four frames share one `budget` with the Exp
// frame: `alreadyListed` are the morph indices Exp lists (never listed twice) and `budget` is what is left of the 255 slots. The facial morphs are
// split across the panels proportionally (largest remainder) so every tab stays populated; whatever does not fit stays in the file unlisted (the
// engine and tools use it). Pass SIZE_MAX as `budget` to list everything. Empty panels produce no frame.
std::vector<pmx::DisplayFrame> FacialDisplayFrames(const pmx::Model& model, const std::vector<int>& alreadyListed = {}, size_t budget = SIZE_MAX);

} // namespace cardbridge::exporter
