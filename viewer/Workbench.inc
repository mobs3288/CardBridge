// viewer fragment: the Material Workbench window (included in order by main.cpp)
//
// The Materials tab of the side panel is a launcher: the class filter, the material
// list and the button that opens this window. Everything that edits a material lives
// here - class / preset / texture, RGB + HEX, the HSV colour picker, the shading
// sliders, the texture inspector with its UV canvas, and reset / undo / save / load -
// in a window as big as the viewer's instead of the 380 px panel.
//
// It is a second top-level window of the same thread and the same message loop, so
// there is no second render loop, no second state: it shares App with the viewer.
// WM_CLOSE hides it (the launcher button shows it again and keeps its scroll state);
// only the viewer's WM_DESTROY ends the process, and that destroys this window first.

constexpr wchar_t kWbClassName[] = L"CardBridgeMaterialWorkbench";

void DumpTestLayout();   // the layout dump (Window.inc); this window answers it too

// ---- buttons ----------------------------------------------------------------------------------------------------------
// One list of every button of the workbench: the drawing shows the one under the
// cursor (g.wbHoverBtn), the click code and the cursor find what was hit, and a
// hover repaint knows which two rectangles changed.
enum WbButtonId {
    kWbNone = 0,
    kWbFilter, kWbClass, kWbPreset, kWbTex,
    kWbChan0 = 10,                     // 10..14: the texture channel buttons
    kWbSlot = 15, kWbPick, kWbInspect,
    kWbEnable, kWbReplace, kWbFit,
    kWbResetThis, kWbResetAll, kWbUndo, kWbRedo, kWbSave, kWbLoad
};

struct WbButton { int id = 0; RECT r{}; };

std::vector<WbButton> WbButtons(const WbLayout& l) {
    std::vector<WbButton> b;
    auto add = [&](int id, const RECT& r) { b.push_back({id, r}); };
    add(kWbFilter, l.filter);
    add(kWbClass, l.classBtn);
    add(kWbPreset, l.presetBtn);
    add(kWbTex, l.texBtn);
    for (int i = 0; i < 5; ++i) add(kWbChan0 + i, l.chan[i]);
    add(kWbSlot, l.slotBtn);
    add(kWbPick, l.pickBtn);
    add(kWbInspect, l.inspectBtn);
    add(kWbEnable, l.enableBtn);
    add(kWbReplace, l.replaceBtn);
    add(kWbFit, l.fitBtn);
    add(kWbResetThis, l.resetThis);
    add(kWbResetAll, l.resetAll);
    add(kWbUndo, l.undoBtn);
    add(kWbRedo, l.redoBtn);
    add(kWbSave, l.saveBtn);
    add(kWbLoad, l.loadBtn);
    return b;
}

int WbButtonAt(const WbLayout& l, int x, int y) {
    const POINT p{x, y};
    for (const WbButton& b : WbButtons(l))
        if (PtInRect(&b.r, p)) return b.id;
    return 0;
}

RECT WbButtonRect(const WbLayout& l, int id) {
    for (const WbButton& b : WbButtons(l))
        if (b.id == id) return b.r;
    return RECT{};
}

// ---- layout / invalidation --------------------------------------------------------------------------------------------
// The workbench's layout at its current size; the drawing code, the click code and
// the layout dump all call this, so they cannot disagree about where things are.
WbLayout WbNow() {
    RECT rc{};
    if (g.matHwnd) GetClientRect(g.matHwnd, &rc);
    return WbLayoutFor(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
}

// Repaint a horizontal band of the workbench (a slider drag, a popup, a hover).
void InvalidateWbBand(int top, int bottom) {
    if (!g.matHwnd) return;
    RECT rc;
    GetClientRect(g.matHwnd, &rc);
    const LONG b = std::min<LONG>(rc.bottom, static_cast<LONG>(bottom));
    const LONG t = std::max<LONG>(0, static_cast<LONG>(top));
    if (b > t) {
        RECT band{0, t, rc.right, b};
        InvalidateRect(g.matHwnd, &band, FALSE);
    }
}
void InvalidateWbList() {
    if (!g.matHwnd) return;
    const WbLayout l = WbNow();
    InvalidateWbBand(l.listTop - 4, l.listBottom + 4);
}

// Puts the selection of a material into view in both lists (they scroll separately).
void RevealSelectedMaterial() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.selMaterial < 0) return;
    const std::vector<int> rows = BuildMatRows(*L);
    int idx = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[static_cast<size_t>(i)] == g.selMaterial) { idx = static_cast<int>(i); break; }
    if (idx < 0) return;
    auto reveal = [&](int& target, float pos, int top, int bottom) {
        const int visible = std::max(1, (bottom - top) / kRowH);
        const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visible);
        const int first = static_cast<int>(std::lround(pos));
        if (idx < first) target = idx;
        else if (idx >= first + visible) target = idx - visible + 1;
        target = std::clamp(target, 0, maxScroll);
    };
    if (g.hwnd) {
        const PanelLayout l = Lay(ClientHeight());
        reveal(g.matScroll, g.matPos, l.matListTop, l.matListBottom);
    }
    if (g.matHwnd) {
        const WbLayout l = WbNow();
        reveal(g.wbMatScroll, g.wbMatPos, l.listTop, l.listBottom);
    }
}

// ---- draw: the colour picker ------------------------------------------------------------------------------------------
// The HSV popup: saturation / value square, hue bar, the colour it is on with its hex,
// and Done. Its rectangles come from MatPickerOuter (MaterialsHelpers), so the click
// code drags exactly what this draws.
void DrawMatPicker(ui::Surface& s, const WbLayout& l) {
    const RECT outer = MatPickerOuter(l);
    s.FillRound(outer, 6, ui::kCard);
    s.StrokeRound(outer, 6, ui::kLineHi);
    auto toColor = [](const Vec3& v) {
        return RGB(static_cast<int>(std::lround(std::clamp(v.x, 0.0f, 1.0f) * 255.0f)),
                   static_cast<int>(std::lround(std::clamp(v.y, 0.0f, 1.0f) * 255.0f)),
                   static_cast<int>(std::lround(std::clamp(v.z, 0.0f, 1.0f) * 255.0f)));
    };
    material::EffectiveMaterial ceff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    const Vec3 cur{ceff.baseColor.x, ceff.baseColor.y, ceff.baseColor.z};
    const Vec3 hsv = material::RgbToHsv(cur);
    const RECT sv = MatPickerSV(outer);
    const int cols = 24, rows = 12;
    const float cw = float(sv.right - sv.left) / float(cols), chh = float(sv.bottom - sv.top) / float(rows);
    for (int r = 0; r < rows; ++r) {
        const float v = 1.0f - (r + 0.5f) / float(rows);
        for (int c = 0; c < cols; ++c) {
            const float ss = (c + 0.5f) / float(cols);
            const RECT cell{sv.left + int(c * cw), sv.top + int(r * chh), sv.left + int((c + 1) * cw) + 1, sv.top + int((r + 1) * chh) + 1};
            s.Fill(cell, toColor(material::HsvToRgb(hsv.x, ss, v)));
        }
    }
    s.StrokeRound(sv, 3, ui::kLineHi);
    const float cx = sv.left + hsv.y * float(sv.right - sv.left), cy = sv.top + (1.0f - hsv.z) * float(sv.bottom - sv.top);
    s.FillDisc(cx, cy, 5, RGB(255, 255, 255));
    s.FillDisc(cx, cy, 2, RGB(10, 10, 10));
    const RECT hb = MatPickerHue(outer);
    const int hseg = 12;
    for (int i = 0; i < hseg; ++i) {
        const int x0 = hb.left + int(i * float(hb.right - hb.left) / float(hseg));
        const int x1 = hb.left + int((i + 1) * float(hb.right - hb.left) / float(hseg)) + 1;
        s.Fill(RECT{x0, hb.top, x1, hb.bottom}, toColor(material::HsvToRgb(i / float(hseg) + 0.5f / float(hseg), 1, 1)));
    }
    s.StrokeRound(hb, 6, ui::kLineHi);
    const float hx = hb.left + hsv.x * float(hb.right - hb.left);
    s.FillDisc(hx, hb.top + float(hb.bottom - hb.top) / 2.0f, 6, RGB(255, 255, 255));
    s.FillDisc(hx, hb.top + float(hb.bottom - hb.top) / 2.0f, 3, RGB(10, 10, 10));
    const RECT pv = MatPickerPreview(outer);
    s.FillRound(pv, 4, toColor(cur));
    s.StrokeRound(pv, 4, ui::kLineHi);
    s.Text(g.fontMono, ui::kText, pv.left, pv.bottom + 4, MatHexOf(ceff), nullptr);
    const RECT dn = MatPickerDone(outer);
    s.FillRound(dn, 5, g.wbHoverBtn == kWbPick ? ui::kLineHi : ui::kCardHi);
    s.TextCentered(g.fontUi, ui::kText, dn, "Done");
}

