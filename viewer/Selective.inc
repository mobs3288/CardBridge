// viewer fragment: the Selective Import window (included in order by main.cpp, after Window.inc)
//
// The clothing import's second mode. The one-click Replace All import is untouched; this window is
// the alternative: three actual 3D views of the character (what it is now, what the imported source
// looks like, what it would look like after the import), a per-component picker, and a transactional
// Apply that commits the in-memory merge through the same pin the Replace All path uses (clothingMode
// 2 + the merge in StartLoad). Cancel and the close button leave the loaded character untouched.
//
// The previews are assembled on worker threads from a *copy* of the loaded card; the loaded card on
// screen is never modified by this window. The proposed scene is cached per decision set and rebuilt
// (debounced) as the user flips components; committing re-runs the same merge on the real card, so
// what the preview shows is what the character gets.
//
// View 0 (current) renders through the main window's Direct3D view (g.gpu) with the live skin and
// effective hidden parts — the very same picture the viewer shows. Views 1 (imported source) and
// 2 (proposed) are drawn by their own Direct3D views (or the software renderer when Direct3D 11 is
// unavailable), skinned with a shared A-pose. Picking is ray-to-triangle on the CPU against the
// skinned clothing meshes only (they are small), so hovering lights the same component in all three
// views and in the list.

constexpr wchar_t kSelClassName[] = L"CardBridgeSelectiveImport";
constexpr UINT_PTR kSelBuildTimer = 3;   // the 250 ms debounce that rebuilds the proposed scene
constexpr UINT_PTR kSelResizeTimer = 4;  // 250 ms without a resize ? re-render the three viewports at the new size

// Forward declarations (the input handlers reach these before their definitions appear):
card::SelectiveDecisions SelDecisionsNow();
card::SelectivePlan SelPlan();
void SelRecomputeValidation();
void SelResetDecisions();
void SelSetAllDecisions(card::SelectiveAction action);
void SelApply();
void SelCycleCoordinate(int delta);
void SelPaint(HWND hwnd);

// ==================================================================== small helpers ============================

// The action colours, shared by the dots, the legend and the 3D highlights in every view.
COLORREF SelActionColor(card::SelectiveAction a) {
    switch (a) {
    case card::SelectiveAction::Add:          return ui::kGood;
    case card::SelectiveAction::Replace:      return ui::kAmber;
    case card::SelectiveAction::KeepExisting: return ui::kAccent;
    case card::SelectiveAction::Skip:         return ui::kDim;
    case card::SelectiveAction::AddLayer:     return ui::kAccentHi;
    }
    return ui::kDim;
}
static COLORREF SelActionText(card::SelectiveAction a) {
    switch (a) {
    case card::SelectiveAction::Add:          return ui::kOnAccent;
    case card::SelectiveAction::Replace:      return ui::kOnAccent;
    case card::SelectiveAction::KeepExisting: return ui::kOnAccent;
    case card::SelectiveAction::Skip:         return ui::kOnAccent;
    case card::SelectiveAction::AddLayer:     return ui::kOnAccent;
    }
    return ui::kOnAccent;
}
static exporter::HighlightPart SelHighlightFor(card::SelectiveAction a, int part) {
    const COLORREF c = SelActionColor(a);
    exporter::HighlightPart h;
    h.part = part;
    h.tint[0] = GetRValue(c) / 255.0f; h.tint[1] = GetGValue(c) / 255.0f; h.tint[2] = GetBValue(c) / 255.0f; h.tint[3] = 0.42f;
    h.ring[0] = GetRValue(c) / 255.0f; h.ring[1] = GetGValue(c) / 255.0f; h.ring[2] = GetBValue(c) / 255.0f; h.ring[3] = 0.95f;
    h.ringWidth = 3.0f;
    return h;
}
static exporter::HighlightPart SelHoverHighlight(int part) {
    exporter::HighlightPart h;
    h.part = part;
    h.tint[0] = 1.0f; h.tint[1] = 1.0f; h.tint[2] = 1.0f; h.tint[3] = 0.20f;
    h.ring[0] = 1.0f; h.ring[1] = 1.0f; h.ring[2] = 1.0f; h.ring[3] = 0.90f;
    h.ringWidth = 3.2f;
    return h;
}

// Which outfit's parts this view's scene carries (view 1 for a coordinate source shows the *source's* coordinate).
int SelViewOutfit(int view) {
    if (view == 1 && g.sel.srcIsCoordinate) return g.sel.srcOutfit;
    return g.sel.dstOutfit;
}

// The scene part indices a component is responsible for (clothes: the slot and the top's sub-parts; accessories: the merged slot).
std::vector<int> SelComponentParts(const assemble::Scene& scene, int outfit, const card::SelectiveComponent& comp,
                                   const std::map<std::string, int>& placedAcc) {
    std::vector<int> out;
    const std::string prefix = "outfit" + std::to_string(outfit) + ".";
    std::string base = prefix + comp.slotName;   // "top" ... or "acc03"
    if (comp.kind == 1) {
        const auto it = placedAcc.find(comp.key);
        if (it != placedAcc.end()) {
            char num[8];
            std::snprintf(num, sizeof num, "%02d", it->second);   // an Add can relocate around the character's own accessories
            base = prefix + "acc" + num;
        }
    }
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const std::string& id = scene.parts[i].id;
        if (id == base) { out.push_back(static_cast<int>(i)); continue; }
        if (comp.kind == 0 && comp.slot == 0 && id.rfind(base + ".sub_", 0) == 0) out.push_back(static_cast<int>(i));   // the top's jacket/sailor sub-parts
        if (comp.kind == 0) {   // the layer grafted on top: "outfitN.<slot>.l<k>" (and its jacket/sailor sub-parts)
            const size_t llen = base.size();
            if (id.size() > llen + 3 && id.compare(0, llen, base) == 0 && id[llen] == '.' && id[llen + 1] == 'l')
                out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

// Recompute the per-view component->parts mapping after a scene lands.
void SelRemapComponents(int view) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    SelView& v = g.sel.views[view];
    if (!v.scene) return;
    const int outfit = SelViewOutfit(view);
    v.compParts.clear();
    v.compParts.reserve(g.sel.comps.size());
    for (const auto& sc : g.sel.comps) v.compParts.push_back(SelComponentParts(*v.scene, outfit, sc.comp, v.placedAcc));
}

// The highlight list for one view: every component the user can see, coloured by the action chosen
// for it. View 0 shows only components with an existing counterpart (the target wears them); views 1
// and 2 colour every visible piece by its action; the proposed view colours retained (keep/skip)
// parts with KeepExisting blue.
std::vector<exporter::HighlightPart> SelHighlightsFor(int view) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // the render worker calls this; the map and the rows are UI state
    std::vector<exporter::HighlightPart> hs;
    const SelView& v = g.sel.views[view];
    if (!v.scene) return hs;
    const int hov = g.sel.hoverComp >= 0 ? g.sel.hoverComp : g.sel.selComp;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        const SelectiveComp& sc = g.sel.comps[i];
        const auto it = g.sel.decisions.find(sc.comp.key);
        const card::SelectiveAction eff = it == g.sel.decisions.end() ? card::SuggestedAction(sc.comp) : it->second;
        const bool hover = static_cast<int>(i) == hov;
        if (!hover && view == 0 && !sc.comp.hasExisting) continue;
        const card::SelectiveAction colour = (view == 2 && eff == card::SelectiveAction::Skip) ? card::SelectiveAction::KeepExisting : eff;
        if (i < v.compParts.size())
            for (int pi : v.compParts[i])
                hs.push_back(hover ? SelHoverHighlight(pi) : SelHighlightFor(colour, pi));
    }
    return hs;
}

// ==================================================================== layout ============================

struct SelLayout {
    RECT title, subtitle;
    RECT srcPrev, srcNext, srcLabel;   // the source-coordinate cycler (Coordinate sources only)
    RECT linkBtn, aposeBtn;
    RECT views[3];
    RECT viewTitle[3];                 // the label chip over each viewport
    RECT legend;                       // the whole legend row
    RECT listCard;                     // the component list card
    RECT listClip;                     // where the rows are drawn (clipped)
    int listTop = 0;                   // the list's content top (after the heading)
    RECT sumCard;                      // the right column card
    RECT planChips;                    // the counts line
    RECT allReplaceBtn, allSkipBtn, resetBtn;
    RECT cancelBtn, applyBtn;
    RECT footer;
    RECT scrollTrack;                  // the list's scrollbar thumb area
    // the garment layers of the pinned import(s), drawn above the components list while any exist
    RECT layersCard;                   // where the layers strip is drawn
    std::vector<RECT> layerRows;       // one row per pinned layer (name + remove button)
    std::vector<RECT> layerRemove;     // each row's small "x" button
};

SelLayout SelLay(int W, int H) {
    SelLayout l;
    const int M = 12;
    const int headerH = 64;
    // Header (title, subtitle, controls right)
    l.title = RECT{M, 6, W - 320, 30};
    l.subtitle = RECT{M, 30, W - 320, 48};
    l.srcPrev = RECT{M, 6, 0, 0}; l.srcNext = RECT{M, 6, 0, 0}; l.srcLabel = RECT{M, 6, 0, 0};
    if (g.sel.srcIsCoordinate) {
        l.srcPrev = RECT{M, 6, M + 26, 30}; l.srcLabel = RECT{M + 30, 6, M + 210, 30}; l.srcNext = RECT{M + 214, 6, M + 240, 30};
        l.title = RECT{M, 0, 0, 0};   // the cycler replaces the plain title line
        l.subtitle = RECT{M, 30, W - 320, 48};
    }
    l.linkBtn = RECT{W - 236, 8, W - 128, 34};
    l.aposeBtn = RECT{W - 120, 8, W - 12, 34};
    // Viewports: three square-ish panels with a caption bar on top of each.
    const int gap = 10;
    const int viewH = std::clamp(static_cast<int>(H * 0.47f), 210, 520);
    int vw = (W - 2 * M - 2 * gap) / 3;
    vw = std::min(vw, static_cast<int>(viewH * 1.55f));   // don't stretch the pictures at very wide windows
    const int viewsW = 3 * vw + 2 * gap;
    const int viewsX = std::max(M, (W - viewsW) / 2);
    for (int i = 0; i < 3; ++i) {
        l.views[i] = RECT{viewsX + i * (vw + gap), headerH, viewsX + i * (vw + gap) + vw, headerH + viewH};
        l.viewTitle[i] = RECT{l.views[i].left + 8, l.views[i].top + 6, l.views[i].left + 8 + 200, l.views[i].top + 26};
    }
    const int legendTop = headerH + viewH + 8;
    l.legend = RECT{M, legendTop, W - M, legendTop + 26};
    const int bodyTop = legendTop + 26 + 8;
    const int footerH = 26;
    const int bodyH = std::max(120, H - footerH - M - bodyTop);
    l.footer = RECT{M, H - footerH, W - M, H};
    const int listW = std::clamp(static_cast<int>(W * 0.46f), 440, 760);
    const int pinnedLayers = [&] {
        int n = 0;
        for (const ClothingPin& pin : g.clothingPins) if (pin.mode == 3) ++n;
        return n;
    }();
    const int layersH = pinnedLayers > 0 ? 22 + pinnedLayers * 38 + 6 : 0;   // caption + one 38 px row per pinned layer
    l.layersCard = RECT{M, bodyTop, 0, 0};
    int layerRow = 0;
    l.layerRows.clear();
    l.layerRemove.clear();
    if (pinnedLayers > 0) {
        l.layersCard = RECT{M, bodyTop, M + listW, bodyTop + layersH};
        for (const ClothingPin& pin : g.clothingPins) {
            if (pin.mode != 3) continue;
            const int y = l.layersCard.top + 22 + layerRow * 38;
            l.layerRows.push_back(RECT{l.layersCard.left + 8, y, l.layersCard.right - 34, y + 32});
            l.layerRemove.push_back(RECT{l.layersCard.right - 28, y + 6, l.layersCard.right - 4, y + 26});
            ++layerRow;
        }
    }
    const int listTop2 = bodyTop + layersH + (pinnedLayers > 0 ? 8 : 0);
    l.listCard = RECT{M, listTop2, M + listW, H - footerH - M + 1};
    l.listClip = RECT{l.listCard.left, l.listCard.top, l.listCard.right, l.listCard.bottom};
    l.listTop = l.listCard.top + 34;   // under the "COMPONENTS" heading
    l.scrollTrack = RECT{l.listCard.right - 10, l.listTop, l.listCard.right - 2, l.listCard.bottom - 6};
    l.sumCard = RECT{l.listCard.right + 10, bodyTop, W - M, H - footerH - M + 1};
    l.planChips = RECT{l.sumCard.left + 12, l.sumCard.top + 38, l.sumCard.right - 12, l.sumCard.top + 62};
    // Right column buttons, stacked at the bottom of the card
    const int bw = l.sumCard.right - l.sumCard.left - 24;
    const int cancelW = bw / 2 - 4;
    l.allReplaceBtn = RECT{l.sumCard.left + 12, l.sumCard.bottom - 128, l.sumCard.left + 12 + (bw - 8) / 2, l.sumCard.bottom - 98};
    l.allSkipBtn = RECT{l.allReplaceBtn.right + 8, l.sumCard.bottom - 128, l.sumCard.right - 12, l.sumCard.bottom - 98};
    l.resetBtn = RECT{l.sumCard.left + 12, l.sumCard.bottom - 90, l.sumCard.right - 12, l.sumCard.bottom - 60};
    l.cancelBtn = RECT{l.sumCard.left + 12, l.sumCard.bottom - 52, l.sumCard.left + 12 + cancelW, l.sumCard.bottom - 14};
    l.applyBtn = RECT{l.cancelBtn.right + 8, l.sumCard.bottom - 52, l.sumCard.right - 12, l.sumCard.bottom - 14};
    return l;
}

// Component rows: the geometry (also used by the hit test), with the scroll offset applied.
struct SelRowRect { int top, bottom, kind, comp; };   // kind: 0 = group header, 1 = component row

// Rows are laid out top-down inside the clipped list area, skipping rows that are scrolled out.
// Returns the rects for the visible rows in order (comp = -1 for a group header).
std::vector<SelRowRect> SelVisibleRows(const SelLayout& l, int scrollPx) {
    std::vector<SelRowRect> out;
    int y = l.listTop - scrollPx;
    const int bottom = l.listClip.bottom;
    int compIdx = 0;
    std::string curCat;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        const auto& comp = g.sel.comps[i].comp;
        if (comp.category != curCat) {   // a group header when the category changes
            if (y + 22 > l.listClip.top && y < bottom) out.push_back({y, y + 22, 0, -1});
            curCat = comp.category;
            y += 22;
        }
        const int rowH = 40;
        if (y + rowH > l.listClip.top && y < bottom) out.push_back({y, y + rowH, 1, compIdx});
        ++compIdx;
        y += rowH;
    }
    return out;
}

int SelListContentHeight() {
    int h = 0;
    std::string curCat;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        const auto& comp = g.sel.comps[i].comp;
        if (comp.category != curCat) { curCat = comp.category; h += 22; }
        h += 40;
    }
    return h;
}

int SelListMaxScroll(const SelLayout& l) {
    return std::max(0, SelListContentHeight() - (static_cast<int>(l.listClip.bottom) - l.listTop));
}

