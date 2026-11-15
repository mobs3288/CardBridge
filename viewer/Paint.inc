// viewer fragment: the translucent-clothes brush (included in order by main.cpp, after Gizmo.inc)
//
// A hand-painted per-texel mask of see-through area: left-drag on a garment paints, right-drag (or the Erase switch)
// erases — the same modal the pencil modes of image editors use. The stamp is ray-cast onto the posed mesh (like the
// picture), into the garment's own texture in UV space, gated by the mesh's UV islands. The bake writes the mask into
// the texture's alpha channel and moves the material to the transparent pass, so the skin (or a bra) shows through — in
// the Direct3D view, in the software view, and in the exported PMX (MikuMikuDance draws a transparent material there; the
// skin under a painted garment is kept visible for exactly this reason, see EffectiveHiddenParts).
//
// The mask is session-only (never saved, like the Morphs tab's live weights): reloading the card drops it. Undo (Ctrl+Z)
// does not reach the brush; the switch below the slider restores the texture, and "Clear the brush" wipes the mask.
// Right-clicking the switch row puts it back to off, like every switch.

// ---- the bake -----------------------------------------------------------------------------------------------------
// Pushes the bake's changed textures through the GPU upload and the software renderer's mip cache, and drops the baked
// "as exported" copy when the bake actually changed pixels (it holds the old paint).
void TransInvalidate(const std::vector<int>& changed) {
    if (changed.empty()) return;
    std::shared_ptr<Loaded> L = Current();
    for (int ti : changed) if (g.state) exporter::InvalidateViewTexture(*g.state, ti);
    if (L && g.gpuOk && g.gpuScene == L) g.gpu.UpdateTextures(changed);
    // The baked "as exported" copy holds the old paint, like it holds the old skin shading: drop it.
    g.expOk = false;
    g.showExp = false;
    g.expFor.reset();
    g.expScene.reset();
    g.gpuExp.Unload();
    g.dirty = true;
    InvalidateView();
    InvalidatePanel();
}

// Re-applies the mask-derived alpha at the slider's strength, then pushes the changed textures.
void TransApplyNow() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    std::vector<int> changed;
    exporter::TranslucencyApply(L->scene, g.tw.transStrength, &changed);
    if (changed.empty()) return;   // nothing changed: the view and the export keep what they have
    TransInvalidate(changed);
}

// The toggle (or the erase switch) changed: apply or revert the bake at once, and say what it means.
void TransSwitchChanged(int id) {
    if (id == kTransBrush) {
        Log(g.transBrush
                ? "translucency brush on: left-drag on a garment paints the see-through area; right-drag (or the Erase switch) erases; the slider sets how see-through it is"
                : "translucency brush off: the see-through effect is off (the paint is kept)");
    } else if (id == kTransErase) {
        Log(std::string("erase mode: ") + (g.transErase ? "on (the brush removes paint)" : "off (the brush lays paint)"));
    }
    TransApplyNow();
    InvalidatePanel();
}

// "Clear the brush from every garment" (the Look tab's button).
void TransClearPaint() {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("no character loaded", LogKind::Warn); return; }
    std::vector<int> changed;
    const int n = exporter::TranslucencyClearAll(L->scene, &changed);
    TransInvalidate(changed);   // the textures are back to the card's pixels: re-upload them
    InvalidatePanel();
    Log(n > 0 ? "cleared the translucency paint from " + std::to_string(n) + " garment(s)" : "no translucency paint to clear");
}

// ---- the stroke ----------------------------------------------------------------------------------------------------
// The framing the picking and the ring use: the same formula as the renderers (orthographic, the whole visible model fits).
bool TransView(exporter::ViewFraming& f, float& W, float& H) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return false;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const float w = static_cast<float>(std::max(0, static_cast<int>(rc.right) - kPanel));
    const float h = static_cast<float>(rc.bottom);
    if (w < 8.0f || h < 8.0f) return false;
    if (!exporter::detail::ComputeViewFraming(L->scene, &g.effHidden, w, h, g.cam, f)) return false;
    W = w;
    H = h;
    return true;
}

