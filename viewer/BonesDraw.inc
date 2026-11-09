// viewer fragment: Bones tab + header draw (included in order by main.cpp)

// The Bones tab: the skeleton switch row, the bone tree, and under it the editor of the selected bone.
void DrawBonesTab(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    {   // the two switches
        const int y = l.partsTop;
        CheckBox(s, 12, y + 3, g.showBones, ui::kAmber);
        const RECT skClip{32, y, 206, y + kRowH};
        s.Text(g.fontUi, g.showBones ? ui::kAmber : ui::kText, 32, y + 2, "skeleton in the view (K)", &skClip);
        CheckBox(s, 214, y + 3, g.showHelpers, ui::kAmber);
        const RECT hpClip{234, y, kPanel - 14, y + kRowH};
        s.Text(g.fontUi, g.showHelpers ? ui::kAmber : ui::kText, 234, y + 2, "helper bones", &hpClip);
    }
    // Keep tree names clear of the scroll thumb at the right edge.
    const RECT listClip{6, l.boneListTop, kPanel - 18, l.boneListBottom};
    const std::vector<BoneRow> rows = BuildBoneRows(scene);
    const int visible = std::max(1, (l.boneListBottom - l.boneListTop) / kRowH);
    g.boneScroll = std::max(0, std::min(g.boneScroll, static_cast<int>(rows.size()) - visible));
    g.bonePos = std::clamp(g.bonePos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip)) {
        s.Begin(sub);
        const ListView v = ViewOf(l.boneListTop, l.boneListBottom, g.bonePos);
        for (int i = 0; i < v.count; ++i) {
            const int idx = v.first + i;
            if (idx >= static_cast<int>(rows.size())) break;
            const BoneRow& r = rows[static_cast<size_t>(idx)];
            const std::string& name = scene.bones[static_cast<size_t>(r.bone)].name;
            const int y = l.boneListTop - v.offset + i * kRowH;
            const bool sel = name == g.selBoneName;
            auto edit = g.boneEdits.find(name);
            const bool edited = edit != g.boneEdits.end() && !assemble::IsIdentity(edit->second);
            if (sel || idx == g.hoverRow) {
                s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, sel ? RGB(36, 50, 78) : ui::kCardHi);
                if (sel) s.FillRound(RECT{8, y + 4, 11, y + kRowH - 4}, 1, ui::kAccent);
            }
            const int x0 = 16 + std::min(r.depth, 12) * 10;
            if (r.kids) Expander(s, x0, y + kRowH / 2, r.open, ui::kAccent);
            s.Text(g.fontUi, edited ? ui::kAmber : sel ? RGB(240, 246, 255) : ui::kText, x0 + 14, y + 2, exporter::BoneLabelEn(name), &listClip);
        }
        ScrollBar(s, l.boneListTop, l.boneListBottom, static_cast<int>(rows.size()), visible, g.bonePos);
        s.Begin(clip);
    }
    // the editor
    s.Fill(RECT{10, l.boneEditTop - 3, kPanel - 10, l.boneEditTop - 2}, ui::kLine);
    const RECT headClip{10, l.boneEditTop, kPanel - 10, l.boneEditTop + kRowH};
    const int sel = g.selBoneName.empty() ? -1 : scene.boneIndex(g.selBoneName);
    {   // undo, redo, save and load the pose: always there
        auto ghost = [&](const RECT& r, const char* label, bool enabled) {
            s.FillRound(r, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, enabled ? ui::kText : ui::kFaint, r, label);
        };
        ghost(l.boneUndo, "undo", !g.undoStack.empty());
        ghost(l.boneRedo, "redo", !g.redoStack.empty());
        ghost(l.boneSave, "save pose", true);
        ghost(l.boneLoad, "load pose", true);
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.boneUndo.top + 4, "Ctrl+Z/Y/S/O");
    }
    if (sel < 0) {
        s.Text(g.fontUi, ui::kSub, 12, l.boneEditTop + 2, "click a bone in the list or in the view (turn on the skeleton)", &headClip);
        return;
    }
    s.Text(g.fontBold, ui::kAccent, 12, l.boneEditTop + 2, exporter::BoneLabelEn(g.selBoneName) + "   " + g.selBoneName, &headClip);
    static const char* const labels[kBoneEditCount] = {"Move X", "Move Y", "Move Z", "Rotate X", "Rotate Y", "Rotate Z", "Scale X", "Scale Y", "Scale Z"};
    for (int k = 0; k < kBoneEditCount; ++k)
        if (TouchesY(clip, l.boneEditTop + (1 + k) * kRowH, l.boneEditTop + (2 + k) * kRowH)) DrawSliderRow(s, kBoneEdit + k, labels[k], l.boneEditTop + (1 + k) * kRowH);
    s.FillRound(l.boneResetOne, 5, ui::kCardHi);
    s.TextCentered(g.fontUi, ui::kSub, l.boneResetOne, "reset this bone");
    s.FillRound(l.boneResetAll, 5, ui::kCardHi);
    s.TextCentered(g.fontUi, ui::kSub, l.boneResetAll, "reset every bone");
    s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.boneResetOne.top + 4, "wheel: fine step");
}

