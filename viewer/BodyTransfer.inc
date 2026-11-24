// viewer fragment: the Body Transfer tab. A separate, non-destructive measurement layer over the card.
// The reference is loaded into its own scene (never g.loaded), the measurements are read from actual
// geometry, and Apply folds the resulting ops into ProportionLayerNow() so the preview and the PMX
// export see exactly the same character.

// The layer contribution of this tab, merged by ProportionLayerNow(). Empty when nothing has been
// applied, or while the user is looking at the "original" side of the before/after preview.
assemble::ProportionLayer BodyTransferLayer() {
    if (!g.bt.applied || g.bt.previewOriginal) return assemble::ProportionLayer{};
    return assemble::transfer::Merge(assemble::ProportionLayer{}, g.bt.appliedSpec.ops, true);
}

bool BTHasReference() {
    return g.bt.selectedRef >= 0 && static_cast<size_t>(g.bt.selectedRef) < g.bt.refs.size() &&
           g.bt.refs[static_cast<size_t>(g.bt.selectedRef)].ready;
}

// Rebuild the editable spec from the selected reference and the current target. Explicit target values
// the user typed are kept when the reference has not changed; picking a different reference starts fresh.
void BTRebuild() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || !BTHasReference()) return;
    const TransferRefEntry& ref = g.bt.refs[static_cast<size_t>(g.bt.selectedRef)];
    const auto targetSet = measure::MeasureTransfer(L->scene);
    assemble::transfer::Spec prev = g.bt.spec;
    if (prev.referenceSha256 != ref.sha256) {
        prev.targetValue.fill(0.0f);
        prev.selected.fill(false);
        prev.refine.fill(g.bt.refine);
    }
    g.bt.spec = assemble::transfer::Build(*ref.scene, *ref.card, ref.measurements,
                                          L->scene, L->card, targetSet, g.tw.body, prev);
    bool any = false;
    for (bool b : g.bt.spec.selected) any = any || b;
    if (!any && !g.bt.spec.ops.empty()) {
        for (int i = 0; i < measure::kTransferCount; ++i)
            if (g.bt.spec.targetValue[i] > 0.0f) g.bt.spec.selected[static_cast<size_t>(i)] = true;
        g.bt.spec = assemble::transfer::Build(*ref.scene, *ref.card, ref.measurements,
                                              L->scene, L->card, targetSet, g.tw.body, g.bt.spec);
    }
    g.bt.status = "reference: " + ref.name + " - " + std::to_string(ref.measurements.size()) + " invalid measurement(s)";
}

// Add a reference card. The worker assembles it on its own and posts WM_BT_READY; g.loaded is untouched.
void BTAddReference(const std::string& path) {
    if (path.empty()) return;
    std::shared_ptr<resolve::Resolver> resolver;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        resolver = g.resolver;
    }
    const size_t slash = path.find_last_of("/\\");
    TransferRefEntry entry;
    entry.path = path;
    entry.name = slash == std::string::npos ? path : path.substr(slash + 1);
    entry.loading = true;
    int index = 0, generation = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.bt.refs.push_back(entry);
        index = static_cast<int>(g.bt.refs.size()) - 1;
        generation = ++g.bt.loadGeneration;
        g.bt.status = "loading reference " + entry.name + "...";
    }
    std::thread([path, index, generation, resolver] {
        std::shared_ptr<card::Card> card;
        std::shared_ptr<assemble::Scene> scene;
        measure::TransferSet set;
        std::string error;
        try {
            if (!resolver) throw std::runtime_error("set the asset library in Setup first");
            card = std::make_shared<card::Card>(card::LoadCard(path));
            if (!card->supported) throw std::runtime_error(card->supportNote);
            assemble::AssembleOptions ao;
            scene = std::make_shared<assemble::Scene>(assemble::Assemble(*card, *resolver, ao));
            set = measure::MeasureTransfer(*scene);
        } catch (const std::exception& e) {
            error = e.what();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (generation != g.bt.loadGeneration || static_cast<size_t>(index) >= g.bt.refs.size()) return;
            TransferRefEntry& e = g.bt.refs[static_cast<size_t>(index)];
            e.loading = false;
            if (!error.empty()) {
                e.error = error;
                e.name += " (failed)";
            } else {
                e.card = std::move(card);
                e.scene = std::move(scene);
                e.measurements = set;
                e.sha256 = e.card->sha256;
                e.name = e.card->displayName();
                e.ready = true;
                g.bt.selectedRef = index;
            }
        }
        if (g.hwnd) PostMessageW(g.hwnd, WM_BT_READY, static_cast<WPARAM>(index), static_cast<LPARAM>(generation));
    }).detach();
    InvalidatePanel();
}

