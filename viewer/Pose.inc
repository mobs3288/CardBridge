// viewer fragment: posing, physics, load (included in order by main.cpp)
std::vector<Mat4> PartTransforms(const assemble::Scene& scene);   // Gizmo.inc: the gizmo's per-part bind-space edits

// ---- posing ---------------------------------------------------------------------------------------------------------
// includeSag: the live preview (and the physics anchors) start from the sagged pose. The export passes false:
// BakePose bakes A-pose + edits only, then BakeSettledPhysics adds the settle + sag once, so MMD and the viewer
// show the same single hang instead of the export sagging twice.
assemble::Pose BuildPose(const Loaded& L, bool includeSag = true) {
    assemble::Pose p;
    for (const auto& [name, e] : g.boneEdits) {
        if (assemble::IsIdentity(e)) continue;
        const int i = L.scene.boneIndex(name);
        if (i >= 0) p.edit(static_cast<size_t>(i)) = e;
    }
    if (g.aPose) assemble::MakeAPose(L.scene, g.aPoseDeg, p.base);
    // The static preview sags the same way the export does, so the viewer shows the hang even with physics off.
    // A bone the user drags herself keeps its own edit (AddSagPose skips those).
    if (includeSag) assemble::AddSagPose(L.scene, p, g.sag);
    // Height: scale everything uniformly about the roots (feet) so the model's standing height is heightCm.
    // A root the user scaled herself keeps that scale multiplied by this factor.
    if (g.baseHeightCm > 1e-4f && g.heightCm > 1e-4f) {
        const float f = g.heightCm / g.baseHeightCm;
        if (std::fabs(f - 1.0f) > 1e-4f) {
            for (size_t i = 0; i < L.scene.bones.size(); ++i) {
                if (L.scene.bones[i].parent != -1) continue;
                auto& e = p.edit(i);
                e.scl.x *= f; e.scl.y *= f; e.scl.z *= f;
            }
        }
    }
    return p;
}

// Re-skins every Direct3D view with the current pose (and the physics, when it is on) without a re-assembly, and refreshes the bone
// positions the overlay draws. `seconds` is the wall time the physics preview advances by (0 only makes the chains follow the pose).
// The jiggle honours JiggleSettings::simulationHz with its own fixed substeps and is drawn interpolated, so it is stable at any frame rate.
void ApplyPoseLive(double seconds = 0.0) {
    std::shared_ptr<Loaded> L = Current();
    g.skin.clear();
    g.boneWorld.clear();
    if (!L || L->scene.bones.empty()) return;
    const assemble::Scene& scene = L->scene;
    const size_t n = scene.bones.size();
    const assemble::Pose p = BuildPose(*L);
    g.poseWorld = assemble::BoneWorlds(scene, p);
    if (!p.empty() || (g.physics && !g.sim.empty())) {
        g.skin.resize(n);
        for (size_t i = 0; i < n; ++i) g.skin[i] = g.poseWorld[i] * Mat4::TRS(scene.bones[i].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        if (g.physics && !g.sim.empty()) {
            g.physicsMoving = g.sim.Advance(scene, g.poseWorld, seconds, g.skin);
            if (seconds == 0.0) g.physicsKick = true;   // the anchors moved: let the chains catch up
        }
    }
    g.boneWorld.resize(n);
    for (size_t i = 0; i < n; ++i) g.boneWorld[i] = g.skin.empty() ? scene.bones[i].pos : g.skin[i].point(scene.bones[i].pos);
    const std::vector<Mat4>* sk = g.skin.empty() ? nullptr : &g.skin;
    if (g.gpuOk) g.gpu.SetSkin(sk);
    if (g.expOk) g.gpuExp.SetSkin(sk);
    const std::vector<Mat4> partXf = L ? PartTransforms(L->scene) : std::vector<Mat4>{};
    // SetSkin above re-skins with the transforms from the last edit; SetPartTransforms then picks up any change (and makes
    // an edit alone count as a pose, so the GPU view does not fall back to its untouched rest buffers).
    if (g.gpuOk) g.gpu.SetPartTransforms(&partXf);
    if (g.expOk) g.gpuExp.SetPartTransforms(&partXf);
    SelectivePoseChanged();   // the static previews in the Selective Import window follow the same pose
    g.dirty = true;
    InvalidateView();
}

// The file a pose is saved to / loaded from: CARDBRIDGE_POSE_PATH when set (scripted runs, no dialog), else the usual file dialog.
std::string PosePathFromUser(bool save) {
    if (const char* env = std::getenv("CARDBRIDGE_POSE_PATH")) return Slashes(env);
    wchar_t file[1024] = L"pose.json";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"CardBridge pose (*.json)\0*.json\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrDefExt = L"json";
    of.lpstrTitle = save ? L"Save the pose" : L"Load a pose";
    of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    return ok ? Slashes(WideToUtf8(file)) : std::string();
}

PoseState CapturePose() {
    PoseState st;
    st.edits = g.boneEdits;
    st.parts = g.partEdits;
    st.deletedParts = g.deletedParts;
    st.aPose = g.aPose;
    st.aPoseDeg = g.aPoseDeg;
    return st;
}

void RestorePose(const PoseState& st) {
    g.boneEdits = st.edits;
    g.partEdits = st.parts;
    g.deletedParts = st.deletedParts;
    g.aPose = st.aPose;
    g.aPoseDeg = st.aPoseDeg;
    ApplyPoseLive();
    InvalidatePanel();
}

// Records `before` as an undo step when the pose is no longer it. `merge`: a step that follows another within a moment (wheel notches) joins it.
void PoseChanged(const PoseState& before, bool merge = false) {
    if (SamePoseState(before, CapturePose())) return;
    const ULONGLONG now = GetTickCount64();
    const bool join = merge && !g.undoStack.empty() && now - g.wheelUndoTick < 700;
    if (merge) g.wheelUndoTick = now;
    if (join) return;
    g.undoStack.push_back(before);
    if (g.undoStack.size() > 200) g.undoStack.erase(g.undoStack.begin());
    g.redoStack.clear();
}

// The pointer went up: whatever the gesture since it went down changed (a slider drag, a bone drag, a click on a switch or a reset) is one step.
void CommitPoseEdit() {
    if (!g.undoBaseValid) return;
    g.undoBaseValid = false;
    PoseChanged(g.undoBase);
}

void UndoPose(bool redo) {
    g.undoBaseValid = false;
    std::vector<PoseState>& from = redo ? g.redoStack : g.undoStack;
    std::vector<PoseState>& to = redo ? g.undoStack : g.redoStack;
    if (from.empty()) { Log(redo ? "nothing to redo" : "nothing to undo", LogKind::Warn); return; }
    to.push_back(CapturePose());
    const PoseState st = from.back();
    from.pop_back();
    RestorePose(st);
    Log(std::string(redo ? "redo" : "undo") + " (" + std::to_string(g.undoStack.size()) + " step(s) to undo, " + std::to_string(g.redoStack.size()) + " to redo)");
}

// The pose as a file (bone edits by name and the A-pose switch). CARDBRIDGE_POSE_PATH names the file without asking (scripted runs).
assemble::PoseFile CurrentPoseFile() {
    assemble::PoseFile pf;
    pf.aPose = g.aPose;
    pf.aPoseDegrees = g.aPoseDeg;
    for (const auto& [name, e] : g.boneEdits)
        if (!assemble::IsIdentity(e)) pf.bones.emplace_back(name, e);
    for (const auto& [id, e] : g.partEdits)
        if (!assemble::IsIdentity(e)) pf.parts.emplace_back(id, e);
    pf.deletedParts.assign(g.deletedParts.begin(), g.deletedParts.end());
    return pf;
}

void SavePoseFile() {
    std::string path = PosePathFromUser(true);
    if (path.empty()) return;
    try {
        const std::string text = assemble::PoseFileToJson(CurrentPoseFile());
        fileio::WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        Log("saved the pose to " + path, LogKind::Good);
    } catch (const std::exception& e) {
        Log(std::string("could not save the pose: ") + e.what(), LogKind::Error);
    }
}

void LoadPoseFile(const std::string& given = std::string()) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    const std::string path = given.empty() ? PosePathFromUser(false) : given;
    if (path.empty()) return;
    assemble::PoseFile pf;
    std::string err;
    try {
        const Bytes raw = fileio::ReadFile(path);
        if (!assemble::PoseFileFromJson(std::string(raw.begin(), raw.end()), pf, &err)) { Log("not a pose file: " + err, LogKind::Error); return; }
    } catch (const std::exception& e) {
        Log(std::string("could not read the pose: ") + e.what(), LogKind::Error);
        return;
    }
    const PoseState before = CapturePose();
    g.undoBaseValid = false;
    g.boneEdits.clear();
    g.partEdits.clear();
    g.deletedParts.clear();
    size_t unknown = 0;
    for (const auto& [name, e] : pf.bones) {
        if (L->scene.boneIndex(name) < 0) { ++unknown; continue; }
        g.boneEdits[name] = e;
    }
    size_t unknownParts = 0;
    for (const auto& [id, e] : pf.parts) {
        bool found = false;
        for (const auto& p : L->scene.parts) if (p.id == id) { found = true; break; }
        if (!found) { ++unknownParts; continue; }
        g.partEdits[id] = e;
    }
    size_t unknownDeleted = 0;
    for (const auto& id : pf.deletedParts) {
        bool found = false;
        for (const auto& p : L->scene.parts) if (p.id == id) { found = true; break; }
        if (!found) { ++unknownDeleted; continue; }
        g.deletedParts.insert(id);   // HiddenPartsNow applies it; the hide checkboxes stay as they were
    }
    g.aPose = pf.aPose;
    g.aPoseDeg = pf.aPoseDegrees;
    ApplyPoseLive();
    InvalidatePanel();
    PoseChanged(before);
    Log("loaded the pose from " + path + ": " + std::to_string(pf.bones.size() - unknown) + " bone edit(s), " + std::to_string(pf.parts.size() - unknownParts) + " part edit(s), " +
        std::to_string(g.deletedParts.size()) + " deleted part(s)" +
        ((unknown || unknownParts || unknownDeleted) ? ", " + std::to_string(unknown + unknownParts + unknownDeleted) + " entry/entries this card does not have skipped" : std::string()),
        (unknown || unknownParts || unknownDeleted) ? LogKind::Warn : LogKind::Good);
}

