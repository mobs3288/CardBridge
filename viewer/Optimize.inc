// viewer fragment: the Optimize tab (included in order by main.cpp)
// Triangle reduction with per-part-class preservation (face, hair, body, clothes, accessories), quality
// presets and a custom slider, and non-destructive LOD generation (every level is made from the
// original geometry, never from another level). The simplification itself runs on a worker; this file
// owns the controls, the cached results and the "Apply" state the export and the as-exported bake read.

// ---- helpers ----------------------------------------------------------------
static std::string Grouped(size_t n) {   // 100000 -> "100,000"
    std::string t = std::to_string(n);
    for (size_t i = t.size(); i > 3; i -= 3) t.insert(i - 3, ",");
    return t;
}

static size_t OptOriginalTriangles() {
    std::shared_ptr<Loaded> L = Current();
    return L ? Triangles(L->scene) : 0;
}
static size_t OptEstimatedTriangles() {   // exact when the cache matches the slider, a plain estimate otherwise
    const size_t orig = OptOriginalTriangles();
    if (g.opt.simplifiedTris > 0 && g.opt.simplifiedFor >= 0.0f && std::fabs(g.opt.simplifiedFor - g.opt.reduction) < 1e-6f) return g.opt.simplifiedTris;
    return static_cast<size_t>(std::llround(orig * (1.0f - std::clamp(g.opt.reduction, 0.0f, 0.9f))));
}

// ---- layout (shared by the drawing and the hit tests) ------------------------
struct OptLayout {
    RECT chips[6];
    RECT slider;            // the wide reduction track (the hit area extends above and below it)
    RECT lodRows[4];        // one row per LOD (checkbox + stats)
    RECT lodDown[4], lodUp[4];   // per-level target steppers
    RECT generate, apply, reset;
    RECT preview[6];        // original, simplified, LOD 1..4
    int headerY, presetCapY, redCapY, trackY, capY, div1Y, lodCapY, div2Y, statsY, previewCapY, previewY, applyY, noteY;
    int totalH = 0;         // the content height (for the scrollbar)
};

// `scroll` (pixels) shifts every row of the content up, so the tab can scroll inside the list area like every other tab.
OptLayout OptLayoutFor(int partsTop, int scroll) {
    constexpr int left = 12, right = kPanel - 12;
    OptLayout o;
    int y = partsTop + 4 - scroll;
    o.headerY = y; y += 22;
    o.presetCapY = y; y += 13;
    for (int r = 0; r < 2; ++r) {
        const int w = (kPanel - 2 * left - 2 * 6) / 3;
        int x = left;
        for (int c = 0; c < 3; ++c) {
            o.chips[r * 3 + c] = RECT{x, y, x + w, y + 22};
            x += w + 6;
        }
        y += 27;
    }
    y += 2;
    o.redCapY = y; y += 21;
    o.trackY = y; o.slider = RECT{left, y, right, y + 4}; y += 19;
    o.capY = y; y += 15;
    y += 2;
    o.div1Y = y; y += 7;
    o.lodCapY = y; y += 15;
    for (int i = 0; i < 4; ++i) {
        o.lodRows[i] = RECT{8, y, kPanel - 8, y + 23};
        o.lodDown[i] = RECT{kPanel - 96, y + 2, kPanel - 74, y + 21};
        o.lodUp[i] = RECT{kPanel - 26, y + 2, kPanel - 4, y + 21};
        y += 25;
    }
    y += 2;
    o.generate = RECT{left, y, right, y + 23}; y += 29;
    o.div2Y = y; y += 7;
    o.statsY = y; y += 3 * 15 + 3;
    o.previewCapY = y; y += 13;
    o.previewY = y;
    const int pw[6] = {60, 76, 46, 46, 46, 46};
    int x = left;
    for (int i = 0; i < 6; ++i) { o.preview[i] = RECT{x, y, x + pw[i], y + 20}; x += pw[i] + 5; }
    y += 26;
    o.applyY = y;
    o.apply = RECT{left, y, left + 152, y + 25};
    o.reset = RECT{left + 160, y, right, y + 25};
    o.noteY = y + 31;
    o.totalH = y + 48 + scroll - partsTop;   // y carries -scroll already; total covers the two note lines
    return o;
}

