// viewer fragment: panel, progress overlay, paint (included in order by main.cpp)

void DrawPanel(ui::Surface& s, const RECT& clip) {
    std::shared_ptr<Loaded> L = Current();
    std::string modsInfo;
    { std::lock_guard<std::recursive_mutex> lk(g.mu); modsInfo = g.modsInfo; }
    const int H = s.height();
    const PanelLayout l = Lay(H);
    double lapT = NowMs();
    auto lap = [&](int i) { const double t = NowMs(); g.prof.sec[i] += t - lapT; lapT = t; };
    s.Begin(clip);
    s.Fill(clip, ui::kBg);
    lap(0);
    if (clip.top < kHeaderBottom) g.prof.sec[7] += 1.0;

    // header and toolbar ---------------------------------------------------------------------------------------------
    if (TouchesY(clip, 0, kHeaderBottom)) DrawHeader(s, clip, l, L, modsInfo);
    if (TouchesY(clip, l.open.top, l.open.bottom)) {
        Button(s, l.open, "Open", g.hoverButton == 1, false, true);
        Button(s, l.exportPmx, g.exporting ? "Exporting..." : "Export PMX", g.hoverButton == 2, false, L != nullptr && !g.busy && !g.exporting, true);
        Button(s, l.issues, "Issues", g.hoverButton == 3, g.showIssues, L != nullptr);
        Button(s, l.reset, "Reset view", g.hoverButton == 4, false, L != nullptr);
        Button(s, l.all, "All", g.hoverButton == 5, false, L != nullptr);
        Button(s, l.asExported, "MMD", g.hoverButton == 6, g.showExp, L != nullptr);   // X: the exported PMX as MikuMikuDance shows it
    }
    // Loading bar: a thin strip in the gap under the toolbar (the only spare
    // space; nothing moves). The viewport overlay carries the labels.
    if (TouchesY(clip, l.open.bottom + 1, l.open.bottom + 11)) {
        const ProgSnapshot ps = GetProgSnapshot();
        if (ps.active) {
            const double phase = static_cast<double>(GetTickCount64() % 1200) / 1200.0;
            s.ProgressBar(RECT{10, l.open.bottom + 3, kPanel - 10, l.open.bottom + 9}, ps.frac, phase);
        }
    }

    lap(1);
    // outfit: the counter and the game's name for the coordinate, over a row of slots that can be clicked (Left / Right still step) ----
    if (TouchesY(clip, l.outfitNameTop - 4, l.outfitChipsTop + kOutfitChipH + 4)) {
        int shown = L ? L->outfit : -1;
        if (g.busy && g.loadingOutfit >= 0) shown = g.loadingOutfit;
        if (L && L->outfits > 0) shown = std::clamp(shown, 0, L->outfits - 1);
        s.Text(g.fontBold, ui::kAccent, 12, l.outfitNameTop, "OUTFIT");
        if (L && L->outfits > 0) {
            // Hovering a slot previews that outfit's name without loading it.
            const int preview = g.hoverOutfit >= 0 && g.hoverOutfit < L->outfits ? g.hoverOutfit : shown;
            const COLORREF nameColor = g.busy ? ui::kAccent : g.hoverOutfit >= 0 ? RGB(246, 249, 255) : ui::kText;
            const RECT nameClip{74, l.outfitNameTop, kPanel - 64, l.outfitNameTop + 20};
            s.Text(g.fontBold, nameColor, 74, l.outfitNameTop, ClipText(L->card.outfitName(preview), 30), &nameClip);
            char count[32];
            std::snprintf(count, sizeof count, "%d / %d", shown + 1, L->outfits);
            s.TextRight(g.fontBold, ui::kAmber, kPanel - 12, l.outfitNameTop, count);
        } else {
            s.Text(g.fontUi, ui::kDim, 74, l.outfitNameTop, g.busy ? "loading ..." : "no card");
        }
        const int chips = L ? std::min(L->outfits, OutfitChipsPerRow()) : 0;
        {
            bool hasC = false;
            int cOutfit = -1;
            if (L && L->card.hasClothing) { hasC = true; cOutfit = L->card.clothingOutfit; }
            else { std::lock_guard<std::recursive_mutex> lk(g.mu); hasC = !g.clothingPath.empty(); cOutfit = g.clothingOutfit; }
            for (int i = 0; i < chips; ++i) {
                const bool marked = hasC && i == cOutfit;   // the slot whose base outfit was replaced
                Button(s, OutfitChipRect(l, i), marked ? (std::to_string(i + 1) + "*") : std::to_string(i + 1),
                       g.hoverOutfit == i, i == shown, L != nullptr && !g.busy);
            }
        }
    }
    // outfit file: which slot holds the imported outfit (if any), the way back
    // to the card's own clothes for it, and the slot it can move between
    // without re-dropping the file. The buttons are drawn greyed when nothing
    // is imported, so the reset exists (and is findable) before the first drop.
    if (TouchesY(clip, l.outfitClothTop, l.outfitClothTop + 20)) {
        std::string clothingName;
        int clothingOutfit = -1;
        bool hasClothing = false;
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            hasClothing = !g.clothingPath.empty();
            clothingName = g.clothingName;
            clothingOutfit = g.clothingOutfit;
        }
        int shown = L ? L->outfit : -1;
        if (g.busy && g.loadingOutfit >= 0) shown = g.loadingOutfit;
        const bool onThisOutfit = hasClothing && clothingOutfit == shown;
        // The slot it overwrote is the base outfit it replaced ("*" on its chip).
        // Short label: the < > slot picker takes the rest of the row.
        const RECT clothClip{12, l.outfitClothTop, 144, l.outfitClothTop + 20};
        if (hasClothing) {
            const std::string where = "#" + std::to_string(clothingOutfit + 1) + " " + ClipText(clothingName, 14);
            s.Text(g.fontSmall, onThisOutfit ? ui::kAmber : ui::kDim, 12, l.outfitClothTop + 3,
                   ClipText("Added: " + where, 20), &clothClip);
        } else {
            s.Text(g.fontSmall, ui::kDim, 12, l.outfitClothTop + 3, ClipText("card's own clothing", 20), &clothClip);
        }
        const bool canPrev = hasClothing && L && clothingOutfit > 0;
        const bool canNext = hasClothing && L && clothingOutfit + 1 < L->outfits;
        Button(s, l.clothingPrev, "<", g.hoverButton == 8, false, canPrev && !g.busy);
        Button(s, l.clothingNext, ">", g.hoverButton == 9, false, canNext && !g.busy);
        Button(s, l.clothingExport, "Export clothes", g.hoverButton == 21, false, L != nullptr && !g.busy && !g.exporting);
        Button(s, l.clothingReset, "Reset clothing", g.hoverButton == 7, false, L != nullptr && !g.busy && hasClothing);
    }

    lap(2);
    // tabs ----------------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.tabs[0].top - 2, l.tabs[0].bottom + 2)) {
        for (int i = 0; i < TabCount; ++i) {
            const RECT& t = l.tabs[i];
            const bool act = !g.showIssues && g.tab == i, hot = g.hoverButton == 10 + i;
            if (act) {
                s.FillRound(t, 7, ui::kCardHi);
                s.FillGradient(RECT{t.left + 10, t.bottom - 3, t.right - 10, t.bottom - 1}, 1, ui::kAccent, ui::kAccent2);
            } else if (hot && L) {
                s.FillRound(t, 7, ui::kCard);
            }
            s.TextCentered(act ? g.fontBold : g.fontSmall, (!L && i != TabSettings) ? ui::kFaint : act ? ui::kText : hot ? ui::kText : ui::kSub, t, kTabNames[i]);
        }
    }

    lap(3);
    // the list: parts, issues, sliders or bones ---------------------------------------------------------------------------
    const RECT listCard{6, l.partsTop - 4, kPanel - 6, l.partsBottom + 3};
    if (Touches(clip, listCard)) {
        s.FillRound(listCard, 8, ui::kCard);
        const RECT listClip{6, l.partsTop, kPanel - 6, l.partsBottom};
        RECT sub;
        const bool inList = IntersectRect(&sub, &clip, &listClip) != 0;
        const RECT clipText{10, l.partsTop, kPanel - 14, l.partsBottom};
        if (!L && g.tab != TabSettings) {
            s.Text(g.fontUi, g.busy ? ui::kAccent : ui::kDim, 14, l.partsTop + 4, g.busy ? "loading the card ..." : "no card loaded");
        } else if (g.showIssues) {
            // Issues, issue by issue: a coloured severity marker, the item's id in brackets, the reason,
            // wrapped to the panel width. More readable than one clipped log line per message.
            std::vector<std::pair<std::string, LogKind>> lines;
            const size_t wrapCol = 46;   // fontLog is monospaced ~7 px/char, the panel is 380 px wide
            auto emit = [&](const std::string& head, const LogKind k, const std::string& msg) {
                size_t pos = 0;
                size_t room = wrapCol > head.size() ? wrapCol - head.size() : 0;   // chars left on the first line
                if (msg.size() <= room) { lines.push_back({head + msg, k}); return; }
                size_t end = std::min(head.size() + room, msg.size());
                size_t cut = msg.rfind(' ', end);
                if (cut == std::string::npos || cut == 0) cut = end;
                else ++cut;
                lines.push_back({head + msg.substr(0, cut), k});
                pos = cut;
                while (pos < msg.size()) {
                    while (pos < msg.size() && msg[pos] == ' ') ++pos;
                    if (msg.size() - pos <= wrapCol) { lines.push_back({std::string("    ") + msg.substr(pos), LogKind::Info}); return; }
                    size_t e = std::min(pos + wrapCol, msg.size());
                    size_t c = msg.rfind(' ', e);
                    if (c == std::string::npos || c <= pos) c = e;
                    else ++c;
                    lines.push_back({std::string("    ") + msg.substr(pos, c - pos), LogKind::Info});
                    pos = c;
                }
            };
            for (const auto& i : L->scene.issues) {
                const LogKind k = i.severity == "error" ? LogKind::Error : LogKind::Warn;
                const std::string sev = i.severity == "error" ? "ERROR" : "WARN ";
                emit(sev + "  " + (i.part.empty() ? "(whole card)" : "[" + i.part + "]") + "  ", k, i.message);
            }
            const int visible = std::max(1, (l.partsBottom - l.partsTop) / 16);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(lines.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(lines.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos, 16);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(lines.size())) break;
                    const int y = l.partsTop - v.offset + i * 16;
                    if (!TouchesY(sub, y, y + 16)) continue;
                    s.Text(g.fontLog, KindColor(lines[static_cast<size_t>(idx)].second), 14, y + 2, lines[static_cast<size_t>(idx)].first, &clipText);
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(lines.size()), visible, g.partPos);
                s.Begin(clip);
            }
        } else if (g.tab == TabBones) {
            DrawBonesTab(s, clip, l, *L);
        } else if (g.tab == TabMaterials) {
            DrawMaterialsTab(s, clip, l, *L);
        } else if (g.tab == TabOptimize) {
            DrawOptimizeTab(s, clip, l, *L);
        } else if (g.tab != TabParts) {
            const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
            const int visible = VisibleRows(l);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(rows.size())) break;
                    const TweakRow& row = rows[static_cast<size_t>(idx)];
                    const int y = l.partsTop - v.offset + i * kRowH;
                    if (!TouchesY(sub, y, y + kRowH)) continue;
                    if (idx == g.hoverRow && row.kind != 4 && !(row.kind == 0 && row.id < 0)) s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, ui::kCardHi);
                    if (row.kind == 0) {
                        std::string name = row.label;
                        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        const bool morph = g.tab == TabMorphs && row.group.find('/') != std::string::npos;   // a side sub-group under a section
                        if (g.tab == TabMorphs && row.id >= 0) {   // brows / eyes / mouth / other, or a side sub-group: the fold arrow folds it
                            const bool collapsed = g.collapsedMorphs.count(row.group) != 0;
                            const int tx = morph ? 26 : 14;
                            Expander(s, tx, y + kRowH / 2, !collapsed, ui::kAccent);
                            const RECT headClip{tx + 14, y, kPanel - 58, y + kRowH};
                            s.Text(g.fontBold, ui::kAccent, tx + 14, y + 3, ClipText(name, 42), &headClip);
                        } else {
                            // Header and its right-aligned "reset" share the row:
                            // clip the title so it can never run under "reset".
                            const RECT headClip{14, y, kPanel - 58, y + kRowH};
                            s.Text(g.fontBold, ui::kAccent, 14, y + 3, ClipText(name, 42), &headClip);
                        }
                        bool any = false;
                        for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) any = any || TweakChanged(rows[k].id);
                        if (row.id >= 0) s.TextRight(g.fontSmall, any ? ui::kAmber : ui::kFaint, kPanel - 16, y + 4, "reset");
                    } else if (row.kind == 4) {
                        const RECT infoClip{14, y, kPanel - 16, y + kRowH};
                        s.Text(g.fontSmall, ui::kDim, 14, y + 4, ClipText(row.label, 58), &infoClip);
                    } else if (row.kind == 5) {
                        const RECT btn{12, y + 1, kPanel - 16, y + kRowH - 1};
                        s.FillRound(btn, 5, idx == g.hoverRow ? ui::kLineHi : ui::kCardHi);
                        const RECT btnClip{20, y, kPanel - 24, y + kRowH};
                        s.Text(g.fontUi, ui::kText, 20, y + 2, ClipText(row.label, 56), &btnClip);
                    } else if (row.kind == 2) {
                        const bool on = TweakChanged(row.id);
                        const bool failed = row.id >= kLayerBase && row.id < kBoneEdit && !g.gpu.layerOk(static_cast<size_t>(row.id - kLayerBase));
                        CheckBox(s, 14, y + 3, on, failed ? ui::kBad : ui::kAmber);
                        const RECT swClip{36, y, kPanel - 18, y + kRowH};
                        s.Text(g.fontUi, failed ? ui::kBad : on ? ui::kAmber : ui::kText, 36, y + 2, ClipText(failed ? row.label + "  (does not compile)" : row.label, 52), &swClip);
                    } else if (row.kind == 6) {
                        DrawPresetChips(s, row.id, row.label, y);
                    } else if (g.morphEdit >= 0 && row.id == kMorphBase + g.morphEdit) {
                        // the rename box: the typed text with a caret, Enter commits (Esc restores)
                        std::string typed = g.morphEditBuf + "_";
                        const RECT box{12, y + 1, kPanel - 44, y + kRowH - 1};
                        s.FillRound(box, 5, ui::kLineHi);
                        s.StrokeRound(box, 5, ui::kAccent);
                        const RECT boxClip{20, y, kPanel - 52, y + kRowH};
                        s.Text(g.fontMono, ui::kText, 20, y + 3, ClipText(typed, 30), &boxClip);
                        s.TextRight(g.fontSmall, ui::kFaint, kPanel - 16, y + 4, "Enter");
                    } else {
                        DrawSliderRow(s, row.id, row.label, y);
                    }
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(rows.size()), visible, g.partPos);
                s.Begin(clip);
            }
        } else {
            const std::vector<PartRow> rows = BuildRows(*L);
            const int visible = VisibleRows(l);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(rows.size())) break;
                    const PartRow& row = rows[static_cast<size_t>(idx)];
                    const int y = l.partsTop - v.offset + i * kRowH;
                    if (!TouchesY(sub, y, y + kRowH)) continue;
                    const bool hot = idx == g.hoverRow;
                    if (row.part < 0) {
                        if (hot) s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, ui::kCardHi);
                        if (row.replaced) {   // the base outfit this slot replaced: kept visible, marked, no parts follow
                            const RECT repClip{14, y, kPanel - 190, y + kRowH};
                            s.Text(g.fontBold, ui::kAmber, 14, y + 3, "OUTFIT (REPLACED)", &repClip);
                            s.TextRight(g.fontSmall, ui::kDim, kPanel - 16, y + 4, "base outfit replaced by Added outfit");
                        } else {
                            // One master checkbox per group, like the Added outfit row: it shows or hides
                            // every part of the group at once; the arrow (or the row) collapses the list.
                            bool allOn = true, someOff = false;
                            int count = 0;
                            for (const auto& p : L->scene.parts) {
                                if (DisplayGroup(p) != row.group || p.underPiece) continue;
                                ++count;
                                const int pi = static_cast<int>(&p - L->scene.parts.data());
                                if (static_cast<size_t>(pi) < g.hidden.size() && g.hidden[static_cast<size_t>(pi)]) { allOn = false; someOff = true; }
                            }
                            (void)someOff;
                            const bool added = row.group == "Added outfit";
                            CheckBox(s, 14, y + 3, allOn, added ? ui::kAmber : ui::kAccent);
                            std::string name = row.group;
                            for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                            const bool collapsed = g.collapsedGroups.count(row.group) != 0;
                            const bool isAmber = added;
                            Expander(s, 34, y + kRowH / 2, !collapsed, ui::kAccent);
                            const RECT gClip1{52, y, kPanel - 58, y + kRowH};
                            s.Text(g.fontBold, isAmber ? ui::kAmber : ui::kAccent, 52, y + 3, ClipText(name, 44), &gClip1);
                            s.TextRight(g.fontSmall, ui::kDim, kPanel - 16, y + 4, std::to_string(count));
                        }
                    } else {
                        const auto& p = L->scene.parts[static_cast<size_t>(row.part)];
                        const bool off = static_cast<size_t>(row.part) < g.hidden.size() && g.hidden[static_cast<size_t>(row.part)];
                        const bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
                        if (hot) {
                            s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, RGB(34, 46, 70));
                            s.FillRound(RECT{8, y + 4, 11, y + kRowH - 4}, 1, ui::kAccent);
                        }
                        const COLORREF col = off ? ui::kDim : !ok ? ui::kBad : ui::kText;
                        const bool acc = p.group == "Accessories";   // the gizmo chip on the right end of accessory rows
                        CheckBox(s, 16, y + 3, !off, ok ? ui::kAccent : ui::kBad);
                        std::string name = p.nudeSwitch ? std::string("nude  (whole body, under clothes too)") : p.id;
                        if (!ok) name += "  (" + p.status + ")";
                        if (p.fromClothing) name += "  (added)";
                        if (acc && AccessorySelected() == row.part) name += "  ·  xyz";
                        // Name and the right-aligned triangle count share the row.
                        const RECT partClip{38, y, acc ? kPanel - 110 : kPanel - 64, y + kRowH};
                        s.Text(g.fontMono, col, 38, y + 3, ClipText(name, acc ? 34 : 44), &partClip);
                        const std::string tris = Count(static_cast<size_t>(row.part) < L->partTriangles.size() ? L->partTriangles[static_cast<size_t>(row.part)] : 0);
                        s.TextRight(g.fontSmall, off ? ui::kFaint : ui::kDim, acc ? kPanel - 44 : kPanel - 16, y + 4, tris);
                        if (acc) DrawAccessoryChip(s, y, AccessorySelected() == row.part, g.hoverAccChip == idx);
                    }
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(rows.size()), visible, g.partPos);
                s.Begin(clip);
            }
        }
    }

    lap(4);
    // log --------------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.logTop, l.logBottom)) {
        const RECT logCard{6, l.logTop, kPanel - 6, l.logBottom};
        s.FillRound(logCard, 8, RGB(11, 13, 19));
        s.StrokeRound(logCard, 8, ui::kLine);
        s.Text(g.fontBold, ui::kAccent, 14, l.logTop + 5, "LOG");
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 14, l.logTop + 7, "clear");
        std::lock_guard<std::mutex> lk(g.logMu);
        const int lineH = 15, top = l.logTop + 26;
        const int visible = std::max(1, (l.logBottom - 4 - top) / lineH);
        g.logScroll = std::max(0, std::min(g.logScroll, static_cast<int>(g.logLines.size()) - visible));
        const int end = static_cast<int>(g.logLines.size()) - g.logScroll;
        const int start = std::max(0, end - visible);
        const RECT lc{10, top, kPanel - 10, l.logBottom - 3};
        if (g.logLines.empty()) s.Text(g.fontLog, ui::kDim, 14, top, "no activity yet", &lc);
        for (int i = start; i < end; ++i) {
            const LogLine& line = g.logLines[static_cast<size_t>(i)];
            const int y = top + (i - start) * lineH;
            s.Text(g.fontLog, ui::kFaint, 14, y, line.time, &lc);
            s.Text(g.fontLog, KindColor(line.kind), 78, y, line.text, &lc);
        }
    }

    lap(5);
    // footer -----------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.footerTop, H)) {
        const RECT footClip{12, l.footerTop, kPanel - 10, l.footerTop + 34};
        s.Text(g.fontSmall, ui::kDim, 12, l.footerTop + 3, ClipText("E export  O open  A all  T tex  H shader  G grade  X as exported", 64), &footClip);
        std::string hint = "1-7 outfit · L lib · K bones · P pose · Y physics · N shake · D reset clothing";
        const ProgSnapshot ps = GetProgSnapshot();
        if (ps.active) hint = ps.cancelRequested ? "cancelling ..." : (ps.stage.empty() ? "working ... (Esc or Cancel to stop)" : ps.stage + "  (Esc cancels)");
        else if (g.busy) hint = "working ...";
        else if (g.exporting) hint = "exporting PMX ...";
        if (L && g.hoverPart >= 0 && static_cast<size_t>(g.hoverPart) < L->scene.parts.size()) {
            const auto& p = L->scene.parts[static_cast<size_t>(g.hoverPart)];
            hint = p.id + "  ·  " + p.status;
            if (static_cast<size_t>(g.hoverPart) < L->partTriangles.size()) hint += "  ·  " + Count(L->partTriangles[static_cast<size_t>(g.hoverPart)]) + " tris";
            if (!p.modName.empty()) hint += "  ·  " + p.modName;
            if (static_cast<size_t>(g.hoverPart) < g.hidden.size() && g.hidden[static_cast<size_t>(g.hoverPart)]) hint += "  ·  hidden";
        } else if (const int acc = AccessorySelected(); acc >= 0 && L && static_cast<size_t>(acc) < L->scene.parts.size()) {
            hint = "gizmo  ·  drag an axis to move " + L->scene.parts[static_cast<size_t>(acc)].id + "  ·  right-click its chip to reset  ·  Esc deselects";
        } else if (!modsInfo.empty() && !L) {
            hint = modsInfo;
        }
        s.Text(g.fontSmall, ps.active ? ui::kAccent : g.busy ? ui::kAccent : g.exporting ? ui::kAmber : ui::kSub, 12, l.footerTop + 17, ClipText(hint, 64), &footClip);
    }
    s.Fill(RECT{kPanel - 1, clip.top, kPanel, clip.bottom}, ui::kLine);
    s.End();
    lap(6);
}

