// viewer fragment: the Proportions tab — optional, non-destructive character proportion correction (included after State.inc)

// Forward declarations (defined later in Pose.inc / BonesDraw.inc).
bool ReshapeNow(bool final);
void ScheduleApplyTweaks();
void SelectBone(const assemble::Scene& scene, int bone);

// The correction layer the currently displayed scene was built with. Everything else (the export, ReshapeNow) sees the same thing.
assemble::ProportionLayer ProportionLayerNow() {
    assemble::ProportionLayer layer;
    layer.enabled = g.propEnabled;
    for (const auto& s : g.propSugs)
        if (s.applied) layer.ops.push_back(s.op);
    return layer;
}

int ProportionAppliedCount() {
    int n = 0;
    for (const auto& s : g.propSugs) if (s.applied) ++n;
    return n;
}

std::string ProportionSafetyTag(int safety) {
    return safety == assemble::kSafetySafe ? "safe" : safety == assemble::kSafetyApproximate ? "approx" : "manual";
}

// Push the applied slider corrections into the viewer's working slider values (so the Body/Face tabs, the BWH
// measurements and every export show the same numbers), or restore the card's own values where a correction was reverted.
void SyncProportionSliders() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    for (const auto& s : g.propSugs) {
        if (s.op.kind == assemble::kBoneScale) continue;
        std::vector<float>& dst = s.op.kind == assemble::kBodySlider ? g.tw.body : g.tw.face;
        const std::vector<float>& card = s.op.kind == assemble::kBodySlider ? g.tw.bodyCard : g.tw.faceCard;
        if (static_cast<size_t>(s.op.index) >= dst.size() || static_cast<size_t>(s.op.index) >= card.size()) continue;
        dst[static_cast<size_t>(s.op.index)] = s.applied ? s.op.value : card[static_cast<size_t>(s.op.index)];
    }
}

// Fresh analysis + suggestion generation from the card. Keeps the applied state of matching ops, applies everything when
// `applyRecommended` is set (the "Apply recommended corrections" button). Refreshes the diagnostic list too.
void ProportionSuggest(bool applyRecommended) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    g.propFindings = assemble::AnalyzeCard(L->card);
    const std::vector<assemble::CorrectionOp> fresh = assemble::SuggestCorrections(L->card);
    std::vector<PropSugItem> next;
    next.reserve(fresh.size());
    for (const auto& op : fresh) {
        bool applied = applyRecommended;
        if (!applied)
            for (const auto& old : g.propSugs)
                if (old.applied && old.op.kind == op.kind && old.op.index == op.index && old.op.bone == op.bone && old.op.value == op.value) {
                    applied = true;
                    break;
                }
        next.push_back(PropSugItem{op, applied});
    }
    g.propSugs = std::move(next);
    SyncProportionSliders();
}

void ProportionApplyScope(const std::string& scope) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (g.propSugs.empty()) ProportionSuggest(false);
    int applied = 0;
    for (auto& s : g.propSugs)
        if (scope.empty() || s.op.scope == scope) { s.applied = true; ++applied; }
    SyncProportionSliders();
    Log(std::string("proportion corrections applied (") + (scope.empty() ? "overall" : scope) + "): " + std::to_string(applied) + " op(s); toggle any row to revert one");
}

void ProportionResetAll() {
    for (auto& s : g.propSugs) s.applied = false;
    SyncProportionSliders();
}

std::string ProportionSummary() {
    const int n = ProportionAppliedCount();
    return "proportion corrections: " + std::string(n ? "layer on, " : "layer off, ") + std::to_string(n) + " applied";
}

