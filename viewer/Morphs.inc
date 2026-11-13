// viewer fragment: the Morphs tab - every morph of the exported PMX, live (included in order by main.cpp)
//
// The Morphs tab lists the exact morphs the exported PMX carries, built from the scene by exporter::BuildLiveMorphs (the same code the
// export uses to pick the morphs, so names, panels and the expressions they play match the PMX that would come out of this scene).
// A slider accumulates weight * shape offset into the rest vertices before skinning (AccumulateLiveMorph evaluates the recipe against
// the *current* scene, so a re-shape keeps the preview correct mid-drag) - the same way the game applies blend shapes and MikuMikuDance
// applies PMX vertex morphs - so the movement seen here is the movement the PMX does. The outfit and nude toggles are material morphs in
// the PMX (they hide and show meshes, not move vertices); the Parts tab does that job. Bones move in the Bones tab, the DynamicBone
// physics in the Look tab (Y) - together that is every way the PMX moves.

// Which side a morph is for, if any: the game spells the left and right twins differently per mesh and per shape
// (winkl / winkr, gyul / gyur, gimoL / gimoR, sL00 / sR00, the whites cf_Ohitomi_L / _R ...). 0 = neither, 1 = left, 2 = right.
int SideOf(const std::string& name) {
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    // MMD winks: ウィンク / ウィンク２ (from まばたき, left), ｳｨﾝｸ２右 / ウィンク右 (right)
    if (has("ウィンク") || has("ウインク") || has("ｳｨﾝｸ")) return has("右") ? 2 : 1;
    // the eye meshes carry every raw morph on them; the white is cf_Ohitomi_L / _R
    if (has("cf_Ohitomi_L")) return 1;
    if (has("cf_Ohitomi_R")) return 2;
    if (has("eye_siroL.") || has("eye_siroR.")) return has("eye_siroL.") ? 1 : 2;
    if (has("sL00")) return 1;
    if (has("sR00")) return 2;
    if (has("winkl")) return 1;
    if (has("winkr")) return 2;
    if (has("gyul")) return 1;
    if (has("gyur")) return 2;
    if (has("gimoL")) return 1;
    if (has("gimoR")) return 2;
    if (has("sianL")) return 1;
    if (has("sianR")) return 2;
    if (has("oko2L")) return 1;
    if (has("oko2R")) return 2;
    return 0;
}

// The displayed name of a morph: the user's rename if any (kept in the viewer's settings), else the PMX's original.
std::string MorphDisplayName(const std::string& name) {
    const auto it = g.settings.morphAliases.find(name);
    return it != g.settings.morphAliases.end() && !it->second.empty() ? it->second : name;
}


// Per-mesh accumulation of the current weights; handed to the renderers (the GPU view folds it into the rest vertices before skinning,
// the software view adds it to its copied frame). Cheap: only the active morphs are evaluated, against the live shapes.
void RebuildMorphDelta() {
    std::shared_ptr<Loaded> L = Current();
    g.morphDelta.assign(L ? L->scene.meshes.size() : 0, {});
    if (!L) return;
    for (size_t i = 0; i < g.morphCatalog.morphs.size() && i < g.morphWeights.size(); ++i) {
        const float w = g.morphWeights[i];
        if (w == 0.0f) continue;
        exporter::AccumulateLiveMorph(L->scene, g.morphCatalog, g.morphCatalog.morphs[i], w, g.morphDelta);
    }
    if (g.gpuOk && g.gpuScene == L) g.gpu.SetMorphDelta(&g.morphDelta);   // store; the caller refreshes the skin
}

// Builds the catalog from the current scene. `keepWeights` (a re-shape: the scene is the same, the order is the same) carries the current
// weights over by index; a fresh load starts every weight at 0.
void SetMorphCatalog(bool keepWeights) {
    std::shared_ptr<Loaded> L = Current();
    if (!keepWeights) { g.morphEdit = -1; g.morphEditBuf.clear(); g.morphEditOrig.clear(); }   // a fresh character: no rename box open
    std::vector<float> old = std::move(g.morphWeights);
    g.morphCatalog = {};
    try { if (L) g.morphCatalog = exporter::BuildLiveMorphs(L->scene); }
    catch (const std::exception& e) { Log(std::string("morphs: could not build the exported PMX's morph list: ") + e.what(), LogKind::Warn); }
    g.morphWeights.assign(g.morphCatalog.morphs.size(), 0.0f);
    if (keepWeights)
        for (size_t i = 0; i < g.morphWeights.size() && i < old.size(); ++i) g.morphWeights[i] = old[i];
    RebuildMorphDelta();   // folds the kept weights into the GPU copy (usually none on load)
}