// Stamps between the last stamped UV and the one under (vx, vy). Every texture the part uses - front and back shells
// of the same garment share one atlas - gets the stamp, so the see-through does not stop at the front layer.
// Interpolates in *wrapped* UV space so a stroke crossing a texture seam continues on the other side instead of
// smearing across the whole atlas.
void TransStrokeTo(int vx, int vy) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    static const bool dbg = std::getenv("CARDBRIDGE_TEST_DEBUG_TRANSLUCENCY") != nullptr;
    exporter::ViewFraming f;
    float W = 0, H = 0;
    if (!TransView(f, W, H)) {
        if (dbg) std::fprintf(stderr, "stroke (%d,%d): no framing\n", vx, vy);
        return;
    }
    exporter::BrushHit hit;
    const std::vector<Mat4>* sk = g.skin.empty() ? nullptr : &g.skin;
    const auto* md = g.morphDelta.empty() ? nullptr : &g.morphDelta;
    if (!exporter::PickBrushPart(L->scene, &g.effHidden, f, W, H, g.cam, static_cast<float>(vx), static_cast<float>(vy), sk, md, hit)) {
        if (dbg) std::fprintf(stderr, "stroke (%d,%d): nothing hit (hidden=%zu, framing cx=%.2f cy=%.2f extent=%.2f scale=%.1f zoom=%.2f)\n",
                              vx, vy, g.effHidden.size(), f.cx, f.cy, f.extent, f.scale, g.cam.zoom);
        g.paintAnchorSet = false;   // out in empty space: the next hit starts a fresh stretch of the stroke
        return;
    }
    if (dbg) std::fprintf(stderr, "stroke (%d,%d): part=%d mesh=%d submesh=%d uv=(%.3f,%.3f) uvPerM=%.3f\n",
                          vx, vy, hit.part, hit.mesh, hit.submesh, hit.uv.x, hit.uv.y, hit.uvPerMetre);
    // The distinct textures every textured submesh of this part binds - shells of the same garment share the atlas.
    std::vector<int> texs;
    int hitMaterial = -1;
    for (int mi : L->scene.parts[static_cast<size_t>(hit.part)].meshes) {
        if (mi < 0 || static_cast<size_t>(mi) >= L->scene.meshes.size()) continue;
        const auto& mesh = L->scene.meshes[static_cast<size_t>(mi)];
        for (size_t smi = 0; smi < mesh.submeshes.size(); ++smi) {
            const auto& sub = mesh.submeshes[smi];
            if (sub.material < 0 || static_cast<size_t>(sub.material) >= L->scene.materials.size()) continue;
            const int t = L->scene.materials[static_cast<size_t>(sub.material)].texture;
            if (mi == hit.mesh && static_cast<int>(smi) == hit.submesh) hitMaterial = sub.material;
            if (t < 0 || static_cast<size_t>(t) >= L->scene.textures.size()) continue;
            if (std::find(texs.begin(), texs.end(), t) == texs.end()) texs.push_back(t);
        }
    }
    if (texs.empty()) { g.paintAnchorSet = false; return; }
    const float rUv = std::clamp((g.tw.transSizeCm / 100.0f) * hit.uvPerMetre, 0.002f, 0.75f);
    const bool erase = g.transEraseStroke;
    const float amount = erase ? 0.0f : 170.0f;
    // The stroke path: the interpolated chord from the last position to the new one (a single point for a fresh stroke).
    std::vector<std::pair<float, float>> path;
    if (g.paintAnchorSet && g.paintPart == hit.part) {
        float du = hit.uv.x - g.paintAnchorU, dv = hit.uv.y - g.paintAnchorV;
        if (!L->scene.textures[static_cast<size_t>(texs.front())].clamp) { du -= std::floor(du + 0.5f); dv -= std::floor(dv + 0.5f); }
        const float dist = std::sqrt(du * du + dv * dv);
        const float step = std::max(rUv * 0.4f, 1e-4f);
        const int steps = std::min(96, static_cast<int>(dist / step));
        if (steps > 0) {
            for (int i = 1; i <= steps; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                path.push_back({g.paintAnchorU + du * t, g.paintAnchorV + dv * t});
            }
        }
    } else {
        path.push_back({hit.uv.x, hit.uv.y});
        g.paintAnchorSet = true;
    }
    int changed = 0;
    for (int ti : texs) {
        int matOf = -1;   // a material binding this texture (for the layer's material field)
        for (int mi : L->scene.parts[static_cast<size_t>(hit.part)].meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= L->scene.meshes.size()) continue;
            for (const auto& sub : L->scene.meshes[static_cast<size_t>(mi)].submeshes)
                if (sub.material >= 0 && static_cast<size_t>(sub.material) < L->scene.materials.size() && L->scene.materials[static_cast<size_t>(sub.material)].texture == ti) { matOf = sub.material; break; }
            if (matOf >= 0) break;
        }
        if (matOf < 0) matOf = hitMaterial;
        const int layer = exporter::TranslucencyLayer(L->scene, hit.part, matOf, hit.mesh);
        if (layer < 0) continue;
        for (const auto& [u, v] : path)
            changed += exporter::TranslucencyStamp(L->scene, layer, u, v, rUv, erase, amount);
    }
    g.paintAnchorU = hit.uv.x;
    g.paintAnchorV = hit.uv.y;
    g.paintPart = hit.part;
    if (dbg) std::fprintf(stderr, "stroke: textures=%zu changed=%d texels\n", texs.size(), changed);
    if (changed > 0) TransApplyNow();
}

