// viewer fragment: card-independent KK/KKS asset library browser (included by main.cpp)

std::string AssetCachePath() {
    const char* la = std::getenv("LOCALAPPDATA");
    return la ? std::string(la) + "/CardBridge/assetcatalog.json" : std::string();
}

std::vector<const library::AssetRecord*> AssetRows() {
    std::shared_ptr<library::AssetCatalog> cat;
    library::AssetCategory filter;
    std::string search;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        cat = g.assets.catalog;
        filter = g.assets.filter;
        search = g.assets.search;
    }
    if (!cat) return {};
    return library::Query(*cat, filter, search);
}

void LoadAssetPreviewAsync(const library::AssetRecord rec) {
    const std::string lib = g.settings.library;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.assets.thumbKey = rec.key;
        g.assets.thumbRgba.clear();
        g.assets.thumbW = g.assets.thumbH = 0;
    }
    std::thread([rec, lib]() {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        const bool ok = library::LoadAssetThumbnail(rec, lib, rgba, w, h);
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            if (g.assets.thumbKey == rec.key && ok) {
                g.assets.thumbRgba = std::move(rgba);
                g.assets.thumbW = w; g.assets.thumbH = h;
            }
        }
        PostMessageW(g.hwnd, WM_ASSETS_FRAME, ok ? 0 : 1, 0);
    }).detach();
}

void EnsureAssetCatalog() {
    bool loading = false, loaded = false;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        loading = g.assets.loading; loaded = g.assets.loaded;
        if (loading || loaded) return;
        g.assets.loading = true;
        g.assets.error.clear();
    }
    const std::string lib = g.settings.library, mods = g.settings.mods;
    std::thread([lib, mods]() {
        bool failed = false;
        std::string err;
        try {
            if (lib.empty()) throw std::runtime_error("no asset library configured - set it in the Setup tab (L)");
            library::CatalogOptions opt;
            opt.libraryRoot = lib; opt.modsRoot = mods;
            opt.cachePath = AssetCachePath(); opt.modCachePath = ModCachePath();
            std::vector<std::string> issues;
            auto cat = std::make_shared<library::AssetCatalog>(library::BuildCatalog(opt, &issues));
            const size_t n = cat->size();
            {
                std::lock_guard<std::recursive_mutex> lk(g.mu);
                g.assets.catalog = cat;
                g.assets.loaded = true;
            }
            Log("asset library: " + std::to_string(n) + " item(s) indexed", LogKind::Good);
            for (const std::string& issue : issues) Log("  " + issue, LogKind::Warn);
        } catch (const std::exception& e) {
            failed = true; err = e.what();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.loading = false;
            g.assets.failed = failed;
            g.assets.error = err;
        }
        PostMessageW(g.hwnd, WM_ASSETS_READY, failed ? 1 : 0, 0);
    }).detach();
}

void StartAssetFit(const library::AssetRecord record) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || !L->NativePmx()) return;
    pmx::AssetTransform tr;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.assets.fitBusy) return;
        g.assets.fitBusy = true;
        g.assets.fitReady = false;
        tr = g.assets.transform;
    }
    library::AssetTemplateOptions opt;
    opt.libraryRoot = g.settings.library;
    opt.modsRoot = g.settings.mods;
    opt.modCachePath = ModCachePath();
    const std::shared_ptr<pmx::Model> doc = L->pmxDoc;
    std::thread([record, opt, tr, doc]() {
        library::AssetTemplate t;
        std::string error;
        pmx::AssetFitReport fit;
        if (library::LoadAssetTemplate(record, opt, t, error)) {
            t = pmx::ApplyTransform(t, tr);
            fit = pmx::AnalyzeAssetFit(t, *doc);
        } else {
            fit.reason = error;
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.fittedTemplate = t;
            g.assets.fit = fit;
            g.assets.fitReady = true;
            g.assets.fitBusy = false;
        }
        PostMessageW(g.hwnd, WM_ASSET_FIT_READY, fit.compatible ? 0 : 1, 0);
    }).detach();
}