// The loading overlay over the 3D view: title, stage, determinate bar (or a
// sliding indeterminate chunk) and Cancel. Drawn into the viewport-sized
// comp DC (origin at the view's top-left, NOT the client's), so `card` is
// shifted left by kPanel. Esc (or Cancel) sets the flag the workers poll.
void DrawProgressOverlay(HDC dc, int clientW, int clientH) {
    const ProgSnapshot ps = GetProgSnapshot();
    if (!ps.active) return;
    const RECT client = ProgOverlayRect(clientW, clientH);
    // The comp DC is viewport-sized: its (0,0) is kPanel, so shift the card.
    const RECT card{client.left - kPanel, client.top, client.right - kPanel, client.bottom};
    if (card.right <= card.left || card.bottom <= card.top) return;
    // Dim the view behind the card (solid, GDI cannot alpha-blend easily).
    HBRUSH dim = CreateSolidBrush(RGB(8, 10, 16));
    FillRect(dc, &card, dim);
    DeleteObject(dim);
    HBRUSH vs = CreateSolidBrush(ui::kCard);
    HPEN border = CreatePen(PS_SOLID, 1, ui::kLineHi);
    HGDIOBJ olds = SelectObject(dc, vs);
    HGDIOBJ oldp = SelectObject(dc, border);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, 16, 16);
    SelectObject(dc, olds);
    SelectObject(dc, oldp);
    DeleteObject(vs);
    DeleteObject(border);
    auto text = [&](HFONT font, COLORREF color, int x, int y, const std::string& s) {
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_LEFT | TA_TOP);
        const std::wstring w = Utf8ToWide(ClipText(s, 52));
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &card, w.c_str(), static_cast<UINT>(w.size()), nullptr);
        SelectObject(dc, old);
    };
    text(g.fontBold, ui::kAccent, card.left + 14, card.top + 10, ps.title);
    text(g.fontSmall, ps.cancelRequested ? ui::kAmber : ui::kSub, card.left + 14, card.top + 32,
         ps.cancelRequested ? std::string("cancelling...") : (ps.stage.empty() ? std::string("working...") : ps.stage));
    // Bar.
    const RECT bar{card.left + 14, card.top + 56, card.right - 14, card.top + 68};
    HBRUSH track = CreateSolidBrush(ui::kLine);
    FillRect(dc, &bar, track);
    DeleteObject(track);
    if (ps.frac >= 0.0) {
        const double f = std::clamp(ps.frac, 0.0, 1.0);
        if (f > 0.001) {
            RECT fill{bar.left, bar.top, bar.left + static_cast<int>((bar.right - bar.left) * f), bar.bottom};
            HBRUSH acc = CreateSolidBrush(ui::kAccent);
            FillRect(dc, &fill, acc);
            DeleteObject(acc);
        }
    } else {
        const double w = static_cast<double>(bar.right - bar.left);
        const double chunk = w * 0.28;
        const double t = static_cast<double>(GetTickCount64() % 1200) / 1200.0;
        const double x = t * (w + chunk) - chunk;
        RECT seg{static_cast<int>(bar.left + x), bar.top, static_cast<int>(bar.left + x + chunk), bar.bottom};
        RECT clipped;
        if (IntersectRect(&clipped, &seg, &bar)) {
            HBRUSH acc = CreateSolidBrush(ui::kAccent);
            FillRect(dc, &clipped, acc);
            DeleteObject(acc);
        }
    }
    // Percent on the left, Cancel on the right.
    char pct[16] = {0};
    if (ps.frac >= 0.0) std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(std::lround(ps.frac * 100.0)));
    else std::snprintf(pct, sizeof pct, "%%");
    text(g.fontSmall, ui::kSub, card.left + 14, card.top + 76, pct);
    text(g.fontSmall, ui::kDim, card.left + 60, card.top + 76, "Esc cancels");
    const RECT cancel = ProgCancelRect(card);
    HBRUSH cb = CreateSolidBrush(ps.cancelRequested ? ui::kLine : (g.progHoverCancel ? ui::kLineHi : ui::kCardHi));
    HPEN cp = CreatePen(PS_SOLID, 1, ui::kLine);
    HGDIOBJ ob = SelectObject(dc, cb);
    HGDIOBJ op = SelectObject(dc, cp);
    RoundRect(dc, cancel.left, cancel.top, cancel.right, cancel.bottom, 10, 10);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(cb);
    DeleteObject(cp);
    {
        HGDIOBJ old = SelectObject(dc, g.fontUi);
        SetTextColor(dc, ps.cancelRequested ? ui::kFaint : ui::kText);
        SetBkMode(dc, TRANSPARENT);
        std::wstring w = L"Cancel";
        RECT t = cancel;
        DrawTextW(dc, w.data(), static_cast<int>(w.size()), &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }
}

