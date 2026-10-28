// CardBridge viewer: opens a Koikatsu card, assembles it exactly as the exporter does, and shows it in a window so every
// card can be checked against its picture without exporting. Windows only (plain Win32 + GDI, no extra dependencies).
//
//   cardbridge-viewer [card.png] [--library <dir>] [--mods <dir>] [--outfit N]
//
// Library and mods folders are remembered in %LOCALAPPDATA%\CardBridge\viewer.json. Drop a card on the window to open it.
//
//   mouse: left drag orbit, right/middle drag pan, wheel zoom      keys: Left/Right outfit, T textures, R reset view,
//   F/S/B front/side/back, I issues, click a part row to hide/show it

#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cardbridge;

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Slashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

std::string SettingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/viewer.json";
}
std::string ModCachePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/modindex.json";
}

struct Settings {
    std::string library, mods;
};
Settings LoadSettings() {
    Settings s;
    try {
        auto j = nlohmann::json::parse([] { Bytes b = fileio::ReadFile(SettingsPath()); return std::string(b.begin(), b.end()); }());
        s.library = j.value("library", "");
        s.mods = j.value("mods", "");
    } catch (...) {}
    return s;
}
void SaveSettings(const Settings& s) {
    try {
        fileio::CreateDirectories(SettingsPath().substr(0, SettingsPath().find_last_of('/')));
        nlohmann::json j;
        j["library"] = s.library;
        j["mods"] = s.mods;
        fileio::WriteTextFile(SettingsPath(), j.dump(2));
    } catch (...) {}
}

// ---- the loaded character ------------------------------------------------------------------------------------------
struct Loaded {
    assemble::Scene scene;
    std::string title;
    int outfit = 0, outfits = 0;
    std::vector<std::string> notes;   // card-level problems
};

struct App {
    HWND hwnd = nullptr;
    Settings settings;
    std::string cardPath;
    int wantOutfit = -1;
    // library state, built once
    resolve::ModIndex mods;
    resolve::ListTable lists;
    std::unique_ptr<resolve::Resolver> resolver;
    std::string resolverKey;

    std::shared_ptr<Loaded> loaded;
    std::vector<char> hidden;
    std::atomic<bool> busy{false};
    std::string status = "drop a Koikatsu card on this window";
    std::mutex mu;

    exporter::ViewCamera cam;
    exporter::ViewStatePtr state = exporter::NewViewState();
    exporter::ViewOptions vopt;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;
    bool interacting = false, dirty = true, showIssues = false, textured = true;
    POINT last{};
    int drag = 0;   // 1 orbit, 2 pan
    int partScroll = 0;
};
App g;

constexpr UINT WM_LOADED = WM_APP + 1;
constexpr int kPanel = 300;   // part list width

bool EnsureResolver(std::string& err) {
    std::string key = g.settings.library + "|" + g.settings.mods;
    if (g.resolver && key == g.resolverKey) return true;
    if (g.settings.library.empty()) { err = "no asset library: start with --library <dir>"; return false; }
    std::vector<std::string> issues;
    if (!g.settings.mods.empty()) {
        g.mods = resolve::ModIndex::Build(g.settings.mods, ModCachePath(), &issues, nullptr, nullptr);
    }
    auto chk = library::CheckLibrary(g.settings.library, false);
    if (!chk.ok) { err = "asset library problem: " + (chk.issues.empty() ? std::string("unusable") : chk.issues[0]); return false; }
    g.lists = resolve::ListTable();
    g.lists.loadBaseGame(library::AbdataRoot(g.settings.library), &issues);
    g.resolver = std::make_unique<resolve::Resolver>(g.mods, g.lists, library::AbdataRoot(g.settings.library));
    g.resolverKey = key;
    return true;
}

void StartLoad(const std::string& path, int outfit) {
    if (g.busy.exchange(true)) return;
    g.cardPath = path;
    g.status = "loading " + path + " ...";
    InvalidateRect(g.hwnd, nullptr, FALSE);
    std::thread([path, outfit]() {
        auto out = std::make_shared<Loaded>();
        std::string err;
        try {
            if (!EnsureResolver(err)) throw std::runtime_error(err);
            card::Card c = card::LoadCard(path);
            if (!c.supported) throw std::runtime_error(c.supportNote);
            assemble::AssembleOptions ao;
            ao.outfit = outfit;
            out->scene = assemble::Assemble(c, *g.resolver, ao);
            out->outfits = static_cast<int>(c.outfits.size());
            out->outfit = outfit >= 0 ? outfit : static_cast<int>(c.status.at("coordinateType").asInt());
            out->title = c.displayName();
        } catch (const std::exception& e) {
            out->notes.push_back(e.what());
        }
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.loaded = out;
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LOADED, 0, 0);
    }).detach();
}