void StartAssetApply() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || !L->NativePmx()) return;
    library::AssetTemplate t;
    pmx::AssetFitReport fit;
    std::shared_ptr<pmx::Model> doc;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.assets.applyBusy || !g.assets.fitReady || !g.assets.fit.compatible) return;
        g.assets.applyBusy = true;
        t = g.assets.fittedTemplate;
        fit = g.assets.fit;
        doc = std::make_shared<pmx::Model>(*L->pmxDoc);
    }
    std::thread([t, fit, doc]() {
        auto working = std::make_shared<pmx::Model>(*doc);
        const auto r = pmx::ApplyAsset(*working, t, fit);
        if (!r.ok) {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.applyError = r.error;
            g.assets.applyBusy = false;
            PostMessageW(g.hwnd, WM_ASSET_APPLY_READY, 1, 0);
            return;
        }
        std::vector<Bytes> overrides(working->textures.size());
        for (size_t i = 0; i < r.importedTextureBytes.size(); ++i)
            if (r.textureStart + i < overrides.size()) overrides[r.textureStart + i] = r.importedTextureBytes[i];
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.pendingDoc = working;
            g.assets.pendingOverrides = std::move(overrides);
            g.assets.applyError.clear();
            g.assets.applyBusy = false;
        }
        PostMessageW(g.hwnd, WM_ASSET_APPLY_READY, 0, 0);
    }).detach();
}

RECT AssetCatRect(const PanelLayout& l, int i) {
    const int w = 116, h = 20, gap = 6;
    const int x = 10 + (i % 3) * (w + gap), y = l.assetCats.top + (i / 3) * (h + 2);
    return {x, y, x + w, y + h};
}

int AssetCatAt(int x, int y, const PanelLayout& l) {
    for (int i = 0; i <= static_cast<int>(library::AssetCategory::Count); ++i) {
        RECT r = AssetCatRect(l, i);
        if (PtInRect(&r, POINT{x, y})) return i;
    }
    return -1;
}