// The physical jiggle settings changed (a Look-tab slider, a preset chip or a reset): persist them as the default for new cards, re-arm the
// sim with them (the preview shows what the export does) and re-pose.
void JiggleSettingsChanged() {
    g.settings.jiggle = g.jiggle;
    SaveSettings(g.settings);
    if (std::shared_ptr<Loaded> L = Current()) {
        if (g.physics) {
            g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggle);
            g.physicsKick = g.physicsMoving = true;
        }
    }
    ApplyPoseLive();   // the sag pose and the chains follow the new settings at once
    InvalidatePanel();
}

// The physics switch: on starts every chain at rest under the current pose and the chains then swing and settle as bones,
// sliders or the A-pose move what they hang from (N gives them a shove). The sim runs as many fixed substeps per picture
// as the wall time asks for, at any frame rate.
void SetPhysics(bool on) {
    std::shared_ptr<Loaded> L = Current();
    if (on && (!L || L->scene.chains.empty())) { Log("no physics chains in this character", LogKind::Warn); on = false; }
    g.physics = on;
    if (on) {
        g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggle);
        g.physicsLast = NowMs();
        g.physicsKick = g.physicsMoving = true;
        Log("physics: on (" + std::to_string(L->scene.chains.size()) + " chains; move a bone or press N)", LogKind::Good);
    } else {
        Log("physics: off");
    }
    ApplyPoseLive();
}

// N: turn the whole demo on (physics + breast + butt) and give it a shove, so a shake always shows bust and ass moving.
void ShakePhysics() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (!g.jiggleBust || !g.jiggleButt || !g.physics) {
        g.jiggleBust = true;
        g.jiggleButt = true;
        InvalidatePanel();
        SetPhysics(true);
    }
    if (!g.physics) return;
    g.sim.Nudge({0.010f, 0.004f, 0.005f});   // 2.5x stronger again inside for the body chains
    g.physicsKick = g.physicsMoving = true;
    Log("physics: shake (bust + butt)");
}

// One step of the physics preview (Frame calls it every picture while the switch is on): advances by the wall time since the last tick
// and draws once. The jiggle is simulated at JiggleSettings::simulationHz with interpolation, so it stays smooth at any frame rate.
void PhysicsTick() {
    if (!g.physics) return;
    const double now = NowMs();
    const double dt = std::min(0.25, g.physicsLast > 0.0 ? (now - g.physicsLast) / 1000.0 : 0.0);
    g.physicsLast = now;
    if (!g.physicsMoving && !g.physicsKick) return;   // everything has settled: nothing to draw
    g.physicsKick = false;
    ApplyPoseLive(dt);
}

// The bone tree as list rows. Driven, shape and attach nodes are left out unless asked for (their children move up to the
// nearest shown ancestor), so the tree is the bones a person poses.
struct BoneRow {
    int bone = -1;
    int depth = 0;
    bool kids = false, open = false;
};

