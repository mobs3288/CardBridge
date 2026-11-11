// viewer fragment: clicks, library, settings (included in order by main.cpp)

// A click in the sliders list: a header resets its group, a switch toggles, a slider starts a drag; right-click resets the row.
void TweakClick(int x, int y, const PanelLayout& l, bool right) {
    std::vector<TweakRow> rows = BuildTweakRows(g.tab);
    int idx = RowIndexAt(rows.size(), l, x, y);
    if (idx < 0) return;
    const TweakRow& row = rows[static_cast<size_t>(idx)];
    if (row.kind == 5) { if (!right) SettingsAction(row.id); return; }   // outside the lock: it opens dialogs
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (row.kind == 4 || (row.kind == 0 && row.id < 0)) return;
    if (row.kind == 2 && row.id >= kLayerBase && row.id < kBoneEdit) {   // a shader layer: no re-assembly, just another frame
        const size_t li = static_cast<size_t>(row.id - kLayerBase);
        g.gpu.SetLayerEnabled(li, !g.gpu.layerEnabled(li));
        Log("layer " + row.label + (g.gpu.layerEnabled(li) ? ": on" : g.gpu.layerOk(li) ? ": off" : ": does not compile"), g.gpu.layerOk(li) ? LogKind::Info : LogKind::Warn);
        if (!g.gpuOk) Log("layers need the Direct3D 11 view", LogKind::Warn);
        g.dirty = true;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    if (row.kind == 0) {
        bool any = false;
        for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) {
            any = any || TweakChanged(rows[k].id);
            ResetTweak(rows[k].id);
        }
        if (any) {
            Log("reset " + row.label);
            if (!ReshapeNow(true)) ScheduleApplyTweaks();
        }
        return;
    }
    if (row.kind == 2 && row.id == kPhysics) {
        SetPhysics(right ? false : !g.physics);
        InvalidatePanel();
        return;
    }
    if (row.kind == 2 && row.id == kAPose) {
        g.aPose = right ? false : !g.aPose;
        Log(std::string("A-pose: ") + (g.aPose ? "on" : "off"));
        ApplyPoseLive();
        InvalidatePanel();
        return;
    }
    if (row.kind == 2 && (row.id == kJiggleBust || row.id == kJiggleButt)) {
        bool& v = row.id == kJiggleBust ? g.jiggleBust : g.jiggleButt;
        if (right) ResetTweak(row.id); else v = !v;
        Log(row.label + ": " + (v ? "on" : "off") + " (exported PMX)");
        TweakEdited(row.id, true);
        return;
    }
    if (row.kind == 2 && row.id == kAutoCloth) {
        if (right) ResetTweak(row.id); else g.autoCloth = !g.autoCloth;
        Log(row.label + ": " + (g.autoCloth ? "on" : "off") + " (reloads the card to rebuild cloth bones)");
        ScheduleApplyTweaks();
        return;
    }
    if (row.kind == 2 && row.id == kNipplePink) {
        if (right) ResetTweak(row.id); else g.tw.pinkNipples = !g.tw.pinkNipples;
        Log(row.label + ": " + (g.tw.pinkNipples ? "on" : "off"));
        TweakEdited(row.id, true);
        return;
    }
    if (row.kind == 2) {
        if (right) ResetTweak(row.id); else g.tw.nippleInside = !g.tw.nippleInside;
        Log(row.label + ": " + (g.tw.nippleInside ? "on" : "off"));
        TweakEdited(kNippleIn, true);
        return;
    }
    SliderRef r;
    if (!SliderFor(row.id, r)) return;
    if (right) {
        if (TweakChanged(row.id)) { ResetTweak(row.id); Log("reset " + row.label); TweakEdited(row.id, true); }
        return;
    }
    if (x < kTrackL - 8) return;
    g.sliderDrag = row.id;
    *r.v = SliderValueAt(r, x);
    TweakEdited(row.id, false);
    SetCapture(g.hwnd);
    InvalidatePanel();
}