void BTOpenReference() {
    const std::string path = PickCard(g.hwnd);
    if (!path.empty()) BTAddReference(path);
}

void BodyTransferReady(int index, int generation) {
    (void)generation;
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (index >= 0 && static_cast<size_t>(index) < g.bt.refs.size() && !g.bt.refs[static_cast<size_t>(index)].error.empty())
        Log(std::string("reference load failed: ") + g.bt.refs[static_cast<size_t>(index)].error, LogKind::Error);
    BTRebuild();
    InvalidatePanel();
}

// Apply selected measurements. Refinement runs on a scratch scene so the live character is never
// touched until the final ReshapeNow, which uses the same layer the exporter will see.
void BTApply() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || !BTHasReference()) { Log("load a reference card first", LogKind::Warn); return; }
    if (g.bt.spec.ops.empty()) BTRebuild();
    if (g.bt.spec.ops.empty()) { Log("no transferable measurement is selected", LogKind::Warn); return; }
    auto scratch = std::make_shared<assemble::Scene>(L->scene);
    assemble::AssembleOptions ao;
    ao.outfit = L->outfit;
    ao.faceShape = g.tw.face;
    // Bone ops only here: the slider values travel in `bodyShape` during the search. Slider transfer ops
    // are added back by ProportionLayerNow() for the final (and exported) shape.
    for (const auto& op : g.bt.spec.ops)
        if (op.kind == assemble::kBoneScale) ao.proportionLayer.ops.push_back(op);
    ao.proportionLayer.enabled = true;
    const auto measureAt = [&](const std::vector<float>& trial) {
        ao.bodyShape = trial;
        if (!assemble::Reshape(*scratch, L->card, *g.resolver, ao)) return measure::TransferSet{};
        return measure::MeasureTransfer(*scratch);
    };
    g.bt.spec.refine.fill(g.bt.refine);   // one switch for the whole panel: copying controls alone is much faster
    assemble::transfer::Refine(g.bt.spec, g.tw.body, g.tw.bodyCard, measureAt);
    g.bt.appliedSpec = g.bt.spec;
    g.bt.appliedBody = g.tw.body;
    g.bt.applied = true;
    g.bt.previewOriginal = false;
    g.bt.status = "transfer applied: " + std::to_string(g.bt.spec.ops.size()) + " op(s)";
    Log(g.bt.status, LogKind::Good);
    for (const auto& note : g.bt.spec.notes) Log(note, LogKind::Warn);
    if (!ReshapeNow(true)) { ScheduleApplyTweaks(); g.bt.status += " (full re-assembly scheduled)"; }
    else {
        const auto appliedSet = measure::MeasureTransfer(L->scene);
        for (int i = 0; i < measure::kTransferCount; ++i)
            if (g.bt.spec.selected[static_cast<size_t>(i)]) {
                g.bt.spec.achievedValue[static_cast<size_t>(i)] = appliedSet[static_cast<size_t>(i)].value;
                g.bt.spec.achievedValid[static_cast<size_t>(i)] = appliedSet[static_cast<size_t>(i)].valid;
            }
        g.bt.appliedSpec = g.bt.spec;
    }
    InvalidatePanel();
}

