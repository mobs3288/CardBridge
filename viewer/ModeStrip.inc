// viewer fragment: the viewport's diagnostic mode strip (included in order by main.cpp)
//
// A compact strip of buttons across the bottom of the 3D view: one per viewport
// diagnostic mode (spec S13, ADR-008) and, at the right end, an Isolate toggle that
// hides every part not using the selected material (MaterialID mode). Drawing by GDI
// directly onto the window DC (like the skeleton overlay), the same geometry for the
// click code and the drawing code. The mode persists in viewer.json ("viewMode").

struct ModeStripCell { RECT r; int mode; };   // mode: the ViewMode enum value, or -1 for the Isolate toggle

constexpr int kModeStripH = 22, kModeStripSide = 6, kModeStripGap = 8, kModeIsolateW = 64;

// The layout of the strip for a given viewport: one cell per diagnostic mode, plus the
// Isolate toggle at the right. Empty when the view is too narrow to show any cell.
std::vector<ModeStripCell> ModeStripCells(int x0, int W, int H) {
    std::vector<ModeStripCell> cells;
    if (!Current() || W < 8 || H < 60) return cells;
    const int n = static_cast<int>(exporter::ViewMode::MaterialID) + 1;
    const int cellW = (W - 2 * kModeStripSide - kModeIsolateW - kModeStripGap) / n;
    if (cellW < 30) return cells;
    const int y0 = H - kModeStripH - kModeStripSide;
    int x = x0 + kModeStripSide;
    for (int i = 0; i < n; ++i) {
        cells.push_back({RECT{x, y0, x + cellW, y0 + kModeStripH}, i});
        x += cellW;
    }
    cells.push_back({RECT{x + kModeStripGap, y0, x + kModeStripGap + kModeIsolateW, y0 + kModeStripH}, -1});
    return cells;
}

// The hit test for the click and the cursor: the index of the cell under a window point, or -1 (not on the strip).
int ModeStripCellIndex(int x, int y, int x0, int W, int H) {
    const auto cells = ModeStripCells(x0, W, H);
    for (size_t i = 0; i < cells.size(); ++i)
        if (x >= cells[i].r.left && x < cells[i].r.right && y >= cells[i].r.top && y < cells[i].r.bottom)
            return static_cast<int>(i);
    return -1;
}

// The mode value under a window point: the ViewMode for a mode cell, -1 for the isolate toggle, -2 outside the strip.
int ModeStripAt(int x, int y, int x0, int W, int H) {
    const auto cells = ModeStripCells(x0, W, H);
    const int i = ModeStripCellIndex(x, y, x0, W, H);
    return i < 0 ? -2 : cells[static_cast<size_t>(i)].mode;
}

bool ModeStripHover(int x, int y, int x0, int W, int H) { return ModeStripCellIndex(x, y, x0, W, H) >= 0; }

// A click on the strip: picks the mode or toggles isolate. True when the click was consumed.
bool ModeStripClick(int x, int y, int x0, int W, int H) {
    const auto cells = ModeStripCells(x0, W, H);
    const int i = ModeStripCellIndex(x, y, x0, W, H);
    if (i < 0 || i >= static_cast<int>(cells.size())) return false;
    const int hit = cells[static_cast<size_t>(i)].mode;
    if (hit >= 0) {
        g.viewMode = hit;
        g.settings.viewMode = exporter::ViewModeName(static_cast<exporter::ViewMode>(hit));
    } else {
        g.viewIsolate = !g.viewIsolate;
    }
    SaveSettings(g.settings);
    g.dirty = true;        // the view repaints with the new mode
    InvalidateView();
    return true;
}

