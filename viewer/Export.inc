// viewer fragment: export, as-exported (included in order by main.cpp)

// ---- export ------------------------------------------------------------------------------------------------------------
std::string PickExportParent(HWND owner) {
    if (std::getenv("CARDBRIDGE_TEST_HOOKS") && std::getenv("CARDBRIDGE_EXPORT_PARENT")) return Slashes(std::getenv("CARDBRIDGE_EXPORT_PARENT"));   // scripted runs: no dialog
    if (!g.settings.exportDir.empty() && Exists(g.settings.exportDir)) return g.settings.exportDir;   // set in the Setup tab
    return PickFolder(owner, L"Choose where the exported PMX folder should be created");
}

void StartExport() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.parts.empty()) { Log("nothing to export yet", LogKind::Warn); return; }
    if (g.busy || g.exporting.exchange(true)) { Log("still busy; wait for the current job to finish", LogKind::Warn); return; }
    std::string parent = PickExportParent(g.hwnd);
    if (parent.empty()) { g.exporting = false; return; }
    std::string stem = SafeStem(L->title.empty() ? std::string("character") : L->title);
    std::string outDir = parent + "/" + stem;
    for (int n = 2; Exists(outDir) && n < 1000; ++n) outDir = parent + "/" + stem + " (" + std::to_string(n) + ")";
    const std::string library = g.settings.library, mods = g.settings.mods;
    const std::vector<char> hiddenNow = g.hidden;
    const bool jiggleBust = g.jiggleBust, jiggleButt = g.jiggleButt;
    const float jiggleIntensity = g.jiggleIntensity;
    const float sag = g.sag;
    const assemble::Pose pose = BuildPose(*L, false);   // no sag here: BakeSettledPhysics adds it once (same as CLI, same as the viewer preview)
    material::WorkbenchState matCopy = g.mat;   // materials: snapshot for the manifest (the scene copy already carries the edits)
    const std::string matSha = L->card.sha256;
    ProgBegin("Exporting PMX: " + stem);
    Log("export started: " + outDir + (pose.empty() ? std::string() : std::string(" (posed)")), LogKind::Info);
    assemble::Scene sceneCopy = L->scene;   // taken here: a slider may re-shape the character while the export runs
    std::thread([L, outDir, stem, library, mods, hiddenNow, jiggleBust, jiggleButt, jiggleIntensity, sag, pose, matCopy, matSha, sceneCopy = std::move(sceneCopy)]() mutable {
        auto t0 = std::chrono::steady_clock::now();
        bool cancelled = false;
        try {
            assemble::Scene scene = std::move(sceneCopy);   // bake the copy, not what the viewport is drawing
            ProgSet(0.03, "posing");
            if (ProgCancelled()) throw Cancelled{};
            assemble::BakePose(scene, pose);    // the A-pose and the bone edits become the model's rest pose
            assemble::BakeSettledPhysics(scene, jiggleBust, jiggleButt, jiggleIntensity, sag);   // the bust/hips as they hang under gravity
            exporter::ApplyPartStateForExport(scene, hiddenNow);   // hidden items leave the PMX entirely (meshes, chains and their own bones); the preview is the viewer, 1:1
            if (matCopy.IsDirty()) material::ReportUnsupported(scene, matCopy);   // unsupported workbench fields are reported, never silently dropped
            Log("baking the game shader textures");
            ProgSet(0.06, "baking shader textures");
            // Colour only, graded like the preview (G): the PMX viewer or engine does the lighting over a white material.
            exporter::BakeShaderTextures(scene, 1024, false, nullptr,
                [](const std::string& m) {
                    Log("  " + m);
                    const double f = ParseBakeFrac(m);
                    ProgSet(f >= 0.0 ? 0.06 + f * 0.44 : -1.0, m);
                }, true, true, [] { return ProgCancelled(); });
            if (ProgCancelled()) throw Cancelled{};
            ProgSet(0.52, "writing PMX");
            exporter::ExportOptions eo;
            eo.outDir = outDir;
            eo.baseName = stem;
            eo.outfit = L->outfit;
            if (g.opt.applied.set) {   // the Optimize tab's Apply commits to the export (the PMX and the LODs carry it)
                eo.simplifyPercent = std::clamp(g.opt.applied.reduction, 0.0f, 0.9f) * 100.0f;
                eo.lodTargets = g.opt.applied.lods;
                const size_t nLods = eo.lodTargets.size();
                char buf[128];
                std::snprintf(buf, sizeof buf, "Mesh Optimization applied: %.0f%% reduction, %zu LOD%s", eo.simplifyPercent, nLods, nLods == 1 ? "" : "s");
                Log(buf, LogKind::Info);
            }
            eo.jiggleBust = jiggleBust;
            eo.jiggleButt = jiggleButt;
            eo.jiggleIntensity = jiggleIntensity;
            eo.assetLibrary = library;
            eo.modsFolder = mods;
            if (matCopy.IsDirty()) {
                material::MaterialFile mf = material::MakeMaterialFile(scene, matCopy, matSha);
                eo.materialFile = "viewer";
                eo.materialWorkbench = nlohmann::json::parse(material::MaterialFileToJson(mf), nullptr, false);
                if (!eo.materialWorkbench.is_discarded()) eo.materialWorkbench["file"] = "viewer";
            }
            eo.log = [](const std::string& m) { Log("  " + m); };
            eo.progress = [](double f, const std::string& stage) {
                ProgSet(f >= 0.0 ? 0.52 + f * 0.48 : -1.0, stage);
            };
            eo.cancelled = [] { return ProgCancelled(); };
            exporter::ExportResult r = exporter::ExportScene(scene, L->card, eo);
            if (ProgCancelled()) throw Cancelled{};
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char msg[512];
            std::snprintf(msg, sizeof msg, "export done in %.1fs: %s (%s vertices, %s triangles, %zu textures)", secs, r.pmxPath.c_str(),
                          Count(r.vertices).c_str(), Count(r.triangles).c_str(), r.textures);
            Log(msg, LogKind::Good);
        } catch (const Cancelled&) {
            cancelled = true;
            Log("export cancelled; the partial folder " + outDir + " is kept (delete it if you do not need it)", LogKind::Warn);
        } catch (const std::exception& e) {
            Log(std::string("export failed: ") + e.what(), LogKind::Error);
        }
        g.exporting = false;
        PostMessageW(g.hwnd, WM_EXPORT_DONE, cancelled ? 1 : 0, 0);
    }).detach();
}

