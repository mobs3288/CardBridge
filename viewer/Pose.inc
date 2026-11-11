// viewer fragment: posing, physics, load (included in order by main.cpp)

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
    if (includeSag) assemble::AddSagPose(L.scene, p, g.jiggleBust, g.jiggleButt, g.jiggleIntensity, g.sag);
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
// positions the overlay draws. `simSteps` physics updates run first (0 only makes the chains follow the pose).
void ApplyPoseLive(int simSteps = 0) {
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
            g.physicsMoving = g.sim.Step(scene, g.poseWorld, simSteps, g.skin);
            if (simSteps == 0) g.physicsKick = true;   // the anchors moved: let the chains catch up
        }
    }
    g.boneWorld.resize(n);
    for (size_t i = 0; i < n; ++i) g.boneWorld[i] = g.skin.empty() ? scene.bones[i].pos : g.skin[i].point(scene.bones[i].pos);
    const std::vector<Mat4>* sk = g.skin.empty() ? nullptr : &g.skin;
    if (g.gpuOk) g.gpu.SetSkin(sk);
    if (g.expOk) g.gpuExp.SetSkin(sk);
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
    st.aPose = g.aPose;
    st.aPoseDeg = g.aPoseDeg;
    return st;
}

void RestorePose(const PoseState& st) {
    g.boneEdits = st.edits;
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
    size_t unknown = 0;
    for (const auto& [name, e] : pf.bones) {
        if (L->scene.boneIndex(name) < 0) { ++unknown; continue; }
        g.boneEdits[name] = e;
    }
    g.aPose = pf.aPose;
    g.aPoseDeg = pf.aPoseDegrees;
    ApplyPoseLive();
    InvalidatePanel();
    PoseChanged(before);
    Log("loaded the pose from " + path + ": " + std::to_string(pf.bones.size() - unknown) + " bone edit(s)" + (unknown ? ", " + std::to_string(unknown) + " bone(s) this rig does not have skipped" : std::string()), unknown ? LogKind::Warn : LogKind::Good);
}