bool BoneShown(const assemble::Scene& scene, int i) { return g.showHelpers || !exporter::IsHelperBone(scene.bones[static_cast<size_t>(i)].name); }

std::vector<BoneRow> BuildBoneRows(const assemble::Scene& scene) {
    const size_t n = scene.bones.size();
    std::vector<std::vector<int>> kids(n);
    std::vector<int> roots;
    for (size_t i = 0; i < n; ++i) {
        const int p = scene.bones[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= n || p == static_cast<int>(i)) roots.push_back(static_cast<int>(i));
        else kids[static_cast<size_t>(p)].push_back(static_cast<int>(i));
    }
    std::function<void(int, std::vector<int>&)> shownKids = [&](int i, std::vector<int>& out) {
        for (int c : kids[static_cast<size_t>(i)]) { if (BoneShown(scene, c)) out.push_back(c); else shownKids(c, out); }
    };
    std::vector<BoneRow> rows;
    std::function<void(int, int)> walk = [&](int i, int depth) {
        std::vector<int> vk;
        shownKids(i, vk);
        BoneRow r{i, depth, !vk.empty(), g.openBones.count(scene.bones[static_cast<size_t>(i)].name) != 0};
        rows.push_back(r);
        if (r.open) for (int c : vk) walk(c, depth + 1);
    };
    for (int r : roots) {
        if (BoneShown(scene, r)) walk(r, 0);
        else { std::vector<int> vk; shownKids(r, vk); for (int c : vk) walk(c, 0); }
    }
    return rows;
}

// First look at a character's tree: the top levels open.
void SeedOpenBones(const assemble::Scene& scene) {
    g.openBones.clear();
    std::vector<int> depth(scene.bones.size(), 0);
    for (size_t i = 0; i < scene.bones.size(); ++i) {
        const int p = scene.bones[i].parent;
        depth[i] = p >= 0 && static_cast<size_t>(p) < i ? depth[static_cast<size_t>(p)] + 1 : 0;
        if (depth[i] < 4) g.openBones.insert(scene.bones[i].name);
    }
}

// Selects a bone (by index) and opens the tree down to it so it can be seen.
void SelectBone(const assemble::Scene& scene, int bone) {
    if (bone < 0 || static_cast<size_t>(bone) >= scene.bones.size()) { g.selBoneName.clear(); return; }
    g.selBoneName = scene.bones[static_cast<size_t>(bone)].name;
    for (int a = scene.bones[static_cast<size_t>(bone)].parent; a >= 0 && static_cast<size_t>(a) < scene.bones.size(); a = scene.bones[static_cast<size_t>(a)].parent) {
        g.openBones.insert(scene.bones[static_cast<size_t>(a)].name);
        if (scene.bones[static_cast<size_t>(a)].parent == a) break;
    }
    if (exporter::IsHelperBone(g.selBoneName)) g.showHelpers = true;
    const std::vector<BoneRow> rows = BuildBoneRows(scene);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].bone == bone) { g.boneScroll = static_cast<int>(i) > 3 ? static_cast<int>(i) - 3 : 0; break; }
}

// The nipple / areola tint the current switches ask for; w <= 0 means the card's own colour.
Vec4 NippleTintOption() {
    if (!g.tw.pinkNipples) return Vec4{0, 0, 0, 0};
    return Vec4{g.tw.nippleColour.x, g.tw.nippleColour.y, g.tw.nippleColour.z, 1.0f};
}

// Nose-ridge drag: change only the head mod's detail-map texture and re-upload that one small image in whichever
// view is showing. No create-job, skin-shade or AO re-bake, so the slider follows the mouse in real time. Both the
// live scene and the baked "as exported" scene are updated, so the mode you are in never changes underneath you.
void ApplyNoseRidgeLive() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    if (L->scene.noseRidge.texture >= 0) {
        assemble::SetNoseRidgeStrength(L->scene, g.tw.noseRidge, g.tw.noseRidgeOpacity);
        const int ti = L->scene.noseRidge.texture;
        if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
        if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
    }
    if (g.expOk && g.expScene && g.expScene->noseRidge.texture >= 0) {
        assemble::SetNoseRidgeStrength(*g.expScene, g.tw.noseRidge, g.tw.noseRidgeOpacity);
        const int ti = g.expScene->noseRidge.texture;
        if (g.gpuExp.ready()) g.gpuExp.UpdateTextures(std::vector<int>{ti});
    }
    g.dirty = true;
    InvalidateView();
}


// Re-applies the live skin look (automatic body shading and the nipple tint). `rebuild` recomputes the occlusion map
// from the current body mesh; a plain re-apply only changes the strength. The create texture is pushed to the GPU and
// the software renderer's cached mips are dropped, as a material tint change does.
void ApplySkinLook(bool rebuildMap) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    exporter::ApplyNippleTint(L->scene, NippleTintOption());
    if (L->scene.noseRidge.texture >= 0) {
        assemble::SetNoseRidgeStrength(L->scene, g.tw.noseRidge, g.tw.noseRidgeOpacity);
        const int ti = L->scene.noseRidge.texture;
        if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
        if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
        g.expOk = false;   // the baked "as exported" copy holds the old texture
        g.showExp = false;
        g.expFor.reset();
        g.expScene.reset();
        g.gpuExp.Unload();
    }
    auto& ss = L->scene.skinShade;
    if (ss.ready()) {
        const bool rebuild = rebuildMap;
        if (exporter::ApplySkinShade(L->scene, g.tw.skinShade, g.tw.skinShadeUnder, g.tw.skinShadeTop, g.tw.skinShadeWidth, rebuild)) {
            const int ti = ss.texture;
            if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
            if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
            if (rebuild && g.expOk) {   // the baked "as exported" copy holds the old shading
                g.expOk = false;
                g.showExp = false;
                g.expFor.reset();
                g.expScene.reset();
                g.gpuExp.Unload();
                Log("the skin shading changed: press X to bake the as-exported view again");
            }
        }
        if (!L->scene.aoBake.baseNormal.empty() && (g.tw.skinAo > 0.0f || L->scene.aoBake.applied >= 0.0f)) {
            if (exporter::ApplySkinAoBake(L->scene, g.tw.skinAo, rebuild)) {
                const int ti = ss.texture;
                if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
                if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
            }
        }
    }
    g.dirty = true;
    InvalidateView();
    InvalidatePanel();
}