void DrawAssetsTab(ui::Surface& s, const RECT& clip, const PanelLayout& l) {
    // search row
    s.FillRound(l.assetSearch, ui::kRad, ui::kBg);
    s.StrokeRound(l.assetSearch, ui::kRad, g.assets.editing ? ui::kAccent : ui::kLine);
    const std::string text = g.assets.search.empty() ? std::string("search name, id, mod ...") : g.assets.search;
    const COLORREF col = g.assets.search.empty() ? ui::kFaint : ui::kText;
    s.Text(g.fontUi, col, l.assetSearch.left + 8, l.assetSearch.top + 3,
           text + (g.assets.editing ? "_" : ""), &l.assetSearch);

    // category chips: All + the six categories
    const int nCat = static_cast<int>(library::AssetCategory::Count) + 1;
    for (int i = 0; i < nCat; ++i) {
        const RECT r = AssetCatRect(l, i);
        const bool active = i == 0 ? g.assets.filter == library::AssetCategory::Count
                                   : g.assets.filter == static_cast<library::AssetCategory>(i - 1);
        s.FillRound(r, ui::kRad, active ? ui::kSelect : ui::kCard);
        if (active) s.StrokeRound(r, ui::kRad, ui::kAccent);
        const char* label = i == 0 ? "All" : library::CategoryName(static_cast<library::AssetCategory>(i - 1));
        s.TextCentered(g.fontSmall, active ? ui::kAccentHi : ui::kSub, r, label);
    }

    const auto rows = AssetRows();
    if (g.assets.loading && rows.empty()) {
        s.Text(g.fontUi, ui::kAccent, 14, l.assetListTop + 4, "indexing the asset library ...");
        return;
    }
    if (g.assets.failed) {
        s.Text(g.fontSmall, ui::kBad, 14, l.assetListTop + 4, ClipText("library error: " + g.assets.error, 50));
        return;
    }
    if (!g.assets.loaded) {
        s.Text(g.fontSmall, ui::kDim, 14, l.assetListTop + 4, "press A in the Setup tab or click here to index assets");
        return;
    }

    const int visible = std::max(1, (l.assetListBottom - l.assetListTop) / kRowH);
    g.assets.listPos = std::clamp(g.assets.listPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    const RECT listClip{6, l.assetListTop, kPanel - 6, l.assetListBottom};
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip) != 0) {
        s.Begin(sub);
        const int offset = static_cast<int>(std::lround(g.assets.listPos * kRowH));
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.assetListTop + static_cast<int>(i) * kRowH - offset;
            if (!TouchesY(sub, y, y + kRowH)) continue;
            const library::AssetRecord& r = *rows[i];
            if (static_cast<int>(i) == g.assets.selected)
                s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, ui::kRad, ui::kSelect);
            s.Text(g.fontUi, ui::kText, 14, y + 2, ClipText(r.name, 34), &listClip);
            s.TextRight(g.fontSmall, ui::kFaint, kPanel - 14, y + 4,
                        r.mod ? "mod" : "base", &listClip);
        }
        ScrollBar(s, l.assetListTop, l.assetListBottom, static_cast<int>(rows.size()), visible, g.assets.listPos);
        s.Begin(clip);
    }

    // detail / preview card
    s.FillRound(l.assetDetail, ui::kRadLg, ui::kCard);
    s.StrokeRound(l.assetDetail, ui::kRadLg, ui::kLine);
    if (g.assets.selected < 0 || static_cast<size_t>(g.assets.selected) >= rows.size()) {
        s.Text(g.fontSmall, ui::kDim, 14, l.assetDetail.top + 10, "select an asset to inspect it");
        return;
    }
    const library::AssetRecord& r = *rows[static_cast<size_t>(g.assets.selected)];
    s.Text(g.fontBold, ui::kText, 14, l.assetDetail.top + 8, ClipText(r.name, 42), &l.assetDetail);
    const std::string sub1 = std::string(library::CategoryName(r.category)) + " / " + r.subcategory +
                             "   id " + std::to_string(r.id);
    s.Text(g.fontSmall, ui::kSub, 14, l.assetDetail.top + 28, ClipText(sub1, 48), &l.assetDetail);
    const std::string source = r.mod ? "mod: " + (r.modName.empty() ? r.modGuid : r.modName) : "base game";
    s.Text(g.fontSmall, ui::kDim, 14, l.assetDetail.top + 46, ClipText(source, 48), &l.assetDetail);
    const std::string files = "bundle " + (r.bundle.empty() ? "-" : r.bundle) + "  asset " + (r.asset.empty() ? "-" : r.asset);
    s.Text(g.fontMono, ui::kDim, 14, l.assetDetail.top + 62, ClipText(files, 46), &l.assetDetail);
    if (!r.parent.empty())
        s.Text(g.fontSmall, ui::kDim, 14, l.assetDetail.top + 78,
               ClipText("attachment: " + r.parent, 48), &l.assetDetail);
    if (!r.textureDeps.empty())
        s.Text(g.fontSmall, ui::kDim, 14, l.assetDetail.top + 94,
               ClipText("textures: " + std::to_string(r.textureDeps.size()), 48), &l.assetDetail);
    if (!g.assets.thumbRgba.empty() && g.assets.thumbW > 0 && g.assets.thumbH > 0) {
        const int size = std::min(static_cast<int>(l.assetDetail.bottom - l.assetDetail.top) - 16, 140);
        RECT thumb{l.assetDetail.right - size - 10, l.assetDetail.top + 8,
                   l.assetDetail.right - 10, l.assetDetail.top + 8 + size};
        const float scale = std::min(float(thumb.right - thumb.left) / g.assets.thumbW,
                                     float(thumb.bottom - thumb.top) / g.assets.thumbH);
        const int w = static_cast<int>(g.assets.thumbW * scale), h = static_cast<int>(g.assets.thumbH * scale);
        thumb = {thumb.right - w, thumb.top, thumb.right, thumb.top + h};
        s.BlitWrappedTexture(thumb, g.assets.thumbRgba.data(), g.assets.thumbW, g.assets.thumbH,
                             0.0f, 1.0f / (thumb.right - thumb.left), 0.0f, 1.0f / (thumb.bottom - thumb.top),
                             true, 0, true);
    }

    const char* fitText = g.assets.fitReady ? pmx::FitStatusName(g.assets.fit.status)
                                            : (g.assets.fitBusy ? "analyzing ..." : "not analyzed");
    s.Text(g.fontSmall, g.assets.fitReady && g.assets.fit.compatible ? ui::kGood : ui::kSub,
           14, l.assetDetail.top + 108,
           std::string("fit: ") + fitText + (g.assets.fitReady
               ? "  " + std::to_string(g.assets.fit.matchedBones) + "/" + std::to_string(g.assets.fit.requiredBones) + " bones" : ""),
           &l.assetDetail);
    static const char* labels[9] = {"X", "Y", "Z", "RX", "RY", "RZ", "SX", "SY", "SZ"};
    for (int i = 0; i < 9; ++i) {
        const RECT& box = l.assetFields[i];
        const bool active = g.assets.editField == i;
        s.FillRound(box, ui::kRad, ui::kBg);
        s.StrokeRound(box, ui::kRad, active ? ui::kAccent : ui::kLine);
        const float value = i < 3 ? (&g.assets.transform.translation.x)[i]
                          : i < 6 ? (&g.assets.transform.rotationDeg.x)[i - 3]
                                  : (&g.assets.transform.scale.x)[i - 6];
        const std::string shown = active ? g.assets.fieldBuf + "_" : std::to_string(value).substr(0, 6);
        s.Text(g.fontSmall, active ? ui::kText : ui::kSub, box.left + 5, box.top + 3,
               std::string(labels[i]) + " " + shown, &box);
    }
    const bool canApply = g.assets.fitReady && g.assets.fit.compatible && !g.assets.applyBusy;
    s.FillRound(l.assetApply, ui::kRad, canApply ? ui::kAccent : ui::kCardHi);
    s.TextCentered(g.fontSemi, canApply ? ui::kOnAccent : ui::kFaint, l.assetApply,
                   g.assets.applyBusy ? "applying ..." : "Apply asset to this PMX");
    if (!g.assets.applyError.empty())
        s.Text(g.fontSmall, ui::kBad, 14, l.assetApply.bottom + 2, ClipText(g.assets.applyError, 48), &l.assetDetail);
}

