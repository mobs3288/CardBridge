// viewer fragment: buttons, check, sliders, scrollbar (included in order by main.cpp)

// The button hierarchy: Normal (secondary, the default), Primary (the one action the screen wants; flat accent),
// Ghost (low-chrome inline actions), Danger (destructive). Flat fills, no gradients: an accent fill says "this does the work".
enum class Btn : int { Normal, Primary, Ghost, Danger, Wip };

bool Touches(const RECT& clip, const RECT& r) { RECT t; return IntersectRect(&t, &clip, &r) != 0; }
bool TouchesY(const RECT& clip, int top, int bottom) { return bottom > clip.top && top < clip.bottom; }

// A scrolled list: which row is first, how far it is pushed up (pixels), and how many rows show.
struct ListView {
    int first = 0, offset = 0, count = 0;
};
int ScrollPx(float pos, int rowH = kRowH) { return static_cast<int>(std::lround(std::max(0.0f, pos) * static_cast<float>(rowH))); }
ListView ViewOf(int top, int bottom, float pos, int rowH = kRowH) {
    const int px = ScrollPx(pos, rowH);
    ListView v;
    v.first = px / rowH;
    v.offset = px % rowH;
    v.count = (bottom - top + v.offset + rowH - 1) / rowH;
    return v;
}

void Button(ui::Surface& s, const RECT& r, const std::string& label, bool hot, bool on, bool enabled, Btn kind = Btn::Normal) {
    if (kind == Btn::Primary) {
        if (enabled) {
            s.FillRound(r, ui::kRad, hot ? ui::kAccentHi : ui::kAccent);
            s.TextCentered(g.fontBold, ui::kOnAccent, r, label);
        } else {
            s.FillRound(r, ui::kRad, ui::kCardHi);
            s.TextCentered(g.fontSemi, ui::kFaint, r, label);
        }
        return;
    }
    if (kind == Btn::Ghost) {   // no chrome until hovered
        if (hot && enabled) s.FillRound(r, ui::kRad, ui::kCardHi);
        s.TextCentered(g.fontSemi, !enabled ? ui::kFaint : hot ? ui::kText : ui::kSub, r, label);
        return;
    }
    if (kind == Btn::Danger) {
        if (hot) s.FillRound(r, ui::kRad, Mix(ui::kBad, ui::kBg, 0.82f));
        s.StrokeRound(r, ui::kRad, ui::kBad);
        s.TextCentered(g.fontSemi, ui::kBad, r, label);
        return;
    }
    if (kind == Btn::Wip) {   // experimental export target: amber, not the success/error colours
        s.FillRound(r, ui::kRad, !enabled ? ui::kCard : hot ? ui::kSelect : ui::kCardHi);
        s.StrokeRound(r, ui::kRad, ui::kAmber);
        s.TextCentered(g.fontSemi, !enabled ? ui::kFaint : ui::kAmber, r, label);
        return;
    }
    s.FillRound(r, ui::kRad, !enabled ? ui::kCard : hot ? ui::kSelect : on ? ui::kSelect : ui::kCardHi);
    s.StrokeRound(r, ui::kRad, on ? ui::kAccent : ui::kLine);
    s.TextCentered(g.fontSemi, !enabled ? ui::kFaint : on ? ui::kAccentHi : ui::kText, r, label);
}

// A modal dropdown menu: one card, one row per item; the checked item is accent-tinted, the hovered row is slate.
void DrawMenu(ui::Surface& s, const RECT& r, const std::vector<std::string>& items, int selected, int hover) {
    s.FillRound(r, ui::kRad, ui::kCard);
    s.StrokeRound(r, ui::kRad, ui::kLineHi);
    for (size_t i = 0; i < items.size(); ++i) {
        const RECT row{r.left + 3, r.top + static_cast<int>(i) * kRowH, r.right - 3, r.top + static_cast<int>(i + 1) * kRowH};
        if (static_cast<int>(i) == hover) s.FillRound(row, ui::kRadSm, ui::kSelect);
        s.Text(g.fontUi, static_cast<int>(i) == selected ? ui::kAccentHi : ui::kText, row.left + 8, row.top + 2, items[i], &row);
    }
}

// A check box: a rounded square, filled with a tick when on.
void CheckBox(ui::Surface& s, int x, int y, bool on, COLORREF onColor, bool dimmed = false) {
    const RECT r{x, y, x + 14, y + 14};
    if (on) {
        s.FillRound(r, ui::kRadSm, dimmed ? ui::kFaint : onColor);
        s.Line(static_cast<float>(x) + 3.5f, static_cast<float>(y) + 7.5f, static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, ui::kOnAccent, 1.8f);
        s.Line(static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, static_cast<float>(x) + 10.8f, static_cast<float>(y) + 4.2f, ui::kOnAccent, 1.8f);
    } else {
        s.StrokeRound(r, ui::kRadSm, ui::kLineHi);
    }
}