// A body or face slider, the nipple switch or offset: re-shapes the assembled scene in place (a few milliseconds, no loading) and re-skins the
// views. `final` (the slider was let go) also rebuilds the normal map frames, which are left alone while dragging. False when it cannot
// be done and the character has to be assembled again.
// Re-measure the body's bust / waist / hip for the "Body measurements" group (slider rows show cm, typed entry solves
// against the skin). Cheap enough to run on every reshape; the scene holds the rest pose, so pose edits don't skew it.
void UpdateBodyMeasurements() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.bones.empty()) return;
    g.bwhMeasure = measure::Measure(L->scene);
    if (!g.bwhCardDefSet) {
        if (g.bwhMeasure.valid) {
            g.bwhCardDef[0] = g.bwhMeasure.bust; g.bwhCardDef[1] = g.bwhMeasure.waist; g.bwhCardDef[2] = g.bwhMeasure.hip;
            g.bwhCardDefSet = true;
        }
    }
}

bool ReshapeNow(bool final) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.busy || !g.resolver || L->scene.bones.empty()) return false;
    const bool solving = g.bwhSolving;      // the typed cm entry re-shapes many times in a row: keep the screen still
    assemble::AssembleOptions ao;
    ao.outfit = L->outfit;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
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
        if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
        ao.proportionLayer = ProportionLayerNow();
    }
    const double t0 = NowMs();
    if (!assemble::Reshape(L->scene, L->card, *g.resolver, ao)) return false;
    {   // the shape changed — any cached preview built against the old rest pose is invalid
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.sel.proposalCache.clear();
        g.sel.proposalCacheLru.clear();
        g.sel.importedCache.clear();
        g.sel.importedCacheLru.clear();
    }
    if (!solving) RebuildMorphDelta();   // the shape offsets moved with the bones (and the rest pose); the accumulated morph offsets move with them
    UpdateBodyMeasurements();            // the scene holds the new rest pose; cheap enough for every reshape
    const double t1 = NowMs();
    if (!solving && g.gpuOk) g.gpu.Refresh(final);
    if (final && !solving && L->scene.skinShade.ready()) {   // the body moved: re-shade the texture from the new creases
        const int ti = L->scene.skinShade.texture;
        if (exporter::ApplySkinShade(L->scene, g.tw.skinShade, g.tw.skinShadeUnder, g.tw.skinShadeTop, g.tw.skinShadeWidth, true)) {
            if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
            if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
        }
        if (!L->scene.aoBake.baseNormal.empty() && (g.tw.skinAo > 0.0f || L->scene.aoBake.applied >= 0.0f)) {
            if (exporter::ApplySkinAoBake(L->scene, g.tw.skinAo, true)) {
                if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
                if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
            }
        }
    }
    g.prof.reshape += t1 - t0;
    g.prof.refresh += NowMs() - t1;
    if (!solving && g.expOk && g.expScene && g.expFor == L) {   // the baked "as exported" copy keeps its textures; only its shape follows
        assemble::Reshape(*g.expScene, L->card, *g.resolver, ao);
        g.gpuExp.Refresh(final);
    }
    // The body sliders move the rest height: the height editor stays an absolute target, so the scale factor
    // re-targets itself to keep the displayed cm where the user put it.
    g.baseHeightCm = assemble::SceneHeightMeters(L->scene) * 100.0f;
    const float bustSize = g.tw.body.size() > 4 ? g.tw.body[4] : 0.5f;
    g.sagDefault = assemble::CardSagDefault(L->scene, bustSize);  // reset target follows the new shape; the slider itself stays put
    if (!solving && g.physics) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggle);
    if (!solving) ApplyPoseLive();
    return true;
}

bool IsShapeId(int id) { return (id >= 0 && id <= kNippleMm) || id == kAreola || id == kNippleIn || IsBodyMeasureId(id); }

// Something a slider or switch controls changed. Pose controls, the shape sliders and the areola size show at once; the character is only
// assembled again when it cannot be re-shaped in place (after the pointer has been still for a moment).
void TweakEdited(int id, bool released) {
    if (g.tab == TabBodyTransfer && IsBodyTransferId(id)) { InvalidatePanel(); return; }   // edit the requested value; Apply does the reshape
    if (id == kBraAwarePhysics) {
        g.settings.braAwarePhysics = g.braAwarePhysics;
        SaveSettings(g.settings);
        InvalidatePanel();
        return;
    }
    if (IsMatId(id)) { MaterialSliderEdited(id, released); return; }
    if (id == kJiggleBust || id == kJiggleButt || (id >= kJiggleMass && id <= kJiggleSimHz)) {
        if (id == kJiggleBust || id == kJiggleButt) {
            if (g.physics) {   // the preview shows what the export will do: prime the sim again with the new switches
                if (std::shared_ptr<Loaded> L = Current()) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggle);
                g.physicsKick = g.physicsMoving = true;
            }
            Log(std::string("jiggle preview: boobs ") + (g.jiggleBust ? "on" : "off") + ", butt " + (g.jiggleButt ? "on" : "off"));
            InvalidatePanel();
            return;
        }
        // A physical slider (mass, stiffness, damping, gravity, amplitudes, rotation, strength, Hz): persist it and re-arm the preview.
        JiggleSettingsChanged();
        if (g.physics) Log("jiggle: " + std::string(assemble::JigglePresetName(assemble::MatchJigglePreset(g.jiggle))));
        return;
    }
    if (id == kSag) {   // static hang: change the pose at once (also re-prime the sim if it is running)
        if (g.physics) {
            if (std::shared_ptr<Loaded> L = Current()) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggle);
            g.physicsKick = g.physicsMoving = true;
        }
        ApplyPoseLive();
        InvalidatePanel();
        return;
    }
    if (id >= kMorphBase && id < kMorphBase + static_cast<int>(g.morphWeights.size())) { MorphSlideEdited(); return; }   // a live blend shape (live, like a pose bone)
    if (id == kTransStrength) {   // the translucency slider re-bakes the painted alpha at the new strength, like the skin shading
        TransApplyNow();
        InvalidatePanel();
        return;
    }
    if (id == kTransSize) { InvalidatePanel(); return; }   // the radius applies to the next stroke; nothing to rebuild
    if (id == kNoseRidge) {
        // Dragging only rewrites one small texture, so the view follows without freezing. The expensive full re-bake
        // runs only after the user explicitly confirms it in a popup when the slider is released.
        if (!released) { ApplyNoseRidgeLive(); return; }
        const int answer = MessageBoxW(g.hwnd,
            L"Apply this nose ridge shading?\n\nThe character's skin textures will be re-baked once. Cancel keeps the previous value.",
            L"CardBridge - nose ridge", MB_YESNO | MB_ICONQUESTION);
        if (answer == IDYES) {
            g.tw.noseRidgeApplied = g.tw.noseRidge;
            g.settings.noseRidge = g.tw.noseRidge;
            SaveSettings(g.settings);
            ApplySkinLook(false);
            Log("nose ridge shading applied");
        } else {
            g.tw.noseRidge = g.tw.noseRidgeApplied;   // keep the UI in step with the scene we put back
            ApplyNoseRidgeLive();
        }
        InvalidatePanel();
        return;
    }
    if (id == kNoseRidgeOpacity) { if (released) { g.settings.noseRidgeOpacity = g.tw.noseRidgeOpacity; SaveSettings(g.settings); } return; }
    if (IsLiveId(id)) { ApplyPoseLive(); return; }
    if (id == kSkinShade || id == kSkinShadeUnder || id == kSkinShadeTop || id == kSkinShadeWidth || id == kSkinAo ||
        id == kNipplePink || id == kNippleR || id == kNippleG || id == kNippleB) {
        // Body shading re-applies at the new strength at once; the occlusion map is rebuilt only when a shape drag ended
        // (ReshapeNow) or from this release when the body had moved, or when the width changed (the band depends on it).
        // The nipple tint is a material property and is live.
        if (id == kSkinShade || id == kSkinShadeUnder || id == kSkinShadeTop || id == kSkinShadeWidth || id == kSkinAo) {   // persisted default for new cards — save it with the other settings
            if (id == kSkinShade) g.settings.skinShade = g.tw.skinShade;
            else if (id == kSkinShadeUnder) g.settings.skinShadeUnder = g.tw.skinShadeUnder;
            else if (id == kSkinShadeTop) g.settings.skinShadeTop = g.tw.skinShadeTop;
            else if (id == kSkinShadeWidth) g.settings.skinShadeWidth = g.tw.skinShadeWidth;
            else if (id == kSkinAo) g.settings.skinAo = g.tw.skinAo;
            SaveSettings(g.settings);
        }
        ApplySkinLook(id == kSkinShadeWidth || (released && Current() && Current()->scene.skinShade.geometryDirty));
        return;
    }
    if (id == kAreola && g.expOk) {   // the baked "as exported" copy has the old areola in its textures
        g.expOk = false;
        g.showExp = false;
        g.expFor.reset();
        g.expScene.reset();
        g.gpuExp.Unload();
        Log("the areola size changed: press X to bake the as-exported view again");
    }
    if (IsShapeId(id)) { if (!ReshapeNow(released)) ScheduleApplyTweaks(); return; }
    if (released) ScheduleApplyTweaks();
}

