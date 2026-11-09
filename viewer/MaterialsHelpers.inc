// viewer fragment: materials helpers, picker, save/load (included in order by main.cpp)

// ---- materials (Stage 2) ----------------------------------------------------------------------------------------------
// The workbench state lives in App; these helpers keep slider buffers in sync
// with the effective material and apply buffer changes to the override.
bool IsMatId(int id) { return id >= kMatBase && id < kMatBase + kMatCount; }

std::vector<int> BuildMatRows(const Loaded& L) {
    std::vector<int> rows;
    for (size_t i = 0; i < L.scene.materials.size(); ++i) {
        if (g.matFilter >= 0) {
            material::EffectiveMaterial e = (i < g.mat.originals.size()) ? material::EffectiveOf(g.mat, i) : material::EffectiveMaterial{};
            if (static_cast<int>(e.cls) != g.matFilter) continue;
        }
        rows.push_back(static_cast<int>(i));
    }
    return rows;
}

void SyncMatBuf() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0 || static_cast<size_t>(g.selMaterial) >= g.mat.originals.size()) return;
    material::EffectiveMaterial e = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    const float vals[kMatCount] = {e.baseColor.x * 255.0f, e.baseColor.y * 255.0f, e.baseColor.z * 255.0f, e.opacity, e.roughness, e.metallic, e.specularPower, e.emissionIntensity, e.normalIntensity};
    for (int k = 0; k < kMatCount; ++k) {
        const int id = kMatBase + k;
        if (g.sliderDrag != id) g.matBuf[k] = vals[k];
    }
}

// ApplyOverrides can rewrite a material's tint copy of its main texture (the
// game's skin / outfit shaders ignore _Color). Push those pixels to the GPU
// view and drop the software renderer's cached mips so the next frame uses
// them. Only textures that actually changed are touched; a roughness drag on
// the same material reuses the copy.
void SyncMaterialTextures() {
    if (g.mat.changedTextures.empty()) return;
    std::shared_ptr<Loaded> L = Current();
    if (!L) { g.mat.changedTextures.clear(); return; }
    if (g.state)
        for (int ti : g.mat.changedTextures) exporter::InvalidateViewTexture(*g.state, ti);
    if (g.gpuOk && g.gpuScene == L && !g.gpu.UpdateTextures(g.mat.changedTextures))
        Log("materials: the tinted texture could not be uploaded to the GPU; the CPU view still shows it", LogKind::Warn);
    g.mat.changedTextures.clear();
}

void MaterialSliderEdited(int id, bool released) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0) return;
    const size_t sel = static_cast<size_t>(g.selMaterial);
    if (sel >= g.mat.originals.size()) return;
    if (g.mat.overrides.size() != g.mat.originals.size()) g.mat.overrides.resize(g.mat.originals.size());
    if (!g.mat.overrides[sel]) g.mat.overrides[sel] = material::MaterialOverride{};
    material::MaterialOverride& o = *g.mat.overrides[sel];
    material::EffectiveMaterial cur = material::EffectiveOf(g.mat, sel);
    const int k = id - kMatBase;
    if (k >= 0 && k <= 2) {
        float a = cur.baseColor.w;
        if (g.mat.overrides[sel]->opacity) a = *g.mat.overrides[sel]->opacity;
        o.baseColor = Vec4{g.matBuf[0] / 255.0f, g.matBuf[1] / 255.0f, g.matBuf[2] / 255.0f, a};
    } else if (k == 3) { o.opacity = std::clamp(g.matBuf[3], 0.0f, 1.0f); }
    else if (k == 4) { o.roughness = std::clamp(g.matBuf[4], 0.0f, 1.0f); }
    else if (k == 5) { o.metallic = std::clamp(g.matBuf[5], 0.0f, 1.0f); }
    else if (k == 6) { o.specularPower = std::clamp(g.matBuf[6], 0.0f, 1.0f); }
    else if (k == 7) { o.emissionIntensity = std::clamp(g.matBuf[7], 0.0f, 2.0f); if (!o.emissionColor) o.emissionColor = cur.emissionColor; }
    else if (k == 8) { o.normalIntensity = std::clamp(g.matBuf[8], 0.0f, 2.0f); }
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    if (g.expOk) {
        g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload();
        Log("the material changed: press X to bake the as-exported view again");
    }
    g.dirty = true;
    InvalidateView();
    if (released) InvalidatePanel(); else InvalidatePanel();
}