// The physics switch: on starts every chain at rest under the current pose and a 60 Hz timer; the chains then swing and settle as bones,
// sliders or the A-pose move what they hang from (N gives them a shove).
void SetPhysics(bool on) {
    std::shared_ptr<Loaded> L = Current();
    if (on && (!L || L->scene.chains.empty())) { Log("no physics chains in this character", LogKind::Warn); on = false; }
    g.physics = on;
    if (on) {
        g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggleIntensity);
        g.physicsLast = GetTickCount64();
        g.physicsAccum = 0.0;
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

// One step of the physics preview (Frame calls it every picture while the switch is on): as many 1/60 s updates as the time asks for
// (at most four), drawn once.
void PhysicsTick() {
    if (!g.physics) return;
    const ULONGLONG now = GetTickCount64();
    g.physicsAccum = std::min(0.1, g.physicsAccum + static_cast<double>(now - g.physicsLast) / 1000.0);
    g.physicsLast = now;
    const int steps = std::min(4, static_cast<int>(g.physicsAccum * 60.0));
    if (steps <= 0) return;
    g.physicsAccum -= steps / 60.0;
    if (!g.physicsMoving && !g.physicsKick) return;   // everything has settled: nothing to draw
    g.physicsKick = false;
    ApplyPoseLive(steps);
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

// Re-applies the live skin look (automatic body shading and the nipple tint). `rebuild` recomputes the occlusion map
// from the current body mesh; a plain re-apply only changes the strength. The create texture is pushed to the GPU and
// the software renderer's cached mips are dropped, as a material tint change does.
void ApplySkinLook(bool rebuildMap) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    exporter::ApplyNippleTint(L->scene, NippleTintOption());
    auto& ss = L->scene.skinShade;
    if (ss.ready()) {
        const bool rebuild = rebuildMap;
        if (exporter::ApplySkinShade(L->scene, g.tw.skinShade, g.tw.skinShadeUnder, g.tw.skinShadeTop, rebuild)) {
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
    }
    g.dirty = true;
    InvalidateView();
    InvalidatePanel();
}

// A body or face slider, the nipple switch or offset: re-shapes the assembled scene in place (a few milliseconds, no loading) and re-skins the
// views. `final` (the slider was let go) also rebuilds the normal map frames, which are left alone while dragging. False when it cannot
// be done and the character has to be assembled again.
bool ReshapeNow(bool final) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.busy || !g.resolver || L->scene.bones.empty()) return false;
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
        if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
    }
    const double t0 = NowMs();
    if (!assemble::Reshape(L->scene, L->card, *g.resolver, ao)) return false;
    const double t1 = NowMs();
    if (g.gpuOk) g.gpu.Refresh(final);
    if (final && L->scene.skinShade.ready()) {   // the body moved: re-shade the texture from the new creases
        const int ti = L->scene.skinShade.texture;
        if (exporter::ApplySkinShade(L->scene, g.tw.skinShade, g.tw.skinShadeUnder, g.tw.skinShadeTop, true)) {
            if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
            if (g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(std::vector<int>{ti});
        }
    }
    g.prof.reshape += t1 - t0;
    g.prof.refresh += NowMs() - t1;
    if (g.expOk && g.expScene && g.expFor == L) {   // the baked "as exported" copy keeps its textures; only its shape follows
        assemble::Reshape(*g.expScene, L->card, *g.resolver, ao);
        g.gpuExp.Refresh(final);
    }
    // The body sliders move the rest height: the height editor stays an absolute target, so the scale factor
    // re-targets itself to keep the displayed cm where the user put it.
    g.baseHeightCm = assemble::SceneHeightMeters(L->scene) * 100.0f;
    const float bustSize = g.tw.body.size() > 4 ? g.tw.body[4] : 0.5f;
    g.sagDefault = assemble::CardSagDefault(L->scene, bustSize);  // reset target follows the new shape; the slider itself stays put
    if (g.physics) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggleIntensity);
    ApplyPoseLive();
    return true;
}

bool IsShapeId(int id) { return (id >= 0 && id <= kNippleMm) || id == kAreola || id == kNippleIn; }