static RECT OptSliderHit(const OptLayout& o) { return RECT{o.slider.left, o.slider.top - 8, o.slider.right, o.slider.bottom + 10}; }

// ---- actions ---------------------------------------------------------------

// A target stepper on an LOD row: moves that level's target by 5% within 5..95% and drops its stale scene.
void OptLodStep(int level, bool up) {
    const int i = std::clamp(level, 0, 3);
    const float delta = up ? 0.05f : -0.05f;
    g.opt.lodTarget[i] = std::clamp(std::round((g.opt.lodTarget[i] + delta) * 100.0f) / 100.0f, 0.05f, 0.95f);
    OptLod& lv = g.opt.lods[static_cast<size_t>(i)];
    lv.scene.reset();          // the stored level is for the old target; drop it
    lv.triangles = lv.vertices = 0;
    lv.seconds = 0.0;
    lv.failed = false;
    g.opt.stale = true;
    Log(std::string("LOD ") + std::to_string(i + 1) + ": target " + std::to_string(static_cast<int>(std::lround(g.opt.lodTarget[i] * 100))) + "% remaining");
    InvalidatePanel();
}

// Move the wide reduction slider from a pointer x. Snaps to 1 % steps; a preset is lit when it matches exactly.
void OptSliderTo(int x) {
    const OptLayout o = OptLayoutFor(Lay(ClientHeight()).partsTop, g.optScroll);
    const int left = o.slider.left + 8, right = o.slider.right - 8;
    if (right <= left) return;
    g.opt.reduction = std::clamp(static_cast<float>(x - left) / static_cast<float>(right - left), 0.0f, 1.0f);
    g.opt.reduction = std::round(g.opt.reduction * 100.0f) / 100.0f;
    int match = -1;
    for (int i = 0; i < 5; ++i) if (std::fabs(g.opt.reduction - mesh::kSimplifyPresets[i].reduction) < 1e-6f) match = i;
    if (match >= 0) g.opt.preset = match; else if (g.opt.preset != 5) g.opt.preset = 5;
    g.opt.stale = true;
    InvalidatePanel();
}

// The preview the view shows changed: move the GPU copy to it. Heavy; call only when something is displayed.
void OptPreviewUpload() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || !g.gpuOk) return;
    const assemble::Scene* src = nullptr;
    if (g.opt.previewLod == 0) src = &L->scene;
    else if (g.opt.previewLod == 1) src = g.opt.simplified.get();
    else if (g.opt.previewLod >= 2) src = g.opt.lods[static_cast<size_t>(g.opt.previewLod - 2)].scene.get();
    if (!src) return;
    ProgBegin(g.opt.previewLod == 0 ? "Uploading the original model" : "Uploading the optimized model");
    ProgSet(0.02, "uploading");
    g.gpu.Unload();   // release the previous character's GPU copy before re-creating it
    std::string why;
    if (g.gpu.Load(*src, &why, [](double f, const std::string& stage) { ProgSet(0.02 + f * 0.96, stage); }, [] { return ProgCancelled(); })) {
        g.gpuScene = L;   // the loaded character the copy belongs to (the GPU's copy reads the scene pointer we pass in)
        g.gpuOk = true;
        g.dirty = true;
    } else {
        g.gpuOk = false;
        Log("preview upload failed: " + why, LogKind::Warn);
    }
    ProgEnd();
    if (g.gpuOk) ApplyPoseLive();   // the uploaded view starts at the rest pose; pose it exactly like the loaded card (bones are identical)
    InvalidateView();
}

void OptPreviewTo(int which) {
    if (which < 0 || which > 5) return;
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const bool available = which == 0 || (which == 1 && g.opt.simplified != nullptr) || (which >= 2 && g.opt.lods[static_cast<size_t>(which - 2)].scene != nullptr);
    if (!available) {
        if (which == 1) Log("the simplified preview is not generated yet; release the slider or press Generate", LogKind::Warn);
        else Log("generate the LODs first (Generate LODs)", LogKind::Warn);
        return;
    }
    if (g.opt.previewLod == which) return;
    g.opt.previewLod = which;
    InvalidatePanel();
    OptPreviewUpload();
}