void MaterialUndo(bool redo) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const bool ok = redo ? material::Redo(g.mat) : material::Undo(g.mat);
    if (!ok) { Log(redo ? "nothing to redo" : "nothing to undo", LogKind::Warn); return; }
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    g.dirty = true;
    InvalidatePanel();
    InvalidateView();
    Log(std::string(redo ? "redo" : "undo") + " (" + std::to_string(g.mat.undoStack.size()) + " step(s) to undo, " + std::to_string(g.mat.redoStack.size()) + " to redo)");
}

std::string MatHexOf(const material::EffectiveMaterial& e) {
    char b[16];
    std::snprintf(b, sizeof b, "%02X%02X%02X", static_cast<int>(std::lround(e.baseColor.x * 255.0f)), static_cast<int>(std::lround(e.baseColor.y * 255.0f)), static_cast<int>(std::lround(e.baseColor.z * 255.0f)));
    return b;
}

void ApplyMatHex(const std::string& hex6) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0 || hex6.size() != 6) return;
    auto hv = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    int v = 0;
    for (char c : hex6) { const int d = hv(c); if (d < 0) return; v = v * 16 + d; }
    const size_t sel = static_cast<size_t>(g.selMaterial);
    if (sel >= g.mat.originals.size()) return;
    if (g.mat.overrides.size() != g.mat.originals.size()) g.mat.overrides.resize(g.mat.originals.size());
    if (!g.mat.overrides[sel]) g.mat.overrides[sel] = material::MaterialOverride{};
    material::EffectiveMaterial cur = material::EffectiveOf(g.mat, sel);
    g.mat.overrides[sel]->baseColor = Vec4{((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, cur.baseColor.w};
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
    g.dirty = true;
    InvalidateView();
    InvalidatePanel();
}

// The colour picker's apply: same as HEX but from a picked RGB. Kept separate so
// HEX typing keeps its own undo handling; the picker pushes once per session.
void ApplyMatRgb(const Vec3& rgb) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0) return;
    const size_t sel = static_cast<size_t>(g.selMaterial);
    if (sel >= g.mat.originals.size()) return;
    if (g.mat.overrides.size() != g.mat.originals.size()) g.mat.overrides.resize(g.mat.originals.size());
    if (!g.mat.overrides[sel]) g.mat.overrides[sel] = material::MaterialOverride{};
    material::EffectiveMaterial cur = material::EffectiveOf(g.mat, sel);
    g.mat.overrides[sel]->baseColor = Vec4{std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f), std::clamp(rgb.z, 0.0f, 1.0f), cur.baseColor.w};
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
    g.dirty = true;
    InvalidateView();
    const PanelLayout l = Lay(ClientHeight());
    InvalidatePanelBand(l.matEditTop, l.matEditTop + kMatEditRows * kRowH);
}

// Direct class pick (dropdown). One undo step; the class itself is only a label
// plus metadata — the shading numbers come from the Preset button.
void SetMatClass(int clsInt) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0) return;
    const size_t sel = static_cast<size_t>(g.selMaterial);
    if (sel >= g.mat.originals.size()) return;
    auto next = static_cast<material::MaterialClass>(std::clamp(clsInt, 0, 10));
    material::PushUndo(g.mat);
    if (g.mat.overrides.size() != g.mat.originals.size()) g.mat.overrides.resize(g.mat.originals.size());
    if (!g.mat.overrides[sel]) g.mat.overrides[sel] = material::MaterialOverride{};
    g.mat.overrides[sel]->class_ = next;
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
    g.dirty = true;
    Log(std::string("class: ") + L->scene.materials[sel].name + " -> " + material::ClassName(next));
    InvalidateAll();
}

// Direct filter pick (dropdown). Not an undoable edit: only the list filter.
void SetMatFilter(int filter) {
    g.matFilter = filter;
    g.matScroll = 0;
    g.matPos = 0;
    InvalidatePanel();
}