void CommitAssetField();   // defined below with AssetsChar

void AssetsTabClick(int x, int y, const PanelLayout& l) {
    if (g.assets.editField >= 0) { CommitAssetField(); return; }
    if (PtInRect(&l.assetSearch, POINT{x, y})) {
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.editing = true;
            g.assets.listPos = 0; g.assets.selected = -1;
        }
        InvalidatePanel();
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.assets.editing) { g.assets.editing = false; InvalidatePanel(); }
    }
    const int cat = AssetCatAt(x, y, l);
    if (cat >= 0) {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.assets.filter = cat == 0 ? library::AssetCategory::Count : static_cast<library::AssetCategory>(cat - 1);
        g.assets.selected = -1; g.assets.listPos = 0;
        g.assets.thumbKey.clear(); g.assets.thumbRgba.clear();
        InvalidatePanel();
        return;
    }
    // Transform fields and Apply button live in the detail card, below the list.
    for (int i = 0; i < 9; ++i) {
        if (PtInRect(&l.assetFields[i], POINT{x, y})) {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.assets.editField = i;
            g.assets.fieldBuf.clear();
            InvalidatePanel();
            return;
        }
    }
    if (PtInRect(&l.assetApply, POINT{x, y})) { StartAssetApply(); return; }
    if (y < l.assetListTop || y > l.assetListBottom) return;
    const auto rows = AssetRows();
    const int idx = static_cast<int>((y - l.assetListTop + std::lround(g.assets.listPos * kRowH)) / kRowH);
    if (idx < 0 || idx >= static_cast<int>(rows.size())) { EnsureAssetCatalog(); return; }
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.assets.selected = idx;
    }
    LoadAssetPreviewAsync(*rows[static_cast<size_t>(idx)]);
    StartAssetFit(*rows[static_cast<size_t>(idx)]);
    InvalidatePanel();
}

void CommitAssetField() {
    if (g.assets.editField < 0) return;
    const float value = static_cast<float>(std::atof(g.assets.fieldBuf.c_str()));
    const int i = g.assets.editField;
    if (i < 3) (&g.assets.transform.translation.x)[i] = value;
    else if (i < 6) (&g.assets.transform.rotationDeg.x)[i - 3] = value;
    else (&g.assets.transform.scale.x)[i - 6] = value;
    g.assets.editField = -1;
    g.assets.fieldBuf.clear();
    g.assets.fitReady = false;   // transform changed: re-analysis needed
}

void AssetsChar(wchar_t c) {
    if (g.tab != TabAssets) return;
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (g.assets.editField >= 0) {
        if (c == 13 || c == 27) CommitAssetField();
        else if (c == 8) { if (!g.assets.fieldBuf.empty()) g.assets.fieldBuf.pop_back(); }
        else if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+') g.assets.fieldBuf += static_cast<char>(c);
        InvalidatePanel();
        return;
    }
    if (!g.assets.editing) return;
    if (c == 13 || c == 27) { g.assets.editing = false; }
    else if (c == 8) { if (!g.assets.search.empty()) g.assets.search.pop_back(); }
    else if (c >= 32) {
        const std::wstring one(1, c);
        g.assets.search += WideToUtf8(one);
    }
    g.assets.listPos = 0; g.assets.selected = -1;
    InvalidatePanel();
}
