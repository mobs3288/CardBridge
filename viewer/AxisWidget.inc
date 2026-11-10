// viewer fragment: the orientation widget (included in order by main.cpp, after ModeStrip.inc)
// The Blender-style navigation widget, ported from the Project K editor's viewport gizmo (ViewportPanes.cpp): six balls on a
// circle — the model's +X/+Y/+Z ends in solid colour with a dark letter, the -X/-Y/-Z ends dim with an axis-coloured rim, lines
// from the centre to the positive balls, far balls under near ones. Click a ball to look at the model from that axis (the view
// stays zoom and pan), drag anywhere in it to orbit, hover shows a white halo and ring. Anti-aliased GDI+, drawn over the frame
// after the blit (like the mode strip), over both render paths.

#include <gdiplus.h>

namespace {

// The six sides, as in Project K: +X +Y +Z -X -Y -Z; a side's axis is axis[side % 3] * (side < 3 ? 1 : -1).
const Vec3 kSideAxis[6] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
const wchar_t kSideLetter[6] = {L'X', L'Y', L'Z', L'X', L'Y', L'Z'};
Gdiplus::Color SideColourA(int side, BYTE alpha) {   // +X red, +Y green, +Z blue (dimmed by the caller for - sides)
    static const int rgb[3][3] = {{230, 75, 85}, {120, 200, 60}, {70, 140, 240}};
    const int k = (side % 3 + 3) % 3;
    return Gdiplus::Color(alpha, static_cast<BYTE>(rgb[k][0]), static_cast<BYTE>(rgb[k][1]), static_cast<BYTE>(rgb[k][2]));
}
Gdiplus::Color SideDimA(int side, BYTE alpha) {
    static const int rgb[3][3] = {{120, 50, 58}, {70, 105, 45}, {45, 78, 128}};
    const int k = (side % 3 + 3) % 3;
    return Gdiplus::Color(alpha, static_cast<BYTE>(rgb[k][0]), static_cast<BYTE>(rgb[k][1]), static_cast<BYTE>(rgb[k][2]));
}

} // namespace

// The widget's metrics, shared by the drawing, the hit tests and the layout dump.
struct AxisWidgetLayout {
    int cx = 0, cy = 0, discR = 0, orbitR = 0, ballR = 0;
};
AxisWidgetLayout AxisWidgetBox(int x0, int W, int /*H*/) {
    AxisWidgetLayout l;
    l.cx = x0 + W - 46;
    l.cy = 45;
    l.discR = 30;
    l.orbitR = 20;
    l.ballR = 8;
    return l;
}

// Screen position of a side's ball (any current camera). -1: off, but always returned via out.
bool AxisWidgetBallAt(const AxisWidgetLayout& l, int side, float& x, float& y) {
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs};
    const Vec3& axis = kSideAxis[side];
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
        const Vec3& axis = kSideAxis[side];
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
    Log("view: from " + std::string(side < 3 ? "+" : "-") + static_cast<char>('X' + (side % 3)));
}

void DrawAxisWidget(HDC dc, int x0, int W, int H) {
    if (W < 170 || H < 120) return;   // too small to host it
    const AxisWidgetLayout l = AxisWidgetBox(x0, W, H);
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs}, toCam{-sn * cp, sp, cs * cp};
    float depth[6];
    for (int s = 0; s < 6; ++s) depth[s] = kSideAxis[s].x * toCam.x + kSideAxis[s].y * toCam.y + kSideAxis[s].z * toCam.z;
    int order[6] = {0, 1, 2, 3, 4, 5};
    std::sort(order, order + 6, [&](int a, int b) { return depth[a] < depth[b]; });   // far first, near over them

    Gdiplus::Graphics gp(dc);
    gp.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gp.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    gp.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gp.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    const Gdiplus::Color shadow(255, 13, 14, 19);
    bool ballAtCenter = false;
    if (g.viewHoverBall >= 0) {   // a soft white halo behind the hovered ball
        float hx, hy;
        AxisWidgetBallAt(l, g.viewHoverBall, hx, hy);
        Gdiplus::GraphicsPath halo;
        halo.AddEllipse(static_cast<INT>(hx - l.ballR - 3), static_cast<INT>(hy - l.ballR - 3), 2 * (l.ballR + 3), 2 * (l.ballR + 3));
        Gdiplus::SolidBrush haloBrush(Gdiplus::Color(46, 255, 255, 255));
        gp.FillPath(&haloBrush, &halo);
    }
    for (int oi = 0; oi < 6; ++oi) {
        const int side = order[oi];
        const int k = side % 3;
        const bool positive = side < 3;
        float bx, by;
        AxisWidgetBallAt(l, side, bx, by);
        {
            const float cdx = bx - l.cx, cdy = by - l.cy;
            if (cdx * cdx + cdy * cdy < static_cast<float>(l.ballR + 2) * (l.ballR + 2)) ballAtCenter = true;
        }
        if (positive) {   // the lines to the three positive balls, axis-coloured
            Gdiplus::Pen line(SideColourA(side, 255), 2.0f);
            line.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
            gp.DrawLine(&line, static_cast<float>(l.cx), static_cast<float>(l.cy), bx, by);
        }
        Gdiplus::GraphicsPath ball;
        ball.AddEllipse(static_cast<INT>(bx - l.ballR), static_cast<INT>(by - l.ballR), 2 * l.ballR, 2 * l.ballR);
        if (positive) {
            Gdiplus::SolidBrush fill(SideColourA(side, 255));
            gp.FillPath(&fill, &ball);
            Gdiplus::FontFamily fam(L"Segoe UI");
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            Gdiplus::GraphicsPath tp;
            const Gdiplus::RectF layout(bx - l.ballR, by - l.ballR, 2.0f * l.ballR, 2.0f * l.ballR);
            tp.AddString(&kSideLetter[side], 1, &fam, Gdiplus::FontStyleBold, 8.0f, layout, &fmt);
            Gdiplus::SolidBrush dark(shadow);
            gp.FillPath(&dark, &tp);
        } else {   // the negative ends: a dim fill with an axis-coloured rim, no letter
            Gdiplus::SolidBrush fill(SideDimA(side, 235));
            gp.FillPath(&fill, &ball);
            Gdiplus::Pen ring(SideColourA(side, 220), 1.5f);
            gp.DrawPath(&ring, &ball);
        }
        if (side == g.viewHoverBall) {   // the white ring round the hovered ball
            Gdiplus::Pen ring(Gdiplus::Color(240, 255, 255, 255), 2.0f);
            gp.DrawPath(&ring, &ball);
        }
    }
    if (!ballAtCenter) {   // a small bright centre dot (skipped when an edge-on ball sits in the middle)
        Gdiplus::GraphicsPath dot;
        dot.AddEllipse(l.cx - 2, l.cy - 2, 4, 4);
        Gdiplus::SolidBrush c(Gdiplus::Color(220, 235, 239, 247));
        gp.FillPath(&c, &dot);
    }
}