// Generate the slider-simplified preview scene from the original (worker). On completion WM_OPT_READY runs the adoption.
void OptGeneratePreview() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (g.busy || g.exporting) { Log("still busy; wait for the current job to finish", LogKind::Warn); return; }
    if (g.opt.reduction <= 0.0f) {   // reduction 0: the preview is the original, no copy needed
        g.opt.simplified.reset();
        g.opt.simplifiedFor = 0.0f;
        g.opt.simplifiedTris = g.opt.simplifiedVerts = 0;
        g.opt.previewLod = 0;
        OptPreviewUpload();
        InvalidatePanel();
        return;
    }
    g.opt.busy = true;
    g.busy = true;
    const float reduction = std::clamp(g.opt.reduction, 0.0f, 0.9f);
    ProgBegin("Simplifying the preview (" + std::to_string(static_cast<int>(reduction * 100)) + "% reduction)");
    assemble::Scene sceneCopy = L->scene;   // taken here on the UI thread; the worker simplifies its own copy from this pristine one
    std::thread([reduction, sceneCopy = std::move(sceneCopy)]() mutable {
        g.optPending.kind = 0;
        g.optPending.simplified = nullptr;
        g.optPending.simplifiedFor = reduction;
        try {
            mesh::SimplifyOptions so;
            so.maxReduction = reduction;
            so.cancelled = [] { return ProgCancelled(); };
            g.optPending.simplified = mesh::SimplifyScene(sceneCopy, reduction, so, g.optPending.simplifiedStats,
                [](float f, const std::string& stage) { ProgSet(f, stage); });
            for (const auto& w : g.optPending.simplifiedStats.warnings) Log("warning: " + w, LogKind::Warn);
        } catch (const std::exception& e) {
            Log(std::string("preview simplification failed: ") + e.what(), LogKind::Error);
        }
        g.optPending.done = g.optPending.simplified != nullptr;
        g.optPending.cancelled = g.optPending.simplified == nullptr && ProgCancelled();
        PostMessageW(g.hwnd, WM_OPT_READY, g.optPending.cancelled ? 1 : g.optPending.done ? 0 : 2, 0);
    }).detach();
}

// Generate the selected LOD levels (worker). Every level starts from the pristine copy; LOD 0 is the untouched original.
void OptGenerateLods() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    if (g.busy || g.exporting) { Log("still busy; wait for the current job to finish", LogKind::Warn); return; }
    std::vector<mesh::LodLevelSpec> levels;
    
    static const char* kName[4] = {"LOD 1", "LOD 2", "LOD 3", "LOD 4"};
    for (int i = 0; i < 4; ++i) if (g.opt.lodSelected[i]) levels.push_back({"lod" + std::to_string(i + 1), kName[i], g.opt.lodTarget[i]});
    if (levels.empty()) { Log("no LOD levels selected", LogKind::Warn); return; }
    g.opt.busy = true;
    g.busy = true;
    for (int i = 0; i < 4; ++i) if (g.opt.lodSelected[i]) g.opt.lods[static_cast<size_t>(i)].pending = true;
    InvalidatePanel();
    ProgBegin("Generating the LODs (all from the original geometry)");
    assemble::Scene sceneCopy = L->scene;
    std::thread([sceneCopy = std::move(sceneCopy), levels]() mutable {
        g.optPending.kind = 1;
        g.optPending.lods.clear();
        try {
            mesh::SimplifyOptions so;
            so.cancelled = [] { return ProgCancelled(); };
            auto results = mesh::GenerateLods(sceneCopy, levels, so, [](float f, const std::string& stage) { ProgSet(f, stage); });
            g.optPending.lods = std::move(results);
            for (const auto& r : g.optPending.lods) for (const auto& w : r.warnings) Log("warning: " + w, LogKind::Warn);
        } catch (const std::exception& e) {
            Log(std::string("LOD generation failed: ") + e.what(), LogKind::Error);
        }
        g.optPending.done = !g.optPending.lods.empty();
        g.optPending.cancelled = g.optPending.lods.empty() && ProgCancelled();
        PostMessageW(g.hwnd, WM_OPT_READY, g.optPending.cancelled ? 1 : g.optPending.done ? 0 : 2, 0);
    }).detach();
}