// A click in the Bones tab: the switch row, a tree row (arrow opens, name selects, right-click resets that bone), the editor's fields.
void BonesTabClick(int x, int y, const PanelLayout& l, bool right) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const assemble::Scene& scene = L->scene;
    const POINT pt{x, y};
    if (y >= l.partsTop && y < l.partsTop + kRowH) {
        if (right) return;
        if (PtInRect(&l.boneSkeleton, pt)) { g.showBones = !g.showBones; Log(g.showBones ? "skeleton: on (click a bone to select it)" : "skeleton: off"); }
        else { g.showHelpers = !g.showHelpers; g.boneScroll = 0; }
        g.dirty = true;
        InvalidateAll();
        return;
    }
    if (y >= l.boneListTop && y < l.boneListBottom) {
        const std::vector<BoneRow> rows = BuildBoneRows(scene);
        const int idx = (y - l.boneListTop + static_cast<int>(std::lround(g.bonePos * kRowH))) / kRowH;
        if (idx < 0 || static_cast<size_t>(idx) >= rows.size()) return;
        const BoneRow& r = rows[static_cast<size_t>(idx)];
        const std::string& name = scene.bones[static_cast<size_t>(r.bone)].name;
        const int arrowX = 16 + std::min(r.depth, 12) * 10;
        if (right) { g.boneEdits.erase(name); Log("reset " + name); ApplyPoseLive(); }
        else if (r.kids && x >= arrowX - 3 && x < arrowX + 12) { if (!g.openBones.erase(name)) g.openBones.insert(name); }
        else g.selBoneName = name;
        g.dirty = true;
        InvalidateAll();
        return;
    }
    if (y < l.boneEditTop) return;
    const int row = (y - l.boneEditTop) / kRowH;
    if (row == kBoneEditRows - 1 && !right) {   // undo / redo / save / load
        if (PtInRect(&l.boneUndo, pt)) UndoPose(false);
        else if (PtInRect(&l.boneRedo, pt)) UndoPose(true);
        else if (PtInRect(&l.boneSave, pt)) SavePoseFile();
        else if (PtInRect(&l.boneLoad, pt)) LoadPoseFile();
        InvalidatePanel();
        return;
    }
    if (g.selBoneName.empty()) return;
    if (row >= 1 && row <= kBoneEditCount) {
        const int id = kBoneEdit + row - 1;
        SliderRef r;
        if (!SliderFor(id, r)) return;
        if (right) { *r.v = r.def; ApplyPoseLive(); InvalidatePanel(); return; }
        if (x < kTrackL - 8) return;
        g.sliderDrag = id;
        *r.v = SliderValueAt(r, x);
        SetCapture(g.hwnd);
        ApplyPoseLive();
        InvalidatePanel();
    } else if (row == kBoneEditRows - 2 && !right) {
        if (PtInRect(&l.boneResetOne, pt)) { g.boneEdits.erase(g.selBoneName); Log("reset " + g.selBoneName); }
        else if (PtInRect(&l.boneResetAll, pt)) { g.boneEdits.clear(); Log("reset every bone"); }
        else return;
        ApplyPoseLive();
        InvalidatePanel();
    }
}

// A click in the Materials tab: the launcher, not the editor. It handles the class
// filter dropdown and picking a material from the list; everything that edits a
// material lives in the Material Workbench window (Workbench.inc, WbClick).
void MaterialsTabClick(int x, int y, const PanelLayout& l, bool right) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (g.mat.originals.size() != L->scene.materials.size()) material::CaptureOriginals(g.mat, L->scene);
    const POINT pt{x, y};
    // The filter dropdown is modal: a click inside picks, a click outside closes it.
    if (g.matFilterDropOpen) {
        const RECT drop = MatFilterDropRect(l.matFilter.left, l.matFilter.top, l.matFilter.right - l.matFilter.left);
        if (!right && PtInRect(&drop, pt)) {
            const int idx = (y - drop.top) / kRowH;
            if (idx >= 0 && idx < 12) SetMatFilter(idx == 0 ? -1 : idx - 1);
            g.matFilterDropOpen = false;
            g.matDropHover = -1;
            InvalidateMat();
            return;
        }
        g.matFilterDropOpen = false;
        g.matDropHover = -1;
        InvalidateMat();
        return;
    }
    // A click outside the workbench's HEX row ends a HEX entry running over there.
    if (g.matHexEditing) {
        g.matHexEditing = false;
        g.matHexUndoPushed = false;
    }
    if (!right && PtInRect(&l.matFilter, pt)) {
        g.matFilterDropOpen = true;
        g.matDropHover = -1;
        InvalidateMat();
        return;
    }
    if (y >= l.matListTop && y < l.matListBottom) {
        const std::vector<int> rows = BuildMatRows(*L);
        const int idx = (y - l.matListTop + ScrollPx(g.matPos)) / kRowH;
        if (idx < 0 || static_cast<size_t>(idx) >= rows.size()) return;
        g.selMaterial = rows[static_cast<size_t>(idx)];
        g.matUvTrisCache = -2;
        g.matTexSlot = -1;
        g.matUvCacheMat = -2;
        g.matTexSlotDropOpen = false;
        SyncMatBuf();
        g.dirty = true;
        RevealSelectedMaterial();   // the workbench's list follows the selection
        InvalidateAll();
        return;
    }
}