void DrawUvCanvas(ui::Surface& s, const WbLayout& l, const assemble::Scene& scene, const RECT& clip);

// ---- draw: the editor -------------------------------------------------------------------------------------------------
// Heading, class / preset / texture, RGB + HEX, then either the shading sliders or
// the texture inspector, and the reset / undo / save / load row. The same rows the
// side panel used to draw, with the workbench's own track and column positions.
void DrawWorkbenchEditor(ui::Surface& s, const RECT& clip, const WbLayout& l, const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    const int x = l.edX, top = l.edTop, w = l.edW;
    auto ghost = [&](const RECT& r, const std::string& label, bool enabled, int id) {
        s.FillRound(r, 5, g.wbHoverBtn == id ? ui::kLineHi : ui::kCardHi);
        s.TextCentered(g.fontUi, enabled ? ui::kText : ui::kFaint, r, label);
    };
    if (g.selMaterial < 0 || static_cast<size_t>(g.selMaterial) >= g.mat.originals.size()) {
        s.Text(g.fontUi, ui::kSub, x + 2, top + 2, "click a material in the list", nullptr);
        return;
    }
    SyncMatBuf();
    material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    const assemble::SceneMaterial& sm = scene.materials[static_cast<size_t>(g.selMaterial)];
    // Heading: the material, its class, and the shader the card gave it.
    {
        const RECT headClip{x, top, x + w, top + kRowH};
        const bool edited = static_cast<size_t>(g.selMaterial) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(g.selMaterial)] && !g.mat.overrides[static_cast<size_t>(g.selMaterial)]->empty();
        s.Text(g.fontBold, edited ? ui::kAmber : ui::kAccent, x + 2, top + 2,
               ClipText(eff.displayName + " [" + material::ClassName(eff.cls) + "]", 48), &headClip);
        std::string shaderName = sm.shader;
        const std::string prefix = "Shader Forge/";
        if (shaderName.rfind(prefix, 0) == 0) shaderName = shaderName.substr(prefix.size());
        s.TextRight(g.fontSmall, ui::kDim, l.valueR, top + 4, ClipText(shaderName, 34));
        s.Fill(RECT{x, top + kRowH - 3, x + w, top + kRowH - 2}, ui::kLine);
    }
    // Class / preset / texture row.
    {
        s.FillRound(l.classBtn, 5, g.matClassDropOpen || g.wbHoverBtn == kWbClass ? ui::kLineHi : ui::kCardHi);
        s.TextCentered(g.fontUi, ui::kText, l.classBtn, "Class: " + std::string(material::ClassName(eff.cls)) + " v");
        ghost(l.presetBtn, "Preset", true, kWbPreset);
        bool texOn = sm.texture >= 0;
        if (static_cast<size_t>(g.selMaterial) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(g.selMaterial)])
            for (const auto& t : g.mat.overrides[static_cast<size_t>(g.selMaterial)]->textures) if (t.property == "_MainTex") texOn = t.enabled;
        ghost(l.texBtn, texOn ? "MainTex: on" : "MainTex: off", true, kWbTex);
    }
    // RGB sliders (shading view only; the inspector uses these rows for the slot).
    if (!g.matInspectOpen) {
        static const char* const rgbLabels[3] = {"Red", "Green", "Blue"};
        for (int k = 0; k < 3; ++k) {
            const int y = top + (2 + k) * kRowH;
            if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + k, rgbLabels[k], y, l.trackL, l.trackR, l.valueR, x + 2);
        }
    }
    // HEX + the colour picker and the texture view toggle (always: HEX is shared).
    {
        const int y = top + 5 * kRowH;
        if (TouchesY(clip, y, y + kRowH)) {
            const std::string hex = MatHexOf(eff);
            s.Text(g.fontUi, ui::kText, x + 2, y + 2, "HEX", nullptr);
            RECT sw{x + 36, y + 2, x + 68, y + kRowH - 2};
            COLORREF col = RGB(static_cast<int>(std::lround(eff.baseColor.x * 255.0f)), static_cast<int>(std::lround(eff.baseColor.y * 255.0f)), static_cast<int>(std::lround(eff.baseColor.z * 255.0f)));
            s.FillRound(sw, 4, col);
            s.StrokeRound(sw, 4, ui::kLineHi);
            s.Text(g.fontMono, g.matHexEditing ? ui::kAmber : ui::kText, x + 76, y + 2, g.matHexEditing ? (g.matHexBuf + "_") : hex, nullptr);
            s.FillRound(l.pickBtn, 5, g.matPickerOpen || g.wbHoverBtn == kWbPick ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, l.pickBtn, "Color");
            s.FillRound(l.inspectBtn, 5, g.wbHoverBtn == kWbInspect ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, l.inspectBtn, g.matInspectOpen ? "Shading" : "Texture");
        }
    }
    if (!g.matInspectOpen) {
        // The six shading sliders.
        static const char* const shadeLabels[6] = {"Opacity", "Roughness", "Metallic", "Specular", "Emission", "Normal"};
        for (int k = 0; k < 6; ++k) {
            const int y = top + (6 + k) * kRowH;
            if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + 3 + k, shadeLabels[k], y, l.trackL, l.trackR, l.valueR, x + 2);
        }
    } else {
        // The texture inspector: slot + channels, two canvases (preview and UV), the
        // diagnostics, and the texture actions.
        EnsureUvCache(scene);
        const MatTexSlotInfo sel = MatTexSelected(scene);
        const auto slots = MatTexSlots(sm);
        const int cy = top + 2 * kRowH;
        if (TouchesY(clip, cy, cy + 2 * kRowH)) {
            static const char* const ch[5] = {"All", "R", "G", "B", "A"};
            for (int i = 0; i < 5; ++i) {
                const RECT r = l.chan[i];
                s.FillRound(r, 5, (i == g.matTexChannel || g.wbHoverBtn == kWbChan0 + i) ? ui::kLineHi : ui::kCardHi);
                s.TextCentered(g.fontUi, (i == g.matTexChannel) ? ui::kAmber : ui::kText, r, ch[i]);
            }
            s.FillRound(l.slotBtn, 5, g.matTexSlotDropOpen || g.wbHoverBtn == kWbSlot ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, l.slotBtn, "Slot: " + sel.prop + " v");
            const RECT det{x, cy + kRowH, x + w, cy + 3 * kRowH};
            s.Text(g.fontSmall, ui::kSub, det.left, det.top + 2, texNameLine(scene, sel), &det);
            s.Text(g.fontSmall, ui::kDim, det.left, det.top + 22, "drag UV to pan  wheel to zoom  click UV to pick island", &det);
        }
        const RECT pv = l.preview;
        const RECT uv = l.uv;
        if (TouchesY(clip, l.canvasTop, l.canvasTop + l.canvasH)) {
            // Preview thumbnail over the texture, checkerboard under the transparent parts.
            s.FillRound(pv, 4, ui::kCardHi);
            s.StrokeRound(pv, 4, ui::kLineHi);
            const RECT inner{pv.left + 2, pv.top + 2, pv.right - 2, pv.bottom - 2};
            const assemble::SceneTexture* tex = nullptr;
            if (sel.texIdx >= 0 && static_cast<size_t>(sel.texIdx) < scene.textures.size()) tex = &scene.textures[static_cast<size_t>(sel.texIdx)];
            const bool texOk = tex && tex->width > 0 && tex->height > 0 && tex->rgba.size() >= static_cast<size_t>(tex->width) * tex->height * 4;
            if (texOk)   // one bitmap + one blit, not one call per 2 px cell
                s.BlitWrappedTexture(inner, tex->rgba.data(), tex->width, tex->height,
                                     0.0f, 1.0f / float(inner.right - inner.left), 1.0f, -1.0f / float(inner.bottom - inner.top),
                                     false, g.matTexChannel, true);
            else
                s.TextCentered(g.fontUi, ui::kDim, inner, "no texture");
            DrawUvCanvas(s, l, scene, clip);
            // The UV diagnostics under the canvases.
            const auto& r = g.matUvReport;
            char db[160];
            std::snprintf(db, sizeof db, "missing %d  out %d  seams %d  islands %d  overlap %d  stretch %.2f-%.2fx of mean",
                          r.missingUvVerts, r.outOfRangeVerts, r.boundaryEdges, r.islands, r.overlapPairs, r.stretchMin, r.stretchMax);
            s.Text(g.fontSmall, ui::kDim, x, l.canvasTop + l.canvasH + 4, ClipText(db, 90), nullptr);
        }
        // Actions: enable / replace the texture, fit the UV view.
        if (TouchesY(clip, l.enableBtn.top, l.enableBtn.bottom + 4)) {
            CheckBox(s, l.enableBtn.left, l.enableBtn.top + 3, sel.enabled, ui::kAccent);
            s.Text(g.fontUi, ui::kText, l.enableBtn.left + 24, l.enableBtn.top + 2, "Enabled", nullptr);
            ghost(l.replaceBtn, "Replace", true, kWbReplace);
            ghost(l.fitBtn, "Fit UV", true, kWbFit);
        }
        // The slot dropdown (modal, over the canvases).
        if (g.matTexSlotDropOpen) {
            const int n = std::min(6, static_cast<int>(slots.size()));
            const RECT drop = MatTexDropRect(l, n);
            s.FillRound(drop, 5, ui::kCard);
            s.StrokeRound(drop, 5, ui::kLineHi);
            for (int i = 0; i < n; ++i) {
                const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
                if (i == g.matTexSlot || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
                s.Text(g.fontUi, (i == g.matTexSlot) ? ui::kAmber : ui::kText, row.left + 4, row.top + 2, slots[static_cast<size_t>(i)].prop, &row);
            }
        }
    }
    // Reset / undo, with save / load of a materials file on the right.
    {
        ghost(l.resetThis, "reset", true, kWbResetThis);
        ghost(l.resetAll, "reset all", g.mat.IsDirty(), kWbResetAll);
        ghost(l.undoBtn, "undo", material::CanUndo(g.mat), kWbUndo);
        ghost(l.redoBtn, "redo", material::CanRedo(g.mat), kWbRedo);
        ghost(l.saveBtn, "Save", true, kWbSave);
        ghost(l.loadBtn, "Load", true, kWbLoad);
    }
    // The class dropdown and the colour picker, drawn last: they sit over the rows.
    if (g.matClassDropOpen) {
        const RECT drop = MatClassDropRect(l);
        s.FillRound(drop, 5, ui::kCard);
        s.StrokeRound(drop, 5, ui::kLineHi);
        for (int i = 0; i < 11; ++i) {
            const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
            if (i == static_cast<int>(eff.cls) || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
            s.Text(g.fontUi, (i == static_cast<int>(eff.cls)) ? ui::kAmber : ui::kText, row.left + 4, row.top + 2,
                   std::string(material::ClassName(static_cast<material::MaterialClass>(i))) + (static_cast<material::MaterialClass>(i) == eff.cls ? "  ✓" : ""), &row);
        }
    }
    if (g.matPickerOpen) DrawMatPicker(s, l);
}

// The UV canvas of the inspector: the texture as the mesh's UVs read it, the seams in
// amber, and the triangles of the selected island in blue. Everything the canvas draws
// is clipped to the canvas rectangle: repaints only cover that band, so lines drawn
// outside it would linger on screen from an older frame. The caller's clip is restored
// afterwards so the rest of the frame draws normally.
void DrawUvCanvas(ui::Surface& s, const WbLayout& l, const assemble::Scene& scene, const RECT& clip) {
    const RECT uv = l.uv;
    s.FillRound(uv, 4, RGB(14, 16, 24));
    s.StrokeRound(uv, 4, ui::kLineHi);
    s.Begin(uv);   // everything below stays inside the canvas; restored to `clip` at the end
    const int meshIdx = MatUvMeshIndex(scene);
    if (meshIdx >= 0 && g.uvScale <= 0) {
        g.uvScale = std::min(float(uv.right - uv.left), float(uv.bottom - uv.top)) / 1.1f;
        g.uvPanX = (float(uv.right - uv.left) - g.uvScale) / 2.0f;
        g.uvPanY = (float(uv.bottom - uv.top) - g.uvScale) / 2.0f;
    }
    if (meshIdx < 0 || g.uvScale <= 0) { s.TextCentered(g.fontUi, ui::kDim, uv, "no triangles"); s.Begin(clip); return; }
    const auto& mesh = scene.meshes[static_cast<size_t>(meshIdx)];
    const assemble::SceneTexture* tex = nullptr;
    const MatTexSlotInfo ssel = MatTexSelected(scene);
    if (ssel.texIdx >= 0 && static_cast<size_t>(ssel.texIdx) < scene.textures.size()) tex = &scene.textures[static_cast<size_t>(ssel.texIdx)];
    const RECT inn{uv.left + 2, uv.top + 2, uv.right - 2, uv.bottom - 2};
    if (tex && tex->width > 0 && tex->height > 0 && tex->rgba.size() >= static_cast<size_t>(tex->width) * tex->height * 4) {
        // One bitmap + one blit instead of a call per 2 px cell: the same mapping the
        // wireframe uses (Unity v=1 = texture row 0, as SampleBilinear does).
        const float inv = 1.0f / g.uvScale;
        const float uLeft = (float(inn.left - uv.left) - g.uvPanX) * inv;
        const float vTop = 1.0f - (float(inn.top - uv.top) - g.uvPanY) * inv;
        s.BlitWrappedTexture(inn, tex->rgba.data(), tex->width, tex->height, uLeft, inv, vTop, -inv, tex->clamp, 0, false);
    }
    auto toPx = [&](const Vec2& p, float& x, float& y) {
        x = uv.left + p.x * g.uvScale + g.uvPanX;
        y = uv.top + (1.0f - p.y) * g.uvScale + g.uvPanY;
        return std::isfinite(x) && std::isfinite(y);
    };
    // Seams first (amber, under the wireframe).
    for (const auto& e : g.matUvSeams) {
        if (e.first >= mesh.vertices.size() || e.second >= mesh.vertices.size()) continue;
        float x0, y0, x1, y1;
        if (!toPx(mesh.vertices[e.first].uv, x0, y0) || !toPx(mesh.vertices[e.second].uv, x1, y1)) continue;
        if ((x0 < uv.left - 100 && x1 < uv.left - 100) || (x0 > uv.right + 100 && x1 > uv.right + 100)) continue;
        s.Line(x0, y0, x1, y1, ui::kAmber, 2.0f);
    }
    if (g.matUvTrisCache != g.selMaterial) {
        g.matUvTris = material::UvTrisForMaterial(scene, g.selMaterial);
        g.matUvTrisCache = g.selMaterial;
    }
    const auto& tris = g.matUvTris;
    const int shown = std::min(2000, static_cast<int>(tris.size()));
    size_t flat = 0;
    for (const auto& dt : tris) {
        if (static_cast<int>(flat) >= shown) break;
        ++flat;
        if (dt.mesh != meshIdx) continue;
        const auto& sub = mesh.submeshes[static_cast<size_t>(dt.submesh)];
        const size_t b = static_cast<size_t>(dt.tri) * 3;
        if (b + 2 >= sub.indices.size()) continue;
        const uint32_t vi[3] = {sub.indices[b], sub.indices[b + 1], sub.indices[b + 2]};
        float x[3], y[3];
        bool ok = true, out = false;
        for (int i = 0; i < 3; ++i) {
            if (vi[i] >= mesh.vertices.size()) { ok = false; break; }
            const Vec2 p = mesh.vertices[vi[i]].uv;
            if (!toPx(p, x[i], y[i])) { ok = false; break; }
            if (p.x < -0.02f || p.x > 1.02f || p.y < -0.02f || p.y > 1.02f) out = true;
        }
        if (!ok) continue;
        const bool sel = flat - 1 < g.matUvSelMask.size() && g.matUvSelMask[flat - 1];
        const COLORREF c = sel ? RGB(90, 200, 255) : out ? ui::kBad : RGB(190, 200, 220);
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3;
            s.Line(x[i], y[i], x[j], y[j], c, 1.0f);
        }
    }
    if (static_cast<int>(tris.size()) > 2000)
        s.Text(g.fontSmall, ui::kDim, uv.left + 4, uv.bottom - 14, "2000 of " + std::to_string(tris.size()) + " tris", nullptr);
    s.Begin(clip);   // give the frame its clip back for the rows the canvas does not own
}

