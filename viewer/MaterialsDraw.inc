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
    // RGB sliders (shading view only).
    if (!g.matInspectOpen) {
        static const char* const rgbLabels[3] = {"Red", "Green", "Blue"};
        for (int k = 0; k < 3; ++k) {
            const int y = l.matEditTop + (2 + k) * kRowH;
            if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + k, rgbLabels[k], y);
        }
    }
    // HEX + colour picker button (always: it holds the view toggle).
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
            s.FillRound(l.matInspectBtn, 5, g.matInspectOpen ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, l.matInspectBtn, g.matInspectOpen ? "Shading" : "Texture");
        }
    }
    // Shading sliders (shading view) or the texture inspector (inspector view).
    if (!g.matInspectOpen) {
        static const char* const shadeLabels[6] = {"Opacity", "Roughness", "Metallic", "Specular", "Emission", "Normal"};
        for (int k = 0; k < 6; ++k) {
            const int y = l.matEditTop + (6 + k) * kRowH;
            if (TouchesY(clip, y, y + kRowH)) DrawSliderRow(s, kMatBase + 3 + k, shadeLabels[k], y);
        }
    } else {
        EnsureUvCache(scene);
        const MatTexSlotInfo sel = MatTexSelected(scene);
        const auto slots = MatTexSlots(sm);
        const int yChan = l.matEditTop + 2 * kRowH;
        // Channel + slot row (rows 2).
        {
            const char* ch[5] = {"All", "R", "G", "B", "A"};
            for (int i = 0; i < 5; ++i) {
                const RECT r = MatTexChannelRect(l, i);
                s.FillRound(r, 5, (i == g.matTexChannel) ? ui::kLineHi : ui::kCardHi);
                s.TextCentered(g.fontUi, (i == g.matTexChannel) ? ui::kAmber : ui::kText, r, ch[i]);
            }
            const RECT sr = MatTexSlotRect(l);
            s.FillRound(sr, 5, g.matTexSlotDropOpen ? ui::kLineHi : ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, sr, "Slot: " + sel.prop + " v");
        }
        // Slot detail rows (rows 3-4).
        {
            const RECT det{10, yChan + kRowH, 370, yChan + 3 * kRowH};
            std::string line2 = texNameLine(scene, sel);
            s.Text(g.fontSmall, ui::kSub, det.left, det.top + 2, line2, &det);
            s.Text(g.fontSmall, ui::kDim, det.left, det.top + 22, "drag UV to pan  wheel to zoom  click UV to pick island", &det);
        }
        // Canvases (rows 6-9).
        const RECT pv = MatTexPreviewRect(l);
        const RECT uv = MatUvRect(l);
        auto texCol = [&](COLORREF c, int a) { return ui::kText; };
        (void)texCol;
        // Preview.
        {
            s.FillRound(pv, 4, ui::kCardHi);
            s.StrokeRound(pv, 4, ui::kLineHi);
            const RECT inner{pv.left + 2, pv.top + 2, pv.right - 2, pv.bottom - 2};
            const int cw = 2, chh = 2;
            const int cols = (inner.right - inner.left) / cw, rows = (inner.bottom - inner.top) / chh;
            const assemble::SceneTexture* tex = nullptr;
            if (sel.texIdx >= 0 && static_cast<size_t>(sel.texIdx) < scene.textures.size()) tex = &scene.textures[static_cast<size_t>(sel.texIdx)];
            for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
                const bool dark = ((r / 4) + (c / 4)) % 2 == 0;
                COLORREF base = dark ? RGB(32, 34, 44) : RGB(52, 56, 70);
                COLORREF col = base;
                if (tex && tex->width > 0 && tex->height > 0 && tex->rgba.size() >= static_cast<size_t>(tex->width) * tex->height * 4) {
                    const int tx = std::min(tex->width - 1, std::max(0, int(c * tex->width / (double)std::max(1, cols))));
                    const int ty = std::min(tex->height - 1, std::max(0, int(r * tex->height / (double)std::max(1, rows))));
                    const uint8_t* p = &tex->rgba[(static_cast<size_t>(ty) * tex->width + tx) * 4];
                    const int R = p[0], G = p[1], B = p[2], A = p[3];
                    if (g.matTexChannel == 0) col = RGB((R * A + GetRValue(base) * (255 - A)) / 255, (G * A + GetGValue(base) * (255 - A)) / 255, (B * A + GetBValue(base) * (255 - A)) / 255);
                    else if (g.matTexChannel == 4) { const int g1 = A; col = RGB(g1, g1, g1); }
                    else { const int g1 = g.matTexChannel == 1 ? R : g.matTexChannel == 2 ? G : B; col = RGB(g1, g1, g1); }
                }
                s.Fill(RECT{inner.left + c * cw, inner.top + r * chh, inner.left + (c + 1) * cw + 1, inner.top + (r + 1) * chh + 1}, col);
            }
            if (!tex) s.TextCentered(g.fontUi, ui::kDim, inner, "no texture");
        }
        // UV canvas.
        {
            s.FillRound(uv, 4, RGB(14, 16, 24));
            s.StrokeRound(uv, 4, ui::kLineHi);
            const int meshIdx = MatUvMeshIndex(scene);
            if (meshIdx >= 0 && g.uvScale <= 0) { g.uvScale = std::min(float(uv.right - uv.left), float(uv.bottom - uv.top)) / 1.1f; g.uvPanX = (float(uv.right - uv.left) - g.uvScale) / 2.0f; g.uvPanY = (float(uv.bottom - uv.top) - g.uvScale) / 2.0f; }
            if (meshIdx >= 0 && g.uvScale > 0) {
                const auto& mesh = scene.meshes[static_cast<size_t>(meshIdx)];
                const MatTexSlotInfo ssel = sel;
                const assemble::SceneTexture* tex = nullptr;
                if (ssel.texIdx >= 0 && static_cast<size_t>(ssel.texIdx) < scene.textures.size()) tex = &scene.textures[static_cast<size_t>(ssel.texIdx)];
                const RECT inn{uv.left + 2, uv.top + 2, uv.right - 2, uv.bottom - 2};
                const int cw = 2, chh = 2;
                const int cols = (inn.right - inn.left) / cw, rows = (inn.bottom - inn.top) / chh;
                for (int r = 0; r < rows; ++r) for (int c = 0; c < cols; ++c) {
                    const float u = ((inn.left + c * cw + 1 - uv.left) - g.uvPanX) / g.uvScale;
                    const float v = 1.0f - ((inn.top + r * chh + 1 - uv.top) - g.uvPanY) / g.uvScale;
                    COLORREF col = RGB(22, 24, 34);
                    if (tex && tex->width > 0 && tex->height > 0 && tex->rgba.size() >= static_cast<size_t>(tex->width) * tex->height * 4) {
                        float uu = u, vv = v;
                        if (tex->clamp) { uu = std::clamp(uu, 0.0f, 1.0f); vv = std::clamp(vv, 0.0f, 1.0f); }
                        else { uu -= std::floor(uu); vv -= std::floor(vv); }
                        const int tx = std::min(tex->width - 1, std::max(0, int(uu * tex->width)));
                        const int ty = std::min(tex->height - 1, std::max(0, int(vv * tex->height)));
                        const uint8_t* p = &tex->rgba[(static_cast<size_t>(ty) * tex->width + tx) * 4];
                        col = RGB((p[0] * p[3] + 22 * (255 - p[3])) / 255, (p[1] * p[3] + 24 * (255 - p[3])) / 255, (p[2] * p[3] + 34 * (255 - p[3])) / 255);
                    }
                    s.Fill(RECT{inn.left + c * cw, inn.top + r * chh, inn.left + (c + 1) * cw + 1, inn.top + (r + 1) * chh + 1}, col);
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
                const auto tris = material::UvTrisForMaterial(scene, g.selMaterial);
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
                    bool sel = flat - 1 < g.matUvSelMask.size() && g.matUvSelMask[flat - 1];
                    const COLORREF c = sel ? RGB(90, 200, 255) : out ? ui::kBad : RGB(190, 200, 220);
                    for (int i = 0; i < 3; ++i) {
                        const int j = (i + 1) % 3;
                        s.Line(x[i], y[i], x[j], y[j], c, 1.0f);
                    }
                }
                if (static_cast<int>(tris.size()) > 2000) s.Text(g.fontSmall, ui::kDim, uv.left + 4, uv.bottom - 14, "2000 of " + std::to_string(tris.size()) + " tris", nullptr);
            } else {
                s.TextCentered(g.fontUi, ui::kDim, uv, "no triangles");
            }
        }
        // Info + diagnostics (one line each, small font).
        {
            const int yInfo = MatUvRect(l).bottom + 4;
            std::string texLine = texNameLine(scene, sel);
            s.Text(g.fontSmall, ui::kSub, 10, yInfo, ClipText(texLine, 58), nullptr);
            const auto& r = g.matUvReport;
            char db[160];
            std::snprintf(db, sizeof db, "missing %d  out %d  seams %d  islands %d  overlap %d  stretch %.1f-%.1f", r.missingUvVerts, r.outOfRangeVerts, r.boundaryEdges, r.islands, r.overlapPairs, r.stretchMin, r.stretchMax);
            s.Text(g.fontSmall, ui::kDim, 10, yInfo + 15, ClipText(db, 58), nullptr);
        }
        // Actions.
        {
            const RECT en = MatTexEnableRect(l);
            CheckBox(s, en.left, en.top + 3, sel.enabled, ui::kAccent);
            s.Text(g.fontUi, ui::kText, en.left + 24, en.top + 2, "Enabled", nullptr);
            const RECT rp = MatTexReplaceRect(l);
            s.FillRound(rp, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, rp, "Replace");
            const RECT ft = MatUvFitRect(l);
            s.FillRound(ft, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, ui::kText, ft, "Fit UV");
        }
        // Slot dropdown popup (modal, over the canvases).
        if (g.matTexSlotDropOpen) {
            const RECT drop = MatTexDropRect(l, std::min(6, static_cast<int>(slots.size())));
            s.FillRound(drop, 5, ui::kCard);
            s.StrokeRound(drop, 5, ui::kLineHi);
            for (int i = 0; i < std::min(6, static_cast<int>(slots.size())); ++i) {
                const RECT row{drop.left + 3, drop.top + i * kRowH, drop.right - 3, drop.top + (i + 1) * kRowH};
                if (i == g.matTexSlot || i == g.matDropHover) s.FillRound(row, 4, (i == g.matDropHover) ? ui::kLineHi : RGB(36, 50, 78));
                s.Text(g.fontUi, (i == g.matTexSlot) ? ui::kAmber : ui::kText, row.left + 4, row.top + 2, slots[static_cast<size_t>(i)].prop, &row);
            }
        }
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

