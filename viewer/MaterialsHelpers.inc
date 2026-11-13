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
    InvalidateSliderRow(id);   // the workbench's slider row, and the panel's "edited" count
    (void)released;
}

void MaterialUndo(bool redo) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const bool ok = redo ? material::Redo(g.mat) : material::Undo(g.mat);
    if (!ok) { Log(redo ? "nothing to redo" : "nothing to undo", LogKind::Warn); return; }
    material::ApplyOverrides(L->scene, g.mat);
    SyncMaterialTextures();
    g.dirty = true;
    InvalidateMat();
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
    InvalidateMat();
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
    InvalidateMat();
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
    g.wbMatScroll = 0;
    g.wbMatPos = 0;
    InvalidateMat();
}

// ---- Workbench popup geometry (single source of truth for draw + click) ----
RECT MatClassDropRect(const WbLayout& l) {
    return {l.classBtn.left, l.classBtn.bottom, l.classBtn.left + 190, l.classBtn.bottom + 11 * kRowH};
}
// The filter dropdown, below a filter button at (x, y) of width w: the side panel's
// launcher and the workbench's own list both open one of these.
RECT MatFilterDropRect(int x, int y, int w) {
    return {x, y + kRowH, x + w, y + kRowH + 12 * kRowH};
}
RECT MatTexDropRect(const WbLayout& l, int rows) {
    return {l.slotBtn.left, l.slotBtn.bottom, l.slotBtn.right, l.slotBtn.bottom + rows * kRowH};
}
// The colour picker: a fixed-height popup on the editor's first free row, as wide as
// the editor allows (capped so the SV square never becomes a strip).
RECT MatPickerOuter(const WbLayout& l) {
    const int w = std::min(l.edW, 560);
    return {l.edX, l.canvasTop, l.edX + w, l.canvasTop + 240};
}
RECT MatPickerSV(const RECT& o) { return {o.left + 12, o.top + 12, o.right - 182, o.top + 168}; }
RECT MatPickerHue(const RECT& o) { return {o.left + 12, o.top + 184, o.right - 182, o.top + 212}; }
RECT MatPickerPreview(const RECT& o) { return {o.right - 170, o.top + 12, o.right - 12, o.top + 96}; }
RECT MatPickerDone(const RECT& o) { return {o.right - 170, o.top + 184, o.right - 12, o.top + 212}; }
// Applies the picker colour at a window point inside the SV square or hue bar.
// kind: 1 = SV (keeps the current hue), 2 = hue (keeps s/v).
void ApplyPickerAt(int x, int y, int kind, const RECT& outer) {
    if (g.selMaterial < 0) return;
    material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    Vec3 hsv = material::RgbToHsv(Vec3{eff.baseColor.x, eff.baseColor.y, eff.baseColor.z});
    if (kind == 1) {
        const RECT sv = MatPickerSV(outer);
        hsv.y = std::clamp((x - sv.left) / float(sv.right - sv.left), 0.0f, 1.0f);
        hsv.z = 1.0f - std::clamp((y - sv.top) / float(sv.bottom - sv.top), 0.0f, 1.0f);
    } else {
        const RECT hb = MatPickerHue(outer);
        hsv.x = std::clamp((x - hb.left) / float(hb.right - hb.left), 0.0f, 1.0f);
    }
    if (!g.matPickerUndoPushed) { material::PushUndo(g.mat); g.matPickerUndoPushed = true; }
    ApplyMatRgb(material::HsvToRgb(hsv.x, hsv.y, hsv.z));
}