// ---- draw: the whole window --------------------------------------------------------------------------------------------
void DrawWorkbench(ui::Surface& s, const RECT& clip) {
    std::shared_ptr<Loaded> L = Current();
    const int W = s.width(), H = s.height();
    if (W <= 0 || H <= 0) return;
    const WbLayout l = WbLayoutFor(W, H);
    s.Begin(clip);
    s.Fill(clip, ui::kBg);
    if (L) {
        if (g.mat.originals.size() != L->scene.materials.size()) material::CaptureOriginals(g.mat, L->scene);
        if (g.selMaterial < 0 && !L->scene.materials.empty()) { g.selMaterial = 0; g.matUvTrisCache = -2; }
    }
    // Header: what this window is, and the keys it answers.
    if (TouchesY(clip, 0, 40)) {
        s.Text(g.fontBold, ui::kAccent, 12, 8, "Material Workbench", nullptr);
        s.TextRight(g.fontSmall, ui::kDim, W - 12, 12, "Ctrl+Z undo   Ctrl+S save   Ctrl+O load", nullptr);
    }
    // Left column: the filter, then the material list in its own card.
    s.FillRound(RECT{l.listX0, 68, l.listX1, H - 8}, 8, ui::kCard);
    s.FillRound(l.filter, 5, g.matFilterDropOpen ? ui::kLineHi : g.wbHoverBtn == kWbFilter ? ui::kLineHi : ui::kCardHi);
    {
        std::string fl = "Filter: All v";
        if (g.matFilter >= 0) fl = std::string("Filter: ") + material::ClassName(static_cast<material::MaterialClass>(g.matFilter)) + " v";
        s.TextCentered(g.fontUi, g.matFilter >= 0 ? ui::kAmber : ui::kText, l.filter, fl);
        if (L) {
            const int total = static_cast<int>(L->scene.materials.size());
            const std::string n = std::to_string(BuildMatRows(*L).size()) + " / " + std::to_string(total);
            s.TextRight(g.fontSmall, ui::kDim, l.listX1 - 4, l.filter.top + 4, n);
        }
    }
    if (L) {
        const std::vector<int> rows = BuildMatRows(*L);
        const int visible = std::max(1, (l.listBottom - l.listTop) / kRowH);
        const int maxScroll = std::max(0, static_cast<int>(rows.size()) - visible);
        g.wbMatScroll = std::max(0, std::min(g.wbMatScroll, maxScroll));
        g.wbMatPos = std::clamp(g.wbMatPos, 0.0f, static_cast<float>(maxScroll));
        DrawMatList(s, clip, l.listX0, l.listX1, l.listTop, l.listBottom, g.wbMatPos, g.wbHoverRow, rows, L->scene);
    }
    // Right: the editor in its own card.
    s.FillRound(RECT{l.edX - 6, 40, W - 8, H - 8}, 8, ui::kCard);
    if (L) DrawWorkbenchEditor(s, clip, l, *L);
    else s.Text(g.fontUi, ui::kDim, l.edX + 2, l.edTop + 2, g.busy ? "loading the card ..." : "open a card first", nullptr);
    // The filter dropdown of this list last: it opens over the list.
    if (g.matFilterDropOpen) DrawMatFilterDrop(s, l.filter.left, l.filter.top, l.filter.right - l.filter.left);
    s.End();
}