void ChooseMods() {
    std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu mods folder");
    if (d.empty()) return;
    g.settings.mods = d;
    g.settings.noMods = false;
    SaveSettings(g.settings);
    g.resolver.reset();   // the next load indexes the new folder
    Log("mods folder: " + d, LogKind::Good);
    if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
    else InvalidatePanel();
}

// Builds the asset library from the user's own Koikatsu folder (the only thing that ever reads the game), on a thread, with the progress in
// the log. When it is done WM_LIBRARY_DONE opens the card that was waiting.
void StartLibraryImport(const std::string& game) {
    if (g.busy.exchange(true)) { Log("still busy; wait for it to finish", LogKind::Warn); return; }
    const std::string dest = DefaultLibraryDir();
    g.status = "copying the game data into the library ...";
    ProgBegin("Building asset library from game");
    Log("building the asset library from " + game, LogKind::Info);
    InvalidatePanel();
    std::thread([game, dest]() {
        bool ok = false, cancelled = false;
        try {
            library::ImportOptions o;
            o.gameRoot = game;
            o.libraryRoot = dest;
            o.profile = library::Profile::Full;
            auto r = library::ImportLibrary(o, [](size_t done, size_t total) {
                const double f = total ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
                ProgSet(f, "copying game data " + std::to_string(done) + " / " + std::to_string(total));
                if (done % 25 == 0 || done == total) Log("  copying game data " + std::to_string(done) + " / " + std::to_string(total));
            }, [] { return ProgCancelled(); });
            for (const auto& issue : r.issues) Log("  " + issue, LogKind::Warn);
            ok = r.files > 0 && library::CheckLibrary(dest, false).ok;
            if (ok) ProgSet(1.0, "done");
            Log(ok ? "asset library ready: " + dest : std::string("the library import failed: is that the Koikatsu folder (it has abdata inside)?"), ok ? LogKind::Good : LogKind::Error);
        } catch (const Cancelled&) {
            cancelled = true;
            Log("library import cancelled (files already copied stay on disk; run it again to finish)", LogKind::Warn);
        } catch (const std::exception& e) {
            Log(std::string("the library import failed: ") + e.what(), LogKind::Error);
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LIBRARY_DONE, 0, cancelled ? 2 : (ok ? 1 : 0));
    }).detach();
}

// The all-in-one exe carries the library inside itself: unpack it once into the viewer's own folder (no dialogs).
void StartEmbeddedUnpack() {
    if (g.busy.exchange(true)) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string exePath = Slashes(WideToUtf8(exe));
    const std::string dest = DefaultLibraryDir();
    g.status = "unpacking the game data that comes with CardBridge (first run only) ...";
    ProgBegin("Unpacking library from exe");
    Log("unpacking the library that is inside this exe into " + dest);
    InvalidatePanel();
    std::thread([exePath, dest]() {
        bool ok = false, cancelled = false;
        std::string why;
        try {
            const std::string parent = dest.substr(0, dest.find_last_of('/'));
            ok = library::ExtractEmbeddedLibrary(exePath, parent, [](size_t done, size_t total) {
                const double f = total ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
                ProgSet(f, "unpacking " + std::to_string(done) + " / " + std::to_string(total));
                if (done % 25 == 0 || done == total) Log("  unpacking " + std::to_string(done) + " / " + std::to_string(total));
            }, &why, [] { return ProgCancelled(); })
                 && library::CheckLibrary(dest, false).ok;
            if (ok) ProgSet(1.0, "done");
        } catch (const Cancelled&) {
            cancelled = true;
            why = "cancelled by user";
        } catch (const std::exception& e) {
            why = e.what();
        }
        Log(cancelled ? "library unpack cancelled (unpack again to finish)" : (ok ? "asset library ready: " + dest : "could not unpack the library: " + why), cancelled ? LogKind::Warn : ok ? LogKind::Good : LogKind::Error);
        g.busy = false;
        PostMessageW(g.hwnd, WM_LIBRARY_DONE, 0, cancelled ? 2 : (ok ? 1 : 0));
    }).detach();
}