void Paint(HWND hwnd) {
    // The area to paint as separate rectangles (rcPaint is only their bounding box, which would redraw all the panel between a hovered
    // row and the footer): read before BeginPaint, which clears it.
    std::vector<RECT> parts;
    if (HRGN rgn = CreateRectRgn(0, 0, 0, 0)) {
        if (GetUpdateRgn(hwnd, rgn, FALSE) > NULLREGION) {
            const DWORD bytes = GetRegionData(rgn, 0, nullptr);
            std::vector<char> buf(bytes);
            if (bytes && GetRegionData(rgn, bytes, reinterpret_cast<RGNDATA*>(buf.data()))) {
                const RGNDATA* rd = reinterpret_cast<const RGNDATA*>(buf.data());
                const RECT* r = reinterpret_cast<const RECT*>(rd->Buffer);
                parts.assign(r, r + rd->rdh.nCount);
            }
        }
        DeleteObject(rgn);
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (parts.empty() || parts.size() > 12) parts.assign(1, ps.rcPaint);   // a complex region: its bounding box will do
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = std::max<int>(0, static_cast<int>(rc.right) - kPanel), H = rc.bottom;
    const double tPaint = NowMs();
    const RECT viewport{kPanel, 0, rc.right, rc.bottom};
    const RECT panelRect{0, 0, kPanel, H};
    bool viewDamaged = false;
    for (const RECT& p : parts) { RECT t; viewDamaged = viewDamaged || IntersectRect(&t, &p, &viewport) != 0; }
    const bool renderNeeded = g.dirty || g.fw != W || g.fh != H;
    if (renderNeeded) {
        Render(W, H);
        g.dirty = false;
        g.prof.render += NowMs() - tPaint;
        ++g.prof.renders;
    }
    const double tBlit = NowMs();
    const bool overlayActive = GetProgSnapshot().active;
    if ((viewDamaged || renderNeeded) && !g.frame.empty() && g.fw == W && g.fh == H && W > 0 && H > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        // Compose the frame + the loading card (or the diagnostic strip + legend) into an off-screen DC, then blit once: the picture
        // underneath never flashes bare between two passes.
        static HDC compDc = nullptr;
        static HBITMAP compBmp = nullptr;
        static HGDIOBJ compOld = nullptr;
        static int compW = 0, compH = 0;
        if (compDc && (compW != W || compH != H)) {
            SelectObject(compDc, compOld);
            DeleteObject(compBmp);
            DeleteDC(compDc);
            compDc = nullptr; compBmp = nullptr; compOld = nullptr;
        }
        if (!compDc) {
            HDC screen = GetDC(nullptr);
            compDc = CreateCompatibleDC(screen);
            compBmp = CreateCompatibleBitmap(screen, W, H);
            ReleaseDC(nullptr, screen);
            if (compDc && compBmp) {
                compOld = SelectObject(compDc, compBmp);
                compW = W; compH = H;
            }
        }
        if (compDc && compBmp) {
            StretchDIBits(compDc, 0, 0, W, H, 0, 0, W, H, g.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            if (overlayActive) {
                DrawProgressOverlay(compDc, static_cast<int>(rc.right), H);
            } else {
                // The diagnostic mode strip and legend: drawn here, on the off-screen DC, so the window never shows a
                // partially drawn strip (the BeginPaint DC is clipped to the invalidation region).
                if (W > 100 && H > 120) {
                    DrawModeLegend(compDc, 0, W, H);
                    DrawModeStrip(compDc, 0, W, H);
                }
            }
            BitBlt(dc, kPanel, 0, W, H, compDc, 0, 0, SRCCOPY);
        } else {
            StretchDIBits(dc, kPanel, 0, W, H, 0, 0, W, H, g.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            if (overlayActive) DrawProgressOverlay(dc, static_cast<int>(rc.right), H);
            else if (W > 100 && H > 120) {
                DrawModeLegend(dc, kPanel, W, H);
                DrawModeStrip(dc, kPanel, W, H);
            }
        }
    }
    const double tPanel = NowMs();
    g.prof.blit += tPanel - tBlit;
    // The panel is drawn off screen, only where it changed, then copied.
    if (g.panel.Resize(kPanel, H)) {   // new surface: blank, so all of it is drawn, though only the damaged part needs showing
        DrawPanel(g.panel, panelRect);
        for (const RECT& p : parts) { RECT t; if (IntersectRect(&t, &p, &panelRect)) g.panel.Present(dc, t, t.left, t.top); }
    } else {
        for (const RECT& p : parts) {
            RECT t;
            if (!IntersectRect(&t, &p, &panelRect)) continue;
            const double tDraw = NowMs();
            DrawPanel(g.panel, t);
            const double tCopy = NowMs();
            g.prof.panelDraw += tCopy - tDraw;
            g.panel.Present(dc, t, t.left, t.top);
            g.prof.panelCopy += NowMs() - tCopy;
        }
    }
    g.prof.panel += NowMs() - tPanel;
    ++g.prof.paints;
    EndPaint(hwnd, &ps);
}

void ResetView(float yaw) { g.cam = {}; g.cam.yawDeg = yaw; g.dirty = true; InvalidateRect(g.hwnd, nullptr, FALSE); }

void ShowAll() {
    g.hidden.assign(g.hidden.size(), 0);
    g.soloPart = -1;
    for (auto L = Current(); L; L = nullptr)
        for (size_t i = 0; i < L->scene.parts.size(); ++i)
            if (L->scene.parts[i].id.find(".shoes") != std::string::npos) L->scene.parts[i].visibleInPreview = true;
    ReshapeNow(true);
    g.dirty = true;
    Log("showing every part");
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void ToggleIssues() {
    g.showIssues = !g.showIssues;
    g.partScroll = 0;
    g.partPos = 0;
    g.hoverRow = g.hoverPart = -1;
    InvalidatePanel();
}

void ExportFolderIfNeeded() {
    // kept separate so the WM_KEYDOWN / button paths read the same
    StartExport();
}

void OpenCard() {
    std::string c = PickCard(g.hwnd);
    if (!c.empty()) {
        g.cam = {};
        {
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.clothingPath.clear();
            g.clothingName.clear();
            g.clothingOutfit = -1;
        }
        StartLoad(c, -1);
    }
}

// An outfit file dropped on a loaded character: it overwrites the outfit on
// screen in memory only (never saved) and stays pinned to that slot (see
// StartLoad). Hair worn as accessories stays.
void ApplyClothingFile(const std::string& path) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.cardPath.empty()) { Log("open a character first, then drop the outfit file", LogKind::Warn); return; }
    if (g.busy) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    card::ClothingFile cl;
    try {
        cl = card::LoadClothingFile(path);
        if (!cl.supported) { Log(cl.supportNote, LogKind::Error); return; }
    } catch (const std::exception& e) {
        Log(std::string("not a valid outfit file: ") + e.what(), LogKind::Error);
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.clothingPath = path;
        g.clothingName = cl.displayName();
        g.clothingOutfit = L->outfit;   // the outfit on screen
    }
    g.cam = {};
    StartLoad(g.cardPath, L->outfit, true);
}

// Picking a slot is the same reload Left/Right does, just aimed. The reload
// reapplies the outfit file only when the picked slot is its pinned one, so
// every other slot shows the card's own clothing (StartLoad).
void ChooseOutfit(int index) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.busy || g.cardPath.empty()) return;
    if (index < 0 || index >= L->outfits || index == L->outfit) return;
    StartLoad(g.cardPath, index, true);
}