// The class filter dropdown, drawn below a filter button at (x, y) of width w. Both
// lists own one of these (the panel's launcher and the workbench), so both draw it here.
void DrawMatFilterDrop(ui::Surface& s, int x, int y, int w) {
    const RECT drop = MatFilterDropRect(x, y, w);
    s.FillRound(drop, 5, ui::kCard);
    s.StrokeRound(drop, 5, ui::kLineHi);
    static const char* const labels[12] = {"All", "Skin", "Fabric", "Hair", "Eyes", "Eyelashes", "Metal", "Plastic", "Leather", "Glass", "Rubber", "Generic"};
    for (int i = 0; i < 12; ++i) {
        const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
        const int val = i == 0 ? -1 : i - 1;
        if (val == g.matFilter || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
        s.Text(g.fontUi, val == g.matFilter ? ui::kAmber : ui::kText, row.left + 4, row.top + 2, labels[i], &row);
    }
}

// ---- Texture inspector + UV canvas helpers (Stage 3) ----
// The canvas geometry lives in WbLayout (preview, uv, channel buttons, slot,
// enable / replace / fit), so the draw code and the click code share one source.
struct MatTexSlotInfo { std::string prop; int texIdx = -1; bool enabled = true; };
std::vector<MatTexSlotInfo> MatTexSlots(const assemble::SceneMaterial& m) {
    std::vector<MatTexSlotInfo> out;
    for (const auto& tb : m.texProps) out.push_back({tb.name, tb.texture, tb.texture >= 0});
    if (out.empty() && m.texture >= 0) out.push_back({"_MainTex", m.texture, true});
    if (out.empty()) out.push_back({"_MainTex", -1, false});
    return out;
}
MatTexSlotInfo MatTexSelected(const assemble::Scene& scene) {
    if (g.selMaterial < 0 || static_cast<size_t>(g.selMaterial) >= scene.materials.size()) return {};
    auto slots = MatTexSlots(scene.materials[static_cast<size_t>(g.selMaterial)]);
    if (slots.empty()) return {};
    if (g.matTexSlot < 0 || g.matTexSlot >= static_cast<int>(slots.size())) {
        int pick = 0;
        for (int i = 0; i < static_cast<int>(slots.size()); ++i) if (slots[static_cast<size_t>(i)].prop == "_MainTex") { pick = i; break; }
        g.matTexSlot = pick;
    }
    return slots[static_cast<size_t>(g.matTexSlot)];
}
std::string texNameLine(const assemble::Scene& scene, const MatTexSlotInfo& sel) {
    if (sel.texIdx < 0 || static_cast<size_t>(sel.texIdx) >= scene.textures.size())
        return sel.prop + ": (disabled / no texture)";
    const auto& t = scene.textures[static_cast<size_t>(sel.texIdx)];
    char hb[32];
    std::snprintf(hb, sizeof hb, "%08llx", static_cast<unsigned long long>(t.pixelHash));
    return sel.prop + ": " + t.name + "  " + std::to_string(t.width) + "x" + std::to_string(t.height) + "  " + (t.format.empty() ? "RGBA8" : t.format) + (t.srgb ? "  sRGB" : "  linear") + (t.clamp ? "  clamp" : "  repeat") + "  #" + hb;
}
int MatUvMeshIndex(const assemble::Scene& scene) {
    if (g.selMaterial < 0) return -1;
    for (int mi = 0; mi < static_cast<int>(scene.meshes.size()); ++mi)
        for (const auto& sub : scene.meshes[static_cast<size_t>(mi)].submeshes)
            if (sub.material == g.selMaterial) return mi;
    return -1;
}
void EnsureUvCache(const assemble::Scene& scene) {
    const int meshIdx = MatUvMeshIndex(scene);
    if (meshIdx == g.matUvCacheMesh && g.selMaterial == g.matUvCacheMat && g.matUvCacheMat != -2) return;
    g.matUvCacheMat = g.selMaterial;
    g.matUvCacheMesh = meshIdx;
    g.matUvSeams.clear();
    if (meshIdx < 0) { g.matUvReport = material::UvReport{}; g.matUvSelIsland = -1; return; }
    int tw = 1024, th = 1024;
    const MatTexSlotInfo sel = MatTexSelected(scene);
    if (sel.texIdx >= 0 && static_cast<size_t>(sel.texIdx) < scene.textures.size()) {
        const auto& t = scene.textures[static_cast<size_t>(sel.texIdx)];
        if (t.width > 0) tw = t.width;
        if (t.height > 0) th = t.height;
    }
    const auto& mesh = scene.meshes[static_cast<size_t>(meshIdx)];
    g.matUvReport = material::DiagnoseUv(mesh, tw, th);
    int total = 0, seamTris = 0;
    material::CountSeams(mesh, total, seamTris, &g.matUvSeams);
    g.matUvSelIsland = -1;
}
std::string PickImageFile() {
    if (const char* env = std::getenv("CARDBRIDGE_REPLACE_TEXTURE_PATH")) return Slashes(env);
    wchar_t file[1024] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.matHwnd ? g.matHwnd : g.hwnd;   // the dialog belongs to the window that asked for it
    of.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of) ? Slashes(WideToUtf8(file)) : std::string();
}
void ReplaceMatTexture(const std::string& prop) {
    const std::string path = PickImageFile();
    if (path.empty()) return;
    const Bytes raw = fileio::ReadFile(path);
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!card::DecodeImageBytes(raw, w, h, rgba) || w <= 0 || h <= 0) { Log("replace: '" + path + "' is not a readable image; kept original", LogKind::Warn); return; }
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0) return;
    assemble::SceneTexture tex;
    tex.name = fileio::FileName(path);
    tex.source = "replaced (viewer)";
    tex.format = "PNG (replaced)";
    tex.width = w; tex.height = h; tex.rgba = std::move(rgba); tex.srgb = true;
    const int idx = material::ReplaceTexture(L->scene, g.mat, static_cast<size_t>(g.selMaterial), prop, tex);
    if (idx < 0) return;
    SyncMaterialTextures();
    if (g.gpuOk && g.gpuScene == L) g.gpu.EnsureTexture(idx);
    Log("replaced " + prop + ": " + std::to_string(w) + "x" + std::to_string(h) + " (texture " + std::to_string(idx) + ")", LogKind::Good);
    g.dirty = true;
    InvalidateView();
    InvalidateMat();
}