// The file the outfit is saved to: CARDBRIDGE_CLOTHES_PATH when set (scripted runs, no dialog), else the usual save dialog.
std::string ClothingPathFromUser() {
    if (const char* env = std::getenv("CARDBRIDGE_CLOTHES_PATH")) return Slashes(env);
    std::shared_ptr<Loaded> L = Current();
    const std::string stem = L && !L->title.empty() ? L->title : std::string("character");
    const int outfit = L ? L->outfit : 0;
    const std::string slot = (L && L->outfits > 0) ? L->card.outfitName(std::clamp(outfit, 0, L->outfits - 1)) : std::string("Outfit");
    std::wstring def = Utf8ToWide(SafeStem(stem + " - " + slot) + ".png");
    wchar_t file[1024] = {0};
    const size_t copy = std::min<size_t>(def.size(), 1023);
    std::memcpy(file, def.c_str(), copy * sizeof(wchar_t));
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"Koikatsu clothing file (*.png)\0*.png\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrDefExt = L"png";
    of.lpstrTitle = L"Export the outfit as a clothing (outfit) file";
    of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    std::wstring initDir;
    if (!g.settings.exportDir.empty()) { initDir = Utf8ToWide(g.settings.exportDir); of.lpstrInitialDir = initDir.c_str(); }
    return GetSaveFileNameW(&of) ? Slashes(WideToUtf8(file)) : std::string();
}