void BTReset() {
    g.bt.applied = false;
    g.bt.appliedSpec = assemble::transfer::Spec{};
    g.bt.appliedBody.clear();
    g.bt.previewOriginal = false;
    g.tw.body = g.tw.bodyCard;
    g.bt.status = "reset to original";
    Log("body transfer: original measurements restored", LogKind::Good);
    if (!ReshapeNow(true)) ScheduleApplyTweaks();
    InvalidatePanel();
}

void BTPreview() {
    if (!g.bt.applied) { Log("nothing to compare yet - apply a transfer first", LogKind::Warn); return; }
    if (!g.bt.previewOriginal) g.bt.appliedBody = g.tw.body;
    g.bt.previewOriginal = !g.bt.previewOriginal;
    g.tw.body = g.bt.previewOriginal ? g.tw.bodyCard : g.bt.appliedBody;
    g.bt.status = g.bt.previewOriginal ? "preview: original target" : "preview: transferred measurements";
    if (!ReshapeNow(true)) ScheduleApplyTweaks();
    InvalidatePanel();
}

// Show only the nude body (the part the measurements come from). g.hidden: 1 hide, 2 reveal an
// under piece. The previous visibility is restored on toggle-off or on loading another card.
void BTNudePreview() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (g.bt.nudePreview) {
        g.bt.nudePreview = false;
        if (g.bt.savedHidden.size() == g.hidden.size()) g.hidden = g.bt.savedHidden;
        else g.hidden.assign(L->scene.parts.size(), 0);
        g.bt.savedHidden.clear();
    } else {
        g.bt.savedHidden = g.hidden;
        g.bt.nudePreview = true;
        for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i) {
            const auto& p = L->scene.parts[i];
            if (p.nudeSwitch) g.hidden[i] = 0;
            else if (p.group == "Outfit" || p.group == "Accessories") g.hidden[i] = 1;
        }
    }
    g.dirty = true;
    InvalidateAll();
}

void BTSelectAll(bool select) {
    for (int i = 0; i < measure::kTransferCount; ++i)
        if (g.bt.spec.targetValue[static_cast<size_t>(i)] > 0.0f) g.bt.spec.selected[static_cast<size_t>(i)] = select;
    InvalidatePanel();
}

bool BTMeasureSliderFor(int id, SliderRef& r) {
    const int i = BodyTransferMeasureOf(id);
    if (i < 0 || i >= measure::kTransferCount) return false;
    const bool ratio = measure::TransferIsRatio(i);
    r.get = [i] { return g.bt.spec.targetValue[static_cast<size_t>(i)]; };
    r.set = [i](float v) {
        if (!std::isfinite(v)) return;
        g.bt.spec.targetValue[static_cast<size_t>(i)] = v;
        g.bt.spec.selected[static_cast<size_t>(i)] = v > 0.0f;
    };
    r.def = g.bt.spec.targetValue[static_cast<size_t>(i)];
    r.lo = 0.0f;
    r.hi = ratio ? 3.0f : 250.0f;
    r.step = 0.0f;
    r.percent = false;
    r.fmt = ratio ? "%.2f x" : "%.1f cm";
    return true;
}