// The action buttons of one row, right-aligned; shared by the drawing and the hit test. Clothes rows carry a
// fifth button — "Add" the garment into an extra "layer" on top of the outfit, besides whatever the slot already holds.
struct SelActionRects { RECT addLayer, add, replace, keep, skip; };
SelActionRects SelActionButtonRects(const SelLayout& l, const RECT& row, bool withAddLayer = false) {
    const int w = withAddLayer ? 40 : 44, h = 22, gap = 4;
    const int n = withAddLayer ? 5 : 4;
    int x = l.listClip.right - 8 - (n * w + (n - 1) * gap);
    const int y = row.top + (row.bottom - row.top - h) / 2;
    SelActionRects r;
    if (withAddLayer) { r.addLayer = {x, y, x + w, y + h}; x += w + gap; } else r.addLayer = {};
    r.add = {x, y, x + w, y + h}; x += w + gap;
    r.replace = {x, y, x + w, y + h}; x += w + gap;
    r.keep = {x, y, x + w, y + h}; x += w + gap;
    r.skip = {x, y, x + w, y + h};
    return r;
}



// forward declarations for the proposal cache helpers (defined with the rest of the cache code below)
static std::string SelEffectiveKey(const std::map<std::string, card::SelectiveAction>& d,
                                   const std::string& cardPath, const std::string& src,
                                   int dstOutfit, int srcOutfit, bool srcCoord);
static void SelProposalCacheClear();
static void SelProposalCachePut(const std::string& k, std::shared_ptr<assemble::Scene> scene);
static void SelImportedCachePut(const std::string& k, std::shared_ptr<assemble::Scene> scene);

// ==================================================================== building & uploads ========================

// The garment layers the user picked (AddLayer on clothes components), in slot order with stable layer indices.
// Both the preview assembly and the pinned-import reload build the layers from the same list, so preview == apply.
std::vector<assemble::AssembleLayerSpec> SelLayersFromDecisions(const std::map<std::string, card::SelectiveAction>& decisions) {
    std::vector<const card::SelectiveComponent*> picked;
    for (const SelectiveComp& sc : g.sel.comps) {
        const auto it = decisions.find(sc.comp.key);
        const card::SelectiveAction a = it == decisions.end() ? card::SuggestedAction(sc.comp) : it->second;
        if (a == card::SelectiveAction::AddLayer && sc.comp.kind == 0 && sc.comp.slot >= 0 && sc.comp.slot < 9)
            picked.push_back(&sc.comp);
    }
    std::stable_sort(picked.begin(), picked.end(), [](const card::SelectiveComponent* a, const card::SelectiveComponent* b) { return a->slot < b->slot; });
    std::vector<assemble::AssembleLayerSpec> out;
    int index = 1;
    const std::string base = g.sel.srcName.empty() ? fileio::FileName(g.sel.srcPath) : g.sel.srcName;
    for (const card::SelectiveComponent* comp : picked) {
        assemble::AssembleLayerSpec spec;
        spec.path = g.sel.srcPath;
        spec.srcOutfit = g.sel.srcIsCoordinate ? g.sel.srcOutfit : -1;
        spec.dstOutfit = g.sel.dstOutfit;
        spec.slots = {comp->slot};
        spec.name = base + " (layer " + comp->slotName + ")";
        spec.layerIndex = index++;
        out.push_back(std::move(spec));
    }
    return out;
}

// ==================================================================== progress reporting =====
// Assemble reports determinate progress at sub-item stage granularity (items, masks, create
// materials, finish). SelSetProgress is cheap — atomic stores plus a throttled WM_SEL_FRAME
// post so the view's placeholder card repaints at ~7 Hz; nothing here blocks the assembly.
static void SelSetProgress(int view, double frac, const std::string& stage) {
    SelView& v = g.sel.views[view];
    v.progFrac.store(frac >= 0.0 ? frac : -1.0, std::memory_order_relaxed);
    ++v.progTicks;
    if (!stage.empty()) { std::lock_guard<std::mutex> lk(v.progMtx); v.progStage = stage; }
    const uint64_t now = GetTickCount64();
    uint64_t last = v.progPostedMs.load(std::memory_order_relaxed);
    if (now - last >= 150 || frac >= 1.0) {
        if (v.progPostedMs.compare_exchange_strong(last, now, std::memory_order_relaxed))
            if (HWND h = g.sel.hwnd) PostMessageW(h, WM_SEL_FRAME, 0, 0);
    }
}
static void SelProgressReset(int view, const std::string& stage) {
    SelView& v = g.sel.views[view];
    v.progFrac.store(-1.0, std::memory_order_relaxed);
    v.progTicks.store(0, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(v.progMtx); v.progStage = stage; }
}
static void SelHookAssemblyProgress(assemble::AssembleOptions& ao, int view) {
    ao.progress = [view](double frac, const std::string& stage) { SelSetProgress(view, frac, stage); };
}

// The assembly options for a preview scene. `useSource` gives the *source character* its own card's
// proportions; everything else mirrors what the on-screen character gets (so the proposal matches
// what the user would see after the import).
assemble::AssembleOptions SelAssembleOptions(int outfit, bool useSource) {
    assemble::AssembleOptions ao;
    ao.outfit = outfit;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (!useSource) {
            ao.bodyShape = g.tw.body;
            ao.faceShape = g.tw.face;
            ao.nippleInside = g.tw.nippleInside;
            ao.nippleOffset = g.tw.nippleMm / 1000.0f;
            ao.nippleColour = NippleTintOption();
            ao.skinShade = g.tw.skinShade;
            ao.skinShadeUnder = g.tw.skinShadeUnder;
            ao.skinShadeTop = g.tw.skinShadeTop;
            ao.skinShadeWidth = g.tw.skinShadeWidth;
            ao.skinAo = g.tw.skinAo;
            ao.autoCloth = g.autoCloth;
            if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
            ao.proportionLayer = ProportionLayerNow();
        }
    }
    ao.log = [](const std::string&) {};
    return ao;
}

// True when `id` is a piece the chosen component (re)creates in the target outfit. Clothes Add/Replace
// overwrite the slot (its base part and the top's sub-parts). An accessory Replace overwrites its slot;
// Add only does when the card's slot was empty — otherwise the merge relocates the new piece and the
// character's old, deleted accessory stays out.
static bool SelImportCoversPart(const std::string& id, const std::string& prefix, const SelectiveComp& sc,
                                card::SelectiveAction a) {
    if (a != card::SelectiveAction::Replace && a != card::SelectiveAction::Add) return false;
    const std::string base = prefix + sc.comp.slotName;
    if (sc.comp.kind == 0) return id == base || id.rfind(base + ".sub_", 0) == 0;
    return id == base && (a == card::SelectiveAction::Replace || !sc.comp.hasExisting);
}

// The hidden set the proposed view renders with. A viewer delete survives a same-card reload, so the
// proposed picture must hide those parts too; the exception is a slot the import is about to (re)create,
// whose old delete must not hide the incoming piece. Caller holds g.mu.
static std::vector<char> SelProposalHidden(const assemble::Scene& scene, const std::set<std::string>& deleted) {
    std::vector<char> hidden(scene.parts.size(), 0);
    const std::string prefix = "outfit" + std::to_string(g.sel.dstOutfit) + ".";
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const std::string& id = scene.parts[i].id;
        if (!deleted.count(id)) continue;
        bool restored = false;
        for (const SelectiveComp& sc : g.sel.comps) {
            const auto it = g.sel.decisions.find(sc.comp.key);
            const card::SelectiveAction a = it == g.sel.decisions.end() ? card::SuggestedAction(sc.comp) : it->second;
            if (SelImportCoversPart(id, prefix, sc, a)) { restored = true; break; }
        }
        hidden[i] = restored ? 0 : 1;
    }
    return exporter::EffectiveHiddenParts(scene, hidden);
}

// Uploads the scene into a preview view's Direct3D copy (or marks the software fallback ready).
// Called on a worker thread after the scene is built. Locks never nest: g.mu guards the state reads/writes,
// selGpuMu is a leaf around the device calls only, so the render worker and the paint path stay lock-brief.
void SelUploadScene(int view, int session) {
    SelView& v = g.sel.views[view];
    SelSetProgress(view, -1.0, "uploading Direct3D picture ...");
    std::shared_ptr<const assemble::Scene> scene;
    std::shared_ptr<exporter::GpuView> dev;
    exporter::GpuOptions go;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.sel.session != session) return;   // the window was closed or reopened; do not touch the new state
        if (!v.gpu) v.gpu = std::make_shared<exporter::GpuView>();   // the cleaner may have taken the device; make a fresh one
        dev = v.gpu;   // one local copy for the whole upload (the cleaner never touches a local copy)
        scene = v.scene;
        go.preference = g.settings.gpu.empty() ? "auto" : g.settings.gpu;
        go.skin = g.settings.skin.empty() ? "gpu" : g.settings.skin;
    }
    if (!scene) { std::lock_guard<std::recursive_mutex> lk(g.mu); v.state = 3; return; }
    v.gpuOk = false;
    {
        std::lock_guard<std::mutex> gpuLk(g.sel.selGpuMu);
        if (dev->Init(go)) {
            std::string why;
            if (dev->Load(*scene, &why)) v.gpuOk = true;
            else Log("selective import preview: GPU upload failed (" + why + ") - using the software renderer", LogKind::Warn);
        } else {
            Log("selective import preview: Direct3D 11 unavailable - the preview uses the software renderer", LogKind::Warn);
        }
    }
    std::vector<char> hidden0;
    if (view != 0) {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (view == 2) hidden0 = SelProposalHidden(*scene, g.sel.deletedParts);
        else hidden0 = exporter::EffectiveHiddenParts(*scene, std::vector<char>(scene->parts.size(), 0));
    }
    SelRemapComponents(view);   // takes g.mu itself
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.sel.session == session) {
            if (view != 0) v.hidden = hidden0;   // view 0 keeps the main window's hidden set (set at open)
            v.state = 2;
            v.error.clear();
            v.dirty = true;   // the freshly assembled picture needs one full render
            if (view == 2) g.sel.buildingProposal = false;   // the Apply gate reads this; it must not lag the upload
        }
    }
}

// Builds view 1 — the imported source: the source *character* for a Coordinate PNG, or the outfit worn
// on this character for a plain outfit file (what Replace All would give; the ceiling of the import).
void SelBuildImported() {
    const std::string path = g.sel.srcPath;
    const bool srcCoord = g.sel.srcIsCoordinate;
    const int srcOutfit = g.sel.srcOutfit;
    const int dstOutfit = g.sel.dstOutfit;
    const int session = g.sel.session;
    SelView& v = g.sel.views[1];
    // identical inputs ? identical result; cache key doesn't depend on decisions at all
    const std::string importedKey = std::string(srcCoord ? "1c|" : "1|") + g.cardPath + '>' + path + '|' + std::to_string(dstOutfit) + '|' + std::to_string(srcOutfit);
    SelProgressReset(1, srcCoord ? "assembling the source character ..." : "assembling the merged outfit ...");
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (auto hit = g.sel.importedCache.find(importedKey); hit != g.sel.importedCache.end()) {
            v.scene = hit->second;
            v.placedAcc.clear();
            v.state = 1;   // upload sets it to ready
            v.error.clear();
            SelUploadScene(1, session);
            if (g.sel.hwnd) PostMessageW(g.sel.hwnd, WM_SEL_READY, 0, static_cast<LPARAM>(session));
            return;
        }
    }
    v.state = 1;
    v.error.clear();
    std::thread([path, srcCoord, srcOutfit, dstOutfit, session, importedKey] {
        std::shared_ptr<Loaded> L;
        std::shared_ptr<resolve::Resolver> resolver;
        { std::lock_guard<std::recursive_mutex> lk(g.mu); L = g.loaded; resolver = g.resolver; }
        std::shared_ptr<assemble::Scene> sc;
        card::ApplyClothingReport applyRep;   // where the accessories landed (for the 3D mapping)
        try {
            if (!resolver) throw std::runtime_error("the asset library is not set");
            if (srcCoord) {
                card::Card src = card::LoadCard(path);
                if (!src.supported) throw std::runtime_error(src.supportNote);
                const int n = srcOutfit >= 0 && static_cast<size_t>(srcOutfit) < src.outfits.size() ? srcOutfit : 0;
                assemble::AssembleOptions aoi = SelAssembleOptions(n, true);
                SelHookAssemblyProgress(aoi, 1);
                sc = std::make_shared<assemble::Scene>(assemble::Assemble(src, *resolver, aoi));
            } else {
                if (!L) throw std::runtime_error("the loaded character is gone");
                card::Card copy = L->card;
                card::ApplyClothingOptions opt;
                opt.outfit = dstOutfit;
                const card::OutfitSourceReport rep = card::ApplyOutfitSource(copy, path, opt);   // whole-outfit merge, in memory only
                applyRep = rep.apply;
                assemble::AssembleOptions aoi = SelAssembleOptions(dstOutfit, false);
                SelHookAssemblyProgress(aoi, 1);
                sc = std::make_shared<assemble::Scene>(assemble::Assemble(copy, *resolver, aoi));
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.sel.session == session) {
                g.sel.views[1].error = e.what();
                g.sel.views[1].state = 3;
            }
            if (HWND h = g.sel.hwnd) PostMessageW(h, WM_SEL_READY, 0, session);
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.sel.session != session) return;   // the window was closed and reopened; the preview is discarded
            g.sel.views[1].scene = sc;
            g.sel.views[1].placedAcc = std::move(applyRep.placedAccessorySlots);
        }
        SelImportedCachePut(importedKey, sc);
        SelUploadScene(1, session);
        if (HWND h = g.sel.hwnd) PostMessageW(h, WM_SEL_READY, 0, session);
    }).detach();
}

// Builds view 2 — the proposed character: the loaded card with only the user's chosen components merged in.
void SelBuildProposal() {
    const std::string path = g.sel.srcPath;
    const bool srcCoord = g.sel.srcIsCoordinate;
    const int dstOutfit = g.sel.dstOutfit;
    const int srcOutfit = g.sel.srcOutfit;
    const card::SelectiveDecisions decisions{g.sel.decisions};
    const std::string sessionKey = SelEffectiveKey(decisions.actions, g.cardPath, path, dstOutfit, srcOutfit, srcCoord);
    const int session = g.sel.session;
    SelProgressReset(2, "assembling the proposed look ...");
    SelView& v = g.sel.views[2];
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (auto hit = g.sel.proposalCache.find(sessionKey); hit != g.sel.proposalCache.end()) {
            ++g.sel.proposalGen;   // any previously queued worker for a different key went stale... (it cannot have finished; discard its result)
            v.scene = hit->second;
            v.placedAcc.clear();
            v.state = 1;           // upload sets it to ready
            v.error.clear();
            SelUploadScene(2, session);
            if (g.sel.hwnd) PostMessageW(g.sel.hwnd, WM_SEL_READY, 1, static_cast<LPARAM>(session));
            return;
        }
    }
    const int gen = ++g.sel.proposalGen;
    v.state = 1;
    v.error.clear();
    std::thread([path, dstOutfit, srcOutfit, gen, session, decisions, sessionKey] {
        std::shared_ptr<Loaded> L;
        std::shared_ptr<resolve::Resolver> resolver;
        { std::lock_guard<std::recursive_mutex> lk(g.mu); L = g.loaded; resolver = g.resolver; }
        std::shared_ptr<assemble::Scene> sc;
        card::ApplyClothingReport applyRep;
        try {
            if (!L) throw std::runtime_error("the loaded character is gone");
            if (!resolver) throw std::runtime_error("the asset library is not set");
            card::Card copy = L->card;   // the merge runs on a copy; the loaded character never changes here
            card::ApplyClothingOptions opt;
            opt.outfit = dstOutfit;
            opt.srcOutfit = srcOutfit;
            // Layers (AddLayer) never reach the merge — the merge treats them as no-ops — but they must still be
            // part of the proposed scene, so the preview matches the applied result: assemble with the layers attached.
            const card::SelectiveOutfitReport rep = card::ApplySelectiveOutfitSource(copy, path, opt, decisions);
            applyRep = rep.apply;
            assemble::AssembleOptions ao = SelAssembleOptions(dstOutfit, false);
            ao.layers = SelLayersFromDecisions(decisions.actions);
            SelHookAssemblyProgress(ao, 2);
            sc = std::make_shared<assemble::Scene>(assemble::Assemble(copy, *resolver, ao));
        } catch (const std::exception& e) {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.sel.session == session && g.sel.proposalGen == gen) {
                g.sel.views[2].error = e.what();
                g.sel.views[2].state = 3;
                g.sel.buildingProposal = false;
            }
            if (HWND h = g.sel.hwnd) PostMessageW(h, WM_SEL_READY, 1, session);
            return;
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.sel.session != session || g.sel.proposalGen != gen) { sc.reset(); return; }   // stale: discarded
            g.sel.views[2].scene = sc;
            g.sel.views[2].placedAcc = std::move(applyRep.placedAccessorySlots);
        }
        SelProposalCachePut(sessionKey, sc);
        SelUploadScene(2, session);
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.sel.session == session) g.sel.buildingProposal = false;
        }
        if (HWND h = g.sel.hwnd) PostMessageW(h, WM_SEL_READY, 1, session);
    }).detach();
}

