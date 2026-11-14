// viewer fragment: part list, skeleton (included in order by main.cpp)

// ---- part list --------------------------------------------------------------------------------------------------------
std::vector<PartRow> BuildRows(const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    std::vector<PartRow> rows;
    static const char* order[] = {"Body", "Head", "Hair", "Outfit", "Accessories", "Added outfit"};
    std::set<std::string> seen;
    const bool baseReplaced = L.card.hasClothing && L.card.clothingOutfit == L.outfit;
    auto addGroup = [&](const std::string& group) {
        bool any = false;
        for (const auto& p : scene.parts) {
            if (DisplayGroup(p) != group || p.underPiece) continue;   // under pieces follow their garments and the Nude body box
            if (!any) { rows.push_back({-1, group, false}); any = true; }
            rows.push_back({static_cast<int>(&p - scene.parts.data()), group, false});
        }
        if (!any && group == "Outfit" && baseReplaced)
            rows.push_back({-1, std::string("Outfit"), true});   // the base outfit this slot replaced (kept visible, marked)
    };
    for (const char* group : order) { addGroup(group); seen.insert(group); }
    for (const auto& p : scene.parts)
        if (!seen.count(DisplayGroup(p)) && !p.underPiece) { addGroup(DisplayGroup(p)); seen.insert(DisplayGroup(p)); }
    // Collapsed groups lose their part rows but keep the header (the replaced-base header is never collapsible).
    std::vector<PartRow> out;
    std::string collapsed;
    for (const auto& row : rows) {
        if (row.part < 0) { collapsed = !row.replaced && g.collapsedGroups.count(row.group) ? row.group : std::string(); out.push_back(row); }
        else if (collapsed != row.group) out.push_back(row);
    }
    return out;
}

int VisibleRows(const PanelLayout& l) { return std::max(1, (l.partsBottom - l.partsTop) / kRowH); }

int RowIndexAt(size_t count, const PanelLayout& l, int x, int y) {
    if (x < 0 || x >= kPanel || y < l.partsTop || y >= l.partsBottom) return -1;
    const int row = (y - l.partsTop + static_cast<int>(std::lround(g.partPos * kRowH))) / kRowH;   // the list as it is drawn now (it may be mid-scroll)
    return row >= 0 && static_cast<size_t>(row) < count ? row : -1;
}
int RowAt(const std::vector<PartRow>& rows, const PanelLayout& l, int x, int y) { return RowIndexAt(rows.size(), l, x, y); }

// Shows or hides every part of a group at once (the group-header master checkbox, used
// for Body / Head / Hair / Outfit / Accessories / Added outfit alike).
void ToggleGroup(const std::string& group, assemble::Scene& scene) {
    bool allOn = true;
    for (size_t i = 0; i < scene.parts.size() && i < g.hidden.size(); ++i) {
        if (DisplayGroup(scene.parts[i]) != group || scene.parts[i].underPiece) continue;
        if (g.hidden[i]) { allOn = false; break; }
    }
    for (size_t i = 0; i < scene.parts.size() && i < g.hidden.size(); ++i)
        if (DisplayGroup(scene.parts[i]) == group && !scene.parts[i].underPiece) g.hidden[i] = allOn ? 1 : 0;
    g.soloPart = -1;
    // Hiding shoes re-drives the foot pitch (settleHeels reads visibleInPreview);
    // nudging the body through the same re-pose path keeps everything honest.
    for (size_t i = 0; i < scene.parts.size() && i < g.hidden.size(); ++i)
        if (scene.parts[i].id.find(".shoes") != std::string::npos) scene.parts[i].visibleInPreview = !g.hidden[i];
    Log(std::string(allOn ? "hide " : "show ") + group);
    ReshapeNow(true);
    g.dirty = true;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    InvalidatePanel();
}

