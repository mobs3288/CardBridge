// viewer fragment: loading bar, invalidation, log (included in order by main.cpp)

// ---- loading bar state (workers report; the UI thread paints) --------------
struct ProgSnapshot {
    bool active = false;
    bool cancelRequested = false;
    double frac = -1.0;   // <0 = indeterminate
    std::string title, stage;
};

void Log(const std::string& text, LogKind kind);   // defined below

void ProgBegin(const std::string& title, bool indeterminate = true) {
    {
        std::lock_guard<std::mutex> lk(g.progMu);
        g.progTitle = title;
        g.progStage = indeterminate ? title : std::string();
        g.progFrac = indeterminate ? -1.0 : 0.0;
        ++g.progGen;
    }
    g.progCancel.store(false);
    g.progActive.store(true);
    g.progHoverCancel = false;
    if (g.hwnd) PostMessageW(g.hwnd, WM_PROGRESS, 0, 0);
}

void ProgSet(double frac, const std::string& stage) {
    // Cheap: just store the snapshot. Frame() repaints at ~30/s while a job
    // runs, so posting a message here would flood paints (one per mod file /
    // material) and make the bar flicker. Fractions never run backwards:
    // denominators that grow mid-job (assemble sub-parts) would otherwise
    // jitter the bar, and a stray indeterminate tick mid-job would flip the
    // bar into a sliding chunk and back.
    if (!g.progActive.load()) return;
    {
        std::lock_guard<std::mutex> lk(g.progMu);
        if (frac >= 0.0) {
            if (!(g.progFrac >= 0.0 && frac < g.progFrac)) g.progFrac = frac;
        }
        // frac < 0 while already determinate: keep the fraction, only the
        // stage label moves. Only a job that started indeterminate stays one.
        else if (g.progFrac < 0.0) g.progFrac = frac;
        if (!stage.empty()) g.progStage = stage;
    }
}

void ProgStage(const std::string& stage) { ProgSet(-1.0, stage); }

void ProgEnd() {
    g.progActive.store(false);
    g.progCancel.store(false);
    if (g.hwnd) PostMessageW(g.hwnd, WM_PROGRESS, 0, 0);
}

void ProgRequestCancel() {
    if (!g.progActive.load()) return;
    g.progCancel.store(true);
    Log("cancelling...", LogKind::Warn);
    if (g.hwnd) PostMessageW(g.hwnd, WM_PROGRESS, 0, 0);
}

bool ProgCancelled() { return g.progCancel.load(); }

ProgSnapshot GetProgSnapshot() {
    ProgSnapshot s;
    s.active = g.progActive.load();
    s.cancelRequested = g.progCancel.load();
    std::lock_guard<std::mutex> lk(g.progMu);
    s.frac = g.progFrac;
    s.title = g.progTitle;
    s.stage = g.progStage;
    return s;
}

// Paint the bar once, synchronously. UI-thread uploads (GPU Load) would
// otherwise block before the first paint of the bar is ever made.
void ProgFlush() {
    if (!g.hwnd) return;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    UpdateWindow(g.hwnd);
}

// The viewport overlay card, centred in the 3D view (middle on both axes).
// Client coordinates (includes the kPanel offset) so hit-tests compare
// directly against WndProc points. DrawProgressOverlay subtracts kPanel
// when drawing into the viewport-sized comp DC.
RECT ProgOverlayRect(int clientW, int clientH) {
    const int vw = clientW - kPanel;
    if (vw < 200 || clientH < 200) return RECT{kPanel + 8, 8, clientW - 8, 8};
    const int w = std::min(460, vw - 48);
    const int h = 132;
    const int x = kPanel + (vw - w) / 2;
    const int y = std::max(14, (clientH - h) / 2);
    return RECT{x, y, x + w, y + h};
}

RECT ProgCancelRect(const RECT& card) {
    return RECT{card.right - 104, card.bottom - 34, card.right - 10, card.bottom - 10};
}

// Parses the bake's "baked texture N/M: ..." into a fraction, -1 when it is not a count line.
double ParseBakeFrac(const std::string& msg) {
    const std::string key = "baked texture ";
    const size_t at = msg.find(key);
    if (at == std::string::npos) return -1.0;
    const char* p = msg.c_str() + at + key.size();
    char* end = nullptr;
    const long n = std::strtol(p, &end, 10);
    if (end == p || end[0] != '/') return -1.0;
    const long m = std::strtol(end + 1, &end, 10);
    if (m <= 0) return -1.0;
    return static_cast<double>(n) / static_cast<double>(m);
}

// Invalidation is by area, so a hover or a slider drag repaints a few rows instead of the whole window.
void InvalidatePanel() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    rc.right = kPanel;
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void InvalidateView() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    rc.left = kPanel;
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void InvalidateAll() { if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

void InvalidatePanelBand(int top, int bottom) {
    if (!g.hwnd) return;
    RECT rc{0, top, kPanel, bottom};
    InvalidateRect(g.hwnd, &rc, FALSE);
}

int ClientHeight() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    return rc.bottom;
}
void InvalidateHeader() { InvalidatePanelBand(0, kHeaderBottom); }
void InvalidateLog() { InvalidatePanelBand(Lay(ClientHeight()).logTop, ClientHeight()); }
void InvalidateFooter() { InvalidatePanelBand(Lay(ClientHeight()).footerTop, ClientHeight()); }
void InvalidateList() {
    const PanelLayout l = Lay(ClientHeight());
    InvalidatePanelBand(l.partsHeadingTop - 4, l.partsBottom + 4);
}

void Log(const std::string& text, LogKind kind = LogKind::Info) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[16];
    std::snprintf(ts, sizeof ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    // Wrap long messages here, so the paint loop stays a plain line blitter. The log text column is
    // kPanel - 76 - 8 px wide in 12 px Consolas, which fits about 40 characters.
    std::vector<std::string> parts;
    std::string rest = text;
    while (rest.size() > 40) {
        size_t cut = rest.rfind(' ', 40);
        if (cut == std::string::npos || cut < 20) cut = 40;
        parts.push_back(rest.substr(0, cut));
        rest = rest.substr(cut + (rest[cut] == ' ' ? 1 : 0));
    }
    parts.push_back(rest);
    {
        std::lock_guard<std::mutex> lk(g.logMu);
        for (size_t i = 0; i < parts.size(); ++i) g.logLines.push_back({i == 0 ? ts : std::string(8, ' '), parts[i], kind});
        if (g.logLines.size() > 800) g.logLines.erase(g.logLines.begin(), g.logLines.begin() + 200);
        g.logScroll = std::min(g.logScroll, static_cast<int>(g.logLines.size()));
    }
    if (g.hwnd) PostMessageW(g.hwnd, WM_LOG, 0, 0);
}

void ClearLog() {
    {
        std::lock_guard<std::mutex> lk(g.logMu);
        g.logLines.clear();
        g.logScroll = 0;
    }
    InvalidateLog();
}

size_t Triangles(const assemble::Scene& s) {
    size_t n = 0;
    for (const auto& m : s.meshes) for (const auto& sub : m.submeshes) n += sub.indices.size() / 3;
    return n;
}

std::string Count(size_t n) {
    char b[32];
    if (n >= 1000000) std::snprintf(b, sizeof b, "%.1fm", n / 1000000.0);
    else if (n >= 10000) std::snprintf(b, sizeof b, "%.1fk", n / 1000.0);
    else std::snprintf(b, sizeof b, "%zu", n);
    return b;
}