// ==================================================================== proposal caching =====
// The proposed scene is cached per effective decision set, so revisiting an earlier choice
// (e.g. flipping a slot back to a state the user already committed to) is instant instead of
// another full assembly. Keep/Skip merge nothing; AddLayer grafts a new piece. Clothes Add and
// Replace both write the slot, so they share a key; an accessory Add relocates around the
// character's own accessories and must be a distinct key from Replace (different geometry).
static std::string SelEffectiveKey(const std::map<std::string, card::SelectiveAction>& d,
                                   const std::string& cardPath, const std::string& src,
                                   int dstOutfit, int srcOutfit, bool srcCoord) {
    std::string key = cardPath;
    key += '>' + src;
    key += '|' + std::to_string(dstOutfit);
    key += "|c" + std::to_string(srcCoord ? srcOutfit : -1);
    for (const auto& [k, a] : d) {
        char c = 0;
        if (a == card::SelectiveAction::Replace) c = 'R';
        else if (a == card::SelectiveAction::Add) {
            bool accessory = false;
            for (const SelectiveComp& sc : g.sel.comps) if (sc.comp.key == k) { accessory = sc.comp.kind == 1; break; }
            c = accessory ? 'A' : 'R';
        } else if (a == card::SelectiveAction::AddLayer) c = 'L';
        if (!c) continue;
        key += '|';
        key += k;
        key += ':';
        key += c;
    }
    return key;
}
static void SelProposalCacheClear() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    g.sel.proposalCache.clear();
    g.sel.proposalCacheLru.clear();
    g.sel.importedCache.clear();
    g.sel.importedCacheLru.clear();
}
static void SelProposalCachePut(const std::string& k, std::shared_ptr<assemble::Scene> scene) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (g.sel.proposalCache.count(k)) return;   // already there; a fresher duplicate upload is harmless
    g.sel.proposalCache[k] = std::move(scene);
    g.sel.proposalCacheLru.push_back(k);
    while (g.sel.proposalCacheLru.size() > 4) {
        g.sel.proposalCache.erase(g.sel.proposalCacheLru.front());
        g.sel.proposalCacheLru.erase(g.sel.proposalCacheLru.begin());
    }
}
static void SelImportedCachePut(const std::string& k, std::shared_ptr<assemble::Scene> scene) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (g.sel.importedCache.count(k)) return;
    g.sel.importedCache[k] = std::move(scene);
    g.sel.importedCacheLru.push_back(k);
    while (g.sel.importedCacheLru.size() > 2) {   // the imported scene is ~30 MB; keep only a couple of recent sources
        g.sel.importedCache.erase(g.sel.importedCacheLru.front());
        g.sel.importedCacheLru.erase(g.sel.importedCacheLru.begin());
    }
}

// The 250 ms debounce between a decision change and the rebuild — skipped entirely when the effective
// inputs are unchanged (every decision becomes a no-op or lands in a slot whose effect matches).
void SelQueueProposal() {
    if (!g.sel.open) return;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        const std::string eff = SelEffectiveKey(g.sel.decisions, g.cardPath, g.sel.srcPath, g.sel.dstOutfit, g.sel.srcOutfit, g.sel.srcIsCoordinate);
        if (g.sel.proposalKey == eff) return;   // already built (or queued) for this exact merge
        g.sel.proposalKey = eff;
    }
    g.sel.proposalQueued = true;
    SetTimer(g.sel.hwnd, kSelBuildTimer, 250, nullptr);
}

// False while any preview is building (decisions may have changed under it); true once everything is ready.
bool SelPreviewsReady() {
    for (const SelView& v : g.sel.views) if (v.state.load() == 1) return false;
    return true;
}

// ==================================================================== render & paint ========================
// Everything is composed into an off-screen device context and blitted once per WM_PAINT: the window
// never shows a half-drawn frame, no matter how often invalidations arrive (a hover, a camera drag).
// The 3D pictures themselves are produced on the render worker (never here) and blitted as-is.

void SelSyncView0Hidden() {
    SelView& v = g.sel.views[0];
    if (!v.scene) return;
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (g.effHidden.size() == v.scene->parts.size() && g.effHidden != v.hidden) {
        v.hidden = g.effHidden;
        v.dirty = true;   // a part was hidden or shown: the picture changed
    }
}

// ==================================================================== the render worker ========================
// Every Direct3D call happens on one background thread, never on the UI thread: WM_PAINT only blits the
// cached pictures, and this worker produces them whenever something changed (a new scene, the pose, the
// camera, hidden parts, the hover/selection highlights, a resize). Each view owns its own Direct3D device,
// so the three pictures never touch the main window's device.

// Snapshots everything one picture needs, under g.mu (the Direct3D call itself happens after the lock is released).
struct SelFrameInputs {
    std::shared_ptr<const assemble::Scene> scene;
    exporter::ViewCamera cam;
    std::vector<char> hidden;
    std::vector<exporter::HighlightPart> highlights;
    std::vector<Mat4> skin;      // the software fallback's skin (views 1 and 2: the shared A-pose)
    int singleHighlight = -1;    // the software fallback colours one part
};

bool SelSnapshotView(int view, SelFrameInputs& in) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    SelView& v = g.sel.views[view];
    if (view == 0) SelSyncView0Hidden();   // the main view's hide/show must reach the picture
    if (v.state != 2 || !v.scene) return false;
    in.scene = v.scene;
    in.cam = v.cam;
    in.hidden = v.hidden;
    in.highlights = SelHighlightsFor(view);
    in.skin = v.previewSkin;
    const int hov = g.sel.hoverComp >= 0 ? g.sel.hoverComp : g.sel.selComp;
    in.singleHighlight = (hov >= 0 && static_cast<size_t>(hov) < v.compParts.size() && !v.compParts[hov].empty()) ? v.compParts[hov][0] : -1;
    return true;
}

// Renders one view if (and only if) it owes a picture. Returns true when one was produced.
bool SelRenderViewNow(int view, const SelLayout& l) {
    SelView& v = g.sel.views[view];
    const RECT rect = l.views[view];
    const int W = rect.right - rect.left, H = rect.bottom - rect.top;
    if (W < 8 || H < 8) return false;
    if (!v.dirty) {   // cached and current: free
        std::lock_guard<std::mutex> flk(v.frameMtx);
        if (v.fw == W && v.fh == H && !v.frame.empty()) return false;
    }
    SelFrameInputs in;
    if (!SelSnapshotView(view, in)) { v.dirty = false; return false; }
    std::shared_ptr<exporter::GpuView> dev;
    std::unique_ptr<exporter::ViewState, exporter::ViewStateDeleter> swState;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);   // one local copy of the device and the software cache for this render
        if (!v.gpu) v.gpu = std::make_shared<exporter::GpuView>();
        dev = v.gpu;
        if (!v.swState) v.swState = exporter::NewViewState();
        swState = std::move(v.swState);
    }
    std::vector<uint32_t> bgra;
    bool drawn = false;
    {
        std::lock_guard<std::mutex> gpuLk(g.sel.selGpuMu);
        if (v.gpuOk && dev->ready())
            drawn = dev->RenderHighlighted(in.cam, W, H, &in.hidden, bgra, in.highlights);
        if (!drawn) {
            // Software fallback (no Direct3D 11): one tinted part (the hovered one).
            exporter::ViewOptions vopt;
            vopt.textured = true;
            vopt.shaded = true;
            vopt.hiddenParts = &in.hidden;
            vopt.skin = in.skin.empty() ? nullptr : &in.skin;
            vopt.highlightPart = in.singleHighlight;
            if (in.scene) exporter::RenderView(*in.scene, in.cam, W, H, vopt, *swState, bgra);
            drawn = !bgra.empty();
        }
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.sel.renderStop) {   // the window is already gone: hand the software cache back and discard the picture
            if (!v.swState) v.swState = std::move(swState);
            v.dirty = false;
            return false;
        }
        if (!v.swState) v.swState = std::move(swState);   // hand it back if nothing replaced it
    }
    if (drawn && !bgra.empty()) {
        std::lock_guard<std::mutex> flk(v.frameMtx);
        v.frame = std::move(bgra);
        v.fw = W; v.fh = H;
    }
    v.dirty = false;   // one attempt per invalidation: a failure keeps the placeholder/the old frame, no retry loop
    return drawn;
}

// True while any view owes a picture (the worker's wait predicate).
bool SelAnyViewDirty() {
    HWND h = nullptr;
    int states[3];
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        h = g.sel.open ? g.sel.hwnd : nullptr;
        for (int i = 0; i < 3; ++i) states[i] = g.sel.views[i].state;
    }
    if (!h) return false;
    RECT rc;
    if (!GetClientRect(h, &rc)) return false;
    if (rc.right <= 8 || rc.bottom <= 8) return false;
    const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    for (int view = 0; view < 3; ++view) {
        SelView& v = g.sel.views[view];
        const RECT r = l.views[view];
        const int W = r.right - r.left, H = r.bottom - r.top;
        if (W < 8 || H < 8 || states[view] != 2) continue;
        if (v.dirty) return true;
        std::lock_guard<std::mutex> flk(v.frameMtx);
        if (v.fw != W || v.fh != H || v.frame.empty()) return true;
    }
    return false;
}

// The render worker: waits until a view owes a picture, re-renders the dirty views, and wakes the paint
// path (WM_SEL_FRAME). Runs for the window's lifetime. SelectiveClose asks it to stop (never joins mid-render):
// it bails between pictures and returns; the detached cleaner thread (SelectiveClose) joins it and releases
// the GPU copies — so the window closes instantly and no Direct3D object is torn down under a running render.
void SelRenderWorkerLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g.sel.renderMtx);
            g.sel.renderCv.wait(lk, [] { return g.sel.renderStop || SelAnyViewDirty(); });
            if (g.sel.renderStop) return;
        }
        HWND h = nullptr;
        { std::lock_guard<std::recursive_mutex> lk(g.mu); h = g.sel.open ? g.sel.hwnd : nullptr; }
        if (!h) continue;
        RECT rc;
        if (!GetClientRect(h, &rc) || rc.right <= 8 || rc.bottom <= 8) continue;
        const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
        bool any = false;
        for (int view = 0; view < 3; ++view) {
            {
                std::lock_guard<std::mutex> lk(g.sel.renderMtx);
                if (g.sel.renderStop) break;   // stop between pictures, never inside one
            }
            if (SelRenderViewNow(view, l)) any = true;
        }
        if (any) {
            HWND h2 = nullptr;
            { std::lock_guard<std::recursive_mutex> lk(g.mu); h2 = g.sel.open ? g.sel.hwnd : nullptr; }
            if (h2) PostMessageW(h2, WM_SEL_FRAME, 0, 0);
        }
    }
}

void SelDrawFrame(int view, HDC dc, const RECT& rect) {
    SelView& v = g.sel.views[view];
    const int W = rect.right - rect.left, H = rect.bottom - rect.top;
    if (W >= 8 && H >= 8) {
        const int state = v.state;
        std::lock_guard<std::mutex> flk(v.frameMtx);
        if (state == 2 && !v.frame.empty() && v.fw == W && v.fh == H) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, rect.left, rect.top, W, H, 0, 0, W, H, v.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            return;
        }
    }
    // Not ready (or the first worker frame has not landed yet): a placeholder card
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (view == 0) SelSyncView0Hidden();
    }
    HBRUSH bg = CreateSolidBrush(ui::kCard);
    FillRect(dc, &rect, bg);
    DeleteObject(bg);
    RECT inner{rect.left + 14, rect.top + 14, rect.right - 14, rect.bottom - 14};
    int state;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        state = v.state;
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT f = state == 3 ? g.fontBold : g.fontUi;
    const COLORREF c = state == 3 ? ui::kBad : ui::kSub;
    const char* line1 = state == 3 ? "preview failed" : (state == 0 ? "queued ..." : "assembling preview ...");
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutW(dc, inner.left, inner.top, Utf8ToWide(line1).c_str(), static_cast<int>(strlen(line1)));
    if (state == 3) {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (!v.error.empty()) {
            RECT clip{inner.left, inner.top + 22, inner.right, inner.bottom};
            std::wstring msg = Utf8ToWide(v.error);
            SetTextColor(dc, ui::kSub);
            DrawTextW(dc, msg.c_str(), -1, &clip, DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        return;
    }
    // stage line
    std::string stage;
    { std::lock_guard<std::mutex> lk(v.progMtx); stage = v.progStage; }
    if (!stage.empty()) {
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, ui::kDim);
        RECT clip{inner.left, inner.top + 24, inner.right, inner.top + 44};
        DrawTextW(dc, Utf8ToWide(stage).c_str(), -1, &clip, DT_SINGLELINE | DT_END_ELLIPSIS | DT_LEFT);
    }
    // progress bar
    const double frac = v.progFrac.load(std::memory_order_relaxed);
    const int barX = inner.left, barW = std::clamp<int>(inner.right - inner.left, 80, 230);
    const int barH = 10, barY = inner.top + 54;
    const RECT bar{barX, barY, barX + barW, barY + barH};
    HBRUSH track = CreateSolidBrush(ui::kCardHi);
    HPEN barPen = CreatePen(PS_SOLID, 1, ui::kCardHi);
    HGDIOBJ ob = SelectObject(dc, track), op = SelectObject(dc, barPen);
    RoundRect(dc, bar.left, bar.top, bar.right, bar.bottom, barH, barH);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(track); DeleteObject(barPen);
    if (frac >= 0.0) {
        const int fill = static_cast<int>((barW - 2) * frac);
        if (fill > 0) {
            HBRUSH accent = CreateSolidBrush(ui::kAccent);
            HGDIOBJ ob2 = SelectObject(dc, accent);
            HPEN ap = CreatePen(PS_SOLID, 1, ui::kAccent);
            HGDIOBJ op2 = SelectObject(dc, ap);
            const RECT fr{barX + 1, barY + 1, barX + 1 + fill, barY + barH - 1};
            RoundRect(dc, fr.left, fr.top, fr.right, fr.bottom, barH - 2, barH - 2);
            SelectObject(dc, ob2); SelectObject(dc, op2);
            DeleteObject(accent); DeleteObject(ap);
        }
        char pct[16];
        std::snprintf(pct, sizeof pct, "%d %%", static_cast<int>(frac * 100.0 + 0.5));
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, ui::kSub);
        SetBkMode(dc, TRANSPARENT);
        RECT prc{barX, barY + barH + 5, barX + barW, barY + barH + 24};
        std::wstring pctw = Utf8ToWide(pct);
        DrawTextW(dc, pctw.c_str(), -1, &prc, DT_RIGHT | DT_SINGLELINE);
    } else {
        // indeterminate: a chunk slides along the track until the assembly starts reporting fractions
        const int slide = static_cast<int>((GetTickCount64() / 12) % static_cast<uint64_t>(barW + 80)) - 40;
        const int chunkW = 64;
        int cL = barX + slide, cR = cL + chunkW;
        if (cL < barX + 1) cL = barX + 1;
        if (cR > barX + barW - 1) cR = barX + barW - 1;
        if (cR > cL) {
            HBRUSH accent = CreateSolidBrush(ui::kAccent);
            HGDIOBJ ob2 = SelectObject(dc, accent);
            HPEN ap = CreatePen(PS_SOLID, 1, ui::kAccent);
            HGDIOBJ op2 = SelectObject(dc, ap);
            RoundRect(dc, cL, barY + 1, cR, barY + barH - 1, barH - 2, barH - 2);
            SelectObject(dc, ob2); SelectObject(dc, op2);
            DeleteObject(accent); DeleteObject(ap);
        }
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, ui::kDim);
        RECT prc{barX, barY + barH + 5, barX + barW, barY + barH + 24};
        DrawTextW(dc, L"...", -1, &prc, DT_RIGHT | DT_SINGLELINE);
    }
}