void OptApply() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    OptApplied a;
    a.reduction = std::clamp(g.opt.reduction, 0.0f, 0.9f);
    
    static const char* kLodName[4] = {"LOD 1", "LOD 2", "LOD 3", "LOD 4"};
    for (int i = 0; i < 4; ++i) if (g.opt.lodSelected[i]) a.lods.push_back({kLodName[i], g.opt.lodTarget[i]});
    a.set = a.reduction > 0.0f || !a.lods.empty();
    g.opt.applied = std::move(a);
    if (!g.opt.applied.set) {
        Log("applied: the export writes the original character (nothing to optimise)", LogKind::Info);
        g.opt.previewLod = 0;
        OptPreviewUpload();
        InvalidatePanel();
        return;
    }
    std::string what = std::to_string(static_cast<int>(std::lround(g.opt.applied.reduction * 100))) + "% reduction";
    if (!g.opt.applied.lods.empty()) {
        what += " + ";
        
        bool first = true;
        for (int i = 0; i < 4; ++i)
            if (g.opt.lodSelected[i]) { what += std::string(first ? "" : ", ") + "LOD " + std::to_string(i + 1) + " (" + std::to_string(static_cast<int>(g.opt.lodTarget[i] * 100)) + "%)"; first = false; }
    }
    Log("applied to the export: " + what, LogKind::Good);
    if (g.opt.applied.reduction > 0.0f) {
        const bool cacheFresh = g.opt.simplified && g.opt.simplifiedFor >= 0.0f && std::fabs(g.opt.simplifiedFor - g.opt.reduction) < 1e-6f;
        if (cacheFresh) { if (g.opt.previewLod != 1) OptPreviewTo(1); }   // the cache is ready: show the simplified model at once
        else { g.opt.previewLod = 1; InvalidatePanel(); OptGeneratePreview(); }   // WM_OPT_READY re-uploads it when the simplification lands
    }
    InvalidatePanel();
}

void OptReset() {
    OptApplied cleared;
    g.opt.applied = std::move(cleared);
    g.opt.reduction = 0.0f;
    g.opt.preset = 0;   // Original
    g.opt.stale = true;
    g.opt.simplified.reset();
    g.opt.simplifiedFor = -1.0f;
    g.opt.simplifiedTris = g.opt.simplifiedVerts = 0;
    g.opt.simplifiedSeconds = 0.0;
    for (int i = 0; i < 4; ++i) g.opt.lods[static_cast<size_t>(i)] = {};
    const float kStock[4] = {0.6f, 0.3f, 0.15f, 0.05f};
    for (int i = 0; i < 4; ++i) g.opt.lodTarget[i] = kStock[i];   // back to the stock targets too
    g.opt.previewLod = 0;
    OptPreviewUpload();
    Log("reset to original: the export writes the untouched character", LogKind::Info);
    InvalidatePanel();
}