// A slider row: label on the left, the track, the value on the right. The Material
// Workbench window has its own, wider track, so the two ends are parameters.
constexpr int kTrackL = 168, kTrackR = 300;
float SliderValueAt(const SliderRef& r, int x, int trackL = kTrackL, int trackR = kTrackR) {
    float t = std::clamp(static_cast<float>(x - trackL) / std::max(1, trackR - trackL), 0.0f, 1.0f);
    float v = r.lo + t * (r.hi - r.lo);
    if (!std::isfinite(v)) return r.lo;
    if (r.step > 0) v = std::round(v / r.step) * r.step;   // step 0: the typed value is kept exactly, no snap
    return std::clamp(v, r.lo, r.hi);
}

int ButtonAt(const PanelLayout& l, int x, int y) {
    POINT p{x, y};
    if (PtInRect(&l.open, p)) return 1;
    if (PtInRect(&l.exportPmx, p)) return 2;
    if (PtInRect(&l.issues, p)) return 3;
    if (PtInRect(&l.reset, p)) return 4;
    if (PtInRect(&l.all, p)) return 5;
    if (PtInRect(&l.asExported, p)) return 6;
    if (PtInRect(&l.clothingReset, p)) return 7;   // reset the imported outfit to the card default
    if (PtInRect(&l.clothingPrev, p)) return 8;    // move the imported outfit to the previous outfit slot
    if (PtInRect(&l.clothingNext, p)) return 9;    // move the imported outfit to the next outfit slot
    if (PtInRect(&l.clothingExport, p)) return 21; // export the shown outfit as an outfit file (【KoiKatuClothes】 png)
    // The Materials tab's launcher button opens the Material Workbench window. Its rect
    // exists in every layout (one LayoutFor), so it only counts on that tab, and before
    // the tab branch below (20 would otherwise mean "tab 10").
    if (g.tab == TabMaterials && PtInRect(&l.matOpen, p)) return 20;
    for (int i = 0; i < TabCount; ++i) if (PtInRect(&l.tabs[i], p)) return 10 + i;
    return 0;
}

// The outfit slots, as a row of numbered buttons under the name line. Koikatsu
// cards hold seven; anything beyond the row keeps to the arrow keys.
constexpr int kOutfitChipW = 46, kOutfitChipH = 22, kOutfitChipGap = 4;

int OutfitChipsPerRow() {
    return std::max(1, (kPanel - 20 + kOutfitChipGap) / (kOutfitChipW + kOutfitChipGap));
}

RECT OutfitChipRect(const PanelLayout& l, int index) {
    const int perRow = OutfitChipsPerRow();
    const int x = 10 + (index % perRow) * (kOutfitChipW + kOutfitChipGap);
    const int y = l.outfitChipsTop + (index / perRow) * (kOutfitChipH + kOutfitChipGap);
    return RECT{x, y, x + kOutfitChipW, y + kOutfitChipH};
}

int OutfitChipAt(const PanelLayout& l, int x, int y) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return -1;
    const int count = std::min(L->outfits, OutfitChipsPerRow());
    for (int i = 0; i < count; ++i) {
        POINT p{x, y};
        RECT chip = OutfitChipRect(l, i);
        if (PtInRect(&chip, p)) return i;
    }
    return -1;
}

// ---- skeleton overlay -------------------------------------------------------------------------------------------------
// Draws the bones the last picture was made with (pose included) over it and remembers where each landed, for picking.
void DrawSkeleton(int w, int h) {
    g.boneScreen.clear();
    std::shared_ptr<Loaded> L = Current();
    if (!g.showBones || !g.lastGpu || !L) return;
    exporter::DrawSkeletonOverlay(L->scene, g.boneWorld, *g.lastGpu, g.frame, w, h, g.selBoneName.empty() ? -1 : L->scene.boneIndex(g.selBoneName), g.hoverBone, g.showHelpers, &g.boneScreen);
}