// GDI helpers (the direct-drawing UI layer). `corner` is the TRUE corner radius, like Surface::FillRound.
void SelRoundBox(HDC dc, const RECT& r, int corner, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, corner * 2, corner * 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
void SelText(HDC dc, HFONT font, COLORREF col, int x, int y, const std::string& s) {
    const std::wstring w = Utf8ToWide(s);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, font);
    TextOutW(dc, x, y, w.c_str(), static_cast<int>(w.size()));
}
void SelTextRight(HDC dc, HFONT font, COLORREF col, int right, int y, const std::string& s) {
    const std::wstring w = Utf8ToWide(s);
    SIZE sz{};
    SelectObject(dc, font);
    GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(w.size()), &sz);
    SelText(dc, font, col, right - static_cast<int>(sz.cx), y, s);
}
void SelTextCentered(HDC dc, HFONT font, COLORREF col, const RECT& r, const std::string& s) {
    const std::wstring w = Utf8ToWide(s);
    RECT t = r;
    SIZE sz{};
    SelectObject(dc, font);
    GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(w.size()), &sz);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    const int tw = static_cast<int>(sz.cx), th = static_cast<int>(sz.cy);
    const int tx = static_cast<int>(t.left) + std::max(0, (static_cast<int>(t.right) - static_cast<int>(t.left) - tw) / 2);
    const int ty = static_cast<int>(t.top) + std::max(0, (static_cast<int>(t.bottom) - static_cast<int>(t.top) - th) / 2);
    TextOutW(dc, tx, ty, w.c_str(), static_cast<int>(w.size()));
}
int SelTextWidth(HFONT font, const std::string& s) {
    const std::wstring w = Utf8ToWide(s);
    SIZE sz{};
    // caller must have a font selected or use SelectObject themselves; we select it here into a screen DC-free helper
    HDC screen = GetDC(nullptr);
    SelectObject(screen, font);
    GetTextExtentPoint32W(screen, w.c_str(), static_cast<int>(w.size()), &sz);
    ReleaseDC(nullptr, screen);
    return static_cast<int>(sz.cx);
}

// The caption chip over each viewport.
void SelDrawViewChip(HDC dc, const RECT& view, int which, const std::string& sub) {
    static const char* kTitle[3] = {"CURRENT", "IMPORTED", "PROPOSED"};
    const int x = view.left + 8, y = view.top + 8;
    const int h = 22;
    std::string text = kTitle[which];
    if (!sub.empty()) text += "  -  " + sub;
    const int tw = SelTextWidth(g.fontBold, text);
    const RECT chip{x, y, x + tw + 18, y + h};
    SelRoundBox(dc, chip, 6, ui::kCard, which == 2 ? ui::kAccent : ui::kLineHi);
    SelText(dc, g.fontBold, which == 2 ? ui::kAccent : ui::kText, chip.left + 9, chip.top + 3, text);
    if (which == 0) SelText(dc, g.fontSmall, ui::kDim, chip.left + 1, chip.bottom + 2, "unchanged");
    else if (which == 1) SelText(dc, g.fontSmall, ui::kDim, chip.left + 1, chip.bottom + 2, "the source, on its own");
    else SelText(dc, g.fontSmall, ui::kDim, chip.left + 1, chip.bottom + 2, "a preview of the import");
}

void SelDrawLegendChip(HDC dc, int x, int y, COLORREF col, const std::string& label) {
    RECT r{x, y + 2, x + 12, y + 14};
    HBRUSH b = CreateSolidBrush(col);
    FillRect(dc, &r, b);
    DeleteObject(b);
    SelText(dc, g.fontSmall, ui::kSub, x + 16, y, label);
}

void SelButton(HDC dc, const RECT& r, const std::string& label, bool hot, bool on, bool enabled, bool primary) {
    if (primary && enabled) {
        SelRoundBox(dc, r, 6, hot ? ui::kAccentHi : ui::kAccent, ui::kAccent);
        SelTextCentered(dc, g.fontBold, ui::kOnAccent, r, label);
        return;
    }
    SelRoundBox(dc, r, 6, !enabled ? ui::kCard : hot ? ui::kSelect : on ? ui::kSelect : ui::kCardHi, on ? ui::kAccent : ui::kLine);
    SelTextCentered(dc, g.fontUi, !enabled ? ui::kFaint : on ? ui::kAccentHi : ui::kText, r, label);
}

namespace {
Vec3 selSub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 selCross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float selDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
}   // namespace

// The A-pose (plus the user's bone edits, matched by name) applied to one preview scene.
assemble::Pose SelBuildPreviewPose(const assemble::Scene& scene) {
    assemble::Pose p;
    if (!g.sel.aPose) return p;
    assemble::MakeAPose(scene, g.aPoseDeg, p.base);
    for (const auto& [name, e] : g.boneEdits) {
        if (assemble::IsIdentity(e)) continue;
        const int i = scene.boneIndex(name);
        if (i >= 0) p.edit(static_cast<size_t>(i)) = e;
    }
    return p;
}

// Skins the static previews with the shared A-pose, keeps the matrices for the picker, refreshes the pictures.
// View 0 now owns its device, so it is skinned like the other two. The GPU calls are serialised with the render worker.
void SelApplyPreviewSkins() {
    // Build the A-pose matrices on the CPU first (g.mu), then push them to the GPU (selGpuMu leaf).
    std::vector<std::pair<int, std::vector<Mat4>>> skins;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        for (int view = 0; view < 3; ++view) {
            SelView& v = g.sel.views[view];
            if (!v.scene) continue;
            const assemble::Pose p = SelBuildPreviewPose(*v.scene);
            v.previewSkin = assemble::SkinMatrices(*v.scene, p);
            skins.emplace_back(view, v.previewSkin);
        }
    }
    std::vector<std::pair<int, std::shared_ptr<exporter::GpuView>>> devs;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);   // copy the devices under the same lock the cleaner steals them
        for (const auto& [view, skin] : skins) devs.emplace_back(view, g.sel.views[view].gpu);
    }
    std::lock_guard<std::mutex> gpuLk(g.sel.selGpuMu);
    for (size_t i = 0; i < devs.size(); ++i) {
        const int view = devs[i].first;
        if (g.sel.views[view].gpuOk && devs[i].second) devs[i].second->SetSkin(skins[i].second.empty() ? nullptr : &skins[i].second);
        g.sel.views[view].dirty = true;   // the pose changed: re-render once, then cache
    }
}

// Möller–Trumbore; on hit, t is written.
bool SelRayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    const Vec3 e1 = selSub(b, a), e2 = selSub(c, a);
    const Vec3 p = selCross(d, e2);
    const float det = selDot(e1, p);
    if (std::fabs(det) < 1e-9f) return false;
    const float inv = 1.0f / det;
    const Vec3 tv = selSub(o, a);
    const float u = selDot(tv, p) * inv;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return false;
    const Vec3 q = selCross(tv, e1);
    const float vv = selDot(d, q) * inv;
    if (vv < -1e-6f || u + vv > 1.0f + 1e-6f) return false;
    t = selDot(e2, q) * inv;
    return t > 1e-6f;
}

// Picks the component under (px, py) in a view's picture. Skinned positions are computed on the fly (clothing meshes only).
int SelPick(int view, int px, int py, int viewW, int viewH) {
    SelView& v = g.sel.views[view];
    std::shared_ptr<const assemble::Scene> scene;
    std::vector<char> hidden;
    exporter::ViewCamera cam;
    std::vector<std::vector<int>> compParts;
    std::vector<Mat4> previewSkin;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);   // a rebuild swaps these; take a consistent snapshot
        scene = v.scene;
        hidden = v.hidden;
        cam = v.cam;
        compParts = v.compParts;
        previewSkin = v.previewSkin;
    }
    if (!scene) return -1;
    exporter::ViewFraming f;
    if (!exporter::detail::ComputeViewFraming(*scene, &hidden, static_cast<float>(viewW), static_cast<float>(viewH), cam, f)) return -1;
    Vec3 o, d;
    if (!exporter::detail::ViewRayAt(f, static_cast<float>(viewW), static_cast<float>(viewH), cam, static_cast<float>(px), static_cast<float>(py), o, d)) return -1;
    const std::vector<Mat4>* skin = nullptr;
    std::vector<Mat4> copied;
    if (view == 0) {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        copied = g.skin;
        skin = &copied;
    } else skin = &previewSkin;
    const bool posed = skin && !skin->empty();
    float bestT = 1e30f; int best = -1;
    std::vector<Vec3> sp;
    for (size_t ci = 0; ci < g.sel.comps.size(); ++ci) {
        if (ci >= compParts.size()) break;
        for (int pi : compParts[ci]) {
            const auto& part = scene->parts[static_cast<size_t>(pi)];
            if (!part.visibleInPreview) continue;
            for (int mi : part.meshes) {
                const auto& mesh = scene->meshes[static_cast<size_t>(mi)];
                sp.assign(mesh.vertices.size(), Vec3{});
                for (size_t vi = 0; vi < mesh.vertices.size(); ++vi)
                    sp[vi] = posed ? assemble::SkinPoint(mesh.vertices[vi], *skin) : mesh.vertices[vi].pos;
                for (const auto& sm : mesh.submeshes) {
                    const auto& idx = sm.indices;
                    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
                        float t;
                        if (!SelRayTriangle(o, d, sp[idx[i]], sp[idx[i + 1]], sp[idx[i + 2]], t)) continue;
                        if (t < bestT) { bestT = t; best = static_cast<int>(ci); }
                    }
                }
            }
        }
    }
    return best;
}

void SelTimerFired() {
    if (!g.sel.open) return;
    KillTimer(g.sel.hwnd, kSelBuildTimer);
    if (g.sel.buildingProposal) return;   // the WM_SEL_READY handler starts the queued build when the running one lands
    g.sel.proposalQueued = false;
    g.sel.buildingProposal = true;
    SelBuildProposal();
}

// Remove the pinned layer at `layerIndex` (counted across all pinned layer pins) and reload, restoring the stack below it.
void SelRemoveLayer(int layerIndex) {
    if (layerIndex < 0) return;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        int n = -1;
        for (auto it = g.clothingPins.begin(); it != g.clothingPins.end(); ++it) {
            if (it->mode != 3) continue;
            ++n;
            if (n == layerIndex) {
                Log("removed layer \"" + it->name + "\" from the import stack", LogKind::Info);
                g.clothingPins.erase(it);
                break;
            }
        }
    }
    // reload the character without the layer
    StartLoad(g.cardPath, -1, true);
}

// ==================================================================== open / close ========================

void SelectiveInvalidateView();

// Drops the CPU-side state of every preview view (scene, component maps, cached pictures, software caches).
// The loaded character is untouched. The Direct3D devices are NOT released here: SelectiveClose hands them
// to a detached cleaner thread (see there), so this function never waits on a GPU call and is safe at any time.
void SelTeardownPreview() {
    {
        std::lock_guard<std::recursive_mutex> gLk(g.mu);   // the CPU-side state (paint placeholders read it)
        for (SelView& v : g.sel.views) {
            v.scene.reset();
            v.hidden.clear();
            v.compParts.clear();
            v.placedAcc.clear();
            v.previewSkin.clear();
            {
                std::lock_guard<std::mutex> flk(v.frameMtx);   // the cached picture: drop it under the same guard the blit uses
                v.frame.clear();
                v.fw = v.fh = 0;
            }
            v.dirty = true;
            v.state = 0;
            v.gpuOk = false;
            v.error.clear();
            // the software cache (ViewState) is recreated lazily by the render path; never reset it here —
            // an old worker may still be using it until the cleaner joins it
        }
        g.sel.comps.clear();
        g.sel.decisions.clear();
        g.sel.deletedParts.clear();
        g.sel.buildingProposal = false;
        g.sel.proposalQueued = false;
        g.sel.proposalGen = 0;
    }
}

// Fill the component rows from the assembled preview scenes (the parts carry the item's names, mods and resolution status).
void SelFillNames(int view) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    SelView& v = g.sel.views[view];
    if (!v.scene) return;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        if (i >= v.compParts.size()) break;
        for (int pi : v.compParts[i]) {
            const auto& p = v.scene->parts[static_cast<size_t>(pi)];
            const std::string label = p.listName.empty() ? (p.modName.empty() ? p.id : p.modName) : p.listName;
            if (view == 0) {
                g.sel.comps[i].existingName = label;
            } else {
                g.sel.comps[i].sourceName = label;
                g.sel.comps[i].sourceStatus = p.status;
                g.sel.comps[i].modName = p.modName;
                g.sel.comps[i].missing = p.status.rfind("missing", 0) == 0;
            }
            break;   // one part per component is enough for the name (a top's sub-parts share one name)
        }
    }
    SelectiveInvalidateView();
}

// Rebuilds the component list and the suggested decisions from the (re)parsed source. False when the source cannot be read.
bool SelBuildComponentList() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // the render worker reads the rows and the decisions
    g.sel.comps.clear();
    g.sel.decisions.clear();
    std::shared_ptr<Loaded> L;
    { std::lock_guard<std::recursive_mutex> lk(g.mu); L = g.loaded; }
    if (!L) return false;
    const int dstOutfit = g.sel.dstOutfit;
    card::ClothingFile cl;
    try {
        if (g.sel.srcIsCoordinate) {
            card::Card src = card::LoadCard(g.sel.srcPath);
            if (!src.supported) throw std::runtime_error(src.supportNote);
            cl = card::ExtractClothingFromCard(src, g.sel.srcOutfit);
        } else {
            cl = card::LoadClothingFile(g.sel.srcPath);
            if (!cl.supported) throw std::runtime_error(cl.supportNote);
        }
    } catch (const std::exception& e) {
        g.sel.status = std::string("cannot read the source: ") + e.what();
        return false;
    }
    const auto components = card::EnumerateSelectiveComponents(L->card, cl, dstOutfit);
    for (const auto& c : components) {
        SelectiveComp sc;
        sc.comp = c;
        sc.sourceName = c.label;   // filled in from the preview scenes (SelFillNames)
        g.sel.comps.push_back(std::move(sc));
        g.sel.decisions[c.key] = c.action;   // the suggested defaults
    }
    g.sel.listScroll = 0;
    g.sel.listPos = 0;
    g.sel.hoverComp.store(-1); g.sel.selComp.store(-1);
    return true;
}