// The open / closed arrow of a group or a tree node.
void Expander(ui::Surface& s, int x, int cy, bool open, COLORREF c) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(cy);
    if (open) s.FillTriangle(fx, fy - 2.0f, fx + 8.0f, fy - 2.0f, fx + 4.0f, fy + 3.0f, c);
    else s.FillTriangle(fx + 1.0f, fy - 4.0f, fx + 1.0f, fy + 4.0f, fx + 6.0f, fy, c);
}

// One preset-chips row: a caption ("Band:" / "Cup:" / "Size:") on the left, small buttons on the right that set the
// value with a single click. The chip matching the underlying slider's current value is lit.
void DrawPresetChips(ui::Surface& s, int rowId, const std::string& label, int y) {
    const int count = PresetChipCount(rowId);
    if (count <= 0) return;
    s.Text(g.fontSmall, ui::kSub, 12, y + 5, label);
    const int active = ActivePresetChip(rowId);
    for (int i = 0; i < count; ++i) {
        const RECT r = PresetChipRect(rowId, i, y);
        const bool on = i == active;
        s.FillRound(r, ui::kRad, on ? ui::kSelect : ui::kCardHi);
        s.StrokeRound(r, ui::kRad, on ? ui::kAccent : ui::kLine);
        s.TextCentered(g.fontSmall, on ? ui::kAccentHi : ui::kText, r, PresetChipText(rowId, i));
    }
}

// One slider row: label, the track filled from the card's value to the current one, the thumb, the value. False when the id names no slider.
// trackL / trackR / valueR / labelX default to the side panel's own; the Material Workbench passes a wider track.
bool DrawSliderRow(ui::Surface& s, int id, const std::string& label, int y, int trackL = kTrackL, int trackR = kTrackR,
                   int valueR = kPanel - 14, int labelX = 12) {
    SliderRef r;
    if (!SliderFor(id, r)) return false;
    const bool changed = TweakChanged(id);
    const COLORREF mark = changed ? ui::kAmber : ui::kAccent;
    const RECT labelClip{labelX, y, trackL - 8, y + kRowH};
    // The label box is only ~148px wide: truncate with … so a long name never
    // hard-cuts mid-word at the track edge.
    s.Text(g.fontUi, changed ? ui::kAmber : ui::kText, labelX, y + 2, ClipText(label, 22), &labelClip);
    if (g.sliderEdit == id) {   // Shift+Left-click opened the keyboard box: the track and the value become one input field
        const RECT box{trackL - 2, y + 1, valueR + 2, y + kRowH - 1};
        s.FillRound(box, ui::kRad, ui::kBg);
        s.StrokeRound(box, ui::kRad, ui::kAccent);
        s.Text(g.fontMono, ui::kAmber, trackL + 6, y + 3, ClipText(g.sliderEditBuf + "_", 46), nullptr);
        return true;
    }
    const int mid = y + kRowH / 2;
    auto xOf = [&](float v) { return trackL + static_cast<int>(std::lround(std::clamp((v - r.lo) / (r.hi - r.lo), 0.0f, 1.0f) * (trackR - trackL))); };
    s.FillRound(RECT{trackL, mid - 2, trackR, mid + 2}, 2, ui::kLine);
    const int a = std::min(xOf(r.def), xOf(r.Value())), b = std::max(xOf(r.def), xOf(r.Value()));
    if (b > a) s.FillRound(RECT{a, mid - 2, b, mid + 2}, 2, mark);
    s.Line(static_cast<float>(xOf(r.def)), static_cast<float>(mid - 5), static_cast<float>(xOf(r.def)), static_cast<float>(mid + 5), ui::kDim, 1.0f);   // where the card has it
    s.FillDisc(static_cast<float>(xOf(r.Value())), static_cast<float>(mid), 6.5f, mark);
    s.FillDisc(static_cast<float>(xOf(r.Value())), static_cast<float>(mid), 3.5f, ui::kBg);
    char val[24];
    if (r.fmt) std::snprintf(val, sizeof val, r.fmt, static_cast<double>(r.Value()));
    else if (r.percent) std::snprintf(val, sizeof val, "%d", static_cast<int>(std::lround(r.Value() * 100)));
    else std::snprintf(val, sizeof val, "%+d", static_cast<int>(std::lround(r.Value())));
    s.TextRight(g.fontMono, changed ? ui::kAmber : ui::kSub, valueR, y + 3, val);
    return true;
}

// A thin scroll bar at the right edge of a list (rightEdge = where its thumb ends), with a faint track.
void ScrollBar(ui::Surface& s, int top, int bottom, int rows, int visible, float pos, int rightEdge = kPanel - 7) {
    if (rows <= visible) return;
    const int trackH = bottom - top;
    const int thumbH = std::max(18, trackH * visible / rows);
    const float span = static_cast<float>(std::max(1, rows - visible));
    const int thumbY = top + static_cast<int>(static_cast<float>(trackH - thumbH) * std::clamp(pos / span, 0.0f, 1.0f));
    s.FillRound(RECT{rightEdge - 4, top, rightEdge, bottom}, 2, ui::kCard);
    s.FillRound(RECT{rightEdge - 4, thumbY, rightEdge, thumbY + thumbH}, 2, ui::kLineHi);
}

