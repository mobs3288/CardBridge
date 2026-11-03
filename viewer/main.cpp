// CardBridge viewer: opens a Koikatsu card, assembles it exactly as the exporter does, and shows it in a window so every
// card can be checked against its picture without exporting. Windows only (plain Win32 + GDI, no extra dependencies).
//
//   cardbridge-viewer [card.png] [--library <dir>] [--mods <dir>] [--outfit N]
//
// Library and mods folders are remembered in %LOCALAPPDATA%\CardBridge\viewer.json. Drop a card on the window to open it.
//
//   mouse: left drag orbit, right/middle drag pan, wheel zoom, hover a part to light it up in the model,
//          click a part to hide/show it, right-click a part to solo it
//   keys:  Left/Right or 1-7 outfit, T textures, H the game shaders on/off, R reset view, F/S/B front/side/back,
//          I issues, A show every part, E export the current scene to PMX, C clear the log, O open a card,
//          M mods folder, L asset library folder

#include "assemble/Assembler.h"
#include "export/ColourGrade.h"
#include "export/Exporter.h"
#include "export/GpuView.h"
#include "card/CardFile.h"
#include "core/FileIo.h"
#include "export/Preview.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "shape/Shape.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
std::string SafeStem(std::string s) {
    for (char& c : s)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            static_cast<unsigned char>(c) < 32)
            c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("character") : s;
}

std::string SettingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/viewer.json";
}
std::string ModCachePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/modindex.json";
}

// The folder the mod index was last built for (the cache stores it), so the viewer finds the mods without asking.
std::string CachedModsRoot() {
    try {
        Bytes b = fileio::ReadFile(ModCachePath());
        auto j = nlohmann::json::parse(std::string(b.begin(), b.end()));
        return Slashes(j.value("root", ""));
    } catch (...) { return {}; }
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

// ---- dialogs and first-run defaults ------------------------------------------------------------------------------------
std::string PickFolder(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[MAX_PATH];
    std::string r;
    if (SHGetPathFromIDListW(pidl, path)) r = Slashes(WideToUtf8(path));
    CoTaskMemFree(pidl);
    return r;
}

std::string PickCard(HWND owner) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = owner;
    of.lpstrFilter = L"Koikatsu cards (*.png)\0*.png\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrTitle = L"Open a Koikatsu character card";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of) ? Slashes(WideToUtf8(file)) : std::string();
}

bool Exists(const std::string& p) { return GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

// The asset library normally sits in the CardBridge folder (library_full, else library): look from the exe upwards.
std::string FindLibrary() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string dir = Slashes(WideToUtf8(exe));
    for (int up = 0; up < 5; ++up) {
        dir = dir.substr(0, dir.find_last_of('/'));
        for (const char* sub : {"/cardbridge/library_full", "/library_full", "/cardbridge/library", "/library"})
            if (Exists(dir + sub + "/abdata") || Exists(dir + sub + "/library.json")) return dir + sub;
        if (dir.find('/') == std::string::npos) break;
    }
    return {};
}

// ---- live log ---------------------------------------------------------------------------------------------------------
enum class LogKind { Info, Good, Warn, Error };
struct LogLine {
    std::string time, text;
    LogKind kind = LogKind::Info;
};

// ---- the loaded character ------------------------------------------------------------------------------------------
struct Loaded {
    assemble::Scene scene;
    card::Card card;
    std::string title;
    int outfit = 0, outfits = 0;
    std::vector<size_t> partTriangles;
    std::vector<std::string> notes;   // card-level problems
};

constexpr int kPanel = 380;
constexpr int kRowH = 20;

struct PartRow {
    int part = -1;        // -1 = a group header
    std::string group;    // header text
};

// ---- tweaks: the sliders and switches on top of what the card says --------------------------------------------------------
// Changing one re-assembles the character (a few seconds, after the pointer has been still for a moment); the exported PMX is
// the re-assembled scene, so it carries the tweaked shape.
struct Tweaks {
    std::vector<float> body, face, bodyCard, faceCard;   // current slider values and the card's own (the defaults)
    bool nippleInside = false;
    float nippleMm = 0.0f;                                // nipple bones along the chest normal, millimetres
    float areola = 0.0f, areolaCard = 0.0f;              // areola texture size
};
enum Tab { TabParts = 0, TabBody, TabFace, TabLook, TabCount };
const char* const kTabNames[TabCount] = {"Parts", "Body", "Face", "Look"};
// Slider ids: body 0..43, face 100..151, then the look sliders; switches from 300.
constexpr int kFaceBase = 100, kNippleMm = 200, kAreola = 201, kNippleIn = 300;
struct TweakRow {
    int kind = 0;        // 0 group header, 1 slider, 2 switch
    int id = 0;
    std::string label, group;
};

struct PanelLayout {
    RECT tabs[TabCount];
    RECT open, exportPmx, issues, reset, all;
    int outfitNameTop = 0, outfitChipsTop = 0;
    int partsHeadingTop = 0, partsTop = 0, partsBottom = 0, logTop = 0, logBottom = 0, footerTop = 0;
    RECT clearLog{};
};

PanelLayout LayoutFor(int H) {
    PanelLayout l;
    l.open = {10, 88, 68, 112};
    l.exportPmx = {74, 88, 174, 112};
    l.issues = {180, 88, 244, 112};
    l.reset = {250, 88, 308, 112};
    l.all = {314, 88, 370, 112};
    for (int i = 0; i < TabCount; ++i) l.tabs[i] = {10 + i * 62, 176, 68 + i * 62, 198};
    const int logH = std::clamp(H / 3, 130, 330);
    l.footerTop = H - 34;
    l.logBottom = l.footerTop - 4;
    l.logTop = std::max(170, l.logBottom - logH);
    // The outfit picker sits between the header buttons and the part list:
    // a name line, then a row of numbered slots.
    l.outfitNameTop = 126;
    l.outfitChipsTop = 146;
    l.partsHeadingTop = 180;
    l.partsTop = 202;
    l.partsBottom = std::max(l.partsTop + kRowH, l.logTop - 8);
    l.clearLog = {kPanel - 58, l.logTop + 5, kPanel - 10, l.logTop + 21};
    return l;
}

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
    std::vector<char> hidden;      // the part checkboxes (1 = unticked)
    std::vector<char> effHidden;   // what the renderers get: hidden plus the nude logic (1 hide, 2 show a default-hidden under piece)
    std::atomic<bool> busy{false};
    std::atomic<bool> exporting{false};
    std::string status = "drop a Koikatsu card on this window";
    std::string modsInfo;   // what the mod index found, shown in the panel
    std::mutex mu;

    exporter::ViewCamera cam;
    exporter::ViewStatePtr state = exporter::NewViewState();
    exporter::GpuView gpu;                    // the game's shaders on Direct3D 11
    std::shared_ptr<Loaded> gpuScene;         // the scene the GPU copy was made from (keeps it alive while the GPU draws it)
    bool gpuOk = false;
    exporter::ViewOptions vopt;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;
    ULONGLONG lastInput = 0;   // tick of the last camera change: the game shaders run only once the view has been still for a moment
    bool interacting = false, dirty = true, showIssues = false, textured = true, shaded = true;
    POINT last{};
    int drag = 0;   // 1 orbit, 2 pan
    int partScroll = 0, logScroll = 0;
    int hoverRow = -1, hoverPart = -1, hoverButton = 0;   // button: 0 none, 1 open, 2 export, 3 issues, 4 reset, 5 all
    int hoverOutfit = -1;                 // slot the cursor is over, for the name line
    int loadingOutfit = -1;               // outfit the worker is assembling, or -1
    int soloPart = -1;
    std::set<std::string> collapsedGroups;
    Tweaks tw;
    int tab = TabParts;
    int sliderDrag = -1;                      // slider id being dragged, or -1
    std::map<std::string, char> keepHidden;   // part checkboxes to restore after a tweak reload
    bool applyKeep = false;
    bool tweakPending = false;                // a change is waiting for the pointer to be still

    std::mutex logMu;
    std::vector<LogLine> logLines;
    HFONT fontTitle = nullptr, fontUi = nullptr, fontBold = nullptr, fontMono = nullptr, fontLog = nullptr, fontSmall = nullptr;
};
App g;

constexpr UINT WM_LOADED = WM_APP + 1;
constexpr UINT WM_LOG = WM_APP + 2;
constexpr UINT WM_EXPORT_DONE = WM_APP + 3;
constexpr UINT_PTR kTimerApplyTweaks = 2;