// Opens the Selective Import window for `path` (a dropped outfit source). Preconditions match the import dialog's:
// a loaded, idle character on screen. The source may be a plain outfit file or a Coordinate PNG.
void SelectiveOpen(const std::string& path) {
    if (g.cardPath.empty() || g.busy || !Current()) {
        Log("open a character first, then choose Selective Import", LogKind::Warn);
        return;
    }
    if (g.sel.open) {
        if (g.sel.srcPath == path) {
            // the same source again: just show the window
            if (IsIconic(g.sel.hwnd)) ShowWindow(g.sel.hwnd, SW_RESTORE);
            ShowWindow(g.sel.hwnd, SW_SHOW);
            SetForegroundWindow(g.sel.hwnd);
            return;
        }
        SelectiveClose();   // a different source: replace the window and its preview entirely
    }
    auto L = Current();
    // Parse and validate the source once; the window trusts this.
    bool srcCoord = false;
    std::string srcName;
    int srcOutfit = -1, srcOutfits = 1;
    try {
        const card::ImageSourceKind kind = card::DetectImageSource(path);
        if (kind == card::ImageSourceKind::Coordinate) {
            card::Card src = card::LoadCard(path);
            if (!src.supported) throw std::runtime_error(src.supportNote);
            if (src.outfits.empty()) throw std::runtime_error("the source card has no coordinates");
            srcCoord = true;
            srcOutfits = static_cast<int>(src.outfits.size());
            srcOutfit = src.status.isMap() ? static_cast<int>(src.status.at("coordinateType").asInt()) : 0;
            if (srcOutfit < 0 || static_cast<size_t>(srcOutfit) >= src.outfits.size()) srcOutfit = 0;
            srcName = src.displayName();
        } else if (kind == card::ImageSourceKind::RegularClothing) {
            card::ClothingFile cl = card::LoadClothingFile(path);
            if (!cl.supported) throw std::runtime_error(cl.supportNote);
            srcName = cl.displayName().empty() ? fileio::FileName(path) : cl.displayName();
        } else {
            throw std::runtime_error("not an outfit source (an outfit file or a character card used as one)");
        }
    } catch (const std::exception& e) {
        Log(std::string("cannot import: ") + e.what(), LogKind::Error);
        return;
    }
    const int outfit = L->outfit;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        SelTeardownPreview();         // release any previous preview completely before building a new one
        g.sel.session += 1;          // fresh window; any stale build from a previous one is ignored
        g.sel.open = true;
        g.sel.proposalKey = "\x01";   // sentinel: no proposal ever built for this session
        g.sel.srcPath = path;
        g.sel.srcIsCoordinate = srcCoord;
        g.sel.srcName = srcName;
        g.sel.srcOutfit = srcOutfit;
        g.sel.dstOutfit = outfit;
        g.sel.comps.clear();
        g.sel.decisions.clear();
        g.sel.hoverComp.store(-1); g.sel.selComp.store(-1);
        g.sel.hoverAction = 0;
        g.sel.listScroll = 0;
        g.sel.listPos = 0;
        g.sel.linkCams = true;
        g.sel.aPose = true;
        g.sel.dragging = 0;
        g.sel.dragView = -1;
        g.sel.dragMoved = false;
        g.sel.buildingProposal = false;
        g.sel.proposalQueued = false;
        g.sel.proposalGen = 0;
        g.sel.validation.clear();
        g.sel.validationError = false;
        g.sel.notes.clear();
        g.sel.status = "assembling the preview ...";
        g.sel.views[0].scene = std::shared_ptr<const assemble::Scene>(L, &L->scene);   // the loaded character itself, kept alive while the window is open
        g.sel.views[0].state = 1;   // uploaded on its own Direct3D device on a worker thread (see below)
        g.sel.views[0].hidden = exporter::EffectiveHiddenParts(L->scene, HiddenPartsNow(L->scene));
        g.sel.deletedParts = g.deletedParts;   // the previews hide the parts the main window already deleted
    }
    if (!SelBuildComponentList()) {
        Log("selective import: " + g.sel.status, LogKind::Error);
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.sel.open = false;
            SelTeardownPreview();       // no GPU was ever uploaded; resets the CPU-side state
        }
        g.sel.hwnd = nullptr;
        return;   // nothing was shown; the character was never touched
    }
    SelRemapComponents(0);
    SelFillNames(0);
    // view 1's mapping needs the source scene; it runs on its worker. The window exists first so the user gets feedback.
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const int w = 1480, h = 880;
    const bool testQuiet = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
    // Centre on the viewer, clamped into the monitor's work area (it used to be able to open partly off-screen).
    int x = rc.left + (rc.right - rc.left - w) / 2, y = rc.top + (rc.bottom - rc.top - h) / 2;
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.sel.hwnd = CreateWindowExW(testQuiet ? WS_EX_NOACTIVATE : 0, kSelClassName, L"CardBridge \u2014 Selective Import",
                                     WS_OVERLAPPEDWINDOW | (testQuiet ? 0 : WS_VISIBLE),
                                     testQuiet ? -32000 : x, testQuiet ? -32000 : y, w, h,
                                     g.hwnd, nullptr, g.hinst, nullptr);
    }
    if (!g.sel.hwnd) {
        Log("the Selective Import window could not be created", LogKind::Error);
        return;
    }
    if (HICON icon = LoadIconW(g.hinst, MAKEINTRESOURCEW(1))) SendMessageW(g.sel.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    ShowWindow(g.sel.hwnd, testQuiet ? SW_SHOWNOACTIVATE : SW_SHOW);   // a hidden window never paints (PrintWindow included)
    SetForegroundWindow(g.sel.hwnd);
    const int session = g.sel.session;
    // The render worker produces all three pictures (spawned per window; stopped at close, joined by the cleaner).
    {
        if (g.sel.renderWorker.joinable()) g.sel.renderWorker.join();   // a previous worker, already stopped; a moved-from slot otherwise
        g.sel.renderStop = false;
        g.sel.renderWorker = std::thread(SelRenderWorkerLoop);
    }
    // view 0 uploads its own Direct3D copy on a worker thread too: the window never blocks on the upload
    { std::thread([session] {
        SelUploadScene(0, session);
        bool mine = false;
        { std::lock_guard<std::recursive_mutex> lk(g.mu); mine = g.sel.open && g.sel.session == session; }
        if (mine) SelApplyPreviewSkins();   // the shared A-pose for view 0 as well (takes g.mu itself)
    }).detach(); }
    SelectiveInvalidateView();
    SelBuildImported();          // view 1: the imported source
    g.sel.buildingProposal = true;
    SelBuildProposal();          // view 2: the proposed result, at the default decisions
    if (srcCoord && srcOutfits > 1)
        Log("selective import: " + srcName + " - pick its coordinate on the window's title bar; the other outfits are untouched", LogKind::Info);
    else
        Log("selective import: " + srcName + " - per-component choices; nothing is applied until Apply", LogKind::Info);
}

void SelectiveInvalidateView() {
    g.sel.renderCv.notify_one();   // wake the render worker: a view owes a picture now
    if (g.sel.hwnd) InvalidateRect(g.sel.hwnd, nullptr, FALSE);
}

// The window is closing: discard the preview. The window leaves the screen immediately; the Direct3D
// copies and the software caches are released on a detached cleaner thread that first waits for the
// render worker to exit (it may still be finishing a picture) — the UI thread never blocks on a GPU call.
void SelectiveClose() {
    HWND hwnd = g.sel.hwnd;
    if (!hwnd && !g.sel.open) return;   // already closed (WM_DESTROY after WM_CLOSE)
    if (hwnd) KillTimer(hwnd, kSelBuildTimer);
    {
        std::lock_guard<std::mutex> lk(g.sel.renderMtx);
        g.sel.renderStop = true;
    }
    g.sel.renderCv.notify_one();
    // the views' GPU copies and software caches move into the cleaner's ownership; the window rebuilds from clean state
    std::array<std::shared_ptr<exporter::GpuView>, 3> corpseGpus;
    std::array<std::unique_ptr<exporter::ViewState, exporter::ViewStateDeleter>, 3> corpseSw;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.sel.open = false;
        g.sel.hwnd = nullptr;
        g.sel.session += 1;   // in-flight workers drop their results
        for (int i = 0; i < 3; ++i) {
            SelView& v = g.sel.views[i];
            v.scene.reset();
            v.hidden.clear();
            v.compParts.clear();
            v.placedAcc.clear();
            v.previewSkin.clear();
            {
                std::lock_guard<std::mutex> flk(v.frameMtx);   // the cached picture: drop it under the same guard the blit uses
                v.frame.clear();
                v.fw = v.fh = 0;
            }
            v.dirty = true;
            v.state = 0;
            v.gpuOk = false;
            v.error.clear();
            corpseGpus[i] = std::move(v.gpu);
            corpseSw[i] = std::move(v.swState);
        }
        g.sel.comps.clear();
        g.sel.decisions.clear();
        g.sel.deletedParts.clear();
        g.sel.buildingProposal = false;
        g.sel.proposalQueued = false;
        g.sel.proposalGen = 0;
    }
    if (hwnd) DestroyWindow(hwnd);
    // Hand the old worker thread and the GPU copies to a detached cleaner: it joins the worker (however long
    // its current picture takes) and then releases the devices. The window leaves the screen without waiting.
    std::thread oldWorker;
    {
        std::lock_guard<std::mutex> lk(g.sel.renderMtx);
        oldWorker = std::move(g.sel.renderWorker);
    }
    std::thread([corpseGpus = std::move(corpseGpus), corpseSw = std::move(corpseSw), w = std::move(oldWorker)]() mutable {
        if (w.joinable()) w.join();
        for (const auto& dev : corpseGpus) if (dev) { dev->Unload(); dev->Shutdown(); }
        (void)corpseSw;   // the software caches are released here
    }).detach();
}

void SelectivePoseChanged() {
    // The A-pose, physics or bone edits moved: reskin the static previews (the main view keeps its live skin).
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (!g.sel.open) return;
    SelApplyPreviewSkins();
    SelectiveInvalidateView();
}

// ==================================================================== input ========================
enum SelBtn {
    kSelNone = 0,
    kSelRowAdd = 1, kSelRowReplace = 2, kSelRowKeep = 3, kSelRowSkip = 4,
    kSelResetDecisions = 5, kSelAllReplace = 6, kSelAllSkip = 7, kSelCancel = 8, kSelApply = 9,
    kSelLinkCams = 10, kSelAPose = 11, kSelSrcPrev = 12, kSelSrcNext = 13, kSelRowAddLayer = 14,
};

struct SelHit {
    int kind;      // 0 nothing, 1 a button, 2 a component row, 3 a row's action button, 4 a viewport, 5 the list scrollbar track, 6 a pinned-layer's remove button
    int id = 0;     // the button id / the component index / the view index / the layer row index
    card::SelectiveAction action = card::SelectiveAction::Skip;
};

SelHit SelHitTest(const SelLayout& l, int x, int y) {
    const POINT p{x, y};
    if (PtInRect(&l.linkBtn, p)) return {1, kSelLinkCams};
    if (PtInRect(&l.aposeBtn, p)) return {1, kSelAPose};
    if (g.sel.srcIsCoordinate && PtInRect(&l.srcPrev, p)) return {1, kSelSrcPrev};
    if (g.sel.srcIsCoordinate && PtInRect(&l.srcNext, p)) return {1, kSelSrcNext};
    for (int i = 0; i < 3; ++i) if (PtInRect(&l.views[i], p)) return {4, i};
    for (size_t ri = 0; ri < l.layerRows.size() && ri < l.layerRemove.size(); ++ri)
        if (PtInRect(&l.layerRemove[ri], p)) return {6, static_cast<int>(ri)};   // remove that pinned layer
    if (PtInRect(&l.listClip, p)) {
        for (const SelRowRect& rr : SelVisibleRows(l, static_cast<int>(std::lround(g.sel.listPos)))) {
            if (rr.kind != 1) continue;
            const int ci = rr.comp;
            const bool isClothes = g.sel.comps[static_cast<size_t>(ci)].comp.kind == 0;
            const RECT row{l.listClip.left + 6, rr.top + 1, l.listClip.right - 12, rr.bottom - 1};
            const SelActionRects rb = SelActionButtonRects(l, row, isClothes);
            if (isClothes && PtInRect(&rb.addLayer, p)) return {3, ci, card::SelectiveAction::AddLayer};
            if (PtInRect(&rb.add, p)) return {3, ci, card::SelectiveAction::Add};
            if (PtInRect(&rb.replace, p)) return {3, ci, card::SelectiveAction::Replace};
            if (PtInRect(&rb.keep, p)) return {3, ci, card::SelectiveAction::KeepExisting};
            if (PtInRect(&rb.skip, p)) return {3, ci, card::SelectiveAction::Skip};
            if (PtInRect(&row, p)) return {2, ci};
        }
        if (PtInRect(&l.scrollTrack, p)) return {5, 0};
    }
    if (PtInRect(&l.resetBtn, p)) return {1, kSelResetDecisions};
    if (PtInRect(&l.allReplaceBtn, p)) return {1, kSelAllReplace};
    if (PtInRect(&l.allSkipBtn, p)) return {1, kSelAllSkip};
    if (PtInRect(&l.cancelBtn, p)) return {1, kSelCancel};
    if (PtInRect(&l.applyBtn, p)) return {1, kSelApply};
    return {0, 0};
}

// Bring the selected component's row into view, if it is off screen.
void SelRevealComponent(const SelLayout& l, int compIdx) {
    if (compIdx < 0) return;
    // find the row Y for compIdx
    int y = l.listTop;
    std::string curCat;
    for (size_t i = 0; i <= static_cast<size_t>(compIdx) && i < g.sel.comps.size(); ++i) {
        const auto& comp = g.sel.comps[i].comp;
        if (comp.category != curCat) { curCat = comp.category; y += 22; }
        if (static_cast<int>(i) != compIdx) y += 40;
    }
    const int rowH = 40;
    const int maxScroll = SelListMaxScroll(l);
    int want = g.sel.listScroll;
    if (y - want < l.listTop) want = y - l.listTop;
    else if (y + rowH - want > l.listClip.bottom) want = y + rowH - l.listClip.bottom;
    g.sel.listScroll = std::clamp(want, 0, maxScroll);
    g.sel.listPos = static_cast<float>(g.sel.listScroll);
}

void SelSetDecision(int compIdx, card::SelectiveAction action) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (compIdx < 0 || compIdx >= static_cast<int>(g.sel.comps.size())) return;
    g.sel.decisions[g.sel.comps[static_cast<size_t>(compIdx)].comp.key] = action;
    for (SelView& v : g.sel.views) v.dirty = true;   // highlight colours changed
    SelQueueProposal();
}

