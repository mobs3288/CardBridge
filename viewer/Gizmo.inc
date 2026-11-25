// viewer fragment: the part move/rotate gizmo (included in order by main.cpp, after Parts.inc)
// Every part row offers two chips: a move cross (arrows in the view) and a rotate ring (rings in the view). Only the picked
// gizmo is drawn; both leave their edits on the same part. The edit is a rigid transform of that part's own vertices in bind
// space, applied before the skin matrices in the preview (GpuView / RenderView) and baked into the scene copy at export.
// That way a garment skinned to the body's bones can be moved alone, yet stays glued to the character through any bone pose,
// and undo / save / load / the PMX all see it. The pivot is the part's bind-space centre: rotations turn around it.

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }   // the core header has no Vec3 minus (as in measure/BodyMeasure.cpp)

// The selected part, validated against the current scene; -1 when nothing (valid) is selected.
int GizmoPartSelected() {
    std::shared_ptr<Loaded> L = Current();
    const int i = g.selGizmoPart;
    if (!L || i < 0 || static_cast<size_t>(i) >= L->scene.parts.size()) return -1;
    if (L->scene.parts[static_cast<size_t>(i)].meshes.empty()) return -1;
    return i;
}

// The per-part bind-space transforms the renderers and the export take. Indexed by scene part; identity for untouched parts.
std::vector<Mat4> PartTransforms(const assemble::Scene& scene) {
    std::vector<Mat4> out(scene.parts.size());
    for (size_t i = 0; i < scene.parts.size(); ++i) {
        const auto it = g.partEdits.find(scene.parts[i].id);
        if (it != g.partEdits.end()) out[i] = assemble::PartTransform(it->second);
    }
    return out;
}

// The part's centre in bind space: the mean of its mesh vertices. Stable while the character is posed, so it is a good pivot.
Vec3 PartBindCenter(const assemble::Scene& scene, int part) {
    Vec3 sum{};
    size_t n = 0;
    if (part < 0 || static_cast<size_t>(part) >= scene.parts.size()) return sum;
    for (int mi : scene.parts[static_cast<size_t>(part)].meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
            sum = sum + v.pos;
            ++n;
        }
    }
    return n ? sum * (1.0f / static_cast<float>(n)) : sum;
}

// The blended bone matrix at the part's centre (from the vertex nearest it), used to turn a world-space gizmo drag into a
// bind-space edit. With no pose the bind space and the model space are the same, so the matrix stays the identity.
Mat4 PartBindSkinAt(const Loaded& L, int part, Vec3 center) {
    if (g.skin.empty()) return Mat4{};
    const assemble::Scene& scene = L.scene;
    if (part < 0 || static_cast<size_t>(part) >= scene.parts.size()) return Mat4{};
    const assemble::SceneVertex* best = nullptr;
    float bestD = 1e30f;
    for (int mi : scene.parts[static_cast<size_t>(part)].meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
        for (const auto& v : scene.meshes[static_cast<size_t>(mi)].vertices) {
            const Vec3 d = v.pos - center;
            const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
            if (dd < bestD) { bestD = dd; best = &v; }
        }
    }
    if (!best) return Mat4{};
    Mat4 m;
    for (float& x : m.m) x = 0.0f;
    float sum = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const float w = best->weight[k];
        const int b = best->bone[k];
        if (!(w > 0.0f) || b < 0 || static_cast<size_t>(b) >= g.skin.size()) continue;
        for (int e = 0; e < 16; ++e) m.m[e] += g.skin[static_cast<size_t>(b)].m[e] * w;
        sum += w;
    }
    if (!(sum > 1e-6f)) return Mat4{};
    for (float& x : m.m) x /= sum;
    return m;
}

// The entry being edited, created on first use with the part's centre as its pivot.
assemble::PartPose& PartEditFor(int part) {
    std::shared_ptr<Loaded> L = Current();
    const std::string& id = L->scene.parts[static_cast<size_t>(part)].id;
    auto [it, fresh] = g.partEdits.try_emplace(id);
    if (fresh) it->second.pivot = PartBindCenter(L->scene, part);
    return it->second;
}