std::shared_ptr<Loaded> Current() {
    std::lock_guard<std::mutex> lk(g.mu);
    return g.loaded;
}

void InvalidatePanel() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    rc.right = kPanel;
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void Log(const std::string& text, LogKind kind = LogKind::Info) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[16];
    std::snprintf(ts, sizeof ts, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    // Wrap long messages here, so the paint loop stays a plain line blitter. The log text column is
    // kPanel - 76 - 8 px wide in 12 px Consolas, which fits about 40 characters.
    std::vector<std::string> parts;
    std::string rest = text;
    while (rest.size() > 40) {
        size_t cut = rest.rfind(' ', 40);
        if (cut == std::string::npos || cut < 20) cut = 40;
        parts.push_back(rest.substr(0, cut));
        rest = rest.substr(cut + (rest[cut] == ' ' ? 1 : 0));
    }
    parts.push_back(rest);
    {
        std::lock_guard<std::mutex> lk(g.logMu);
        for (size_t i = 0; i < parts.size(); ++i) g.logLines.push_back({i == 0 ? ts : std::string(8, ' '), parts[i], kind});
        if (g.logLines.size() > 800) g.logLines.erase(g.logLines.begin(), g.logLines.begin() + 200);
        g.logScroll = std::min(g.logScroll, static_cast<int>(g.logLines.size()));
    }
    if (g.hwnd) PostMessageW(g.hwnd, WM_LOG, 0, 0);
}

void ClearLog() {
    {
        std::lock_guard<std::mutex> lk(g.logMu);
        g.logLines.clear();
        g.logScroll = 0;
    }
    InvalidatePanel();
}

size_t Triangles(const assemble::Scene& s) {
    size_t n = 0;
    for (const auto& m : s.meshes) for (const auto& sub : m.submeshes) n += sub.indices.size() / 3;
    return n;
}

std::string Count(size_t n) {
    char b[32];
    if (n >= 1000000) std::snprintf(b, sizeof b, "%.1fm", n / 1000000.0);
    else if (n >= 10000) std::snprintf(b, sizeof b, "%.1fk", n / 1000.0);
    else std::snprintf(b, sizeof b, "%zu", n);
    return b;
}

// ---- tweak access ----------------------------------------------------------------------------------------------------
struct SliderRef {
    float* v = nullptr;
    float def = 0, lo = 0, hi = 1, step = 0.01f;
    bool percent = true;   // shown as 0..100 %, otherwise as plain millimetres
};

bool SliderFor(int id, SliderRef& r) {
    if (id >= 0 && id < kFaceBase) {
        if (static_cast<size_t>(id) >= g.tw.body.size()) return false;
        r.v = &g.tw.body[static_cast<size_t>(id)];
        r.def = g.tw.bodyCard[static_cast<size_t>(id)];
        r.lo = std::min(-1.0f, r.def); r.hi = std::max(2.0f, r.def);
        return true;
    }
    if (id >= kFaceBase && id < kNippleMm) {
        size_t i = static_cast<size_t>(id - kFaceBase);
        if (i >= g.tw.face.size()) return false;
        r.v = &g.tw.face[i];
        r.def = g.tw.faceCard[i];
        r.lo = std::min(-1.0f, r.def); r.hi = std::max(2.0f, r.def);
        return true;
    }
    if (id == kNippleMm) { r.v = &g.tw.nippleMm; r.def = 0; r.lo = -15; r.hi = 15; r.step = 1; r.percent = false; return true; }
    if (id == kAreola) { r.v = &g.tw.areola; r.def = g.tw.areolaCard; r.lo = 0; r.hi = 1; return true; }
    return false;
}

std::vector<TweakRow> BuildTweakRows(int tab) {
    std::vector<TweakRow> rows;
    auto addSliders = [&](const std::vector<shape::SliderInfo>& list, int base, size_t have) {
        std::string cur;
        for (size_t i = 0; i < list.size() && i < have; ++i) {
            if (cur != list[i].group) { cur = list[i].group; rows.push_back({0, base + static_cast<int>(i), cur, cur}); }
            rows.push_back({1, base + static_cast<int>(i), list[i].label, cur});
        }
    };
    if (tab == TabBody) addSliders(shape::BodySliders(), 0, g.tw.body.size());
    else if (tab == TabFace) addSliders(shape::FaceSliders(), kFaceBase, g.tw.face.size());
    else if (tab == TabLook) {
        rows.push_back({0, kNippleIn, "Nipples", "Nipples"});
        rows.push_back({2, kNippleIn, "Push the nipples into the body", "Nipples"});
        rows.push_back({1, kNippleMm, "Nipple out / in (mm)", "Nipples"});
        rows.push_back({1, kAreola, "Areola size", "Nipples"});
    }
    return rows;
}

bool TweakChanged(int id) {
    SliderRef r;
    if (id == kNippleIn) return g.tw.nippleInside;
    return SliderFor(id, r) && std::fabs(*r.v - r.def) > 1e-4f;
}

bool AnyTweak() {
    for (int i = 0; i < kFaceBase; ++i) if (TweakChanged(i)) return true;
    for (int i = kFaceBase; i < kNippleMm; ++i) if (TweakChanged(i)) return true;
    return TweakChanged(kNippleMm) || TweakChanged(kAreola) || g.tw.nippleInside;
}

void ResetTweak(int id) {
    SliderRef r;
    if (id == kNippleIn) g.tw.nippleInside = false;
    else if (SliderFor(id, r)) *r.v = r.def;
}

void ScheduleApplyTweaks() {
    g.tweakPending = true;
    SetTimer(g.hwnd, kTimerApplyTweaks, 800, nullptr);
    InvalidatePanel();
}

// Long paths and names must not run out of the panel and over the model: keep the start (the drive) and the
// useful tail, or cut the end for hints.
std::string Shorten(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars || maxChars < 12) return s;
    const size_t tail = maxChars - 9;   // "D:/..." + tail
    return s.substr(0, 6) + "..." + s.substr(s.size() - tail);
}
std::string ClipText(const std::string& s, size_t maxChars) {
    return s.size() <= maxChars ? s : s.substr(0, maxChars - 3) + "...";
}

bool EnsureResolver(std::string& err) {
    std::string key = g.settings.library + "|" + g.settings.mods;
    if (g.resolver && key == g.resolverKey) return true;
    if (g.settings.library.empty()) { err = "no asset library: start with --library <dir>"; return false; }
    std::vector<std::string> issues;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.modsInfo.clear();
    }
    if (!g.settings.mods.empty()) {
        Log("indexing mods in " + g.settings.mods + " ...");
        g.mods = resolve::ModIndex::Build(g.settings.mods, ModCachePath(), &issues, nullptr, [](size_t done, size_t total) {
            Log("indexing mods " + std::to_string(done) + " / " + std::to_string(total));
        });
        // A game folder instead of its mods folder: look one level down.
        if (g.mods.guidCount() == 0 && Exists(g.settings.mods + "/mods")) {
            g.settings.mods += "/mods";
            g.mods = resolve::ModIndex::Build(g.settings.mods, ModCachePath(), &issues, nullptr, [](size_t done, size_t total) {
                Log("indexing mods " + std::to_string(done) + " / " + std::to_string(total));
            });
            SaveSettings(g.settings);
        }
        std::lock_guard<std::mutex> lk(g.mu);
        g.modsInfo = std::to_string(g.mods.guidCount()) + " mods found";
        if (g.mods.guidCount() == 0) g.modsInfo = "NO mods found in that folder - press M and pick the folder that holds the .zipmod files";
        Log(g.modsInfo, g.mods.guidCount() ? LogKind::Good : LogKind::Error);
    } else {
        std::lock_guard<std::mutex> lk(g.mu);
        g.modsInfo = "no mods folder set - mod items will be missing (press M)";
        Log(g.modsInfo, LogKind::Warn);
    }
    auto chk = library::CheckLibrary(g.settings.library, false);
    if (!chk.ok) { err = "asset library problem: " + (chk.issues.empty() ? std::string("unusable") : chk.issues[0]); return false; }
    Log("asset library: " + (chk.profile.empty() ? std::string("base") : chk.profile) + " profile", LogKind::Good);
    g.lists = resolve::ListTable();
    g.lists.loadBaseGame(library::AbdataRoot(g.settings.library), &issues);
    g.resolver = std::make_unique<resolve::Resolver>(g.mods, g.lists, library::AbdataRoot(g.settings.library));
    if (exporter::LoadColourGrade(g.settings.library)) Log("colour grading loaded (G toggles it)", LogKind::Good);
    g.resolverKey = key;
    return true;
}

