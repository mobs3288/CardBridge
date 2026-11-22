// viewer fragment: Model Explorer for native PMX models (included by main.cpp)

void OpenWorkbench();   // defined later in Workbench.inc

struct ExplorerRow {
    enum Kind { Header, Mesh, Material, Bone, Morph, Physics, Texture, Frame, Note } kind = Header;
    int index = -1;
    std::string label, detail;
};

std::vector<ExplorerRow> BuildExplorerRows(const Loaded& L) {
    std::vector<ExplorerRow> rows;
    if (!L.NativePmx() || !L.pmxDoc) return rows;
    const pmx::Model& doc = *L.pmxDoc;
    auto section = [&](const char* name, size_t count, const char* key) {
        ExplorerRow r; r.kind = ExplorerRow::Header; r.label = name;
        r.detail = std::to_string(count);
        rows.push_back(r);
        (void)key;
    };
    auto collapsed = [&](const char* key) { return g.explorerCollapsed.count(key) != 0; };

    rows.push_back([] { ExplorerRow r; r.kind = ExplorerRow::Header; r.label = "MODEL"; r.detail = ""; return r; }());
    if (!collapsed("MODEL")) {
        ExplorerRow n; n.kind = ExplorerRow::Note; n.label = doc.nameEn.empty() ? doc.name : doc.nameEn; n.detail = "name"; rows.push_back(n);
        n.detail = "PMX " + std::to_string(static_cast<int>(doc.format.version * 10) / 10.0f);
        n.label = doc.format.encoding() == 1 ? "UTF-8 text" : "UTF-16LE text"; rows.push_back(n);
        n.label = "additional UV"; n.detail = std::to_string(doc.format.additionalUv()); rows.push_back(n);
    }

    section("MESHES", L.scene.meshes.size(), "MESHES");
    if (!collapsed("MESHES")) {
        for (size_t i = 0; i < L.scene.meshes.size(); ++i) {
            ExplorerRow r; r.kind = ExplorerRow::Mesh; r.index = static_cast<int>(i);
            r.label = L.scene.meshes[i].name.empty() ? "mesh " + std::to_string(i) : L.scene.meshes[i].name;
            r.detail = Count(L.scene.meshes[i].vertices.size()) + " v";
            rows.push_back(r);
        }
    }
    section("MATERIALS", doc.materials.size(), "MATERIALS");
    if (!collapsed("MATERIALS")) {
        for (size_t i = 0; i < doc.materials.size(); ++i) {
            ExplorerRow r; r.kind = ExplorerRow::Material; r.index = static_cast<int>(i);
            r.label = doc.materials[i].nameEn.empty() ? doc.materials[i].name : doc.materials[i].nameEn;
            r.detail = "tex " + std::to_string(doc.materials[i].texture);
            rows.push_back(r);
        }
    }
    section("BONES", doc.bones.size(), "BONES");
    if (!collapsed("BONES")) {
        for (size_t i = 0; i < doc.bones.size(); ++i) {
            ExplorerRow r; r.kind = ExplorerRow::Bone; r.index = static_cast<int>(i);
            std::string name = doc.bones[i].nameEn.empty() ? doc.bones[i].name : doc.bones[i].nameEn;
            int depth = 0;
            for (int p = doc.bones[i].parent; p >= 0 && depth < 8; p = doc.bones[static_cast<size_t>(p)].parent) ++depth;
            r.label = std::string(depth * 2, ' ') + name;
            r.detail = (doc.bones[i].flags & 0x0020) ? "IK" : "";
            rows.push_back(r);
        }
    }
    const size_t nMorph = doc.morphs.size();
    size_t nVertex = 0, nMaterial = 0, nOther = 0;
    for (const auto& mo : doc.morphs) {
        if (mo.type == 1) ++nVertex;
        else if (mo.type == 8) ++nMaterial;
        else ++nOther;
    }
    section("MORPHS", nMorph, "MORPHS");
    if (!collapsed("MORPHS")) {
        ExplorerRow r; r.kind = ExplorerRow::Note;
        r.label = "vertex " + std::to_string(nVertex) + "  material " + std::to_string(nMaterial) + "  other " + std::to_string(nOther);
        rows.push_back(r);
        for (size_t i = 0; i < doc.morphs.size(); ++i) {
            if (doc.morphs[i].type != 1) continue;   // vertex morphs are the editable ones this phase
            ExplorerRow m; m.kind = ExplorerRow::Morph; m.index = static_cast<int>(i);
            m.label = doc.morphs[i].name; m.detail = std::to_string(doc.morphs[i].vertices.size());
            rows.push_back(m);
        }
    }
    section("PHYSICS", doc.rigidBodies.size() + doc.joints.size(), "PHYSICS");
    if (!collapsed("PHYSICS")) {
        ExplorerRow r; r.kind = ExplorerRow::Note;
        r.label = std::to_string(doc.rigidBodies.size()) + " rigid bodies, " + std::to_string(doc.joints.size()) + " joints";
        r.detail = "read-only"; rows.push_back(r);
    }
    section("TEXTURES", doc.textures.size(), "TEXTURES");
    if (!collapsed("TEXTURES")) {
        for (size_t i = 0; i < doc.textures.size(); ++i) {
            ExplorerRow r; r.kind = ExplorerRow::Texture; r.index = static_cast<int>(i);
            r.label = fileio::FileName(doc.textures[i]); r.detail = ""; rows.push_back(r);
        }
    }
    section("DISPLAY FRAMES", doc.frames.size(), "DISPLAY FRAMES");
    if (!collapsed("DISPLAY FRAMES")) {
        for (size_t i = 0; i < doc.frames.size(); ++i) {
            ExplorerRow r; r.kind = ExplorerRow::Frame; r.index = static_cast<int>(i);
            r.label = doc.frames[i].nameEn.empty() ? doc.frames[i].name : doc.frames[i].nameEn;
            r.detail = std::to_string(doc.frames[i].items.size()); rows.push_back(r);
        }
    }
    return rows;
}