// Long paths and names must not run out of the panel and over the model: keep the start (the drive) and the
// useful tail, or cut the end for hints.
std::string Shorten(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars || maxChars < 12) return s;
    const size_t tail = maxChars - 9;   // "D:/..." + tail
    return s.substr(0, 6) + "..." + s.substr(s.size() - tail);
}
std::string ClipText(const std::string& s, size_t maxChars) {
    return s.size() <= maxChars ? s : s.substr(0, maxChars - 3) + "...";
}

// `libraryRoot` / `modsRoot` / `modCachePath` are snapshots StartLoad takes on
// the UI thread: a profile switch while a card is loading must not change the
// source underneath a build. The resolver key is the card-specific pair; the
// mod index has its own key (mod source only) inside ModIndexCache.
bool EnsureResolver(const std::string& libraryRoot, const std::string& modsRoot,
                    const std::string& modCachePath, std::string& err) {
    std::string mods = modsRoot;
    if (g.resolver && libraryRoot + "|" + mods == g.resolverKey) return true;
    if (libraryRoot.empty()) { err = "no asset library: start with --library <dir>"; return false; }
    std::vector<std::string> issues;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo.clear();
    }
    if (!mods.empty()) {
        auto modProgress = [](size_t done, size_t total) {
            // Map the mod-index range (which can be minutes on a 4,000-mod
            // folder) into the first slice of the overall loading bar.
            const double f = total ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
            ProgSet(f >= 0.0 ? f * 0.15 : -1.0, "indexing mods " + std::to_string(done) + " / " + std::to_string(total));
            if (done % 200 == 0 || done == total) Log("indexing mods " + std::to_string(done) + " / " + std::to_string(total));
        };
        auto modCancel = [] { return ProgCancelled(); };
        const size_t buildsBefore = g.modsCache.buildCount();
        resolve::ModIndex::Stats st;
        const resolve::ModIndex* index = &g.modsCache.get(mods, modCachePath, &issues, &st, modProgress, modCancel);
        bool built = g.modsCache.buildCount() != buildsBefore;
        // A game folder instead of its mods folder: look one level down.
        if (index->guidCount() == 0 && Exists(mods + "/mods")) {
            mods += "/mods";
            st = resolve::ModIndex::Stats{};
            const size_t nestedBefore = g.modsCache.buildCount();
            index = &g.modsCache.get(mods, modCachePath, &issues, &st, modProgress, modCancel);
            built = built || g.modsCache.buildCount() != nestedBefore;
            g.settings.mods = mods;   // keep the persisted fallback the old code wrote
            SaveSettings(g.settings);
        }
        if (built) Log("indexing mods in " + mods + " ...");
        else Log("reusing mod index for " + mods);
        ProgSet(0.15, "mods indexed");
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo = std::to_string(index->guidCount()) + " mods found";
        if (index->guidCount() == 0) g.modsInfo = "NO mods found in that folder - press M and pick the folder that holds the .zipmod files";
        Log(g.modsInfo, index->guidCount() ? LogKind::Good : LogKind::Error);
    } else {
        g.modsCache.get(std::string(), modCachePath, &issues);   // drop a stale index when mods are turned off
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo = "no mods folder set - mod items will be missing (press M)";
        Log(g.modsInfo, LogKind::Warn);
        ProgSet(0.15, "no mods folder");
    }
    ProgSet(0.16, "checking asset library");
    auto chk = library::CheckLibrary(libraryRoot, false);
    if (!chk.ok) { err = "asset library problem: " + (chk.issues.empty() ? std::string("unusable") : chk.issues[0]); return false; }
    const auto gameInfo = library::ReadGameInfo(libraryRoot);
    Log("asset library: " + (chk.profile.empty() ? std::string("base") : chk.profile) + " profile (" +
        std::string(gameInfo.name()) + (gameInfo.unityVersion.empty() ? std::string() : ", Unity " + gameInfo.unityVersion) + ")", LogKind::Good);
    g.modsCache.setGame(gameInfo);   // KK-only mods are excluded when the library is KKS; no rebuild
    g.lists = resolve::ListTable();
    g.lists.loadBaseGame(library::AbdataRoot(libraryRoot), &issues);
    g.resolver = std::make_shared<resolve::Resolver>(g.modsCache.index(), g.lists, library::AbdataRoot(libraryRoot), gameInfo);
    if (exporter::LoadColourGrade(libraryRoot)) Log("colour grading loaded (G toggles it)", LogKind::Good);
    g.resolverKey = libraryRoot + "|" + mods;
    return true;
}