void StartLoad(const std::string& path, int outfit, bool keepTweaks = false) {
    if (g.busy.exchange(true)) { Log("still loading; wait for it to finish", LogKind::Warn); return; }
    g.cardPath = path;
    g.applyKeep = keepTweaks;
    g.keepHidden.clear();
    if (keepTweaks)   // the same card again (another outfit, a re-shape): keep the part checkboxes
        if (std::shared_ptr<Loaded> prev = Current())
            for (size_t i = 0; i < prev->scene.parts.size() && i < g.hidden.size(); ++i) g.keepHidden[prev->scene.parts[i].id] = g.hidden[i];
    g.tweakPending = false;
    KillTimer(g.hwnd, kTimerApplyTweaks);
    g.loadingOutfit = outfit;
    g.status = "loading " + fileio::FileName(path) + " ...";   // the full path is in the log
    Log("open " + fileio::FileName(path));
    InvalidatePanel();
    std::thread([path, outfit, keepTweaks]() {
        auto t0 = std::chrono::steady_clock::now();
        auto out = std::make_shared<Loaded>();
        std::string err;
        try {
            if (!EnsureResolver(err)) throw std::runtime_error(err);
            Log("reading the card container");
            card::Card c = card::LoadCard(path);
            if (!c.supported) throw std::runtime_error(c.supportNote);
            // The card's current coordinate when none is asked for; an
            // out-of-range slot falls back to the first, the way Assembler does.
            int shown = outfit >= 0 ? outfit : static_cast<int>(c.status.at("coordinateType").asInt());
            if (shown < 0 || shown >= static_cast<int>(c.outfits.size())) shown = 0;
            Log("assembling outfit " + std::to_string(shown) + " (" + c.outfitName(shown) + ") ...");
            assemble::AssembleOptions ao;
            ao.outfit = shown;
            {   // the sliders: the card's own values unless the user changed them
                std::lock_guard<std::mutex> lk(g.mu);
                if (!keepTweaks || g.tw.bodyCard.empty()) {
                    g.tw = Tweaks();
                    const Value& bv = c.body.at("shapeValueBody");
                    const Value& fv = c.face.at("shapeValueFace");
                    for (size_t i = 0; i < bv.size(); ++i) g.tw.bodyCard.push_back(static_cast<float>(bv.at(i).asFloat()));
                    for (size_t i = 0; i < fv.size(); ++i) g.tw.faceCard.push_back(static_cast<float>(fv.at(i).asFloat()));
                    g.tw.body = g.tw.bodyCard;
                    g.tw.face = g.tw.faceCard;
                    g.tw.areolaCard = g.tw.areola = static_cast<float>(c.body.at("areolaSize").asFloat());
                }
                ao.bodyShape = g.tw.body;
                ao.faceShape = g.tw.face;
                ao.nippleInside = g.tw.nippleInside;
                ao.nippleOffset = g.tw.nippleMm / 1000.0f;
                if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
            }
            ao.log = [](const std::string& m) { Log("  " + m); };
            out->scene = assemble::Assemble(c, *g.resolver, ao);
            out->outfits = static_cast<int>(c.outfits.size());
            out->outfit = shown;
            out->title = c.displayName();
            out->partTriangles.assign(out->scene.parts.size(), 0);
            for (size_t pi = 0; pi < out->scene.parts.size(); ++pi)
                for (int mi : out->scene.parts[pi].meshes)
                    for (const auto& sub : out->scene.meshes[static_cast<size_t>(mi)].submeshes)
                        out->partTriangles[pi] += sub.indices.size() / 3;
            out->card = std::move(c);
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char msg[256];
            std::snprintf(msg, sizeof msg, "ready in %.1fs: %zu parts, %s triangles", secs, out->scene.parts.size(), Count(Triangles(out->scene)).c_str());
            Log(msg, LogKind::Good);
        } catch (const std::exception& e) {
            out->notes.push_back(e.what());
            Log(std::string("load failed: ") + e.what(), LogKind::Error);
        }
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.loaded = out;
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LOADED, 0, 0);
    }).detach();
}

// ---- export ------------------------------------------------------------------------------------------------------------
std::string PickExportParent(HWND owner) {
    return PickFolder(owner, L"Choose where the exported PMX folder should be created");
}

void StartExport() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.parts.empty()) { Log("nothing to export yet", LogKind::Warn); return; }
    if (g.busy || g.exporting.exchange(true)) { Log("still busy; wait for the current job to finish", LogKind::Warn); return; }
    std::string parent = PickExportParent(g.hwnd);
    if (parent.empty()) { g.exporting = false; return; }
    std::string stem = SafeStem(L->title.empty() ? std::string("character") : L->title);
    std::string outDir = parent + "/" + stem;
    for (int n = 2; Exists(outDir) && n < 1000; ++n) outDir = parent + "/" + stem + " (" + std::to_string(n) + ")";
    const std::string library = g.settings.library, mods = g.settings.mods;
    const std::vector<char> hiddenNow = g.hidden;
    Log("export started: " + outDir, LogKind::Info);
    std::thread([L, outDir, stem, library, mods, hiddenNow]() {
        auto t0 = std::chrono::steady_clock::now();
        try {
            assemble::Scene scene = L->scene;   // bake the copy, not what the viewport is drawing
            exporter::ApplyPartStateForExport(scene, hiddenNow);   // the PMX starts the way the viewer shows it; morphs switch the rest
            Log("baking the game shader textures");
            exporter::BakeShaderTextures(scene, 1024, true, nullptr, [](const std::string& m) { Log("  " + m); });
            exporter::ExportOptions eo;
            eo.outDir = outDir;
            eo.baseName = stem;
            eo.outfit = L->outfit;
            eo.assetLibrary = library;
            eo.modsFolder = mods;
            eo.log = [](const std::string& m) { Log("  " + m); };
            exporter::ExportResult r = exporter::ExportScene(scene, L->card, eo);
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            char msg[512];
            std::snprintf(msg, sizeof msg, "export done in %.1fs: %s (%s vertices, %s triangles, %zu textures)", secs, r.pmxPath.c_str(),
                          Count(r.vertices).c_str(), Count(r.triangles).c_str(), r.textures);
            Log(msg, LogKind::Good);
        } catch (const std::exception& e) {
            Log(std::string("export failed: ") + e.what(), LogKind::Error);
        }
        g.exporting = false;
        PostMessageW(g.hwnd, WM_EXPORT_DONE, 0, 0);
    }).detach();
}

// ---- part list --------------------------------------------------------------------------------------------------------
std::vector<PartRow> BuildRows(const assemble::Scene& scene) {
    std::vector<PartRow> rows;
    static const char* order[] = {"Body", "Head", "Hair", "Outfit", "Accessories"};
    std::set<std::string> seen;
    auto addGroup = [&](const std::string& group) {
        bool any = false;
        for (const auto& p : scene.parts) {
            if (p.group != group || p.underPiece) continue;   // under pieces follow their garments and the Nude body box
            if (!any) { rows.push_back({-1, group}); any = true; }
            rows.push_back({static_cast<int>(&p - scene.parts.data()), group});
        }
    };
    for (const char* group : order) { addGroup(group); seen.insert(group); }
    for (const auto& p : scene.parts)
        if (!seen.count(p.group) && !p.underPiece) { addGroup(p.group); seen.insert(p.group); }
    // Collapsed groups lose their part rows but keep the header.
    std::vector<PartRow> out;
    std::string collapsed;
    for (const auto& row : rows) {
        if (row.part < 0) { collapsed = g.collapsedGroups.count(row.group) ? row.group : std::string(); out.push_back(row); }
        else if (collapsed != row.group) out.push_back(row);
    }
    return out;
}

int VisibleRows(const PanelLayout& l) { return std::max(1, (l.partsBottom - l.partsTop) / kRowH); }