// The worker's results are waiting: adopt them, and move the preview to whatever is being shown now.
// `wp` is the WM_OPT_READY wParam: 1 = cancelled, 2 = failed (adopted slots that never came back are marked failed, not left "pending").
void OptAdoptPending(int wp) {
    const int kind = g.optPending.kind;
    const bool ok = wp == 0 && g.optPending.done;
    if (kind == 0) {          // the slider preview
        if (ok) {
            g.opt.simplified = std::move(g.optPending.simplified);
            g.opt.simplifiedFor = g.optPending.simplifiedFor;
            const size_t before = g.optPending.simplifiedStats.originalTriangles;
            g.opt.simplifiedTris = g.optPending.simplifiedStats.resultTriangles;
            g.opt.simplifiedVerts = g.optPending.simplifiedStats.resultVertices;
            g.opt.simplifiedSeconds = g.optPending.simplifiedStats.seconds;
            g.opt.stale = false;
            const double kept = before > 0 ? 100.0 * (1.0 - static_cast<double>(g.opt.simplifiedTris) / static_cast<double>(before)) : 0.0;
            char keptBuf[16];
            std::snprintf(keptBuf, sizeof keptBuf, "%.1f", kept);
            Log("simplified preview: " + Grouped(before) + " -> " + Grouped(g.opt.simplifiedTris) + " triangles (-" + keptBuf + "%)", LogKind::Good);
        }
    } else if (kind == 1) {   // the LODs
        const bool generatedSomething = !g.optPending.lods.empty();
        for (int k = 0; k < 4; ++k) g.opt.lods[static_cast<size_t>(k)].pending = false;   // clear even on cancel/failure
        for (size_t i = 0; i < g.optPending.lods.size(); ++i) {
            mesh::LodResult& r = g.optPending.lods[i];
            int slot = -1;
            for (int k = 0; k < 4; ++k) if (r.spec.id == "lod" + std::to_string(k + 1)) slot = k;
            if (slot < 0) continue;
            OptLod& dst = g.opt.lods[static_cast<size_t>(slot)];
            dst.failed = r.cancelled || !r.scene;
            if (r.scene) {
                dst.scene = std::move(r.scene);
                dst.triangles = r.triangles;
                dst.vertices = r.vertices;
                dst.seconds = r.seconds;
                Log(r.spec.label + ": " + Grouped(r.triangles) + " triangles (" + std::to_string(std::lround(r.spec.remaining * 100)) + "% remaining target)", LogKind::Good);
            } else {
                dst.failed = true;
            }
        }
        if (!generatedSomething && wp == 2)
            for (int k = 0; k < 4; ++k) if (g.opt.lodSelected[static_cast<size_t>(k)]) g.opt.lods[static_cast<size_t>(k)].failed = true;   // the job itself failed
        g.opt.stale = true;
    } else {
        for (int i = 0; i < 4; ++i) g.opt.lods[static_cast<size_t>(i)].pending = false;
    }
    // Re-upload only when the displayed model may have changed: the slider preview when it landed, an LOD when its own slot was regenerated.
    const bool wantsUpload = kind == 0 ? g.opt.previewLod >= 1 : g.opt.previewLod >= 2;
    if (ok && wantsUpload) OptPreviewUpload();
    InvalidatePanel();
    InvalidateView();
}

// ---- drawing ----------------------------------------------------------------

// The wide slider: a filled track from the left to the value, a disc on top, and the %-caption handled by the caller.
void DrawOptSlider(ui::Surface& s, const RECT& track, float value, bool hot) {
    const int mid = (track.top + track.bottom) / 2;
    const int left = track.left + 8, right = track.right - 8;
    s.FillRound(RECT{track.left, mid - 2, track.right, mid + 2}, 2, ui::kLine);
    const int vx = left + static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * static_cast<float>(right - left)));
    s.FillRound(RECT{left, mid - 2, vx, mid + 2}, 2, ui::kAccent);
    s.FillDisc(static_cast<float>(vx), static_cast<float>(mid), hot ? 8.0f : 7.0f, ui::kAccent);
    s.FillDisc(static_cast<float>(vx), static_cast<float>(mid), 4.0f, ui::kBg);
}