// Something a slider or switch controls changed. Pose controls, the shape sliders and the areola size show at once; the character is only
// assembled again when it cannot be re-shaped in place (after the pointer has been still for a moment).
void TweakEdited(int id, bool released) {
    if (IsMatId(id)) { MaterialSliderEdited(id, released); return; }
    if (id == kJiggleBust || id == kJiggleButt || id == kJiggleIntensity) {
        if (g.physics) {   // the preview shows what the export will do: prime the sim again with the new switches
            if (std::shared_ptr<Loaded> L = Current()) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggleIntensity);
            g.physicsKick = g.physicsMoving = true;
            Log(std::string("jiggle preview: boobs ") + (g.jiggleBust ? "on" : "off") + ", butt " + (g.jiggleButt ? "on" : "off"));
        }
        InvalidatePanel();
        return;
    }
    if (id == kSag) {   // static hang: change the pose at once (also re-prime the sim if it is running)
        if (g.physics) {
            if (std::shared_ptr<Loaded> L = Current()) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)), g.jiggleBust, g.jiggleButt, g.jiggleIntensity);
            g.physicsKick = g.physicsMoving = true;
        }
        ApplyPoseLive();
        InvalidatePanel();
        return;
    }
    if (IsLiveId(id)) { ApplyPoseLive(); return; }
    if (id == kSkinShade || id == kSkinShadeUnder || id == kSkinShadeTop ||
        id == kNipplePink || id == kNippleR || id == kNippleG || id == kNippleB) {
        // Body shading re-applies at the new strength at once; the occlusion map is rebuilt only when a shape drag ended
        // (ReshapeNow) or from this release when the body had moved. The nipple tint is a material property and is live.
        if (id == kSkinShade || id == kSkinShadeUnder || id == kSkinShadeTop) {   // persisted default for new cards — save it with the other settings
            if (id == kSkinShade) g.settings.skinShade = g.tw.skinShade;
            else if (id == kSkinShadeUnder) g.settings.skinShadeUnder = g.tw.skinShadeUnder;
            else if (id == kSkinShadeTop) g.settings.skinShadeTop = g.tw.skinShadeTop;
            SaveSettings(g.settings);
        }
        ApplySkinLook(released && Current() && Current()->scene.skinShade.geometryDirty);
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

bool EnsureResolver(std::string& err) {
    std::string key = g.settings.library + "|" + g.settings.mods;
    if (g.resolver && key == g.resolverKey) return true;
    if (g.settings.library.empty()) { err = "no asset library: start with --library <dir>"; return false; }
    std::vector<std::string> issues;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo.clear();
    }
    if (!g.settings.mods.empty()) {
        Log("indexing mods in " + g.settings.mods + " ...");
        auto modProgress = [](size_t done, size_t total) {
            // Map the mod-index range (which can be minutes on a 4,000-mod
            // folder) into the first slice of the overall loading bar.
            const double f = total ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
            ProgSet(f >= 0.0 ? f * 0.15 : -1.0, "indexing mods " + std::to_string(done) + " / " + std::to_string(total));
            if (done % 200 == 0 || done == total) Log("indexing mods " + std::to_string(done) + " / " + std::to_string(total));
        };
        auto modCancel = [] { return ProgCancelled(); };
        g.mods = resolve::ModIndex::Build(g.settings.mods, ModCachePath(), &issues, nullptr, modProgress, modCancel);
        // A game folder instead of its mods folder: look one level down.
        if (g.mods.guidCount() == 0 && Exists(g.settings.mods + "/mods")) {
            g.settings.mods += "/mods";
            g.mods = resolve::ModIndex::Build(g.settings.mods, ModCachePath(), &issues, nullptr, modProgress, modCancel);
            SaveSettings(g.settings);
        }
        ProgSet(0.15, "mods indexed");
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo = std::to_string(g.mods.guidCount()) + " mods found";
        if (g.mods.guidCount() == 0) g.modsInfo = "NO mods found in that folder - press M and pick the folder that holds the .zipmod files";
        Log(g.modsInfo, g.mods.guidCount() ? LogKind::Good : LogKind::Error);
    } else {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo = "no mods folder set - mod items will be missing (press M)";
        Log(g.modsInfo, LogKind::Warn);
        ProgSet(0.15, "no mods folder");
    }
    ProgSet(0.16, "checking asset library");
    auto chk = library::CheckLibrary(g.settings.library, false);
    if (!chk.ok) { err = "asset library problem: " + (chk.issues.empty() ? std::string("unusable") : chk.issues[0]); return false; }
    Log("asset library: " + (chk.profile.empty() ? std::string("base") : chk.profile) + " profile", LogKind::Good);
    g.lists = resolve::ListTable();
    g.lists.loadBaseGame(library::AbdataRoot(g.settings.library), &issues);
    g.resolver = std::make_unique<resolve::Resolver>(g.mods, g.lists, library::AbdataRoot(g.settings.library));
    if (exporter::LoadColourGrade(g.settings.library)) Log("colour grading loaded (G toggles it)", LogKind::Good);
    g.resolverKey = key;
    return true;
}