// Shift+E / the "Export clothes" button: write the outfit on screen as a Koikatsu outfit file (【KoiKatuClothes】 .png) —
// the same file --clothes applies. The shown slot's unticked accessory slots are exported as empty (nothing is forced
// back into them on apply), and the card's own hidden state (showAccessory) plus hair accessories are omitted by default.
void StartExportClothing() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    if (g.busy || g.exporting.exchange(true)) { Log("still busy; wait for the current job to finish", LogKind::Warn); return; }
    try {
        // "Export clothes" asks first, like a Windows confirm: the outfit's data, and whether to keep hair.
        // A verification checkbox (the same checkbox Win10 dialog has) offers to keep hair-type accessories
        // (type 122/123, worn as hair) and to show the character's hair on the mannequin icon.
        BOOL withHairDialog = FALSE;
        if (std::getenv("CARDBRIDGE_TEST_HOOKS")) {
            // Scripted runs: no dialog; CARDBRIDGE_CLOTHES_WITH_HAIR=1 keeps hair on.
            const char* wh = std::getenv("CARDBRIDGE_CLOTHES_WITH_HAIR");
            withHairDialog = (wh && *wh == '1') ? TRUE : FALSE;
        } else {
            TASKDIALOGCONFIG cfg{};
            cfg.cbSize = sizeof(cfg);
            cfg.hwndParent = g.hwnd;
            cfg.pszWindowTitle = L"CardBridge";
            cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;   // starts unchecked: hair inclusion is opt-in
            cfg.pszMainInstruction = L"Export the outfit on screen as a Koikatsu clothing file?";
            cfg.pszContent = L"The outfit's clothes, accessories and makeup are written to a \u3010KoiKatuClothes\u3011 .png — the same format --clothes applies. "
                              L"It never contains the character's face, body or hair meshes; only hair worn as accessories, and only when the switch below is on.";
            cfg.pszVerificationText = L"Include hair-type accessories (and show the hair on the mannequin icon)";
            int pressed = 0;
            BOOL wasChecked = FALSE;
            if (TaskDialogIndirect(&cfg, &pressed, nullptr, &wasChecked) != S_OK || pressed != IDOK) { g.exporting = false; return; }
            withHairDialog = wasChecked;
        }
        const bool withHairFile = withHairDialog != FALSE;
        const std::string out = ClothingPathFromUser();
        if (out.empty()) { g.exporting = false; return; }
        const int outfit = std::clamp(L->outfit, 0, std::max(0, L->outfits - 1));
        // The viewer's untick state for this outfit's accessory slots becomes empty slots in the file.
        std::set<int> hiddenSlots;
        const std::string prefix = "outfit" + std::to_string(outfit) + ".acc";
        for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i) {
            if (!g.hidden[i]) continue;
            const std::string& id = L->scene.parts[i].id;
            if (id.rfind(prefix, 0) != 0) continue;
            const std::string digits = id.substr(prefix.size());
            const bool digitsOnly = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); });
            if (digitsOnly) hiddenSlots.insert(std::atoi(digits.c_str()));
        }
        // The icon the game ships in an outfit file: a faceless, bald, flat-grey mannequin wearing exactly the outfit on
        // screen (what unticked is not there; hair only with the checkbox).
        // The icon is rendered pose-baked like the export (and like the CLI); bake into a copy.
        assemble::Scene iconScene = L->scene;
        const assemble::Pose pose = assemble::MakeHandsOnHipsPose(iconScene);
        if (!iconScene.bones.empty()) assemble::BakePose(iconScene, pose);
        std::vector<char> base(iconScene.parts.size(), 0);
        for (size_t i = 0; i < base.size() && i < g.hidden.size(); ++i) base[i] = g.hidden[i] ? 1 : 0;
        std::vector<char> iconFlags = exporter::OutfitIconHiddenParts(iconScene, L->card, outfit, base, withHairFile);
        exporter::OutfitIconOptions ipo;
        ipo.hiddenParts = &iconFlags;
        std::string iconNote;
        ipo.note = &iconNote;
        card::ClothingExportOptions opt;
        opt.outfit = outfit;
        opt.hiddenSlots = std::move(hiddenSlots);
        opt.withHairAccessories = withHairFile;
        opt.thumbnailBytes = exporter::RenderOutfitIcon(iconScene, ipo);
        if (!iconNote.empty()) Log("icon: " + iconNote, LogKind::Warn);
        const card::ClothingExportReport rep = card::ExportClothingToFile(L->card, out, opt, true);   // the save dialog already prompted to overwrite
        char msg[640];
        std::snprintf(msg, sizeof msg, "clothing written: %s (outfit %d \"%s\", %llu bytes)", out.c_str(), rep.outfit + 1, rep.outfitName.c_str(),
                      (unsigned long long)fileio::FileSize(out));
        Log(msg, LogKind::Good);
        Log("  icon: rendered the outfit on a faceless mannequin (Koikatsu's own convention for outfit files)", LogKind::Info);
        Log("  clothes re-localised: " + std::to_string(rep.clothesIdsLocalized) + " (+ " + std::to_string(rep.subPartsLocalized) + " sub-part(s))   accessories: " +
            std::to_string(rep.accessoryIdsLocalized) + "   hidden/hair omitted: " + std::to_string(rep.hiddenAccessoriesDropped) + "/" + std::to_string(rep.hairAccessoriesDropped), LogKind::Info);
        std::string plugins;
        for (const std::string& p : rep.droppedPlugins) plugins += " " + p;
        Log("  rows kept: " + std::to_string(rep.uarRowsKept) + "   KCOX garments: " + std::to_string(rep.kcoxGarments) + "   MoreAccessories: " +
            std::to_string(rep.extraAccessoriesKept) + (plugins.empty() ? "" : "   dropped plugin blocks:" + plugins), LogKind::Info);
        for (const std::string& w : rep.warnings) Log("  warning: " + w, LogKind::Warn);
    } catch (const std::exception& e) {
        Log(std::string("could not write the clothing file: ") + e.what(), LogKind::Error);
    }
    g.exporting = false;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    InvalidatePanel();
}

