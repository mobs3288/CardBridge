#include "Ui.h"

#include <algorithm>
using std::max;   // the MinGW GDI+ headers use unqualified min / max
using std::min;

#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstring>

namespace ui {

namespace {

ULONG_PTR g_token = 0;
std::unique_ptr<Gdiplus::Bitmap> g_logo;           // the logo as loaded (premultiplied ARGB)
std::unique_ptr<Gdiplus::Bitmap> g_logoScaled;     // the logo at the size it was last drawn
int g_logoScaledSize = 0;

Gdiplus::Color Col(COLORREF c, int a = 255) { return Gdiplus::Color(static_cast<BYTE>(a), GetRValue(c), GetGValue(c), GetBValue(c)); }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) / 2.0f);
    if (r < 0.75f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    const float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

}   // namespace

bool Startup() {
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_token, &in, nullptr) == Gdiplus::Ok;
}

void Shutdown() {
    g_logoScaled.reset();
    g_logo.reset();
    if (g_token) Gdiplus::GdiplusShutdown(g_token);
    g_token = 0;
}

bool LoadLogo(HINSTANCE inst, int resourceId) {
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(resourceId), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return false;
    HGLOBAL loaded = LoadResource(inst, res);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(inst, res);
    if (!data || !size) return false;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);   // the stream owns this and frees it
    if (!copy) return false;
    std::memcpy(GlobalLock(copy), data, size);
    GlobalUnlock(copy);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK) { GlobalFree(copy); return false; }
    Gdiplus::Bitmap from(stream);
    bool ok = from.GetLastStatus() == Gdiplus::Ok;
    if (ok) {   // a copy that does not need the stream any more
        g_logo.reset(new Gdiplus::Bitmap(static_cast<INT>(from.GetWidth()), static_cast<INT>(from.GetHeight()), PixelFormat32bppPARGB));
        Gdiplus::Graphics gg(g_logo.get());
        gg.DrawImage(&from, 0, 0, static_cast<INT>(from.GetWidth()), static_cast<INT>(from.GetHeight()));
    }
    stream->Release();
    return ok;
}

struct Surface::Impl {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    void* bits = nullptr;
    std::unique_ptr<Gdiplus::Bitmap> wrap;   // the same pixels, for GDI+
    std::unique_ptr<Gdiplus::Graphics> gfx;
    bool gdiPending = false;                 // GDI drew since GDI+ last did: flush before GDI+ touches the pixels
    // Persistent scratch DIB for BlitWrappedTexture (owned once, grown as needed).
    HDC dibDc = nullptr;
    HBITMAP dibBuf = nullptr;
    void* dibBits = nullptr;
    int dibW = 0, dibH = 0;
    // One reused pen (creating one per Line() costs more than the draw itself when the wireframe is re-stroking).
    std::unique_ptr<Gdiplus::Pen> penCache;
    COLORREF penColor = 0;
    float penWidth = -1.0f;

    Gdiplus::Graphics& G() {
        if (gdiPending) { GdiFlush(); gdiPending = false; }
        return *gfx;
    }
    void ReleaseDib() {
        if (dibDc) DeleteDC(dibDc);      // detaches the bitmap; it must not be deleted while selected
        if (dibBuf) DeleteObject(dibBuf);
        dibDc = nullptr; dibBuf = nullptr; dibBits = nullptr; dibW = dibH = 0;
    }
    // Makes the scratch DIB at least w x h. False when a DIB section could not be made.
    bool EnsureDib(int w, int h) {
        if (dibDc && dibBuf && dibW >= w && dibH >= h) return true;
        ReleaseDib();
        HDC screen = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP d = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC newdc = d ? CreateCompatibleDC(screen) : nullptr;
        ReleaseDC(nullptr, screen);
        if (!d || !newdc) { if (d) DeleteObject(d); return false; }
        dibBuf = d;
        dibBits = bits;
        dibDc = newdc;
        dibW = w;
        dibH = h;
        SelectObject(dibDc, dibBuf);
        return true;
    }
    void Release() {
        ReleaseDib();
        penCache.reset();
        penWidth = -1.0f;
        gfx.reset();
        wrap.reset();
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        dc = nullptr; bmp = nullptr; old = nullptr; bits = nullptr;
    }
};

Surface::Surface() : impl_(new Impl) {}
Surface::~Surface() { impl_->Release(); }

