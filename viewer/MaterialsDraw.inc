// viewer fragment: Materials tab draw (included in order by main.cpp)

// The Materials tab: filter row, material list, and under it the inspector of
// the selected material (class/preset, RGB + HEX, shading sliders, texture
// enable, reset + undo). All edits go through ApplyOverrides (same frame).
void DrawMaterialsTab(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    if (g.mat.originals.size() != scene.materials.size()) material::CaptureOriginals(g.mat, scene);
    if (g.selMaterial < 0 && !scene.materials.empty()) g.selMaterial = 0;
    // Filter row.
    {
        const int y = l.partsTop;
        std::string fl = "Filter: All v";
        if (g.matFilter >= 0) fl = std::string("Filter: ") + material::ClassName(static_cast<material::MaterialClass>(g.matFilter)) + " v";
        s.FillRound(l.matFilter, 5, g.matFilterDropOpen ? ui::kLineHi : ui::kCardHi);
        s.TextCentered(g.fontUi, g.matFilter >= 0 ? ui::kAmber : ui::kText, l.matFilter, fl);
        const int total = static_cast<int>(scene.materials.size());
        const std::string n = std::to_string(BuildMatRows(L).size()) + " / " + std::to_string(total);
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, y + 4, n);
    }
    // Material list.
    const RECT listClip{6, l.matListTop, kPanel - 18, l.matListBottom};
    const std::vector<int> rows = BuildMatRows(L);
    const int visible = std::max(1, (l.matListBottom - l.matListTop) / kRowH);
    g.matScroll = std::max(0, std::min(g.matScroll, std::max(0, static_cast<int>(rows.size()) - visible)));
    g.matPos = std::clamp(g.matPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip)) {
        s.Begin(sub);
        const ListView v = ViewOf(l.matListTop, l.matListBottom, g.matPos);
        for (int i = 0; i < v.count; ++i) {
            const int idx = v.first + i;
            if (idx >= static_cast<int>(rows.size())) break;
            const int mi = rows[static_cast<size_t>(idx)];
            const int y = l.matListTop - v.offset + i * kRowH;
            const bool sel = mi == g.selMaterial;
            const bool edited = mi >= 0 && static_cast<size_t>(mi) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(mi)] && !g.mat.overrides[static_cast<size_t>(mi)]->empty();
            if (sel || idx == g.hoverRow) {
                s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, sel ? RGB(36, 50, 78) : ui::kCardHi);
                if (sel) s.FillRound(RECT{8, y + 4, 11, y + kRowH - 4}, 1, ui::kAccent);
            }
            material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(mi));
            std::string txt = std::to_string(mi) + " " + eff.displayName + " [" + material::ClassName(eff.cls) + "]";
            const int shared = material::CountSubmeshesForMaterial(scene, mi);
            if (shared > 1) txt += " x" + std::to_string(shared);
            s.Text(g.fontUi, edited ? ui::kAmber : sel ? RGB(240, 246, 255) : ui::kText, 14, y + 2, ClipText(txt, 46), &listClip);
        }
        ScrollBar(s, l.matListTop, l.matListBottom, static_cast<int>(rows.size()), visible, g.matPos);
        s.Begin(clip);
    }
    // Editor.
    s.Fill(RECT{10, l.matEditTop - 3, kPanel - 10, l.matEditTop - 2}, ui::kLine);
    if (g.selMaterial < 0 || static_cast<size_t>(g.selMaterial) >= g.mat.originals.size()) {
        const RECT headClip{10, l.matEditTop, kPanel - 10, l.matEditTop + kRowH};
        s.Text(g.fontUi, ui::kSub, 12, l.matEditTop + 2, "click a material in the list", &headClip);
        return;
    }
    SyncMatBuf();
    material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
    const assemble::SceneMaterial& sm = scene.materials[static_cast<size_t>(g.selMaterial)];
    {
        const RECT headClip{10, l.matEditTop, kPanel - 10, l.matEditTop + kRowH};
        const bool edited = static_cast<size_t>(g.selMaterial) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(g.selMaterial)] && !g.mat.overrides[static_cast<size_t>(g.selMaterial)]->empty();
        s.Text(g.fontBold, edited ? ui::kAmber : ui::kAccent, 12, l.matEditTop + 2, ClipText(eff.displayName + " [" + material::ClassName(eff.cls) + "]", 30), &headClip);
    }
    // Class / preset / texture row.
    {
        const int y = l.matEditTop + 1 * kRowH;
        auto ghost = [&](const RECT& r, const std::string& label, bool enabled) {
            s.FillRound(r, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, enabled ? ui::kText : ui::kFaint, r, label);
        };
        s.FillRound(l.matClassBtn, 5, g.matClassDropOpen ? ui::kLineHi : ui::kCardHi);
        s.TextCentered(g.fontUi, ui::kText, l.matClassBtn, "Class: " + std::string(material::ClassName(eff.cls)) + " v");
        ghost(l.matPresetBtn, "Preset", true);
        bool texOn = sm.texture >= 0;
        // Reflect enable/disable overrides for _MainTex.
        if (static_cast<size_t>(g.selMaterial) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(g.selMaterial)])
            for (const auto& t : g.mat.overrides[static_cast<size_t>(g.selMaterial)]->textures) if (t.property == "_MainTex") texOn = t.enabled;
        ghost(l.matTexBtn, texOn ? "MainTex: on" : "MainTex: off", true);
        // The shader belongs on the material's heading line: drawn in this row it ran under the three buttons.
        std::string shaderName = sm.shader;
        const std::string prefix = "Shader Forge/";
        if (shaderName.rfind(prefix, 0) == 0) shaderName = shaderName.substr(prefix.size());
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.matEditTop + 4, ClipText(shaderName, 22));
    }
    // RGB sliders.
    static const char* const rgbLabels[3] = {"Red", "Green", "Blue"};
    for (int k = 0; k < 3; ++k) {
        const int y = l.matEditTop + (2 + k) * kRowH;
        if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + k, rgbLabels[k], y);
    }
    // HEX + colour picker button.
    {
        const int y = l.matEditTop + 5 * kRowH;
        if (TouchesY(clip, y, y + kRowH)) {
            const std::string hex = MatHexOf(eff);
            s.Text(g.fontUi, ui::kText, 12, y + 2, "HEX", nullptr);
            RECT sw{168, y + 2, 200, y + kRowH - 2};
            COLORREF col = RGB(static_cast<int>(std::lround(eff.baseColor.x * 255.0f)), static_cast<int>(std::lround(eff.baseColor.y * 255.0f)), static_cast<int>(std::lround(eff.baseColor.z * 255.0f)));
            s.FillRound(sw, 4, col);
            s.StrokeRound(sw, 4, ui::kLineHi);
            s.Text(g.fontMono, g.matHexEditing ? ui::kAmber : ui::kText, 208, y + 2, g.matHexEditing ? (g.matHexBuf + "_") : hex, nullptr);
            s.FillRound(l.matPickBtn, 5, g.matPickerOpen ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, l.matPickBtn, "Color");
        }
    }
    // Shading sliders.
    static const char* const shadeLabels[6] = {"Opacity", "Roughness", "Metallic", "Specular", "Emission", "Normal"};
    for (int k = 0; k < 6; ++k) {
        const int y = l.matEditTop + (6 + k) * kRowH;
        if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + 3 + k, shadeLabels[k], y);
    }
    // Reset + undo row.
    {
        auto ghost = [&](const RECT& r, const char* label, bool enabled) {
            s.FillRound(r, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, enabled ? ui::kText : ui::kFaint, r, label);
        };
        ghost(l.matResetThis, "reset", true);
        ghost(l.matResetAll, "reset all", g.mat.IsDirty());
        ghost(l.matUndo, "undo", material::CanUndo(g.mat));
        ghost(l.matRedo, "redo", material::CanRedo(g.mat));
    }
    // Popups drawn last so they sit over the editor/list.
    auto toColor = [](const Vec3& v) { return RGB(static_cast<int>(std::lround(std::clamp(v.x, 0.0f, 1.0f) * 255.0f)), static_cast<int>(std::lround(std::clamp(v.y, 0.0f, 1.0f) * 255.0f)), static_cast<int>(std::lround(std::clamp(v.z, 0.0f, 1.0f) * 255.0f))); };
    if (g.matFilterDropOpen) {
        const RECT drop = MatFilterDropRect(l);
        s.FillRound(drop, 5, ui::kCard);
        s.StrokeRound(drop, 5, ui::kLineHi);
        const char* labels[12] = {"All", "Skin", "Fabric", "Hair", "Eyes", "Eyelashes", "Metal", "Plastic", "Leather", "Glass", "Rubber", "Generic"};
        for (int i = 0; i < 12; ++i) {
            const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
            const int val = i == 0 ? -1 : i - 1;
            if (val == g.matFilter || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
            s.Text(g.fontUi, val == g.matFilter ? ui::kAmber : ui::kText, row.left + 4, row.top + 2, labels[i], &row);
        }
    }
    if (g.matClassDropOpen) {
        const RECT drop = MatClassDropRect(l);
        s.FillRound(drop, 5, ui::kCard);
        s.StrokeRound(drop, 5, ui::kLineHi);
        for (int i = 0; i < 11; ++i) {
            const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
            if (i == static_cast<int>(eff.cls) || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
            s.Text(g.fontUi, (i == static_cast<int>(eff.cls)) ? ui::kAmber : ui::kText, row.left + 4, row.top + 2, std::string(material::ClassName(static_cast<material::MaterialClass>(i))) + (static_cast<material::MaterialClass>(i) == eff.cls ? "  ✓" : ""), &row);
        }
    }
    if (g.matPickerOpen) {
        const RECT outer = MatPickerOuter(l);
        s.FillRound(outer, 6, ui::kCard);
        s.StrokeRound(outer, 6, ui::kLineHi);
        material::EffectiveMaterial ceff = material::EffectiveOf(g.mat, static_cast<size_t>(g.selMaterial));
        const Vec3 cur{ceff.baseColor.x, ceff.baseColor.y, ceff.baseColor.z};
        const Vec3 hsv = material::RgbToHsv(cur);
        const RECT sv = MatPickerSV(l);
        const int cols = 24, rows = 12;
        const float cw = float(sv.right - sv.left) / float(cols), ch = float(sv.bottom - sv.top) / float(rows);
        for (int r = 0; r < rows; ++r) {
            const float v = 1.0f - (r + 0.5f) / float(rows);
            for (int c = 0; c < cols; ++c) {
                const float ss = (c + 0.5f) / float(cols);
                const RECT cell{sv.left + int(c * cw), sv.top + int(r * ch), sv.left + int((c + 1) * cw) + 1, sv.top + int((r + 1) * ch) + 1};
                s.Fill(cell, toColor(material::HsvToRgb(hsv.x, ss, v)));
            }
        }
        s.StrokeRound(sv, 3, ui::kLineHi);
        const float cx = sv.left + hsv.y * float(sv.right - sv.left), cy = sv.top + (1.0f - hsv.z) * float(sv.bottom - sv.top);
        s.FillDisc(cx, cy, 5, RGB(255, 255, 255));
        s.FillDisc(cx, cy, 2, RGB(10, 10, 10));
        const RECT hb = MatPickerHue(l);
        const int hseg = 12;
        for (int i2 = 0; i2 < hseg; ++i2) {
            const int x0 = hb.left + int(i2 * float(hb.right - hb.left) / float(hseg));
            const int x1 = hb.left + int((i2 + 1) * float(hb.right - hb.left) / float(hseg)) + 1;
            s.Fill(RECT{x0, hb.top, x1, hb.bottom}, toColor(material::HsvToRgb(i2 / float(hseg) + 0.5f / float(hseg), 1, 1)));
        }
        s.StrokeRound(hb, 6, ui::kLineHi);
        const float hx = hb.left + hsv.x * float(hb.right - hb.left);
        s.FillDisc(hx, hb.top + float(hb.bottom - hb.top) / 2.0f, 6, RGB(255, 255, 255));
        s.FillDisc(hx, hb.top + float(hb.bottom - hb.top) / 2.0f, 3, RGB(10, 10, 10));
        const RECT pv = MatPickerPreview(l);
        s.FillRound(pv, 4, toColor(cur));
        s.StrokeRound(pv, 4, ui::kLineHi);
        s.Text(g.fontMono, ui::kText, pv.left, pv.bottom + 4, MatHexOf(ceff), nullptr);
        const RECT dn = MatPickerDone(l);
        s.FillRound(dn, 5, ui::kCardHi);
        s.TextCentered(g.fontUi, ui::kText, dn, "Done");
    }
}