int RowIndexAt(size_t count, const PanelLayout& l, int x, int y) {
    if (x < 0 || x >= kPanel || y < l.partsTop || y >= l.partsBottom) return -1;
    int row = (y - l.partsTop) / kRowH + g.partScroll;
    return row >= 0 && static_cast<size_t>(row) < count ? row : -1;
}
int RowAt(const std::vector<PartRow>& rows, const PanelLayout& l, int x, int y) { return RowIndexAt(rows.size(), l, x, y); }

// A slider row: label on the left, the track, the value on the right.
constexpr int kTrackL = 168, kTrackR = 300;
float SliderValueAt(const SliderRef& r, int x) {
    float t = std::clamp(static_cast<float>(x - kTrackL) / (kTrackR - kTrackL), 0.0f, 1.0f);
    float v = r.lo + t * (r.hi - r.lo);
    return std::clamp(std::round(v / r.step) * r.step, r.lo, r.hi);
}

int ButtonAt(const PanelLayout& l, int x, int y) {
    POINT p{x, y};
    if (PtInRect(&l.open, p)) return 1;
    if (PtInRect(&l.exportPmx, p)) return 2;
    if (PtInRect(&l.issues, p)) return 3;
    if (PtInRect(&l.reset, p)) return 4;
    if (PtInRect(&l.all, p)) return 5;
    for (int i = 0; i < TabCount; ++i) if (PtInRect(&l.tabs[i], p)) return 10 + i;
    return 0;
}

// The outfit slots, as a row of numbered buttons under the name line. Koikatsu
// cards hold seven; anything beyond the row keeps to the arrow keys.
constexpr int kOutfitChipW = 46, kOutfitChipH = 22, kOutfitChipGap = 4;

int OutfitChipsPerRow() {
    return std::max(1, (kPanel - 20 + kOutfitChipGap) / (kOutfitChipW + kOutfitChipGap));
}

RECT OutfitChipRect(const PanelLayout& l, int index) {
    const int perRow = OutfitChipsPerRow();
    const int x = 10 + (index % perRow) * (kOutfitChipW + kOutfitChipGap);
    const int y = l.outfitChipsTop + (index / perRow) * (kOutfitChipH + kOutfitChipGap);
    return RECT{x, y, x + kOutfitChipW, y + kOutfitChipH};
}

int OutfitChipAt(const PanelLayout& l, int x, int y) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return -1;
    const int count = std::min(L->outfits, OutfitChipsPerRow());
    for (int i = 0; i < count; ++i) {
        POINT p{x, y};
        RECT chip = OutfitChipRect(l, i);
        if (PtInRect(&chip, p)) return i;
    }
    return -1;
}

// ---- drawing ---------------------------------------------------------------------------------------------------------
void Render(int w, int h) {
    std::shared_ptr<Loaded> L = Current();
    if (w < 8 || h < 8) return;
    g.fw = w; g.fh = h;
    if (!L || L->scene.parts.empty()) { g.frame.assign(static_cast<size_t>(w) * h, 0xFFE2E4E8u); return; }
    g.hidden.resize(L->scene.parts.size(), 0);
    g.effHidden = exporter::EffectiveHiddenParts(L->scene, g.hidden);
    g.vopt.textured = g.textured;
    g.vopt.highlightPart = g.hoverPart;
    if (g.shaded && g.gpuOk && g.gpuScene == L) {   // Direct3D 11: the game's shaders at full rate
        if (g.gpu.Render(g.cam, w, h, &g.effHidden, g.frame, g.hoverPart)) return;
        g.gpuOk = false;
    }
    // Software shaders are slow: only once the view has been still for a moment.
    const bool still = GetTickCount64() - g.lastInput > 300;
    g.vopt.shaded = g.shaded && still;
    if (g.shaded && !still) SetTimer(g.hwnd, 1, 320, nullptr);   // come back and draw the shaded picture
    g.vopt.hiddenParts = &g.effHidden;
    g.vopt.step = g.interacting ? 3 : 1;
    exporter::RenderView(L->scene, g.cam, w, h, g.vopt, *g.state, g.frame);
}

void DrawTextAt(HDC dc, HFONT font, COLORREF col, int x, int y, const std::string& s, const RECT* clip = nullptr) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = Utf8ToWide(s);
    if (clip) ExtTextOutW(dc, x, y, ETO_CLIPPED, clip, w.c_str(), static_cast<UINT>(w.size()), nullptr);
    else TextOutW(dc, x, y, w.c_str(), static_cast<int>(w.size()));
    SelectObject(dc, old);
}

void DrawTextRight(HDC dc, HFONT font, COLORREF col, int right, int y, const std::string& s, const RECT* clip) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = Utf8ToWide(s);
    SetTextAlign(dc, TA_RIGHT | TA_TOP);
    if (clip) ExtTextOutW(dc, right, y, ETO_CLIPPED, clip, w.c_str(), static_cast<UINT>(w.size()), nullptr);
    else TextOutW(dc, right, y, w.c_str(), static_cast<int>(w.size()));
    SetTextAlign(dc, TA_LEFT | TA_TOP);
    SelectObject(dc, old);
}

COLORREF KindColor(LogKind k) {
    switch (k) {
    case LogKind::Good: return RGB(120, 210, 140);
    case LogKind::Warn: return RGB(235, 190, 90);
    case LogKind::Error: return RGB(240, 110, 110);
    default: return RGB(170, 180, 196);
    }
}

