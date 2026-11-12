// viewer fragment: typing a slider's value with the keyboard (Shift+Left-click) (included in order by main.cpp)
//
// A slider's value can be entered by hand: Shift+Left-click its row and the row turns into a text box. Enter (or a
// click anywhere else) commits the typed number, Esc puts the old value back. While the box is open the keyboard goes
// to it and nothing else. The typed text is the stored value (percent rows: 0..100, like the %d the row shows); on
// commit it is clamped to the slider's range and rounded to its step, exactly how a drag ends.

void EndSliderEdit(bool commit);

std::string SliderEditTrim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string SliderEditInitText(const SliderRef& r) {
    char b[40];
    if (r.percent) {   // the row shows v*100 as an integer; the box holds (and parses) the same
        std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(*r.v * 100.0f)));
        return b;
    }
    std::snprintf(b, sizeof b, "%.4f", static_cast<double>(*r.v));
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
bool SliderEditParse(const std::string& buf, double& out) {
    const std::string t = SliderEditTrim(buf);
    if (t.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return false;   // the whole text must be one number
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}
void BeginSliderEdit(int id) {
    SliderRef r;
    if (!SliderFor(id, r) || !r.v) return;
    if (g.sliderEdit == id) return;                      // already typing here
    if (g.sliderEdit >= 0) EndSliderEdit(true);          // another row was open: commit it first
    g.sliderEdit = id;
    g.sliderEditOrig = *r.v;
    g.sliderEditBuf = SliderEditInitText(r);
    g.sliderEditMatPushed = false;
    g.sliderEditPoseBefore = CapturePose();
    g.matHexEditing = false; g.matHexUndoPushed = false; // one text entry at a time (the materials' HEX entry)
    if (IsMatId(id)) { material::PushUndo(g.mat); g.sliderEditMatPushed = true; }   // one undo step per session, like the start of a drag
    InvalidatePanel();
    InvalidateWorkbench();
}
void EndSliderEdit(bool commit) {
    const int id = g.sliderEdit;
    if (id < 0) return;
    SliderRef r;
    if (SliderFor(id, r) && r.v) {
        if (commit) {
            double v = 0;
            if (SliderEditParse(g.sliderEditBuf, v)) {
                if (r.percent) v /= 100.0;
                v = std::clamp(v, static_cast<double>(r.lo), static_cast<double>(r.hi));
                if (r.step > 0) v = std::round(v / static_cast<double>(r.step)) * static_cast<double>(r.step);
                *r.v = static_cast<float>(v);
            } else {
                *r.v = g.sliderEditOrig;   // not a number: the value stays what it was
                if (!SliderEditTrim(g.sliderEditBuf).empty()) Log("not a number; the value was kept", LogKind::Warn);
            }
        } else {
            *r.v = g.sliderEditOrig;       // Esc: put it back, exactly
        }
        TweakEdited(id, true);           // the same release path a drag ends with (re-shape / pose / material rebuild)
        PoseChanged(g.sliderEditPoseBefore);   // one undo step for a pose field (a no-op when nothing moved)
        if (!IsLiveId(id) && !IsMatId(id)) {
            char msg[96];
            std::snprintf(msg, sizeof msg, "slider %d set to %.3f (card %.3f)", id, static_cast<double>(*r.v), static_cast<double>(r.def));
            Log(msg);
        }
    }
    g.sliderEdit = -1;
    g.sliderEditBuf.clear();
    g.sliderEditMatPushed = false;
    InvalidatePanel();
    InvalidateMat();
}
bool SliderEditKey(UINT wp) {
    // While the box is open the keyboard belongs to it: every key is taken, handled or swallowed.
    if (wp == VK_ESCAPE) { EndSliderEdit(false); return true; }
    if (wp == VK_RETURN) { EndSliderEdit(true); return true; }
    if (wp == VK_BACK) { if (!g.sliderEditBuf.empty()) g.sliderEditBuf.pop_back(); InvalidatePanel(); InvalidateWorkbench(); return true; }
    if (wp == VK_DELETE) { g.sliderEditBuf.clear(); InvalidatePanel(); InvalidateWorkbench(); return true; }
    int ch = -1;
    if (wp >= '0' && wp <= '9') ch = static_cast<int>(wp);
    else if (wp >= VK_NUMPAD0 && wp <= VK_NUMPAD9) ch = '0' + static_cast<int>(wp - VK_NUMPAD0);
    else if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) ch = '-';
    else if (wp == VK_OEM_PLUS || wp == VK_ADD) ch = '+';
    else if (wp == VK_OEM_PERIOD || wp == VK_DECIMAL) ch = '.';
    if (ch >= 0) {
        if (g.sliderEditBuf.size() >= 12) return true;
        if (ch == '-' || ch == '+') {
            if (!g.sliderEditBuf.empty()) {
                if (g.sliderEditBuf.size() == 1 && (g.sliderEditBuf[0] == '-' || g.sliderEditBuf[0] == '+')) g.sliderEditBuf[0] = static_cast<char>(ch);   // the sign was all there is: swap it
                return true;   // a sign only leads
            }
            g.sliderEditBuf.push_back(static_cast<char>(ch));
        } else if (ch == '.') {
            if (g.sliderEditBuf.find('.') != std::string::npos) return true;
            if (g.sliderEditBuf.empty() || g.sliderEditBuf == "-" || g.sliderEditBuf == "+") g.sliderEditBuf.push_back('0');
            g.sliderEditBuf.push_back('.');
        } else {
            g.sliderEditBuf.push_back(static_cast<char>(ch));
        }
        InvalidatePanel();
        InvalidateWorkbench();
    }
    return true;   // a stray key: swallowed, the edit stays open
}