// X: show the character the way the exported PMX looks (baked textures, plain materials). The bake takes a while, so it runs// on a thread once per character and grading state; later presses only switch.
void ToggleAsExported() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.parts.empty()) return;
    if (g.showExp) { g.showExp = false; g.dirty = true; Log("as exported: off"); return; }
    if (!g.gpu.ready()) { Log("as exported needs the Direct3D 11 view", LogKind::Warn); return; }
    if (g.expOk && g.expFor == L && g.expGrade == exporter::ColourGradeEnabled()) { g.showExp = true; g.dirty = true; Log("as exported: on"); return; }
    if (g.expBusy.exchange(true)) { Log("the export preview is still being baked", LogKind::Warn); return; }
    const bool grade = exporter::ColourGradeEnabled();
    const bool applyOpt = g.opt.applied.set && g.opt.applied.reduction > 0.0f;
    const float optReduction = g.opt.applied.reduction;
    ProgBegin("Rendering as-exported preview (MMD look)");
    Log(std::string("baking the export preview (colour only, toon ramps, ") + (grade ? "graded" : "not graded") + ")");
    auto copy = std::make_shared<assemble::Scene>(L->scene);
    std::thread([L, grade, copy, applyOpt, optReduction]() {
        std::shared_ptr<assemble::Scene> sc;
        bool cancelled = false;
        try {
            if (applyOpt) {   // the Optimize tab's Apply commits: the as-exported preview shows the simplified model too
                mesh::SimplifyOptions so;
                so.maxReduction = optReduction;
                so.cancelled = [] { return ProgCancelled(); };
                mesh::SceneSimplifyStats st;
                sc = mesh::SimplifyScene(*copy, optReduction, so, st,
                    [](float f, const std::string& stage) { ProgSet(f * 0.05, "simplifying: " + stage); });
                for (const auto& w : st.warnings) Log("warning: " + w, LogKind::Warn);
                if (!sc) { if (ProgCancelled()) throw Cancelled{}; throw std::runtime_error("simplification failed"); }
            } else {
                sc = copy;
            }
            ProgSet(0.05, "baking shader textures");
            exporter::BakeShaderTextures(*sc, 1024, false, nullptr,
                [](const std::string& m) {
                    const double f = ParseBakeFrac(m);
                    ProgSet(f >= 0.0 ? 0.05 + f * 0.80 : -1.0, m);
                }, grade, true, [] { return ProgCancelled(); });
            if (ProgCancelled()) throw Cancelled{};
        } catch (const Cancelled&) {
            cancelled = true;
            Log("as-exported preview cancelled", LogKind::Warn);
            sc.reset();
        } catch (const std::exception& e) {
            Log(std::string("export preview failed: ") + e.what(), LogKind::Error);
            sc.reset();
        }
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            g.expPending = sc; g.expPendingFor = L; g.expPendingGrade = grade;
        }
        PostMessageW(g.hwnd, WM_EXP_READY, cancelled ? 1 : 0, 0);
    }).detach();
}

