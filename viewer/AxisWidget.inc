// viewer fragment: the orientation widget (included in order by main.cpp, after ModeStrip.inc)
// The Blender-style navigation widget, ported from the Project K editor's viewport gizmo (ViewportPanes.cpp): six balls on a
// circle — the model's +X/+Y/+Z ends in solid colour with a dark letter, the -X/-Y/-Z ends dim with an axis-coloured rim, lines
// from the centre to the positive balls, far balls under near ones. Click a ball to look at the model from that axis (the view
// stays zoom and pan), drag anywhere in it to orbit, hover shows a halo and a white ring. It is drawn straight into the frame
// bitmap (like the accessory gizmo and the skeleton), so it is always pixel-consistent with the picture — no window-DC clip or
// GDI+ alpha quirks can half-draw it. Anti-aliased ring and ball fills are faked with two nested discs.

namespace {

constexpr uint32_t kNavAxisA[3] = {0xFFE64B55, 0xFF78C83C, 0xFF468CF0};   // X red, Y green, Z blue (0xAARRGGBB)
constexpr uint32_t kNavAxisDim[3] = {0xFF783232, 0xFF46692D, 0xFF2D4E80};
constexpr uint32_t kNavShadow = 0xFF0D0E13, kNavWhite = 0xFFFFFFFF, kNavRingWhite = 0xFFF0F4FA;
const Vec3 kNavSideAxis[6] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};   // +X +Y +Z -X -Y -Z
const char kNavLetter[6] = {'X', 'Y', 'Z', 'X', 'Y', 'Z'};

void navPut(std::vector<uint32_t>& px, int w, int h, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
}

void navDisc(std::vector<uint32_t>& px, int w, int h, float cx, float cy, float r, uint32_t c) {
    const int x0 = static_cast<int>(std::floor(cx - r)), x1 = static_cast<int>(std::ceil(cx + r));
    const int y0 = static_cast<int>(std::floor(cy - r)), y1 = static_cast<int>(std::ceil(cy + r));
    const float r2 = r * r;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r2) navPut(px, w, h, x, y, c);
        }
}

// A ring: fills the band |distance - radius| <= thickness / 2.
void navRing(std::vector<uint32_t>& px, int w, int h, float cx, float cy, float r, float thickness, uint32_t c) {
    const int x0 = static_cast<int>(std::floor(cx - r - thickness)), x1 = static_cast<int>(std::ceil(cx + r + thickness));
    const int y0 = static_cast<int>(std::floor(cy - r - thickness)), y1 = static_cast<int>(std::ceil(cy + r + thickness));
    const float inner = (r - thickness * 0.5f) * (r - thickness * 0.5f), outer = (r + thickness * 0.5f) * (r + thickness * 0.5f);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
            if (d2 >= inner && d2 <= outer) navPut(px, w, h, x, y, c);
        }
}

// Simple line with two colours: dark underlay, coloured core.
void navLine(std::vector<uint32_t>& px, int w, int h, float x0, float y0, float x1, float y1, uint32_t c) {
    int ix0 = static_cast<int>(x0), iy0 = static_cast<int>(y0);
    const int ix1 = static_cast<int>(x1), iy1 = static_cast<int>(y1);
    const int dx = std::abs(ix1 - ix0), dy = -std::abs(iy1 - iy0), sx = ix0 < ix1 ? 1 : -1, sy = iy0 < iy1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 512; ++guard) {
        navPut(px, w, h, ix0, iy0, c);
        navPut(px, w, h, ix0 + 1, iy0, c);
        if (ix0 == ix1 && iy0 == iy1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ix0 += sx; }
        if (e2 <= dx) { err += dx; iy0 += sy; }
    }
}

// A 5x7 glyph bitmap for X, Y, Z; drawn scaled, in one colour.
void navGlyph(std::vector<uint32_t>& px, int w, int h, int gx, int gy, int scale, char letter, uint32_t c) {
    static const uint8_t kGlyph[3][7] = {
        {0b10001, 0b01010, 0b00100, 0b00100, 0b01010, 0b10001, 0b00000},   // X
        {0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000},   // Y
        {0b11111, 0b00010, 0b00100, 0b01000, 0b11111, 0b00000, 0b00000},   // Z
    };
    int gi = letter == 'X' ? 0 : letter == 'Y' ? 1 : 2;
    for (int row = 0; row < 6; ++row)
        for (int col = 0; col < 5; ++col)
            if (kGlyph[gi][row] & (1 << (4 - col)))
                for (int s = 0; s < scale; ++s) navPut(px, w, h, gx + col * scale + s, gy + row * scale + s, c);
}

} // namespace

// The widget's metrics, shared by the drawing, the hit tests and the layout dump.
struct AxisWidgetLayout {
    int cx = 0, cy = 0, discR = 0, orbitR = 0, ballR = 0;
};
AxisWidgetLayout AxisWidgetBox(int viewW) {
    AxisWidgetLayout l;
    l.cx = viewW - 46;
    l.cy = 45;
    l.discR = 30;
    l.orbitR = 20;
    l.ballR = 8;
    return l;
}

