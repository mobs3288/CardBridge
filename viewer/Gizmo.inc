// viewer fragment: the accessory move gizmo (included in order by main.cpp, after Parts.inc)
// An accessory hangs on a body attach node, but everything the item itself added to the skeleton carries its part id as
// the bone's `source` (the item root bone first, then the N_move chain). The gizmo translates those root bones through the
// ordinary pose (assemble::MoveBone), so the meshes follow in the preview and in an export exactly like any pose edit, and
// undo / save / load / the Bones tab's numbers see it.

// The selected accessory, validated against the current scene; -1 when nothing (valid) is selected.
int AccessorySelected() {
    std::shared_ptr<Loaded> L = Current();
    const int i = g.selAccessory;
    if (!L || i < 0 || static_cast<size_t>(i) >= L->scene.parts.size()) return -1;
    if (L->scene.parts[static_cast<size_t>(i)].group != "Accessories") return -1;
    return i;
}

// The bones this part added that nothing else of the part parents: the roots of the part's bone set.
std::vector<int> AccessoryRootBones(const assemble::Scene& scene, const std::string& partId) {
    std::vector<int> out;
    for (size_t i = 0; i < scene.bones.size(); ++i) {
        if (scene.bones[i].source != partId) continue;
        const int p = scene.bones[i].parent;
        if (p < 0 || scene.bones[static_cast<size_t>(p)].source != partId) out.push_back(static_cast<int>(i));
    }
    return out;
}

// The gizmo anchor: the mean of the part's posed root bones (one for a typical prefab, so it is simply its origin).
bool AccessoryGizmoOrigin(const Loaded& L, Vec3& out) {
    const int pi = AccessorySelected();
    if (pi < 0) return false;
    const std::vector<int> roots = AccessoryRootBones(L.scene, L.scene.parts[static_cast<size_t>(pi)].id);
    Vec3 sum{};
    int n = 0;
    for (int i : roots) {
        const Vec3 p = static_cast<size_t>(i) < g.boneWorld.size() ? g.boneWorld[static_cast<size_t>(i)] : L.scene.bones[static_cast<size_t>(i)].pos;
        sum = sum + p;
        ++n;
    }
    if (!n) return false;
    out = sum * (1.0f / static_cast<float>(n));
    return true;
}

// Adds `delta` (model space) to every root bone of the selected accessory. MoveBone is called with the pose as it is, so
// the roots are independent of each other's edits (a root's parent is never inside the part).
bool MoveAccessoryRoots(const Vec3& delta) {
    std::shared_ptr<Loaded> L = Current();
    const int pi = AccessorySelected();
    if (!L || pi < 0) return false;
    const std::vector<int> roots = AccessoryRootBones(L->scene, L->scene.parts[static_cast<size_t>(pi)].id);
    if (roots.empty()) return false;
    const assemble::Pose pose = BuildPose(*L);
    for (int i : roots) {
        if (i < 0 || static_cast<size_t>(i) >= L->scene.bones.size()) continue;
        assemble::BonePose& e = g.boneEdits[L->scene.bones[static_cast<size_t>(i)].name];
        e.pos = assemble::MoveBone(L->scene, pose, static_cast<size_t>(i), delta);
    }
    return true;
}

// Applies the pointer movement of the current gizmo drag as a move along the grabbed axis (once per picture, like a bone drag).
void DragAccessoryGizmo(float dx, float dy) {
    if (g.gizmoAxis < 0 || g.gizmoAxis > 2) return;
    const float wpp = g.gizmoState.worldPerPixel[g.gizmoAxis];
    if (wpp <= 0.0f) return;
    const float d = (dx * g.gizmoState.dirX[g.gizmoAxis] + dy * g.gizmoState.dirY[g.gizmoAxis]) * wpp;
    if (d == 0.0f) return;
    const float dc = std::clamp(d, -0.5f, 0.5f);   // edge-on axis seen through a partly-projected frame: never a huge jump
    const Vec3 axis = g.gizmoAxis == 0 ? Vec3{1, 0, 0} : g.gizmoAxis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Vec3 delta = axis * dc;
    if (!MoveAccessoryRoots(delta)) return;
    g.gizmoAcc[0] += delta.x;
    g.gizmoAcc[1] += delta.y;
    g.gizmoAcc[2] += delta.z;
    ApplyPoseLive();
}