void Button(HDC dc, const RECT& r, const std::string& label, bool hot, bool on, bool enabled) {
    HBRUSH fill = CreateSolidBrush(hot && enabled ? RGB(52, 66, 88) : on ? RGB(44, 56, 74) : RGB(36, 40, 50));
    FillRect(dc, &r, fill);
    DeleteObject(fill);
    HBRUSH edge = CreateSolidBrush(on ? RGB(90, 160, 230) : RGB(58, 64, 78));
    FrameRect(dc, &r, edge);
    DeleteObject(edge);
    RECT t = r;
    HGDIOBJ old = SelectObject(dc, g.fontUi);
    SetTextColor(dc, enabled ? (on ? RGB(210, 232, 255) : RGB(214, 220, 232)) : RGB(90, 95, 105));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, Utf8ToWide(label).c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void DrawPanel(HDC dc, const RECT& rc, const RECT& paint) {
    std::shared_ptr<Loaded> L = Current();
    std::string modsInfo;
    { std::lock_guard<std::mutex> lk(g.mu); modsInfo = g.modsInfo; }
    PanelLayout l = LayoutFor(rc.bottom);

    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    RECT all{0, 0, kPanel, rc.bottom};
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    // Belt and braces: nothing the panel draws may cross into the viewport, whatever the path or name length.
    int savedDc = SaveDC(dc);
    if (savedDc) IntersectClipRect(dc, 0, 0, kPanel, rc.bottom);

    // header -----------------------------------------------------------------------------------------------------------
    std::string title = L ? L->title : std::string("CardBridge viewer");
    if (title.empty()) title = "CardBridge viewer";
    DrawTextAt(dc, g.fontTitle, RGB(238, 241, 248), 10, 7, ClipText(title, 38));
    if (L && !L->scene.parts.empty()) {
        size_t listed = 0;
        for (const auto& p : L->scene.parts) listed += !p.underPiece;
        std::string stats = Count(Triangles(L->scene)) + " triangles, " + std::to_string(L->scene.materials.size()) + " materials, " +
                            std::to_string(listed) + " parts";
        DrawTextAt(dc, g.fontUi, RGB(150, 160, 178), 10, 30, stats);
        size_t warn = 0, err = 0;
        for (const auto& i : L->scene.issues) (i.severity == "error" ? err : warn)++;
        DrawTextAt(dc, g.fontUi, warn + err ? RGB(235, 190, 90) : RGB(120, 200, 120), 10, 48,
                   std::to_string(err) + " errors, " + std::to_string(warn) + " warnings   [I]");
        std::string mode = g.gpuOk ? "Direct3D 11 shaders" : "software shaders";
        mode += g.textured ? "  ·  textured" : "  ·  untextured";
        DrawTextAt(dc, g.fontUi, g.gpuOk ? RGB(110, 180, 240) : RGB(150, 160, 178), 10, 66, mode);
    } else if (L) {
        // The assembly threw (unsupported card, broken container): show why instead of a row of zeros.
        DrawTextAt(dc, g.fontBold, RGB(240, 110, 110), 10, 34, "could not assemble this card");
        RECT noteClip{10, 54, kPanel - 10, 74};
        if (!L->notes.empty()) DrawTextAt(dc, g.fontUi, RGB(220, 180, 180), 10, 54, ClipText(L->notes[0], 54), &noteClip);
        DrawTextAt(dc, g.fontUi, RGB(140, 150, 168), 10, 76, "details in the log below  ·  O opens another card");
    } else {
        DrawTextAt(dc, g.fontUi, RGB(190, 200, 215), 10, 34, "drop a card .png onto this window, or press O");
        DrawTextAt(dc, g.fontUi, g.busy ? RGB(120, 200, 255) : RGB(140, 150, 168), 10, 54, ClipText(g.status, 54));
        DrawTextAt(dc, g.fontUi, RGB(120, 130, 150), 10, 72,
                   "library: " + (g.settings.library.empty() ? std::string("(not set)") : Shorten(g.settings.library, 42)));
    }

    Button(dc, l.open, "Open", g.hoverButton == 1, false, true);
    Button(dc, l.exportPmx, g.exporting ? "Exporting..." : "Export PMX", g.hoverButton == 2, false, L != nullptr && !g.busy && !g.exporting);
    Button(dc, l.issues, "Issues", g.hoverButton == 3, g.showIssues, L != nullptr);
    Button(dc, l.reset, "Reset", g.hoverButton == 4, false, L != nullptr);
    Button(dc, l.all, "All", g.hoverButton == 5, false, L != nullptr);

    HBRUSH div = CreateSolidBrush(RGB(42, 46, 56));
    RECT d1{10, 120, kPanel - 10, 121};
    FillRect(dc, &d1, div);
    DeleteObject(div);

    // outfit ------------------------------------------------------------------------------------------------------------
    // The counter and the game's name for the coordinate, over a row of slots
    // that can be clicked - Left/Right still steps through them.
    {
        int shown = L ? L->outfit : -1;
        if (g.busy && g.loadingOutfit >= 0) shown = g.loadingOutfit;
        if (L && L->outfits > 0) shown = std::clamp(shown, 0, L->outfits - 1);
        DrawTextAt(dc, g.fontBold, RGB(120, 180, 240), 10, l.outfitNameTop, "OUTFIT");
        if (L && L->outfits > 0) {
            // Hovering a slot previews that outfit's name without loading it.
            const int preview = g.hoverOutfit >= 0 && g.hoverOutfit < L->outfits ? g.hoverOutfit : shown;
            const COLORREF nameColor = g.busy ? RGB(120, 200, 255)
                                              : g.hoverOutfit >= 0 ? RGB(238, 244, 255) : RGB(206, 218, 236);
            DrawTextAt(dc, g.fontBold, nameColor, 72, l.outfitNameTop, ClipText(L->card.outfitName(preview), 30));
            char count[32];
            std::snprintf(count, sizeof count, "%d / %d", shown + 1, L->outfits);
            DrawTextRight(dc, g.fontBold, RGB(235, 190, 90), kPanel - 10, l.outfitNameTop, count, nullptr);
        } else {
            DrawTextAt(dc, g.fontUi, RGB(110, 120, 138), 72, l.outfitNameTop, g.busy ? "loading ..." : "no card");
        }
        const int chips = L ? std::min(L->outfits, OutfitChipsPerRow()) : 0;
        for (int i = 0; i < chips; ++i) {
            char number[8];
            std::snprintf(number, sizeof number, "%d", i + 1);
            Button(dc, OutfitChipRect(l, i), number, g.hoverOutfit == i, i == shown, L != nullptr && !g.busy);
        }
    }

    // parts / issues ---------------------------------------------------------------------------------------------------
    RECT clip{10, l.partsTop, kPanel - 10, l.partsBottom};
    for (int i = 0; i < TabCount; ++i)
        Button(dc, l.tabs[i], kTabNames[i], g.hoverButton == 10 + i, !g.showIssues && g.tab == i, L != nullptr);
    if (L) {
        std::string right;
        if (g.showIssues) right = std::to_string(L->scene.issues.size()) + " finding(s)";
        else if (g.busy && g.tab != TabParts) right = "re-shaping ...";
        else if (g.tweakPending) right = "applying ...";
        else if (g.tab == TabParts) right = std::to_string(L->scene.parts.size()) + " parts";
        else right = AnyTweak() ? "tweaked" : "as the card says";
        DrawTextRight(dc, g.fontSmall, g.busy || g.tweakPending ? RGB(120, 200, 255) : RGB(110, 120, 138), kPanel - 10, l.partsHeadingTop + 2, right, nullptr);
    }
    if (!L) {
        DrawTextAt(dc, g.fontUi, g.busy ? RGB(120, 200, 255) : RGB(120, 130, 150), 10, l.partsTop + 4,
                   g.busy ? "loading the card ..." : "no card loaded");
    } else if (g.showIssues) {
        std::vector<std::pair<std::string, LogKind>> lines;
        for (const auto& i : L->scene.issues) {
            std::string t = (i.part.empty() ? "" : "[" + i.part + "] ") + i.message;
            LogKind k = i.severity == "error" ? LogKind::Error : LogKind::Warn;
            while (t.size() > 50) {
                size_t cut = t.rfind(' ', 50);
                if (cut == std::string::npos || cut < 20) cut = 50;
                lines.push_back({t.substr(0, cut), k});
                t = t.substr(cut + (t[cut] == ' ' ? 1 : 0));
            }
            lines.push_back({t, k});
        }
        int visible = std::max(1, (l.partsBottom - l.partsTop) / 16);
        g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(lines.size()) - visible));
        for (int i = 0; i < visible; ++i) {
            int idx = g.partScroll + i;
            if (idx >= static_cast<int>(lines.size())) break;
            DrawTextAt(dc, g.fontLog, KindColor(lines[static_cast<size_t>(idx)].second), 12, l.partsTop + i * 16,
                       lines[static_cast<size_t>(idx)].first, &clip);
        }
    } else if (g.tab != TabParts) {
        std::vector<TweakRow> rows = BuildTweakRows(g.tab);
        int visible = VisibleRows(l);
        g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
        for (int i = 0; i < visible; ++i) {
            int idx = g.partScroll + i;
            if (idx >= static_cast<int>(rows.size())) break;
            const TweakRow& row = rows[static_cast<size_t>(idx)];
            int y = l.partsTop + i * kRowH;
            if (idx == g.hoverRow) {
                HBRUSH hb = CreateSolidBrush(row.kind == 0 ? RGB(32, 36, 45) : RGB(36, 44, 58));
                RECT hr{0, y, kPanel, y + kRowH};
                FillRect(dc, &hr, hb);
                DeleteObject(hb);
            }
            if (row.kind == 0) {
                std::string name = row.label;
                for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                DrawTextAt(dc, g.fontBold, RGB(120, 180, 240), 12, y + 3, name);
                bool any = false;
                for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) any = any || TweakChanged(rows[k].id);
                DrawTextRight(dc, g.fontSmall, any ? RGB(235, 190, 90) : RGB(80, 88, 104), kPanel - 12, y + 4, "reset", &clip);
            } else if (row.kind == 2) {
                bool on = TweakChanged(row.id);
                DrawTextAt(dc, g.fontMono, on ? RGB(235, 190, 90) : RGB(206, 218, 236), 12, y + 3, on ? "[x]" : "[ ]");
                DrawTextAt(dc, g.fontUi, on ? RGB(235, 190, 90) : RGB(206, 218, 236), 46, y + 2, row.label, &clip);
            } else {
                SliderRef r;
                if (!SliderFor(row.id, r)) continue;
                bool changed = TweakChanged(row.id);
                RECT labelClip{12, y, kTrackL - 6, y + kRowH};
                DrawTextAt(dc, g.fontUi, changed ? RGB(235, 190, 90) : RGB(206, 218, 236), 12, y + 2, row.label, &labelClip);
                const int mid = y + kRowH / 2;
                HBRUSH tr = CreateSolidBrush(RGB(48, 54, 68));
                RECT track{kTrackL, mid - 1, kTrackR, mid + 2};
                FillRect(dc, &track, tr);
                DeleteObject(tr);
                auto xOf = [&](float v) { return kTrackL + static_cast<int>(std::clamp((v - r.lo) / (r.hi - r.lo), 0.0f, 1.0f) * (kTrackR - kTrackL)); };
                HBRUSH dm = CreateSolidBrush(RGB(110, 120, 140));   // where the card has it
                RECT dr{xOf(r.def) - 1, mid - 5, xOf(r.def) + 2, mid + 6};
                FillRect(dc, &dr, dm);
                DeleteObject(dm);
                HBRUSH th = CreateSolidBrush(changed ? RGB(235, 190, 90) : RGB(90, 170, 240));
                RECT tb{xOf(*r.v) - 3, mid - 6, xOf(*r.v) + 4, mid + 7};
                FillRect(dc, &tb, th);
                DeleteObject(th);
                char val[24];
                if (r.percent) std::snprintf(val, sizeof val, "%d", static_cast<int>(std::lround(*r.v * 100)));
                else std::snprintf(val, sizeof val, "%+d", static_cast<int>(std::lround(*r.v)));
                DrawTextRight(dc, g.fontMono, changed ? RGB(235, 190, 90) : RGB(150, 160, 178), kPanel - 12, y + 3, val, nullptr);
            }
        }
        if (static_cast<int>(rows.size()) > visible) {
            int trackTop = l.partsTop, trackH = l.partsBottom - l.partsTop;
            int thumbH = std::max(18, trackH * visible / static_cast<int>(rows.size()));
            int span = std::max(1, static_cast<int>(rows.size()) - visible);
            int thumbY = trackTop + (trackH - thumbH) * g.partScroll / span;
            HBRUSH th = CreateSolidBrush(RGB(90, 100, 120));
            RECT rr{kPanel - 5, thumbY, kPanel - 2, thumbY + thumbH};
            FillRect(dc, &rr, th);
            DeleteObject(th);
        }
    } else {
        std::vector<PartRow> rows = BuildRows(L->scene);
        int visible = VisibleRows(l);
        g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
        for (int i = 0; i < visible; ++i) {
            int idx = g.partScroll + i;
            if (idx >= static_cast<int>(rows.size())) break;
            const PartRow& row = rows[static_cast<size_t>(idx)];
            int y = l.partsTop + i * kRowH;
            bool hot = idx == g.hoverRow;
            if (row.part < 0) {
                if (hot) {
                    HBRUSH hb = CreateSolidBrush(RGB(32, 36, 45));
                    RECT hr{0, y, kPanel, y + kRowH};
                    FillRect(dc, &hr, hb);
                    DeleteObject(hb);
                }
                std::string name = row.group;
                for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                bool collapsed = g.collapsedGroups.count(row.group) != 0;
                int count = 0;
                for (const auto& p : L->scene.parts) if (p.group == row.group && !p.underPiece) ++count;
                DrawTextAt(dc, g.fontBold, RGB(120, 180, 240), 12, y + 3, (collapsed ? "+ " : "- ") + name);
                DrawTextRight(dc, g.fontSmall, RGB(100, 110, 128), kPanel - 12, y + 4, std::to_string(count), &clip);
            } else {
                const auto& p = L->scene.parts[static_cast<size_t>(row.part)];
                bool off = static_cast<size_t>(row.part) < g.hidden.size() && g.hidden[static_cast<size_t>(row.part)];
                bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
                if (hot) {
                    HBRUSH hb = CreateSolidBrush(RGB(45, 58, 78));
                    RECT hr{0, y, kPanel, y + kRowH};
                    FillRect(dc, &hr, hb);
                    DeleteObject(hb);
                    HBRUSH bar = CreateSolidBrush(RGB(90, 170, 240));
                    RECT br{0, y, 3, y + kRowH};
                    FillRect(dc, &br, bar);
                    DeleteObject(bar);
                }
                COLORREF col = off ? RGB(95, 100, 112) : !ok ? RGB(235, 110, 110) : RGB(206, 218, 236);
                DrawTextAt(dc, g.fontMono, col, 12, y + 3, off ? "[ ]" : "[x]");
                std::string name = p.nudeSwitch ? std::string("nude  (whole body, under clothes too)") : p.id;
                if (!ok) name += "  (" + p.status + ")";
                DrawTextAt(dc, g.fontMono, col, 46, y + 3, name, &clip);
                std::string tris = Count(static_cast<size_t>(row.part) < L->partTriangles.size() ? L->partTriangles[static_cast<size_t>(row.part)] : 0);
                DrawTextRight(dc, g.fontSmall, off ? RGB(85, 90, 100) : RGB(110, 120, 138), kPanel - 12, y + 4, tris, &clip);
            }
        }
        // a slim scrollbar, when there is more than fits
        if (static_cast<int>(rows.size()) > visible) {
            int trackTop = l.partsTop, trackH = l.partsBottom - l.partsTop;
            int thumbH = std::max(18, trackH * visible / static_cast<int>(rows.size()));
            int span = std::max(1, static_cast<int>(rows.size()) - visible);
            int thumbY = trackTop + (trackH - thumbH) * g.partScroll / span;
            HBRUSH tb = CreateSolidBrush(RGB(42, 46, 56));
            RECT tr{kPanel - 5, trackTop, kPanel - 2, trackTop + trackH};
            FillRect(dc, &tr, tb);
            DeleteObject(tb);
            HBRUSH th = CreateSolidBrush(RGB(90, 100, 120));
            RECT rr{kPanel - 5, thumbY, kPanel - 2, thumbY + thumbH};
            FillRect(dc, &rr, th);
            DeleteObject(th);
        }
    }

    // log --------------------------------------------------------------------------------------------------------------
    HBRUSH logBg = CreateSolidBrush(RGB(17, 19, 24));
    RECT logRect{0, l.logTop, kPanel, l.logBottom};
    FillRect(dc, &logRect, logBg);
    DeleteObject(logBg);
    HBRUSH logEdge = CreateSolidBrush(RGB(42, 46, 56));
    RECT le{0, l.logTop, kPanel, l.logTop + 1};
    FillRect(dc, &le, logEdge);
    DeleteObject(logEdge);
    DrawTextAt(dc, g.fontBold, RGB(120, 180, 240), 10, l.logTop + 5, "LOG");
    DrawTextRight(dc, g.fontSmall, RGB(100, 110, 128), kPanel - 10, l.logTop + 6, "clear", nullptr);
    {
        std::lock_guard<std::mutex> lk(g.logMu);
        int lineH = 15;
        int top = l.logTop + 24;
        int visible = std::max(1, (l.logBottom - top) / lineH);
        g.logScroll = std::max(0, std::min(g.logScroll, static_cast<int>(g.logLines.size()) - visible));
        int end = static_cast<int>(g.logLines.size()) - g.logScroll;
        int start = std::max(0, end - visible);
        RECT lc{8, top, kPanel - 8, l.logBottom};
        if (g.logLines.empty()) DrawTextAt(dc, g.fontLog, RGB(90, 96, 110), 12, top, "no activity yet", &lc);
        for (int i = start; i < end; ++i) {
            const LogLine& line = g.logLines[static_cast<size_t>(i)];
            int y = top + (i - start) * lineH;
            DrawTextAt(dc, g.fontLog, RGB(90, 96, 110), 10, y, line.time, &lc);
            DrawTextAt(dc, g.fontLog, KindColor(line.kind), 76, y, line.text, &lc);
        }
    }

    // footer -----------------------------------------------------------------------------------------------------------
    HBRUSH foot = CreateSolidBrush(RGB(21, 23, 29));
    RECT fr{0, l.footerTop, kPanel, rc.bottom};
    FillRect(dc, &fr, foot);
    DeleteObject(foot);
    DrawTextAt(dc, g.fontSmall, RGB(110, 120, 138), 10, l.footerTop + 3, ClipText("E export  O open  I issues  A all  T tex  H shader  R reset", 64));
    std::string hint = "1-7 outfit · L library · M mods · C clear log · drop a card";
    if (g.busy) hint = "working ...";
    else if (g.exporting) hint = "exporting PMX ...";
    if (L && g.hoverPart >= 0 && static_cast<size_t>(g.hoverPart) < L->scene.parts.size()) {
        const auto& p = L->scene.parts[static_cast<size_t>(g.hoverPart)];
        hint = p.id + "  ·  " + p.status;
        if (static_cast<size_t>(g.hoverPart) < L->partTriangles.size()) hint += "  ·  " + Count(L->partTriangles[static_cast<size_t>(g.hoverPart)]) + " tris";
        if (!p.modName.empty()) hint += "  ·  " + p.modName;
        if (g.hidden[static_cast<size_t>(g.hoverPart)]) hint += "  ·  hidden";
    } else if (!modsInfo.empty() && !L) {
        hint = modsInfo;
    }
    DrawTextAt(dc, g.fontSmall, g.busy ? RGB(120, 200, 255) : g.exporting ? RGB(240, 170, 90) : RGB(120, 130, 148), 10,
               l.footerTop + 16, ClipText(hint, 64));
    if (savedDc) RestoreDC(dc, savedDc);
    (void)paint;
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = std::max<int>(0, static_cast<int>(rc.right) - kPanel), H = rc.bottom;
    if (g.dirty || g.fw != W || g.fh != H) {
        Render(W, H);
        g.dirty = false;
    }
    RECT viewport{kPanel, 0, rc.right, rc.bottom};
    RECT both{};
    bool viewportPainted = IntersectRect(&both, &ps.rcPaint, &viewport) != 0 || ps.rcPaint.right == 0;
    if (viewportPainted && !g.frame.empty() && g.fw == W && g.fh == H && W > 0 && H > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, kPanel, 0, W, H, 0, 0, W, H, g.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    DrawPanel(dc, rc, ps.rcPaint);
    EndPaint(hwnd, &ps);
}