// ---- paint ------------------------------------------------------------------------------------------------------------
void WbPaint(HWND hwnd) {
    // The damaged area as separate rectangles (like the viewer's Paint): the surface
    // keeps what did not change, so a hover costs a row, not the whole window.
    std::vector<RECT> parts;
    if (HRGN rgn = CreateRectRgn(0, 0, 0, 0)) {
        if (GetUpdateRgn(hwnd, rgn, FALSE) > NULLREGION) {
            const DWORD bytes = GetRegionData(rgn, 0, nullptr);
            std::vector<char> buf(bytes);
            if (bytes && GetRegionData(rgn, bytes, reinterpret_cast<RGNDATA*>(buf.data()))) {
                const RGNDATA* rd = reinterpret_cast<const RGNDATA*>(buf.data());
                const RECT* r = reinterpret_cast<const RECT*>(rd->Buffer);
                parts.assign(r, r + rd->rdh.nCount);
            }
        }
        DeleteObject(rgn);
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        if (g.wb.Resize(rc.right, rc.bottom)) parts.assign(1, rc);   // new (or resized) surface: blank, draw all of it
        else if (parts.empty() || parts.size() > 12) parts.assign(1, ps.rcPaint);
        for (const RECT& p : parts) {
            RECT t;
            if (!IntersectRect(&t, &p, &rc)) continue;
            DrawWorkbench(g.wb, t);
            g.wb.Present(dc, t, t.left, t.top);
        }
    }
    EndPaint(hwnd, &ps);
}