// Resets the move the gizmo (or the Bones tab on its root bones) put on the accessory.
void ResetAccessoryMove(int part) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || part < 0 || static_cast<size_t>(part) >= L->scene.parts.size()) return;
    int n = 0;
    for (int i : AccessoryRootBones(L->scene, L->scene.parts[static_cast<size_t>(part)].id))
        if (i >= 0 && static_cast<size_t>(i) < L->scene.bones.size() && g.boneEdits.erase(L->scene.bones[static_cast<size_t>(i)].name)) ++n;
    if (n) {
        ApplyPoseLive();
        Log("reset the position of " + L->scene.parts[static_cast<size_t>(part)].id);
    } else {
        Log(L->scene.parts[static_cast<size_t>(part)].id + " has no position edit");
    }
}

// Burns the gizmo into the finished picture (like the skeleton overlay). Called from Render after it has drawn the scene.
void DrawAccessoryGizmo(int w, int h) {
    g.gizmoState = exporter::MoveGizmoState{};
    std::shared_ptr<Loaded> L = Current();
    const int pi = AccessorySelected();
    if (!L || pi < 0 || !g.lastGpu) return;
    const size_t upi = static_cast<size_t>(pi);
    const bool gone = upi < g.effHidden.size() ? g.effHidden[upi] == 1 : (upi < g.hidden.size() && g.hidden[upi]);
    if (gone) return;   // hidden: nothing to grab
    Vec3 origin{};
    if (!AccessoryGizmoOrigin(*L, origin)) return;
    g.gizmoState = exporter::DrawMoveGizmo(*g.lastGpu, g.frame, w, h, origin, g.gizmoHoverAxis, g.gizmoAxis);
}

// The chip that selects an accessory for the gizmo: a small "move" cross on the row (accessory rows only).
constexpr int kAccChipW = 22, kAccChipH = 14;
RECT AccessoryChipRect(int rowTop) { return RECT{kPanel - 34, rowTop + 3, kPanel - 34 + kAccChipW, rowTop + 3 + kAccChipH}; }

void DrawAccessoryChip(ui::Surface& s, int y, bool selected, bool hot) {
    const RECT r = AccessoryChipRect(y);
    const COLORREF ink = selected ? RGB(12, 14, 24) : (hot ? ui::kAccent : ui::kSub);
    s.FillRound(r, 4, selected ? ui::kAccent : (hot ? ui::kCardHi : ui::kCard));
    s.StrokeRound(r, 4, selected ? ui::kAccent : (hot ? ui::kAccent : ui::kLine));
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f, a = 4.2f, head = 2.3f;
    s.Line(cx - a, cy, cx + a, cy, ink, 1.4f);
    s.Line(cx, cy - a, cx, cy + a, ink, 1.4f);
    s.FillTriangle(cx + a + head, cy, cx + a - 0.7f, cy - 2.3f, cx + a - 0.7f, cy + 2.3f, ink);
    s.FillTriangle(cx - a - head, cy, cx - a + 0.7f, cy - 2.3f, cx - a + 0.7f, cy + 2.3f, ink);
    s.FillTriangle(cx, cy + a + head, cx - 2.3f, cy + a - 0.7f, cx + 2.3f, cy + a - 0.7f, ink);
    s.FillTriangle(cx, cy - a - head, cx - 2.3f, cy - a + 0.7f, cx + 2.3f, cy - a + 0.7f, ink);
}