// ---- Materials tab popup geometry (single source of truth for draw + click) ----
RECT MatClassDropRect(const PanelLayout& l) {
    const int y = l.matEditTop + 2 * kRowH;
    return {10, y, 200, y + 11 * kRowH};
}
RECT MatFilterDropRect(const PanelLayout& l) {
    const int y = l.partsTop + kRowH;
    return {10, y, 200, y + 12 * kRowH};
}
RECT MatPickerOuter(const PanelLayout& l) {
    const int y = l.matEditTop + 6 * kRowH;
    return {10, y, kPanel - 10, y + 136};
}
RECT MatPickerSV(const PanelLayout& l) {
    const int y = l.matEditTop + 6 * kRowH;
    return {20, y + 8, 200, y + 88};
}
RECT MatPickerHue(const PanelLayout& l) {
    const int y = l.matEditTop + 6 * kRowH;
    return {20, y + 96, 200, y + 110};
}
RECT MatPickerPreview(const PanelLayout& l) {
    const int y = l.matEditTop + 6 * kRowH;
    return {210, y + 8, 360, y + 48};
}
RECT MatPickerDone(const PanelLayout& l) {
    const int y = l.matEditTop + 6 * kRowH;
    return {210, y + 112, 360, y + 128};
}
// Applies the picker colour at a window point inside the SV square or hue bar.
// kind: 1 = SV (keeps the current hue), 2 = hue (keeps s/v).
void ApplyPickerAt(int x, int y, int kind, const PanelLayout& l) {
    if (g.selMaterial < 0) return;
    material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    Vec3 hsv = material::RgbToHsv(Vec3{eff.baseColor.x, eff.baseColor.y, eff.baseColor.z});
    if (kind == 1) {
        const RECT sv = MatPickerSV(l);
        hsv.y = std::clamp((x - sv.left) / float(sv.right - sv.left), 0.0f, 1.0f);
        hsv.z = 1.0f - std::clamp((y - sv.top) / float(sv.bottom - sv.top), 0.0f, 1.0f);
    } else {
        const RECT hb = MatPickerHue(l);
        hsv.x = std::clamp((x - hb.left) / float(hb.right - hb.left), 0.0f, 1.0f);
    }
    if (!g.matPickerUndoPushed) { material::PushUndo(g.mat); g.matPickerUndoPushed = true; }
    ApplyMatRgb(material::HsvToRgb(hsv.x, hsv.y, hsv.z));
}

std::string MatPathFromUser(bool save) {
    if (const char* env = std::getenv("CARDBRIDGE_MATERIALS_PATH")) return Slashes(env);
    wchar_t file[1024] = L"materials.json";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"CardBridge materials (*.json)\0*.json\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrDefExt = L"json";
    of.lpstrTitle = save ? L"Save materials" : L"Load materials";
    of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    return ok ? Slashes(WideToUtf8(file)) : std::string();
}

void SaveMatFile() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    const std::string path = MatPathFromUser(true);
    if (path.empty()) return;
    try {
        material::MaterialFile f = material::MakeMaterialFile(L->scene, g.mat, L->card.sha256);
        const std::string text = material::MaterialFileToJson(f);
        fileio::WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        Log("saved " + std::to_string(f.entries.size()) + " material edit(s) to " + path, LogKind::Good);
    } catch (const std::exception& e) { Log(std::string("could not save materials: ") + e.what(), LogKind::Error); }
}

void LoadMatFile(const std::string& given = std::string()) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    const std::string path = given.empty() ? MatPathFromUser(false) : given;
    if (path.empty()) return;
    material::MaterialFile f;
    std::string err;
    try {
        const Bytes raw = fileio::ReadFile(path);
        if (!material::MaterialFileFromJson(std::string(raw.begin(), raw.end()), f, &err)) { Log("not a materials file: " + err, LogKind::Error); return; }
    } catch (const std::exception& e) { Log(std::string("could not read materials: ") + e.what(), LogKind::Error); return; }
    material::PushUndo(g.mat);
    std::vector<std::string> unknown;
    const int n = material::ApplyMaterialFile(L->scene, g.mat, f, &unknown);
    SyncMaterialTextures();
    for (const auto& u : unknown) Log("materials: '" + u + "' is not in this scene; skipped", LogKind::Warn);
    material::ReportUnsupported(L->scene, g.mat);
    g.dirty = true;
    InvalidatePanel();
    InvalidateView();
    Log("loaded " + std::to_string(n) + " material edit(s) from " + path, LogKind::Good);
}

std::string Shorten(const std::string& s, size_t maxChars);

