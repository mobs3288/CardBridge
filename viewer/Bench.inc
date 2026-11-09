// viewer fragment: bench, pump (included in order by main.cpp)

void FlushFrame() { Frame(); }

// One round of the main loop: the queued messages, then a picture.
void PumpOnce() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Frame();
}

// Runs the main loop until a paint has finished (or a few seconds passed).
void PumpUntilPaint() {
    const long p0 = g.prof.paints;
    const ULONGLONG t0 = GetTickCount64();
    while (g.prof.paints == p0 && GetTickCount64() - t0 < 5000) {
        PumpOnce();
        if (g.prof.paints == p0) MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
}

void RunBench() {
    const char* outPath = std::getenv("CARDBRIDGE_BENCH");
    std::shared_ptr<Loaded> L = Current();
    if (!outPath || !L || L->scene.parts.empty()) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    struct Phase { std::vector<double> ms; double render = 0, blit = 0, panel = 0, panelDraw = 0, panelCopy = 0, reshape = 0, refresh = 0; std::vector<double> sec = std::vector<double>(8, 0.0); long paints = 0, renders = 0; };
    std::map<std::string, Phase> phases;
    auto run = [&](const char* name, int n, const std::function<void(int)>& step) {
        Phase& ph = phases[name];
        const App::Prof p0 = g.prof;
        for (int i = 0; i < n; ++i) {
            const double t = NowMs();
            step(i);
            ph.ms.push_back(NowMs() - t);
        }
        ph.render = g.prof.render - p0.render; ph.blit = g.prof.blit - p0.blit; ph.panel = g.prof.panel - p0.panel;
        ph.reshape = g.prof.reshape - p0.reshape; ph.refresh = g.prof.refresh - p0.refresh;
        for (int k = 0; k < 8; ++k) ph.sec[static_cast<size_t>(k)] = g.prof.sec[k] - p0.sec[k];
        ph.panelDraw = g.prof.panelDraw - p0.panelDraw; ph.panelCopy = g.prof.panelCopy - p0.panelCopy;
        ph.paints = g.prof.paints - p0.paints; ph.renders = g.prof.renders - p0.renders;
    };
    auto send = [&](UINT m, WPARAM w, int x, int y) { SendMessageW(g.hwnd, m, w, MAKELPARAM(x, y)); };
    FlushFrame();
    PanelLayout l = Lay(rc.bottom);
    const int vx = kPanel + (rc.right - kPanel) / 2, vy = rc.bottom / 2;

    run("orbit", 80, [&](int i) {
        if (i == 0) send(WM_LBUTTONDOWN, MK_LBUTTON, vx, vy);
        send(WM_MOUSEMOVE, MK_LBUTTON, vx + i * 4, vy + (i % 7));
        FlushFrame();
        if (i == 79) send(WM_LBUTTONUP, 0, vx + i * 4, vy);
    });
    FlushFrame();
    g.tab = TabParts; g.showIssues = false;
    run("hover_parts", 120, [&](int i) {
        send(WM_MOUSEMOVE, 0, 100, l.partsTop + 4 + (i * 7) % std::max(40, l.partsBottom - l.partsTop - 8));
        FlushFrame();
    });
    send(WM_MOUSEMOVE, 0, vx, vy);
    run("tab_switch", 50, [&](int i) {
        const RECT t = l.tabs[i % TabCount];
        send(WM_LBUTTONDOWN, MK_LBUTTON, (t.left + t.right) / 2, (t.top + t.bottom) / 2);
        send(WM_LBUTTONUP, 0, (t.left + t.right) / 2, (t.top + t.bottom) / 2);
        FlushFrame();
    });
    // a body slider: press on its track, then drag back and forth
    g.tab = TabBody; g.partScroll = 0; g.partPos = 0; g.showIssues = false;
    FlushFrame();
    {
        const std::vector<TweakRow> rows = BuildTweakRows(TabBody);
        int first = -1;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].kind == 1) { first = static_cast<int>(i); break; }
        if (first >= 0) {
            l = Lay(rc.bottom);
            const int y = l.partsTop + first * kRowH + kRowH / 2;
            const int a = kTrackL + (kTrackR - kTrackL) * 4 / 10, b = kTrackL + (kTrackR - kTrackL) * 9 / 10;
            send(WM_LBUTTONDOWN, MK_LBUTTON, a, y);
            run("slider_drag", 60, [&](int i) {
                send(WM_MOUSEMOVE, MK_LBUTTON, a + (b - a) * (i % 20) / 20, y);
                FlushFrame();
            });
            run("slider_flood", 8, [&](int) {   // 25 moves arrive at once, as from a fast mouse: time to the next finished paint
                for (int k = 0; k < 25; ++k) PostMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(a + (b - a) * k / 25, y));
                PumpUntilPaint();
            });
            send(WM_LBUTTONUP, 0, b, y);
            FlushFrame();
            run("slider_reset", 1, [&](int) { send(WM_RBUTTONDOWN, MK_RBUTTON, 60, y); send(WM_RBUTTONUP, 0, 60, y); FlushFrame(); });
        }
    }
    g.tab = TabParts; g.partScroll = 0; g.partPos = 0;
    FlushFrame();
    run("log_line", 60, [&](int i) {
        Log("benchmark line " + std::to_string(i) + " - a message about as long as the ones the loader writes");
        PumpUntilPaint();
    });
    {
        POINT pt{100, l.partsTop + 40};
        ClientToScreen(g.hwnd, &pt);
        run("wheel", 60, [&](int i) {
            SendMessageW(g.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (i / 15) % 2 ? WHEEL_DELTA : -WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
            FlushFrame();
        });
    }
    run("view_idle_repaint", 40, [&](int) { g.dirty = true; InvalidateRect(g.hwnd, nullptr, FALSE); FlushFrame(); });

    nlohmann::json j;
    j["client"] = {rc.right, rc.bottom};
    j["gpu"] = g.gpuOk;
    j["triangles"] = Triangles(L->scene);
    for (auto& [name, ph] : phases) {
        std::vector<double> v = ph.ms;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        auto pct = [&](double q) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
        j["phases"][name] = {{"n", v.size()}, {"mean_ms", v.empty() ? 0.0 : sum / static_cast<double>(v.size())}, {"p50_ms", pct(0.5)}, {"p95_ms", pct(0.95)}, {"max_ms", v.empty() ? 0.0 : v.back()},
                             {"paints", ph.paints}, {"renders", ph.renders}, {"render_ms_total", ph.render}, {"blit_ms_total", ph.blit}, {"panel_ms_total", ph.panel}, {"panel_draw_ms_total", ph.panelDraw}, {"secs", ph.sec}, {"panel_copy_ms_total", ph.panelCopy}, {"reshape_ms_total", ph.reshape}, {"refresh_ms_total", ph.refresh}};
    }
    const std::string text = j.dump(2);
    fileio::WriteFile(outPath, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