float PmxSuggestedYaw(const pmx::Model& doc) {
    // The default camera looks from +Z. A model whose toes extend toward -Z
    // faces away from it, so start that model from the opposite side; models
    // facing +Z keep the default. No geometry is changed.
    auto contains = [](const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; };
    int toe = -1, ankle = -1;
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const std::string a = doc.bones[i].name, b = doc.bones[i].nameEn;
        if (toe < 0 && (contains(a, "つま先") || contains(b, "つま先") || contains(b, "Toe") || contains(a, "Toe"))) toe = static_cast<int>(i);
        if (ankle < 0 && (contains(a, "足首") || contains(b, "足首") || contains(b, "Ankle") || contains(a, "Ankle"))) ankle = static_cast<int>(i);
    }
    if (toe < 0) return 0.0f;
    if (ankle < 0) ankle = doc.bones[static_cast<size_t>(toe)].parent;
    if (ankle < 0 || static_cast<size_t>(ankle) >= doc.bones.size()) return 0.0f;
    const Vec3 t = doc.bones[static_cast<size_t>(toe)].position;
    const Vec3 a = doc.bones[static_cast<size_t>(ankle)].position;
    const float forwardZ = t.z - a.z;
    return forwardZ < -0.01f ? 180.0f : 0.0f;
}

bool IsPmxPath(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "pmx";
}

// Native PMX entry point. Deliberately does not touch the resolver or the
// card: a PMX is a first-class input, not a conversion result.
void StartLoadPmx(const std::string& path) {
    if (g.busy.exchange(true)) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    const std::string prevCardPath = g.cardPath;
    g.cardPath = path;
    g.loadingOutfit = -1;
    g.applyKeep = false;
    g.tweakPending = false;
    g.cam.pitchDeg = 0.0f;
    g.cam.zoom = 1.0f; g.cam.panX = g.cam.panY = 0.0f;
    KillTimer(g.hwnd, kTimerApplyTweaks);
    g.status = "loading " + fileio::FileName(path) + " ...";
    ProgBegin("Loading " + fileio::FileName(path));
    Log("open PMX " + path);
    InvalidatePanel();
    std::thread([path, prevCardPath]() {
        auto t0 = std::chrono::steady_clock::now();
        auto out = std::make_shared<Loaded>();
        bool failed = false;
        try {
            ProgSet(0.05, "reading the PMX");
            out->pmxDoc = std::make_shared<pmx::Model>(pmx::ReadPmxFile(path));
            const size_t slash = path.rfind('/');
            out->pmxDir = slash == std::string::npos ? std::string() : path.substr(0, slash);
            ProgSet(0.45, "building the scene");
            out->suggestedYaw = PmxSuggestedYaw(*out->pmxDoc);
            const pmx::PmxLoadResult built = pmx::BuildScene(*out->pmxDoc, out->pmxDir);
            out->scene = built.scene;
            out->notes = built.warnings;
            out->title = out->pmxDoc->nameEn.empty() ? out->pmxDoc->name : out->pmxDoc->nameEn;
            if (out->title.empty()) out->title = fileio::FileName(path);
            out->partTriangles.assign(out->scene.parts.size(), 0);
            for (size_t pi = 0; pi < out->scene.parts.size(); ++pi)
                for (int mi : out->scene.parts[pi].meshes)
                    for (const auto& sub : out->scene.meshes[static_cast<size_t>(mi)].submeshes)
                        out->partTriangles[pi] += sub.indices.size() / 3;
            char msg[256];
            std::snprintf(msg, sizeof msg, "ready in %.1fs: %s vertices, %s morphs, %s bones",
                          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                          Count(out->scene.meshes.empty() ? 0 : out->scene.meshes[0].vertices.size()).c_str(),
                          Count(out->pmxDoc->morphs.size()).c_str(), Count(out->scene.bones.size()).c_str());
            Log(msg, LogKind::Good);
            for (const std::string& w : built.warnings) Log("  " + w, LogKind::Warn);
        } catch (const std::exception& e) {
            failed = true;
            Log(std::string("PMX load failed: ") + e.what(), LogKind::Error);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (!failed) g.loaded = out;
            else g.cardPath = prevCardPath;
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LOADED, failed ? 1 : 0, 0);
    }).detach();
}