void Surface::Reset() {
    impl_->Release();
    w_ = h_ = 0;
}

bool Surface::Resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == w_ && h == h_ && impl_->dc) return false;
    impl_->Release();
    HDC screen = GetDC(nullptr);
    impl_->dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    impl_->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &impl_->bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    impl_->old = static_cast<HBITMAP>(SelectObject(impl_->dc, impl_->bmp));
    impl_->wrap.reset(new Gdiplus::Bitmap(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(impl_->bits)));
    impl_->gfx.reset(new Gdiplus::Graphics(impl_->wrap.get()));
    impl_->gfx->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    impl_->gfx->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    w_ = w;
    h_ = h;
    return true;
}

void Surface::Begin(const RECT& clip) {
    clip_ = clip;
    SelectClipRgn(impl_->dc, nullptr);
    IntersectClipRect(impl_->dc, clip.left, clip.top, clip.right, clip.bottom);
    impl_->G().SetClip(Gdiplus::Rect(clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top));
}

void Surface::End() { SelectClipRgn(impl_->dc, nullptr); }

void Surface::Fill(const RECT& r, COLORREF c, int alpha) {
    if (alpha < 255) {
        Gdiplus::SolidBrush b(Col(c, alpha));
        impl_->G().FillRectangle(&b, static_cast<INT>(r.left), static_cast<INT>(r.top), static_cast<INT>(r.right - r.left), static_cast<INT>(r.bottom - r.top));
        return;
    }
    // GDI fills a solid rectangle many times faster than GDI+
    SetBkColor(impl_->dc, c);
    ExtTextOutW(impl_->dc, 0, 0, ETO_OPAQUE, &r, L"", 0, nullptr);
    impl_->gdiPending = true;
}

void Surface::FillRound(const RECT& r, int radius, COLORREF c, int alpha) {
    Gdiplus::GraphicsPath p;
    RoundPath(p, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top), static_cast<float>(radius));
    Gdiplus::SolidBrush b(Col(c, alpha));
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (alpha < 255 || radius < 2 || w < 6 * radius || h < 6 * radius) {
        impl_->G().FillPath(&b, &p);
        return;
    }
    // A big rounded rectangle: the straight parts by GDI, only the four corners (anti-aliased) by GDI+.
    Fill(RECT{r.left + radius, r.top, r.right - radius, r.bottom}, c);
    Fill(RECT{r.left, r.top + radius, r.left + radius, r.bottom - radius}, c);
    Fill(RECT{r.right - radius, r.top + radius, r.right, r.bottom - radius}, c);
    Gdiplus::Graphics& g = impl_->G();
    const RECT corners[4] = {{r.left, r.top, r.left + radius, r.top + radius},
                             {r.right - radius, r.top, r.right, r.top + radius},
                             {r.left, r.bottom - radius, r.left + radius, r.bottom},
                             {r.right - radius, r.bottom - radius, r.right, r.bottom}};
    for (const RECT& k : corners) {
        RECT t;
        if (!IntersectRect(&t, &k, &clip_)) continue;
        g.SetClip(Gdiplus::Rect(t.left, t.top, t.right - t.left, t.bottom - t.top));
        g.FillPath(&b, &p);
    }
    g.SetClip(Gdiplus::Rect(clip_.left, clip_.top, clip_.right - clip_.left, clip_.bottom - clip_.top));
}

void Surface::StrokeRound(const RECT& r, int radius, COLORREF c, int alpha) {
    Gdiplus::GraphicsPath p;
    RoundPath(p, r.left + 0.5f, r.top + 0.5f, static_cast<float>(r.right - r.left) - 1.0f, static_cast<float>(r.bottom - r.top) - 1.0f, static_cast<float>(radius));
    Gdiplus::Pen pen(Col(c, alpha), 1.0f);
    impl_->G().DrawPath(&pen, &p);
}

void Surface::FillGradient(const RECT& r, int radius, COLORREF left, COLORREF right) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    Gdiplus::GraphicsPath p;
    const Gdiplus::RectF rf(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
    RoundPath(p, rf.X, rf.Y, rf.Width, rf.Height, static_cast<float>(radius));
    Gdiplus::LinearGradientBrush b(rf, Col(left), Col(right), Gdiplus::LinearGradientModeHorizontal);
    impl_->G().FillPath(&b, &p);
}