void DrawOptimizeTab(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const int partsTop = l.partsTop;
    const int maxScroll = std::max(0, OptLayoutFor(partsTop, 0).totalH - (l.partsBottom - partsTop));
    g.optScroll = std::clamp(g.optScroll, 0, maxScroll);
    const OptLayout o = OptLayoutFor(partsTop, g.optScroll);
    const size_t origTris = OptOriginalTriangles();
    const RECT listClip{0, l.partsTop, kPanel, l.partsBottom};
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip) == 0) return;
    s.Begin(sub);

    // Header: the tab's title, right-aligned status.
    s.Text(g.fontBold, ui::kText, 12, o.headerY + 2, "MESH OPTIMIZATION");
    const char* badge = g.opt.busy ? "working..." : g.opt.applied.set ? "applied to export" : "original";
    s.TextRight(g.fontSmall, g.opt.busy ? ui::kAmber : g.opt.applied.set ? ui::kGood : ui::kDim, kPanel - 12, o.headerY + 4, badge);

    // Quality preset chips.
    s.Text(g.fontSmall, ui::kSub, 12, o.presetCapY + 2, "QUALITY PRESET");
    static const char* const kPresetLabel[6] = {"Original", "High Quality", "Balanced", "Performance", "Aggressive", "Custom"};
    for (int i = 0; i < 6; ++i) {
        const RECT r = o.chips[static_cast<size_t>(i)];
        const bool sel = g.opt.preset == i;
        const bool hot = g.opt.hover == kOptPresetOriginal + i;
        s.FillRound(r, 6, sel ? RGB(34, 48, 76) : hot ? ui::kCardHi : ui::kCard);
        s.StrokeRound(r, 6, sel ? ui::kAccent : ui::kLine);
        s.TextCentered(sel ? g.fontBold : g.fontUi, sel ? ui::kAccent : ui::kText, r, kPresetLabel[i]);
    }

    // Triangle reduction: caption, the slider and the big % on the same baseline.
    s.Text(g.fontUi, ui::kText, 12, o.redCapY + 2, "Triangle reduction");
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(std::lround(std::clamp(g.opt.reduction, 0.0f, 1.0f) * 100.0f)));
    const int pctRight = kPanel - 12;
    s.TextRight(g.fontTitle, ui::kAccent, pctRight, o.redCapY + 1, pct);
    DrawOptSlider(s, o.slider, g.opt.reduction, g.opt.hover == -2);
    s.Text(g.fontSmall, ui::kDim, 12, o.capY + 2, "Original");
    s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, o.capY + 2, "Maximum simplification");

    s.Fill(RECT{12, o.div1Y, kPanel - 12, o.div1Y + 1}, ui::kLine);

    // LOD generation: one row per level; each target is configurable in 5% steps, independent of the others.
    s.Text(g.fontBold, ui::kText, 12, o.lodCapY + 1, "LOD Generation");
    s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, o.lodCapY + 3, "from the original");
    static const char* const kLodName[4] = {"LOD 1", "LOD 2", "LOD 3", "LOD 4"};
    static const char* const kLodDist[4] = {"near", "medium", "far", "very far"};
    for (int i = 0; i < 4; ++i) {
        const OptLod& lv = g.opt.lods[static_cast<size_t>(i)];
        const int y = o.lodRows[static_cast<size_t>(i)].top;
        const bool hot = g.opt.hover == kOptLodBase + i;
        if (hot) s.FillRound(RECT{8, y, kPanel - 8, y + 23}, 5, ui::kCardHi);
        CheckBox(s, 14, y + 3, g.opt.lodSelected[static_cast<size_t>(i)], ui::kAmber);
        s.Text(g.fontUi, ui::kText, 38, y + 3, kLodName[i]);
        s.Text(g.fontSmall, ui::kFaint, 84, y + 5, kLodDist[i]);
        if (lv.pending) {
            s.TextRight(g.fontSmall, ui::kAmber, kPanel - 104, y + 5, "...");
        } else if (lv.scene) {
            char line[96];
            std::snprintf(line, sizeof line, "%s (%d%%)", Grouped(lv.triangles).c_str(), static_cast<int>(std::lround(g.opt.lodTarget[i] * 100)));
            s.TextRight(g.fontSmall, ui::kGood, kPanel - 104, y + 5, line);
        } else {
            s.TextRight(g.fontSmall, lv.failed ? ui::kBad : ui::kSub, kPanel - 104, y + 5, lv.failed ? "failed" : (std::to_string(static_cast<int>(std::lround(g.opt.lodTarget[i] * 100))) + "%"));
        }
        // the target steppers (only meaningful while the row is enabled, but always shown)
        for (int side = 0; side < 2; ++side) {
            const RECT& r = side == 0 ? o.lodDown[i] : o.lodUp[i];
            const bool shot = g.opt.hover == (side == 0 ? kOptLodDown + i : kOptLodUp + i);
            s.FillRound(r, 5, shot ? ui::kLineHi : ui::kCard);
            s.StrokeRound(r, 5, ui::kLine);
            s.TextCentered(g.fontUi, shot ? ui::kAccent : ui::kSub, r, side == 0 ? "-" : "+");
        }
    }
    {
        const bool hot = g.opt.hover == kOptGenerate;
        s.FillRound(o.generate, 6, hot ? ui::kLineHi : ui::kCardHi);
        s.StrokeRound(o.generate, 6, g.opt.busy ? ui::kLine : ui::kLineHi);
        s.TextCentered(g.fontUi, g.opt.busy ? ui::kFaint : ui::kText, o.generate, g.opt.busy ? "Generating..." : "Generate LODs");
    }

    s.Fill(RECT{12, o.div2Y, kPanel - 12, o.div2Y + 1}, ui::kLine);

    // Statistics: the untouched count and where the current settings land (exact when the cache matches).
    const size_t est = OptEstimatedTriangles();
    const bool exact = !g.opt.stale && g.opt.simplifiedFor >= 0.0f && std::fabs(g.opt.simplifiedFor - g.opt.reduction) < 1e-6f;
    const int sy = o.statsY;
    auto statRow = [&](int row, const char* label, const std::string& value) {
        s.Text(g.fontSmall, ui::kSub, 12, sy + row * 15 + 2, label);
        s.TextRight(g.fontMono, ui::kText, kPanel - 12, sy + row * 15 + 2, value);
    };
    statRow(0, "Original triangles", Grouped(origTris));
    statRow(1, "Simplified triangles", exact ? Grouped(est) : std::string("~ ") + Grouped(est));
    const char* presetName = g.opt.preset >= 0 && g.opt.preset < 6 ? kPresetLabel[g.opt.preset] : "Custom";
    statRow(2, "Reduction", std::to_string(static_cast<int>(std::lround(g.opt.reduction * 100))) + "%  (" + presetName + ")");

    // Preview selector: original, the simplified slider preview, or a generated LOD.
    s.Text(g.fontSmall, ui::kSub, 12, o.previewCapY + 2, "PREVIEW");
    static const char* const kPreviewLabel[6] = {"Original", "Simplified", "LOD 1", "LOD 2", "LOD 3", "LOD 4"};
    for (int i = 0; i < 6; ++i) {
        const RECT r = o.preview[static_cast<size_t>(i)];
        const bool active = g.opt.previewLod == i;
        const bool ready = i == 0 || (i == 1 ? g.opt.simplified != nullptr : g.opt.lods[static_cast<size_t>(i - 2)].scene != nullptr);
        const bool hot = g.opt.hover == kOptPreviewBase + i;
        s.FillRound(r, 6, active ? RGB(34, 48, 76) : hot ? ui::kCardHi : ui::kCard);
        s.StrokeRound(r, 6, active ? ui::kAccent : ui::kLine);
        s.TextCentered(g.fontSmall, active ? ui::kAccent : ready ? ui::kText : ui::kFaint, r, kPreviewLabel[i]);
    }

    // Apply & reset: the export reads g.opt.applied, so this row decides what the PMX gets.
    {
        const bool hot = g.opt.hover == kOptApply;
        if (g.opt.applied.set) {
            s.FillRound(o.apply, 6, ui::kGood);
            s.TextCentered(g.fontBold, RGB(16, 26, 18), o.apply, "Applied to export");
        } else {
            s.FillRound(o.apply, 6, hot ? ui::kLineHi : ui::kAccent);
            s.TextCentered(g.fontBold, RGB(12, 20, 30), o.apply, "Apply to export");
        }
        const bool rhov = g.opt.hover == kOptReset;
        s.FillRound(o.reset, 6, rhov ? ui::kCardHi : ui::kCard);
        s.StrokeRound(o.reset, 6, ui::kLine);
        s.TextCentered(g.fontUi, ui::kText, o.reset, "Reset to original");
    }
    s.Text(g.fontSmall, ui::kFaint, 12, o.noteY + 2, "Always generated from the original; no cumulative loss.");
    s.Begin(clip);   // restore the caller's clip region (the content was drawn under the list clip)
    if (maxScroll > 0)
        ScrollBar(s, l.partsTop, l.partsBottom, std::max(1, o.totalH / 16), std::max(1, (l.partsBottom - l.partsTop) / 16), static_cast<float>(g.optScroll) / 16.0f);
    s.End();
}