// Where the gizmo sits now: the mean of the part's vertices after its own edit and the current bone pose.
bool PartGizmoOrigin(const Loaded& L, Vec3& out) {
    const int pi = GizmoPartSelected();
    if (pi < 0) return false;
    const auto it = g.partEdits.find(L.scene.parts[static_cast<size_t>(pi)].id);
    const Mat4 px = it == g.partEdits.end() ? Mat4{} : assemble::PartTransform(it->second);
    const bool posed = !g.skin.empty();
    Vec3 sum{};
    size_t n = 0;
    for (int mi : L.scene.parts[static_cast<size_t>(pi)].meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= L.scene.meshes.size()) continue;
        for (const auto& src : L.scene.meshes[static_cast<size_t>(mi)].vertices) {
            assemble::SceneVertex v = src;
            v.pos = px.point(v.pos);
            sum = sum + (posed ? assemble::SkinPoint(v, g.skin) : v.pos);
            ++n;
        }
    }
    if (!n) return false;
    out = sum * (1.0f / static_cast<float>(n));
    return true;
}

// Adds `worldDelta` to the part's bind-space offset. Converted through the skin at the pivot so the drag follows the hand
// even when the character is posed; the edit itself rides with the bones.
bool MovePartGizmo(const Vec3& worldDelta) {
    std::shared_ptr<Loaded> L = Current();
    const int pi = GizmoPartSelected();
    if (!L || pi < 0) return false;
    assemble::PartPose& e = PartEditFor(pi);
    const Mat4 skin = PartBindSkinAt(*L, pi, e.pivot);
    const Vec3 delta = inverse(skin).dir(worldDelta);
    e.pos = e.pos + delta;
    return true;
}

// Turns the part's edit about its pivot. `worldTurn` is converted into bind space the same way the move is, so the ring
// tracks the pointer on a posed character and the turned part still follows the skeleton afterwards.
bool RotatePartGizmo(int axis, float radians) {
    std::shared_ptr<Loaded> L = Current();
    const int pi = GizmoPartSelected();
    if (!L || pi < 0 || axis < 0 || axis > 2 || radians == 0.0f) return false;
    assemble::PartPose& e = PartEditFor(pi);
    const Vec3 axisVec = axis == 0 ? Vec3{1, 0, 0} : axis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Mat4 worldTurn = assemble::RotationAxisAngle(axisVec, radians);
    const Mat4 skin = PartBindSkinAt(*L, pi, e.pivot);
    const Mat4 bindTurn = inverse(skin) * worldTurn * skin;
    e.rot = bindTurn * e.rot;
    e.pos = bindTurn.dir(e.pos);
    return true;
}

// Applies the pointer movement of the current gizmo drag as a move along the grabbed axis (once per picture, like a bone drag).
void DragPartGizmo(float dx, float dy) {
    if (g.gizmoAxis < 0 || g.gizmoAxis > 2) return;
    const float wpp = g.gizmoState.worldPerPixel[g.gizmoAxis];
    if (wpp <= 0.0f) return;
    const float d = (dx * g.gizmoState.dirX[g.gizmoAxis] + dy * g.gizmoState.dirY[g.gizmoAxis]) * wpp;
    if (d == 0.0f) return;
    const float dc = std::clamp(d, -0.5f, 0.5f);   // edge-on axis seen through a partly-projected frame: never a huge jump
    const Vec3 axis = g.gizmoAxis == 0 ? Vec3{1, 0, 0} : g.gizmoAxis == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Vec3 delta = axis * dc;
    if (!MovePartGizmo(delta)) return;
    g.gizmoAcc[0] += delta.x;
    g.gizmoAcc[1] += delta.y;
    g.gizmoAcc[2] += delta.z;
    ApplyPoseLive();
}

// Applies the pointer movement of the current ring drag as a turn about the grabbed axis (once per picture, like a bone drag).
// The pointer's absolute position is stored by the mouse-move handler; the angle comes from the projection inverse, so the
// turn tracks the hand exactly, whatever the ring's foreshortening.
void DragPartRotateGizmo() {
    if (g.rotGizmoAxis < 0 || g.rotGizmoAxis > 2) return;
    float theta = 0.0f;
    if (!exporter::RotateGizmoPointerAngle(g.rotGizmoState, g.rotGizmoAxis, g.rotGizmoPtrX, g.rotGizmoPtrY, theta)) return;   // too near the centre, or edge-on: hold the edit
    float d = theta - g.rotGizmoTheta;   // the wrapped signed turn since the last applied step
    if (d > 3.14159265f) d -= 6.2831853f;
    else if (d < -3.14159265f) d += 6.2831853f;
    g.rotGizmoTheta = theta;
    if (d == 0.0f) return;
    d = std::clamp(d, -0.35f, 0.35f);   // never a huge jump
    if (!RotatePartGizmo(g.rotGizmoAxis, d)) return;
    g.rotGizmoAcc[g.rotGizmoAxis] += d * 57.29577951f;   // degrees
    ApplyPoseLive();
}

