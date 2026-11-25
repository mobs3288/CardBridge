// viewer fragment: frame, layout dump, WindowProc (included in order by main.cpp)

// ---- one picture -------------------------------------------------------------------------------------------------------
// A slider's row repaints while it is dragged; nothing else.
void InvalidateSliderRow(int id) {
    const PanelLayout l = Lay(ClientHeight());
    if (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) {
        const int y = l.boneEditTop + (1 + id - kBoneEdit) * kRowH;
        InvalidatePanelBand(y, y + kRowH);
        return;
    }
    if (IsMatId(id)) {
        const int k = id - kMatBase;
        int row = -1;
        if (k >= 0 && k <= 2) row = 2 + k;
        else if (k >= 3 && k <= 8) row = 6 + (k - 3);
        if (row >= 0 && g.matHwnd) {
            // The material sliders live in the Material Workbench window now.
            const WbLayout wl = WbNow();
            const int y = wl.edTop + row * kRowH;
            InvalidateWbBand(y, y + kRowH);
        }
        return;
    }
    const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == 1 && rows[i].id == id) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - ScrollPx(g.partPos);
            InvalidatePanelBand(std::max(l.partsTop, y), std::min(l.partsBottom, y + kRowH));
            break;
        }
}

// Mouse movement during a drag is only noted by the window procedure (dragPending); this applies it, once per picture, so a fast mouse
// cannot queue up work faster than it can be drawn.
// Rows in the current tab's list (Parts or a slider tab; the Bones / Mats tabs have their own lists).
int ListRowCount() {
    if (g.tab == TabAssets) return static_cast<int>(AssetRows().size());
    if (g.tab == TabParts) {
        std::shared_ptr<Loaded> L = Current();
        if (!L) return 0;
        return L->NativePmx() ? static_cast<int>(BuildExplorerRows(*L).size()) : static_cast<int>(BuildRows(*L).size());
    }
    return static_cast<int>(BuildTweakRows(g.tab).size());
}

// A release in the list with no drag: the row under the press does its thing (what the mouse-down used to do; deferred so a
// drag that scrolls the list can cancel the click).
void PanelListClick(int x, int y, const PanelLayout& l) {
    std::shared_ptr<Loaded> L = Current();
    if (g.tab == TabAssets) { AssetsTabClick(x, y, l); return; }
    if (g.tab == TabParts) {
        if (!L || g.showIssues) return;
        if (L->NativePmx()) { ExplorerListClick(x, y, l, *L); return; }
        const std::vector<PartRow> rows = BuildRows(*L);
        const int idx = RowAt(rows, l, x, y);
        if (idx < 0) return;
        const PartRow& row = rows[static_cast<size_t>(idx)];
        if (row.part < 0) {
            if (row.replaced) { /* nothing to expand: the base outfit has no parts left */ }
            else if (x < 30) {   // master checkbox: all parts of this group at once
                ToggleGroup(row.group, L->scene);
            } else {             // elsewhere on the row: collapse / expand
                if (g.collapsedGroups.count(row.group)) g.collapsedGroups.erase(row.group);
                else g.collapsedGroups.insert(row.group);
                g.hoverRow = -1;
                g.hoverPart = -1;
                InvalidatePanel();
            }
            return;
        }
        const int ry = l.partsTop + idx * kRowH - ScrollPx(g.partPos);
        const POINT cp{x, y};
        const RECT delChip = DeleteChipRect(ry);
        if (PtInRect(&delChip, cp)) { DeletePart(row.part); return; }   // x chip: gone from preview and export
        if (static_cast<size_t>(row.part) >= g.hidden.size()) return;
        {
            const auto& pp = L->scene.parts[static_cast<size_t>(row.part)];
            if (!pp.meshes.empty()) {   // every part with geometry offers the gizmo chips
                const RECT chip = GizmoChipRect(ry);
                const RECT rotChip = GizmoRotateChipRect(ry);
                const RECT scaleChip = GizmoScaleChipRect(ry);
                const bool moveHit = PtInRect(&chip, cp), rotHit = PtInRect(&rotChip, cp), scaleHit = PtInRect(&scaleChip, cp);
                if (moveHit || rotHit || scaleHit) {   // the gizmo chips, not the hide/show toggle
                    const int mode = moveHit ? 0 : rotHit ? 1 : 2;
                    const bool sel = GizmoPartSelected() == row.part && g.gizmoMode == mode;
                    g.selGizmoPart = sel ? -1 : row.part;
                    if (!sel) g.gizmoMode = mode;   // switching chips switches the gizmo; deselecting keeps the last mode
                    if (sel) Log("gizmo off (" + pp.id + ")");
                    else {
                        if (mode == 0) Log("gizmo move: " + pp.id + "  (drag an axis in the view, X/Y/Z; right-click the move chip to reset)");
                        else if (mode == 1) Log("gizmo rotate: " + pp.id + "  (drag a ring in the view, X/Y/Z; right-click the rotate chip to reset)");
                        else Log("gizmo resize: " + pp.id + "  (drag a square handle in the view, X/Y/Z; right-click the resize chip to reset)");
                        if (!g.gpuOk && !(g.showExp && g.expOk)) Log("the gizmo needs the Direct3D view", LogKind::Warn);
                    }
                    g.gizmoHoverAxis = g.rotGizmoHoverAxis = g.scaleGizmoHoverAxis = -1;   // the pointer is over the panel, not over an axis of the new gizmo
                    g.dirty = true;
                    InvalidateRect(g.hwnd, nullptr, FALSE);
                    InvalidatePanel();
                    InvalidateFooter();
                    return;
                }
            }
        }
        g.hidden[static_cast<size_t>(row.part)] ^= 1;
        g.soloPart = -1;
        Log(std::string(g.hidden[static_cast<size_t>(row.part)] ? "hide " : "show ") + L->scene.parts[static_cast<size_t>(row.part)].id);
        // Heels: hiding/showing a shoe re-drives the foot pitch at once (settleHeels
        // reads visibleInPreview), otherwise a hidden heeled shoe leaves the foot
        // pitched over a flat shoe. ReshapeNow re-skins from the flat locals and
        // re-poses, so the change is live on the click.
        {
            const std::string& pid = L->scene.parts[static_cast<size_t>(row.part)].id;
            if (pid.find(".shoes") != std::string::npos) {
                for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i)
                    if (L->scene.parts[i].id.find(".shoes") != std::string::npos)
                        L->scene.parts[i].visibleInPreview = !g.hidden[i];
                ReshapeNow(true);
            }
        }
        g.dirty = true;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    if (L || g.tab == TabSettings) TweakClick(x, y, l, false);
}

void ApplyPending() {
    if (!g.dragPending) return;
    g.dragPending = false;
    if (g.sliderDrag >= 0) {
        const int trackL = g.dragTrackL > 0 ? g.dragTrackL : kTrackL;
        const int trackR = g.dragTrackR > 0 ? g.dragTrackR : kTrackR;
        if (IsBodyMeasureId(g.sliderDrag)) {   // a measurement row: the coupled sliders move by the delta this drag position maps to
            const float frac = std::clamp(static_cast<float>(g.dragX - trackL) / static_cast<float>(std::max(1, trackR - trackL)), 0.0f, 1.0f);
            BwhDragAt(g.sliderDrag, frac);
        } else {
            SliderRef r;
            if (SliderFor(g.sliderDrag, r)) {
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                // The drag may be on the panel's track or on the workbench's wider one:
                // both record which one they started on.
                r.Set(SliderValueAt(r, g.dragX, trackL, trackR));
            }
        }
        TweakEdited(g.sliderDrag, false);
        InvalidateSliderRow(g.sliderDrag);
    } else if (g.drag == 3 && (g.boneDragDx != 0.0f || g.boneDragDy != 0.0f)) {
        const float dx = g.boneDragDx, dy = g.boneDragDy;
        g.boneDragDx = g.boneDragDy = 0.0f;
        DragBone(g.dragBone, dx, dy, g.dragMove);
        InvalidateList();
    } else if (g.drag == 6 && g.transPaintPending) {
        g.transPaintPending = false;
        TransStrokeTo(g.transPaintX, g.transPaintY);
        InvalidateList();
    } else if (g.drag == 4 && (g.gizmoDragDx != 0.0f || g.gizmoDragDy != 0.0f)) {
        const float dx = g.gizmoDragDx, dy = g.gizmoDragDy;
        g.gizmoDragDx = g.gizmoDragDy = 0.0f;
        DragPartGizmo(dx, dy);
        InvalidateList();
    } else if (g.drag == 7) {
        DragPartRotateGizmo();
        InvalidateList();
    } else if (g.drag == 8 && (g.scaleGizmoDragDx != 0.0f || g.scaleGizmoDragDy != 0.0f)) {
        const float dx = g.scaleGizmoDragDx, dy = g.scaleGizmoDragDy;
        g.scaleGizmoDragDx = g.scaleGizmoDragDy = 0.0f;
        DragScaleGizmo(dx, dy);
        InvalidateList();
    }
}

// Moves a scroll position towards its target; false when it is there.
bool EaseScroll(float& pos, int target, float dt) {
    const float diff = static_cast<float>(target) - pos;
    if (std::fabs(diff) < 0.02f) {
        if (pos == static_cast<float>(target)) return false;
        pos = static_cast<float>(target);
        return true;
    }
    pos += diff * (1.0f - std::exp(-dt * 20.0f));
    return true;
}

// Everything one picture needs: the drag movement, the physics, the scrolling, then the paint of whatever was invalidated.
void Frame() {
    const double now = NowMs();
    const float dt = g.lastFrameTick > 0.0 ? std::min(1.0f / 30.0f, static_cast<float>(now - g.lastFrameTick) / 1000.0f) : 1.0f / 60.0f;
    g.lastFrameTick = now;
    { cbtime::Scope s("UI: Frame/ApplyPending"); ApplyPending(); }
    { cbtime::Scope s("UI: Frame/PhysicsTick"); PhysicsTick(); }
    const bool a = EaseScroll(g.partPos, g.partScroll, dt);
    const bool b = EaseScroll(g.bonePos, g.boneScroll, dt);
    const bool c = EaseScroll(g.matPos, g.matScroll, dt);
    const bool wbList = EaseScroll(g.wbMatPos, g.wbMatScroll, dt);   // the workbench's own list
    g.animating = a || b || c || wbList;
    if (g.animating) InvalidateList();
    if (wbList) InvalidateWbList();
    // While a job runs, Frame is the single painter for the bar (~30/s): it
    // repaints the overlay card, the strip and the footer hint, so worker
    // reports never queue paints of their own.
    if (g.progActive.load()) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        const RECT card = ProgOverlayRect(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
        InvalidateRect(g.hwnd, &card, FALSE);
        const PanelLayout l = Lay(rc.bottom);
        InvalidatePanelBand(l.open.bottom + 1, l.open.bottom + 12);
        InvalidateFooter();
    }
    if (GetUpdateRect(g.hwnd, nullptr, FALSE)) { cbtime::Scope s("UI: Frame/UpdateWindow (paint)"); UpdateWindow(g.hwnd); }
}