// Drops the imported outfit file: the current outfit is re-assembled from the
// card's own clothes. The card on disk is never saved, so this only clears the
// in-memory override; the next visit to the pinned slot is the card default too.
void ResetClothing() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.cardPath.empty()) return;
    if (g.busy) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.clothingPath.empty()) return;
        name = g.clothingName;
        g.clothingPath.clear();
        g.clothingName.clear();
        g.clothingOutfit = -1;
    }
    Log("clothing reset to the card's own outfit" + (name.empty() ? std::string() : " (dropped " + name + ")"), LogKind::Good);
    g.cam = {};
    StartLoad(g.cardPath, L->outfit, true);
}

// Moves the imported outfit to the next/previous outfit slot and shows it
// there, without re-dropping the file (the card is reloaded from disk and the
// file re-applied to the new slot).
void MoveClothing(int delta) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.cardPath.empty()) return;
    if (g.busy) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    int target = -1;
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        if (g.clothingPath.empty()) return;
        name = g.clothingName;
        target = g.clothingOutfit + delta;
        if (target < 0 || target >= L->outfits) return;
        g.clothingOutfit = target;
    }
    Log("clothing moved to outfit " + std::to_string(target + 1) + (name.empty() ? std::string() : " (" + name + ")"), LogKind::Good);
    StartLoad(g.cardPath, target, true);
}