std::vector<TweakRow> BuildTweakRows(int tab) {
    std::vector<TweakRow> rows;
    auto addSliders = [&](const std::vector<shape::SliderInfo>& list, int base, size_t have) {
        std::string cur;
        for (size_t i = 0; i < list.size() && i < have; ++i) {
            if (cur != list[i].group) { cur = list[i].group; rows.push_back({0, base + static_cast<int>(i), cur, cur}); }
            rows.push_back({1, base + static_cast<int>(i), list[i].label, cur});
        }
    };
    if (tab == TabBody) addSliders(shape::BodySliders(), 0, g.tw.body.size());
    else if (tab == TabFace) addSliders(shape::FaceSliders(), kFaceBase, g.tw.face.size());
    else if (tab == TabLook) {
        rows.push_back({0, kHeightCm, "Height (cm): scales everything to that height", "Height"});
        rows.push_back({1, kHeightCm, "Height (cm)", "Height"});
        rows.push_back({0, kNippleIn, "Nipples", "Nipples"});
        rows.push_back({2, kNippleIn, "Push the nipples into the body", "Nipples"});
        rows.push_back({1, kNippleMm, "Nipple out / in (mm)", "Nipples"});
        rows.push_back({1, kAreola, "Areola size", "Nipples"});
        rows.push_back({0, -1, "Pose (shows at once, the export carries it)", "Pose"});
        rows.push_back({2, kAPose, "Automatic A-pose: arms down from the T-pose", "Pose"});
        rows.push_back({1, kAPoseDeg, "A-pose angle", "Pose"});
        rows.push_back({0, -1, "Physics preview (Y; N shakes it)", "Physics"});
        rows.push_back({2, kPhysics, "Simulate hair, skirts and ribbons live", "Physics"});
        rows.push_back({0, kJiggleBust, "Jiggle (the exported PMX, and the physics preview)", "Jiggle"});
        rows.push_back({2, kJiggleBust, "Breast jiggle", "Jiggle"});
        rows.push_back({2, kJiggleButt, "Butt jiggle", "Jiggle"});
        rows.push_back({1, kJiggleIntensity, "Jiggle intensity", "Jiggle"});
        rows.push_back({1, kSag, "Sag", "Jiggle"});
        rows.push_back({0, kAutoCloth, "Auto cloth (added bones + physics where parts have none; reloads the card)", "Auto cloth"});
        rows.push_back({2, kAutoCloth, "Auto cloth: sleeves / wings / tails", "Auto cloth"});
        rows.push_back({0, -1, "Shader layers (on top of the game's shaders)", "Shader layers"});
        if (g.layerDefs.empty()) rows.push_back({4, 0, "none found: put .hlsl files in the layers folder", "Shader layers"});
        for (size_t i = 0; i < g.layerDefs.size(); ++i)
            rows.push_back({2, kLayerBase + static_cast<int>(i), g.layerDefs[i].name, "Shader layers"});
    }
    else if (tab == TabSettings) {
        auto shown = [](const std::string& v, const char* none) { return v.empty() ? std::string(none) : Shorten(v, 50); };
        rows.push_back({0, -1, "Asset library (the game's data)", "Library"});
        rows.push_back({4, 0, shown(g.settings.library, "(not set)"), "Library"});
        rows.push_back({5, kSetLibChoose, "Use another library folder ...", "Library"});
        rows.push_back({5, kSetLibBuild, "Build a library from my Koikatsu folder ...", "Library"});
        rows.push_back({0, -1, "Mods", "Mods"});
        rows.push_back({4, 0, g.settings.noMods ? std::string("(no mods: base-game items only)") : shown(g.settings.mods, "(not set)"), "Mods"});
        rows.push_back({5, kSetModsChoose, "Choose the mods folder ...", "Mods"});
        rows.push_back({5, kSetModsReindex, "Rebuild the mod index (after adding mods)", "Mods"});
        rows.push_back({5, kSetModsClear, "Use no mods", "Mods"});
        rows.push_back({0, -1, "Export", "Export"});
        rows.push_back({4, 0, std::string("PMX info always names: ") + CARDBRIDGE_AUTHOR, "Export"});
        rows.push_back({4, 0, "exports go to: " + shown(g.settings.exportDir, "(asks every time)"), "Export"});
        rows.push_back({5, kSetExportDir, "Always export into one folder ...", "Export"});
        rows.push_back({5, kSetExportAsk, "Ask for the folder every time", "Export"});
        rows.push_back({0, -1, "Shader layers", "Layers"});
        rows.push_back({4, 0, "extra folder: " + shown(g.settings.layers, "(none)"), "Layers"});
        rows.push_back({5, kSetLayers, "Choose a shader layers folder ...", "Layers"});
        rows.push_back({5, kSetLayersDefault, "Use only the default places", "Layers"});
        rows.push_back({0, -1, "Graphics (GPU when there is one, CPU fallback otherwise)", "Graphics"});
        rows.push_back({4, 0, "device: " + Shorten(g.gpu.adapter(), 44), "Graphics"});
        rows.push_back({4, 0, "use: " + g.settings.gpu + "  (auto picks a discrete NVIDIA/AMD card first; applies at once)", "Graphics"});
        auto pick = [](bool active, const std::string& label) { return std::string(active ? "[x] " : "[ ] ") + label; };
        rows.push_back({5, kSetGpuAuto, pick(g.settings.gpu == "auto", "auto (discrete GPU first)"), "Graphics"});
        rows.push_back({5, kSetGpuHardware, pick(g.settings.gpu == "hardware", "hardware only (no WARP fallback)"), "Graphics"});
        rows.push_back({5, kSetGpuWarp, pick(g.settings.gpu == "warp", "WARP on CPU (no real GPU needed)"), "Graphics"});
        rows.push_back({5, kSetGpuSoftware, pick(g.settings.gpu == "software", "software renderer (CPU fallback)"), "Graphics"});
        rows.push_back({4, 0, "skin: " + g.settings.skin + (g.gpu.ready() && g.gpu.gpuSkin() ? "  (this card: GPU compute shader)" : g.gpu.ready() ? "  (this card: CPU loop)" : ""), "Graphics"});
        rows.push_back({5, kSetSkinGpu, pick(g.settings.skin == "gpu", "skin on GPU (compute shader)"), "Graphics"});
        rows.push_back({5, kSetSkinAuto, pick(g.settings.skin == "auto", "skin auto (CPU for small meshes, GPU for large)"), "Graphics"});
        rows.push_back({5, kSetSkinCpu, pick(g.settings.skin == "cpu", "skin on CPU"), "Graphics"});
        rows.push_back({0, -1, "CardBridge", std::string("v") + CARDBRIDGE_VERSION});
        rows.push_back({5, kSetOpenFolder, "Open the settings folder in Explorer", "CardBridge"});
        rows.push_back({5, kSetForget, "Forget all settings (asked again next start)", "CardBridge"});
    }
    return rows;
}