// ---- click ------------------------------------------------------------------------------------------------------------
// What a click in this window does. Popups are modal: a click inside one picks,
// drags or closes it, a click outside closes it and is consumed (the next click acts).
// Returns true when the click was consumed.
bool WbClick(int x, int y, bool right) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return true;
    if (g.mat.originals.size() != L->scene.materials.size()) material::CaptureOriginals(g.mat, L->scene);
    const WbLayout l = WbNow();
    const POINT pt{x, y};
    if (g.matPickerOpen) {
        const RECT outer = MatPickerOuter(l);
        const RECT sv = MatPickerSV(outer), hb = MatPickerHue(outer), dn = MatPickerDone(outer);
        if (!right && PtInRect(&dn, pt)) { g.matPickerOpen = false; g.matPickerDrag = 0; InvalidateWorkbench(); return true; }
        if (!right && PtInRect(&l.pickBtn, pt)) { g.matPickerOpen = false; g.matPickerDrag = 0; InvalidateWorkbench(); return true; }
        if (!right && PtInRect(&sv, pt)) { g.matPickerDrag = 1; ApplyPickerAt(x, y, 1, outer); SetCapture(g.matHwnd); InvalidateWorkbench(); return true; }
        if (!right && PtInRect(&hb, pt)) { g.matPickerDrag = 2; ApplyPickerAt(x, y, 2, outer); SetCapture(g.matHwnd); InvalidateWorkbench(); return true; }
        RECT inner = outer;
        if (PtInRect(&inner, pt)) return true;
        g.matPickerOpen = false; g.matPickerDrag = 0; InvalidateWorkbench(); return true;
    }
    if (g.matClassDropOpen) {
        const RECT drop = MatClassDropRect(l);
        if (!right && PtInRect(&drop, pt)) {
            const int idx = (y - drop.top) / kRowH;
            if (idx >= 0 && idx < 11) SetMatClass(idx);
            g.matClassDropOpen = false; g.matDropHover = -1; InvalidateMat(); return true;
        }
        g.matClassDropOpen = false; g.matDropHover = -1; InvalidateMat(); return true;
    }
    if (g.matFilterDropOpen) {
        const RECT drop = MatFilterDropRect(l.filter.left, l.filter.top, l.filter.right - l.filter.left);
        if (!right && PtInRect(&drop, pt)) {
            const int idx = (y - drop.top) / kRowH;
            if (idx >= 0 && idx < 12) SetMatFilter(idx == 0 ? -1 : idx - 1);
            g.matFilterDropOpen = false; g.matDropHover = -1; InvalidateMat(); return true;
        }
        g.matFilterDropOpen = false; g.matDropHover = -1; InvalidateMat(); return true;
    }
    if (g.matTexSlotDropOpen && g.matInspectOpen && g.selMaterial >= 0) {
        const auto slots = MatTexSlots(L->scene.materials[static_cast<size_t>(g.selMaterial)]);
        const int rows = std::min(6, static_cast<int>(slots.size()));
        const RECT drop = MatTexDropRect(l, rows);
        if (!right && PtInRect(&drop, pt)) {
            const int idx = (y - drop.top) / kRowH;
            if (idx >= 0 && idx < rows) { g.matTexSlot = idx; g.matUvCacheMat = -2; }
            g.matTexSlotDropOpen = false; g.matDropHover = -1; InvalidateWorkbench(); return true;
        }
        g.matTexSlotDropOpen = false; g.matDropHover = -1; InvalidateWorkbench(); return true;
    }
    // Leaving HEX entry: any click that is not on the HEX row ends it.
    if (g.matHexEditing && !(y >= l.edTop + 5 * kRowH && y < l.edTop + 6 * kRowH)) {
        g.matHexEditing = false; g.matHexUndoPushed = false;
    }
    // The filter of this list.
    if (!right && PtInRect(&l.filter, pt)) {
        g.matFilterDropOpen = !g.matFilterDropOpen;
        g.matDropHover = -1;
        InvalidateMat();
        return true;
    }
    // The list: select and scroll it into view in both lists.
    if (x >= l.listX0 && x < l.listX1 && y >= l.listTop && y < l.listBottom) {
        const std::vector<int> rows = BuildMatRows(*L);
        const int idx = (y - l.listTop + ScrollPx(g.wbMatPos)) / kRowH;
        if (idx < 0 || static_cast<size_t>(idx) >= rows.size()) return true;
        g.selMaterial = rows[static_cast<size_t>(idx)];
        g.matUvTrisCache = -2;
        g.matTexSlot = -1; g.matUvCacheMat = -2; g.matTexSlotDropOpen = false;
        SyncMatBuf();
        g.dirty = true;
        RevealSelectedMaterial();
        InvalidateAll();
        return true;
    }
    if (g.selMaterial < 0) return true;
    // Row 1: class dropdown, preset, _MainTex on/off.
    if (y >= l.edTop + kRowH && y < l.edTop + 2 * kRowH && !right) {
        if (PtInRect(&l.classBtn, pt)) {
            g.matClassDropOpen = !g.matClassDropOpen;
            g.matDropHover = -1;
            InvalidateMat();
            return true;
        }
        if (PtInRect(&l.presetBtn, pt)) {
            material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
            material::ApplyPreset(g.mat, static_cast<size_t>(g.selMaterial), eff.cls);
            material::ApplyOverrides(L->scene, g.mat);
            SyncMaterialTextures();
            if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
            g.dirty = true;
            Log("preset: " + std::string(material::ClassName(eff.cls)) + "/Default");
            InvalidateAll();
            return true;
        }
        if (PtInRect(&l.texBtn, pt)) {
            const size_t sel = static_cast<size_t>(g.selMaterial);
            bool on = L->scene.materials[sel].texture >= 0;
            if (sel < g.mat.overrides.size() && g.mat.overrides[sel])
                for (const auto& t : g.mat.overrides[sel]->textures) if (t.property == "_MainTex") on = t.enabled;
            material::SetTextureEnabled(L->scene, g.mat, sel, "_MainTex", !on);
            SyncMaterialTextures();
            if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
            g.dirty = true;
            InvalidateAll();
            return true;
        }
        return true;
    }
    // Row 5: HEX entry, the colour picker, the texture view.
    if (y >= l.edTop + 5 * kRowH && y < l.edTop + 6 * kRowH && !right) {
        if (PtInRect(&l.inspectBtn, pt)) {
            g.matInspectOpen = !g.matInspectOpen;
            g.matTexSlotDropOpen = false;
            g.matDropHover = -1;
            if (g.matInspectOpen) { g.uvScale = 0; g.uvPanX = 0; g.uvPanY = 0; g.matUvCacheMat = -2; }
            InvalidateWorkbench();
            return true;
        }
        if (PtInRect(&l.pickBtn, pt)) {
            g.matPickerOpen = !g.matPickerOpen;
            g.matPickerDrag = 0;
            g.matPickerUndoPushed = false;
            g.matHexEditing = false;
            g.matHexUndoPushed = false;
            InvalidateWorkbench();
            return true;
        }
        {   // anywhere else on the row: start typing the colour
            material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
            g.matHexBuf = MatHexOf(eff);
            g.matHexEditing = true;
            InvalidateWorkbench();
            return true;
        }
        return true;
    }
    // Sliders: RGB in rows 2-4, the six shading sliders in rows 6-11.
    {
        const int row = (y - l.edTop) / kRowH;
        int id = -1;
        if (row >= 2 && row <= 4) id = kMatBase + (row - 2);
        else if (row >= 6 && row <= 11) id = kMatBase + 3 + (row - 6);
        const bool inspectorRows = g.matInspectOpen && row >= 2 && row <= 11;
        if (id >= 0 && !inspectorRows) {
            SliderRef r;
            if (!SliderFor(id, r)) return true;
            if (right) { ResetTweak(id); TweakEdited(id, true); return true; }
            if (GetKeyState(VK_SHIFT) & 0x8000) { BeginSliderEdit(id); return true; }   // type the value with the keyboard
            if (x < l.trackL - 8) return true;
            if (g.sliderDrag != id) material::PushUndo(g.mat);
            g.sliderDrag = id;
            g.dragTrackL = l.trackL;   // ApplyPending reads the track the drag started on
            g.dragTrackR = l.trackR;
            *r.v = SliderValueAt(r, x, l.trackL, l.trackR);
            MaterialSliderEdited(id, false);
            SetCapture(g.matHwnd);
            return true;
        }
    }
    // The inspector's own controls.
    if (g.matInspectOpen && !right) {
        for (int i = 0; i < 5; ++i) {
            const RECT r = l.chan[i];
            if (PtInRect(&r, pt)) { g.matTexChannel = i; InvalidateWorkbench(); return true; }
        }
        if (PtInRect(&l.slotBtn, pt)) { g.matTexSlotDropOpen = !g.matTexSlotDropOpen; g.matDropHover = -1; InvalidateWorkbench(); return true; }
        if (PtInRect(&l.enableBtn, pt)) {
            const MatTexSlotInfo sel = MatTexSelected(L->scene);
            material::SetTextureEnabled(L->scene, g.mat, static_cast<size_t>(g.selMaterial), sel.prop, !sel.enabled);
            SyncMaterialTextures();
            if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
            g.dirty = true;
            InvalidateAll();
            return true;
        }
        if (PtInRect(&l.replaceBtn, pt)) {
            ReplaceMatTexture(MatTexSelected(L->scene).prop);
            return true;
        }
        if (PtInRect(&l.fitBtn, pt)) {
            g.uvScale = std::min(float(l.uv.right - l.uv.left), float(l.uv.bottom - l.uv.top)) / 1.1f;
            g.uvPanX = (float(l.uv.right - l.uv.left) - g.uvScale) / 2.0f;
            g.uvPanY = (float(l.uv.bottom - l.uv.top) - g.uvScale) / 2.0f;
            InvalidateWorkbench();
            return true;
        }
        if (PtInRect(&l.preview, pt)) return true;
        if (PtInRect(&l.uv, pt)) {   // drag to pan; the click on release picks an island
            EnsureUvCache(L->scene);
            g.matUvDrag = 1;
            g.uvDown = pt;
            g.uvPan0X = g.uvPanX;
            g.uvPan0Y = g.uvPanY;
            if (g.uvScale <= 0) g.uvScale = std::min(float(l.uv.right - l.uv.left), float(l.uv.bottom - l.uv.top)) / 1.1f;
            SetCapture(g.matHwnd);
            InvalidateWorkbench();
            return true;
        }
    }
    // Reset / undo, save / load.
    if (!right) {
        if (PtInRect(&l.resetThis, pt)) {
            material::ResetMaterial(g.mat, static_cast<size_t>(g.selMaterial));
            material::ApplyOverrides(L->scene, g.mat);
            SyncMaterialTextures();
        } else if (PtInRect(&l.resetAll, pt)) {
            material::ResetAll(g.mat);
            material::ApplyOverrides(L->scene, g.mat);
            SyncMaterialTextures();
        } else if (PtInRect(&l.undoBtn, pt)) { MaterialUndo(false); return true; }
        else if (PtInRect(&l.redoBtn, pt)) { MaterialUndo(true); return true; }
        else if (PtInRect(&l.saveBtn, pt)) { SaveMatFile(); return true; }
        else if (PtInRect(&l.loadBtn, pt)) { LoadMatFile(); RevealSelectedMaterial(); InvalidateAll(); return true; }
        else return true;
        if (g.expOk) { g.expOk = false; g.showExp = false; g.expFor.reset(); g.expScene.reset(); g.gpuExp.Unload(); }
        SyncMatBuf();
        g.dirty = true;
        InvalidateAll();
        return true;
    }
    return true;
}