size_t Triangles(const assemble::Scene& s) {
    size_t n = 0;
    for (const auto& m : s.meshes) for (const auto& sub : m.submeshes) n += sub.indices.size() / 3;
    return n;
}

// ---- drawing ---------------------------------------------------------------------------------------------------------
void Render(int w, int h) {
    std::shared_ptr<Loaded> L;
    { std::lock_guard<std::mutex> lk(g.mu); L = g.loaded; }
    if (w < 8 || h < 8) return;
    g.fw = w; g.fh = h;
    if (!L || L->scene.parts.empty()) { g.frame.assign(static_cast<size_t>(w) * h, 0xFFE2E4E8u); return; }
    g.hidden.resize(L->scene.parts.size(), 0);
    g.vopt.textured = g.textured;
    g.vopt.hiddenParts = &g.hidden;
    g.vopt.step = g.interacting ? 3 : 1;
    exporter::RenderView(L->scene, g.cam, w, h, g.vopt, *g.state, g.frame);
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - kPanel, H = rc.bottom;
    if (g.dirty || g.fw != W || g.fh != H) { Render(W, H); g.dirty = false; }
    if (!g.frame.empty() && g.fw == W && g.fh == H) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, kPanel, 0, W, H, 0, 0, W, H, g.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    // side panel
    RECT pr{0, 0, kPanel, rc.bottom};
    HBRUSH br = CreateSolidBrush(RGB(28, 30, 36));
    FillRect(dc, &pr, br);
    DeleteObject(br);
    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    HGDIOBJ old = SelectObject(dc, font);
    int y = 8;
    auto line = [&](const std::string& s, COLORREF col) {
        SetTextColor(dc, col);
        std::wstring w16 = Utf8ToWide(s);
        TextOutW(dc, 10, y, w16.c_str(), static_cast<int>(w16.size()));
        y += 17;
    };
    std::shared_ptr<Loaded> L;
    { std::lock_guard<std::mutex> lk(g.mu); L = g.loaded; }
    if (L && !L->scene.parts.empty()) {
        line(L->title + "   outfit " + std::to_string(L->outfit) + "/" + std::to_string(std::max(0, L->outfits - 1)), RGB(240, 240, 240));
        line(std::to_string(Triangles(L->scene)) + " triangles, " + std::to_string(L->scene.materials.size()) + " materials", RGB(160, 170, 190));
        size_t warn = 0, err = 0;
        for (const auto& i : L->scene.issues) (i.severity == "error" ? err : warn)++;
        line(std::to_string(err) + " errors, " + std::to_string(warn) + " warnings   [I] list", warn + err ? RGB(240, 190, 90) : RGB(120, 200, 120));
        line("Left/Right outfit  T textures  R reset", RGB(120, 130, 150));
        y += 6;
        if (g.showIssues) {
            for (const auto& i : L->scene.issues) {
                std::string t = (i.part.empty() ? "" : "[" + i.part + "] ") + i.message;
                for (size_t p = 0; p < t.size(); p += 40) line(t.substr(p, 40), i.severity == "error" ? RGB(240, 110, 110) : RGB(220, 190, 120));
                if (y > rc.bottom - 20) break;
            }
        } else {
            for (size_t i = static_cast<size_t>(g.partScroll); i < L->scene.parts.size(); ++i) {
                const auto& p = L->scene.parts[i];
                bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
                bool off = i < g.hidden.size() && g.hidden[i];
                COLORREF col = off ? RGB(90, 95, 105) : !ok ? RGB(235, 110, 110) : p.meshes.empty() ? RGB(150, 150, 150) : RGB(210, 225, 245);
                line(std::string(off ? "[ ] " : "[x] ") + p.id + (ok ? "" : "  (" + p.status + ")"), col);
                if (y > rc.bottom - 20) break;
            }
        }
    } else {
        line(g.status, RGB(220, 220, 220));
        if (L) for (const auto& n : L->notes) for (size_t p = 0; p < n.size(); p += 40) line(n.substr(p, 40), RGB(240, 110, 110));
    }
    if (g.busy) line("working ...", RGB(120, 200, 255));
    SelectObject(dc, old);
    DeleteObject(font);
    EndPaint(hwnd, &ps);
}

