#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"
#include "core/Math.h"
#include "shape/Shape.h"

#include <string>
#include <vector>

namespace cardbridge::assemble {

// Optional, non-destructive proportion correction. The imported card, its shapeValueBody /
// shapeValueFace arrays and its ABMX table are only read here: corrections live in a separate
// override layer (ProportionLayer) that the assemble / reshape pipeline applies after the card's
// own data and after ABMX. With the layer empty or disabled the pipeline performs exactly the
// passes it performs without this feature.
//
// The recognizable design is preserved: suggestions move a value toward neutral, half-way
// (0.5 + (v - 0.5) * 0.5), never all the way; ABMX bone scales are partially neutralised,
// never zeroed, and rotations / positions are never touched.

enum CorrectionKind { kBodySlider = 0, kFaceSlider = 1, kBoneScale = 2 };

// Safety of a suggestion: 0 = safe (in-range, halves toward neutral), 1 = approximate
// (SliderUnlocker / ABMX, half-way back to neutral), 2 = manual review (informational only).
inline constexpr int kSafetySafe = 0;
inline constexpr int kSafetyApproximate = 1;
inline constexpr int kSafetyManual = 2;

struct CorrectionOp {
    int kind = kBodySlider;
    int index = -1;          // slider index for kBodySlider / kFaceSlider (body 44, face 52)
    std::string bone;        // kBoneScale only
    Vec3 scaleMul{1, 1, 1};  // kBoneScale: multiplier on the bone's local scale
    float value = 0.5f;      // kBodySlider / kFaceSlider: absolute slider rate to use
    std::string label, why;
    int safety = kSafetySafe;
    std::string scope;       // "body" | "face" | "head"
};

struct ProportionLayer {
    bool enabled = true;
    std::vector<CorrectionOp> ops;
    bool empty() const { return ops.empty(); }
    // The op for a slider (first match), or nullptr.
    const CorrectionOp* forSlider(int kind, int index) const {
        if (!enabled) return nullptr;
        for (const CorrectionOp& o : ops)
            if (o.kind == kind && o.index == index) return &o;
        return nullptr;
    }
    // The op for a bone (first match), or nullptr.
    const CorrectionOp* forBone(const std::string& bone) const {
        if (!enabled) return nullptr;
        for (const CorrectionOp& o : ops)
            if (o.kind == kBoneScale && !o.bone.empty() && o.bone == bone) return &o;
        return nullptr;
    }
};

// One diagnostic finding about a card. Informational — the presence of a finding does not mean
// the character is broken, and no caller may apply a correction because of one.
struct ProportionFinding {
    std::string property;    // e.g. "Overall · Head size"
    std::string current;     // the current value, formatted
    std::string reason;      // why this was flagged
    std::string suggestion;  // what fixing it would do (empty for notes without a suggestion)
    int safety = kSafetyApproximate;
    std::string scope;       // "body" | "face" | "head"
    int opIndex = -1;        // index into the SuggestCorrections() vector, or -1 for notes
};

// Analyze the card's identity data only (sliders + ABMX table). Never touches a scene.
std::vector<ProportionFinding> AnalyzeCard(const card::Card& card);

// The suggestions that correspond to the findings (a finding with safety < 2), in finding order.
// `onlyScope` (empty = all) filters to one of "body" / "face" / "head".
std::vector<CorrectionOp> SuggestCorrections(const card::Card& card, const std::string& onlyScope = std::string());

// Presets: suggestion sets, never applied by themselves.
std::vector<CorrectionOp> PresetNormalizeHead(const card::Card& card);
std::vector<CorrectionOp> PresetNormalizeBody(const card::Card& card);
std::vector<CorrectionOp> PresetNormalizeFace(const card::Card& card);
std::vector<CorrectionOp> PresetNormalizeOverall(const card::Card& card);

// The value the shape sampler should use for a slider: the card's value, unless the layer overrides it.
float SliderValueCorrected(float baseValue, const ProportionLayer& layer, int kind, int index);

// The bone's local scale after the corrections: the computed scale (shape * ABMX) times every
// bone op for that bone. `changed` is set when the layer affects the bone.
Vec3 BoneScaleCorrected(Vec3 baseScale, const ProportionLayer& layer, const std::string& bone, bool* changed = nullptr);

// Apply format / validate a layer read from a file (the CLI's --proportions). False with `error`
// when the text is unusable. The layer is rejected wholesale on any problem, never partially applied.
bool ProportionLayerFromJson(const std::string& text, ProportionLayer& out, std::string* error = nullptr);
std::string ProportionLayerToJson(const ProportionLayer& layer);

} // namespace cardbridge::assemble