void DrawModeStrip(HDC dc, int x0, int W, int H) {
    const auto cells = ModeStripCells(x0, W, H);
    if (cells.empty()) return;
    const int y0 = cells[0].r.top;
    const int stripLeft = cells[0].r.left - 4, stripRight = cells.back().r.right + 4;
    // the backdrop: a dark rounded bar (GDI; the viewport has no off-screen surface)
    if (HRGN rgn = CreateRoundRectRgn(stripLeft, y0 - 3, stripRight, cells[0].r.bottom + 3, 8, 8)) {
        HBRUSH bg = CreateSolidBrush(RGB(24, 26, 34));
        FillRgn(dc, rgn, bg);
        DeleteObject(bg);
        DeleteObject(rgn);
    }
    static constexpr const char* const labels[11] = {"Lit", "Unlit", "Base", "Wire", "Normal", "Rough", "Metal", "Opacity", "UV", "Trans", "M-ID"};
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g.fontSmall));
    for (size_t i = 0; i < cells.size(); ++i) {
        const ModeStripCell& c = cells[i];
        const bool isolate = c.mode < 0;
        const bool active = isolate ? g.viewIsolate : (c.mode == g.viewMode);
        const bool hover = g.viewHoverCell == static_cast<int>(i);
        HBRUSH cb = CreateSolidBrush(active ? RGB(58, 82, 122) : hover ? RGB(44, 46, 58) : RGB(32, 34, 44));
        if (HRGN rc = CreateRoundRectRgn(c.r.left, c.r.top, c.r.right, c.r.bottom, 4, 4)) {
            FillRgn(dc, rc, cb);
            if (active) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 170, 230));
                HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
                HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
                RoundRect(dc, c.r.left, c.r.top, c.r.right, c.r.bottom, 4, 4);
                SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
                DeleteObject(pen);
            }
            DeleteObject(rc);
        }
        DeleteObject(cb);
        const char* label = isolate ? "Isolate" : labels[c.mode];
        RECT tr = c.r;
        SetTextColor(dc, active ? RGB(238, 244, 255) : RGB(196, 199, 212));
        SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (oldFont) SelectObject(dc, oldFont);
}

// The MaterialID legend: one row per material on screen — deterministic colour, index, name, shared count. Drawn only
// in MaterialID mode, top-left of the viewport (the strip draws the mode buttons itself).
void DrawModeLegend(HDC dc, int x0, int W, int H) {
    if (g.viewMode != static_cast<int>(exporter::ViewMode::MaterialID)) return;
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const assemble::Scene& scene = L->scene;
    const int rowH = 16, titleH = 22, pad = 8, boxW = 268;
    const int maxRows = std::max(0, std::min(static_cast<int>(scene.materials.size()), (H - 64) / rowH));
    if (maxRows < 1) return;
    const int boxX = x0 + 8, boxY = 8, boxH = titleH + maxRows * rowH + pad;
    if (HRGN rgn = CreateRoundRectRgn(boxX, boxY, boxX + boxW, boxY + boxH, 6, 6)) {
        HBRUSH bg = CreateSolidBrush(RGB(18, 19, 26));
        FillRgn(dc, rgn, bg);
        DeleteObject(bg);
        DeleteObject(rgn);
    }
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g.fontSmall));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(150, 190, 240));
    RECT tr{boxX + 8, boxY + 4, boxX + boxW - 8, boxY + titleH};
    DrawTextA(dc, "MaterialID", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (int i = 0; i < maxRows; ++i) {
        const int y = boxY + titleH + i * rowH;
        const Vec3 c = material::MaterialIdColor(i);
        const COLORREF swatch = RGB(static_cast<int>(c.x * 255.0f), static_cast<int>(c.y * 255.0f), static_cast<int>(c.z * 255.0f));
        RECT sw{boxX + 8, y + 2, boxX + 20, y + 14};
        HBRUSH sb = CreateSolidBrush(swatch);
        FillRect(dc, &sw, sb);
        DeleteObject(sb);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(60, 62, 74));
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, GetStockObject(NULL_BRUSH)));
        Rectangle(dc, sw.left, sw.top, sw.right, sw.bottom);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(pen);
        material::EffectiveMaterial eff = material::EffectiveOf(g.mat, static_cast<size_t>(i));
        const int shared = material::CountSubmeshesForMaterial(scene, i);
        std::string txt = std::to_string(i) + " " + eff.displayName + " [" + material::ClassName(eff.cls) + "]";
        if (shared > 1) txt += " x" + std::to_string(shared);
        RECT t{boxX + 26, y, boxX + boxW - 6, y + rowH};
        SetTextColor(dc, i == g.selMaterial ? RGB(240, 246, 255) : RGB(170, 175, 190));
        DrawTextA(dc, txt.c_str(), -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (oldFont) SelectObject(dc, oldFont);
}