void ResetView(float yaw) { g.cam = {}; g.cam.yawDeg = yaw; g.dirty = true; InvalidateRect(g.hwnd, nullptr, FALSE); }

void ShowAll() {
    g.hidden.assign(g.hidden.size(), 0);
    g.soloPart = -1;
    g.dirty = true;
    Log("showing every part");
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void ToggleIssues() {
    g.showIssues = !g.showIssues;
    g.partScroll = 0;
    g.hoverRow = g.hoverPart = -1;
    InvalidatePanel();
}

void ExportFolderIfNeeded() {
    // kept separate so the WM_KEYDOWN / button paths read the same
    StartExport();
}

void OpenCard() {
    std::string c = PickCard(g.hwnd);
    if (!c.empty()) { g.cam = {}; StartLoad(c, -1); }
}

// Picking a slot is the same reload Left/Right does, just aimed.
void ChooseOutfit(int index) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.busy || g.cardPath.empty()) return;
    if (index < 0 || index >= L->outfits || index == L->outfit) return;
    StartLoad(g.cardPath, index, true);
}

// A click in the sliders list: a header resets its group, a switch toggles, a slider starts a drag; right-click resets the row.
void TweakClick(int x, int y, const PanelLayout& l, bool right) {
    std::vector<TweakRow> rows = BuildTweakRows(g.tab);
    int idx = RowIndexAt(rows.size(), l, x, y);
    if (idx < 0) return;
    const TweakRow& row = rows[static_cast<size_t>(idx)];
    std::lock_guard<std::mutex> lk(g.mu);
    if (row.kind == 0) {
        bool any = false;
        for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) { any = any || TweakChanged(rows[k].id); ResetTweak(rows[k].id); }
        if (any) { Log("reset " + row.label); ScheduleApplyTweaks(); }
        return;
    }
    if (row.kind == 2) {
        if (right) ResetTweak(row.id); else g.tw.nippleInside = !g.tw.nippleInside;
        Log(row.label + ": " + (g.tw.nippleInside ? "on" : "off"));
        ScheduleApplyTweaks();
        return;
    }
    SliderRef r;
    if (!SliderFor(row.id, r)) return;
    if (right) {
        if (TweakChanged(row.id)) { ResetTweak(row.id); Log("reset " + row.label); ScheduleApplyTweaks(); }
        return;
    }
    if (x < kTrackL - 8) return;
    g.sliderDrag = row.id;
    *r.v = SliderValueAt(r, x);
    SetCapture(g.hwnd);
    InvalidatePanel();
}

