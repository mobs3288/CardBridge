// viewer fragment: the accessory move/rotate gizmo (included in order by main.cpp, after Parts.inc)
// An accessory hangs on a body attach node, but everything the item itself added to the skeleton carries its part id as
// the bone's `source` (the item root bone first, then the N_move chain). The gizmo edits those root bones through the
// ordinary pose (assemble::MoveBone / assemble::TurnBone), so the meshes follow in the preview and in an export exactly
// like any pose edit, and undo / save / load / the Bones tab's numbers see it.
// The row offers two chips: a move cross (arrows in the view) and a rotate ring (rings in the view). Only the picked
// gizmo is drawn; both leave their edits on the same root bones.

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }   // the core header has no Vec3 minus (as in measure/BodyMeasure.cpp)

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

// Turns every root bone of the selected accessory by `radians` about `axis` (0 X, 1 Y, 2 Z) around the gizmo anchor.
// TurnBone turns the orientation of each root (as many edits are independent, the pose is read once); the orbit is a
// rigid turn about the anchor, so the whole assembly keeps together. A single root sitting on the anchor turns in place.
bool RotateAccessoryRoots(int axis, float radians) {
    std::shared_ptr<Loaded> L = Current();
    const int pi = AccessorySelected();
    if (!L || pi < 0 || axis < 0 || axis > 2 || radians == 0.0f) return false;
    const std::vector<int> roots = AccessoryRootBones(L->scene, L->scene.parts[static_cast<size_t>(pi)].id);
    if (roots.empty()) return false;
    Vec3 anchor{};
    if (!AccessoryGizmoOrigin(*L, anchor)) return false;
    const Vec3 axisVec = axis == 0 ? Vec3{1, 0, 0} : axis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Mat4 turn = assemble::RotationAxisAngle(axisVec, radians);
    const assemble::Pose pose = BuildPose(*L);
    for (int i : roots) {
        if (i < 0 || static_cast<size_t>(i) >= L->scene.bones.size()) continue;
        const size_t si = static_cast<size_t>(i);
        const Vec3 pw = si < g.boneWorld.size() ? g.boneWorld[si] : L->scene.bones[si].pos;
        const Vec3 r = pw - anchor;
        assemble::BonePose& e = g.boneEdits[L->scene.bones[si].name];
        e.rot = assemble::TurnBone(L->scene, pose, si, turn);
        e.pos = assemble::MoveBone(L->scene, pose, si, turn.dir(r) - r);   // the orbit about the anchor
    }
    return true;
}

// Applies the pointer movement of the current ring drag as a turn about the grabbed axis (once per picture, like a bone drag).
// The pointer's absolute position is stored by the mouse-move handler; the angle comes from the projection inverse, so the
// turn tracks the hand exactly, whatever the ring's foreshortening.
void DragAccessoryRotateGizmo() {
    if (g.rotGizmoAxis < 0 || g.rotGizmoAxis > 2) return;
    float theta = 0.0f;
    if (!exporter::RotateGizmoPointerAngle(g.rotGizmoState, g.rotGizmoAxis, g.rotGizmoPtrX, g.rotGizmoPtrY, theta)) return;   // too near the centre, or edge-on: hold the edit
    float d = theta - g.rotGizmoTheta;   // the wrapped signed turn since the last applied step
    if (d > 3.14159265f) d -= 6.2831853f;
    else if (d < -3.14159265f) d += 6.2831853f;
    g.rotGizmoTheta = theta;
    if (d == 0.0f) return;
    d = std::clamp(d, -0.35f, 0.35f);   // never a huge jump
    if (!RotateAccessoryRoots(g.rotGizmoAxis, d)) return;
    g.rotGizmoAcc[g.rotGizmoAxis] += d * 57.29577951f;   // degrees
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

// Resets the rotation the gizmo (or the Bones tab on its root bones) put on the accessory; the move is kept.
void ResetAccessoryRot(int part) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || part < 0 || static_cast<size_t>(part) >= L->scene.parts.size()) return;
    int n = 0;
    for (int i : AccessoryRootBones(L->scene, L->scene.parts[static_cast<size_t>(part)].id)) {
        if (i < 0 || static_cast<size_t>(i) >= L->scene.bones.size()) continue;
        const auto it = g.boneEdits.find(L->scene.bones[static_cast<size_t>(i)].name);
        if (it == g.boneEdits.end()) continue;
        it->second.rot = {0, 0, 0};
        if (assemble::IsIdentity(it->second)) g.boneEdits.erase(it);   // the move stays; a fully-identity edit goes
        ++n;
    }
    if (n) {
        ApplyPoseLive();
        Log("reset the rotation of " + L->scene.parts[static_cast<size_t>(part)].id);
    } else {
        Log(L->scene.parts[static_cast<size_t>(part)].id + " has no rotation edit");
    }
}