// The Proportions tab: everything about the layer, drawn by the generic tweak-row renderer (kinds 0/2/4/5).
// diagnostics and suggestions come pre-computed (once per load / on demand), never per frame.
std::vector<TweakRow> BuildProportionRows() {
    std::vector<TweakRow> rows;
    const int applied = ProportionAppliedCount();
    rows.push_back({0, -1, "PROPORTION CORRECTION (uncorrected = faithful)", 0});
    rows.push_back({2, kPropMaster, std::string("Correction layer: ") + (g.propEnabled ? "ON" : "OFF") + (applied ? "" : " (empty - faithful)"), 0});
    rows.push_back({4, -1, "Corrections are a separate layer; the card is never modified.", 0});
    rows.push_back({4, -1, "OFF: the original character is shown and exported.", 0});
    rows.push_back({4, -1, "ON: the layer is applied to preview and PMX export (1:1).", 0});

    {   // diagnostics, read-only
        std::string head = "DIAGNOSTICS";
        if (!g.propFindings.empty()) head += " (" + std::to_string(g.propFindings.size()) + ")";
        rows.push_back({0, -1, head, 0});
        if (g.propFindings.empty()) rows.push_back({4, -1, "Nothing flagged - the card looks proportionate.", 0});
        for (const auto& f : g.propFindings) {
            std::string line = "[" + ProportionSafetyTag(f.safety) + "] " + f.property + " - " + f.reason;
            rows.push_back({4, -1, line, 0});
            if (!f.suggestion.empty()) rows.push_back({4, -1, "    -> " + f.suggestion, 0});
        }
    }

    {   // suggested corrections, per-row apply/revert switches
        std::string head = "SUGGESTED CORRECTIONS";
        if (!g.propSugs.empty()) head += " (" + std::to_string(g.propSugs.size()) + ")";
        rows.push_back({0, -1, head, 0});
        if (g.propSugs.empty()) {
            rows.push_back({4, -1, "None yet. \"Suggest corrections\" re-checks this card.", 0});
        } else {
            for (size_t i = 0; i < g.propSugs.size(); ++i)
                rows.push_back({2, kPropSugs0 + static_cast<int>(i), g.propSugs[i].op.label + "  [" + ProportionSafetyTag(g.propSugs[i].op.safety) + "]", 0});
        }
    }

    rows.push_back({5, kPropActSuggest, "Suggest corrections", 0});
    rows.push_back({5, kPropActApply, "Apply recommended corrections", 0});
    rows.push_back({5, kPropActReset, "Reset all corrections", 0});

    rows.push_back({0, -1, "PRESETS", 0});
    rows.push_back({5, kPropPresetHead, "Preset: normalize head proportions", 0});
    rows.push_back({5, kPropPresetBody, "Preset: normalize body proportions", 0});
    rows.push_back({5, kPropPresetFace, "Preset: normalize facial proportions", 0});
    rows.push_back({5, kPropPresetOverall, "Preset: normalize overall proportions", 0});
    return rows;
}

void ProportionsClick(int idx, const TweakRow& row, bool right) {
    if (row.kind == 2) {
        if (row.id == kPropMaster) {
            g.propEnabled = right ? false : !g.propEnabled;
            Log(ProportionSummary());
        } else if (row.id >= kPropSugs0 && row.id < kPropSugs0 + static_cast<int>(g.propSugs.size())) {
            auto& s = g.propSugs[static_cast<size_t>(row.id - kPropSugs0)];
            s.applied = right ? false : !s.applied;
            Log((s.applied ? "proportion correction applied: " : "proportion correction reverted: ") + s.op.label);
            if (s.op.kind == assemble::kBoneScale && !s.op.bone.empty()) {   // highlight the affected bone on the skeleton overlay
                std::shared_ptr<Loaded> L = Current();
                if (L) {
                    const int bi = L->scene.boneIndex(s.op.bone);
                    if (bi >= 0) { SelectBone(L->scene, bi); g.showBones = true; }
                }
            }
        }
        SyncProportionSliders();
        if (!ReshapeNow(true)) ScheduleApplyTweaks();
        InvalidatePanel();
        return;
    }
    if (row.kind != 5) return;
    switch (row.id) {
    case kPropActSuggest:
        ProportionSuggest(false);
        Log("proportion corrections: " + std::to_string(g.propSugs.size()) + " suggestion(s) from this card");
        if (!ReshapeNow(true)) ScheduleApplyTweaks();
        break;
    case kPropActApply:
        if (g.propSugs.empty()) ProportionSuggest(false);
        for (auto& s : g.propSugs) s.applied = true;
        SyncProportionSliders();
        Log("proportion corrections applied (" + std::to_string(ProportionAppliedCount()) + " op(s)); toggle any row to revert one");
        if (!ReshapeNow(true)) ScheduleApplyTweaks();
        break;
    case kPropActReset:
        ProportionResetAll();
        Log("proportion corrections reset - faithful");
        if (!ReshapeNow(true)) ScheduleApplyTweaks();
        break;
    case kPropPresetHead: ProportionSuggest(false); ProportionApplyScope("head"); break;
    case kPropPresetBody: ProportionSuggest(false); ProportionApplyScope("body"); break;
    case kPropPresetFace: ProportionSuggest(false); ProportionApplyScope("face"); break;
    case kPropPresetOverall: ProportionSuggest(false); ProportionApplyScope(""); break;
    }
    InvalidatePanel();
}