// The press that starts a stroke (left button, or right button for an erase). Captures the pointer; g.drag == 6 while painting.
void TransPaintPress(int vx, int vy, bool erase) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    g.paintPart = -1;
    g.paintAnchorSet = false;
    g.transEraseStroke = erase || g.transErase;
    g.transPaintX = vx;
    g.transPaintY = vy;
    g.transPaintPending = false;
    g.drag = 6;
    g.interacting = true;
    g.lastInput = GetTickCount64();
    SetCapture(g.hwnd);
    TransStrokeTo(vx, vy);   // the first stamp; the release ends the stroke
    InvalidateView();
}

void TransEndStroke() {
    g.paintPart = -1;
    g.paintAnchorSet = false;
    g.transPaintPending = false;
    TransApplyNow();   // final pixels; a no-op when nothing changed (the bake counts real pixel changes)
}

// ---- the cursor ring ------------------------------------------------------------------------------------------------
namespace {
void transRing(std::vector<uint32_t>& px, int W, int H, float cx, float cy, float r, float thickness, uint32_t c) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - thickness))), x1 = std::min(W - 1, static_cast<int>(std::ceil(cx + r + thickness)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - thickness))), y1 = std::min(H - 1, static_cast<int>(std::ceil(cy + r + thickness)));
    const float inner = (r - thickness * 0.5f) * (r - thickness * 0.5f), outer = (r + thickness * 0.5f) * (r + thickness * 0.5f);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
            if (d2 >= inner && d2 <= outer) px[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] = c;
        }
}
} // namespace

// The brush ring, drawn straight into the finished frame (like the skeleton and the gizmo). Shown while the mode is on and the pointer is over the view.
void DrawTransCursor(int w, int h) {
    if (!g.transBrush || g.transHoverX < 0 || g.transHoverY < 0) return;
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.frame.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return;
    exporter::ViewFraming f;
    if (!exporter::detail::ComputeViewFraming(L->scene, &g.effHidden, static_cast<float>(w), static_cast<float>(h), g.cam, f)) return;
    const float rPx = std::clamp((g.tw.transSizeCm / 100.0f) * f.scale, 3.0f, 400.0f);
    const float cx = static_cast<float>(g.transHoverX), cy = static_cast<float>(g.transHoverY);
    const bool erasing = g.transErase || g.transEraseStroke;
    transRing(g.frame, w, h, cx, cy, rPx, 3.2f, 0xFF16161E);
    transRing(g.frame, w, h, cx, cy, rPx, 1.5f, erasing ? 0xFF6FD3E7u : 0xFFFFB454u);
    // the centre dot: the exact stamp point
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx * dx + dy * dy <= 2) {
                const int x = static_cast<int>(std::lround(cx)) + dx, y = static_cast<int>(std::lround(cy)) + dy;
                if (x >= 0 && y >= 0 && x < w && y < h) g.frame[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = 0xFF16161E;
            }
}