// ---- hover / click routing --------------------------------------------------

void OptTabHover(int x, int y) {
    const int partsTop = Lay(ClientHeight()).partsTop;
    const OptLayout o = OptLayoutFor(partsTop, g.optScroll);
    const POINT p{x, y};
    int hover = -1;
    const RECT sliderHit = OptSliderHit(o);
    if (PtInRect(&sliderHit, p)) hover = -2;
    else if (PtInRect(&o.generate, p)) hover = kOptGenerate;
    else if (PtInRect(&o.apply, p)) hover = kOptApply;
    else if (PtInRect(&o.reset, p)) hover = kOptReset;
    else for (int i = 0; i < 6; ++i) if (PtInRect(&o.chips[static_cast<size_t>(i)], p)) { hover = kOptPresetOriginal + i; break; }
    if (hover < 0) for (int i = 0; i < 4; ++i) {
        if (PtInRect(&o.lodDown[static_cast<size_t>(i)], p)) { hover = kOptLodDown + i; break; }
        if (PtInRect(&o.lodUp[static_cast<size_t>(i)], p)) { hover = kOptLodUp + i; break; }
        if (PtInRect(&o.lodRows[static_cast<size_t>(i)], p)) { hover = kOptLodBase + i; break; }
    }
    if (hover < 0) for (int i = 0; i < 6; ++i) if (PtInRect(&o.preview[static_cast<size_t>(i)], p)) { hover = kOptPreviewBase + i; break; }
    if (hover != g.opt.hover) { g.opt.hover = hover; InvalidatePanel(); }
}