void SelApplyCameraDelta(int view, float dx, float dy, bool orbit) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // the render worker reads the cameras under the same lock
    auto apply = [&](exporter::ViewCamera& cam) {
        if (orbit) {
            cam.yawDeg -= dx * 0.5f;
            cam.pitchDeg = std::max(-89.0f, std::min(89.0f, cam.pitchDeg - dy * 0.5f));
        } else {
            cam.panX += dx / 600.0f;   // fractions of the viewport height (a mid-size viewer)
            cam.panY += dy / 600.0f;
        }
    };
    if (g.sel.linkCams) {
        for (int i = 0; i < 3; ++i) { apply(g.sel.views[i].cam); g.sel.views[i].dirty = true; }
    } else {
        apply(g.sel.views[view].cam);
        g.sel.views[view].dirty = true;
    }
}

void SelWheelZoom(int view, int delta) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // the render worker reads the cameras under the same lock
    const float f = delta > 0 ? 1.12f : 1.0f / 1.12f;
    auto apply = [&](exporter::ViewCamera& cam) { cam.zoom = std::max(0.2f, std::min(20.0f, cam.zoom * f)); };
    if (g.sel.linkCams) {
        for (int i = 0; i < 3; ++i) { apply(g.sel.views[i].cam); g.sel.views[i].dirty = true; }
    } else {
        apply(g.sel.views[view].cam);
        g.sel.views[view].dirty = true;
    }
}

int SelViewAtPoint(const SelLayout& l, int x, int y) {
    const POINT p{x, y};
    for (int i = 0; i < 3; ++i) if (PtInRect(&l.views[i], p)) return i;
    return -1;
}

void SelMouseDown(int x, int y, bool right, bool middle) {
    RECT rc;
    GetClientRect(g.sel.hwnd, &rc);
    const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    const SelHit hit = SelHitTest(l, x, y);
    // viewport press: begin a drag (or a click to select)
    if (hit.kind == 4) {
        g.sel.dragging = right || middle ? 2 : 1;
        g.sel.dragView = hit.id;
        g.sel.dragX = x;
        g.sel.dragY = y;
        g.sel.downAt = {x, y};
        g.sel.dragMoved = false;
        SetCapture(g.sel.hwnd);
        return;
    }
    if (hit.kind == 3) {   // a row's action button
        SelSetDecision(hit.id, hit.action);
        g.sel.hoverAction = hit.action == card::SelectiveAction::AddLayer ? kSelRowAddLayer
                           : hit.action == card::SelectiveAction::Add ? kSelRowAdd
                           : hit.action == card::SelectiveAction::Replace ? kSelRowReplace
                           : hit.action == card::SelectiveAction::KeepExisting ? kSelRowKeep : kSelRowSkip;
        SelectiveInvalidateView();
        return;
    }
    if (hit.kind == 6 && hit.id >= 0) {   // remove a pinned layer
        SelRemoveLayer(hit.id);
        SelectiveInvalidateView();
        return;
    }
    if (hit.kind == 2) {   // select the component (sticky); or toggle it off on a second click
        g.sel.selComp = (g.sel.selComp == hit.id) ? -1 : hit.id;
        if (g.sel.selComp >= 0) SelRevealComponent(l, g.sel.selComp);
        for (SelView& v : g.sel.views) v.dirty = true;   // the selected ring moved
        SelectiveInvalidateView();
        return;
    }
    if (hit.kind == 1) {
        switch (hit.id) {
        case kSelLinkCams:
            g.sel.linkCams = !g.sel.linkCams;
            if (g.sel.linkCams) {   // adopt the current view (view 0) as the reference for all three
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                for (int i = 1; i < 3; ++i) g.sel.views[i].cam = g.sel.views[0].cam;
            }
            SelectiveInvalidateView();
            return;
        case kSelAPose:
            g.sel.aPose = !g.sel.aPose;
            SelApplyPreviewSkins();
            SelectiveInvalidateView();
            return;
        case kSelResetDecisions: SelResetDecisions(); SelectiveInvalidateView(); return;
        case kSelAllReplace: SelSetAllDecisions(card::SelectiveAction::Replace); SelectiveInvalidateView(); return;
        case kSelAllSkip: SelSetAllDecisions(card::SelectiveAction::Skip); SelectiveInvalidateView(); return;
        case kSelCancel: SelectiveClose(); return;
        case kSelApply: SelApply(); return;
        case kSelSrcPrev: SelCycleCoordinate(-1); return;
        case kSelSrcNext: SelCycleCoordinate(+1); return;
        default: return;
        }
    }
    // a click on the scrollbar track: page down/up
    if (hit.kind == 5) {
        const int page = std::max(1, (static_cast<int>(l.listClip.bottom) - l.listTop) / 40);
        g.sel.listScroll = y < (l.listClip.top + l.listClip.bottom) / 2 ? std::max(0, g.sel.listScroll - page) : std::min(SelListMaxScroll(l), g.sel.listScroll + page);
        g.sel.listPos = static_cast<float>(g.sel.listScroll);
        SelectiveInvalidateView();
    }
}

void SelMouseMove(int x, int y) {
    RECT rc;
    GetClientRect(g.sel.hwnd, &rc);
    const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    if (g.sel.dragging) {
        const float dx = static_cast<float>(x - g.sel.dragX), dy = static_cast<float>(y - g.sel.dragY);
        if (std::hypot(static_cast<double>(x - g.sel.downAt.x), static_cast<double>(y - g.sel.downAt.y)) > 4.0) g.sel.dragMoved = true;
        SelApplyCameraDelta(g.sel.dragView, dx, dy, g.sel.dragging == 1);
        g.sel.dragX = x;
        g.sel.dragY = y;
        SelectiveInvalidateView();
        return;
    }
    // hover: buttons first, then rows, then the 3D pick
    const SelHit hit = SelHitTest(l, x, y);
    int hoverBtn = 0;
    if (hit.kind == 1) hoverBtn = hit.id;
    else if (hit.kind == 6) hoverBtn = 100 + hit.id;
    else if (hit.kind == 3)
        hoverBtn = hit.action == card::SelectiveAction::AddLayer ? kSelRowAddLayer
                 : hit.action == card::SelectiveAction::Add ? kSelRowAdd
                 : hit.action == card::SelectiveAction::Replace ? kSelRowReplace
                 : hit.action == card::SelectiveAction::KeepExisting ? kSelRowKeep : kSelRowSkip;
    g.sel.hoverAction = hoverBtn;
    int hoverComp = -1;
    if (hit.kind == 2 || hit.kind == 3) hoverComp = hit.id;
    else if (hit.kind == 4) {
        const ULONGLONG now = GetTickCount64();
        if (now - g.sel.lastHoverPick > 33) {   // ~30 Hz is plenty for a highlight
            g.sel.lastHoverPick = now;
            const int view = hit.id;
            hoverComp = SelPick(view, x - l.views[view].left, y - l.views[view].top,
                                static_cast<int>(l.views[view].right) - static_cast<int>(l.views[view].left),
                                static_cast<int>(l.views[view].bottom) - static_cast<int>(l.views[view].top));
        } else {
            hoverComp = g.sel.hoverComp;   // keep the previous pick until the throttle fires
        }
    }
    if (hoverComp != g.sel.hoverComp) {
        g.sel.hoverComp = hoverComp;
        for (SelView& v : g.sel.views) v.dirty = true;   // the hovered ring moved
        SelectiveInvalidateView();
    }
}

void SelMouseUp(int x, int y, bool right, bool middle) {
    if (!g.sel.dragging) return;
    if ((right || middle) != (g.sel.dragging == 2)) return;   // the other button ended a different drag (shouldn't happen)
    const bool wasOrbit = g.sel.dragging == 1;
    const int view = g.sel.dragView;
    const bool moved = g.sel.dragMoved;
    g.sel.dragging = 0;
    g.sel.dragView = -1;
    g.sel.dragMoved = false;
    ReleaseCapture();
    if (wasOrbit && !moved) {
        // a click (not a drag) in a viewport: toggle the component selection
        RECT rc;
        GetClientRect(g.sel.hwnd, &rc);
        const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
        if (view >= 0 && view < 3 && PtInRect(&l.views[view], POINT{x, y})) {
            const int pick = SelPick(view, x - l.views[view].left, y - l.views[view].top,
                                     static_cast<int>(l.views[view].right) - static_cast<int>(l.views[view].left),
                                     static_cast<int>(l.views[view].bottom) - static_cast<int>(l.views[view].top));
            if (pick >= 0) {
                g.sel.selComp = (g.sel.selComp == pick) ? -1 : pick;
                if (g.sel.selComp >= 0) SelRevealComponent(l, g.sel.selComp);
            } else {
                g.sel.selComp = -1;
            }
            for (SelView& v : g.sel.views) v.dirty = true;   // the selected ring moved
            SelectiveInvalidateView();
        }
    }
}

void SelWheel(int x, int y, int delta) {
    RECT rc;
    GetClientRect(g.sel.hwnd, &rc);
    const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    const int view = SelViewAtPoint(l, x, y);
    if (view >= 0) { SelWheelZoom(view, delta); SelectiveInvalidateView(); return; }
    if (PtInRect(&l.listClip, POINT{x, y})) {
        const int step = std::max(1, static_cast<int>(std::lround(120.0 * 1.5)));   // ~1.5 rows per notch
        const int maxScroll = SelListMaxScroll(l);
        const int dir = delta > 0 ? -1 : 1;
        g.sel.listScroll = std::clamp(g.sel.listScroll + dir * step, 0, maxScroll);
        g.sel.listPos = static_cast<float>(g.sel.listScroll);
        SelectiveInvalidateView();
    }
}

// ==================================================================== window ========================
// What a scripted run needs to drive this window: the button centres, the viewport rects, the rows and the
// preview state, as JSON at CARDBRIDGE_TEST_LAYOUT. Only answered when CARDBRIDGE_TEST_HOOKS is set.
void SelDumpLayout() {
    const char* path = std::getenv("CARDBRIDGE_TEST_LAYOUT");
    if (!path) return;
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // the views and the decisions are swapped by workers; take a consistent snapshot
    RECT rc;
    GetClientRect(g.sel.hwnd, &rc);
    const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    auto mid = [](const RECT& r) { return std::vector<int>{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; };
    nlohmann::json j;
    j["client"] = {rc.right, rc.bottom};
    j["views"] = nlohmann::json::array();
    for (int i = 0; i < 3; ++i) {
        const SelView& v = g.sel.views[i];
        j["views"].push_back({{"state", v.state.load()}, {"gpuOk", v.gpuOk.load()}, {"parts", v.scene ? static_cast<int>(v.scene->parts.size()) : -1},
                              {"rect", mid(l.views[i])}});
    }
    j["comps"] = static_cast<int>(g.sel.comps.size());
    j["plan"] = {{"replaced", SelPlan().replaced}, {"added", SelPlan().added}, {"kept", SelPlan().kept}, {"skipped", SelPlan().skipped}};
    j["buttons"] = {{"linkCams", mid(l.linkBtn)}, {"aPose", mid(l.aposeBtn)},
                    {"allReplace", mid(l.allReplaceBtn)}, {"allSkip", mid(l.allSkipBtn)}, {"reset", mid(l.resetBtn)},
                    {"cancel", mid(l.cancelBtn)}, {"apply", mid(l.applyBtn)}};
    j["rows"] = nlohmann::json::array();
    int y = l.listTop;
    std::string curCat;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        const auto& comp = g.sel.comps[i].comp;
        if (comp.category != curCat) { curCat = comp.category; y += 22; }
        const RECT row{l.listClip.left + 6, y, l.listClip.right - 12, y + 40};
        const SelActionRects rb = SelActionButtonRects(l, row, comp.kind == 0);
        j["rows"].push_back({{"key", comp.key}, {"y", (row.top + row.bottom) / 2}, {"addLayer", mid(rb.addLayer)}, {"add", mid(rb.add)}, {"replace", mid(rb.replace)}, {"keep", mid(rb.keep)}, {"skip", mid(rb.skip)}});
        y += 40;
    }
    j["ready"] = SelPreviewsReady();
    j["buildingProposal"] = g.sel.buildingProposal;
    j["proposalQueued"] = g.sel.proposalQueued;
    j["layers"] = nlohmann::json::array();
    for (const ClothingPin& pin : g.clothingPins)
        if (pin.mode == 3)
            j["layers"].push_back({{"name", pin.name}, {"slots", pin.layerSlots}, {"dstOutfit", pin.dstOutfit}});
    std::string s = j.dump();
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(s.data(), 1, s.size(), f); std::fclose(f); }
}