void StartLoad(const std::string& path, int outfit, bool keepTweaks = false) {
    if (g.busy.exchange(true)) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    const std::string prevCardPath = g.cardPath;
    g.cardPath = path;
    std::string clothingPath;
    int clothingOutfit = -1;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        clothingPath = g.clothingPath;
        clothingOutfit = g.clothingOutfit;
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
    std::thread([path, outfit, keepTweaks, clothingPath, clothingOutfit, prevCardPath]() {
        auto t0 = std::chrono::steady_clock::now();
        auto out = std::make_shared<Loaded>();
        std::string err;
        bool cancelled = false;
        try {
            ProgSet(0.02, "indexing mods");
            if (ProgCancelled()) throw Cancelled{};
            if (!EnsureResolver(err)) throw std::runtime_error(err);
            if (ProgCancelled()) throw Cancelled{};
            ProgSet(0.18, "reading the card");
            // The previous character's bundles are no longer needed (its
            // scene holds copies of everything it extracted): drop them
            // before this character's bundles allocate beside that scene
            // and the new one being built.
            if (g.resolver) g.resolver->clearCaches();
            Log("reading the card container");
            card::Card c = card::LoadCard(path);
            if (!c.supported) throw std::runtime_error(c.supportNote);
            // The card's current coordinate when none is asked for; an
            // out-of-range slot falls back to the first, the way Assembler does.
            int shown = outfit >= 0 ? outfit : static_cast<int>(c.status.at("coordinateType").asInt());
            if (shown < 0 || shown >= static_cast<int>(c.outfits.size())) shown = 0;
            // The outfit file is pinned to the one slot it was dropped onto (--clothes
            // with no --outfit pins to the card's current slot on the first load): it
            // replaces only that slot. Every other slot keeps the card's own clothing,
            // so switching away and back shows native -> imported -> native. A pinned
            // slot outside this card never applies.
            const bool applyClothing = !clothingPath.empty() && (clothingOutfit < 0 || clothingOutfit == shown) &&
                                       clothingOutfit < static_cast<int>(c.outfits.size());
            if (applyClothing) {
                ProgSet(0.20, "applying outfit file (cloth)");
                try {
                    card::ClothingFile cl = card::LoadClothingFile(clothingPath);
                    if (!cl.supported) throw std::runtime_error(cl.supportNote);
                    card::ApplyClothingOptions opt;
                    opt.outfit = shown;
                    auto rep = card::ApplyClothing(c, cl, opt);
                    {
                        std::lock_guard<std::recursive_mutex> lk(g.mu);
                        g.clothingOutfit = rep.outfit;
                        g.clothingName = cl.displayName();   // --clothes arrives without one; the drop path already set it
                    }
                    out->clothing = cl.displayName();
                    Log("clothing: " + cl.displayName() + " -> outfit " + std::to_string(rep.outfit) +
                        " (" + std::to_string(rep.clothesReplaced) + " clothes slot(s) replaced, " +
                        std::to_string(rep.accessoriesAdded) + " accessory(ies) added" +
                        (rep.accessoriesRelocated ? ", " + std::to_string(rep.accessoriesRelocated) + " moved to keep hair" : "") +
                        ", " + std::to_string(rep.hairPreserved) + " hair accessories kept; in memory only, card not saved)");
                } catch (const std::exception& e) {
                    out->notes.push_back(e.what());
                    Log(std::string("clothing failed: ") + e.what(), LogKind::Error);
                }
            }
            Log("assembling outfit " + std::to_string(shown) + " (" + c.outfitName(shown) + ") ...");
            assemble::AssembleOptions ao;
            ao.outfit = shown;
            {   // the sliders: the card's own values unless the user changed them
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                if (!keepTweaks || g.tw.bodyCard.empty()) {
                    g.tw = Tweaks();
                    g.tw.skinShade = g.settings.skinShade;          // persisted default from the Setup tab (and the Look slider); share value with the settings file
                    g.tw.skinShadeUnder = g.settings.skinShadeUnder;
                    g.tw.skinShadeTop = g.settings.skinShadeTop;
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
                ao.autoCloth = g.autoCloth;
                if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
            }
            ao.log = [](const std::string& m) { Log("  " + m); };
            ao.progress = [](double f, const std::string& stage) {
                // The assemble spans most of the overall bar; map its 0..1 into 0.22..0.96.
                ProgSet(f >= 0.0 ? 0.22 + f * 0.74 : -1.0, stage);
            };
            ao.cancelled = [] { return ProgCancelled(); };
            if (ProgCancelled()) throw Cancelled{};
            out->scene = assemble::Assemble(c, *g.resolver, ao);
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