void LoadMorphCatalog() { SetMorphCatalog(false); }

// A morph slider (or the tab's reset) changed: accumulate and show at once. No re-assembly - the live shapes follow the slider for free.
void MorphSlideEdited() {
    RebuildMorphDelta();
    if (g.showExp) {   // the X / "as exported" copy is a separate baked scene: it cannot show live morphs
        g.showExp = false;
        Log("as exported: off (morphs play in the live view)");
    }
    std::shared_ptr<Loaded> L = Current();
    if (g.gpuOk && g.gpuScene == L) g.gpu.Refresh(false);   // rebuild the rest with the deltas folded in and re-skin (the pose rides along)
    g.dirty = true;   // the picture must be re-rendered (else the change is invisible until something else repaints)
    InvalidateView();
    InvalidatePanel();
}

// All weights at zero, and their number for the header status ("3 of 212 on")
size_t MorphsOn() {
    size_t on = 0;
    for (float w : g.morphWeights) if (std::fabs(w) > 1e-4f) ++on;
    return on;
}

void ResetAllMorphs() {
    std::fill(g.morphWeights.begin(), g.morphWeights.end(), 0.0f);
    MorphSlideEdited();
    Log("reset every morph (0)");
}

// ---- renaming a morph's displayed name (Morphs tab): Shift+click its row ------------------------------------------------
// The edit is modal while it is open: the keyboard goes to the box (Enter commits, Esc restores the original), a click anywhere else
// commits. The rename is display only (the PMX still carries the original - MMD motions key morphs by it) and is kept in the viewer's
// settings (%LOCALAPPDATA%\CardBridge\viewer.json, "morphAliases"), so it survives restarts. Right-click resets a renamed row.
void EndMorphEdit(bool commit);
std::string MorphEditTrim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
void BeginMorphEdit(int morphIndex) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || morphIndex < 0 || static_cast<size_t>(morphIndex) >= g.morphCatalog.morphs.size()) return;
    if (g.morphEdit == morphIndex) return;
    if (g.morphEdit >= 0) { EndMorphEdit(true); }   // another row was open: commit it first
    g.morphEdit = morphIndex;
    const std::string& name = g.morphCatalog.morphs[static_cast<size_t>(morphIndex)].name;
    g.morphEditOrig = g.settings.morphAliases.count(name) ? g.settings.morphAliases[name] : std::string();
    g.morphEditBuf = MorphDisplayName(name);
    g.sliderEdit = -1;   // one text box at a time
    InvalidatePanel();
}
void EndMorphEdit(bool commit) {
    const int morphIndex = g.morphEdit;
    if (morphIndex < 0) return;
    std::shared_ptr<Loaded> L = Current();
    const std::string name = (L && morphIndex < static_cast<int>(g.morphCatalog.morphs.size())) ? g.morphCatalog.morphs[static_cast<size_t>(morphIndex)].name : std::string();
    if (commit && !name.empty()) {
        const std::string typed = MorphEditTrim(g.morphEditBuf);
        if (typed.empty() || typed == name) {
            if (g.settings.morphAliases.erase(name)) Log("rename cleared for " + name);
        } else {
            g.settings.morphAliases[name] = typed;
            Log("renamed \"" + name + "\" to \"" + typed + "\" (display only; the PMX keeps the original name)", LogKind::Good);
        }
        SaveSettings(g.settings);
    }
    g.morphEdit = -1;
    g.morphEditBuf.clear();
    g.morphEditOrig.clear();
    InvalidatePanel();
}
bool MorphEditKey(UINT wp) {
    // While the box is open the keyboard belongs to it: every key is taken, handled or swallowed.
    if (wp == VK_ESCAPE) { EndMorphEdit(false); return true; }
    if (wp == VK_RETURN) { EndMorphEdit(true); return true; }
    if (wp == VK_BACK) { if (!g.morphEditBuf.empty()) g.morphEditBuf.pop_back(); InvalidatePanel(); return true; }
    if (wp == VK_DELETE) { g.morphEditBuf.clear(); InvalidatePanel(); return true; }
    return true;   // a stray key: swallowed, the edit stays open
}
bool MorphEditChar(wchar_t ch) {
    if (g.morphEdit < 0) return false;
    if (ch < 32 || ch >= 127) return true;   // control characters: swallowed, the box keeps the keyboard
    if (g.morphEditBuf.size() < 40) { g.morphEditBuf.push_back(static_cast<char>(ch)); InvalidatePanel(); }
    return true;
}