void StartLoad(const std::string& path, int outfit, bool keepTweaks = false) {
    if (IsPmxPath(path)) { StartLoadPmx(path); return; }
    if (g.busy.exchange(true)) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    const std::string prevCardPath = g.cardPath;
    g.cardPath = path;
    // Select the source profile from the card marker BEFORE the worker builds
    // the resolver, so mods and base assets are indexed from the right pair.
    // A reload of the same card keeps a manual profile switch intact.
    const card::CardFamily openingFamily = card::DetectCardFamily(path);
    if (path != prevCardPath) ActivateProfileForCard(openingFamily);
    WarnLibraryMismatch(openingFamily);
    std::vector<ClothingPin> clothingPins;   // the stack copy StartLoad applies, first to last
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        clothingPins = g.clothingPins;
    }
    g.applyKeep = keepTweaks;
    g.keepHidden.clear();
    if (keepTweaks)   // the same card again (another outfit, a re-shape): keep the part checkboxes
        if (std::shared_ptr<Loaded> prev = Current())
            for (size_t i = 0; i < prev->scene.parts.size() && i < g.hidden.size(); ++i) g.keepHidden[prev->scene.parts[i].id] = g.hidden[i];
    g.tweakPending = false;
    KillTimer(g.hwnd, kTimerApplyTweaks);
    g.loadingOutfit = outfit;
    g.status = "loading " + fileio::FileName(path) + " ...";   // the full path is in the log
    ProgBegin("Loading " + fileio::FileName(path));
    Log("open " + fileio::FileName(path));
    InvalidatePanel();
    // Snapshot after profile activation: switching cards/games on the UI thread
    // cannot retarget the source while the worker is indexing.
    const std::string libraryRoot = g.settings.library, modsRoot = g.settings.mods, modCachePath = ModCachePath();
    std::thread([path, outfit, keepTweaks, clothingPins, prevCardPath, libraryRoot, modsRoot, modCachePath]() {
        auto t0 = std::chrono::steady_clock::now();
        auto out = std::make_shared<Loaded>();
        std::string err;
        bool cancelled = false;
        cbtime::Scope totalScope("load: total card load");
        try {
            ProgSet(0.02, "indexing mods");
            if (ProgCancelled()) throw Cancelled{};
            cbtime::Scope resolverScope("load: resolver + mod index");
            if (!EnsureResolver(libraryRoot, modsRoot, modCachePath, err)) throw std::runtime_error(err);
            resolverScope.Stop();
            if (ProgCancelled()) throw Cancelled{};
            ProgSet(0.18, "reading the card");
            // The previous character's bundles are no longer needed (its
            // scene holds copies of everything it extracted): drop them
            // before this character's bundles allocate beside that scene
            // and the new one being built.
            if (g.resolver) g.resolver->clearCaches();
            Log("reading the card container");
            {   // the loaded character changed — any cached preview built against it is invalid now
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                g.sel.proposalCache.clear();
                g.sel.proposalCacheLru.clear();
                g.sel.importedCache.clear();
                g.sel.importedCacheLru.clear();
            }
            cbtime::Scope cardScope("load: read card PNG");
            card::Card c = card::LoadCard(path);
            cardScope.Stop();
            if (!c.supported) throw std::runtime_error(c.supportNote);
            // The card's current coordinate when none is asked for; an
            // out-of-range slot falls back to the first, the way Assembler does.
            int shown = outfit >= 0 ? outfit : static_cast<int>(c.status.at("coordinateType").asInt());
            if (shown < 0 || shown >= static_cast<int>(c.outfits.size())) shown = 0;
            // The pinned clothing layers, in apply order (first to last). A Selective Import pushes a
            // layer each time it is applied, so pieces from several cards compose into one outfit (each
            // reload re-derives the composite from the card on disk — deterministic, in memory only).
            // Replace All and Coordinate imports are single layers by definition. A pinned slot outside
            // this card never applies; switching away and back shows native -> imported -> native.
            std::string clothingLabel;
            bool appliedUnpinned = false;
            int layerIndex = 0;
            for (const ClothingPin& pin : clothingPins) {
                ++layerIndex;
                if (pin.mode == 3) {   // a graft layer, not a merge: built during assembly, counted here for the label
                    if (!pin.path.empty()) clothingLabel += (clothingLabel.empty() ? std::string() : " + ") + pin.name + " (layer)";
                    continue;
                }
                bool applyPin = true;
                if (pin.dstOutfit >= 0) applyPin = pin.dstOutfit == shown && pin.dstOutfit < static_cast<int>(c.outfits.size());
                if (!applyPin) continue;
                // A selective pin whose decision map is empty would re-apply with the *suggested* actions
                // (Replace the lot) on reload — never do that; the import selected replace-your-slot or nothing.
                if (pin.mode == 2 && pin.decisions.empty()) { Log("selective import: skipped an empty pin (nothing to merge)"); continue; }
                const card::ImageSourceKind srcKind = card::DetectImageSource(pin.path);
                ProgSet(0.20, pin.mode == 2 ? "applying selective import" : pin.mode == 1 ? "applying coordinate import" : "applying outfit source (clothing)");
                const std::string layerTag = clothingPins.size() > 1 ? " (layer " + std::to_string(layerIndex) + "/" + std::to_string(clothingPins.size()) + ")" : std::string();
                try {
                    if (pin.mode == 2) {
                        // The selective import: only the components the user chose in the Selective
                        // Import window are merged. An outfit file on disk is never written; the card
                        // itself is only ever this in-memory copy.
                        card::SelectiveDecisions sd;
                        sd.actions = pin.decisions;
                        card::ApplyClothingOptions sopt;
                        sopt.outfit = shown;
                        sopt.srcOutfit = pin.srcOutfit;
                        const card::SelectiveOutfitReport rep = card::ApplySelectiveOutfitSource(c, pin.path, sopt, sd);
                        clothingLabel += (clothingLabel.empty() ? std::string() : " + ") + rep.sourceName + " (selective)";
                        Log("selective" + layerTag + ": " + std::to_string(rep.plan.replaced) + " replaced, " + std::to_string(rep.plan.added) +
                            " added, " + std::to_string(rep.plan.kept) + " kept, " + std::to_string(rep.plan.skipped) + " skipped" +
                            (rep.fromCoordinate ? " (outfit source: the coordinate it was pinned to)" : "") +
                            " -> outfit " + std::to_string(rep.apply.outfit) + "; in memory only, card not saved", LogKind::Good);
                        for (const std::string& note : rep.apply.notes) Log("  " + note);
                    } else if (pin.mode == 1 && srcKind == card::ImageSourceKind::Coordinate) {
                        // A character card used as a source: the whole-coordinate semantics (Status copies included).
                        card::Card src = card::LoadCard(pin.path);
                        if (!src.supported) throw std::runtime_error(src.supportNote);
                        card::ApplyCoordinateOptions copt;
                        copt.srcOutfit = pin.srcOutfit;
                        copt.dstOutfit = shown;
                        const card::ApplyCoordinateReport rep = card::ApplyCoordinate(c, src, copt);
                        clothingLabel += (clothingLabel.empty() ? std::string() : " + ") + rep.sourceName + " - " + src.outfitName(rep.srcOutfit) + " (coordinate)";
                        for (const std::string& note : rep.notes) Log("  coordinate: " + note, LogKind::Warn);
                        Log("coordinate" + layerTag + ": " + rep.sourceName + " (" + src.outfitName(rep.srcOutfit) + ") -> outfit " + std::to_string(rep.dstOutfit) +
                            " (" + std::to_string(rep.clothesReplaced) + " clothes slot(s) replaced, " + std::to_string(rep.accessoriesReplaced) +
                            " accessory slot(s) replaced; the whole coordinate was imported; in memory only, card not saved)");
                    } else {
                        // A Coordinate (outfit) file: as clothing (the merge) or as a coordinate (wholesale).
                        card::ApplyClothingOptions opt;
                        opt.outfit = shown;
                        opt.srcOutfit = pin.srcOutfit;   // -1 for plain files
                        if (pin.mode == 1) opt.coordinateImport = true;
                        const card::OutfitSourceReport rep = card::ApplyOutfitSource(c, pin.path, opt);
                        clothingLabel += (clothingLabel.empty() ? std::string() : " + ") + rep.sourceName + (pin.mode == 1 ? std::string(" (coordinate)") : std::string());
                        if (pin.mode == 1) {
                            Log("coordinate import" + layerTag + ": " + rep.sourceName + " -> outfit " + std::to_string(rep.apply.outfit) +
                                " (the whole coordinate replaced verbatim, including accessories; " + std::to_string(rep.apply.clothesReplaced) +
                                " clothes slot(s) replaced; in memory only, card not saved)");
                        } else {
                            Log(std::string(rep.fromCoordinate ? "coordinate (as clothing): " : "clothing: ") + rep.sourceName +
                                " -> outfit " + std::to_string(rep.apply.outfit) +
                                " (" + std::to_string(rep.apply.clothesReplaced) + " clothes slot(s) replaced, " +
                                std::to_string(rep.apply.accessoriesAdded) + " accessory(ies) added" +
                                (rep.apply.accessoriesRelocated ? ", " + std::to_string(rep.apply.accessoriesRelocated) + " moved to keep hair" : "") +
                                ", " + std::to_string(rep.apply.hairPreserved) + " hair accessories kept; in memory only, card not saved)");
                        }
                    }
                    if (pin.dstOutfit < 0) appliedUnpinned = true;   // --clothes with no --outfit: the layer landed on the shown slot
                } catch (const std::exception& e) {
                    out->notes.push_back(e.what());
                    Log(std::string(pin.mode == 1 ? "coordinate import failed: " : "clothing failed: ") + e.what(), LogKind::Error);
                }
            }
            if (!clothingLabel.empty()) out->clothing = clothingLabel;
            if (appliedUnpinned) {
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                for (ClothingPin& p : g.clothingPins) if (p.dstOutfit < 0) p.dstOutfit = shown;
            }
            // Keep the card's own coordinate-specific ABMX on a normal load: the authored skirt length
            // (e.g. cf_d_sk_top Y=2.0) is part of the card's silhouette and the CLI export uses the same
            // table. The assembler now clamps only the inward X pinch of skirt bones that used to pull
            // fabric into the body, so the long skirt is kept without the clipping. Imports still
            // merge/replace the slot through mergeAbmxPayloads; no other coordinate is touched.
            Log("assembling outfit " + std::to_string(shown) + " (" + c.outfitName(shown) + ") ...");
            assemble::AssembleOptions ao;
            ao.outfit = shown;
            {   // the layers of the pinned imports, first to last (slot order so the suffixes stay stable across reloads)
                std::vector<const ClothingPin*> layerPins;
                for (const ClothingPin& pin : clothingPins) if (pin.mode == 3) layerPins.push_back(&pin);
                std::stable_sort(layerPins.begin(), layerPins.end(),
                                 [](const ClothingPin* a, const ClothingPin* b) { return a->layerSlots.empty() ? 99 : a->layerSlots.front() < (b->layerSlots.empty() ? 99 : b->layerSlots.front()); });
                ao.layers.clear();
                int li = 1;
                for (const ClothingPin* pin : layerPins) {
                    if (pin->layerSlots.empty()) continue;
                    assemble::AssembleLayerSpec spec;
                    spec.path = pin->path;
                    spec.srcOutfit = pin->srcOutfit;
                    spec.dstOutfit = shown;
                    spec.name = pin->name;
                    spec.slots = pin->layerSlots;
                    spec.layerIndex = li++;
                    ao.layers.push_back(std::move(spec));
                }
            }
            bool originalGarment = true;   // Look tab: the card's own garment vertices; off = the clipping correction runs on this scene too
            {   // the sliders: the card's own values unless the user changed them
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                if (!keepTweaks || g.tw.bodyCard.empty()) {
                    g.tw = Tweaks();
                    g.bwhCardDefSet = false;   // the measurement baseline is re-captured from the freshly assembled card
                    g.tw.skinShade = g.settings.skinShade;          // persisted default from the Setup tab (and the Look slider); share value with the settings file
                    g.tw.skinShadeUnder = g.settings.skinShadeUnder;
                    g.tw.skinShadeTop = g.settings.skinShadeTop;
                    g.tw.skinShadeWidth = g.settings.skinShadeWidth;
                    g.tw.skinAo = g.settings.skinAo;
                    g.tw.noseRidge = g.tw.noseRidgeApplied = g.settings.noseRidge;   // persisted default for new cards
                    g.tw.noseRidgeOpacity = g.settings.noseRidgeOpacity;
                    const Value& bv = c.body.at("shapeValueBody");
                    const Value& fv = c.face.at("shapeValueFace");
                    for (size_t i = 0; i < bv.size(); ++i) g.tw.bodyCard.push_back(static_cast<float>(bv.at(i).asFloat()));
                    for (size_t i = 0; i < fv.size(); ++i) g.tw.faceCard.push_back(static_cast<float>(fv.at(i).asFloat()));
                    g.tw.body = g.tw.bodyCard;
                    g.tw.face = g.tw.faceCard;
                    g.tw.areolaCard = g.tw.areola = static_cast<float>(c.body.at("areolaSize").asFloat());
                }
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
                originalGarment = g.originalGarment;
                if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
                ao.proportionLayer = ProportionLayerNow();
            }
            ao.log = [](const std::string& m) { Log("  " + m); };
            ao.progress = [](double f, const std::string& stage) {
                // The assemble spans most of the overall bar; map its 0..1 into 0.22..0.96.
                ProgSet(f >= 0.0 ? 0.22 + f * 0.74 : -1.0, stage);
            };
            ao.cancelled = [] { return ProgCancelled(); };
            if (ProgCancelled()) throw Cancelled{};
            cbtime::Scope assembleScope("load: assemble scene");
            out->scene = assemble::Assemble(c, *g.resolver, ao);
            assembleScope.Stop();
            if (!originalGarment) {
                // The viewer's own copy of the export's clipping correction: the preview shows exactly the garment the PMX carries.
                const auto fix = assemble::FixGarmentIntersections(out->scene);
                Log("  garment intersection fix: " + std::to_string(fix.verticesMoved) +
                    " vertex(es) moved out of the body (Original garment is off)");
            }
            if (ProgCancelled()) throw Cancelled{};
            ProgSet(0.97, "counting triangles");
            out->outfits = static_cast<int>(c.outfits.size());
            out->outfit = shown;
            out->title = c.displayName();
            out->partTriangles.assign(out->scene.parts.size(), 0);
            for (size_t pi = 0; pi < out->scene.parts.size(); ++pi)
                for (int mi : out->scene.parts[pi].meshes)
                    for (const auto& sub : out->scene.meshes[static_cast<size_t>(mi)].submeshes)
                        out->partTriangles[pi] += sub.indices.size() / 3;
            out->card = std::move(c);
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char msg[256];
            std::snprintf(msg, sizeof msg, "ready in %.1fs: %zu parts, %s triangles", secs, out->scene.parts.size(), Count(Triangles(out->scene)).c_str());
            Log(msg, LogKind::Good);
        } catch (const Cancelled&) {
            cancelled = true;
            Log("loading cancelled", LogKind::Warn);
        } catch (const std::exception& e) {
            out->notes.push_back(e.what());
            Log(std::string("load failed: ") + e.what(), LogKind::Error);
        }
        if (!cancelled) {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.loaded = out;
        } else {
            // The old character stays on screen: forget the cancelled file.
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.cardPath = prevCardPath;
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LOADED, cancelled ? 1 : 0, 0);
    }).detach();
}