void DrawExplorerRows(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const std::vector<ExplorerRow> rows = BuildExplorerRows(L);
    const int visible = std::max(1, (l.partsBottom - l.partsTop) / kRowH);
    g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
    g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    const RECT listClip{6, l.partsTop, kPanel - 6, l.partsBottom};
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip) == 0) return;
    s.Begin(sub);
    const RECT textClip{10, l.partsTop, kPanel - 16, l.partsBottom};
    const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos);
    for (int i = 0; i < v.count; ++i) {
        const int idx = v.first + i;
        if (idx >= static_cast<int>(rows.size())) break;
        const ExplorerRow& row = rows[static_cast<size_t>(idx)];
        const int y = l.partsTop - v.offset + i * kRowH;
        if (!TouchesY(sub, y, y + kRowH)) continue;
        if (idx == g.hoverRow && row.kind != ExplorerRow::Header)
            s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, ui::kRad, ui::kCardHi);
        if (row.kind == ExplorerRow::Header) {
            const std::string key = row.label;
            Expander(s, 12, y + kRowH / 2, g.explorerCollapsed.count(key) == 0, ui::kDim);
            s.Text(g.fontSemi, ui::kSub, 30, y + 4, row.label, &textClip);
            s.TextRight(g.fontSmall, ui::kFaint, kPanel - 14, y + 5, row.detail, &textClip);
        } else {
            const COLORREF col = row.kind == ExplorerRow::Note ? ui::kDim :
                                 row.kind == ExplorerRow::Bone ? ui::kText : ui::kSub;
            s.Text(row.kind == ExplorerRow::Bone ? g.fontMono : g.fontUi, col, 18, y + 3,
                   ClipText(row.label, 40), &textClip);
            if (!row.detail.empty()) s.TextRight(g.fontSmall, ui::kFaint, kPanel - 14, y + 5, row.detail, &textClip);
        }
    }
    ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(rows.size()), visible, g.partPos);
    s.Begin(clip);
}

void ExplorerListClick(int x, int y, const PanelLayout& l, Loaded& L) {
    const std::vector<ExplorerRow> rows = BuildExplorerRows(L);
    const int idx = RowIndexAt(rows.size(), l, x, y);
    if (idx < 0) return;
    const ExplorerRow& row = rows[static_cast<size_t>(idx)];
    if (row.kind == ExplorerRow::Header) {
        const std::string key = row.label;
        if (g.explorerCollapsed.count(key)) g.explorerCollapsed.erase(key);
        else g.explorerCollapsed.insert(key);
        g.hoverRow = -1;
        InvalidatePanel();
        return;
    }
    if (row.kind == ExplorerRow::Bone && row.index >= 0 && static_cast<size_t>(row.index) < L.scene.bones.size()) {
        g.selBoneName = L.scene.bones[static_cast<size_t>(row.index)].name;
        g.tab = TabBones;
        Log("bone: " + g.selBoneName);
    } else if (row.kind == ExplorerRow::Material) {
        g.selMaterial = row.index;
        g.tab = TabMaterials;
        OpenWorkbench();
        Log("material: " + row.label);
    } else if (row.kind == ExplorerRow::Morph) {
        g.tab = TabMorphs;
        Log("morph: " + row.label);
    } else if (row.kind == ExplorerRow::Texture) {
        Log("texture: " + row.label);
    } else if (row.kind == ExplorerRow::Frame) {
        Log("display frame: " + row.label + " (" + row.detail + " entries)");
    } else {
        return;
    }
    g.partScroll = 0;
    g.partPos = 0;
    InvalidatePanel();
}