LRESULT CALLBACK SelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: SelPaint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;   // everything is drawn in WM_PAINT
    case WM_SIZE:
        // the 3D pictures render at the view rect's size, so a resize means all three views owe a picture;
        // a debounced timer keeps the drag itself fluid and only re-renders once it settles
        for (int i = 0; i < 3; ++i) g.sel.views[i].dirty = true;
        KillTimer(hwnd, kSelResizeTimer);
        SetTimer(hwnd, kSelResizeTimer, 250, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {1020, 630};   // below this the list and the summary column cannot breathe
        return 0;
    }
    case WM_TEST:
        if (std::getenv("CARDBRIDGE_TEST_HOOKS")) {
            if (wp == 1) { SelApply(); return 0; }
            if (wp == 2) { SelResetDecisions(); return 0; }
            if (wp == 3) { SelSetAllDecisions(card::SelectiveAction::Replace); return 0; }
            if (wp == 4) { SelSetAllDecisions(card::SelectiveAction::Skip); return 0; }
            if (wp == 10) { SelDumpLayout(); return 0; }
        }
        return 0;
    case WM_CLOSE: SelectiveClose(); return 0;
    case WM_DESTROY: SelectiveClose(); return 0;
    case WM_TIMER:
        if (wp == kSelBuildTimer) { SelTimerFired(); return 0; }
        if (wp == kSelResizeTimer) { KillTimer(hwnd, kSelResizeTimer); SelectiveInvalidateView(); return 0; }
        return 0;
    case WM_SEL_FRAME:   // the render worker produced fresh pictures; repaint (a plain blit of the cache)
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SEL_READY: {
        const int session = static_cast<int>(lp);
        if (!g.sel.open || session != g.sel.session) return 0;
        const int kind = static_cast<int>(wp);
        const int view = kind == 0 ? 1 : 2;
        SelRemapComponents(view);
        SelFillNames(view);
        if (kind == 1) {
            g.sel.buildingProposal = false;
            if (g.sel.proposalQueued) {
                g.sel.proposalQueued = false;
                g.sel.buildingProposal = true;
                SelBuildProposal();
            }
        }
        SelApplyPreviewSkins();
        g.sel.status = "ready";
        SelectiveInvalidateView();
        return 0;
    }
    case WM_KEYDOWN: {
        const UINT key = static_cast<UINT>(wp);
        if (key == VK_ESCAPE) { SelectiveClose(); return 0; }
        if (key == VK_RETURN) { SelApply(); return 0; }
        if (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const SelLayout l = SelLay(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
            const int n = static_cast<int>(g.sel.comps.size());
            if (n > 0) {
                int idx = g.sel.selComp >= 0 ? g.sel.selComp.load() : (g.sel.hoverComp >= 0 ? g.sel.hoverComp.load() : 0);
                if (key == VK_HOME) idx = 0;
                else if (key == VK_END) idx = n - 1;
                else idx = std::clamp(idx + (key == VK_DOWN ? 1 : -1), 0, n - 1);
                g.sel.selComp = idx;
                g.sel.hoverComp = idx;
                SelRevealComponent(l, idx);
                for (SelView& v : g.sel.views) v.dirty = true;   // the selection ring moved
                SelectiveInvalidateView();
            }
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: SelMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, false); return 0;
    case WM_RBUTTONDOWN: SelMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, false); return 0;
    case WM_MBUTTONDOWN: SelMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, true); return 0;
    case WM_MOUSEMOVE: SelMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: SelMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, false); return 0;
    case WM_RBUTTONUP: SelMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, false); return 0;
    case WM_MBUTTONUP: SelMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, true); return 0;
    case WM_CAPTURECHANGED: g.sel.dragging = 0; g.sel.dragView = -1; return 0;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        SelWheel(p.x, p.y, static_cast<short>(GET_WHEEL_DELTA_WPARAM(wp)));
        return 0;
    }
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void SelViewportBorder(HDC dc, const RECT& r, COLORREF col, int width) {
    HPEN p = CreatePen(PS_SOLID, width, col);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void SelDrawActionBtn(HDC dc, const RECT& r, const char* label, card::SelectiveAction action, bool on, bool hot, bool enabled) {
    const COLORREF col = SelActionColor(action);
    if (!enabled) {
        SelRoundBox(dc, r, 5, ui::kCard, ui::kLine);
        SelTextCentered(dc, g.fontSmall, ui::kFaint, r, label);
        return;
    }
    if (on) {
        SelRoundBox(dc, r, 5, col, col);
        SelTextCentered(dc, g.fontSmall, SelActionText(action), r, label);
        return;
    }
    SelRoundBox(dc, r, 5, hot ? ui::kLineHi : ui::kSelect, ui::kLineHi);
    SelTextCentered(dc, g.fontSmall, ui::kText, r, label);
}

// The window header: title (or the Coordinate cycler), subtitle, and the two view toggles.
void SelDrawHeader(HDC dc, const SelLayout& l) {
    const bool srcCoord = g.sel.srcIsCoordinate;
    if (!srcCoord) {
        SelText(dc, g.fontSemi, ui::kText, l.title.left, l.title.top - 2, "Selective Import");
        const std::string sub = "outfit source:  " + (g.sel.srcName.empty() ? fileio::FileName(g.sel.srcPath) : g.sel.srcName) +
                                "     landing on outfit " + std::to_string(g.sel.dstOutfit + 1) + "  (in memory only - the card on disk is never saved)";
        RECT clip{l.subtitle.left, l.subtitle.top, l.subtitle.right, l.subtitle.bottom};
        const std::wstring w = Utf8ToWide(sub);
        SetTextColor(dc, ui::kSub);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontSmall);
        DrawTextW(dc, w.c_str(), -1, &clip, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    } else {
        SelButton(dc, l.srcPrev, "<", g.sel.hoverAction == 12, false, true, false);
        {
            const std::string label = g.sel.srcName + "   (outfit " + std::to_string(g.sel.srcOutfit + 1) + ")";
            RECT clip{l.srcLabel.left, l.srcLabel.top, l.srcLabel.right, l.srcLabel.bottom};
            const std::wstring w = Utf8ToWide(label);
            SetTextColor(dc, ui::kText);
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g.fontBold);
            DrawTextW(dc, w.c_str(), -1, &clip, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);
        }
        SelButton(dc, l.srcNext, ">", g.sel.hoverAction == 13, false, true, false);
        const std::string sub = "Coordinate source - pick the coordinate to import from; the rest of the card never travels";
        RECT clip{l.subtitle.left, l.subtitle.top, l.subtitle.right, l.subtitle.bottom};
        const std::wstring w = Utf8ToWide(sub);
        SetTextColor(dc, ui::kSub);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontSmall);
        DrawTextW(dc, w.c_str(), -1, &clip, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelButton(dc, l.linkBtn, "Link cameras", g.sel.hoverAction == 10, g.sel.linkCams, true, false);
    SelButton(dc, l.aposeBtn, "A-pose", g.sel.hoverAction == 11, g.sel.aPose, true, false);
}

// The full legend row under the viewports.
void SelDrawLegend(HDC dc, const SelLayout& l) {
    const int y = l.legend.top + 5;
    const RECT clip = l.legend;
    const std::string hint = "hover a component to light it up in every view and in the list";
    int x = clip.left;
    auto chip = [&](COLORREF col, const char* label) {
        SelDrawLegendChip(dc, x, y, col, label);
        x += 16 + SelTextWidth(g.fontSmall, label) + 14;
    };
    chip(SelActionColor(card::SelectiveAction::AddLayer), "Add layer");
    chip(SelActionColor(card::SelectiveAction::Add), "Add");
    chip(SelActionColor(card::SelectiveAction::Replace), "Replace");
    chip(SelActionColor(card::SelectiveAction::KeepExisting), "Keep existing");
    chip(SelActionColor(card::SelectiveAction::Skip), "Skip");
    chip(RGB(255, 255, 255), "hovered / picked");
    SelTextRight(dc, g.fontSmall, ui::kDim, clip.right, y, hint);
    // a thin divider above the row
    HPEN p = CreatePen(PS_SOLID, 1, ui::kLine);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, l.legend.left, l.legend.top, nullptr);
    LineTo(dc, l.legend.right, l.legend.top);
    SelectObject(dc, op);
    DeleteObject(p);
}

// The pinned garment layers (the ones laid on top of the shown outfit): a strip with one row per layer and a remove button.
// Drawn between the legend and the component list while any layer is pinned. Removing a layer reloads the character.
void SelDrawLayers(HDC dc, const SelLayout& l) {
    if (l.layerRows.empty()) return;
    SelRoundBox(dc, l.layersCard, 8, ui::kCardHi, ui::kLine);
    SelText(dc, g.fontSmall, ui::kFaint, l.layersCard.left + 8, l.layersCard.top + 5, "LAYERS (top of the outfit, applied in order)");
    size_t drawn = 0;
    for (const ClothingPin& pin : g.clothingPins) {
        if (pin.mode != 3) continue;
        const size_t i = drawn;
        if (i >= l.layerRows.size()) break;
        const RECT row = l.layerRows[i];
        const RECT xBtn = l.layerRemove[i];
        const bool hot = g.sel.hoverAction == 100 + static_cast<int>(i);
        SelRoundBox(dc, row, 8, hot ? ui::kSelect : ui::kCardHi, hot ? ui::kLineHi : ui::kLine);
        std::string label = pin.name.empty() ? std::string("layer") : pin.name;
        SelText(dc, g.fontBold, ui::kText, row.left + 8, row.top + 7, "+ " + ClipText(label, 26));
        SelRoundBox(dc, xBtn, 6, hot ? ui::kSelect : ui::kCardHi, hot ? ui::kBad : ui::kLine);
        RECT r{xBtn.left, xBtn.top, xBtn.right, xBtn.bottom};
        SetTextColor(dc, hot ? ui::kText : ui::kFaint);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"x", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ++drawn;
    }
}

// The component list's rows, clipped to the list area.
void SelDrawComponentList(HDC dc, const SelLayout& l) {
    SelRoundBox(dc, l.listCard, 8, ui::kCard, ui::kLine);
    // heading
    const std::string head = "COMPONENTS";
    SelText(dc, g.fontSemi, ui::kSub, l.listCard.left + 12, l.listCard.top + 9, head);
    const int shown = static_cast<int>(g.sel.comps.size());
    SelTextRight(dc, g.fontSmall, ui::kDim, l.listCard.right - 12, l.listCard.top + 9,
                 std::to_string(shown) + (shown == 1 ? " importable part" : " importable parts"));
    if (g.sel.comps.empty()) {
        SelText(dc, g.fontUi, ui::kDim, l.listCard.left + 14, l.listCard.top + 40, "this source carries nothing this outfit can use");
        return;
    }
    const int scrollPx = static_cast<int>(std::lround(g.sel.listPos));
    const HRGN prevRgn = CreateRectRgn(l.listClip.left, l.listClip.top, l.listClip.right, l.listClip.bottom);
    SelectClipRgn(dc, prevRgn);
    const int hoverRow = g.sel.hoverComp;
    for (const SelRowRect& rr : SelVisibleRows(l, scrollPx)) {
        const RECT row{l.listClip.left + 6, rr.top + 1, l.listClip.right - 12, rr.bottom - 1};
        if (rr.kind == 0) {   // a group header
            const std::string cat = g.sel.comps.empty() ? "" : g.sel.comps.front().comp.category;   // unused; header text resolved below
            (void)cat;
            // find the category for this header row by looking at the next row's component
            // (SelVisibleRows emits headers from category changes; we re-derive the name from the sequence)
            continue;
        }
        const int ci = rr.comp;
        const auto& sc = g.sel.comps[static_cast<size_t>(ci)];
        const bool hovered = ci == hoverRow || ci == g.sel.selComp;
        // row background
        if (hovered) {
            SelRoundBox(dc, row, 6, ui::kSelect, ui::kLineHi);
        }
        // the action dot
        const auto it = g.sel.decisions.find(sc.comp.key);
        const card::SelectiveAction eff = it == g.sel.decisions.end() ? card::SuggestedAction(sc.comp) : it->second;
        HBRUSH dot = CreateSolidBrush(SelActionColor(eff));
        HPEN dotPen = CreatePen(PS_SOLID, 1, eff == card::SelectiveAction::Skip ? ui::kDim : SelActionColor(eff));
        HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, dotPen);
        Ellipse(dc, row.left + 10, row.top + 14, row.left + 20, row.top + 24);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(dot); DeleteObject(dotPen);
        // two text lines
        const int tx = row.left + 28;
        const int textRight = (sc.comp.kind == 0 ? SelActionButtonRects(l, row, true).addLayer.left : SelActionButtonRects(l, row, false).add.left) - 8;
        RECT line1{tx, row.top + 3, textRight, row.top + 20};
        RECT line2{tx, row.top + 20, textRight, row.bottom - 2};
        std::string l1 = sc.comp.label + " - " + (sc.sourceName.empty() ? std::string("?") : sc.sourceName);
        if (sc.comp.hasExisting && !sc.existingName.empty()) l1 += "  (was: " + sc.existingName + ")";
        SetTextColor(dc, ui::kText);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontBold);
        DrawTextW(dc, Utf8ToWide(l1).c_str(), -1, &line1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);
        std::string l2 = (sc.comp.hasExisting ? std::string("replaces this outfit's ") : std::string("new here  ·  "));
        l2 += sc.comp.slotName;
        l2 += "  ·  from ";
        l2 += sc.modName.empty() ? "base game" : sc.modName;
        if (sc.missing) l2 += "  -  NOT INSTALLED";
        SetTextColor(dc, sc.missing ? ui::kBad : ui::kDim);
        SelectObject(dc, g.fontSmall);
        DrawTextW(dc, Utf8ToWide(l2).c_str(), -1, &line2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // the action buttons
        const bool isClothes = sc.comp.kind == 0;
        const SelActionRects rb = SelActionButtonRects(l, row, isClothes);
        const UINT hot = g.sel.hoverAction;
        if (isClothes) SelDrawActionBtn(dc, rb.addLayer, "Add layer", card::SelectiveAction::AddLayer,
                                        eff == card::SelectiveAction::AddLayer, hot == kSelRowAddLayer, true);
        SelDrawActionBtn(dc, rb.add, "Add", card::SelectiveAction::Add, eff == card::SelectiveAction::Add, hot == 1, !sc.comp.hasExisting || sc.comp.kind == 1);
        SelDrawActionBtn(dc, rb.replace, "Replace", card::SelectiveAction::Replace, eff == card::SelectiveAction::Replace, hot == 2, sc.comp.hasExisting);
        SelDrawActionBtn(dc, rb.keep, "Keep", card::SelectiveAction::KeepExisting, eff == card::SelectiveAction::KeepExisting, hot == 3, sc.comp.hasExisting);
        SelDrawActionBtn(dc, rb.skip, "Skip", card::SelectiveAction::Skip, eff == card::SelectiveAction::Skip, hot == 4, true);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(prevRgn);
    // group headers: second pass (they were skipped above) — draw them over the rows they belong to
    // (deterministic order, same clip).
    SelectClipRgn(dc, prevRgn);
    std::string curCat;
    int y = l.listTop - scrollPx;
    for (size_t i = 0; i < g.sel.comps.size(); ++i) {
        const auto& comp = g.sel.comps[i].comp;
        if (comp.category != curCat) {
            const RECT hr{l.listClip.left + 8, y, l.listClip.right - 8, y + 22};
            if (y + 22 > l.listClip.top && y < l.listClip.bottom) {
                SelText(dc, g.fontSmall, ui::kFaint, hr.left, hr.top + 4, comp.category);
                // a thin separator line
                HPEN p = CreatePen(PS_SOLID, 1, ui::kLineHi);
                HGDIOBJ op = SelectObject(dc, p);
                MoveToEx(dc, hr.left, hr.bottom - 4, nullptr);
                LineTo(dc, hr.right, hr.bottom - 4);
                SelectObject(dc, op);
                DeleteObject(p);
            }
            curCat = comp.category;
            y += 22;
        }
        y += 40;
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(prevRgn);
    // scrollbar
    const int maxScroll = SelListMaxScroll(l);
    if (maxScroll > 0) {
        const int trackH = static_cast<int>(l.scrollTrack.bottom) - static_cast<int>(l.scrollTrack.top);
        const int contentH = SelListContentHeight();
        const int thumbH = std::max(24, trackH * (static_cast<int>(l.listClip.bottom) - l.listTop) / contentH);
        const int thumbY = static_cast<int>(l.scrollTrack.top) + (maxScroll > 0 ? (trackH - thumbH) * static_cast<int>(g.sel.listPos) / maxScroll : 0);
        SelRoundBox(dc, RECT{l.scrollTrack.left, thumbY, l.scrollTrack.right, thumbY + thumbH}, 3, ui::kLineHi, ui::kLineHi);
    }
}

// The import plan card (right column).
void SelDrawSummary(HDC dc, const SelLayout& l) {
    SelRoundBox(dc, l.sumCard, 8, ui::kCard, ui::kLine);
    SelText(dc, g.fontSemi, ui::kSub, l.sumCard.left + 12, l.sumCard.top + 9, "IMPORT PLAN");
    const card::SelectivePlan plan = SelPlan();
    // counts chips
    int x = l.sumCard.left + 12;
    const int y = l.planChips.top + 1;
    auto chip = [&](int count, const char* label, card::SelectiveAction a) {
        if (count <= 0) return;
        std::string s = std::to_string(count) + " " + label;
        const int w = SelTextWidth(g.fontSmall, s) + 22;
        const RECT r{x, y, x + w, l.planChips.bottom - 2};
        SelRoundBox(dc, r, 8, SelActionColor(a), SelActionColor(a));
        SelTextCentered(dc, g.fontSmall, SelActionText(a), r, s);
        x += w + 6;
    };
    chip(plan.replaced, "replace", card::SelectiveAction::Replace);
    chip(plan.added, "add", card::SelectiveAction::Add);
    chip(plan.kept, "keep", card::SelectiveAction::KeepExisting);
    chip(plan.skipped, "skip", card::SelectiveAction::Skip);
    // notes (hair replacements, relocations): flow below the chips, then the validation box, then a dim info line
    int flowY = l.planChips.bottom + 10;
    const int flowMax = l.sumCard.bottom - 134;   // room for the buttons
    for (const std::string& note : g.sel.notes) {
        if (flowY + 40 > flowMax) break;
        const RECT r{l.sumCard.left + 12, flowY, l.sumCard.right - 12, flowY + 36};
        SelRoundBox(dc, r, 6, Mix(ui::kAmber, ui::kBg, 0.82f), ui::kAmber);
        RECT tx{r.left + 8, r.top + 3, r.right - 8, r.bottom - 3};
        SetTextColor(dc, Mix(ui::kAmber, ui::kText, 0.45f));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontSmall);
        DrawTextW(dc, Utf8ToWide(note).c_str(), -1, &tx, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        flowY += 42;
    }
    // the validation banner (or the dim counterpart line): drawn only when it fits in the free flow area
    // above the button rows; at short heights the buttons take the space and the banner simply doesn't draw
    SelRecomputeValidation();
    if (flowY + 36 <= flowMax) {
        const RECT vb{l.sumCard.left + 12, flowY, l.sumCard.right - 12, flowY + 36};
        if (!g.sel.validation.empty()) {
            SelRoundBox(dc, vb, 6, g.sel.validationError ? Mix(ui::kBad, ui::kBg, 0.80f) : Mix(ui::kGood, ui::kBg, 0.82f), g.sel.validationError ? ui::kBad : ui::kGood);
            RECT tx{vb.left + 8, vb.top + 3, vb.right - 8, vb.bottom - 3};
            SetTextColor(dc, g.sel.validationError ? Mix(ui::kBad, ui::kText, 0.55f) : Mix(ui::kGood, ui::kText, 0.45f));
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g.fontSmall);
            DrawTextW(dc, Utf8ToWide(g.sel.validation).c_str(), -1, &tx, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            SelRoundBox(dc, vb, 6, Mix(ui::kGood, ui::kBg, 0.82f), ui::kGood);
            SelText(dc, g.fontSmall, Mix(ui::kGood, ui::kText, 0.45f), vb.left + 10, vb.top + 10, "everything resolves - ready to apply");
        }
        flowY = vb.bottom + 8;
    }
    // footer line: what does not have a counterpart
    int keptNoCounterpart = 0;
    for (const auto& sc : g.sel.comps) if (!sc.comp.hasExisting && card::ResolveAction(sc.comp, SelDecisionsNow()) == card::SelectiveAction::Skip) ++keptNoCounterpart;
    if (keptNoCounterpart > 0 && flowY + 32 <= flowMax) {
        const RECT ir{l.sumCard.left + 12, flowY, l.sumCard.right - 12, flowY + 32};
        RECT tx{ir.left, ir.top, ir.right, ir.bottom};
        SetTextColor(dc, ui::kDim);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g.fontSmall);
        DrawTextW(dc, Utf8ToWide(std::to_string(keptNoCounterpart) + " component(s) with no counterpart in the source are skipped").c_str(), -1, &tx,
                  DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    // buttons
    const bool busy = g.sel.buildingProposal;
    const card::SelectivePlan p = SelPlan();
    const bool canApply = !busy && !g.sel.validationError && !p.importsNothing() && SelPreviewsReady();
    SelButton(dc, l.allReplaceBtn, "All: Replace", g.sel.hoverAction == 6, false, true, false);
    SelButton(dc, l.allSkipBtn, "All: Skip", g.sel.hoverAction == 7, false, true, false);
    SelButton(dc, l.resetBtn, "Reset to suggested", g.sel.hoverAction == 5, false, true, false);
    SelButton(dc, l.cancelBtn, "Cancel", g.sel.hoverAction == 8, false, true, false);
    SelButton(dc, l.applyBtn, busy ? "Previewing..." : "Apply to character", g.sel.hoverAction == 9, false, canApply, true);
}

void SelViewportBordersAndChips(HDC dc, const SelLayout& l) {
    // borders
    for (int i = 0; i < 3; ++i) SelViewportBorder(dc, l.views[i], i == 2 ? ui::kAccent : ui::kLine, i == 2 ? 2 : 1);
}

void SelDrawFooter(HDC dc, const RECT& r) {
    const std::string left = g.sel.buildingProposal ? "assembling the proposed character ..."
                                                    : "nothing changes on the character until you press Apply";
    SelText(dc, g.fontSmall, g.sel.buildingProposal ? ui::kAmber : ui::kSub, r.left + 2, r.top + 5, left);
    SelTextRight(dc, g.fontSmall, ui::kDim, r.right - 2, r.top + 5,
                 "left-drag orbit  ·  wheel zoom  ·  right-drag pan  ·  hover/click picks  ·  Esc closes");
}

void SelPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = static_cast<int>(rc.right), H = static_cast<int>(rc.bottom);
    if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return; }
    // Compose into an off-screen DC and blit once: no background-flash, no partial picture, however the
    // invalidations land (a hover storm coalesces into a single clean copy).
    static HDC mem = nullptr;
    static HBITMAP bmp = nullptr;
    static int mw = 0, mh = 0;
    if (!mem) { mem = CreateCompatibleDC(hdc); }
    if (mw != W || mh != H) {
        HBITMAP nb = CreateCompatibleBitmap(hdc, W, H);
        SelectObject(mem, nb);   // the previous bmp is no longer selected; safe to delete
        if (bmp) DeleteObject(bmp);
        bmp = nb; mw = W; mh = H;
    }
    HDC dc = mem;
    { HBRUSH b = CreateSolidBrush(ui::kBg); RECT f{0, 0, W, H}; FillRect(dc, &f, b); DeleteObject(b); }
    const SelLayout l = SelLay(W, H);
    // header: title / coordinate cycler + the two toggles
    SelDrawHeader(dc, l);
    // viewport pictures
    for (int i = 0; i < 3; ++i) SelDrawFrame(i, dc, l.views[i]);
    // caption chips
    SelDrawViewChip(dc, l.views[0], 0, "your character");
    SelDrawViewChip(dc, l.views[1], 1, g.sel.srcName);
    SelDrawViewChip(dc, l.views[2], 2, "after import");
    SelViewportBordersAndChips(dc, l);
    // 2D chrome
    SelDrawLegend(dc, l);
    SelDrawLayers(dc, l);
    SelDrawComponentList(dc, l);
    SelDrawSummary(dc, l);
    SelDrawFooter(dc, l.footer);
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

// ==================================================================== the import plan ========================

// Recompute the Apply guard: what the plan does, hair warnings, and whether the proposed scene resolved every imported part.
void SelRecomputeValidation() {
    g.sel.validation.clear();
    g.sel.validationError = false;
    g.sel.notes.clear();
    std::vector<card::SelectiveComponent> ccs;
    ccs.reserve(g.sel.comps.size());
    for (const auto& sc : g.sel.comps) ccs.push_back(sc.comp);
    const card::SelectivePlan plan = card::BuildSelectivePlan(ccs, SelDecisionsNow());
    if (g.sel.comps.empty())
        g.sel.validation = "this source carries no importable components";
    else if (plan.importsNothing())
        g.sel.validation = "nothing is selected for import - tick at least one component, or Reset to the suggested actions";
    if (g.sel.validation.empty()) {
        int hair = 0;
        for (const auto& sc : g.sel.comps) {
            const auto it = g.sel.decisions.find(sc.comp.key);
            if (it == g.sel.decisions.end()) continue;
            if (it->second == card::SelectiveAction::Replace && sc.comp.existingHair) ++hair;
        }
        if (hair) g.sel.notes.push_back(std::to_string(hair) + " replacement(s) drop a hair accessory on screen (kept visible in the previews; the character loses it on Apply)");
    }
    const SelView& v = g.sel.views[2];
    if (!g.sel.buildingProposal && v.state == 2 && v.scene) {
        for (size_t i = 0; i < g.sel.comps.size(); ++i) {
            const auto& sc = g.sel.comps[i];
            const auto it = g.sel.decisions.find(sc.comp.key);
            const card::SelectiveAction a = it == g.sel.decisions.end() ? card::SuggestedAction(sc.comp) : it->second;
            if (a != card::SelectiveAction::Add && a != card::SelectiveAction::Replace) continue;
            if (sc.comp.sourceId != 0 && i < v.compParts.size() && v.compParts[i].empty()) {
                g.sel.validation = "the imported " + sc.sourceName + " did not resolve (mod not installed?) - install it, or deselect it";
                g.sel.validationError = true;
                return;
            }
            if (i < v.compParts.size())
                for (int pi : v.compParts[i])
                    if (v.scene->parts[static_cast<size_t>(pi)].status.rfind("missing", 0) == 0) {
                    g.sel.validation = "the imported " + sc.sourceName + " did not resolve (" + v.scene->parts[static_cast<size_t>(pi)].status +
                                       ") - install its mod, or deselect it";
                    g.sel.validationError = true;
                    return;
                }
        }
    }
    if (!g.sel.validation.empty()) g.sel.validationError = true;
}

// The plan's counts for the chips.
// The library decision map wrapped for the library calls.
card::SelectiveDecisions SelDecisionsNow() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);   // paint-side callers run outside the lock
    card::SelectiveDecisions d;
    d.actions = g.sel.decisions;
    return d;
}
card::SelectivePlan SelPlan() {
    std::vector<card::SelectiveComponent> ccs;
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    ccs.reserve(g.sel.comps.size());
    for (const auto& sc : g.sel.comps) ccs.push_back(sc.comp);
    return card::BuildSelectivePlan(ccs, SelDecisionsNow());
}