// Screen position of a side's ball (view coordinates: origin at the view's top-left).
bool AxisWidgetBallAt(const AxisWidgetLayout& l, int side, float& x, float& y) {
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs};
    const Vec3& axis = kNavSideAxis[side];
    x = l.cx + (axis.x * right.x + axis.y * right.y + axis.z * right.z) * l.orbitR;
    y = l.cy - (axis.x * up.x + axis.y * up.y + axis.z * up.z) * l.orbitR;
    return true;
}

// The side whose ball is under a point, nearest the viewer first; -1 for none. (toCam points at the camera, so the
// ball with the largest toward-ness wins when two balls overlap.)
int AxisWidgetPick(const AxisWidgetLayout& l, float x, float y) {
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 toCam{-sn * cp, sp, cs * cp};
    int best = -1;
    float nearest = -1e9f;
    for (int side = 0; side < 6; ++side) {
        float bx, by;
        AxisWidgetBallAt(l, side, bx, by);
        const float dx = x - bx, dy = y - by;
        if (dx * dx + dy * dy > static_cast<float>(l.ballR + 2) * (l.ballR + 2)) continue;
        const Vec3& axis = kNavSideAxis[side];
        const float toward = axis.x * toCam.x + axis.y * toCam.y + axis.z * toCam.z;
        if (toward > nearest) { nearest = toward; best = side; }
    }
    return best;
}

// The widget's back-disc contains this point (used to start a drag over it).
bool AxisWidgetWithin(const AxisWidgetLayout& l, float x, float y) {
    const float dx = x - l.cx, dy = y - l.cy;
    return dx * dx + dy * dy <= static_cast<float>(l.discR) * l.discR;
}

// Snap the view to look at the model from that side (the zoom and pan stay). Called on a click on a ball.
void AxisWidgetSnapView(int side) {
    static const float kYaw[6] = {90.0f, 0.0f, 0.0f, 270.0f, 0.0f, 180.0f};    // +X +Y +Z -X -Y -Z -> the yaw that faces the side
    static const float kPitch[6] = {0.0f, 89.0f, 0.0f, 0.0f, -89.0f, 0.0f};
    g.cam.yawDeg = kYaw[side];
    g.cam.pitchDeg = kPitch[side];
    g.dirty = true;
    InvalidateView();
    Log("view: from " + std::string(side < 3 ? "+" : "-") + kNavLetter[side]);
}

// Burns the widget into the frame (called from Render after it drew the scene).
void DrawAxisWidget(std::vector<uint32_t>& px, int w, int h) {
    if (w < 170 || h < 120) return;
    const AxisWidgetLayout l = AxisWidgetBox(w);
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs}, toCam{-sn * cp, sp, cs * cp};
    float depth[6];
    for (int s = 0; s < 6; ++s) depth[s] = kNavSideAxis[s].x * toCam.x + kNavSideAxis[s].y * toCam.y + kNavSideAxis[s].z * toCam.z;
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(order, order + 6, [&](int a, int b) { return depth[a] < depth[b]; });   // far first, near over them
    if (g.viewHoverBall >= 0) {   // a soft white halo behind the hovered ball
        float hx, hy;
        AxisWidgetBallAt(l, g.viewHoverBall, hx, hy);
        navRing(px, w, h, hx, hy, static_cast<float>(l.ballR + 2), 2.0f, kNavRingWhite);
    }
    bool ballAtCenter = false;
    for (int oi = 0; oi < 6; ++oi) {
        const int side = order[oi];
        const bool positive = side < 3;
        float bx, by;
        AxisWidgetBallAt(l, side, bx, by);
        const int k = side % 3;
        if (positive) {
            navLine(px, w, h, l.cx, l.cy, bx, by, kNavAxisA[k]);
        }
        {
            const float cdx = bx - l.cx, cdy = by - l.cy;
            if (cdx * cdx + cdy * cdy < static_cast<float>(l.ballR + 2) * (l.ballR + 2)) ballAtCenter = true;
        }
        if (positive) {
            navDisc(px, w, h, bx, by, static_cast<float>(l.ballR), kNavAxisA[k]);
            navGlyph(px, w, h, static_cast<int>(std::lround(bx)) - 5, static_cast<int>(std::lround(by)) - 5, 2, kNavLetter[side], 0xFF0F1014);
        } else {
            navDisc(px, w, h, bx, by, static_cast<float>(l.ballR), kNavAxisDim[k]);
            navRing(px, w, h, bx, by, static_cast<float>(l.ballR - 1), 1.5f, kNavAxisA[k]);
        }
        if (side == g.viewHoverBall) navRing(px, w, h, bx, by, static_cast<float>(l.ballR + 1), 2.0f, kNavRingWhite);
    }
    if (!ballAtCenter) navDisc(px, w, h, static_cast<float>(l.cx), static_cast<float>(l.cy), 2.0f, kNavWhite);
}
