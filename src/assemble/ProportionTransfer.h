#pragma once

// Reference-based body measurement transfer. Builds a set of transfer ops for the existing
// non-destructive ProportionLayer: absolute body-slider values for girth/head/height controls and
// ABMX-style bone length multipliers for segment lengths. The ops are applied by the same assemble /
// reshape pipeline the faithful and normalize paths use, so preview and PMX export stay identical.

#include "assemble/Proportions.h"
#include "card/CardFile.h"
#include "measure/TransferMeasure.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace cardbridge::assemble::transfer {

struct Spec {
    std::string referenceName;
    std::string referenceSha256;
    std::string targetSha256;
    std::array<bool, measure::kTransferCount> selected{};
    std::array<bool, measure::kTransferCount> refine{};
    std::array<float, measure::kTransferCount> targetValue{};     // requested, from the reference
    std::array<float, measure::kTransferCount> achievedValue{};   // last measured, after ops
    std::array<bool, measure::kTransferCount> achievedValid{};
    std::vector<CorrectionOp> ops;
    std::vector<std::string> notes;
};

// Rebuild the ops from a reference/target pair. `workingBody` is the target's current slider vector
// (the viewer's g.tw.body); it is copied, never mutated by this function.
Spec Build(const assemble::Scene& referenceScene, const card::Card& referenceCard, const measure::TransferSet& reference,
           const assemble::Scene& targetScene, const card::Card& targetCard, const measure::TransferSet& target,
           const std::vector<float>& workingBody, const Spec& previous);

// Closed-loop refinement: measureAt(trialBody) must re-shape the target scene and return its
// measurements. Each selected row whose `refine` flag is set gets a bounded 1-D search on its
// owning slider(s) or bone-length multiplier(s). `workingBody` is updated with the best result and
// the ops are updated to match. A row that cannot be reached keeps its nearest result and a note.
using MeasureFn = std::function<measure::TransferSet(const std::vector<float>&)>;
void Refine(Spec& spec, std::vector<float>& workingBody, const std::vector<float>& cardBody, const MeasureFn& measureAt);

// Merge normalize suggestions and transfer ops into one layer. Transfer ops win on every slider
// and bone they own; conflicting normalize ops are dropped so nothing compounds.
ProportionLayer Merge(const ProportionLayer& normalize, const std::vector<CorrectionOp>& transferOps, bool enabled);

// Persist / restore a spec (used by the CLI's --transfer and by the manifest).
std::string ToJson(const Spec& spec);
bool FromJson(const std::string& text, Spec& out, std::string* error = nullptr);

// The body slider vector stored in a card ("shapeValueBody"), padded to 44 with 0.5.
std::vector<float> BodySlidersOf(const card::Card& card);
std::vector<float> FaceSlidersOf(const card::Card& card);

} // namespace cardbridge::assemble::transfer
