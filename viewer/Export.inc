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

// X: show the character the way the exported PMX looks (baked textures, plain materials). The bake takes a while, so it runs
// on a thread once per character and grading state; later presses only switch.
void ToggleAsExported() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.parts.empty()) return;
    if (g.showExp) { g.showExp = false; g.dirty = true; Log("as exported: off"); return; }
    if (!g.gpu.ready()) { Log("as exported needs the Direct3D 11 view", LogKind::Warn); return; }
    if (g.expOk && g.expFor == L && g.expGrade == exporter::ColourGradeEnabled()) { g.showExp = true; g.dirty = true; Log("as exported: on"); return; }
    if (g.expBusy.exchange(true)) { Log("the export preview is still being baked", LogKind::Warn); return; }
    const bool grade = exporter::ColourGradeEnabled();
    ProgBegin("Rendering as-exported preview (MMD look)");
    Log(std::string("baking the export preview (colour only, toon ramps, ") + (grade ? "graded" : "not graded") + ")");
    auto copy = std::make_shared<assemble::Scene>(L->scene);
    std::thread([L, grade, copy]() {
        std::shared_ptr<assemble::Scene> sc;
        bool cancelled = false;
        try {
            sc = copy;
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

