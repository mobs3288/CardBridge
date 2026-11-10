// viewer fragment: Materials tab draw (included in order by main.cpp)

// The material list as the side panel's launcher and the Material Workbench both show
// it: one row per material (index, name, class, xN where a material is shared), the
// selected one marked with a bar, an amber tint while it has edits, and a scroll bar
// at the right edge. The card around the rows belongs to the caller (the panel draws
// its own list card, the workbench draws its own).
void DrawMatList(ui::Surface& s, const RECT& clip, int x0, int x1, int top, int bottom, float pos, int hoverRow,
                 const std::vector<int>& rows, const assemble::Scene& scene) {
    const RECT listClip{x0, top, x1, bottom};
    RECT sub;
    if (!IntersectRect(&sub, &clip, &listClip)) return;
    const int visible = std::max(1, (bottom - top) / kRowH);
    const ListView v = ViewOf(top, bottom, pos);
    const size_t chars = static_cast<size_t>(std::max(16, (x1 - x0) * 46 / 368));   // the panel's text budget, by width
    s.Begin(sub);
    for (int i = 0; i < v.count; ++i) {
        const int idx = v.first + i;
        if (idx >= static_cast<int>(rows.size())) break;
        const int mi = rows[static_cast<size_t>(idx)];
        const int y = top - v.offset + i * kRowH;
        const bool sel = mi == g.selMaterial;
        const bool edited = mi >= 0 && static_cast<size_t>(mi) < g.mat.overrides.size() && g.mat.overrides[static_cast<size_t>(mi)] && !g.mat.overrides[static_cast<size_t>(mi)]->empty();
        if (sel || idx == hoverRow) {
            s.FillRound(RECT{x0 + 2, y + 1, x1 - 2, y + kRowH - 1}, 5, sel ? RGB(36, 50, 78) : ui::kCardHi);
            if (sel) s.FillRound(RECT{x0 + 2, y + 4, x0 + 5, y + kRowH - 4}, 1, ui::kAccent);
        }
        material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(mi));
        std::string txt = std::to_string(mi) + " " + eff.displayName + " [" + material::ClassName(eff.cls) + "]";
        const int shared = material::CountSubmeshesForMaterial(scene, mi);
        if (shared > 1) txt += " x" + std::to_string(shared);
        const RECT textClip{x0 + 4, y, x1 - 12, y + kRowH};
        s.Text(g.fontUi, edited ? ui::kAmber : sel ? RGB(240, 246, 255) : ui::kText, x0 + 8, y + 2, ClipText(txt, chars), &textClip);
    }
    ScrollBar(s, top, bottom, static_cast<int>(rows.size()), visible, pos, x1 - 1);
    s.Begin(clip);
}

// The Materials tab of the side panel: a launcher, not the editor. It holds the
// class filter, a button that opens the Material Workbench window (Workbench.inc,
// where every material edit lives) and the material list with the selection marked.
void DrawMaterialsTab(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    if (g.mat.originals.size() != scene.materials.size()) material::CaptureOriginals(g.mat, scene);
    if (g.selMaterial < 0 && !scene.materials.empty()) { g.selMaterial = 0; g.matUvTrisCache = -2; }
    // Filter row.
    {
        std::string fl = "Filter: All v";
        if (g.matFilter >= 0) fl = std::string("Filter: ") + material::ClassName(static_cast<material::MaterialClass>(g.matFilter)) + " v";
        s.FillRound(l.matFilter, 5, g.matFilterDropOpen ? ui::kLineHi : ui::kCardHi);
        s.TextCentered(g.fontUi, g.matFilter >= 0 ? ui::kAmber : ui::kText, l.matFilter, fl);
        const int total = static_cast<int>(scene.materials.size());
        const std::string n = std::to_string(BuildMatRows(L).size()) + " / " + std::to_string(total);
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.partsTop + 4, n);
    }
    // The launcher button (hover id 20, see ButtonAt): it opens the workbench window.
    Button(s, l.matOpen, "Open Material Workbench", g.hoverButton == 20, false, true, true);
    // The list; the editor of the selected material is in the workbench window.
    const std::vector<int> rows = BuildMatRows(L);
    const int visible = std::max(1, (l.matListBottom - l.matListTop) / kRowH);
    g.matScroll = std::max(0, std::min(g.matScroll, std::max(0, static_cast<int>(rows.size()) - visible)));
    g.matPos = std::clamp(g.matPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    DrawMatList(s, clip, 6, kPanel - 6, l.matListTop, l.matListBottom, g.matPos, g.hoverRow, rows, scene);
    // The filter dropdown is drawn last: it opens over the list.
    if (g.matFilterDropOpen) DrawMatFilterDrop(s, l.matFilter.left, l.matFilter.top, l.matFilter.right - l.matFilter.left);
}