// No library yet (first run) or the user asked for one (L): build it from the game folder, or point at one that exists.
void SetupLibrary(bool offerExisting) {
    if (std::getenv("CARDBRIDGE_TEST_HOOKS") && std::getenv("CARDBRIDGE_SETUP_GAME")) { StartLibraryImport(Slashes(std::getenv("CARDBRIDGE_SETUP_GAME"))); return; }   // scripted runs: no dialogs
    if (offerExisting) {
        const int a = MessageBoxW(g.hwnd, L"Yes: build the library from your Koikatsu folder (about 0.9 GB, copied once).\nNo: use a library folder that already exists.", L"CardBridge - asset library", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDNO) {
            const std::string d = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder");
            if (d.empty()) return;
            g.settings.library = d;
            SaveSettings(g.settings);
            Log("asset library: " + d, LogKind::Good);
            if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
            else InvalidatePanel();
            return;
        }
    } else {
        MessageBoxW(g.hwnd, L"CardBridge needs a copy of the base-game data (skeleton, body, shaders, hair and clothes: about 0.9 GB).\nIt is copied once from your own Koikatsu install and never needs the game again.\n\nNext: choose your Koikatsu folder (the one that contains \"abdata\").", L"CardBridge - first run", MB_OK | MB_ICONINFORMATION);
    }
    const std::string game = PickFolder(g.hwnd, L"Choose your Koikatsu folder (it contains abdata)");
    if (game.empty()) { g.status = "no asset library: press L to set one up"; InvalidatePanel(); return; }
    StartLibraryImport(game);
}

void ChooseLibrary() { SetupLibrary(true); }

// (Re)loads the shader layers from the default places and the user's extra folder.
void ReloadLayers(bool announce) {
    g.layerDefs = exporter::LoadShaderLayers(LayerFolders(g.settings.library, g.settings.layers));
    std::string report;
    g.gpu.SetLayers(g.layerDefs, &report);
    if (!g.layerDefs.empty()) {
        std::string names;
        for (const auto& d : g.layerDefs) names += (names.empty() ? "" : ", ") + d.name;
        Log(std::to_string(g.layerDefs.size()) + " shader layer(s): " + names + "  (V toggles, Look tab lists them)", LogKind::Good);
        for (size_t at = 0; at < report.size();) {
            size_t end = report.find('\n', at);
            if (end == std::string::npos) end = report.size();
            if (end > at) Log("layer failed: " + report.substr(at, end - at), LogKind::Warn);
            at = end + 1;
        }
    } else if (announce) {
        Log("no shader layers found", LogKind::Warn);
    }
    g.dirty = true;
}