void Surface::FillDisc(float cx, float cy, float radius, COLORREF c, int alpha) {
    Gdiplus::SolidBrush b(Col(c, alpha));
    impl_->G().FillEllipse(&b, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
}

void Surface::FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, COLORREF c) {
    const Gdiplus::PointF pts[3] = {{x0, y0}, {x1, y1}, {x2, y2}};
    Gdiplus::SolidBrush b(Col(c));
    impl_->G().FillPolygon(&b, pts, 3);
}

void Surface::Line(float x0, float y0, float x1, float y1, COLORREF c, float width) {
    Impl& I = *impl_;
    if (I.penWidth != width || I.penColor != c || !I.penCache) {
        I.penCache.reset(new Gdiplus::Pen(Col(c), width));
        I.penCache->SetStartCap(Gdiplus::LineCapRound);
        I.penCache->SetEndCap(Gdiplus::LineCapRound);
        I.penColor = c;
        I.penWidth = width;
    }
    impl_->G().DrawLine(I.penCache.get(), x0, y0, x1, y1);
}

void Surface::ProgressBar(const RECT& r, double fraction, double phase) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    const int h = r.bottom - r.top;
    FillRound(r, h / 2, kLine);
    if (fraction >= 0.0) {
        const double f = std::clamp(fraction, 0.0, 1.0);
        if (f > 0.001) {
            RECT fill{r.left, r.top, r.left + static_cast<int>((r.right - r.left) * f), r.bottom};
            FillGradient(fill, h / 2, kAccent, kAccent2);
        }
    } else {
        // Indeterminate: a chunk sliding across, wrapping on its own length.
        const double w = static_cast<double>(r.right - r.left);
        const double chunk = w * 0.28;
        double t = phase - static_cast<double>(static_cast<long long>(phase));
        const double x = (t * (w + chunk)) - chunk;
        RECT seg{static_cast<int>(r.left + x), r.top, static_cast<int>(r.left + x + chunk), r.bottom};
        RECT clipped;
        if (IntersectRect(&clipped, &seg, &r)) FillGradient(clipped, h / 2, kAccent, kAccent2);
    }
    StrokeRound(r, h / 2, kLineHi);
}

void Surface::BlitWrappedTexture(const RECT& dst, const uint8_t* rgba, int srcW, int srcH,
                                 float uForLeft, float duPerPx, float vForTop, float dvPerPx,
                                 bool clamp, int channel, bool checkerBase) {
    if (!rgba || srcW <= 0 || srcH <= 0) return;
    const int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return;
    if (!impl_->EnsureDib(dw, dh)) return;
    const int W = impl_->dibW;
    uint8_t* bits = static_cast<uint8_t*>(impl_->dibBits);
    for (int r = 0; r < dh; ++r) {
        const float v = vForTop + r * dvPerPx;
        uint8_t* row = &bits[static_cast<size_t>(r) * W * 4];
        for (int c = 0; c < dw; ++c) {
            float uu = uForLeft + c * duPerPx;
            float vv = v;
            if (clamp) { uu = std::clamp(uu, 0.0f, 1.0f); vv = std::clamp(vv, 0.0f, 1.0f); }
            else { uu -= std::floor(uu); vv -= std::floor(vv); }
            const int tx = std::min(srcW - 1, std::max(0, static_cast<int>(uu * srcW)));
            const int ty = std::min(srcH - 1, std::max(0, static_cast<int>((1.0f - vv) * srcH)));
            const uint8_t* p = &rgba[(static_cast<size_t>(ty) * srcW + tx) * 4];
            uint8_t* d = &row[c * 4];
            if (channel == 0) {
                int br, bg, bb;
                if (checkerBase) {
                    const bool light = (((c >> 3) + (r >> 3)) & 1) != 0;   // 8 px cells, like the editor's transparency grid
                    br = light ? 52 : 32; bg = light ? 56 : 34; bb = light ? 70 : 44;
                } else { br = 22; bg = 24; bb = 34; }
                const int a = p[3];
                d[0] = static_cast<uint8_t>((p[2] * a + bb * (255 - a)) / 255);   // B
                d[1] = static_cast<uint8_t>((p[1] * a + bg * (255 - a)) / 255);   // G
                d[2] = static_cast<uint8_t>((p[0] * a + br * (255 - a)) / 255);   // R
                d[3] = 255;
            } else {
                const int gv = channel == 4 ? p[3] : channel == 1 ? p[0] : channel == 2 ? p[1] : p[2];
                d[0] = d[1] = d[2] = static_cast<uint8_t>(gv);
                d[3] = 255;
            }
        }
    }
    SetStretchBltMode(impl_->dc, COLORONCOLOR);
    StretchBlt(impl_->dc, dst.left, dst.top, dw, dh, impl_->dibDc, 0, 0, dw, dh, SRCCOPY);
    impl_->gdiPending = true;
}