std::vector<TweakRow> BuildBodyTransferRows() {
    std::vector<TweakRow> rows;
    rows.push_back({0, -1, "REFERENCE CHARACTER", ""});
    rows.push_back({5, kBTOpenRef, "Load reference card...", ""});
    rows.push_back({5, kBTRefresh, "Refresh measurements / rebuild spec", ""});
    for (size_t i = 0; i < g.bt.refs.size(); ++i) {
        const TransferRefEntry& e = g.bt.refs[i];
        std::string label = (static_cast<int>(i) == g.bt.selectedRef ? "[x] " : "[ ] ") + e.name;
        if (e.loading) label += " (loading...)";
        else if (!e.error.empty()) label += " - " + e.error;
        rows.push_back({5, 24050 + static_cast<int>(i), label, ""});
    }
    if (g.bt.refs.empty()) rows.push_back({4, 0, "The reference is measured from its assembled skin and bones.", ""});
    rows.push_back({4, 0, g.bt.status, ""});
    rows.push_back({0, -1, "TARGET CHARACTER", ""});
    if (std::shared_ptr<Loaded> L = Current())
        rows.push_back({4, 0, L->card.displayName() + " (the card currently on screen)", ""});
    else
        rows.push_back({4, 0, "no character loaded", ""});
    rows.push_back({4, 0, "The target keeps its own face, hair, outfit, textures and accessories.", ""});

    rows.push_back({0, -1, "MEASUREMENTS (select / requested value)", ""});
    if (!BTHasReference()) {
        rows.push_back({4, 0, "Load a reference card to see its measurements.", ""});
    } else {
        for (int i = 0; i < measure::kTransferCount; ++i) {
            const auto& ref = g.bt.refs[static_cast<size_t>(g.bt.selectedRef)].measurements[static_cast<size_t>(i)];
            char buf[160];
            const char* unit = measure::TransferUnit(i);
            if (measure::TransferIsRatio(i))
                std::snprintf(buf, sizeof buf, "%s  ref %.2f%s / now %.2f%s%s", measure::TransferName(i),
                              ref.value, unit, g.bt.spec.achievedValue[static_cast<size_t>(i)], unit, ref.valid ? "" : " (unavailable)");
            else
                std::snprintf(buf, sizeof buf, "%s  ref %.1f%s / now %.1f%s%s", measure::TransferName(i),
                              ref.value, unit, g.bt.spec.achievedValue[static_cast<size_t>(i)], unit, ref.valid ? "" : " (unavailable)");
            rows.push_back({2, kBTSelect0 + i, buf, ""});
            rows.push_back({1, kBTMeasure0 + i, std::string("  target ") + unit, ""});
        }
    }
    rows.push_back({5, kBTSelectAll, "Select all available measurements", ""});
    rows.push_back({5, kBTDeselectAll, "Deselect all", ""});
    rows.push_back({5, kBTNude, g.bt.nudePreview ? "Nude body preview: ON (show clothes again)" : "Nude body preview: OFF (see proportions)", ""});
    rows.push_back({5, kBTRefine, g.bt.refine ? "Measured refinement: ON (slower, matches cm)" : "Measured refinement: OFF (copy control values)", ""});
    rows.push_back({5, kBTApply, "Apply Transfer", ""});
    rows.push_back({5, kBTReset, "Reset to Original", ""});
    rows.push_back({5, kBTPreview, g.bt.previewOriginal ? "Preview: ORIGINAL (click for transferred)" : "Preview: TRANSFERRED (click for original)", ""});
    for (const auto& note : g.bt.spec.notes) rows.push_back({4, 0, note, ""});
    rows.push_back({4, 0, "Only the selected body measurements change: face, hair, clothes, textures, accessories and bone names are preserved.", ""});
    return rows;
}

void BodyTransferClick(int idx, const TweakRow& row, bool right) {
    (void)idx;
    if (row.kind == 2) {
        const int i = row.id - kBTSelect0;
        if (i >= 0 && i < measure::kTransferCount) g.bt.spec.selected[static_cast<size_t>(i)] = !right && !g.bt.spec.selected[static_cast<size_t>(i)];
        InvalidatePanel();
        return;
    }
    if (row.kind != 5) return;
    if (row.id >= 24050 && row.id < 24100) {
        g.bt.selectedRef = row.id - 24050;
        BTRebuild();
        InvalidatePanel();
        return;
    }
    switch (row.id) {
    case kBTOpenRef: BTOpenReference(); break;
    case kBTRefresh: BTRebuild(); InvalidatePanel(); break;
    case kBTNude: BTNudePreview(); InvalidatePanel(); break;
    case kBTRefine: g.bt.refine = !g.bt.refine; g.bt.spec.refine.fill(g.bt.refine); InvalidatePanel(); break;
    case kBTSelectAll: BTSelectAll(true); break;
    case kBTDeselectAll: BTSelectAll(false); break;
    case kBTApply: BTApply(); break;
    case kBTReset: BTReset(); break;
    case kBTPreview: BTPreview(); break;
    default: break;
    }
}