bool TweakChanged(int id) {
    SliderRef r;
    if (id >= kLayerBase && id < kBoneEdit) return g.gpu.layerEnabled(static_cast<size_t>(id - kLayerBase));
    if (id == kNippleIn) return g.tw.nippleInside;
    if (id == kAPose) return g.aPose;
    if (id == kPhysics) return g.physics;
    if (id == kJiggleBust) return g.jiggleBust;
    if (id == kJiggleButt) return g.jiggleButt;
    if (id == kAutoCloth) return g.autoCloth;
    return SliderFor(id, r) && std::fabs(*r.v - r.def) > 1e-4f;
}

bool AnyTweak() {
    for (int i = 0; i < kFaceBase; ++i) if (TweakChanged(i)) return true;
    for (int i = kFaceBase; i < kNippleMm; ++i) if (TweakChanged(i)) return true;
    if (TweakChanged(kNippleMm) || TweakChanged(kAreola) || g.tw.nippleInside) return true;
    if (g.mat.IsDirty()) return true;
    return false;
}

void ResetTweak(int id) {
    SliderRef r;
    if (IsMatId(id)) {
        std::shared_ptr<Loaded> L = Current();
        if (!L || g.selMaterial < 0) return;
        const int k = id - kMatBase;
        material::EffectiveMaterial orig = (static_cast<size_t>(g.selMaterial) < g.mat.originals.size()) ? material::EffectiveOf(g.mat.originals[static_cast<size_t>(g.selMaterial)], nullptr) : material::EffectiveMaterial{};
        float def = 0;
        if (k == 0) def = orig.baseColor.x * 255.0f;
        else if (k == 1) def = orig.baseColor.y * 255.0f;
        else if (k == 2) def = orig.baseColor.z * 255.0f;
        else if (k == 3) def = orig.opacity;
        else if (k == 4) def = orig.roughness;
        else if (k == 5) def = orig.metallic;
        else if (k == 6) def = orig.specularPower;
        else if (k == 7) def = orig.emissionIntensity;
        else if (k == 8) def = orig.normalIntensity;
        g.matBuf[k] = def;
        material::PushUndo(g.mat);
        // MaterialSliderEdited rebuilds baseColor from all three RGB buffers, so
        // the untouched channels (still synced to effective) are preserved.
        MaterialSliderEdited(id, false);
        return;
    }
    if (id == kNippleIn) g.tw.nippleInside = false;
    else if (id == kAPose) g.aPose = true;
    else if (id == kJiggleBust) g.jiggleBust = true;
    else if (id == kJiggleButt) g.jiggleButt = true;
    else if (id == kAutoCloth) g.autoCloth = false;
    else if (SliderFor(id, r)) *r.v = r.def;
}

void ScheduleApplyTweaks() {
    g.tweakPending = true;
    SetTimer(g.hwnd, kTimerApplyTweaks, 800, nullptr);
    InvalidateList();
}

