// A small drawing kit for the viewer's panel: a retained off-screen surface that GDI draws text into and GDI+ draws the
// anti-aliased shapes into (both write the same pixels), the colour theme, and the logo. Windows only.
//
// The panel is drawn into the surface only where it changed (the clip given to Begin), then copied to the window; nothing is
// drawn straight onto the screen, so there is no flicker and a hover or a slider drag costs a few rows, not the whole panel.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ui {

// Theme: deep blue-black surfaces, the logo's blue and violet as accents, amber for "changed from the card".
constexpr COLORREF kBg = RGB(15, 17, 25), kCard = RGB(23, 26, 38), kCardHi = RGB(31, 36, 52), kLine = RGB(40, 46, 64), kLineHi = RGB(62, 72, 98);
constexpr COLORREF kText = RGB(228, 232, 245), kSub = RGB(150, 159, 182), kDim = RGB(94, 102, 126), kFaint = RGB(66, 73, 94);
constexpr COLORREF kAccent = RGB(86, 168, 255), kAccent2 = RGB(172, 110, 255), kAmber = RGB(244, 192, 94), kGood = RGB(112, 210, 142), kBad = RGB(244, 112, 112);

// One-time GDI+ start and stop (call around the window's life).
bool Startup();
void Shutdown();

// Loads the logo from an RCDATA resource (a PNG). False when it is missing; drawing the logo then does nothing.
bool LoadLogo(HINSTANCE inst, int resourceId);

class Surface {
public:
    Surface();
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    // Makes the surface w x h. True when it was (re)created, which leaves it blank: the caller must draw all of it.
    bool Resize(int w, int h);
    void Reset();   // frees the bitmap (before Shutdown)
    int width() const { return w_; }
    int height() const { return h_; }

    // Everything drawn between Begin and End stays inside `clip` (surface coordinates).
    void Begin(const RECT& clip);
    void End();

    // Shapes: GDI+, anti-aliased. Rects are given as RECT (right and bottom exclusive).
    void Fill(const RECT& r, COLORREF c, int alpha = 255);
    void FillRound(const RECT& r, int radius, COLORREF c, int alpha = 255);
    void StrokeRound(const RECT& r, int radius, COLORREF c, int alpha = 255);
    void FillGradient(const RECT& r, int radius, COLORREF left, COLORREF right);
    void FillDisc(float cx, float cy, float radius, COLORREF c, int alpha = 255);
    void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, COLORREF c);
    void Line(float x0, float y0, float x1, float y1, COLORREF c, float width = 1.0f);
    // Stretch-samples raw RGBA pixels into dst using the UV→pixel mapping u(x) = uForLeft + (x - dst.left) * duPerPx and
    // v(y) = vForTop + (y - dst.top) * dvPerPx, wrapping (or clamping) as the sampler does. channel 0 = colour composited
    // over a solid or checkerboard base; 1..3 = that channel as opaque gray, 4 = alpha as opaque gray. One bitmap and one
    // blit per call, not one call per pixel - the texture previews and the UV canvas.
    void BlitWrappedTexture(const RECT& dst, const uint8_t* rgba, int srcW, int srcH,
                            float uForLeft, float duPerPx, float vForTop, float dvPerPx,
                            bool clamp, int channel, bool checkerBase);
    // A loading bar: track, fill up to `fraction` (0..1), or an animated
    // sliding chunk when indeterminate (fraction < 0). `phase` animates it.
    void ProgressBar(const RECT& r, double fraction, double phase = 0.0);
    void Logo(const RECT& r, int radius);

    // Text: GDI (ClearType). `clip` keeps a long string inside its box.
    void Text(HFONT font, COLORREF c, int x, int y, const std::string& utf8, const RECT* clip = nullptr);
    void TextRight(HFONT font, COLORREF c, int right, int y, const std::string& utf8, const RECT* clip = nullptr);
    void TextCentered(HFONT font, COLORREF c, const RECT& box, const std::string& utf8);
    int TextWidth(HFONT font, const std::string& utf8);

    // Copies a part of the surface to a window DC.
    void Present(HDC dst, const RECT& src, int dstX, int dstY);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int w_ = 0, h_ = 0;
    RECT clip_{};
};

}   // namespace ui