std::string MatPathFromUser(bool save) {
    if (const char* env = std::getenv("CARDBRIDGE_MATERIALS_PATH")) return Slashes(env);
    wchar_t file[1024] = L"materials.json";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.matHwnd ? g.matHwnd : g.hwnd;   // owned by the workbench when it is open
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
    InvalidateMat();
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
    if (tab == TabBody) {
        addSliders(shape::BodySliders(), 0, g.tw.body.size());
        // The band + cup pair for the Bust > Size row, right under it (inserting below the Size row, newest first).
        auto it = std::find_if(rows.begin(), rows.end(), [](const TweakRow& r) { return r.kind == 1 && r.id == kBodyBust; });
        if (it != rows.end()) {
            const int at = static_cast<int>(std::distance(rows.begin(), it)) + 1;
            rows.insert(rows.begin() + at, TweakRow{6, kBustCup, "Cup:", "Bust"});
            rows.insert(rows.begin() + at, TweakRow{6, kBustBand, "Band:", "Bust"});
        }
    }
    else if (tab == TabFace) addSliders(shape::FaceSliders(), kFaceBase, g.tw.face.size());
    else if (tab == TabLook) {
        rows.push_back({0, kHeightCm, "Height (cm): scales everything to that height", "Height"});
        rows.push_back({1, kHeightCm, "Height (cm)", "Height"});
        rows.push_back({0, -1, "Skin shading: automatic creases (cleavage, under the breasts, navel)", "Skin shading"});
        rows.push_back({1, kSkinShade, "Cleavage shading", "Skin shading"});
        rows.push_back({1, kSkinShadeUnder, "Fold under the breasts", "Skin shading"});
        rows.push_back({1, kSkinShadeTop, "Cleavage length", "Skin shading"});
        rows.push_back({1, kSkinShadeWidth, "Shading width", "Skin shading"});
        rows.push_back({1, kSkinAo, "Koikatsu AO (base body normal + detail lines)", "Skin shading"});
        rows.push_back({0, -1, "Translucent clothes: paint the see-through area on the garment", "Translucent clothes"});
        rows.push_back({2, kTransBrush, "Translucency brush: left-drag on a garment paints (right-drag erases)", "Translucent clothes"});
        rows.push_back({2, kTransErase, "Erase the paint instead of laying it down", "Translucent clothes"});
        rows.push_back({1, kTransSize, "Brush size (cm)", "Translucent clothes"});
        rows.push_back({1, kTransStrength, "How see-through the painted area is", "Translucent clothes"});
        rows.push_back({4, 0, "the garment moves to the transparent pass: the skin (or a bra) shows through", "Translucent clothes"});
        rows.push_back({5, kTransClear, "Clear the brush from every garment", "Translucent clothes"});
        rows.push_back({0, kNippleIn, "Nipples", "Nipples"});
        rows.push_back({2, kNippleIn, "Push the nipples into the body", "Nipples"});
        rows.push_back({1, kNippleMm, "Nipple out / in (mm)", "Nipples"});
        rows.push_back({6, kNippleSize, "Size:", "Nipples"});   // S .. XL presets for the nipple offset above
        rows.push_back({1, kAreola, "Areola size", "Nipples"});
        rows.push_back({2, kNipplePink, "Automatic pink nipple / areola (off: the card's colour)", "Nipples"});
        rows.push_back({1, kNippleR, "Nipple colour: red", "Nipples"});
        rows.push_back({1, kNippleG, "Nipple colour: green", "Nipples"});
        rows.push_back({1, kNippleB, "Nipple colour: blue", "Nipples"});
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
    else if (tab == TabMorphs) {
        static const char* const kSectionName[5] = {"", "Brows", "Eyes", "Mouth", "Other"};
        static const char* const kSubName[3] = {"Other", "Left", "Right"};   // SideOf: 0 / 1 / 2 -> display order Left, Right, Other
        size_t secCount[5] = {}, subCount[5][3] = {};
        for (const auto& m : g.morphCatalog.morphs) {
            const int p = m.panel >= 1 && m.panel <= 4 ? m.panel : 4;
            const int s = SideOf(m.name);
            secCount[p]++; subCount[p][s]++;
        }
        const size_t on = MorphsOn();
        rows.push_back({4, 0, "the " + std::to_string(g.morphCatalog.morphs.size()) + " morphs the exported PMX carries; sliders play them live (0..100)", "Morphs"});
        rows.push_back({4, 0, "Shift+click a row to rename it (display only, saved for you); click a header to fold it", "Morphs"});
        rows.push_back({5, kMorphResetAll, on ? "reset every morph (" + std::to_string(on) + " on)" : "reset every morph", "Morphs"});
        if (g.morphCatalog.morphs.empty()) {
            rows.push_back({4, 0, "this head carries no blend shapes (the face does not animate)", "Other"});
            return rows;
        }
        // The fold of a sub-group follows its section's: a folded section shows nothing under it.
        for (int p = 1; p <= 4; ++p) {
            if (secCount[p] == 0) continue;
            const std::string secKey = kSectionName[p];
            const bool secFolded = g.collapsedMorphs.count(secKey) != 0;
            rows.push_back({0, p, std::string(kSectionName[p]) + "  (" + std::to_string(secCount[p]) + ")", secKey});
            if (secFolded) continue;
            for (int si = 0; si < 3; ++si) {   // Left, Right, Other
                const int s = si == 0 ? 1 : si == 1 ? 2 : 0;
                if (subCount[p][s] == 0) continue;
                const std::string subKey = secKey + "/" + kSubName[s];
                const bool subFolded = g.collapsedMorphs.count(subKey) != 0;
                rows.push_back({0, 10 + p * 4 + s, std::string(kSubName[s]) + "  (" + std::to_string(subCount[p][s]) + ")", subKey});
                if (subFolded) continue;
                for (size_t i = 0; i < g.morphCatalog.morphs.size(); ++i) {
                    const auto& m = g.morphCatalog.morphs[i];
                    if ((m.panel >= 1 && m.panel <= 4 ? m.panel : 4) != p) continue;
                    if (SideOf(m.name) != s) continue;
                    rows.push_back({1, kMorphBase + static_cast<int>(i), MorphDisplayName(m.name), subKey});
                }
            }
        }
        rows.push_back({4, 0, "bones move in the Bones tab, the physics in the Look tab (Y)", "Other"});
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
        rows.push_back({0, -1, "Export defaults (new cards)", "Export"});
        rows.push_back({1, kSkinShade, "Cleavage shading (new cards)", "Export"});
        rows.push_back({1, kSkinShadeUnder, "Underboob fold shading (new cards)", "Export"});
        rows.push_back({1, kSkinShadeTop, "Cleavage shading length (new cards)", "Export"});
        rows.push_back({1, kSkinShadeWidth, "Cleavage shading width (new cards)", "Export"});
        rows.push_back({1, kSkinAo, "Koikatsu AO (new cards)", "Export"});
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
    if (id == kTransBrush) return g.transBrush;
    if (id == kTransErase) return g.transErase;
    if (id == kNipplePink) return !g.tw.pinkNipples;
    return SliderFor(id, r) && std::fabs(*r.v - r.def) > 1e-4f;
}

bool AnyTweak() {
    for (int i = 0; i < kFaceBase; ++i) if (TweakChanged(i)) return true;
    for (int i = kFaceBase; i < kNippleMm; ++i) if (TweakChanged(i)) return true;
    if (TweakChanged(kNippleMm) || TweakChanged(kAreola) || g.tw.nippleInside) return true;
    if (TweakChanged(kSkinShade) || TweakChanged(kSkinShadeUnder) || TweakChanged(kSkinShadeTop) ||
        TweakChanged(kSkinShadeWidth) || TweakChanged(kSkinAo) ||
        TweakChanged(kNipplePink) || TweakChanged(kNippleR) ||
        TweakChanged(kNippleG) || TweakChanged(kNippleB) ||
        TweakChanged(kTransSize) || TweakChanged(kTransStrength) || g.transBrush) return true;
    if (g.mat.IsDirty()) return true;
    for (float w : g.morphWeights) if (std::fabs(w) > 1e-4f) return true;   // a live blend shape of the exported PMX
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
    else if (id == kTransBrush || id == kTransErase) {
        if (id == kTransBrush) g.transBrush = false; else g.transErase = false;
        TransSwitchChanged(id);   // the toggle reverts the bake at once (the paint is kept)
    }
    else if (id == kNipplePink) g.tw.pinkNipples = true;
    else if (SliderFor(id, r)) *r.v = r.def;
}

void ScheduleApplyTweaks() {
    g.tweakPending = true;
    SetTimer(g.hwnd, kTimerApplyTweaks, 800, nullptr);
    InvalidateList();
}

