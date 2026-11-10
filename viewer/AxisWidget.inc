// viewer fragment: the orientation widget (included in order by main.cpp, after ModeStrip.inc)
// A Blender-style axis tripod in the top-right corner of the view, always on: how the model's X/Y/Z axes point relative to
// the camera right now. The nearest axis is drawn bright and last; an axis seen edge-on is drawn as a filled ball. Display
// only — the view is positioned with the ordinary left-drag. GDI, drawn after the frame is blitted (like the mode strip).

void DrawAxisWidget(HDC dc, int x0, int W, int H) {
    if (W < 170 || H < 120) return;   // too small to host it
    const float yaw = g.cam.yawDeg * 3.14159265358979323846f / 180.0f, pit = g.cam.pitchDeg * 3.14159265358979323846f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs}, toCam{-sn * cp, sp, cs * cp};
    static const Vec3 kAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static const COLORREF kCol[3] = {RGB(232, 96, 96), RGB(112, 224, 112), RGB(122, 168, 255)};   // X red, Y green, Z blue
    static const COLORREF kColDim[3] = {RGB(112, 54, 54), RGB(58, 110, 58), RGB(60, 82, 128)};    // far side: muted
    static const COLORREF kShadow = RGB(9, 10, 14);
    static const wchar_t kName[3] = {L'X', L'Y', L'Z'};
    static const float kDot = 0.18f;              // below this the axis is edge-on: draw a ball instead
    const int cx = x0 + W - 48, cy = 46, R = 22, discR = 34;
    // The back disc and its rim (dark, so the arrows read against any picture).
    {
        HBRUSH bg = CreateSolidBrush(RGB(23, 26, 38));
        HBRUSH oldB = static_cast<HBRUSH>(SelectObject(dc, bg));
        HPEN rim = CreatePen(PS_SOLID, 1, RGB(62, 72, 98));
        HPEN oldP = static_cast<HPEN>(SelectObject(dc, rim));
        Ellipse(dc, cx - discR, cy - discR, cx + discR, cy + discR);
        SelectObject(dc, oldP); SelectObject(dc, oldB);
        DeleteObject(rim); DeleteObject(bg);
    }
    float depth[3];
    for (int a = 0; a < 3; ++a) depth[a] = kAxis[a].x * toCam.x + kAxis[a].y * toCam.y + kAxis[a].z * toCam.z;
    bool ballAtCenter = false;
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return depth[a] < depth[b]; });   // far first, near last (drawn over)
    const int oldBk = SetBkMode(dc, TRANSPARENT);
    for (int oi = 0; oi < 3; ++oi) {
        const int a = order[oi];
        const Vec3& axis = kAxis[a];
        const float dx = axis.x * right.x + axis.y * right.y + axis.z * right.z;
        const float dy = axis.x * up.x + axis.y * up.y + axis.z * up.z;
        const float len = std::sqrt(dx * dx + dy * dy);
        const bool alive = len >= kDot;
        const float inv = 1.0f / std::max(len, 1e-4f);
        const float sx = dx * inv, sy = -dy * inv;   // screen y points down
        const COLORREF col = depth[a] >= 0.0f ? kCol[a] : kColDim[a];
        const HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g.fontBold));
        if (!alive) {   // pointing at the camera: a ball with the letter in it
            HBRUSH ball = CreateSolidBrush(col);
            HPEN rim = CreatePen(PS_SOLID, 1, kShadow);
            HBRUSH oldB = static_cast<HBRUSH>(SelectObject(dc, ball));
            HPEN oldP = static_cast<HPEN>(SelectObject(dc, rim));
            Ellipse(dc, cx - 11, cy - 11, cx + 11, cy + 11);
            SelectObject(dc, oldP); SelectObject(dc, oldB);
            DeleteObject(rim); DeleteObject(ball);
            SetTextColor(dc, kShadow);
            RECT r{cx - 11, cy - 11, cx + 11, cy + 11};
            DrawTextW(dc, &kName[a], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            ballAtCenter = true;
            continue;
        }
        const int tx = cx + static_cast<int>(std::lround(sx * R)), ty = cy + static_cast<int>(std::lround(sy * R));
        // the shaft, with a dark outline (draw a thick black line, then the coloured one on top)
        HPEN dark = CreatePen(PS_SOLID, 5, kShadow);
        HPEN col3 = CreatePen(PS_SOLID, 3, col);
        {
            HPEN oldP = static_cast<HPEN>(SelectObject(dc, dark));
            MoveToEx(dc, cx, cy, nullptr); LineTo(dc, tx, ty);
            SelectObject(dc, oldP);
        }
        {
            HPEN oldP = static_cast<HPEN>(SelectObject(dc, col3));
            MoveToEx(dc, cx, cy, nullptr); LineTo(dc, tx, ty);
            SelectObject(dc, oldP);
        }
        // the arrowhead (a small filled triangle), then the letter just past it
        const float pxf = -sy, pyf = sx;   // perpendicular of the axis on screen
        const float apex = 4.0f, half = 4.6f;
        POINT tip[3];
        tip[0] = POINT{static_cast<LONG>(std::lround(tx + sx * apex)), static_cast<LONG>(std::lround(ty + sy * apex))};
        tip[1] = POINT{static_cast<LONG>(std::lround(tx + pxf * half)), static_cast<LONG>(std::lround(ty + pyf * half))};
        tip[2] = POINT{static_cast<LONG>(std::lround(tx - pxf * half)), static_cast<LONG>(std::lround(ty - pyf * half))};
        HPEN nPen = static_cast<HPEN>(GetStockObject(NULL_PEN));
        HPEN oldP = static_cast<HPEN>(SelectObject(dc, nPen));
        HBRUSH head = CreateSolidBrush(col);
        HBRUSH oldB = static_cast<HBRUSH>(SelectObject(dc, head));
        Polygon(dc, tip, 3);
        // outline ring of the triangle for contrast against dark pictures: skip (the shaft already carries the outline)
        SelectObject(dc, oldP); SelectObject(dc, oldB);
        DeleteObject(head);
        // the letter, shadowed by one pixel
        const int lx = cx + static_cast<int>(std::lround(sx * (R + 12))), ly = cy + static_cast<int>(std::lround(sy * (R + 12)));
        RECT r{lx - 12, ly - 10, lx + 12, ly + 10};
        SetTextColor(dc, kShadow);
        RECT off{r.left + 1, r.top + 1, r.right + 1, r.bottom + 1};
        DrawTextW(dc, &kName[a], 1, &off, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, col);
        DrawTextW(dc, &kName[a], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetBkMode(dc, oldBk);
        SelectObject(dc, oldFont);
        DeleteObject(dark); DeleteObject(col3);
    }
    // the centre dot (skipped when an edge-on axis ball sits in the middle)
    if (!ballAtCenter) {
        HBRUSH dot = CreateSolidBrush(RGB(240, 244, 252));
        HPEN darkPen = CreatePen(PS_SOLID, 1, kShadow);
        HBRUSH oldB = static_cast<HBRUSH>(SelectObject(dc, dot));
        HPEN oldP = static_cast<HPEN>(SelectObject(dc, darkPen));
        Ellipse(dc, cx - 3, cy - 3, cx + 4, cy + 4);
        SelectObject(dc, oldP); SelectObject(dc, oldB);
        DeleteObject(darkPen); DeleteObject(dot);
    }
}