// Resets the move/rotation the gizmo put on the part. One edit holds both, so reset removes only the requested half:
// the rotation is written back as the identity while the offset stays, and vice versa.
void ResetPartMove(int part) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || part < 0 || static_cast<size_t>(part) >= L->scene.parts.size()) return;
    const auto it = g.partEdits.find(L->scene.parts[static_cast<size_t>(part)].id);
    if (it == g.partEdits.end() || (it->second.pos.x == 0 && it->second.pos.y == 0 && it->second.pos.z == 0)) {
        Log(L->scene.parts[static_cast<size_t>(part)].id + " has no position edit");
        return;
    }
    it->second.pos = {0, 0, 0};
    if (assemble::IsIdentity(it->second)) g.partEdits.erase(it);
    ApplyPoseLive();
    Log("reset the position of " + L->scene.parts[static_cast<size_t>(part)].id);
}

void ResetPartRot(int part) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || part < 0 || static_cast<size_t>(part) >= L->scene.parts.size()) return;
    const auto it = g.partEdits.find(L->scene.parts[static_cast<size_t>(part)].id);
    const Mat4 id{};
    bool rotated = it != g.partEdits.end();
    if (rotated) for (int i = 0; i < 16; ++i) if (it->second.rot.m[i] != id.m[i]) { rotated = false; break; }
    if (!rotated) {
        Log(L->scene.parts[static_cast<size_t>(part)].id + " has no rotation edit");
        return;
    }
    it->second.rot = Mat4{};
    if (assemble::IsIdentity(it->second)) g.partEdits.erase(it);
    ApplyPoseLive();
    Log("reset the rotation of " + L->scene.parts[static_cast<size_t>(part)].id);
}

// Burns the gizmo into the finished picture (like the skeleton overlay). Called from Render after it has drawn the scene.
void DrawPartGizmo(int w, int h) {
    g.gizmoState = exporter::MoveGizmoState{};
    g.rotGizmoState = exporter::RotateGizmoState{};
    std::shared_ptr<Loaded> L = Current();
    const int pi = GizmoPartSelected();
    if (!L || pi < 0 || !g.lastGpu) return;
    const size_t upi = static_cast<size_t>(pi);
    const bool gone = upi < g.effHidden.size() ? g.effHidden[upi] == 1 : (upi < g.hidden.size() && g.hidden[upi]);
    if (gone) return;   // hidden: nothing to grab
    Vec3 origin{};
    if (!PartGizmoOrigin(*L, origin)) return;
    if (g.gizmoMode == 0)
        g.gizmoState = exporter::DrawMoveGizmo(*g.lastGpu, g.frame, w, h, origin, g.gizmoHoverAxis, g.gizmoAxis);
    else
        g.rotGizmoState = exporter::DrawRotateGizmo(*g.lastGpu, g.frame, w, h, origin, g.rotGizmoHoverAxis, g.rotGizmoAxis);
}

// True when the gizmo the current mode picks was drawn in the last picture (and could be picked).
bool PartGizmoDrawn() { return g.gizmoMode == 0 ? g.gizmoState.ox >= 0.0f : g.rotGizmoState.ox >= 0.0f; }

// The axis (arrow or ring, whichever the mode picks) nearest to a point within `radius` pixels; -1 for none.
int PickPartGizmoAxis(float x, float y, float radius) {
    if (g.gizmoMode == 0) return exporter::PickMoveGizmoAxis(g.gizmoState, x, y, radius);
    return exporter::PickRotateGizmoAxis(g.rotGizmoState, x, y, radius);
}

// The chips that select a part for a gizmo: a "move" cross and a "rotate" arc on the row.
constexpr int kAccChipW = 22, kAccChipH = 14;
RECT GizmoChipRect(int rowTop) { return RECT{kPanel - 34, rowTop + 3, kPanel - 34 + kAccChipW, rowTop + 3 + kAccChipH}; }
RECT GizmoRotateChipRect(int rowTop) { return RECT{kPanel - 62, rowTop + 3, kPanel - 62 + kAccChipW, rowTop + 3 + kAccChipH}; }

void DrawGizmoChip(ui::Surface& s, int y, bool selected, bool hot) {
    const RECT r = GizmoChipRect(y);
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

void DrawGizmoRotateChip(ui::Surface& s, int y, bool selected, bool hot) {
    const RECT r = GizmoRotateChipRect(y);
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