void ResetView(float yaw) { g.cam = {}; g.cam.yawDeg = yaw; g.dirty = true; InvalidateRect(g.hwnd, nullptr, FALSE); }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LOADED: {
        std::shared_ptr<Loaded> L;
        { std::lock_guard<std::mutex> lk(g.mu); L = g.loaded; }
        g.hidden.assign(L ? L->scene.parts.size() : 0, 0);
        g.state = exporter::NewViewState();
        g.dirty = true;
        if (L) SetWindowTextW(hwnd, Utf8ToWide("CardBridge viewer - " + (L->title.empty() ? g.cardPath : L->title)).c_str());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t buf[1024];
        if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, buf, 1024)) { g.cam = {}; StartLoad(Slashes(WideToUtf8(buf)), -1); }
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < kPanel) {   // part row click
            std::shared_ptr<Loaded> L;
            { std::lock_guard<std::mutex> lk(g.mu); L = g.loaded; }
            if (L && !g.showIssues) {
                int row = (y - 8 - 17 * 4 - 6) / 17 + g.partScroll;
                if (row >= 0 && static_cast<size_t>(row) < L->scene.parts.size() && static_cast<size_t>(row) < g.hidden.size()) {
                    g.hidden[static_cast<size_t>(row)] ^= 1;
                    g.dirty = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        g.drag = 1; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        g.drag = 2; g.last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; g.interacting = true; SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g.drag) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            float dx = static_cast<float>(x - g.last.x), dy = static_cast<float>(y - g.last.y);
            if (g.drag == 1) { g.cam.yawDeg -= dx * 0.5f; g.cam.pitchDeg = std::max(-89.0f, std::min(89.0f, g.cam.pitchDeg - dy * 0.5f)); }
            else { RECT rc; GetClientRect(hwnd, &rc); g.cam.panX += dx / rc.bottom; g.cam.panY += dy / rc.bottom; }
            g.last = {x, y};
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        g.drag = 0; g.interacting = false; ReleaseCapture();
        g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x < kPanel) { g.partScroll = std::max(0, g.partScroll - (d > 0 ? 3 : -3)); }
        else g.cam.zoom = std::max(0.2f, std::min(20.0f, g.cam.zoom * (d > 0 ? 1.12f : 1 / 1.12f)));
        g.dirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        std::shared_ptr<Loaded> L;
        { std::lock_guard<std::mutex> lk(g.mu); L = g.loaded; }
        if (wp == 'T') { g.textured = !g.textured; g.dirty = true; }
        else if (wp == 'R') ResetView(0);
        else if (wp == 'F') ResetView(0);
        else if (wp == 'S') ResetView(90);
        else if (wp == 'B') ResetView(180);
        else if (wp == 'I') g.showIssues = !g.showIssues;
        else if ((wp == VK_LEFT || wp == VK_RIGHT) && L && L->outfits > 1 && !g.cardPath.empty()) {
            int n = L->outfits, o = (L->outfit + (wp == VK_RIGHT ? 1 : n - 1)) % n;
            StartLoad(g.cardPath, o);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string card;
    g.settings = LoadSettings();
    for (int i = 1; i < argc; ++i) {
        std::string a = WideToUtf8(argvw[i]);
        if (a == "--library" && i + 1 < argc) g.settings.library = Slashes(WideToUtf8(argvw[++i]));
        else if (a == "--mods" && i + 1 < argc) g.settings.mods = Slashes(WideToUtf8(argvw[++i]));
        else if (a == "--outfit" && i + 1 < argc) g.wantOutfit = std::atoi(WideToUtf8(argvw[++i]).c_str());
        else card = Slashes(a);
    }
    LocalFree(argvw);
    SaveSettings(g.settings);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CardBridgeViewer";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"CardBridge viewer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1280, 800, nullptr, nullptr, inst, nullptr);
    ShowWindow(g.hwnd, show);
    if (!card.empty()) StartLoad(card, g.wantOutfit);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