// Creates the Direct3D device again on the device the Setup tab now asks for, and re-uploads the current character on it,
// so switching GPU / WARP / software takes effect at once without restarting. The baked "as exported" copy is re-uploaded
// too when there is one. On failure the viewer keeps the CPU fallback (software renderer).
void ReinitGpu() {
    // A device switch re-uploads the whole scene (the "render" preparation);
    // it runs on the UI thread, so the bar shows indeterminate over it and
    // cannot be pressed until it returns. The upload itself is short.
    ProgBegin("Switching graphics device");
    ProgFlush();
    exporter::GpuOptions go;
    go.preference = g.settings.gpu.empty() ? "auto" : g.settings.gpu;
    go.skin = g.settings.skin.empty() ? "gpu" : g.settings.skin;
    if (const char* env = std::getenv("CARDBRIDGE_GPU"))
        Log(std::string("CARDBRIDGE_GPU=") + env + " overrides the Setup choice; the device uses that value", LogKind::Warn);
    const bool wasExp = g.expOk && g.expScene != nullptr && g.expFor == Current();
    g.gpu.Shutdown();
    g.gpuExp.Shutdown();
    g.gpuOk = false;
    g.gpuScene.reset();
    g.lastGpu = nullptr;
    g.expOk = false;
    if (!wasExp) g.showExp = false;
    std::string why;
    const bool ok = g.gpu.Init(go, &why);
    if (ok) {
        const char* what = g.gpu.kind() == exporter::GpuKind::Warp ? "WARP on the CPU" : "the real GPU";
        Log("graphics device: " + g.gpu.adapter() + " (" + what + ")", LogKind::Good);
    } else {
        Log("Direct3D 11 unavailable (" + why + "); software renderer (the CPU fallback)", LogKind::Warn);
    }
    ReloadLayers(false);
    std::shared_ptr<Loaded> L = Current();
    if (ok && L && !L->scene.parts.empty()) {
        if (g.gpu.Load(L->scene, &why)) {
            g.gpuScene = L;
            g.gpuOk = true;
            Log("game shaders uploaded to Direct3D 11", LogKind::Good);
        } else {
            Log("Direct3D 11 upload failed; using the software renderer", LogKind::Warn);
        }
    }
    if (wasExp && g.expScene) {
        std::string ewhy;
        if (g.gpuExp.Init(go, &ewhy)) {
            g.gpuExp.SetPlain(true);
            if (g.gpuExp.Load(*g.expScene, &ewhy)) {
                g.expOk = true;
                g.showExp = true;
            } else {
                Log("export preview upload failed: " + ewhy, LogKind::Warn);
                g.expOk = false;
                g.showExp = false;
                g.expFor.reset();
                g.expScene.reset();
            }
        } else {
            g.expOk = false;
            g.showExp = false;
        }
    }
    ApplyPoseLive();
    ProgEnd();
    g.dirty = true;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// The Setup tab: what a person may need to change. Each saves to viewer.json at once.
void SettingsAction(int id) {
    auto reloadCard = [] { g.resolver.reset(); if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true); };
    switch (id) {
    case kSetLibChoose: {
        const std::string d = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder (it contains abdata)");
        if (d.empty()) break;
        if (!Exists(d + "/abdata")) { Log("that folder has no abdata inside: " + d, LogKind::Error); break; }
        g.settings.library = d;
        SaveSettings(g.settings);
        Log("asset library: " + d, LogKind::Good);
        reloadCard();
        break;
    }
    case kSetLibBuild: {
        const std::string game = PickFolder(g.hwnd, L"Choose your Koikatsu folder (it contains abdata)");
        if (!game.empty()) StartLibraryImport(game);
        break;
    }
    case kSetModsChoose: ChooseMods(); break;
    case kSetModsReindex:
        fileio::RemoveFile(ModCachePath());
        Log("the mod index will be built again", LogKind::Info);
        reloadCard();
        break;
    case kSetModsClear:
        g.settings.mods.clear();
        g.settings.noMods = true;
        SaveSettings(g.settings);
        Log("no mods folder: base-game items only", LogKind::Info);
        reloadCard();
        break;
    case kSetExportDir: {
        const std::string d = PickFolder(g.hwnd, L"Choose the folder exports go into");
        if (d.empty()) break;
        g.settings.exportDir = d;
        SaveSettings(g.settings);
        Log("exports go to " + d, LogKind::Good);
        break;
    }
    case kSetExportAsk:
        g.settings.exportDir.clear();
        SaveSettings(g.settings);
        Log("the export folder is asked every time");
        break;
    case kSetLayers: {
        const std::string d = PickFolder(g.hwnd, L"Choose a folder of shader layers (.hlsl files)");
        if (d.empty()) break;
        g.settings.layers = d;
        SaveSettings(g.settings);
        ReloadLayers(true);
        break;
    }
    case kSetLayersDefault:
        g.settings.layers.clear();
        SaveSettings(g.settings);
        ReloadLayers(true);
        break;
    case kSetGpuAuto: case kSetGpuHardware: case kSetGpuWarp: case kSetGpuSoftware: {
        const std::string value = id == kSetGpuAuto ? "auto" : id == kSetGpuHardware ? "hardware" : id == kSetGpuWarp ? "warp" : "software";
        if (g.settings.gpu == value) { Log("graphics device: already on " + value); break; }
        g.settings.gpu = value;
        SaveSettings(g.settings);
        Log(std::string("graphics device: ") + value, LogKind::Good);
        ReinitGpu();
        break;
    }
    case kSetSkinGpu: case kSetSkinCpu: case kSetSkinAuto: {
        const std::string value = id == kSetSkinGpu ? "gpu" : id == kSetSkinAuto ? "auto" : "cpu";
        if (g.settings.skin == value) { Log("skin: already on " + value); break; }
        g.settings.skin = value;
        SaveSettings(g.settings);
        g.gpu.SetSkinMode(g.settings.skin);
        g.gpuExp.SetSkinMode(g.settings.skin);
        if (g.gpu.ready() && !g.cardPath.empty() && Current()) StartLoad(g.cardPath, -1, true);
        else Log("skin: " + g.settings.skin + (g.gpu.ready() ? " (next card)" : " (next start)"));
        break;
    }
    case kSetOpenFolder: {
        const std::string dir = SettingsPath().substr(0, SettingsPath().find_last_of('/'));
        fileio::CreateDirectories(dir);
        ShellExecuteW(g.hwnd, L"open", Utf8ToWide(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kSetForget:
        g.settings = Settings();
        SaveSettings(g.settings);
        Log("settings forgotten: the library and the mods folder are looked for again at the next start (the library on disk stays)", LogKind::Warn);
        break;
    default: break;
    }
    InvalidatePanel();
}