void DrawHeader(ui::Surface& s, const RECT& clip, const PanelLayout& l, const std::shared_ptr<Loaded>& L, const std::string& modsInfo) {
    (void)modsInfo;
    s.FillGradient(RECT{0, 0, kPanel - 1, kHeaderBottom - 2}, 0, RGB(18, 25, 44), RGB(30, 22, 52));
    s.FillGradient(RECT{0, kHeaderBottom - 2, kPanel - 1, kHeaderBottom}, 0, ui::kAccent, ui::kAccent2);
    s.Logo(l.logo, 12);
    s.StrokeRound(l.logo, 12, ui::kLineHi);
    const int tx = 80, right = kPanel - 12;
    const RECT line{tx, 0, right, kHeaderBottom};
    const int cardW = s.TextWidth(g.fontTitle, "Card");
    s.Text(g.fontTitle, ui::kText, tx, 8, "Card");
    s.Text(g.fontTitle, ui::kAccent, tx + cardW, 8, "Bridge");
    s.TextRight(g.fontSmall, ui::kDim, right, 14, std::string("v") + CARDBRIDGE_VERSION);
    std::string title = L ? L->title : std::string();
    if (L && !L->scene.parts.empty()) {
        if (title.empty()) title = "(unnamed card)";
        s.Text(g.fontBold, ui::kText, tx, 35, title, &line);
        size_t listed = 0;
        for (const auto& p : L->scene.parts) listed += !p.underPiece;
        s.Text(g.fontSmall, ui::kSub, tx, 52,
               Count(Triangles(L->scene)) + " triangles  ·  " + std::to_string(L->scene.materials.size()) + " materials  ·  " + std::to_string(listed) + " parts", &line);
        size_t warn = 0, err = 0;
        for (const auto& i : L->scene.issues) (i.severity == "error" ? err : warn)++;
        s.Text(g.fontSmall, warn + err ? ui::kAmber : ui::kGood, tx, 68, std::to_string(err) + " errors, " + std::to_string(warn) + " warnings  [I]", &line);
        std::string mode;
        if (g.showExp && g.expOk) mode = g.expGrade ? "as exported, MMD look (graded)" : "as exported, MMD look";
        else if (g.gpuOk) mode = g.gpu.adapter() + (g.gpu.kind() == exporter::GpuKind::Warp ? " · CPU" : " · GPU" + std::string(g.gpu.gpuSkin() ? " · skin on GPU" : ""));
        else mode = "software";
        mode += g.textured ? "" : "  ·  untextured";
        s.TextRight(g.fontSmall, g.gpuOk ? ui::kAccent : ui::kSub, right, 68, Shorten(mode, 34));
    } else if (L) {
        // The assembly threw (unsupported card, broken container): show why instead of a row of zeros.
        s.Text(g.fontBold, ui::kBad, tx, 33, "could not assemble this card", &line);
        if (!L->notes.empty()) s.Text(g.fontSmall, RGB(226, 184, 184), tx, 52, ClipText(L->notes[0], 54), &line);
        s.Text(g.fontSmall, ui::kSub, tx, 68, "details in the log below  ·  O opens another card", &line);
    } else {
        s.Text(g.fontBold, ui::kText, tx, 33, "Drop a card here, or press O", &line);
        s.Text(g.fontSmall, g.busy ? ui::kAccent : ui::kSub, tx, 52, ClipText(g.status, 54), &line);
        s.Text(g.fontSmall, ui::kDim, tx, 68, "library: " + (g.settings.library.empty() ? std::string("(not set)") : Shorten(g.settings.library, 42)), &line);
    }
    (void)clip;
}