void OptTabClick(int x, int y, bool right) {
    if (right) return;   // the tab is click-driven; nothing hides behind a right-click here
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    const int partsTop = Lay(ClientHeight()).partsTop;
    const OptLayout o = OptLayoutFor(partsTop, g.optScroll);
    const POINT p{x, y};
    for (int i = 0; i < 6; ++i)
        if (PtInRect(&o.chips[static_cast<size_t>(i)], p)) {
            g.opt.preset = i;
            if (i < 5) g.opt.reduction = mesh::kSimplifyPresets[i].reduction;
            g.opt.stale = true;
            InvalidatePanel();
            if (g.opt.reduction <= 0.0f) { OptPreviewTo(0); }   // Original: the view goes straight back to the loaded card
            return;
        }
    const RECT sliderHit = OptSliderHit(o);
    if (PtInRect(&sliderHit, p)) {
        g.opt.dragging = 1;
        g.dragX = x;
        OptSliderTo(x);
        SetCapture(g.hwnd);
        return;
    }
    for (int i = 0; i < 4; ++i)
        if (PtInRect(&o.lodDown[static_cast<size_t>(i)], p)) { OptLodStep(i, false); return; }
    for (int i = 0; i < 4; ++i)
        if (PtInRect(&o.lodUp[static_cast<size_t>(i)], p)) { OptLodStep(i, true); return; }
    for (int i = 0; i < 4; ++i)
        if (PtInRect(&o.lodRows[static_cast<size_t>(i)], p)) {
            g.opt.lodSelected[static_cast<size_t>(i)] = !g.opt.lodSelected[static_cast<size_t>(i)];
            InvalidatePanel();
            return;
        }
    if (PtInRect(&o.generate, p)) { OptGenerateLods(); return; }
    for (int i = 0; i < 6; ++i)
        if (PtInRect(&o.preview[static_cast<size_t>(i)], p)) { OptPreviewTo(i); return; }
    if (PtInRect(&o.apply, p)) { OptApply(); return; }
    if (PtInRect(&o.reset, p)) { OptReset(); return; }
}

void OptSliderRelease() {
    if (!g.opt.dragging) return;
    g.opt.dragging = 0;
    ReleaseCapture();
    Log("triangle reduction set to " + std::to_string(static_cast<int>(std::lround(g.opt.reduction * 100))) + "%");
    if (g.opt.reduction <= 0.0f) OptPreviewTo(0);
    else OptGeneratePreview();   // refresh the cached preview (a no-op if one is queued)
}

void OptSliderMouseMove(int x) {
    if (!g.opt.dragging) return;
    g.dragX = x;
    OptSliderTo(x);
}