// What a scripted run needs to aim its mouse: the panel's tabs, the rows of the current tab with their centres, the bone list and editor,
// where the overlay drew some bones in the view, and a few state flags. Written as JSON to CARDBRIDGE_TEST_LAYOUT.
void DumpTestLayout() {
    const char* path = std::getenv("CARDBRIDGE_TEST_LAYOUT");
    if (!path) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const PanelLayout l = Lay(rc.bottom);
    nlohmann::json j;
    j["client"] = {rc.right, rc.bottom};
    j["panel"] = kPanel;
    j["track"] = {kTrackL, kTrackR};
    j["rowH"] = kRowH;
    j["tab"] = g.tab;
    j["bakeQuality"] = g.bakeQuality;   // Look tab preset: so a scripted click can verify the chip set it
    j["tabs"] = nlohmann::json::array();
    for (int i = 0; i < TabCount; ++i) j["tabs"].push_back({(l.tabs[i].left + l.tabs[i].right) / 2, (l.tabs[i].top + l.tabs[i].bottom) / 2, true});
    j["buttons"] = {{"open", {(l.open.left + l.open.right) / 2, (l.open.top + l.open.bottom) / 2}}, {"export", {(l.exportPmx.left + l.exportPmx.right) / 2, (l.exportPmx.top + l.exportPmx.bottom) / 2}}, {"exportFormat", {(l.exportFormat.left + l.exportFormat.right) / 2, (l.exportFormat.top + l.exportFormat.bottom) / 2}}, {"resetClothing", {(l.clothingReset.left + l.clothingReset.right) / 2, (l.clothingReset.top + l.clothingReset.bottom) / 2}}, {"exportClothing", {(l.clothingExport.left + l.clothingExport.right) / 2, (l.clothingExport.top + l.clothingExport.bottom) / 2}}};
    std::shared_ptr<Loaded> L = Current();
    // The Optimize tab's controls (centre of each hit area), for scripted runs and the smoke test.
    if (L && g.tab == TabOptimize) {
        const OptLayout oo = OptLayoutFor(l.partsTop, g.optScroll);
        auto mid = [](const RECT& r) { return std::vector<int>{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; };
        nlohmann::json chips = nlohmann::json::array(), lodRows = nlohmann::json::array(), preview = nlohmann::json::array();
        for (int i = 0; i < 6; ++i) chips.push_back(mid(oo.chips[static_cast<size_t>(i)]));
        for (int i = 0; i < 4; ++i) lodRows.push_back(mid(oo.lodRows[static_cast<size_t>(i)]));
        for (int i = 0; i < 6; ++i) preview.push_back(mid(oo.preview[static_cast<size_t>(i)]));
        j["optimize"] = {{"presetChips", chips}, {"slider", mid(oo.slider)}, {"lodRows", lodRows}, {"generate", mid(oo.generate)},
                         {"preview", preview}, {"apply", mid(oo.apply)}, {"reset", mid(oo.reset)},
                         {"reduction", static_cast<int>(std::lround(g.opt.reduction * 100))}, {"applied", g.opt.applied.set},
                         {"previewLod", g.opt.previewLod}, {"simplifiedTris", g.opt.simplifiedTris}};
    } else {
        j["optimize"] = nullptr;
    }
    j["rows"] = nlohmann::json::array();
    if (L && g.tab != TabParts && g.tab != TabBones && g.tab != TabMaterials && g.tab != TabOptimize) {
        const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.partPos * kRowH)) + kRowH / 2;
            j["rows"].push_back({{"id", rows[i].id}, {"kind", rows[i].kind}, {"label", rows[i].label}, {"y", y >= l.partsTop && y < l.partsBottom ? y : -1}});
        }
    }
    // Every part row with geometry (and its gizmo chips), plus the gizmos themselves, so a scripted run can select any part
    // and drag an axis or a ring. `accessories` stays as the old subset for scripts that only drive accessory rows.
    j["accessories"] = nlohmann::json::array();
    j["gizmoParts"] = nlohmann::json::array();
    if (L && g.tab == TabParts && !L->NativePmx()) {
        const std::vector<PartRow> prows = BuildRows(*L);
        const int scroll = static_cast<int>(std::lround(g.partPos * kRowH));
        auto gizmoRow = [&](int pi, int i) {
            const auto& part = L->scene.parts[static_cast<size_t>(pi)];
            if (part.meshes.empty()) return nlohmann::json{};
            const int yTop = l.partsTop + static_cast<int>(i) * kRowH - scroll;
            const RECT chip = GizmoChipRect(yTop);
            const RECT rotChip = GizmoRotateChipRect(yTop);
            return nlohmann::json{{"part", pi}, {"id", part.id}, {"group", part.group}, {"y", yTop + kRowH / 2},
                                  {"chip", {(chip.left + chip.right) / 2, (chip.top + chip.bottom) / 2}},
                                  {"rotChip", {(rotChip.left + rotChip.right) / 2, (rotChip.top + rotChip.bottom) / 2}},
                                  {"inView", yTop >= l.partsTop && yTop + kRowH <= l.partsBottom}, {"sel", GizmoPartSelected() == pi}};
        };
        for (size_t i = 0; i < prows.size(); ++i) {
            const PartRow& r = prows[static_cast<size_t>(i)];
            if (r.part < 0) continue;
            const auto row = gizmoRow(r.part, static_cast<int>(i));
            if (!row.is_null()) j["gizmoParts"].push_back(row);
            if (L->scene.parts[static_cast<size_t>(r.part)].group == "Accessories" && !row.is_null()) j["accessories"].push_back(row);
        }
    }
    j["gizmo"] = nullptr;
    if (g.gizmoState.ox >= 0.0f) {
        j["gizmo"] = {{"origin", {g.gizmoState.ox + kPanel, g.gizmoState.oy}},
                      {"tipX", {g.gizmoState.tipX[0] + kPanel, g.gizmoState.tipX[1] + kPanel, g.gizmoState.tipX[2] + kPanel}},
                      {"tipY", {g.gizmoState.tipY[0], g.gizmoState.tipY[1], g.gizmoState.tipY[2]}},
                      {"axisLen", {g.gizmoState.axisLen[0], g.gizmoState.axisLen[1], g.gizmoState.axisLen[2]}}};
    }
    j["rotateGizmo"] = nullptr;   // likewise the rotate gizmo: a grab point and the quarter-turn point on each ring, for scripted drags
    if (g.rotGizmoState.ox >= 0.0f) {
        const float r = g.rotGizmoState.ringWorld;
        nlohmann::json a = nlohmann::json::array(), b = nlohmann::json::array();
        for (int i = 0; i < 3; ++i) {
            a.push_back({g.rotGizmoState.ox + r * g.rotGizmoState.uX[i] + kPanel, g.rotGizmoState.oy + r * g.rotGizmoState.uY[i]});
            b.push_back({g.rotGizmoState.ox + r * g.rotGizmoState.vX[i] + kPanel, g.rotGizmoState.oy + r * g.rotGizmoState.vY[i]});
        }
        j["rotateGizmo"] = {{"origin", {g.rotGizmoState.ox + kPanel, g.rotGizmoState.oy}},
                            {"ringA", a}, {"ringB", b},
                            {"minorPx", {g.rotGizmoState.minorPx[0], g.rotGizmoState.minorPx[1], g.rotGizmoState.minorPx[2]}},
                            {"ringPx", {g.rotGizmoState.ringPx[0], g.rotGizmoState.ringPx[1], g.rotGizmoState.ringPx[2]}}};
    }
    j["axesWidget"] = nullptr;   // the navigation widget: its centre and the six balls, for scripted clicks
    {
        const int vx = std::max(0, static_cast<int>(rc.right) - kPanel), vh = static_cast<int>(rc.bottom);
        if (vx >= 170 && vh >= 120) {
            const AxisWidgetLayout al = AxisWidgetBox(vx);
            nlohmann::json balls = nlohmann::json::array();
            for (int s = 0; s < 6; ++s) { float bx = 0, by = 0; AxisWidgetBallAt(al, s, bx, by); balls.push_back({bx + kPanel, by}); }
            j["axesWidget"] = {{"center", {al.cx + kPanel, al.cy}}, {"balls", balls}};
        }
    }
    j["materials"] = nlohmann::json::object();
    if (L && g.tab == TabMaterials) {
        // The side panel's Materials tab is a launcher: the filter, the list and the
        // button that opens the Material Workbench (its own section below).
        const std::vector<int> mrows = BuildMatRows(*L);
        j["materials"]["list"] = nlohmann::json::array();
        for (size_t i = 0; i < mrows.size(); ++i) {
            const int y = l.matListTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.matPos * kRowH)) + kRowH / 2;
            if (y >= l.matListTop && y < l.matListBottom)
                j["materials"]["list"].push_back({{"index", mrows[i]}, {"y", y}, {"name", L->scene.materials[static_cast<size_t>(mrows[i])].name}, {"sel", mrows[i] == g.selMaterial}});
        }
        j["materials"]["filterY"] = (l.matFilter.top + l.matFilter.bottom) / 2;
        j["materials"]["filterBtn"] = {(l.matFilter.left + l.matFilter.right) / 2, (l.matFilter.top + l.matFilter.bottom) / 2};
        j["materials"]["open"] = {(l.matOpen.left + l.matOpen.right) / 2, (l.matOpen.top + l.matOpen.bottom) / 2};
        j["materials"]["filterDropOpen"] = g.matFilterDropOpen;
        j["materials"]["sel"] = g.selMaterial;
        j["materials"]["filter"] = g.matFilter;
        j["materials"]["overrides"] = g.mat.OverrideCount();
        j["materials"]["undo"] = g.mat.undoStack.size();
        j["materials"]["redo"] = g.mat.redoStack.size();
    }
    j["workbench"] = nlohmann::json::object();
    DumpWorkbenchLayout(j["workbench"]);   // its own window, its own coordinates (empty while it has never been opened)
    j["bones"] = nlohmann::json::object();
    if (L && g.tab == TabBones) {
        const std::vector<BoneRow> rows = BuildBoneRows(L->scene);
        j["bones"]["list"] = nlohmann::json::array();
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.boneListTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.bonePos * kRowH)) + kRowH / 2;
            if (y >= l.boneListTop && y < l.boneListBottom) j["bones"]["list"].push_back({{"name", L->scene.bones[static_cast<size_t>(rows[i].bone)].name}, {"y", y}, {"arrowX", 16 + std::min(rows[i].depth, 12) * 10 + 4}});
        }
        j["bones"]["editTop"] = l.boneEditTop;
        j["bones"]["fieldY"] = nlohmann::json::array();
        for (int k = 0; k < kBoneEditCount; ++k) j["bones"]["fieldY"].push_back(l.boneEditTop + (1 + k) * kRowH + kRowH / 2);
        j["bones"]["resetY"] = l.boneEditTop + 10 * kRowH + kRowH / 2;
        j["bones"]["actionY"] = l.boneEditTop + 11 * kRowH + kRowH / 2;
        j["bones"]["switchY"] = l.partsTop + kRowH / 2;
    }
    j["view"] = nlohmann::json::object();
    if (L)
        for (const char* name : {"cf_j_hand_L", "cf_j_hand_R", "cf_j_forearm01_L", "cf_j_arm00_L", "cf_j_head", "cf_j_hips", "cf_j_leg01_L"}) {
            const int b = L->scene.boneIndex(name);
            if (b >= 0 && static_cast<size_t>(b) < g.boneScreen.size() && g.boneScreen[static_cast<size_t>(b)].first >= 0)
                j["view"][name] = {static_cast<int>(g.boneScreen[static_cast<size_t>(b)].first) + kPanel, static_cast<int>(g.boneScreen[static_cast<size_t>(b)].second)};
        }
    j["outfit"] = L ? L->outfit : -1;
    j["outfits"] = L ? L->outfits : 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        const ClothingPin* last = g.clothingPins.empty() ? nullptr : &g.clothingPins.back();
        j["clothingPath"] = last ? last->path : std::string();
        j["clothingName"] = ClothingDisplayName();
        j["clothingOutfit"] = last ? last->dstOutfit : -1;
        j["clothingMode"] = last ? last->mode : 0;
        j["selectiveDecisions"] = last && last->mode == 2 ? last->decisions.size() : 0;
        j["clothingPins"] = g.clothingPins.size();
    }
    j["clothingApplied"] = L ? L->clothing : std::string();
    j["state"] = {{"loaded", L != nullptr}, {"busy", g.busy.load()}, {"gpu", g.gpuOk}, {"physics", g.physics}, {"aPose", g.aPose}, {"showBones", g.showBones},
                  {"diagMode", g.gpu.DiagnosticMode()}, {"diagError", g.gpu.DiagnosticError()},
                  {"showExp", g.showExp}, {"expOk", g.expOk}, {"exporting", g.exporting.load()}, {"undo", g.undoStack.size()}, {"redo", g.redoStack.size()}, {"edits", static_cast<size_t>(std::count_if(g.boneEdits.begin(), g.boneEdits.end(), [](const auto& e) { return !assemble::IsIdentity(e.second); }))}, {"selBone", g.selBoneName}, {"selMaterial", g.selMaterial}, {"matUndo", g.mat.undoStack.size()}, {"matRedo", g.mat.redoStack.size()}, {"matOverrides", g.mat.OverrideCount()}, {"camYaw", g.cam.yawDeg}, {"camPitch", g.cam.pitchDeg}, {"camZoom", g.cam.zoom}, {"drag", g.drag}, {"selGizmoPart", g.selGizmoPart}, {"gizmoMode", g.gizmoMode}, {"partEdits", g.partEdits.size()}, {"deletedParts", g.deletedParts.size()}, {"axesHover", g.viewHoverBall}};
    if (L) j["state"]["bones"] = L->scene.bones.size();
    {   // the loading bar: title / stage / fraction so scripted runs can check it
        const ProgSnapshot ps = GetProgSnapshot();
        j["progress"] = {{"active", ps.active}, {"cancelRequested", ps.cancelRequested}, {"frac", ps.frac}, {"title", ps.title}, {"stage", ps.stage}};
    }
    const std::string text = j.dump(2);
    fileio::WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KILLFOCUS:
        g.shiftDown = false;   // the window cannot see the release: pretend the keys are up
        g.ctrlDown = false;
        return 0;
    case WM_TIMER:
        if (wp == kTimerApplyTweaks) {
            KillTimer(hwnd, kTimerApplyTweaks);
            if (g.sliderDrag >= 0) { ScheduleApplyTweaks(); return 0; }   // still dragging
            std::shared_ptr<Loaded> L = Current();
            if (g.busy) { ScheduleApplyTweaks(); return 0; }
            if (L && !g.cardPath.empty()) StartLoad(g.cardPath, L->outfit, true);
            return 0;
        }
        KillTimer(hwnd, 1); g.dirty = true; InvalidateView(); return 0;
    case WM_LOG: InvalidateLog(); return 0;
    case WM_BT_READY: BodyTransferReady(static_cast<int>(wp), static_cast<int>(lp)); return 0;
    case WM_ASSETS_READY:
        InvalidatePanel();
        if (wp) Log(std::string("asset library failed: ") + g.assets.error, LogKind::Error);
        return 0;
    case WM_ASSETS_FRAME:
        InvalidatePanel();
        return 0;
    case WM_PROGRESS: {
        // Keep the indeterminate chunk / percent moving: repaint the strip
        // under the toolbar and the overlay card, plus the footer hint.
        InvalidatePanelBand(Lay(ClientHeight()).open.bottom + 1, Lay(ClientHeight()).open.bottom + 12);
        InvalidateFooter();
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const RECT card = ProgOverlayRect(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
            InvalidateRect(hwnd, &card, FALSE);
        }
        return 0;
    }
    case WM_TEST: {
        static const bool hooks = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
        if (!hooks) return 0;
        if (wp == 1) UndoPose(false);
        else if (wp == 2) UndoPose(true);
        else if (wp == 3) SavePoseFile();
        else if (wp == 4) LoadPoseFile();
        else if (wp == 13) MaterialUndo(false);
        else if (wp == 14) MaterialUndo(true);
        else if (wp == 15) SaveMatFile();
        else if (wp == 16) LoadMatFile();
        else if (wp == 12) ResetClothing();   // reset the imported outfit to the card default
        else if (wp == 20) {   // open the Selective Import window for the file in CARDBRIDGE_SELECTIVE_SRC
            const char* src = std::getenv("CARDBRIDGE_SELECTIVE_SRC");
            if (src && *src) SelectiveOpen(Slashes(src));
        }
        else if (wp == 21) SelApply();   // apply (only reached through the validation guard)
        else if (wp == 22) SelectiveClose();   // close the selective import window
        else if (wp == 24) {   // show the drop-time import dialog for the file in CARDBRIDGE_SELECTIVE_SRC (the modal harness must Dismiss it)
            const char* src = std::getenv("CARDBRIDGE_SELECTIVE_SRC");
            if (src && *src) ShowCoordinateImportDialog(Slashes(src));
        }
        else if (wp == 10) DumpTestLayout();
        else if (wp == 11) {   // the nude body on, and the camera close on the chest (lp = pan up, in thousandths of the window height)
            if (std::shared_ptr<Loaded> L = Current())
                for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i) {
                    const auto& p = L->scene.parts[i];
                    if (p.nudeSwitch) g.hidden[i] = 0;
                    else if (p.group == "Outfit" || p.group == "Accessories") g.hidden[i] = 1;
                }
            g.cam = {};
            g.cam.zoom = 3.0f;
            g.cam.panY = static_cast<float>(lp) / 1000.0f;
            g.dirty = true;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {kPanel + 380, 620};
        return 0;
    }
    case WM_EXPORT_DONE: ProgEnd(); InvalidatePanel(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_OPT_READY: {
        if (wp == 1) Log("simplification cancelled", LogKind::Warn);
        else if (wp == 2) Log("simplification failed; the character is untouched", LogKind::Error);
        OptAdoptPending(static_cast<int>(wp));   // adopts g.optPending (published with the message)
        g.opt.busy = false;
        g.busy = false;
        ProgEnd();
        InvalidatePanel();
        InvalidateView();
        return 0;
    }
    case WM_EXP_READY: {
        if (static_cast<int>(lp) != g.expGen.load()) return 0;   // a stale bake must not disturb a newer one
        const bool wasCancelled = (wp == 1);
        std::shared_ptr<PendingExportUpload> up;
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            up = std::move(g.expUploadPending);
            g.expPending.reset();
            g.expPendingFor.reset();
        }
        g.expBusy = false;
        g.expOk = false;
        if (wasCancelled || !up || !up->ok || !up->view) {   // cancelled or failed: keep the previous look
            if (up && !up->ok && !up->why.empty()) Log("export preview upload failed: " + up->why, LogKind::Warn);
            g.expFor.reset();
            ProgEnd();
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (up->forCharacter != Current()) {   // the character changed while baking/uploading: drop the stale copy
            g.expFor.reset();
            ProgEnd();
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Adopt the worker-built GPU view. Its old counterpart is released here, on
        // the UI thread; no D3D11 object is used by two threads at the same time.
        g.gpuExp = std::move(*up->view);
        g.lastGpu = nullptr;
        g.expScene = std::move(up->scene);
        g.expFor = up->forCharacter;
        g.expGrade = up->grade;
        g.expOk = true;
        g.showExp = true;   // expFor == Current() was checked above
        ApplyPoseLive();
        MorphsOnExportViewLoaded();
        const size_t fb = g.gpuExp.fallbackMaterials();
        Log(std::string("as exported: on (X switches back)") + (fb ? "; " + std::to_string(fb) + " material(s) shade via their normal map - the game's shader did not upload" : ""), LogKind::Good);
        ProgEnd();
        g.dirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LOADED: {
        cbtime::Scope loadedScope("UI: WM_LOADED total");
        if (wp == 1) {   // cancelled: keep the previous character, just stop the bar
            g.loadingOutfit = -1;
            ProgEnd();
            g.status = Current() ? "loading cancelled" : "drop a Koikatsu card on this window";
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        std::shared_ptr<Loaded> L = Current();
        cbtime::Scope measureScope("UI: WM_LOADED measure + analyse");
        if (L && !L->scene.bones.empty()) {
            g.baseHeightCm = assemble::SceneHeightMeters(L->scene) * 100.0f;   // metres to centimetres
            g.heightCm = g.baseHeightCm;
            const float bustSize = g.tw.bodyCard.size() > 4 ? g.tw.bodyCard[4] : 0.5f;
            g.sagDefault = assemble::CardSagDefault(L->scene, bustSize);
            g.sag = g.sagDefault;   // follow the card: each card starts at its own hang
            UpdateBodyMeasurements();   // the measurements exist from the first frame, not only after an edit
        }
        g.hidden.assign(L ? L->scene.parts.size() : 0, 0);
        g.bt.nudePreview = false;   // a card load rebuilds the visibility state
        g.bt.savedHidden.clear();
        if (L && g.applyKeep)
            for (size_t i = 0; i < L->scene.parts.size(); ++i) {
                auto it = g.keepHidden.find(L->scene.parts[i].id);
                if (it != g.keepHidden.end()) g.hidden[i] = it->second;
            }
        if (L)
            for (size_t i = 0; i < L->scene.parts.size(); ++i)
                if (L->scene.parts[i].nudeSwitch && !(g.applyKeep && g.keepHidden.count(L->scene.parts[i].id))) g.hidden[i] = 1;   // starts unticked
        g.keepHidden.clear();
        // Proportion correction state: a different card starts faithful with fresh diagnostics; a same-card reload (an outfit switch or a re-apply) keeps the user's corrections.
        // A native PMX has no card and no game shape sliders, so that panel stays empty for it.
        if (L && L->NativePmx()) { g.propCardKey.clear(); g.propSugs.clear(); g.propFindings.clear(); }
        else if (L && L->card.path != g.propCardKey) {
            g.propCardKey = L->card.path;
            g.propSugs.clear();
            g.propFindings = assemble::AnalyzeCard(L->card);
        } else if (!L) {
            g.propCardKey.clear();
            g.propSugs.clear();
            g.propFindings.clear();
        }
        g.partScroll = 0;
        g.partPos = 0;
        g.optScroll = 0;   // a new character starts at the top of the Optimize tab
        g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
        g.loadingOutfit = -1;
        g.soloPart = -1;
        g.selGizmoPart = -1;   // the scene is new: the gizmo's part index means nothing now
        g.partEdits.clear();   // ... and neither do the old part ids
        g.deletedParts.clear();
        g.gizmoHoverAxis = g.gizmoAxis = -1;
        g.rotGizmoHoverAxis = g.rotGizmoAxis = -1;
        g.scaleGizmoHoverAxis = g.scaleGizmoAxis = -1;
        g.hoverAccChip = g.hoverRotChip = g.hoverScaleChip = g.hoverDeleteChip = -1;
        g.state = exporter::NewViewState();
        g.gpuOk = false;
        g.gpuScene.reset();
        measureScope.Stop();
        cbtime::Scope gpuResetScope("UI: WM_LOADED GPU reset");
        g.gpu.Unload();   // flush + release the previous card's GPU copy before reloading, so an in-flight skin dispatch cannot bleed into the new buffers
        g.showExp = g.expOk = false;   // the baked copy belongs to the previous character
        g.expFor.reset();
        // The baked "as exported" scene is a full second copy of the previous
        // character (textures included) and its GPU copy is another. It used
        // to stay resident after switching characters until the next X bake,
        // so each character left its copy behind. Release both now.
        g.expScene.reset();
        g.expPending.reset();
        g.expPendingFor.reset();
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            g.expUploadPending.reset();
            ++g.expGen;   // any bake/upload still in flight now belongs to the previous character
        }
        g.expBusy = false;
        g.gpuExp.Unload();
        // No pose or physics may carry over from the previous card (bone counts and names differ).
        g.skin.clear();
        g.boneWorld.clear();
        g.poseWorld.clear();
        g.sim = assemble::PhysicsSim{};
        // The Lit-view GPU upload is deliberately not done here: the viewer goes straight to the
        // "as exported" (MMD look) view on load, so paying to upload the game shaders would be dead weight.
        // The Lit view uploads on demand when the user presses X (toggle Lit back on), see UploadLitGpu.
        // Default the viewport to the "as exported" (MMD look) view — the same image the PMX export produces.
        // The bake runs in the background; WM_EXP_READY switches the view on once it is ready. X (the MMD button) toggles back.
        gpuResetScope.Stop();
        cbtime::Scope autoBakeScope("UI: WM_LOADED start export bake");
        if (L && g.gpu.ready()) ToggleAsExported();
        // Keep the loading bar alive while the bake is in flight - hiding it now would drop the user onto the
        // interactive view until the bake finished. WM_EXP_READY hides it (on success and on failure alike) and flips the view.
        if (g.expBusy.load()) ProgSet(0.98, "rendering the exported preview (MMD look)"); else ProgEnd();
        // The exported PMX's morphs, for the Morphs tab (all weights at 0 on a fresh character)
        {   // a new character: the cached optimized scenes and the applied export setting belonged to the previous one; the preset stays as a preference
            g.opt.applied = OptApplied{};
            g.opt.simplified.reset();
            g.opt.simplifiedFor = -1.0f;
            g.opt.simplifiedTris = g.opt.simplifiedVerts = 0;
            g.opt.simplifiedSeconds = 0.0;
            for (int i = 0; i < 4; ++i) g.opt.lods[static_cast<size_t>(i)] = OptLod{};
            g.opt.previewLod = 0;
            g.opt.stale = true;
        }
        autoBakeScope.Stop();
        cbtime::Scope morphScope("UI: WM_LOADED morph catalog");
        if (L) LoadMorphCatalog();
        else { g.morphCatalog = {}; g.morphWeights.clear(); g.morphDelta.clear(); g.gpu.SetMorphDelta(nullptr); g.gpuExp.SetMorphDelta(nullptr); }
        morphScope.Stop();
        if (!g.expBusy.load()) ProgEnd();   // only close the bar here when no exported-preview bake is in flight (WM_EXP_READY closes it)
        if (L && !g.applyKeep) { g.boneEdits.clear(); g.partEdits.clear(); g.deletedParts.clear(); g.undoStack.clear(); g.redoStack.clear(); g.selBoneName.clear(); g.boneScroll = 0; g.bonePos = 0; SeedOpenBones(L->scene); }
        else if (L && g.openBones.empty()) SeedOpenBones(L->scene);
        // Materials: capture a fresh baseline for the new character.
        if (L) {
            material::CaptureOriginals(g.mat, L->scene);
            g.selMaterial = L->scene.materials.empty() ? -1 : 0;
            g.matUvTrisCache = -2;
            g.matFilter = -1;
            g.matScroll = 0; g.matPos = 0;
            for (int k = 0; k < kMatCount; ++k) g.matBuf[k] = 0;
            g.matHexEditing = false; g.matHexBuf.clear();
            g.matClassDropOpen = false; g.matFilterDropOpen = false; g.matPickerOpen = false; g.matPickerDrag = 0; g.matDropHover = -1;
            g.matInspectOpen = false; g.matTexSlotDropOpen = false; g.matUvDrag = 0; g.matUvCacheMat = -2; g.matTexSlot = -1; g.uvScale = 0;
            SyncMatBuf();
        }
        if (g.physics) { g.physics = false; SetPhysics(true); }
        else ApplyPoseLive();
        if (L && !g.wantPose.empty()) { const std::string pose = g.wantPose; g.wantPose.clear(); LoadPoseFile(pose); }
        g.dirty = true;
        if (L) SetWindowTextW(hwnd, Utf8ToWide("CardBridge " CARDBRIDGE_VERSION " - " + (L->title.empty() ? g.cardPath : L->title)).c_str());
        InvalidateRect(hwnd, nullptr, FALSE);
        static bool benchStarted = false;
        if (L && !L->scene.parts.empty() && !benchStarted && std::getenv("CARDBRIDGE_BENCH")) { benchStarted = true; PostMessageW(hwnd, WM_BENCH, 0, 0); }
        return 0;
    }
    case WM_BENCH: RunBench(); return 0;
    case WM_SETUP_LIBRARY: SetupLibrary(false); return 0;
    case WM_LIBRARY_DONE:
        ProgEnd();
        if (lp == 2) {   // cancelled: the library stays unfinished; start again to finish
            g.status = "library setup cancelled: press L to try again";
        } else if (lp) {
            g.settings.library = DefaultLibraryDir();
            const auto imported = library::ReadGameInfo(g.settings.library);
            if (imported.game != library::SourceGame::Unknown) {
                g.settings.activeProfile = ProfileKey(imported.game);
                Log("library built for " + std::string(ProfileLabel(g.settings.activeProfile)), LogKind::Good);
            }
            if (!g.cliLibraryOverride && !g.cliModsOverride) {
                StoreActiveProfile();
                SaveSettings(g.settings);
            }
            g.resolver.reset();   // the next load builds its resolver over the new library
            if (exporter::LoadColourGrade(g.settings.library)) Log("colour grading loaded (G toggles it)", LogKind::Good);
            g.status = "drop a Koikatsu card on this window";
            if (!g.pendingCard.empty()) { const std::string card = g.pendingCard; g.pendingCard.clear(); StartLoad(card, g.pendingOutfit); }
            else if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
        } else {
            g.status = "library setup failed: press L to try again";
        }
        InvalidatePanel();
        return 0;
    case WM_DROPFILES: {
        wchar_t buf[1024];
        if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, buf, 1024)) {
            const std::string dropped = Slashes(WideToUtf8(buf));
            if (IsPmxPath(dropped)) {
                g.cam = {};
                {
                    std::lock_guard<std::recursive_mutex> lk(g.mu);
                    g.clothingPins.clear();
                }
                StartLoadPmx(dropped);
                DragFinish(reinterpret_cast<HDROP>(wp));
                return 0;
            }
            const card::ImageSourceKind sourceKind = card::DetectImageSource(dropped);
            if (sourceKind == card::ImageSourceKind::RegularClothing) {
                // A Koikatsu Coordinate (outfit) file: always ask how it goes onto the outfit on screen.
                ShowCoordinateImportDialog(dropped);
            } else {
                // Nothing on screen (open the card, as always), or a source we do not import:
                // fall back to the plain drop behavior (open as character), never to clothing import.
                g.cam = {};
                {
                    std::lock_guard<std::recursive_mutex> lk(g.mu);
                    g.clothingPins.clear();
                }
                StartLoad(dropped, -1);
            }
        }
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (g.morphEdit >= 0) EndMorphEdit(true);   // any click commits the rename box
        if (g.sliderEdit >= 0) EndSliderEdit(true);   // any click commits the typed value (Shift+click a slider to type)
        g.undoBase = CapturePose();
        g.undoBaseValid = true;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.matHexEditing && x >= kPanel) { g.matHexEditing = false; g.matHexUndoPushed = false; }
        if (x >= kPanel && GetProgSnapshot().active) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const RECT cancel = ProgCancelRect(ProgOverlayRect(static_cast<int>(rc.right), rc.bottom));
            POINT p{x, y};
            if (PtInRect(&cancel, p)) { ProgRequestCancel(); return 0; }
        }
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            int button = ButtonAt(l, x, y);
            if (button == 1) { OpenCard(); return 0; }
            if (button == 2) { ExportFolderIfNeeded(); return 0; }
            if (button == 61) {
                const exporter::ExportFormat next = static_cast<exporter::ExportFormat>((static_cast<int>(ViewerExportFormat()) + 1) % 4);
                g.settings.exportFormat = exporter::FormatName(next);
                SaveSettings(g.settings);
                Log(std::string("export format: ") + exporter::FormatName(next) +
                    (next == exporter::ExportFormat::Pmx ? "" : " (work in progress)"), LogKind::Info);
                InvalidatePanel();
                return 0;
            }
            if (button == 3) { if (Current()) ToggleIssues(); return 0; }
            if (button == 4) { if (Current()) ResetView(0); return 0; }
            if (button == 5) { if (Current()) ShowAll(); return 0; }
            if (button == 6) { ToggleAsExported(); InvalidateRect(hwnd, nullptr, FALSE); InvalidatePanel(); return 0; }
            if (button == 7) { ResetClothing(); return 0; }
            if (button == 8) { MoveClothing(-1); return 0; }
            if (button == 9) { MoveClothing(1); return 0; }
            if (button == 21) { StartExportClothing(); return 0; }
            if (button == 60 && !g.matFilterDropOpen) { OpenWorkbench(); return 0; }   // the Materials tab's launcher
            if (button >= 10 && button < 10 + TabCount && button != 22) {
                g.showIssues = false;
                g.tab = button - 10;
                if (g.tab == TabAssets) EnsureAssetCatalog();
                g.partScroll = 0;
                g.partPos = 0;
                g.hoverRow = g.hoverPart = -1;
                InvalidatePanel();
                return 0;
            }
            if (g.tab == TabAssets) { AssetsTabClick(x, y, l); return 0; }
            int chip = OutfitChipAt(l, x, y);
            if (chip >= 0) { ChooseOutfit(chip); return 0; }
            POINT p{x, y};
            if (PtInRect(&l.clearLog, p)) { ClearLog(); return 0; }
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) { BonesTabClick(x, y, l, false); return 0; }
            if (L && !g.showIssues && g.tab == TabMaterials) { MaterialsTabClick(x, y, l, false); return 0; }
            if (L && !g.showIssues && g.tab == TabOptimize) { OptTabClick(x, y, false); return 0; }
            const bool inList = y >= l.partsTop && y < l.partsBottom;
            if (inList) {   // Parts and the slider tabs: a press in the list, not a completed click yet
                if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) {
                    const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
                    const int idx = RowIndexAt(rows.size(), l, x, y);
                    if (idx >= 0 && rows[static_cast<size_t>(idx)].kind == 1) {   // a slider row: the press drags the slider (mousedown applies the value)
                        TweakClick(x, y, l, false);
                        // the press is captured: a drag sideways drags the slider, a vertical drag scrolls the list
                        g.listPressX = x; g.listPressY = y; g.listPressPos = g.partPos; g.listScroll = false;
                        g.hoverRow = -1;
                        SetCapture(hwnd);
                        return 0;
                    }
                    g.listPressX = x; g.listPressY = y; g.listPressPos = g.partPos; g.listScroll = false;   // a plain row: release or drag decides
                    g.hoverRow = -1;
                    SetCapture(hwnd);
                    return 0;
                }
                if (L && !g.showIssues && g.tab == TabParts) {
                    g.listPressX = x; g.listPressY = y; g.listPressPos = g.partPos; g.listScroll = false;
                    g.hoverRow = -1;
                    g.hoverPart = -1;
                    SetCapture(hwnd);
                    return 0;
                }
            }
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, false); return 0; }
            return 0;
        }
        if (x >= kPanel) {   // the navigation widget in the top-right corner wins over the camera (a click there snaps the view, a drag orbits it)
            RECT rc;
            GetClientRect(hwnd, &rc);
            const AxisWidgetLayout al = AxisWidgetBox(std::max(0, static_cast<int>(rc.right) - kPanel));
            if (AxisWidgetWithin(al, float(x - kPanel), float(y))) {
                g.drag = 5; g.last = {x, y}; g.downAt = {x, y}; g.interacting = true; SetCapture(hwnd);
                return 0;
            }
        }
        {   // The diagnostic mode strip over the viewport (spec S13): clicks there do not orbit the camera
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int W = std::max(0, static_cast<int>(rc.right) - kPanel), H = rc.bottom;
            if (ModeStripClick(x, y, kPanel, W, H)) return 0;
        }
        if (g.transBrush && x >= kPanel) {   // the paint mode wins over the camera, the gizmo and the bones
            TransPaintPress(x - kPanel, y, false);
            return 0;
        }
        if (g.selGizmoPart >= 0 && PartGizmoDrawn()) {   // the gizmo wins over the camera and the bones
            const int axis = PickPartGizmoAxis(float(x - kPanel), float(y), 12.0f);
            if (axis >= 0) {
                g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
                if (g.gizmoMode == 0) {
                    g.gizmoAxis = axis;
                    g.gizmoAcc[0] = g.gizmoAcc[1] = g.gizmoAcc[2] = 0.0f;
                    g.gizmoDragDx = g.gizmoDragDy = 0.0f;
                    g.drag = 4;
                } else if (g.gizmoMode == 1) {
                    g.rotGizmoAxis = axis;
                    g.rotGizmoAcc[0] = g.rotGizmoAcc[1] = g.rotGizmoAcc[2] = 0.0f;
                    g.rotGizmoPtrX = float(x - kPanel); g.rotGizmoPtrY = float(y);
                    exporter::RotateGizmoPointerAngle(g.rotGizmoState, axis, g.rotGizmoPtrX, g.rotGizmoPtrY, g.rotGizmoTheta);   // if it fails, the drag just holds the edit
                    g.drag = 7;
                } else {
                    g.scaleGizmoAxis = axis;
                    g.scaleGizmoAcc[0] = g.scaleGizmoAcc[1] = g.scaleGizmoAcc[2] = 1.0f;
                    g.scaleGizmoDragDx = g.scaleGizmoDragDy = 0.0f;
                    g.drag = 8;
                }
                InvalidateView();
                return 0;
            }
        }
        if (g.showBones && !g.boneScreen.empty()) {   // a bone under the pointer is grabbed: dragging turns it, Ctrl+drag moves it
            const int hit = exporter::PickBone(g.boneScreen, static_cast<float>(x - kPanel), static_cast<float>(y), 12.0f);
            if (std::shared_ptr<Loaded> L = Current(); L && hit >= 0) {
                g.selBoneName = L->scene.bones[static_cast<size_t>(hit)].name;
                g.dragBone = hit;
                g.dragMove = g.ctrlDown;
                g.drag = 3; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
                InvalidatePanel();
                return 0;
            }
        }
        g.drag = 1; g.last = {x, y}; g.downAt = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (g.morphEdit >= 0) { EndMorphEdit(false); return 0; }   // right-click puts the typed name back and is swallowed
        if (g.sliderEdit >= 0) { EndSliderEdit(false); return 0; }   // right-click puts the typed value back and is swallowed
        g.undoBase = CapturePose();
        g.undoBaseValid = true;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) { BonesTabClick(x, y, l, true); return 0; }
            if (L && !g.showIssues && g.tab == TabMaterials) { MaterialsTabClick(x, y, l, true); return 0; }
            if (L && !g.showIssues && g.tab == TabOptimize) { OptTabClick(x, y, true); return 0; }
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, true); return 0; }
            if (L && !g.showIssues && !L->NativePmx()) {
                std::vector<PartRow> rows = BuildRows(*L);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0 && rows[static_cast<size_t>(idx)].part >= 0) {
                    int part = rows[static_cast<size_t>(idx)].part;
                    if (!L->scene.parts[static_cast<size_t>(part)].meshes.empty()) {
                        const int ry = l.partsTop + idx * kRowH - ScrollPx(g.partPos);
                        const POINT cp{x, y};
                        const RECT chip = GizmoChipRect(ry);
                        if (PtInRect(&chip, cp)) { ResetPartMove(part); g.dirty = true; InvalidateView(); InvalidatePanel(); InvalidateFooter(); return 0; }   // right-click the move chip resets the move
                        const RECT rotChip = GizmoRotateChipRect(ry);
                        if (PtInRect(&rotChip, cp)) { ResetPartRot(part); g.dirty = true; InvalidateView(); InvalidatePanel(); InvalidateFooter(); return 0; }   // right-click the rotate chip resets the rotation
                        const RECT scaleChip = GizmoScaleChipRect(ry);
                        if (PtInRect(&scaleChip, cp)) { ResetPartScale(part); g.dirty = true; InvalidateView(); InvalidatePanel(); InvalidateFooter(); return 0; }   // right-click the resize chip resets the scale
                    }
                    if (g.soloPart == part) {
                        g.hidden.assign(g.hidden.size(), 0);
                        g.soloPart = -1;
                        Log("showing every part");
                    } else {
                        g.hidden.assign(g.hidden.size(), 1);
                        if (static_cast<size_t>(part) < g.hidden.size()) g.hidden[static_cast<size_t>(part)] = 0;
                        g.soloPart = part;
                        Log("solo: " + L->scene.parts[static_cast<size_t>(part)].id);
                    }
                    // Solo changes which shoes are on screen: keep the heel pose honest.
                    for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i)
                        if (L->scene.parts[i].id.find(".shoes") != std::string::npos)
                            L->scene.parts[i].visibleInPreview = !g.hidden[i];
                    ReshapeNow(true);
                    g.dirty = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (g.transBrush) {   // right-drag paints the eraser (middle-mouse still pans)
            TransPaintPress(x - kPanel, y, true);
            return 0;
        }
        g.drag = 2; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_MBUTTONDOWN:
        g.drag = 2; g.last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; g.interacting = true; SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.listPressY >= 0) {   // a press in the list: a vertical drag scrolls it (the row's click is swallowed by the drag)
            const int dy = y - g.listPressY, dx = x - g.listPressX;
            if (!g.listScroll && std::abs(dy) > 4 && std::abs(dy) >= std::abs(dx)) {
                g.listScroll = true;
                g.sliderDrag = -1;   // a slider's press that became a vertical gesture turns into a list scroll
                g.dragTrackL = g.dragTrackR = 0;
            }
            if (g.listScroll) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                const PanelLayout l = Lay(rc.bottom);
                const int rows = ListRowCount();
                const int visible = VisibleRows(l);
                const float maxScroll = static_cast<float>(std::max(0, rows - visible));
                g.partScroll = static_cast<int>(std::clamp(g.listPressPos - dy / static_cast<float>(kRowH), 0.0f, maxScroll));
                g.partPos = static_cast<float>(g.partScroll);
                g.hoverRow = -1;
                InvalidateList();
                return 0;
            }
            if (g.opt.dragging) { OptSliderMouseMove(x); return 0; }
            if (g.sliderDrag >= 0) { g.dragX = x; g.dragPending = true; return 0; }
            return 0;   // pressed, no motion yet
        }
        if (g.opt.dragging) { OptSliderMouseMove(x); return 0; }   // the wide reduction slider: live value
        if (g.sliderDrag >= 0) { g.dragX = x; g.dragPending = true; return 0; }   // applied by Frame, once per picture
        // The filter dropdown is the only popup this window owns; the class dropdown,
        // the colour picker and the UV canvas live in the Material Workbench window.
        if (g.matFilterDropOpen && !g.matClassDropOpen && !g.matPickerOpen && !g.matTexSlotDropOpen &&
            x < kPanel && g.tab == TabMaterials && !g.showIssues) {
            const PanelLayout l = Lay(ClientHeight());
            const RECT drop = MatFilterDropRect(l.matFilter.left, l.matFilter.top, l.matFilter.right - l.matFilter.left);
            const POINT hp{x, y};
            const int nh = PtInRect(&drop, hp) ? (y - drop.top) / kRowH : -1;
            if (nh != g.matDropHover) {
                g.matDropHover = nh;
                InvalidatePanelBand(drop.top, drop.bottom);
            }
            return 0;
        }
        if (g.drag == 3) {
            g.boneDragDx += static_cast<float>(x - g.last.x);
            g.boneDragDy += static_cast<float>(y - g.last.y);
            g.last = {x, y};
            g.dragPending = true;
            return 0;
        }
        if (g.drag == 6) {   // the paint stroke: the position, applied once per picture
            g.transPaintX = x - kPanel; g.transPaintY = y;
            g.transPaintPending = true;
            g.dragPending = true;
            g.lastInput = GetTickCount64();
            {   // the ring follows the pointer even mid-stroke
                RECT rc2;
                GetClientRect(hwnd, &rc2);
                const int newX = (g.transBrush && x >= kPanel && x < rc2.right && y >= 0 && y < rc2.bottom) ? x - kPanel : -1;
                if (newX != g.transHoverX || y != g.transHoverY) {
                    g.transHoverX = newX;
                    g.transHoverY = (newX >= 0) ? y : -1;
                    g.dirty = true;
                    InvalidateView();
                }
            }
            return 0;
        }
        if (g.drag == 5) {   // the navigation widget: a click candidate until the pointer moves; then it is simply an orbit
            if (std::hypot(static_cast<double>(x - g.downAt.x), static_cast<double>(y - g.downAt.y)) > 3.0) g.drag = 1;
            else return 0;   // the view stays still until the gesture is known
        }
        if (g.drag == 4) {
            g.gizmoDragDx += static_cast<float>(x - g.last.x);
            g.gizmoDragDy += static_cast<float>(y - g.last.y);
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dragPending = true;
            return 0;
        }
        if (g.drag == 7) {   // a ring drag: the angle comes from the absolute pointer position, not the frame's delta
            g.rotGizmoPtrX = float(x - kPanel); g.rotGizmoPtrY = float(y);
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dragPending = true;
            return 0;
        }
        if (g.drag == 8) {
            g.scaleGizmoDragDx += static_cast<float>(x - g.last.x);
            g.scaleGizmoDragDy += static_cast<float>(y - g.last.y);
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dragPending = true;
            return 0;
        }
        if (g.drag) {
            float dx = static_cast<float>(x - g.last.x), dy = static_cast<float>(y - g.last.y);
            if (g.drag == 1) { g.cam.yawDeg -= dx * 0.5f; g.cam.pitchDeg = std::max(-89.0f, std::min(89.0f, g.cam.pitchDeg - dy * 0.5f)); }
            else { RECT rc; GetClientRect(hwnd, &rc); g.cam.panX += dx / rc.bottom; g.cam.panY += dy / rc.bottom; }
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dirty = true;
            InvalidateView();
            return 0;
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        {   // the brush ring follows the pointer (its own little invalidation; the view repaints as usual otherwise)
            RECT rc3;
            GetClientRect(hwnd, &rc3);
            const int newX = (g.transBrush && x >= kPanel && x < rc3.right && y >= 0 && y < rc3.bottom) ? x - kPanel : -1;
            const int newY = (newX >= 0) ? y : -1;
            if (newX != g.transHoverX || newY != g.transHoverY) {
                g.transHoverX = newX;
                g.transHoverY = newY;
                g.dirty = true;
                InvalidateView();
            }
        }
        RECT rc;
        GetClientRect(hwnd, &rc);
        PanelLayout l = Lay(rc.bottom);
        int oldRow = g.hoverRow, oldPart = g.hoverPart, oldButton = g.hoverButton, oldOutfit = g.hoverOutfit;
        int hoverBone = -1;   // the pointer over a bone of the overlay in the view, or over its row in the tree
        if (x >= kPanel && g.showBones) hoverBone = exporter::PickBone(g.boneScreen, static_cast<float>(x - kPanel), static_cast<float>(y), 12.0f);
        else if (x < kPanel && g.tab == TabBones && y >= l.boneListTop && y < l.boneListBottom)
            if (std::shared_ptr<Loaded> LL = Current()) {
                const std::vector<BoneRow> rows = BuildBoneRows(LL->scene);
                const int ri = (y - l.boneListTop + ScrollPx(g.bonePos)) / kRowH;
                if (ri >= 0 && static_cast<size_t>(ri) < rows.size()) hoverBone = rows[static_cast<size_t>(ri)].bone;
            }
        int hoverAcc = -1, hoverRot = -1, hoverScale = -1, hoverDelete = -1;   // the row of the part chips under the pointer, -1 = none
        {
            const int oldChip = g.hoverAccChip, oldRotChip = g.hoverRotChip, oldScaleChip = g.hoverScaleChip, oldDeleteChip = g.hoverDeleteChip;
            if (x < kPanel && g.tab == TabParts && !g.showIssues)
                if (std::shared_ptr<Loaded> LL = Current(); LL && !LL->NativePmx()) {
                    std::vector<PartRow> rows = BuildRows(*LL);
                    int idx = RowAt(rows, l, x, y);
                    if (idx >= 0) {
                        const PartRow& rr = rows[static_cast<size_t>(idx)];
                        if (rr.part >= 0) {
                            const int ry = l.partsTop + idx * kRowH - ScrollPx(g.partPos);
                            const POINT cp{x, y};
                            const RECT delChip = DeleteChipRect(ry);
                            if (PtInRect(&delChip, cp)) hoverDelete = idx;
                            if (!LL->scene.parts[static_cast<size_t>(rr.part)].meshes.empty()) {
                                const RECT chip = GizmoChipRect(ry);
                                if (PtInRect(&chip, cp)) hoverAcc = idx;
                                const RECT rotChip = GizmoRotateChipRect(ry);
                                if (PtInRect(&rotChip, cp)) hoverRot = idx;
                                const RECT scaleChip = GizmoScaleChipRect(ry);
                                if (PtInRect(&scaleChip, cp)) hoverScale = idx;
                            }
                        }
                    }
                }
            g.hoverAccChip = hoverAcc;
            g.hoverRotChip = hoverRot;
            g.hoverScaleChip = hoverScale;
            g.hoverDeleteChip = hoverDelete;
            if (oldChip != g.hoverAccChip || oldRotChip != g.hoverRotChip || oldScaleChip != g.hoverScaleChip || oldDeleteChip != g.hoverDeleteChip) {
                const int px = ScrollPx(g.partPos);
                for (int idx : {oldChip, g.hoverAccChip, oldRotChip, g.hoverRotChip, oldScaleChip, g.hoverScaleChip, oldDeleteChip, g.hoverDeleteChip})
                    if (idx >= 0) InvalidatePanelBand(std::max(l.partsTop, l.partsTop + idx * kRowH - px), std::min(l.partsBottom, l.partsTop + (idx + 1) * kRowH - px));
            }
        }
        if (x >= kPanel) {   // the navigation widget's hover highlight
            RECT rc2;
            GetClientRect(hwnd, &rc2);
            const int vw = std::max(0, static_cast<int>(rc2.right) - kPanel), vh = static_cast<int>(rc2.bottom);
            const AxisWidgetLayout al = AxisWidgetBox(vw);
            const int hoverBall = AxisWidgetPick(al, float(x - kPanel), float(y));
            if (hoverBall != g.viewHoverBall) { g.viewHoverBall = hoverBall; g.dirty = true; InvalidateView(); }
        } else if (g.viewHoverBall != -1) { g.viewHoverBall = -1; g.dirty = true; InvalidateView(); }
        if (x >= kPanel) {   // the gizmo's axis under the pointer lights up (whichever mode's gizmo is in the view)
            const int hoverAxis = (g.selGizmoPart >= 0 && PartGizmoDrawn()) ? PickPartGizmoAxis(float(x - kPanel), float(y), 10.0f) : -1;
            if (g.gizmoMode == 0) {
                if (hoverAxis != g.gizmoHoverAxis) { g.gizmoHoverAxis = hoverAxis; g.dirty = true; InvalidateView(); }
            } else if (g.gizmoMode == 1) {
                if (hoverAxis != g.rotGizmoHoverAxis) { g.rotGizmoHoverAxis = hoverAxis; g.dirty = true; InvalidateView(); }
            } else {
                if (hoverAxis != g.scaleGizmoHoverAxis) { g.scaleGizmoHoverAxis = hoverAxis; g.dirty = true; InvalidateView(); }
            }
        }
        if (hoverBone != g.hoverBone) {
            g.hoverBone = hoverBone;
            if (g.showBones) { g.dirty = true; InvalidateView(); }
        }
        g.hoverButton = x < kPanel ? ButtonAt(l, x, y) : 0;
        g.hoverOutfit = x < kPanel ? OutfitChipAt(l, x, y) : -1;
        const RECT cardNow = ProgOverlayRect(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
        const RECT cancelNow = ProgCancelRect(cardNow);
        const bool hoverCancel = x >= kPanel && GetProgSnapshot().active && PtInRect(&cancelNow, POINT{x, y}) != 0;
        if (hoverCancel != g.progHoverCancel) {
            g.progHoverCancel = hoverCancel;
            InvalidateRect(hwnd, &cardNow, FALSE);
        }
        g.hoverRow = -1;
        g.hoverPart = -1;
        if (x < kPanel) {
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) {
                if (y >= l.boneListTop && y < l.boneListBottom) g.hoverRow = (y - l.boneListTop + ScrollPx(g.bonePos)) / kRowH;
            } else if (L && !g.showIssues && g.tab == TabMaterials) {
                if (y >= l.matListTop && y < l.matListBottom) g.hoverRow = (y - l.matListTop + ScrollPx(g.matPos)) / kRowH;
            } else if (L && !g.showIssues && g.tab == TabOptimize) {
                OptTabHover(x, y);
            } else if (g.tab == TabAssets) {
                const int idx = (y >= l.assetListTop && y < l.assetListBottom)
                              ? static_cast<int>((y - l.assetListTop + std::lround(g.assets.listPos * kRowH)) / kRowH) : -1;
                g.hoverRow = idx;
            } else if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts && g.tab != TabOptimize) {
                g.hoverRow = RowIndexAt(BuildTweakRows(g.tab).size(), l, x, y);
            } else if (L && !g.showIssues && L->NativePmx()) {
                g.hoverRow = RowIndexAt(BuildExplorerRows(*L).size(), l, x, y);
            } else if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(*L);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0) {
                    g.hoverRow = idx;
                    if (rows[static_cast<size_t>(idx)].part >= 0) g.hoverPart = rows[static_cast<size_t>(idx)].part;
                }
            }
        }
        // Repaint only what changed: the old and new row, the buttons band, the footer's hint.
        if (oldRow != g.hoverRow) {
            const bool bones = g.tab == TabBones;
            const bool mats = g.tab == TabMaterials;
            const int top = bones ? l.boneListTop : mats ? l.matListTop : l.partsTop, bottom = bones ? l.boneListBottom : mats ? l.matListBottom : l.partsBottom;
            const int px = ScrollPx(bones ? g.bonePos : mats ? g.matPos : g.partPos);
            for (int idx : {oldRow, g.hoverRow})
                if (idx >= 0) InvalidatePanelBand(std::max(top, top + idx * kRowH - px), std::min(bottom, top + (idx + 1) * kRowH - px));
        }
        if (oldButton != g.hoverButton || oldOutfit != g.hoverOutfit) {
            InvalidatePanelBand(l.open.top - 2, l.tabs[TabCount - 1].bottom + 2);
            if (oldButton == 60 || g.hoverButton == 60) InvalidatePanelBand(l.matFilter.top - 2, l.matListTop - 2);   // the launcher button
            if (oldButton == 22 || g.hoverButton == 22) InvalidateLog();   // the log's clear button
        }
        // The viewport's diagnostic mode strip: hover highlight (repaint only on change).
        {
            RECT rc2;
            GetClientRect(hwnd, &rc2);
            const int W = std::max(0, static_cast<int>(rc2.right) - kPanel), H = rc2.bottom;
            const int oldCell = g.viewHoverCell;
            g.viewHoverCell = x >= kPanel ? ModeStripCellIndex(x, y, kPanel, W, H) : -1;
            if (oldCell != g.viewHoverCell) InvalidateView();
        }
        if (oldPart != g.hoverPart) {
            g.lastInput = GetTickCount64();
            g.dirty = true;
            InvalidateView();
            InvalidateFooter();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.hoverBone >= 0) { g.hoverBone = -1; g.dirty = true; InvalidateView(); }
        if (g.transHoverX >= 0) { g.transHoverX = -1; g.transHoverY = -1; g.dirty = true; InvalidateView(); }
        if (g.selGizmoPart >= 0) { g.gizmoHoverAxis = g.rotGizmoHoverAxis = g.scaleGizmoHoverAxis = -1; g.dirty = true; InvalidateView(); }
        if (g.hoverAccChip >= 0 || g.hoverRotChip >= 0 || g.hoverScaleChip >= 0 || g.hoverDeleteChip >= 0) {
            g.hoverAccChip = g.hoverRotChip = g.hoverScaleChip = g.hoverDeleteChip = -1; InvalidatePanel();
        }
        if (g.viewHoverBall >= 0) { g.viewHoverBall = -1; g.dirty = true; InvalidateView(); }
        if (g.matDropHover >= 0) { g.matDropHover = -1; InvalidatePanel(); }
        if (g.hoverRow >= 0 || g.hoverPart >= 0 || g.hoverButton || g.hoverOutfit >= 0 || g.viewHoverCell >= 0) {
            bool part = g.hoverPart >= 0;
            g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
            g.hoverButton = 0;
            if (g.viewHoverCell >= 0) { g.viewHoverCell = -1; InvalidateView(); }
            if (part) { g.dirty = true; InvalidateView(); }
            InvalidatePanel();
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (g.listScroll) {   // the list was dragged: the row's click is swallowed by the drag
            g.listScroll = false;
            g.listPressY = -1;
            RECT rc;
            GetClientRect(hwnd, &rc);
            const PanelLayout l = Lay(rc.bottom);
            g.partScroll = std::clamp(g.partScroll, 0, std::max(0, ListRowCount() - VisibleRows(l)));
            g.partPos = static_cast<float>(g.partScroll);
            ReleaseCapture();
            InvalidateList();
            return 0;
        }
        if (g.listPressY >= 0 && g.sliderDrag >= 0) {   // a slider's press: keep it a slider drag here
            g.listPressY = -1;
        } else if (g.listPressY >= 0) {   // a release in the list without a drag: the row does its thing (what the mouse-down used to do)
            const int px = g.listPressX, py = g.listPressY;
            g.listPressY = -1;
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelListClick(px, py, Lay(rc.bottom));
            ReleaseCapture();
            return 0;
        }
        ApplyPending();   // the last movement of the drag counts
        CommitPoseEdit();
        if (g.opt.dragging) { OptSliderRelease(); return 0; }
        if (g.sliderDrag >= 0) {
            SliderRef r;
            const bool live = IsLiveId(g.sliderDrag);
            if (!live && SliderFor(g.sliderDrag, r)) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "slider %d set to %.3f (card %.3f)", g.sliderDrag, r.Value(), r.def);
                Log(msg);
            }
            const int released = g.sliderDrag;
            g.sliderDrag = -1;
            g.dragTrackL = g.dragTrackR = 0;
            ReleaseCapture();
            TweakEdited(released, true);
            InvalidateList();   // the group's "reset" mark follows the sliders
            return 0;
        }
        if (g.drag == 5) {   // a click on the navigation widget: snap the view to the ball's axis
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            const AxisWidgetLayout al = AxisWidgetBox(std::max(0, static_cast<int>(rc.right) - kPanel));
            const int side = AxisWidgetPick(al, float(mx - kPanel), float(my));
            if (side >= 0) AxisWidgetSnapView(side);
            g.drag = 0; g.interacting = false; ReleaseCapture();
            g.dirty = true; InvalidateView();
            return 0;
        }
        if (g.drag == 4 || g.drag == 7 || g.drag == 8) {
            if (std::shared_ptr<Loaded> LL = Current(); LL) {
                const int pi = GizmoPartSelected();
                if (g.drag == 4) {
                    if (g.gizmoAcc[0] != 0.0f || g.gizmoAcc[1] != 0.0f || g.gizmoAcc[2] != 0.0f) {
                        char msg[192];
                        std::snprintf(msg, sizeof msg, "part %s moved by %+.1f / %+.1f / %+.1f cm (X/Y/Z)", pi >= 0 ? LL->scene.parts[static_cast<size_t>(pi)].id.c_str() : "?",
                                      static_cast<double>(g.gizmoAcc[0]) * 100.0, static_cast<double>(g.gizmoAcc[1]) * 100.0, static_cast<double>(g.gizmoAcc[2]) * 100.0);
                        Log(msg);
                    }
                } else if (g.drag == 7) {
                    if (g.rotGizmoAcc[0] != 0.0f || g.rotGizmoAcc[1] != 0.0f || g.rotGizmoAcc[2] != 0.0f) {
                        char msg[192];
                        std::snprintf(msg, sizeof msg, "part %s rotated by %+.1f / %+.1f / %+.1f degrees (X/Y/Z)", pi >= 0 ? LL->scene.parts[static_cast<size_t>(pi)].id.c_str() : "?",
                                      static_cast<double>(g.rotGizmoAcc[0]), static_cast<double>(g.rotGizmoAcc[1]), static_cast<double>(g.rotGizmoAcc[2]));
                        Log(msg);
                    }
                } else {
                    if (g.scaleGizmoAcc[0] != 1.0f || g.scaleGizmoAcc[1] != 1.0f || g.scaleGizmoAcc[2] != 1.0f) {
                        char msg[192];
                        std::snprintf(msg, sizeof msg, "part %s resized to %.2f / %.2f / %.2f (X/Y/Z)", pi >= 0 ? LL->scene.parts[static_cast<size_t>(pi)].id.c_str() : "?",
                                      static_cast<double>(g.scaleGizmoAcc[0]), static_cast<double>(g.scaleGizmoAcc[1]), static_cast<double>(g.scaleGizmoAcc[2]));
                        Log(msg);
                    }
                }
            }
            g.gizmoAxis = -1; g.rotGizmoAxis = -1; g.scaleGizmoAxis = -1; g.drag = 0; g.interacting = false; ReleaseCapture();
            g.dirty = true; InvalidateView(); InvalidatePanel(); InvalidateFooter();
            return 0;
        }
        if (g.drag == 3) {
            if (std::shared_ptr<Loaded> LL = Current(); LL && g.dragBone >= 0 && static_cast<size_t>(g.dragBone) < LL->scene.bones.size()) Log((g.dragMove ? "moved " : "turned ") + LL->scene.bones[static_cast<size_t>(g.dragBone)].name);
            g.dragBone = -1; g.drag = 0; g.interacting = false; ReleaseCapture();
            g.dirty = true; InvalidateView();
            return 0;
        }
        if (g.drag == 6) {   // the paint stroke ends with the button
            TransEndStroke();
            g.drag = 0; g.interacting = false; g.lastInput = GetTickCount64(); ReleaseCapture();
            g.dirty = true; InvalidateView(); InvalidatePanel();
            return 0;
        }
        if (g.drag == 0) return 0;   // a click on the panel: nothing to finish, the picture stays as it is
        if (msg == WM_LBUTTONUP && g.drag == 1 && g.showBones && std::abs(GET_X_LPARAM(lp) - g.downAt.x) < 4 && std::abs(GET_Y_LPARAM(lp) - g.downAt.y) < 4)
            PickBoneAt(GET_X_LPARAM(lp) - kPanel, GET_Y_LPARAM(lp));   // a click, not a drag
        g.drag = 0; g.interacting = false; g.lastInput = GetTickCount64(); ReleaseCapture();
        g.dirty = true; InvalidateView();
        return 0;
    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (pt.y >= l.logTop) {
                std::lock_guard<std::mutex> lk(g.logMu);   // Log() touches logScroll from worker threads
                int visible = std::max(1, (l.logBottom - (l.logTop + 24)) / 15);
                int maxScroll = std::max(0, static_cast<int>(g.logLines.size()) - visible);
                g.logScroll = std::max(0, std::min(maxScroll, g.logScroll + (d > 0 ? 3 : -3)));
                InvalidateLog();
            } else if (L || g.tab == TabSettings || g.tab == TabAssets) {
                if (g.tab == TabAssets) {
                    const int visible = std::max(1, (l.assetListBottom - l.assetListTop) / kRowH);
                    const int maxScroll = std::max(0, static_cast<int>(AssetRows().size()) - visible);
                    g.assets.listPos = std::clamp(g.assets.listPos + (d > 0 ? -3.0f : 3.0f), 0.0f, static_cast<float>(maxScroll));
                    InvalidatePanel();
                } else if (g.tab == TabOptimize) {
                    const int maxScroll = std::max(0, OptLayoutFor(l.partsTop, 0).totalH - (l.partsBottom - l.partsTop));
                    g.optScroll = std::clamp(g.optScroll - (d > 0 ? 48 : -48), 0, maxScroll);
                    InvalidatePanel();
                } else if (g.showIssues) {
                    g.partScroll = std::max(0, g.partScroll - (d > 0 ? 3 : -3));
                } else if (g.tab == TabMaterials) {
                    const int visible = std::max(1, (l.matListBottom - l.matListTop) / kRowH);
                    const int maxScroll = std::max(0, static_cast<int>(BuildMatRows(*L).size()) - visible);
                    g.matScroll = std::max(0, std::min(maxScroll, g.matScroll - (d > 0 ? 3 : -3)));
                } else if (g.tab == TabBones) {
                    if (pt.y >= l.boneEditTop && !g.selBoneName.empty()) {   // over a field: a fine step (Shift: ten)
                        const int row = (pt.y - l.boneEditTop) / kRowH;
                        SliderRef r;
                        if (row >= 1 && row <= kBoneEditCount && SliderFor(kBoneEdit + row - 1, r)) {
                            const float step = r.step * (g.shiftDown ? 10.0f : 1.0f);
                            const PoseState before = CapturePose();
                            r.Set(std::clamp(r.Value() + (d > 0 ? step : -step), r.lo, r.hi));
                            ApplyPoseLive();
                            PoseChanged(before, true);
                            InvalidateList();
                        }
                    } else if (BuildBoneRows(L->scene).size() > 0) {
                        const int visible = std::max(1, (l.boneListBottom - l.boneListTop) / kRowH);
                        const int maxScroll = std::max(0, static_cast<int>(BuildBoneRows(L->scene).size()) - visible);
                        g.boneScroll = std::max(0, std::min(maxScroll, g.boneScroll - (d > 0 ? 3 : -3)));   // Frame eases the list there
                    }
                } else if (g.tab != TabParts && g.tab != TabOptimize && g.tab != TabBones && g.tab != TabMaterials) {
                    int maxScroll = std::max(0, static_cast<int>(BuildTweakRows(g.tab).size()) - VisibleRows(l));
                    g.partScroll = std::max(0, std::min(maxScroll, g.partScroll - (d > 0 ? 3 : -3)));
                } else if (L->NativePmx()) {
                    int maxScroll = std::max(0, static_cast<int>(BuildExplorerRows(*L).size()) - VisibleRows(l));
                    g.partScroll = std::max(0, std::min(maxScroll, g.partScroll - (d > 0 ? 3 : -3)));
                } else {
                    std::vector<PartRow> rows = BuildRows(*L);
                    int maxScroll = std::max(0, static_cast<int>(rows.size()) - VisibleRows(l));
                    g.partScroll = std::max(0, std::min(maxScroll, g.partScroll - (d > 0 ? 3 : -3)));
                }
            }
        } else {
            g.cam.zoom = std::max(0.2f, std::min(20.0f, g.cam.zoom * (d > 0 ? 1.12f : 1 / 1.12f)));
            g.lastInput = GetTickCount64();
        }
        if (pt.x >= kPanel) { g.dirty = true; InvalidateView(); }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            bool hand = ButtonAt(l, p.x, p.y) != 0 || PtInRect(&l.clearLog, p) != 0 ||
                        OutfitChipAt(l, p.x, p.y) >= 0;
            if (p.x >= kPanel && GetProgSnapshot().active) {
                const RECT cardNow = ProgOverlayRect(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
                const RECT cancelNow = ProgCancelRect(cardNow);
                hand = hand || PtInRect(&cancelNow, p) != 0;
            }
            if (p.x >= kPanel) {   // the diagnostic mode strip's cells want the pointing-hand cursor too
                const int W = std::max(0, static_cast<int>(rc.right) - kPanel), H = rc.bottom;
                hand = hand || ModeStripHover(p.x, p.y, kPanel, W, H);
                const AxisWidgetLayout al = AxisWidgetBox(W);
                hand = hand || AxisWidgetWithin(al, float(p.x - kPanel), float(p.y));   // the navigation widget
            }
            if (p.x < kPanel && !hand) {
                std::shared_ptr<Loaded> L = Current();
                if (L && !g.showIssues && g.tab == TabMaterials) {
                    hand = (p.y >= l.matListTop && p.y < l.matListBottom) || PtInRect(&l.matFilter, p) != 0;
                } else if (L && !g.showIssues && g.tab == TabOptimize) {
                    hand = g.opt.hover != -1;
                } else if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts && g.tab != TabOptimize) {
                    hand = RowIndexAt(BuildTweakRows(g.tab).size(), l, p.x, p.y) >= 0;
                } else if (L && !g.showIssues && L->NativePmx()) {
                    hand = RowIndexAt(BuildExplorerRows(*L).size(), l, p.x, p.y) >= 0;
                } else if (L && !g.showIssues) {
                    std::vector<PartRow> rows = BuildRows(*L);
                    hand = RowAt(rows, l, p.x, p.y) >= 0;
                }
            }
            bool gizmoHot = false;
            if (p.x >= kPanel && GizmoPartSelected() >= 0 && PartGizmoDrawn())
                gizmoHot = PickPartGizmoAxis(float(p.x - kPanel), float(p.y), 9.0f) >= 0;
            SetCursor(LoadCursor(nullptr, gizmoHot ? IDC_SIZEALL : (hand || g.hoverAccChip >= 0 || g.hoverRotChip >= 0 || g.hoverScaleChip >= 0 || g.hoverDeleteChip >= 0) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_CHAR:
        if (g.morphEdit >= 0) { MorphEditChar(static_cast<wchar_t>(wp)); return 0; }   // the rename box takes printable characters
        if (g.tab == TabAssets && g.assets.editing) { AssetsChar(static_cast<wchar_t>(wp)); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_KEYUP:
        if (wp == VK_SHIFT || wp == VK_LSHIFT || wp == VK_RSHIFT) g.shiftDown = false;
        else if (wp == VK_CONTROL || wp == VK_LCONTROL || wp == VK_RCONTROL) g.ctrlDown = false;
        return 0;
    case WM_KEYDOWN: {
        if (wp == VK_SHIFT || wp == VK_LSHIFT || wp == VK_RSHIFT) g.shiftDown = true;
        else if (wp == VK_CONTROL || wp == VK_LCONTROL || wp == VK_RCONTROL) g.ctrlDown = true;
        std::shared_ptr<Loaded> L = Current();
        if (g.morphEdit >= 0) { MorphEditKey(wp); return 0; }   // the rename box is open: the keyboard goes to it
        if (g.sliderEdit >= 0) { SliderEditKey(wp); return 0; }   // Shift+Left-click opened the keyboard box: the row owns the keys now
        if (g.matHexEditing) {
            if (wp == VK_ESCAPE) { g.matHexEditing = false; g.matHexUndoPushed = false; g.matHexBuf.clear(); InvalidateMat(); return 0; }
            if (wp == VK_RETURN) { g.matHexEditing = false; g.matHexUndoPushed = false; InvalidateMat(); return 0; }
            if (wp == VK_BACK) { if (!g.matHexBuf.empty()) g.matHexBuf.pop_back(); InvalidateMat(); return 0; }
            int ch = -1;
            if (wp >= '0' && wp <= '9') ch = wp;
            else if (wp >= 'A' && wp <= 'F') ch = wp;
            else if (wp >= 'a' && wp <= 'f') ch = wp;
            else if (wp >= VK_NUMPAD0 && wp <= VK_NUMPAD9) ch = '0' + (wp - VK_NUMPAD0);
            if (ch >= 0) {
                if (g.matHexBuf.size() >= 6) return 0;
                if (g.matHexBuf.empty()) { material::EffectiveMaterial eff = (g.selMaterial >= 0) ? material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial)) : material::EffectiveMaterial{}; g.matHexBuf = MatHexOf(eff); }
                // Clear on first digit so typing replaces the colour.
                if (g.matHexBuf.size() == 6 && !g.matHexUndoPushed) g.matHexBuf.clear();
                g.matHexBuf.push_back(static_cast<char>(ch));
                if (g.matHexBuf.size() == 6) {
                    if (!g.matHexUndoPushed) { material::PushUndo(g.mat); g.matHexUndoPushed = true; }
                    ApplyMatHex(g.matHexBuf);
                }
                InvalidateMat();
            }
            return 0;
        }
        if (wp == VK_ESCAPE && (g.matClassDropOpen || g.matFilterDropOpen || g.matPickerOpen || g.matTexSlotDropOpen || g.matUvDrag)) {
            g.matClassDropOpen = false; g.matFilterDropOpen = false; g.matPickerOpen = false; g.matPickerDrag = 0; g.matTexSlotDropOpen = false; g.matUvDrag = 0; g.matDropHover = -1;
            ReleaseCapture();
            InvalidateMat(); return 0;
        }
        if (wp == VK_ESCAPE && GetProgSnapshot().active) { ProgRequestCancel(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (wp == VK_ESCAPE && GizmoPartSelected() >= 0) {   // deselect the part gizmo
            const int pi = GizmoPartSelected();
            std::string id = pi >= 0 ? Current()->scene.parts[static_cast<size_t>(pi)].id : std::string();
            g.selGizmoPart = -1;
            g.gizmoHoverAxis = g.rotGizmoHoverAxis = g.scaleGizmoHoverAxis = -1;
            if (g.drag == 4) { g.gizmoAxis = -1; g.drag = 0; g.interacting = false; ReleaseCapture(); }
            if (g.drag == 7) { g.rotGizmoAxis = -1; g.drag = 0; g.interacting = false; ReleaseCapture(); }
            if (g.drag == 8) { g.scaleGizmoAxis = -1; g.drag = 0; g.interacting = false; ReleaseCapture(); }
            Log("gizmo off" + (id.empty() ? std::string("") : (" (" + id + ")")));
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            InvalidatePanel();
            InvalidateFooter();
            return 0;
        }
        if (g.ctrlDown) {   // Ctrl+Z undo (Shift: redo), Ctrl+Y redo, Ctrl+S save, Ctrl+O load (pose or materials by tab)
            if (g.tab == TabMaterials) {
                if (wp == 'Z') MaterialUndo(g.shiftDown);
                else if (wp == 'Y') MaterialUndo(true);
                else if (wp == 'S') SaveMatFile();
                else if (wp == 'O') LoadMatFile();
            } else {
                if (wp == 'Z') UndoPose(g.shiftDown);
                else if (wp == 'Y') UndoPose(true);
                else if (wp == 'S') SavePoseFile();
                else if (wp == 'O') LoadPoseFile();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == 'T') { g.textured = !g.textured; g.dirty = true; }
        else if (wp == 'H') { g.shaded = !g.shaded; g.dirty = true; }
        else if (wp == 'V') {   // every shader layer on / off
            bool any = false;
            for (size_t i = 0; i < g.gpu.layerCount(); ++i) any = any || g.gpu.layerEnabled(i);
            for (size_t i = 0; i < g.gpu.layerCount(); ++i) g.gpu.SetLayerEnabled(i, !any);
            Log(g.gpu.layerCount() == 0 ? std::string("no shader layers (put .hlsl files in the layers folder)") : std::string(any ? "shader layers off" : "shader layers on"));
            g.dirty = true;
        }
        else if (wp == 'G') { exporter::SetColourGradeEnabled(!exporter::ColourGradeEnabled()); g.dirty = true; }
        else if (wp == 'X') ToggleAsExported();
        else if (wp == 'K') { g.showBones = !g.showBones; Log(g.showBones ? "skeleton: on (click a bone to select it)" : "skeleton: off"); g.dirty = true; }
        else if (wp == 'Y') { SetPhysics(!g.physics); InvalidatePanel(); }
        else if (wp == 'N') { ShakePhysics(); InvalidatePanel(); }
        else if (wp == 'P') { const PoseState before = CapturePose(); g.aPose = !g.aPose; Log(std::string("A-pose: ") + (g.aPose ? "on" : "off")); ApplyPoseLive(); PoseChanged(before); }
        else if (wp == 'R') ResetView(0);
        else if (wp == 'D') ResetClothing();
        else if (wp == 'F') ResetView(0);
        else if (wp == 'S') ResetView(90);
        else if (wp == 'B') ResetView(180);
        else if (wp == 'I') { if (L) ToggleIssues(); }
        else if (wp == 'A') { if (L) ShowAll(); }
        else if (wp == 'C') ClearLog();
        else if (wp == 'E') { if (g.shiftDown) StartExportClothing(); else ExportFolderIfNeeded(); }
        else if (wp == 'O') OpenCard();
        else if (wp == 'M') ChooseMods();
        else if (wp == 'L') ChooseLibrary();
        else if (wp >= '1' && wp <= '9') { if (L) ChooseOutfit(static_cast<int>(wp - '1')); }
        else if ((wp == VK_LEFT || wp == VK_RIGHT) && L && L->outfits > 1) {
            int n = L->outfits, o = (L->outfit + (wp == VK_RIGHT ? 1 : n - 1)) % n;
            ChooseOutfit(o);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        // The Selective Import window is this window's owned window: close it first too (it does not quit the process).
        if (g.sel.hwnd) SelectiveClose();
        // The Material Workbench is this window's owned window: close it first (it
        // does not quit the process, this window does).
        if (g.matHwnd) {
            HWND wb = g.matHwnd;
            g.matHwnd = nullptr;
            DestroyWindow(wb);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- benchmark -------------------------------------------------------------------------------------------------------
// CARDBRIDGE_BENCH=<file.json>: once the first card is up, plays a scripted session through the window procedure (orbit, hover over the
// parts, tab switches, a slider drag, a flood of mouse moves, log lines, wheel scrolling), times every step from input to the finished
// paint and writes the numbers, then closes the window.