void ChooseMods() {
    std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu mods folder");
    if (d.empty()) return;
    g.settings.mods = d;
    SaveSettings(g.settings);
    Log("mods folder: " + d, LogKind::Good);
    if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
    else InvalidatePanel();
}

void ChooseLibrary() {
    std::string d = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder");
    if (d.empty()) return;
    g.settings.library = d;
    SaveSettings(g.settings);
    Log("asset library: " + d, LogKind::Good);
    if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
    else InvalidatePanel();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == kTimerApplyTweaks) {
            KillTimer(hwnd, kTimerApplyTweaks);
            if (g.sliderDrag >= 0) { ScheduleApplyTweaks(); return 0; }   // still dragging
            std::shared_ptr<Loaded> L = Current();
            if (g.busy) { ScheduleApplyTweaks(); return 0; }
            if (L && !g.cardPath.empty()) StartLoad(g.cardPath, L->outfit, true);
            return 0;
        }
        KillTimer(hwnd, 1); g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LOG: InvalidatePanel(); return 0;
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {kPanel + 380, 540};
        return 0;
    }
    case WM_EXPORT_DONE: InvalidatePanel(); return 0;
    case WM_LOADED: {
        std::shared_ptr<Loaded> L = Current();
        g.hidden.assign(L ? L->scene.parts.size() : 0, 0);
        if (L && g.applyKeep)
            for (size_t i = 0; i < L->scene.parts.size(); ++i) {
                auto it = g.keepHidden.find(L->scene.parts[i].id);
                if (it != g.keepHidden.end()) g.hidden[i] = it->second;
            }
        if (L)
            for (size_t i = 0; i < L->scene.parts.size(); ++i)
                if (L->scene.parts[i].nudeSwitch && !(g.applyKeep && g.keepHidden.count(L->scene.parts[i].id))) g.hidden[i] = 1;   // starts unticked
        g.keepHidden.clear();
        g.partScroll = 0;
        g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
        g.loadingOutfit = -1;
        g.soloPart = -1;
        g.state = exporter::NewViewState();
        g.gpuOk = false;
        g.gpuScene.reset();
        if (L && g.gpu.ready()) {
            if (g.gpu.Load(L->scene)) {
                g.gpuScene = L;
                g.gpuOk = true;
                Log("game shaders uploaded to Direct3D 11", LogKind::Good);
            } else {
                Log("Direct3D 11 upload failed; using the software renderer", LogKind::Warn);
            }
        } else if (L) {
            Log("no Direct3D 11 device; using the software renderer", LogKind::Warn);
        }
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
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = LayoutFor(rc.bottom);
            int button = ButtonAt(l, x, y);
            if (button == 1) { OpenCard(); return 0; }
            if (button == 2) { ExportFolderIfNeeded(); return 0; }
            if (button == 3) { if (Current()) ToggleIssues(); return 0; }
            if (button == 4) { if (Current()) ResetView(0); return 0; }
            if (button == 5) { if (Current()) ShowAll(); return 0; }
            if (button >= 10) {
                g.showIssues = false;
                g.tab = button - 10;
                g.partScroll = 0;
                g.hoverRow = g.hoverPart = -1;
                InvalidatePanel();
                return 0;
            }
            int chip = OutfitChipAt(l, x, y);
            if (chip >= 0) { ChooseOutfit(chip); return 0; }
            POINT p{x, y};
            if (PtInRect(&l.clearLog, p)) { ClearLog(); return 0; }
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, false); return 0; }
            if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(L->scene);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0) {
                    const PartRow& row = rows[static_cast<size_t>(idx)];
                    if (row.part < 0) {
                        if (g.collapsedGroups.count(row.group)) g.collapsedGroups.erase(row.group);
                        else g.collapsedGroups.insert(row.group);
                        g.hoverRow = -1;
                        g.hoverPart = -1;
                        InvalidatePanel();
                    } else if (static_cast<size_t>(row.part) < g.hidden.size()) {
                        g.hidden[static_cast<size_t>(row.part)] ^= 1;
                        g.soloPart = -1;
                        Log(std::string(g.hidden[static_cast<size_t>(row.part)] ? "hide " : "show ") + L->scene.parts[static_cast<size_t>(row.part)].id);
                        g.dirty = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
            }
            return 0;
        }
        g.drag = 1; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = LayoutFor(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, true); return 0; }
            if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(L->scene);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0 && rows[static_cast<size_t>(idx)].part >= 0) {
                    int part = rows[static_cast<size_t>(idx)].part;
                    if (g.soloPart == part) {
                        g.hidden.assign(g.hidden.size(), 0);
                        g.soloPart = -1;
                        Log("showing every part");
                    } else {
                        g.hidden.assign(g.hidden.size(), 1);
                        if (static_cast<size_t>(part) < g.hidden.size()) g.hidden[static_cast<size_t>(part)] = 0;
                        g.soloPart = part;
                        Log("solo: " + L->scene.parts[static_cast<size_t>(part)].id);
                    }
                    g.dirty = true;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        g.drag = 2; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_MBUTTONDOWN:
        g.drag = 2; g.last = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; g.interacting = true; SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (g.sliderDrag >= 0) {
            SliderRef r;
            if (SliderFor(g.sliderDrag, r)) { std::lock_guard<std::mutex> lk(g.mu); *r.v = SliderValueAt(r, x); }
            InvalidatePanel();
            return 0;
        }
        if (g.drag) {
            float dx = static_cast<float>(x - g.last.x), dy = static_cast<float>(y - g.last.y);
            if (g.drag == 1) { g.cam.yawDeg -= dx * 0.5f; g.cam.pitchDeg = std::max(-89.0f, std::min(89.0f, g.cam.pitchDeg - dy * 0.5f)); }
            else { RECT rc; GetClientRect(hwnd, &rc); g.cam.panX += dx / rc.bottom; g.cam.panY += dy / rc.bottom; }
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PanelLayout l = LayoutFor(rc.bottom);
        int oldRow = g.hoverRow, oldPart = g.hoverPart, oldButton = g.hoverButton, oldOutfit = g.hoverOutfit;
        g.hoverButton = x < kPanel ? ButtonAt(l, x, y) : 0;
        g.hoverOutfit = x < kPanel ? OutfitChipAt(l, x, y) : -1;
        g.hoverRow = -1;
        g.hoverPart = -1;
        if (x < kPanel) {
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab != TabParts) {
                g.hoverRow = RowIndexAt(BuildTweakRows(g.tab).size(), l, x, y);
            } else if (L && !g.showIssues) {
                std::vector<PartRow> rows = BuildRows(L->scene);
                int idx = RowAt(rows, l, x, y);
                if (idx >= 0) {
                    g.hoverRow = idx;
                    if (rows[static_cast<size_t>(idx)].part >= 0) g.hoverPart = rows[static_cast<size_t>(idx)].part;
                }
            }
        }
        if (oldRow != g.hoverRow || oldPart != g.hoverPart || oldButton != g.hoverButton || oldOutfit != g.hoverOutfit) {
            if (oldPart != g.hoverPart) {
                g.lastInput = GetTickCount64();
                g.dirty = true;
                RECT vp{kPanel, 0, rc.right, rc.bottom};
                InvalidateRect(hwnd, &vp, FALSE);
            }
            InvalidatePanel();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.hoverRow >= 0 || g.hoverPart >= 0 || g.hoverButton || g.hoverOutfit >= 0) {
            bool part = g.hoverPart >= 0;
            g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
            g.hoverButton = 0;
            if (part) { g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); }
            InvalidatePanel();
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (g.sliderDrag >= 0) {
            SliderRef r;
            if (SliderFor(g.sliderDrag, r)) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "slider %d set to %.3f (card %.3f)", g.sliderDrag, *r.v, r.def);
                Log(msg);
            }
            g.sliderDrag = -1;
            ReleaseCapture();
            ScheduleApplyTweaks();
            return 0;
        }
        g.drag = 0; g.interacting = false; g.lastInput = GetTickCount64(); ReleaseCapture();
        g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = LayoutFor(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (pt.y >= l.logTop) {
                std::lock_guard<std::mutex> lk(g.logMu);   // Log() touches logScroll from worker threads
                int visible = std::max(1, (l.logBottom - (l.logTop + 24)) / 15);
                int maxScroll = std::max(0, static_cast<int>(g.logLines.size()) - visible);
                g.logScroll = std::max(0, std::min(maxScroll, g.logScroll + (d > 0 ? 3 : -3)));
            } else if (L) {
                if (g.showIssues) {
                    g.partScroll = std::max(0, g.partScroll - (d > 0 ? 3 : -3));
                } else if (g.tab != TabParts) {
                    int maxScroll = std::max(0, static_cast<int>(BuildTweakRows(g.tab).size()) - VisibleRows(l));
                    g.partScroll = std::max(0, std::min(maxScroll, g.partScroll - (d > 0 ? 3 : -3)));
                } else {
                    std::vector<PartRow> rows = BuildRows(L->scene);
                    int maxScroll = std::max(0, static_cast<int>(rows.size()) - VisibleRows(l));
                    g.partScroll = std::max(0, std::min(maxScroll, g.partScroll - (d > 0 ? 3 : -3)));
                }
            }
        } else {
            g.cam.zoom = std::max(0.2f, std::min(20.0f, g.cam.zoom * (d > 0 ? 1.12f : 1 / 1.12f)));
            g.lastInput = GetTickCount64();
        }
        g.dirty = pt.x >= kPanel;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = LayoutFor(rc.bottom);
            bool hand = ButtonAt(l, p.x, p.y) != 0 || PtInRect(&l.clearLog, p) != 0 ||
                        OutfitChipAt(l, p.x, p.y) >= 0;
            if (p.x < kPanel && !hand) {
                std::shared_ptr<Loaded> L = Current();
                if (L && !g.showIssues && g.tab != TabParts) {
                    hand = RowIndexAt(BuildTweakRows(g.tab).size(), l, p.x, p.y) >= 0;
                } else if (L && !g.showIssues) {
                    std::vector<PartRow> rows = BuildRows(L->scene);
                    hand = RowAt(rows, l, p.x, p.y) >= 0;
                }
            }
            SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_KEYDOWN: {
        std::shared_ptr<Loaded> L = Current();
        if (wp == 'T') { g.textured = !g.textured; g.dirty = true; }
        else if (wp == 'H') { g.shaded = !g.shaded; g.dirty = true; }
        else if (wp == 'G') { exporter::SetColourGradeEnabled(!exporter::ColourGradeEnabled()); g.dirty = true; }
        else if (wp == 'R') ResetView(0);
        else if (wp == 'F') ResetView(0);
        else if (wp == 'S') ResetView(90);
        else if (wp == 'B') ResetView(180);
        else if (wp == 'I') { if (L) ToggleIssues(); }
        else if (wp == 'A') { if (L) ShowAll(); }
        else if (wp == 'C') ClearLog();
        else if (wp == 'E') ExportFolderIfNeeded();
        else if (wp == 'O') OpenCard();
        else if (wp == 'M') ChooseMods();
        else if (wp == 'L') ChooseLibrary();
        else if (wp >= '1' && wp <= '9') { if (L) ChooseOutfit(static_cast<int>(wp - '1')); }
        else if ((wp == VK_LEFT || wp == VK_RIGHT) && L && L->outfits > 1) {
            int n = L->outfits, o = (L->outfit + (wp == VK_RIGHT ? 1 : n - 1)) % n;
            ChooseOutfit(o);
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
                             CW_USEDEFAULT, 1280, 820, nullptr, nullptr, inst, nullptr);
    ShowWindow(g.hwnd, show);
    g.fontTitle = CreateFontW(-18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontUi = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g.fontLog = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g.fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    std::string gpuWhy;
    if (g.gpu.Init(&gpuWhy)) Log("Direct3D 11 ready", LogKind::Good);
    else Log("Direct3D 11 unavailable (" + gpuWhy + "); software renderer", LogKind::Warn);
    CoInitialize(nullptr);
    if (g.settings.library.empty() || !Exists(g.settings.library)) g.settings.library = FindLibrary();
    if (g.settings.library.empty()) g.settings.library = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder (made by 'cardbridge library import')");
    if (g.settings.mods.empty() || !Exists(g.settings.mods)) g.settings.mods = Exists(CachedModsRoot()) ? CachedModsRoot() : std::string();
    if (g.settings.mods.empty()) {
        // Optional: without mods only base-game items resolve.
        std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu mods folder (Cancel to use base-game items only)");
        if (!d.empty()) g.settings.mods = d;
    }
    SaveSettings(g.settings);
    Log("library: " + (g.settings.library.empty() ? std::string("(not set)") : g.settings.library));
    Log("mods: " + (g.settings.mods.empty() ? std::string("(not set)") : g.settings.mods));
    if (!card.empty()) StartLoad(card, g.wantOutfit);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HFONT* f : {&g.fontTitle, &g.fontUi, &g.fontBold, &g.fontMono, &g.fontLog, &g.fontSmall})
        if (*f) DeleteObject(*f);
    return 0;
}
