// viewer fragment: viewport render (included in order by main.cpp)

// ---- drawing ---------------------------------------------------------------------------------------------------------
void Render(int w, int h) {
    std::shared_ptr<Loaded> L = Current();
    if (w < 8 || h < 8) return;
    g.fw = w; g.fh = h;
    if (!L || L->scene.parts.empty()) { g.frame.assign(static_cast<size_t>(w) * h, 0xFFE2E4E8u); return; }
    g.hidden.resize(L->scene.parts.size(), 0);
    g.effHidden = exporter::EffectiveHiddenParts(L->scene, g.hidden);
    g.vopt.textured = g.textured;
    int highlight = g.hoverPart;
    if (g.tab == TabMaterials && g.selMaterial >= 0) {
        highlight = -1;
        for (size_t pi = 0; pi < L->scene.parts.size() && highlight < 0; ++pi)
            for (int mi : L->scene.parts[pi].meshes) {
                if (mi < 0 || static_cast<size_t>(mi) >= L->scene.meshes.size()) continue;
                for (const auto& sub : L->scene.meshes[static_cast<size_t>(mi)].submeshes) if (sub.material == g.selMaterial) { highlight = static_cast<int>(pi); break; }
            }
    }
    g.vopt.highlightPart = highlight;
    if (g.showExp && g.expOk && g.expFor == L && g.gpuExp.Render(g.cam, w, h, &g.effHidden, g.frame, highlight)) { g.lastGpu = &g.gpuExp; DrawSkeleton(w, h); return; }   // as exported
    if (g.shaded && g.gpuOk && g.gpuScene == L) {   // Direct3D 11: the game's shaders at full rate
        if (g.gpu.Render(g.cam, w, h, &g.effHidden, g.frame, highlight)) { g.lastGpu = &g.gpu; DrawSkeleton(w, h); return; }
        g.gpuOk = false;
    }
    g.lastGpu = nullptr;
    g.boneScreen.clear();
    // Software shaders are slow: only once the view has been still for a moment.
    const bool still = GetTickCount64() - g.lastInput > 300;
    g.vopt.shaded = g.shaded && still;
    if (g.shaded && !still) SetTimer(g.hwnd, 1, 320, nullptr);   // come back and draw the shaded picture
    g.vopt.hiddenParts = &g.effHidden;
    g.vopt.step = g.interacting ? 3 : 1;
    g.vopt.skin = g.skin.empty() ? nullptr : &g.skin;   // the software view shows the pose (and the physics) too
    exporter::RenderView(L->scene, g.cam, w, h, g.vopt, *g.state, g.frame);
}

COLORREF KindColor(LogKind k) {
    switch (k) {
    case LogKind::Good: return ui::kGood;
    case LogKind::Warn: return ui::kAmber;
    case LogKind::Error: return ui::kBad;
    default: return ui::kSub;
    }
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [&](int (*get)(COLORREF)) { return static_cast<int>(std::lround(get(a) + (get(b) - get(a)) * t)); };
    return RGB(ch([](COLORREF c) -> int { return GetRValue(c); }), ch([](COLORREF c) -> int { return GetGValue(c); }), ch([](COLORREF c) -> int { return GetBValue(c); }));
}