// Click (not drag) on the UV canvas: select the island under the point and highlight
// its triangles. The point is in workbench window coordinates.
void WbPickUvIsland(int ux, int uy) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const WbLayout l = WbNow();
    if (!PtInRect(&l.uv, POINT{ux, uy})) return;
    EnsureUvCache(L->scene);
    const int meshIdx = MatUvMeshIndex(L->scene);
    if (meshIdx < 0 || g.uvScale <= 0) return;
    const auto& mesh = L->scene.meshes[static_cast<size_t>(meshIdx)];
    const auto islands = material::UvIslands(mesh);
    const float uu = (ux - l.uv.left - g.uvPanX) / g.uvScale;
    const float vv = 1.0f - (uy - l.uv.top - g.uvPanY) / g.uvScale;
    // The island whose UV bounding box holds the point (a cheap pick).
    int pick = -1;
    for (int ii = 0; ii < static_cast<int>(islands.size()); ++ii) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const auto& id : islands[static_cast<size_t>(ii)]) {
            const auto& sub = mesh.submeshes[static_cast<size_t>(id.submesh)];
            for (int k = 0; k < 3; ++k) {
                const uint32_t vi = sub.indices[static_cast<size_t>(id.tri) * 3 + k];
                if (vi >= mesh.vertices.size()) continue;
                const Vec2 p = mesh.vertices[vi].uv;
                x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
            }
        }
        if (uu >= x0 && uu <= x1 && vv >= y0 && vv <= y1) { pick = ii; break; }
    }
    g.matUvSelMask.clear();
    if (pick >= 0) {
        std::set<std::pair<int, int>> want;
        for (const auto& id : islands[static_cast<size_t>(pick)]) want.insert({id.submesh, id.tri});
        // Index the mask exactly like the triangle list the canvas walks (UvTrisForMaterial,
        // meshes → submeshes → triangles), so the highlight lands on the picked island.
        if (g.matUvTrisCache != g.selMaterial) { g.matUvTris = material::UvTrisForMaterial(L->scene, g.selMaterial); g.matUvTrisCache = g.selMaterial; }
        g.matUvSelMask.assign(g.matUvTris.size(), 0);
        for (size_t i = 0; i < g.matUvTris.size(); ++i)
            if (want.count({g.matUvTris[i].submesh, g.matUvTris[i].tri})) g.matUvSelMask[i] = 1;
    }
}