void Surface::Logo(const RECT& r, int radius) {
    const int size = r.right - r.left;
    if (!g_logo || size < 4) return;
    if (!g_logoScaled || g_logoScaledSize != size) {   // scaled once with the good filter, not on every redraw
        g_logoScaled.reset(new Gdiplus::Bitmap(size, size, PixelFormat32bppPARGB));
        Gdiplus::Graphics gg(g_logoScaled.get());
        gg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        gg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        gg.DrawImage(g_logo.get(), Gdiplus::Rect(0, 0, size, size), 0, 0, static_cast<INT>(g_logo->GetWidth()), static_cast<INT>(g_logo->GetHeight()), Gdiplus::UnitPixel);
        g_logoScaledSize = size;
    }
    Gdiplus::GraphicsPath p;
    RoundPath(p, static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(size), static_cast<float>(r.bottom - r.top), static_cast<float>(radius));
    Gdiplus::TextureBrush tb(g_logoScaled.get(), Gdiplus::WrapModeClamp);
    tb.TranslateTransform(static_cast<float>(r.left), static_cast<float>(r.top));
    impl_->G().FillPath(&tb, &p);
}

void Surface::Text(HFONT font, COLORREF c, int x, int y, const std::string& utf8, const RECT* clip) {
    const std::wstring w = Widen(utf8);
    HGDIOBJ old = SelectObject(impl_->dc, font);
    SetTextColor(impl_->dc, c);
    SetBkMode(impl_->dc, TRANSPARENT);
    SetTextAlign(impl_->dc, TA_LEFT | TA_TOP);
    ExtTextOutW(impl_->dc, x, y, clip ? ETO_CLIPPED : 0, clip, w.c_str(), static_cast<UINT>(w.size()), nullptr);
    SelectObject(impl_->dc, old);
    impl_->gdiPending = true;
}

void Surface::TextRight(HFONT font, COLORREF c, int right, int y, const std::string& utf8, const RECT* clip) {
    const std::wstring w = Widen(utf8);
    HGDIOBJ old = SelectObject(impl_->dc, font);
    SetTextColor(impl_->dc, c);
    SetBkMode(impl_->dc, TRANSPARENT);
    SetTextAlign(impl_->dc, TA_RIGHT | TA_TOP);
    ExtTextOutW(impl_->dc, right, y, clip ? ETO_CLIPPED : 0, clip, w.c_str(), static_cast<UINT>(w.size()), nullptr);
    SetTextAlign(impl_->dc, TA_LEFT | TA_TOP);
    SelectObject(impl_->dc, old);
    impl_->gdiPending = true;
}

void Surface::TextCentered(HFONT font, COLORREF c, const RECT& box, const std::string& utf8) {
    const std::wstring w = Widen(utf8);
    HGDIOBJ old = SelectObject(impl_->dc, font);
    SetTextColor(impl_->dc, c);
    SetBkMode(impl_->dc, TRANSPARENT);
    RECT t = box;
    DrawTextW(impl_->dc, w.c_str(), static_cast<int>(w.size()), &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(impl_->dc, old);
    impl_->gdiPending = true;
}

int Surface::TextWidth(HFONT font, const std::string& utf8) {
    const std::wstring w = Widen(utf8);
    HGDIOBJ old = SelectObject(impl_->dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(impl_->dc, w.c_str(), static_cast<int>(w.size()), &sz);
    SelectObject(impl_->dc, old);
    return sz.cx;
}

void Surface::Present(HDC dst, const RECT& src, int dstX, int dstY) {
    if (impl_->gdiPending) { GdiFlush(); impl_->gdiPending = false; }
    BitBlt(dst, dstX, dstY, src.right - src.left, src.bottom - src.top, impl_->dc, src.left, src.top, SRCCOPY);
}

}   // namespace ui
