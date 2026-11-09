// viewer fragment: frame, layout dump, WindowProc (included in order by main.cpp)

// ---- one picture -------------------------------------------------------------------------------------------------------
// A slider's row repaints while it is dragged (and the tab heading, whose "tweaked" label follows it); nothing else.
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
        if (row >= 0) {
            const int y = l.matEditTop + row * kRowH;
            InvalidatePanelBand(y, y + kRowH);
        }
        RECT heading{kPanel - 80, l.partsHeadingTop - 2, kPanel, l.partsHeadingTop + 16};
        InvalidateRect(g.hwnd, &heading, FALSE);
        return;
    }
    const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == 1 && rows[i].id == id) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - ScrollPx(g.partPos);
            InvalidatePanelBand(std::max(l.partsTop, y), std::min(l.partsBottom, y + kRowH));
            break;
        }
    RECT heading{kPanel - 80, l.partsHeadingTop - 2, kPanel, l.partsHeadingTop + 16};
    InvalidateRect(g.hwnd, &heading, FALSE);
}

// Mouse movement during a drag is only noted by the window procedure (dragPending); this applies it, once per picture, so a fast mouse
// cannot queue up work faster than it can be drawn.
void ApplyPending() {
    if (!g.dragPending) return;
    g.dragPending = false;
    if (g.sliderDrag >= 0) {
        SliderRef r;
        if (SliderFor(g.sliderDrag, r)) { std::lock_guard<std::recursive_mutex> lk(g.mu); *r.v = SliderValueAt(r, g.dragX); }
        TweakEdited(g.sliderDrag, false);
        InvalidateSliderRow(g.sliderDrag);
    } else if (g.drag == 3 && (g.boneDragDx != 0.0f || g.boneDragDy != 0.0f)) {
        const float dx = g.boneDragDx, dy = g.boneDragDy;
        g.boneDragDx = g.boneDragDy = 0.0f;
        DragBone(g.dragBone, dx, dy, g.dragMove);
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
    const ULONGLONG now = GetTickCount64();
    const float dt = g.lastFrameTick ? std::min(1.0f / 30.0f, static_cast<float>(now - g.lastFrameTick) / 1000.0f) : 1.0f / 60.0f;
    g.lastFrameTick = now;
    ApplyPending();
    PhysicsTick();
    const bool a = EaseScroll(g.partPos, g.partScroll, dt);
    const bool b = EaseScroll(g.bonePos, g.boneScroll, dt);
    const bool c = EaseScroll(g.matPos, g.matScroll, dt);
    g.animating = a || b || c;
    if (g.animating) InvalidateList();
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
    if (GetUpdateRect(g.hwnd, nullptr, FALSE)) UpdateWindow(g.hwnd);
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
    j["tabs"] = nlohmann::json::array();
    for (int i = 0; i < TabCount; ++i) j["tabs"].push_back({(l.tabs[i].left + l.tabs[i].right) / 2, (l.tabs[i].top + l.tabs[i].bottom) / 2});
    j["buttons"] = {{"open", {(l.open.left + l.open.right) / 2, (l.open.top + l.open.bottom) / 2}}, {"export", {(l.exportPmx.left + l.exportPmx.right) / 2, (l.exportPmx.top + l.exportPmx.bottom) / 2}}, {"resetClothing", {(l.clothingReset.left + l.clothingReset.right) / 2, (l.clothingReset.top + l.clothingReset.bottom) / 2}}};
    std::shared_ptr<Loaded> L = Current();
    j["rows"] = nlohmann::json::array();
    if (L && g.tab != TabParts && g.tab != TabBones && g.tab != TabMaterials) {
        const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.partPos * kRowH)) + kRowH / 2;
            j["rows"].push_back({{"id", rows[i].id}, {"kind", rows[i].kind}, {"label", rows[i].label}, {"y", y >= l.partsTop && y < l.partsBottom ? y : -1}});
        }
    }
    j["materials"] = nlohmann::json::object();
    if (L && g.tab == TabMaterials) {
        const std::vector<int> mrows = BuildMatRows(*L);
        j["materials"]["list"] = nlohmann::json::array();
        for (size_t i = 0; i < mrows.size(); ++i) {
            const int y = l.matListTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.matPos * kRowH)) + kRowH / 2;
            if (y >= l.matListTop && y < l.matListBottom)
                j["materials"]["list"].push_back({{"index", mrows[i]}, {"y", y}, {"name", L->scene.materials[static_cast<size_t>(mrows[i])].name}, {"sel", mrows[i] == g.selMaterial}});
        }
        j["materials"]["editTop"] = l.matEditTop;
        j["materials"]["fieldY"] = nlohmann::json::array();
        for (int k = 0; k < kMatCount; ++k) {
            int row = (k <= 2) ? (2 + k) : (3 + k);
            j["materials"]["fieldY"].push_back(l.matEditTop + row * kRowH + kRowH / 2);
        }
        j["materials"]["hexY"] = l.matEditTop + 5 * kRowH + kRowH / 2;
        j["materials"]["classY"] = l.matEditTop + 1 * kRowH + kRowH / 2;
        j["materials"]["classBtn"] = {(l.matClassBtn.left + l.matClassBtn.right) / 2, (l.matClassBtn.top + l.matClassBtn.bottom) / 2};
        j["materials"]["pickBtn"] = {(l.matPickBtn.left + l.matPickBtn.right) / 2, (l.matPickBtn.top + l.matPickBtn.bottom) / 2};
        j["materials"]["classDropOpen"] = g.matClassDropOpen;
        j["materials"]["filterDropOpen"] = g.matFilterDropOpen;
        j["materials"]["pickerOpen"] = g.matPickerOpen;
        {
            const RECT sv = MatPickerSV(l), hb = MatPickerHue(l);
            j["materials"]["pickerSV"] = {(sv.left + sv.right) / 2, (sv.top + sv.bottom) / 2};
            j["materials"]["pickerHue"] = {(hb.left + hb.right) / 2, (hb.top + hb.bottom) / 2};
            j["materials"]["pickerHueLeft"] = {hb.left + 4, (hb.top + hb.bottom) / 2};
            j["materials"]["pickerSVHi"] = {sv.left + 4, sv.top + 4};
            j["materials"]["pickerSVLo"] = {sv.right - 4, sv.bottom - 4};
        }
        if (g.selMaterial >= 0) {
            const auto eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
            j["materials"]["cls"] = static_cast<int>(eff.cls);
        }
        j["materials"]["resetY"] = l.matEditTop + 12 * kRowH + kRowH / 2;
        j["materials"]["filterY"] = l.partsTop + kRowH / 2;
        j["materials"]["sel"] = g.selMaterial;
        j["materials"]["filter"] = g.matFilter;
        j["materials"]["overrides"] = g.mat.OverrideCount();
        j["materials"]["undo"] = g.mat.undoStack.size();
        j["materials"]["redo"] = g.mat.redoStack.size();
    }
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
        j["clothingPath"] = g.clothingPath;
        j["clothingName"] = g.clothingName;
        j["clothingOutfit"] = g.clothingOutfit;
    }
    j["clothingApplied"] = L ? L->clothing : std::string();
    j["state"] = {{"loaded", L != nullptr}, {"busy", g.busy.load()}, {"gpu", g.gpuOk}, {"physics", g.physics}, {"aPose", g.aPose}, {"showBones", g.showBones},
                  {"showExp", g.showExp}, {"expOk", g.expOk}, {"exporting", g.exporting.load()}, {"undo", g.undoStack.size()}, {"redo", g.redoStack.size()}, {"edits", static_cast<size_t>(std::count_if(g.boneEdits.begin(), g.boneEdits.end(), [](const auto& e) { return !assemble::IsIdentity(e.second); }))}, {"selBone", g.selBoneName}, {"selMaterial", g.selMaterial}, {"matUndo", g.mat.undoStack.size()}, {"matRedo", g.mat.redoStack.size()}, {"matOverrides", g.mat.OverrideCount()}};
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
    case WM_EXP_READY: {
        const bool wasCancelled = (wp == 1);
        std::shared_ptr<assemble::Scene> sc;
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            sc = std::move(g.expPending);
            g.expFor = g.expPendingFor; g.expGrade = g.expPendingGrade;
            g.expPendingFor.reset();
        }
        g.expBusy = false;
        g.expOk = false;
        if (wasCancelled || !sc) {   // cancelled (or a failed bake): keep the previous look
            g.expFor.reset();
            ProgEnd();
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (sc && g.expFor != Current()) {   // the character changed while it baked: drop the stale copy instead of keeping it
            g.expFor.reset();
            g.gpuExp.Unload();
            ProgEnd();
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        ProgSet(0.92, "uploading to GPU");
        std::string why;
        exporter::GpuOptions go;
        go.preference = g.settings.gpu.empty() ? "auto" : g.settings.gpu;
        go.skin = g.settings.skin.empty() ? "gpu" : g.settings.skin;
        if (sc && (g.gpuExp.ready() || g.gpuExp.Init(go, &why))) {
            g.gpuExp.SetPlain(true);
            if (g.gpuExp.Load(*sc, &why, [](double f, const std::string& stage) { ProgSet(0.92 + 0.08 * std::clamp(f, 0.0, 1.0), stage); }, [] { return ProgCancelled(); })) { g.expScene = sc; g.expOk = true; g.showExp = g.expFor == Current(); ApplyPoseLive(); Log("as exported: on (X switches back)", LogKind::Good); }
            else Log("export preview upload failed: " + why, LogKind::Warn);
        } else if (sc) Log("export preview: no Direct3D 11 device (" + why + ")", LogKind::Warn);
        ProgEnd();
        g.dirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LOADED: {
        if (wp == 1) {   // cancelled: keep the previous character, just stop the bar
            g.loadingOutfit = -1;
            ProgEnd();
            g.status = Current() ? "loading cancelled" : "drop a Koikatsu card on this window";
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        std::shared_ptr<Loaded> L = Current();
        if (L && !L->scene.bones.empty()) {
            g.baseHeightCm = assemble::SceneHeightMeters(L->scene) * 100.0f;   // metres to centimetres
            g.heightCm = g.baseHeightCm;
            const float bustSize = g.tw.bodyCard.size() > 4 ? g.tw.bodyCard[4] : 0.5f;
            g.sagDefault = assemble::CardSagDefault(L->scene, bustSize);
            g.sag = g.sagDefault;   // follow the card: each card starts at its own hang
        }
        g.hidden.assign(L ? L->scene.parts.size() : 0, 0);
        if (L && g.applyKeep)
            for (size_t i = 0; i < L->scene.parts.size(); ++i) {
                auto it = g.keepHidden.find(L->scene.parts[i].id);
                if (it != g.keepHidden.end()) g.hidden[i] = it->second;
            }
        if (L)
            for (size_t i = 0; i < L->scene.parts.size(); ++i)
                if (L->scene.parts[i].nudeSwitch && !(g.applyKeep && g.keepHidden.count(L->scene.parts[i].id))) g.hidden[i] = 1;   // starts unticked
        g.keepHidden.clear();
        g.partScroll = 0;
        g.partPos = 0;
        g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
        g.loadingOutfit = -1;
        g.soloPart = -1;
        g.state = exporter::NewViewState();
        g.gpuOk = false;
        g.gpuScene.reset();
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
        g.gpuExp.Unload();
        // No pose or physics may carry over from the previous card (bone counts and names differ).
        g.skin.clear();
        g.boneWorld.clear();
        g.poseWorld.clear();
        g.sim = assemble::PhysicsSim{};
        if (L && g.gpu.ready()) {
            ProgSet(0.97, "uploading shaders to GPU");
            ProgFlush();
            std::string gpuWhy;
            if (g.gpu.Load(L->scene, &gpuWhy, [](double f, const std::string& stage) { ProgSet(0.97 + 0.03 * std::clamp(f, 0.0, 1.0), stage); }, [] { return ProgCancelled(); })) {
                g.gpuScene = L;
                g.gpuOk = true;
                Log("game shaders uploaded to Direct3D 11", LogKind::Good);
            } else if (gpuWhy == "cancelled by user") {
                Log("GPU upload cancelled; showing the software renderer", LogKind::Warn);
            } else {
                Log("Direct3D 11 upload failed; using the software renderer", LogKind::Warn);
            }
        } else if (L) {
            Log("no Direct3D 11 device; using the software renderer", LogKind::Warn);
        }
        ProgEnd();
        if (L && !g.applyKeep) { g.boneEdits.clear(); g.undoStack.clear(); g.redoStack.clear(); g.selBoneName.clear(); g.boneScroll = 0; g.bonePos = 0; SeedOpenBones(L->scene); }
        else if (L && g.openBones.empty()) SeedOpenBones(L->scene);
        // Materials: capture a fresh baseline for the new character.
        if (L) {
            material::CaptureOriginals(g.mat, L->scene);
            g.selMaterial = L->scene.materials.empty() ? -1 : 0;
            g.matFilter = -1;
            g.matScroll = 0; g.matPos = 0;
            for (int k = 0; k < kMatCount; ++k) g.matBuf[k] = 0;
            g.matHexEditing = false; g.matHexBuf.clear();
            g.matClassDropOpen = false; g.matFilterDropOpen = false; g.matPickerOpen = false; g.matPickerDrag = 0; g.matDropHover = -1;
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
            SaveSettings(g.settings);
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
            bool isClothing = false;
            try {
                card::ClothingFile probe = card::LoadClothingFile(dropped);
                (void)probe;
                isClothing = true;
            } catch (const card::CardError& e) {
                isClothing = e.kind == card::CardErrorKind::ClothesFile;
            } catch (const std::exception&) {
                isClothing = false;
            }
            if (isClothing) {
                ApplyClothingFile(dropped);
            } else {
                g.cam = {};
                {
                    std::lock_guard<std::recursive_mutex> lk(g.mu);
                    g.clothingPath.clear();
                    g.clothingName.clear();
                    g.clothingOutfit = -1;
                }
                StartLoad(dropped, -1);
            }
        }
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    case WM_LBUTTONDOWN: {
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
            if (button == 3) { if (Current()) ToggleIssues(); return 0; }
            if (button == 4) { if (Current()) ResetView(0); return 0; }
            if (button == 5) { if (Current()) ShowAll(); return 0; }
            if (button == 6) { ToggleAsExported(); InvalidateRect(hwnd, nullptr, FALSE); InvalidatePanel(); return 0; }
            if (button == 7) { ResetClothing(); return 0; }
            if (button == 8) { MoveClothing(-1); return 0; }
            if (button == 9) { MoveClothing(1); return 0; }
            if (button >= 10) {
                g.showIssues = false;
                g.tab = button - 10;
                g.partScroll = 0;
                g.partPos = 0;
                g.hoverRow = g.hoverPart = -1;
                InvalidatePanel();
                return 0;
            }
            int chip = OutfitChipAt(l, x, y);
            if (chip >= 0) { ChooseOutfit(chip); return 0; }
            POINT p{x, y};
            if (PtInRect(&l.clearLog, p)) { ClearLog(); return 0; }
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) { BonesTabClick(x, y, l, false); return 0; }
            if (L && !g.showIssues && g.tab == TabMaterials) { MaterialsTabClick(x, y, l, false); return 0; }
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, false); return 0; }
            if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(*L);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0) {
                    const PartRow& row = rows[static_cast<size_t>(idx)];
                    if (row.part < 0) {
                        if (row.replaced) { /* nothing to expand: the base outfit has no parts left */ }
                        else if (row.group == "Added outfit" && x < 30) {   // master checkbox: all added parts at once
                            bool masterOn = true;
                            for (const auto& p : L->scene.parts) {
                                if (DisplayGroup(p) != row.group || p.underPiece) continue;
                                const int pi = static_cast<int>(&p - L->scene.parts.data());
                                if (static_cast<size_t>(pi) < g.hidden.size() && g.hidden[static_cast<size_t>(pi)]) { masterOn = false; break; }
                            }
                            for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i)
                                if (DisplayGroup(L->scene.parts[i]) == row.group && !L->scene.parts[i].underPiece)
                                    g.hidden[i] = masterOn ? 1 : 0;
                            g.soloPart = -1;
                            for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i)
                                if (L->scene.parts[i].id.find(".shoes") != std::string::npos)
                                    L->scene.parts[i].visibleInPreview = !g.hidden[i];
                            Log(masterOn ? "hide Added outfit" : "show Added outfit");
                            ReshapeNow(true);
                            g.dirty = true;
                            InvalidateRect(hwnd, nullptr, FALSE);
                            InvalidatePanel();
                        } else {
                            if (g.collapsedGroups.count(row.group)) g.collapsedGroups.erase(row.group);
                            else g.collapsedGroups.insert(row.group);
                            g.hoverRow = -1;
                            g.hoverPart = -1;
                            InvalidatePanel();
                        }
                    } else if (static_cast<size_t>(row.part) < g.hidden.size()) {
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
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        if (g.showBones && !g.boneScreen.empty()) {   // a bone under the pointer is grabbed: dragging turns it, Ctrl+drag moves it
            const int hit = exporter::PickBone(g.boneScreen, static_cast<float>(x - kPanel), static_cast<float>(y), 12.0f);
            if (std::shared_ptr<Loaded> L = Current(); L && hit >= 0) {
                g.selBoneName = L->scene.bones[static_cast<size_t>(hit)].name;
                g.dragBone = hit;
                g.dragMove = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                g.drag = 3; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
                InvalidatePanel();
                return 0;
            }
        }
        g.drag = 1; g.last = {x, y}; g.downAt = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: {
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
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, true); return 0; }
            if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(*L);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0 && rows[static_cast<size_t>(idx)].part >= 0) {
                    int part = rows[static_cast<size_t>(idx)].part;
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
        g.drag = 2; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_MBUTTONDOWN:
        g.drag = 2; g.last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; g.interacting = true; SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.sliderDrag >= 0) { g.dragX = x; g.dragPending = true; return 0; }   // applied by Frame, once per picture
        if (g.matPickerDrag != 0 && g.matPickerOpen) {
            const PanelLayout l = Lay(ClientHeight());
            ApplyPickerAt(x, y, g.matPickerDrag, l);   // immediate: same frame as a slider drag
            return 0;
        }
        // Dropdown hover (highlight the row under the cursor).
        if ((g.matClassDropOpen || g.matFilterDropOpen) && x < kPanel && g.tab == TabMaterials && !g.showIssues) {
            const PanelLayout l = Lay(ClientHeight());
            const RECT drop = g.matClassDropOpen ? MatClassDropRect(l) : MatFilterDropRect(l);
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
            } else if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) {
                g.hoverRow = RowIndexAt(BuildTweakRows(g.tab).size(), l, x, y);
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
        if (oldButton != g.hoverButton || oldOutfit != g.hoverOutfit) InvalidatePanelBand(l.open.top - 2, l.tabs[0].bottom + 2);
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
        if (g.matDropHover >= 0) { g.matDropHover = -1; InvalidatePanel(); }
        if (g.hoverRow >= 0 || g.hoverPart >= 0 || g.hoverButton || g.hoverOutfit >= 0) {
            bool part = g.hoverPart >= 0;
            g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
            g.hoverButton = 0;
            if (part) { g.dirty = true; InvalidateView(); }
            InvalidatePanel();
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ApplyPending();   // the last movement of the drag counts
        CommitPoseEdit();
        if (g.matPickerDrag != 0) {
            g.matPickerDrag = 0;
            ReleaseCapture();
            InvalidatePanel();
            return 0;
        }
        if (g.sliderDrag >= 0) {
            SliderRef r;
            const bool live = IsLiveId(g.sliderDrag);
            if (!live && SliderFor(g.sliderDrag, r)) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "slider %d set to %.3f (card %.3f)", g.sliderDrag, *r.v, r.def);
                Log(msg);
            }
            const int released = g.sliderDrag;
            g.sliderDrag = -1;
            ReleaseCapture();
            TweakEdited(released, true);
            InvalidateList();   // the group's "reset" mark follows the sliders
            return 0;
        }
        if (g.drag == 3) {
            if (std::shared_ptr<Loaded> LL = Current(); LL && g.dragBone >= 0 && static_cast<size_t>(g.dragBone) < LL->scene.bones.size()) Log((g.dragMove ? "moved " : "turned ") + LL->scene.bones[static_cast<size_t>(g.dragBone)].name);
            g.dragBone = -1; g.drag = 0; g.interacting = false; ReleaseCapture();
            g.dirty = true; InvalidateView();
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
            } else if (L || g.tab == TabSettings) {
                if (g.showIssues) {
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
                            const float step = r.step * ((GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f);
                            const PoseState before = CapturePose();
                            *r.v = std::clamp(*r.v + (d > 0 ? step : -step), r.lo, r.hi);
                            ApplyPoseLive();
                            PoseChanged(before, true);
                            InvalidateList();
                        }
                    } else if (BuildBoneRows(L->scene).size() > 0) {
                        const int visible = std::max(1, (l.boneListBottom - l.boneListTop) / kRowH);
                        const int maxScroll = std::max(0, static_cast<int>(BuildBoneRows(L->scene).size()) - visible);
                        g.boneScroll = std::max(0, std::min(maxScroll, g.boneScroll - (d > 0 ? 3 : -3)));   // Frame eases the list there
                    }
                } else if (g.tab != TabParts) {
                    int maxScroll = std::max(0, static_cast<int>(BuildTweakRows(g.tab).size()) - VisibleRows(l));
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
            if (p.x < kPanel && !hand) {
                std::shared_ptr<Loaded> L = Current();
                if (L && !g.showIssues && g.tab == TabMaterials) {
                    hand = (p.y >= l.matListTop && p.y < l.matListBottom) || (p.y >= l.matEditTop);
                } else if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) {
                    hand = RowIndexAt(BuildTweakRows(g.tab).size(), l, p.x, p.y) >= 0;
                } else if (L && !g.showIssues) {
                    std::vector<PartRow> rows = BuildRows(*L);
                    hand = RowAt(rows, l, p.x, p.y) >= 0;
                }
            }
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN: {
        std::shared_ptr<Loaded> L = Current();
        if (g.matHexEditing) {
            if (wp == VK_ESCAPE) { g.matHexEditing = false; g.matHexUndoPushed = false; g.matHexBuf.clear(); InvalidatePanel(); return 0; }
            if (wp == VK_RETURN) { g.matHexEditing = false; g.matHexUndoPushed = false; InvalidatePanel(); return 0; }
            if (wp == VK_BACK) { if (!g.matHexBuf.empty()) g.matHexBuf.pop_back(); InvalidatePanel(); return 0; }
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
                InvalidatePanel();
            }
            return 0;
        }
        if (wp == VK_ESCAPE && (g.matClassDropOpen || g.matFilterDropOpen || g.matPickerOpen)) {
            g.matClassDropOpen = false; g.matFilterDropOpen = false; g.matPickerOpen = false; g.matPickerDrag = 0; g.matDropHover = -1;
            InvalidatePanel(); return 0;
        }
        if (wp == VK_ESCAPE && GetProgSnapshot().active) { ProgRequestCancel(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (GetKeyState(VK_CONTROL) & 0x8000) {   // Ctrl+Z undo (Shift: redo), Ctrl+Y redo, Ctrl+S save, Ctrl+O load (pose or materials by tab)
            if (g.tab == TabMaterials) {
                if (wp == 'Z') MaterialUndo((GetKeyState(VK_SHIFT) & 0x8000) != 0);
                else if (wp == 'Y') MaterialUndo(true);
                else if (wp == 'S') SaveMatFile();
                else if (wp == 'O') LoadMatFile();
            } else {
                if (wp == 'Z') UndoPose((GetKeyState(VK_SHIFT) & 0x8000) != 0);
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
        else if (wp == 'E') ExportFolderIfNeeded();
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- benchmark -------------------------------------------------------------------------------------------------------
// CARDBRIDGE_BENCH=<file.json>: once the first card is up, plays a scripted session through the window procedure (orbit, hover over the
// parts, tab switches, a slider drag, a flood of mouse moves, log lines, wheel scrolling), times every step from input to the finished
// paint and writes the numbers, then closes the window.