// ---- the window -------------------------------------------------------------------------------------------------------
LRESULT CALLBACK MatWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: WbPaint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {920, 600};   // the editor keeps its columns at about 900 x 560 of client
        return 0;
    }
    case WM_TEST: {   // the scripted run drives this window too (CARDBRIDGE_TEST_HOOKS)
        static const bool hooks = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
        if (!hooks) return 0;
        if (wp == 10) DumpTestLayout();
        else if (wp == 13) MaterialUndo(false);
        else if (wp == 14) MaterialUndo(true);
        else if (wp == 15) SaveMatFile();
        else if (wp == 16) LoadMatFile();
        return 0;
    }
    case WM_CLOSE:
        // Hiding instead of destroying keeps the scroll position and the drawn surface;
        // the launcher button in the panel brings it back.
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_LBUTTONDOWN: {
        if (g.sliderEdit >= 0) EndSliderEdit(true);   // any click commits the typed value (Shift+click a slider to type)
        WbClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (g.sliderEdit >= 0) { EndSliderEdit(false); return 0; }   // right-click puts the typed value back and is swallowed
        WbClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.sliderDrag >= 0) { g.dragX = x; g.dragPending = true; return 0; }   // applied by Frame, once per picture
        if (g.matPickerDrag != 0 && g.matPickerOpen) {
            const WbLayout l = WbNow();
            const RECT outer = MatPickerOuter(l);
            ApplyPickerAt(x, y, g.matPickerDrag, outer);   // same frame as a slider drag
            InvalidateWbBand(l.edTop + 2 * kRowH, outer.bottom);
            return 0;
        }
        if (g.matUvDrag != 0 && g.matInspectOpen) {
            g.uvPanX = g.uvPan0X + (x - g.uvDown.x);
            g.uvPanY = g.uvPan0Y + (y - g.uvDown.y);
            const WbLayout l = WbNow();
            InvalidateWbBand(l.uv.top, l.uv.bottom);
            return 0;
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        const WbLayout l = WbNow();
        const POINT pt{x, y};
        // Dropdown hover: highlight the row under the cursor.
        if (g.matClassDropOpen || g.matFilterDropOpen || (g.matTexSlotDropOpen && g.matInspectOpen)) {
            RECT drop{};
            if (g.matClassDropOpen) drop = MatClassDropRect(l);
            else if (g.matFilterDropOpen) drop = MatFilterDropRect(l.filter.left, l.filter.top, l.filter.right - l.filter.left);
            else {
                std::shared_ptr<Loaded> LL = Current();
                const auto slots = LL ? MatTexSlots(LL->scene.materials[static_cast<size_t>(g.selMaterial)]) : std::vector<MatTexSlotInfo>{};
                drop = MatTexDropRect(l, std::min(6, static_cast<int>(slots.size())));
            }
            const int nh = PtInRect(&drop, pt) ? (y - drop.top) / kRowH : -1;
            if (nh != g.matDropHover) {
                g.matDropHover = nh;
                InvalidateWbBand(drop.top, drop.bottom);
            }
            return 0;
        }
        const int oldRow = g.wbHoverRow, oldBtn = g.wbHoverBtn;
        g.wbHoverRow = -1;
        if (x >= l.listX0 && x < l.listX1 && y >= l.listTop && y < l.listBottom)
            g.wbHoverRow = (y - l.listTop + ScrollPx(g.wbMatPos)) / kRowH;
        g.wbHoverBtn = WbButtonAt(l, x, y);
        if (oldRow != g.wbHoverRow)
            for (int idx : {oldRow, g.wbHoverRow})
                if (idx >= 0) InvalidateWbBand(l.listTop + idx * kRowH - ScrollPx(g.wbMatPos) - 1,
                                               l.listTop + (idx + 1) * kRowH - ScrollPx(g.wbMatPos) + 1);
        if (oldBtn != g.wbHoverBtn) {
            const RECT a = WbButtonRect(l, oldBtn), b = WbButtonRect(l, g.wbHoverBtn);
            if (oldBtn) InvalidateWbBand(a.top - 2, a.bottom + 2);
            if (g.wbHoverBtn) InvalidateWbBand(b.top - 2, b.bottom + 2);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.wbHoverRow >= 0 || g.wbHoverBtn || g.matDropHover >= 0) {
            g.wbHoverRow = -1;
            g.wbHoverBtn = kWbNone;
            g.matDropHover = -1;
            InvalidateWorkbench();
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP:
        if (g.matPickerDrag != 0) {
            g.matPickerDrag = 0;
            ReleaseCapture();
            InvalidateWorkbench();
            return 0;
        }
        if (g.matUvDrag != 0) {
            const int ux = GET_X_LPARAM(lp), uy = GET_Y_LPARAM(lp);
            const bool still = std::abs(ux - g.uvDown.x) < 4 && std::abs(uy - g.uvDown.y) < 4;
            const int held = g.matUvDrag;
            g.matUvDrag = 0;
            ReleaseCapture();
            if (still && held == 1) WbPickUvIsland(ux, uy);   // a click, not a drag: pick the island
            InvalidateWorkbench();
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
            g.dragTrackL = g.dragTrackR = 0;
            ReleaseCapture();
            TweakEdited(released, true);
            InvalidateList();   // the panel's "edited" marks follow the sliders
            return 0;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        const short d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const WbLayout l = WbNow();
        if (g.matInspectOpen && PtInRect(&l.uv, pt)) {
            if (g.uvScale <= 0) g.uvScale = std::min(float(l.uv.right - l.uv.left), float(l.uv.bottom - l.uv.top)) / 1.1f;
            const float u0 = (pt.x - l.uv.left - g.uvPanX) / g.uvScale;
            const float v0 = 1.0f - (pt.y - l.uv.top - g.uvPanY) / g.uvScale;
            g.uvScale = std::clamp(g.uvScale * (d > 0 ? 1.15f : 1.0f / 1.15f), 10.0f, 4000.0f);
            g.uvPanX = pt.x - l.uv.left - u0 * g.uvScale;
            g.uvPanY = (pt.y - l.uv.top) - (1.0f - v0) * g.uvScale;
            InvalidateWbBand(l.uv.top, l.uv.bottom);
        } else if (pt.x >= l.listX0 && pt.x < l.listX1 && pt.y >= l.listTop && pt.y < l.listBottom) {
            std::shared_ptr<Loaded> L = Current();
            if (!L) return 0;
            const int visible = std::max(1, (l.listBottom - l.listTop) / kRowH);
            const int maxScroll = std::max(0, static_cast<int>(BuildMatRows(*L).size()) - visible);
            g.wbMatScroll = std::max(0, std::min(maxScroll, g.wbMatScroll - (d > 0 ? 3 : -3)));
            InvalidateWbList();
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            const WbLayout l = WbNow();
            std::shared_ptr<Loaded> L = Current();
            bool hand = WbButtonAt(l, p.x, p.y) != 0;
            if (L && p.x >= l.listX0 && p.x < l.listX1 && p.y >= l.listTop && p.y < l.listBottom) hand = true;
            if (g.matInspectOpen && (PtInRect(&l.uv, p) || PtInRect(&l.preview, p))) hand = true;
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN: {
        if (g.sliderEdit >= 0) { SliderEditKey(wp); return 0; }   // the typed box owns the keyboard while it is open
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
                if (g.matHexBuf.empty()) {
                    material::EffectiveMaterial eff = (g.selMaterial >= 0) ? material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial)) : material::EffectiveMaterial{};
                    g.matHexBuf = MatHexOf(eff);
                }
                if (g.matHexBuf.size() == 6 && !g.matHexUndoPushed) g.matHexBuf.clear();   // the first digit replaces the colour
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
            g.matClassDropOpen = false; g.matFilterDropOpen = false; g.matPickerOpen = false; g.matPickerDrag = 0;
            g.matTexSlotDropOpen = false; g.matUvDrag = 0; g.matDropHover = -1;
            ReleaseCapture();
            InvalidateMat();
            return 0;
        }
        if (GetKeyState(VK_CONTROL) & 0x8000) {   // the material shortcuts, wherever focus is
            if (wp == 'Z') MaterialUndo((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            else if (wp == 'Y') MaterialUndo(true);
            else if (wp == 'S') SaveMatFile();
            else if (wp == 'O') { LoadMatFile(); RevealSelectedMaterial(); }
            else return 0;
            InvalidateMat();
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        // No PostQuitMessage: the viewer's window owns the process. Hiding on close
        // means this normally never runs before the viewer's own WM_DESTROY.
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- opening ----------------------------------------------------------------------------------------------------------
// Creates the window on the first click (it mirrors the viewer's size, to its right,
// inside the monitor's work area) and just shows it again after that.
void OpenWorkbench() {
    if (!g.hinst || !g.hwnd) return;
    if (!g.matHwnd) {
        WNDCLASSW wcb{};
        wcb.lpfnWndProc = MatWndProc;
        wcb.hInstance = g.hinst;
        wcb.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcb.hIcon = LoadIconW(g.hinst, MAKEINTRESOURCEW(1));
        wcb.lpszClassName = kWbClassName;
        if (!RegisterClassW(&wcb) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Log("the Material Workbench window could not be created", LogKind::Error);
            return;
        }
        RECT wr{};
        GetWindowRect(g.hwnd, &wr);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w < 960) w = 960;
        if (h < 620) h = 620;
        // the private test instance: the workbench is created off-screen and never activates
        const bool testQuiet = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
        int x = testQuiet ? -32000 : wr.right, y = testQuiet ? -32000 : wr.top;
        if (!testQuiet) {
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                if (x + w > mi.rcWork.right) x = mi.rcWork.left + std::max(0L, (mi.rcWork.right - mi.rcWork.left - w) / 2);
                if (x < mi.rcWork.left) x = mi.rcWork.left;
                if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
                if (y < mi.rcWork.top) y = mi.rcWork.top;
            }
        }
        if (testQuiet) {
            // the private test instance: the workbench stays off-screen and never activates
            g.matHwnd = CreateWindowExW(WS_EX_NOACTIVATE, kWbClassName, L"Material Workbench - CardBridge", WS_OVERLAPPEDWINDOW,
                                        x, y, w, h, g.hwnd, nullptr, g.hinst, nullptr);
        } else {
            g.matHwnd = CreateWindowExW(0, kWbClassName, L"Material Workbench - CardBridge", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                        x, y, w, h, g.hwnd, nullptr, g.hinst, nullptr);
        }
        if (!g.matHwnd) {
            Log("the Material Workbench window could not be created", LogKind::Error);
            return;
        }
        if (testQuiet) ShowWindow(g.matHwnd, SW_SHOWNOACTIVATE);   // mapped but never active
        if (HICON icon = LoadIconW(g.hinst, MAKEINTRESOURCEW(1))) SendMessageW(g.matHwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        Log("Material Workbench open (the Materials tab keeps the list)", LogKind::Good);
    } else {
        const bool testQuiet = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
        if (IsIconic(g.matHwnd)) ShowWindow(g.matHwnd, SW_RESTORE);
        ShowWindow(g.matHwnd, testQuiet ? SW_SHOWNOACTIVATE : SW_SHOW);
    }
    RevealSelectedMaterial();
    InvalidateWorkbench();
    InvalidateList();
    if (!std::getenv("CARDBRIDGE_TEST_HOOKS")) SetForegroundWindow(g.matHwnd);   // the private test instance never steals the user's screen
}

// ---- the layout a scripted run aims at --------------------------------------------------------------------------------
// Written into the "workbench" section of the layout dump (Window.inc) while the
// window exists: its client size, the slider track, every row and button centre and
// the popup rectangles, all in this window's coordinates.
void DumpWorkbenchLayout(nlohmann::json& j) {
    j = nlohmann::json::object();
    if (!g.matHwnd) return;
    RECT rc;
    GetClientRect(g.matHwnd, &rc);
    const WbLayout l = WbLayoutFor(static_cast<int>(rc.right), static_cast<int>(rc.bottom));
    std::shared_ptr<Loaded> L = Current();
    auto mid = [](const RECT& r) { return nlohmann::json{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; };
    j["open"] = IsWindowVisible(g.matHwnd) != 0;
    j["client"] = {rc.right, rc.bottom};
    j["rowH"] = kRowH;
    j["track"] = {l.trackL, l.trackR};
    j["filterBtn"] = mid(l.filter);
    j["listTop"] = l.listTop;
    j["list"] = nlohmann::json::array();
    if (L) {
        const std::vector<int> rows = BuildMatRows(*L);
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.listTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.wbMatPos * kRowH)) + kRowH / 2;
            if (y >= l.listTop && y < l.listBottom)
                j["list"].push_back({{"index", rows[static_cast<size_t>(i)]}, {"y", y}, {"name", L->scene.materials[static_cast<size_t>(rows[static_cast<size_t>(i)])].name},
                                     {"sel", rows[static_cast<size_t>(i)] == g.selMaterial}});
        }
    }
    j["fieldY"] = nlohmann::json::array();
    for (int k = 0; k < kMatCount; ++k) {
        const int row = (k <= 2) ? (2 + k) : (3 + k);
        j["fieldY"].push_back(l.edTop + row * kRowH + kRowH / 2);
    }
    j["hexY"] = l.edTop + 5 * kRowH + kRowH / 2;
    j["classY"] = l.edTop + 1 * kRowH + kRowH / 2;
    j["classBtn"] = mid(l.classBtn);
    j["classDropTop"] = l.classBtn.bottom;
    j["pickBtn"] = mid(l.pickBtn);
    j["inspectBtn"] = mid(l.inspectBtn);
    j["resetY"] = (l.resetThis.top + l.resetThis.bottom) / 2;
    j["resetThis"] = mid(l.resetThis);
    j["resetAll"] = mid(l.resetAll);
    j["undoBtn"] = mid(l.undoBtn);
    j["redoBtn"] = mid(l.redoBtn);
    j["saveBtn"] = mid(l.saveBtn);
    j["loadBtn"] = mid(l.loadBtn);
    j["classDropOpen"] = g.matClassDropOpen;
    j["filterDropOpen"] = g.matFilterDropOpen;
    j["pickerOpen"] = g.matPickerOpen;
    j["inspectOpen"] = g.matInspectOpen;
    j["texChannel"] = g.matTexChannel;
    {
        const RECT pv = l.preview, uv = l.uv;
        j["texPreview"] = {(pv.left + pv.right) / 2, (pv.top + pv.bottom) / 2};
        j["uvCanvas"] = {(uv.left + uv.right) / 2, (uv.top + uv.bottom) / 2};
        if (L && g.selMaterial >= 0) {
            const MatTexSlotInfo ssel = MatTexSelected(L->scene);
            j["texSlot"] = g.matTexSlot;
            j["texIdx"] = ssel.texIdx;
            j["texEnabled"] = ssel.enabled;
            if (ssel.texIdx >= 0 && static_cast<size_t>(ssel.texIdx) < L->scene.textures.size())
                j["texFormat"] = L->scene.textures[static_cast<size_t>(ssel.texIdx)].format;
        }
    }
    {
        const RECT outer = MatPickerOuter(l);
        const RECT sv = MatPickerSV(outer), hb = MatPickerHue(outer);
        j["pickerSV"] = {(sv.left + sv.right) / 2, (sv.top + sv.bottom) / 2};
        j["pickerHue"] = {(hb.left + hb.right) / 2, (hb.top + hb.bottom) / 2};
        j["pickerHueLeft"] = {hb.left + 4, (hb.top + hb.bottom) / 2};
        j["pickerSVHi"] = {sv.left + 4, sv.top + 4};
        j["pickerSVLo"] = {sv.right - 4, sv.bottom - 4};
        const RECT dn = MatPickerDone(outer);
        j["pickerDone"] = {(dn.left + dn.right) / 2, (dn.top + dn.bottom) / 2};
    }
    if (L && g.selMaterial >= 0) {
        const auto eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
        j["cls"] = static_cast<int>(eff.cls);
    }
    j["sel"] = g.selMaterial;
    j["filter"] = g.matFilter;
    j["overrides"] = g.mat.OverrideCount();
    j["undo"] = g.mat.undoStack.size();
    j["redo"] = g.mat.redoStack.size();
}