// Turns (or, with Ctrl, moves) a bone by a pointer movement: the bone's tip follows the pointer, as if it were grabbed there. The new
// angles go into the bone's edit and the views re-skin at once.
void DragBone(int bone, float dx, float dy, bool move) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || bone < 0 || static_cast<size_t>(bone) >= L->scene.bones.size() || g.boneWorld.size() != L->scene.bones.size() || !g.lastGpu) return;
    const assemble::Scene& scene = L->scene;
    const size_t n = scene.bones.size();
    // the picture's right and up in the world (the same camera GpuView::Render builds)
    const float yaw = g.cam.yawDeg * 3.14159265f / 180.0f, pit = g.cam.pitchDeg * 3.14159265f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs};
    // the tip: the nearest descendant that is not on the bone itself (a shown bone before a helper at the same depth; helpers such as
    // cf_s_hand_L stand on their parent, so the search goes on below them), else the parent's direction extended
    const Vec3 pivot = g.boneWorld[static_cast<size_t>(bone)];
    int tipBone = -1;
    {
        auto apart = [&](size_t i) {
            const Vec3 d{g.boneWorld[i].x - pivot.x, g.boneWorld[i].y - pivot.y, g.boneWorld[i].z - pivot.z};
            return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) > 1e-3f;
        };
        std::vector<int> level{bone};
        for (int depth = 0; depth < 4 && tipBone < 0 && !level.empty(); ++depth) {
            std::vector<int> below;
            int shown = -1, any = -1;
            for (size_t i = 0; i < n; ++i) {
                if (static_cast<int>(i) == bone || std::find(level.begin(), level.end(), scene.bones[i].parent) == level.end()) continue;
                below.push_back(static_cast<int>(i));
                if (!apart(i)) continue;
                if (any < 0) any = static_cast<int>(i);
                if (shown < 0 && !exporter::IsHelperBone(scene.bones[i].name)) shown = static_cast<int>(i);
            }
            tipBone = shown >= 0 ? shown : any;
            level = std::move(below);
        }
    }
    Vec3 tip;
    if (tipBone >= 0) tip = g.boneWorld[static_cast<size_t>(tipBone)];
    else {
        const int par = scene.bones[static_cast<size_t>(bone)].parent;
        Vec3 back = par >= 0 ? Vec3{pivot.x - g.boneWorld[static_cast<size_t>(par)].x, pivot.y - g.boneWorld[static_cast<size_t>(par)].y, pivot.z - g.boneWorld[static_cast<size_t>(par)].z} : Vec3{0, -0.05f, 0};
        tip = pivot + normalize(back) * 0.05f;
    }
    Vec3 arm{tip.x - pivot.x, tip.y - pivot.y, tip.z - pivot.z};
    const float armLen = std::sqrt(arm.x * arm.x + arm.y * arm.y + arm.z * arm.z);
    if (armLen < 1e-5f) return;
    float ax, ay, bx, by, depth;
    if (!g.lastGpu->Project(pivot, ax, ay, depth) || !g.lastGpu->Project(tip, bx, by, depth)) return;
    const float pixels = std::max(8.0f, std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)));
    const float perPixel = armLen / pixels;                        // world length of one pixel at the bone
    const Vec3 delta = (right * dx + up * (-dy)) * perPixel;      // the pointer's movement in the world
    assemble::Pose pose = BuildPose(*L);
    assemble::BonePose& e = g.boneEdits[scene.bones[static_cast<size_t>(bone)].name];
    if (move) e.pos = assemble::MoveBone(scene, pose, static_cast<size_t>(bone), delta);
    else {
        const Vec3 dir = normalize(arm);
        const Mat4 turn = assemble::RotationBetween(dir, normalize(arm + delta));
        e.rot = assemble::TurnBone(scene, pose, static_cast<size_t>(bone), turn);
    }
    ApplyPoseLive();
}

// A click in the view on a bone of the overlay selects it (the nearest within a few pixels) and shows it in the Bones tab.
void PickBoneAt(int px, int py) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.boneScreen.empty()) return;
    const int best = exporter::PickBone(g.boneScreen, static_cast<float>(px), static_cast<float>(py), 14.0f);
    if (best < 0) return;
    SelectBone(L->scene, best);
    g.tab = TabBones;
    g.showIssues = false;
    Log("bone " + L->scene.bones[static_cast<size_t>(best)].name);
    g.dirty = true;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