// A same-card reload keeps the viewer's delete set keyed by stable part id, and an imported piece reuses
// its slot's id — so a delete left on that slot would keep the new garment hidden and Apply would look
// like it did nothing. Bring back only the pieces the chosen actions actually (re)create; a skipped or
// kept slot keeps its delete.
static void SelRestoreImportedParts(const std::map<std::string, card::SelectiveAction>& decisions) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    const std::string prefix = "outfit" + std::to_string(g.sel.dstOutfit) + ".";
    for (auto it = g.deletedParts.begin(); it != g.deletedParts.end(); ) {
        bool restored = false;
        for (const SelectiveComp& sc : g.sel.comps) {
            const auto dit = decisions.find(sc.comp.key);
            const card::SelectiveAction a = dit == decisions.end() ? card::SuggestedAction(sc.comp) : dit->second;
            if (SelImportCoversPart(*it, prefix, sc, a)) { restored = true; break; }
        }
        if (restored) it = g.deletedParts.erase(it);
        else ++it;
    }
}

// Commit: pin the selective import on the on-screen outfit and reload — the same pin the clothing imports use.
void SelApply() {
    SelRecomputeValidation();
    // Nothing is applied until the preview of the exact decisions is ready (or the rebuild never started):
    // the user has to have seen what they are committing.
    if (g.sel.buildingProposal || g.sel.proposalQueued || g.sel.validationError) { SelectiveInvalidateView(); return; }
    std::shared_ptr<Loaded> L;
    { std::lock_guard<std::recursive_mutex> lk(g.mu); L = g.loaded; }
    if (!L) return;
    const int outfit = g.sel.dstOutfit;
    const std::string name = g.sel.srcName;
    size_t layerCount = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        // A deleted slot must not keep hiding the imported replacement (it reuses the slot's stable part id).
        SelRestoreImportedParts(g.sel.decisions);
        // the layers this apply adds (AddLayer on clothes components), in slot order with stable layer indices
        const std::vector<assemble::AssembleLayerSpec> specLayers = SelLayersFromDecisions(g.sel.decisions);
        for (const assemble::AssembleLayerSpec& spec : specLayers) {
            ClothingPin lp;
            lp.path = spec.path;
            lp.name = spec.name;
            lp.mode = 3;
            lp.srcOutfit = spec.srcOutfit;
            lp.dstOutfit = outfit;
            lp.layerSlots = spec.slots;
            g.clothingPins.push_back(std::move(lp));
        }
        // everything else becomes/extends a normal (mode 2) pinned merge — nothing at all when every component
        // was set to Add Layer. Pushing an empty pair would land the whole file with the *suggested* (Replace)
        // actions on reload after an additive apply — that is what replaced the previous outfit silently.
        card::SelectiveDecisions merge;
        for (const auto& [key, action] : g.sel.decisions)
            if (action != card::SelectiveAction::AddLayer) merge.actions[key] = action;
        const bool mergeImports = std::any_of(merge.actions.begin(), merge.actions.end(),
                                              [](const auto& kv) { return kv.second != card::SelectiveAction::Skip; });
        if (mergeImports) {   // only pin the merge when it imports something; a pure Add-Layer apply pushes no merge pin
            ClothingPin pin;
            pin.path = g.sel.srcPath;
            pin.name = (name.empty() ? fileio::FileName(g.sel.srcPath) : name) + " (selective)";
            pin.mode = 2;
            pin.srcOutfit = g.sel.srcIsCoordinate ? g.sel.srcOutfit : -1;
            pin.dstOutfit = outfit;
            pin.decisions = merge.actions;
            g.clothingPins.push_back(std::move(pin));
        }
        layerCount = g.clothingPins.size();
    }
    const size_t layerSpecCount = [&] { size_t n = 0; for (const auto& kv : g.sel.decisions) if (kv.second == card::SelectiveAction::AddLayer) ++n; return n; }();
    const int kept = static_cast<int>(g.sel.decisions.size());
    SelectiveClose();
    StartLoad(g.cardPath, outfit, true);
    Log("selective import applied to outfit " + std::to_string(outfit + 1) + " (" + std::to_string(kept) + " component choice(s))"
        + (layerSpecCount > 0 ? "; " + std::to_string(layerSpecCount) + " grafted as layer(s)" : std::string())
        + (layerCount > 1 ? "; layer " + std::to_string(layerCount) + " of the imported-clothing stack" : std::string())
        + "; in memory only, card not saved", LogKind::Good);
}

// Reset all decisions to the suggested actions.
void SelResetDecisions() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    for (auto& sc : g.sel.comps) g.sel.decisions[sc.comp.key] = sc.comp.action;
    for (SelView& v : g.sel.views) v.dirty = true;
    SelQueueProposal();
}
// Set every component to one action.
void SelSetAllDecisions(card::SelectiveAction action) {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    for (const auto& sc : g.sel.comps) g.sel.decisions[sc.comp.key] = action;
    for (SelView& v : g.sel.views) v.dirty = true;
    SelQueueProposal();
}
// Switch which coordinate of a Coordinate-PNG source is imported (the viewer renames the outfit label).
void SelCycleCoordinate(int delta) {
    if (!g.sel.srcIsCoordinate) return;
    std::shared_ptr<Loaded> L;
    { std::lock_guard<std::recursive_mutex> lk(g.mu); L = g.loaded; }
    if (!L) return;
    card::Card src;
    try { src = card::LoadCard(g.sel.srcPath); } catch (const std::exception& e) {
        Log(std::string("selective import: ") + e.what(), LogKind::Error);
        return;
    }
    if (!src.supported || src.outfits.empty()) return;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        const int n = static_cast<int>(src.outfits.size());
        int idx = (g.sel.srcOutfit + delta) % n;
        if (idx < 0) idx += n;
        g.sel.srcOutfit = idx;
        SelBuildComponentList();          // re-enumerate from the new coordinate
        SelRemapComponents(0);            // the target's parts are unchanged (identity mapping), kept for clarity
        for (auto& sc : g.sel.comps) g.sel.decisions[sc.comp.key] = sc.comp.action;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.sel.views[1].scene.reset();
        g.sel.views[1].error.clear();
        g.sel.views[1].state = 0;
        g.sel.views[1].compParts.clear();
        g.sel.views[1].placedAcc.clear();
        g.sel.views[1].previewSkin.clear();
        g.sel.views[2].scene.reset();
        g.sel.views[2].error.clear();
        g.sel.views[2].state = 0;
        g.sel.views[2].compParts.clear();
        g.sel.views[2].placedAcc.clear();
        g.sel.views[2].previewSkin.clear();
        g.sel.buildingProposal = false;
        g.sel.proposalQueued = false;
        g.sel.proposalGen = 0;
    }
    SelBuildImported();              // rebuilt from the new coordinate
    g.sel.buildingProposal = true;
    SelBuildProposal();
    SelectiveInvalidateView();
}