// Burns the gizmo into the finished picture (like the skeleton overlay). Called from Render after it has drawn the scene.
void DrawAccessoryGizmo(int w, int h) {
    g.gizmoState = exporter::MoveGizmoState{};
    g.rotGizmoState = exporter::RotateGizmoState{};
    std::shared_ptr<Loaded> L = Current();
    const int pi = AccessorySelected();
    if (!L || pi < 0 || !g.lastGpu) return;
    const size_t upi = static_cast<size_t>(pi);
    const bool gone = upi < g.effHidden.size() ? g.effHidden[upi] == 1 : (upi < g.hidden.size() && g.hidden[upi]);
    if (gone) return;   // hidden: nothing to grab
    Vec3 origin{};
    if (!AccessoryGizmoOrigin(*L, origin)) return;
    if (g.gizmoMode == 0)
        g.gizmoState = exporter::DrawMoveGizmo(*g.lastGpu, g.frame, w, h, origin, g.gizmoHoverAxis, g.gizmoAxis);
    else
        g.rotGizmoState = exporter::DrawRotateGizmo(*g.lastGpu, g.frame, w, h, origin, g.rotGizmoHoverAxis, g.rotGizmoAxis);
}

// True when the gizmo the current mode picks was drawn in the last picture (and could be picked).
bool AccessoryGizmoDrawn() { return g.gizmoMode == 0 ? g.gizmoState.ox >= 0.0f : g.rotGizmoState.ox >= 0.0f; }

// The axis (arrow or ring, whichever the mode picks) nearest to a point within `radius` pixels; -1 for none.
int PickAccessoryGizmoAxis(float x, float y, float radius) {
    if (g.gizmoMode == 0) return exporter::PickMoveGizmoAxis(g.gizmoState, x, y, radius);
    return exporter::PickRotateGizmoAxis(g.rotGizmoState, x, y, radius);
}

// The chips that select an accessory for a gizmo: a "move" cross and a "rotate" arc on the row (accessory rows only).
constexpr int kAccChipW = 22, kAccChipH = 14;
RECT AccessoryChipRect(int rowTop) { return RECT{kPanel - 34, rowTop + 3, kPanel - 34 + kAccChipW, rowTop + 3 + kAccChipH}; }
RECT AccessoryRotateChipRect(int rowTop) { return RECT{kPanel - 62, rowTop + 3, kPanel - 62 + kAccChipW, rowTop + 3 + kAccChipH}; }

void DrawAccessoryChip(ui::Surface& s, int y, bool selected, bool hot) {
    const RECT r = AccessoryChipRect(y);
    const COLORREF ink = selected ? ui::kOnAccent : (hot ? ui::kAccentHi : ui::kSub);
    s.FillRound(r, ui::kRad, selected ? ui::kAccent : (hot ? ui::kSelect : ui::kCard));
    s.StrokeRound(r, ui::kRad, selected ? ui::kAccent : (hot ? ui::kAccent : ui::kLine));
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f, a = 4.2f, head = 2.3f;
    s.Line(cx - a, cy, cx + a, cy, ink, 1.4f);
    s.Line(cx, cy - a, cx, cy + a, ink, 1.4f);
    s.FillTriangle(cx + a + head, cy, cx + a - 0.7f, cy - 2.3f, cx + a - 0.7f, cy + 2.3f, ink);
    s.FillTriangle(cx - a - head, cy, cx - a + 0.7f, cy - 2.3f, cx - a + 0.7f, cy + 2.3f, ink);
    s.FillTriangle(cx, cy + a + head, cx - 2.3f, cy + a - 0.7f, cx + 2.3f, cy + a - 0.7f, ink);
    s.FillTriangle(cx, cy - a - head, cx - 2.3f, cy - a + 0.7f, cx + 2.3f, cy - a + 0.7f, ink);
}

void DrawAccessoryRotateChip(ui::Surface& s, int y, bool selected, bool hot) {
    const RECT r = AccessoryRotateChipRect(y);
    const COLORREF ink = selected ? ui::kOnAccent : (hot ? ui::kAccentHi : ui::kSub);
    s.FillRound(r, ui::kRad, selected ? ui::kAccent : (hot ? ui::kSelect : ui::kCard));
    s.StrokeRound(r, ui::kRad, selected ? ui::kAccent : (hot ? ui::kAccent : ui::kLine));
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f, rad = 4.0f;
    const float a0 = -0.55f, sweep = 4.4f;   // an almost full circle, open at the upper right where the arrowhead sits
    constexpr int kSeg = 12;
    float px = cx + rad * std::cos(a0), py = cy + rad * std::sin(a0);
    for (int i = 1; i <= kSeg; ++i) {
        const float a = a0 + sweep * static_cast<float>(i) / static_cast<float>(kSeg);
        const float nx = cx + rad * std::cos(a), ny = cy + rad * std::sin(a);
        s.Line(px, py, nx, ny, ink, 1.4f);
        px = nx; py = ny;
    }
    const float ea = a0 + sweep;   // the arrowhead at the end of the arc, pointing along it
    const float ex = cx + rad * std::cos(ea), ey = cy + rad * std::sin(ea);
    const float tx = -std::sin(ea), ty = std::cos(ea);   // the tangent (increasing angle)
    const float bx = std::cos(ea), by = std::sin(ea);    // the outward radius
    s.FillTriangle(ex + tx * 2.8f, ey + ty * 2.8f, ex + bx * 2.1f, ey + by * 2.1f, ex - bx * 2.1f, ey - by * 2.1f, ink);
}
