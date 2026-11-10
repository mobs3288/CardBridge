// viewer fragment: buttons, check, sliders, scrollbar (included in order by main.cpp)

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

void Button(ui::Surface& s, const RECT& r, const std::string& label, bool hot, bool on, bool enabled, bool primary = false) {
    COLORREF text;
    if (primary && enabled) {
        s.FillGradient(r, 7, hot ? Mix(ui::kAccent, RGB(255, 255, 255), 0.18f) : ui::kAccent, hot ? Mix(ui::kAccent2, RGB(255, 255, 255), 0.18f) : ui::kAccent2);
        text = RGB(12, 14, 24);
    } else {
        s.FillRound(r, 7, !enabled ? ui::kCard : hot ? ui::kLineHi : on ? RGB(34, 48, 76) : ui::kCardHi);
        s.StrokeRound(r, 7, on ? ui::kAccent : ui::kLine);
        text = !enabled ? ui::kFaint : on ? RGB(206, 230, 255) : ui::kText;
    }
    s.TextCentered(primary && enabled ? g.fontBold : g.fontUi, text, r, label);
}

// A check box: a rounded square, filled with a tick when on.
void CheckBox(ui::Surface& s, int x, int y, bool on, COLORREF onColor, bool dimmed = false) {
    const RECT r{x, y, x + 14, y + 14};
    if (on) {
        s.FillRound(r, 4, dimmed ? ui::kFaint : onColor);
        s.Line(static_cast<float>(x) + 3.5f, static_cast<float>(y) + 7.5f, static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, RGB(14, 16, 24), 1.8f);
        s.Line(static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, static_cast<float>(x) + 10.8f, static_cast<float>(y) + 4.2f, RGB(14, 16, 24), 1.8f);
    } else {
        s.StrokeRound(r, 4, ui::kLineHi);
    }
}

// The open / closed arrow of a group or a tree node.
void Expander(ui::Surface& s, int x, int cy, bool open, COLORREF c) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(cy);
    if (open) s.FillTriangle(fx, fy - 2.0f, fx + 8.0f, fy - 2.0f, fx + 4.0f, fy + 3.0f, c);
    else s.FillTriangle(fx + 1.0f, fy - 4.0f, fx + 1.0f, fy + 4.0f, fx + 6.0f, fy, c);
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
    const int mid = y + kRowH / 2;
    auto xOf = [&](float v) { return trackL + static_cast<int>(std::lround(std::clamp((v - r.lo) / (r.hi - r.lo), 0.0f, 1.0f) * (trackR - trackL))); };
    s.FillRound(RECT{trackL, mid - 2, trackR, mid + 2}, 2, ui::kLine);
    const int a = std::min(xOf(r.def), xOf(*r.v)), b = std::max(xOf(r.def), xOf(*r.v));
    if (b > a) s.FillRound(RECT{a, mid - 2, b, mid + 2}, 2, mark);
    s.Line(static_cast<float>(xOf(r.def)), static_cast<float>(mid - 5), static_cast<float>(xOf(r.def)), static_cast<float>(mid + 5), ui::kDim, 1.0f);   // where the card has it
    s.FillDisc(static_cast<float>(xOf(*r.v)), static_cast<float>(mid), 6.5f, mark);
    s.FillDisc(static_cast<float>(xOf(*r.v)), static_cast<float>(mid), 3.5f, ui::kBg);
    char val[24];
    if (r.fmt) std::snprintf(val, sizeof val, r.fmt, static_cast<double>(*r.v));
    else if (r.percent) std::snprintf(val, sizeof val, "%d", static_cast<int>(std::lround(*r.v * 100)));
    else std::snprintf(val, sizeof val, "%+d", static_cast<int>(std::lround(*r.v)));
    s.TextRight(g.fontMono, changed ? ui::kAmber : ui::kSub, valueR, y + 3, val);
    return true;
}

// A thin scroll bar at the right edge of a list (rightEdge = where its thumb ends).
void ScrollBar(ui::Surface& s, int top, int bottom, int rows, int visible, float pos, int rightEdge = kPanel - 7) {
    if (rows <= visible) return;
    const int trackH = bottom - top;
    const int thumbH = std::max(18, trackH * visible / rows);
    const float span = static_cast<float>(std::max(1, rows - visible));
    const int thumbY = top + static_cast<int>(static_cast<float>(trackH - thumbH) * std::clamp(pos / span, 0.0f, 1.0f));
    s.FillRound(RECT{rightEdge - 4, thumbY, rightEdge, thumbY + thumbH}, 2, ui::kLineHi);
}

