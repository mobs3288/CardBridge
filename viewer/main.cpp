// CardBridge viewer: opens a Koikatsu card, assembles it exactly as the exporter does, and shows it in a window so every
// card can be checked against its picture without exporting. Windows only (plain Win32 + GDI, no extra dependencies).
//
//   cardbridge-viewer [card.png] [--library <dir>] [--mods <dir>] [--outfit N]
//
// Library and mods folders are remembered in %LOCALAPPDATA%\CardBridge\viewer.json. Drop a card on the window to open it.
//
//   mouse: left drag orbit, right/middle drag pan, wheel zoom, hover a part to light it up in the model,
//          click a part to hide/show it, right-click a part to solo it
//   keys:  Left/Right or 1-7 outfit, T textures, H the game shaders on/off, V extra shader layers on/off (layers/*.hlsl),
//          G colour grading on/off, X "as exported": the baked PMX textures on plain materials (what a PMX viewer shows),
//          K the skeleton over the model (click a bone to select it, Bones tab edits it), P automatic A-pose,
//          R reset view, F/S/B front/side/back,
//          I issues, A show every part, E export the current scene to PMX, C clear the log, O open a card,
//          M mods folder, L asset library folder

#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "assemble/PhysicsSim.h"
#include "export/BoneLabels.h"
#include "export/SkeletonOverlay.h"
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
#include "Ui.h"
#include "core/Version.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
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
double NowMs() {
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
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
    std::string author;      // named in the PMX model information; empty = the default author
    std::string exportDir;   // where exports go without asking; empty = ask every time
    std::string layers;      // an extra folder of shader layers (.hlsl)
    bool noMods = false;     // the user chose "no mods folder": do not fall back to the one the index cache remembers
};
Settings LoadSettings() {
    Settings s;
    try {
        auto j = nlohmann::json::parse([] { Bytes b = fileio::ReadFile(SettingsPath()); return std::string(b.begin(), b.end()); }());
        s.library = j.value("library", "");
        s.mods = j.value("mods", "");
        s.author = j.value("author", "");
        s.exportDir = j.value("exportDir", "");
        s.layers = j.value("layers", "");
        s.noMods = j.value("noMods", false);
    } catch (...) {}
    return s;
}
void SaveSettings(const Settings& s) {
    try {
        fileio::CreateDirectories(SettingsPath().substr(0, SettingsPath().find_last_of('/')));
        nlohmann::json j;
        j["library"] = s.library;
        j["mods"] = s.mods;
        j["author"] = s.author;
        j["exportDir"] = s.exportDir;
        j["layers"] = s.layers;
        j["noMods"] = s.noMods;
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

// Where a library the viewer builds itself goes (beside its settings), so the single exe needs no folder of its own.
std::string DefaultLibraryDir() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/library";
}

// The asset library normally sits in the CardBridge folder (library_full, else library): look from the exe upwards, then in the
// viewer's own folder under %LOCALAPPDATA%.
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
    const std::string own = DefaultLibraryDir();
    if (Exists(own + "/abdata")) return own;
    return {};
}

// Where the extra shader layers live: CARDBRIDGE_LAYERS, a layers folder in the library, then the repository's cardbridge/layers
// found from the exe upwards.
std::vector<std::string> LayerFolders(const std::string& library, const std::string& extra = std::string()) {
    std::vector<std::string> out;
    if (!extra.empty()) out.push_back(extra);
    if (const char* env = std::getenv("CARDBRIDGE_LAYERS")) out.push_back(Slashes(env));
    if (!library.empty()) out.push_back(library + "/layers");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string dir = Slashes(WideToUtf8(exe));
    for (int up = 0; up < 5; ++up) {
        dir = dir.substr(0, dir.find_last_of('/'));
        for (const char* sub : {"/cardbridge/layers", "/layers"})
            if (Exists(dir + sub)) out.push_back(dir + sub);
        if (dir.find('/') == std::string::npos) break;
    }
    return out;
}

// ---- live log ---------------------------------------------------------------------------------------------------------
enum class LogKind { Info, Good, Warn, Error };
struct LogLine {
    std::string time, text;
    LogKind kind = LogKind::Info;
};

// ---- the pose the user has made: bone edits and the A-pose, what undo and redo move between ----
struct PoseState {
    std::map<std::string, assemble::BonePose> edits;
    bool aPose = false;
    float aPoseDeg = 40.0f;
};

bool SamePoseState(const PoseState& a, const PoseState& b) {
    if (a.aPose != b.aPose || (a.aPose && a.aPoseDeg != b.aPoseDeg)) return false;
    auto rest = [](const assemble::BonePose& e) { return assemble::IsIdentity(e); };
    for (const auto& [name, e] : a.edits) {   // an edit that is the rest pose is the same as no edit
        auto it = b.edits.find(name);
        const assemble::BonePose other = it == b.edits.end() ? assemble::BonePose{} : it->second;
        if (e.pos.x != other.pos.x || e.pos.y != other.pos.y || e.pos.z != other.pos.z || e.rot.x != other.rot.x || e.rot.y != other.rot.y ||
            e.rot.z != other.rot.z || e.scl.x != other.scl.x || e.scl.y != other.scl.y || e.scl.z != other.scl.z) return false;
    }
    for (const auto& [name, e] : b.edits)
        if (!a.edits.count(name) && !rest(e)) return false;
    return true;
}

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
    float areola = 0.0f, areolaCard = 0.0f;              // areola size (a material property, re-shaped live)
};
enum Tab { TabParts = 0, TabBody, TabFace, TabLook, TabBones, TabSettings, TabCount };
const char* const kTabNames[TabCount] = {"Parts", "Body", "Face", "Look", "Bones", "Setup"};
// Slider ids: body 0..43, face 100..151, then the look sliders; switches from 300.
// Pose controls change the picture at once (no re-assembly): the A-pose switch and angle, and the nine fields of the selected bone (10000..10008).
// Settings tab actions (above every other id, so the slider and switch code never mistakes one for its own).
constexpr int kSetLibChoose = 20000, kSetLibBuild = 20001, kSetModsChoose = 20002, kSetModsReindex = 20003, kSetModsClear = 20004, kSetAuthor = 20005, kSetExportDir = 20006,
              kSetExportAsk = 20007, kSetLayers = 20008, kSetLayersDefault = 20009, kSetOpenFolder = 20010, kSetForget = 20011;
void SettingsAction(int id);
constexpr int kFaceBase = 100, kNippleMm = 200, kAreola = 201, kAPoseDeg = 202, kNippleIn = 300, kAPose = 301, kPhysics = 302, kLayerBase = 400, kBoneEdit = 10000, kBoneEditCount = 9;   // switches from 400: the shader layers
struct TweakRow {
    int kind = 0;        // 0 group header (id -1: nothing to reset), 1 slider, 2 switch, 4 plain text, 5 a settings action (a button row)
    int id = 0;
    std::string label, group;
};

struct PanelLayout {
    RECT logo{}, tabs[TabCount]{};
    RECT open{}, exportPmx{}, issues{}, reset{}, all{};
    int outfitNameTop = 0, outfitChipsTop = 0;
    int partsHeadingTop = 0, partsTop = 0, partsBottom = 0, logTop = 0, logBottom = 0, footerTop = 0;
    int boneListTop = 0, boneListBottom = 0, boneEditTop = 0;   // Bones tab: a switch row, the tree, then the editor of the selected bone
    RECT clearLog{};
    // Bones tab hit areas (the click code and the drawing use the same ones): the two switches, the reset row, the undo / redo / save / load row
    RECT boneSkeleton{}, boneHelpers{}, boneResetOne{}, boneResetAll{}, boneUndo{}, boneRedo{}, boneSave{}, boneLoad{};
};

constexpr int kBoneEditRows = 12;   // heading, nine fields, the reset row, the undo / redo / save / load row
constexpr int kHeaderBottom = 90;   // the header band (logo, name, stats) ends here

PanelLayout LayoutFor(int H, bool boneTab = false) {
    PanelLayout l;
    l.logo = {12, 12, 68, 68};
    const int by = 98, bh = 26;
    l.open = {10, by, 66, by + bh};
    l.exportPmx = {72, by, 172, by + bh};
    l.issues = {178, by, 234, by + bh};
    l.reset = {240, by, 308, by + bh};
    l.all = {314, by, 370, by + bh};
    // The outfit picker sits between the header buttons and the part list: a name line, then a row of numbered slots.
    l.outfitNameTop = 138;
    l.outfitChipsTop = 160;
    for (int i = 0; i < TabCount; ++i) l.tabs[i] = {10 + i * 51, 192, 59 + i * 51, 216};
    l.partsHeadingTop = 198;
    l.partsTop = 224;
    const int logH = boneTab ? 100 : std::clamp(H / 3, 130, 330);   // the bone editor needs the room
    l.footerTop = H - 36;
    l.logBottom = l.footerTop - 4;
    l.logTop = std::max(l.partsTop + 90, l.logBottom - logH);
    l.partsBottom = std::max(l.partsTop + kRowH, l.logTop - 10);
    l.boneEditTop = std::max(l.partsTop + 3 * kRowH, l.partsBottom - kBoneEditRows * kRowH - 4);
    l.boneListTop = l.partsTop + kRowH;
    l.boneListBottom = l.boneEditTop - 4;
    l.clearLog = {kPanel - 60, l.logTop + 3, kPanel - 8, l.logTop + 23};
    l.boneSkeleton = {10, l.partsTop, 200, l.partsTop + kRowH};
    l.boneHelpers = {204, l.partsTop, kPanel - 8, l.partsTop + kRowH};
    const int ry = l.boneEditTop + 10 * kRowH, ay = l.boneEditTop + 11 * kRowH;
    l.boneResetOne = {10, ry, 145, ry + kRowH};
    l.boneResetAll = {149, ry, 262, ry + kRowH};
    l.boneUndo = {10, ay, 56, ay + kRowH};
    l.boneRedo = {60, ay, 110, ay + kRowH};
    l.boneSave = {114, ay, 190, ay + kRowH};
    l.boneLoad = {194, ay, 268, ay + kRowH};
    return l;
}

struct App {
    HWND hwnd = nullptr;
    Settings settings;
    std::string cardPath;
    int wantOutfit = -1;
    std::string wantPose;                     // --pose <file>: a pose to load once the first card is up
    std::string pendingCard;                  // a card to open once the library has been set up
    int pendingOutfit = -1;
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
    std::recursive_mutex mu;   // recursive: a slider handler holds it while it re-shapes, which reads the character through Current()

    exporter::ViewCamera cam;
    exporter::ViewStatePtr state = exporter::NewViewState();
    exporter::GpuView gpu;                    // the game's shaders on Direct3D 11
    std::shared_ptr<Loaded> gpuScene;         // the scene the GPU copy was made from (keeps it alive while the GPU draws it)
    bool gpuOk = false;
    // "As exported" (X): a baked copy of the scene (colour only, graded like the preview when G is on) drawn with plain materials.
    exporter::GpuView gpuExp;
    std::shared_ptr<assemble::Scene> expScene, expPending;   // pending: filled by the bake thread, taken over on WM_EXP_READY
    std::shared_ptr<Loaded> expFor, expPendingFor;           // the loaded character the baked copy belongs to
    bool showExp = false, expOk = false, expGrade = false, expPendingGrade = false;
    std::atomic<bool> expBusy{false};
    exporter::ViewOptions vopt;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;
    ULONGLONG lastInput = 0;   // tick of the last camera change: the game shaders run only once the view has been still for a moment
    bool interacting = false, dirty = true, showIssues = false, textured = true, shaded = true;
    POINT last{};
    int drag = 0;   // 1 orbit, 2 pan
    int partScroll = 0, logScroll = 0;        // list scroll targets, in rows
    float partPos = 0, bonePos = 0;           // what the lists show now: the targets, eased (smooth scrolling)
    ui::Surface panel;                        // the panel, drawn off screen and copied where it changed
    // Mouse movement during a drag is only noted here; Frame() applies it once per picture, so a fast mouse cannot queue up work.
    bool dragPending = false;
    int dragX = 0;                            // slider drag: the pointer's x
    float boneDragDx = 0, boneDragDy = 0;     // bone drag: movement since the last picture
    ULONGLONG lastFrameTick = 0;
    bool animating = false;                   // a list is still easing towards its scroll target
    int hoverRow = -1, hoverPart = -1, hoverButton = 0;   // button: 0 none, 1 open, 2 export, 3 issues, 4 reset, 5 all
    int hoverOutfit = -1;                 // slot the cursor is over, for the name line
    int loadingOutfit = -1;               // outfit the worker is assembling, or -1
    int soloPart = -1;
    std::set<std::string> collapsedGroups;
    Tweaks tw;
    int tab = TabParts;
    int sliderDrag = -1;                      // slider id being dragged, or -1
    std::vector<exporter::ShaderLayer> layerDefs;   // extra HLSL shading layers found in the layers folder (the GPU view runs them)
    std::map<std::string, char> keepHidden;   // part checkboxes to restore after a tweak reload
    bool applyKeep = false;
    bool tweakPending = false;                // a change is waiting for the pointer to be still
    // Posing: bone edits by bone name (they survive a re-shape or an outfit change), the automatic A-pose, the skeleton overlay.
    std::map<std::string, assemble::BonePose> boneEdits;
    std::vector<PoseState> undoStack, redoStack;   // earlier poses (Ctrl+Z) and the ones undone (Ctrl+Y)
    PoseState undoBase;                           // the pose when the pointer went down: a gesture is one undo step
    bool undoBaseValid = false;
    ULONGLONG wheelUndoTick = 0;                  // wheel steps within a moment are one undo step
    bool aPose = false;
    float aPoseDeg = 40.0f;
    bool showBones = false, showHelpers = false;
    std::string selBoneName;                  // the selected bone, by name
    std::set<std::string> openBones;          // tree nodes that are open
    bool bonesSeeded = false;                 // openBones was given its first value for this character
    std::vector<Mat4> skin;                   // current skin matrices (empty = rest pose)
    std::vector<Vec3> boneWorld;              // posed bone positions, for the overlay
    std::vector<std::pair<float, float>> boneScreen;   // where the overlay put each bone in the last picture (x < 0: not drawn)
    exporter::GpuView* lastGpu = nullptr;     // the view that drew the last picture
    POINT downAt{};                           // where the left button went down in the view (a click without a drag picks a bone)
    int boneScroll = 0;
    int hoverBone = -1;                       // the bone under the pointer (in the view or the tree), lit up in the overlay
    int dragBone = -1;                        // the bone being turned (or moved) with the left button in the view
    bool dragMove = false;
    // Physics preview: the DynamicBone chains simulated live while the switch is on.
    assemble::PhysicsSim sim;
    bool physics = false, physicsMoving = false, physicsKick = false;
    ULONGLONG physicsLast = 0;
    double physicsAccum = 0.0;
    std::vector<Mat4> poseWorld;

    // Frame timing (CARDBRIDGE_BENCH): milliseconds spent in the three parts of a paint, summed, and how many paints ran.
    struct Prof { double render = 0, blit = 0, panel = 0, panelDraw = 0, panelCopy = 0, reshape = 0, refresh = 0, sec[8] = {}; long paints = 0, renders = 0; } prof;

    std::mutex logMu;
    std::vector<LogLine> logLines;
    HFONT fontTitle = nullptr, fontUi = nullptr, fontBold = nullptr, fontMono = nullptr, fontLog = nullptr, fontSmall = nullptr;
};
App g;

PanelLayout Lay(int H) { return LayoutFor(H, g.tab == TabBones && !g.showIssues); }

constexpr UINT WM_LOADED = WM_APP + 1;
constexpr UINT WM_LOG = WM_APP + 2;
constexpr UINT WM_EXPORT_DONE = WM_APP + 3;
constexpr UINT WM_EXP_READY = WM_APP + 4;
constexpr UINT WM_LIBRARY_DONE = WM_APP + 5;    // the library import finished (lParam 1 = ok)
constexpr UINT WM_SETUP_LIBRARY = WM_APP + 6;   // first run: no library yet, offer to build one
// Test hook (viewer_smoke.ps1): only answered when CARDBRIDGE_TEST_HOOKS is set in the environment, so a normal run ignores it.
// wParam: 1 undo, 2 redo, 3 save the pose, 4 load the pose (both to CARDBRIDGE_POSE_PATH), 10 write the layout JSON to CARDBRIDGE_TEST_LAYOUT, 11 nude body on, clothes and accessories off, the camera on the chest.
constexpr UINT WM_TEST = WM_APP + 40;
constexpr UINT WM_BENCH = WM_APP + 41;   // CARDBRIDGE_BENCH: run the scripted timing session (see RunBench)
void RunBench();
constexpr UINT_PTR kTimerApplyTweaks = 2;

std::shared_ptr<Loaded> Current() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    return g.loaded;
}

// Invalidation is by area, so a hover or a slider drag repaints a few rows instead of the whole window.
void InvalidatePanel() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    rc.right = kPanel;
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void InvalidateView() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    rc.left = kPanel;
    InvalidateRect(g.hwnd, &rc, FALSE);
}

void InvalidateAll() { if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE); }

void InvalidatePanelBand(int top, int bottom) {
    if (!g.hwnd) return;
    RECT rc{0, top, kPanel, bottom};
    InvalidateRect(g.hwnd, &rc, FALSE);
}

int ClientHeight() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    return rc.bottom;
}
void InvalidateHeader() { InvalidatePanelBand(0, kHeaderBottom); }
void InvalidateLog() { InvalidatePanelBand(Lay(ClientHeight()).logTop, ClientHeight()); }
void InvalidateFooter() { InvalidatePanelBand(Lay(ClientHeight()).footerTop, ClientHeight()); }
void InvalidateList() {
    const PanelLayout l = Lay(ClientHeight());
    InvalidatePanelBand(l.partsHeadingTop - 4, l.partsBottom + 4);
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
    InvalidateLog();
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
    const char* fmt = nullptr;   // a printf format for the value (one double), instead of the two above
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
    if (id == kAPoseDeg) { r.v = &g.aPoseDeg; r.def = 40.0f; r.lo = 15; r.hi = 70; r.step = 1; r.percent = false; r.fmt = "%.0f"; return true; }
    if (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) {   // a field of the selected bone
        if (g.selBoneName.empty()) return false;
        assemble::BonePose& e = g.boneEdits[g.selBoneName];
        float* f[kBoneEditCount] = {&e.pos.x, &e.pos.y, &e.pos.z, &e.rot.x, &e.rot.y, &e.rot.z, &e.scl.x, &e.scl.y, &e.scl.z};
        const int k = id - kBoneEdit;
        r.v = f[k];
        r.percent = false;
        if (k < 3) { r.def = 0; r.lo = -0.3f; r.hi = 0.3f; r.step = 0.001f; r.fmt = "%+.3f"; }
        else if (k < 6) { r.def = 0; r.lo = -180; r.hi = 180; r.step = 1; r.fmt = "%+.0f"; }
        else { r.def = 1; r.lo = 0.1f; r.hi = 3; r.step = 0.01f; r.fmt = "%.2f"; }
        return true;
    }
    return false;
}

// Pose controls change the picture at once, no re-assembly.
bool IsLiveId(int id) { return id == kAPose || id == kAPoseDeg || (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount); }

std::string Shorten(const std::string& s, size_t maxChars);

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
        rows.push_back({0, -1, "Pose (shows at once, the export carries it)", "Pose"});
        rows.push_back({2, kAPose, "Automatic A-pose: arms down from the T-pose", "Pose"});
        rows.push_back({1, kAPoseDeg, "A-pose angle (degrees)", "Pose"});
        rows.push_back({0, -1, "Physics preview (Y; N shakes it)", "Physics"});
        rows.push_back({2, kPhysics, "Simulate hair, skirts and ribbons live", "Physics"});
        rows.push_back({0, -1, "Shader layers (on top of the game's shaders)", "Shader layers"});
        if (g.layerDefs.empty()) rows.push_back({4, 0, "none found: put .hlsl files in the layers folder", "Shader layers"});
        for (size_t i = 0; i < g.layerDefs.size(); ++i)
            rows.push_back({2, kLayerBase + static_cast<int>(i), g.layerDefs[i].name, "Shader layers"});
    }
    else if (tab == TabSettings) {
        auto shown = [](const std::string& v, const char* none) { return v.empty() ? std::string(none) : Shorten(v, 50); };
        rows.push_back({0, -1, "Asset library (the game's data)", "Library"});
        rows.push_back({4, 0, shown(g.settings.library, "(not set)"), "Library"});
        rows.push_back({5, kSetLibChoose, "Use another library folder ...", "Library"});
        rows.push_back({5, kSetLibBuild, "Build a library from my Koikatsu folder ...", "Library"});
        rows.push_back({0, -1, "Mods", "Mods"});
        rows.push_back({4, 0, g.settings.noMods ? std::string("(no mods: base-game items only)") : shown(g.settings.mods, "(not set)"), "Mods"});
        rows.push_back({5, kSetModsChoose, "Choose the mods folder ...", "Mods"});
        rows.push_back({5, kSetModsReindex, "Rebuild the mod index (after adding mods)", "Mods"});
        rows.push_back({5, kSetModsClear, "Use no mods", "Mods"});
        rows.push_back({0, -1, "Export", "Export"});
        rows.push_back({4, 0, "author in the PMX info: " + (g.settings.author.empty() ? std::string(CARDBRIDGE_AUTHOR) : g.settings.author), "Export"});
        rows.push_back({5, kSetAuthor, "Change the author name ...", "Export"});
        rows.push_back({4, 0, "exports go to: " + shown(g.settings.exportDir, "(asks every time)"), "Export"});
        rows.push_back({5, kSetExportDir, "Always export into one folder ...", "Export"});
        rows.push_back({5, kSetExportAsk, "Ask for the folder every time", "Export"});
        rows.push_back({0, -1, "Shader layers", "Layers"});
        rows.push_back({4, 0, "extra folder: " + shown(g.settings.layers, "(none)"), "Layers"});
        rows.push_back({5, kSetLayers, "Choose a shader layers folder ...", "Layers"});
        rows.push_back({5, kSetLayersDefault, "Use only the default places", "Layers"});
        rows.push_back({0, -1, "CardBridge", std::string("v") + CARDBRIDGE_VERSION});
        rows.push_back({5, kSetOpenFolder, "Open the settings folder in Explorer", "CardBridge"});
        rows.push_back({5, kSetForget, "Forget all settings (asked again next start)", "CardBridge"});
    }
    return rows;
}

bool TweakChanged(int id) {
    SliderRef r;
    if (id >= kLayerBase && id < kBoneEdit) return g.gpu.layerEnabled(static_cast<size_t>(id - kLayerBase));
    if (id == kNippleIn) return g.tw.nippleInside;
    if (id == kAPose) return g.aPose;
    if (id == kPhysics) return g.physics;
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
    else if (id == kAPose) g.aPose = false;
    else if (SliderFor(id, r)) *r.v = r.def;
}

void ScheduleApplyTweaks() {
    g.tweakPending = true;
    SetTimer(g.hwnd, kTimerApplyTweaks, 800, nullptr);
    InvalidateList();
}

// ---- posing ---------------------------------------------------------------------------------------------------------
assemble::Pose BuildPose(const Loaded& L) {
    assemble::Pose p;
    for (const auto& [name, e] : g.boneEdits) {
        if (assemble::IsIdentity(e)) continue;
        const int i = L.scene.boneIndex(name);
        if (i >= 0) p.edit(static_cast<size_t>(i)) = e;
    }
    if (g.aPose) assemble::MakeAPose(L.scene, g.aPoseDeg, p.base);
    return p;
}

// Re-skins every Direct3D view with the current pose (and the physics, when it is on) without a re-assembly, and refreshes the bone
// positions the overlay draws. `simSteps` physics updates run first (0 only makes the chains follow the pose).
void ApplyPoseLive(int simSteps = 0) {
    std::shared_ptr<Loaded> L = Current();
    g.skin.clear();
    g.boneWorld.clear();
    if (!L || L->scene.bones.empty()) return;
    const assemble::Scene& scene = L->scene;
    const size_t n = scene.bones.size();
    const assemble::Pose p = BuildPose(*L);
    g.poseWorld = assemble::BoneWorlds(scene, p);
    if (!p.empty() || (g.physics && !g.sim.empty())) {
        g.skin.resize(n);
        for (size_t i = 0; i < n; ++i) g.skin[i] = g.poseWorld[i] * Mat4::TRS(scene.bones[i].pos * -1.0f, Quat{}, Vec3{1, 1, 1});
        if (g.physics && !g.sim.empty()) {
            g.physicsMoving = g.sim.Step(scene, g.poseWorld, simSteps, g.skin);
            if (simSteps == 0) g.physicsKick = true;   // the anchors moved: let the chains catch up
        }
    }
    g.boneWorld.resize(n);
    for (size_t i = 0; i < n; ++i) g.boneWorld[i] = g.skin.empty() ? scene.bones[i].pos : g.skin[i].point(scene.bones[i].pos);
    const std::vector<Mat4>* sk = g.skin.empty() ? nullptr : &g.skin;
    if (g.gpuOk) g.gpu.SetSkin(sk);
    if (g.expOk) g.gpuExp.SetSkin(sk);
    g.dirty = true;
    InvalidateView();
}

// The file a pose is saved to / loaded from: CARDBRIDGE_POSE_PATH when set (scripted runs, no dialog), else the usual file dialog.
std::string PosePathFromUser(bool save) {
    if (const char* env = std::getenv("CARDBRIDGE_POSE_PATH")) return Slashes(env);
    wchar_t file[1024] = L"pose.json";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g.hwnd;
    of.lpstrFilter = L"CardBridge pose (*.json)\0*.json\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrDefExt = L"json";
    of.lpstrTitle = save ? L"Save the pose" : L"Load a pose";
    of.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    return ok ? Slashes(WideToUtf8(file)) : std::string();
}

PoseState CapturePose() {
    PoseState st;
    st.edits = g.boneEdits;
    st.aPose = g.aPose;
    st.aPoseDeg = g.aPoseDeg;
    return st;
}

void RestorePose(const PoseState& st) {
    g.boneEdits = st.edits;
    g.aPose = st.aPose;
    g.aPoseDeg = st.aPoseDeg;
    ApplyPoseLive();
    InvalidatePanel();
}

// Records `before` as an undo step when the pose is no longer it. `merge`: a step that follows another within a moment (wheel notches) joins it.
void PoseChanged(const PoseState& before, bool merge = false) {
    if (SamePoseState(before, CapturePose())) return;
    const ULONGLONG now = GetTickCount64();
    const bool join = merge && !g.undoStack.empty() && now - g.wheelUndoTick < 700;
    if (merge) g.wheelUndoTick = now;
    if (join) return;
    g.undoStack.push_back(before);
    if (g.undoStack.size() > 200) g.undoStack.erase(g.undoStack.begin());
    g.redoStack.clear();
}

// The pointer went up: whatever the gesture since it went down changed (a slider drag, a bone drag, a click on a switch or a reset) is one step.
void CommitPoseEdit() {
    if (!g.undoBaseValid) return;
    g.undoBaseValid = false;
    PoseChanged(g.undoBase);
}

void UndoPose(bool redo) {
    g.undoBaseValid = false;
    std::vector<PoseState>& from = redo ? g.redoStack : g.undoStack;
    std::vector<PoseState>& to = redo ? g.undoStack : g.redoStack;
    if (from.empty()) { Log(redo ? "nothing to redo" : "nothing to undo", LogKind::Warn); return; }
    to.push_back(CapturePose());
    const PoseState st = from.back();
    from.pop_back();
    RestorePose(st);
    Log(std::string(redo ? "redo" : "undo") + " (" + std::to_string(g.undoStack.size()) + " step(s) to undo, " + std::to_string(g.redoStack.size()) + " to redo)");
}

// The pose as a file (bone edits by name and the A-pose switch). CARDBRIDGE_POSE_PATH names the file without asking (scripted runs).
assemble::PoseFile CurrentPoseFile() {
    assemble::PoseFile pf;
    pf.aPose = g.aPose;
    pf.aPoseDegrees = g.aPoseDeg;
    for (const auto& [name, e] : g.boneEdits)
        if (!assemble::IsIdentity(e)) pf.bones.emplace_back(name, e);
    return pf;
}

void SavePoseFile() {
    std::string path = PosePathFromUser(true);
    if (path.empty()) return;
    try {
        const std::string text = assemble::PoseFileToJson(CurrentPoseFile());
        fileio::WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
        Log("saved the pose to " + path, LogKind::Good);
    } catch (const std::exception& e) {
        Log(std::string("could not save the pose: ") + e.what(), LogKind::Error);
    }
}

void LoadPoseFile(const std::string& given = std::string()) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) { Log("open a card first", LogKind::Warn); return; }
    const std::string path = given.empty() ? PosePathFromUser(false) : given;
    if (path.empty()) return;
    assemble::PoseFile pf;
    std::string err;
    try {
        const Bytes raw = fileio::ReadFile(path);
        if (!assemble::PoseFileFromJson(std::string(raw.begin(), raw.end()), pf, &err)) { Log("not a pose file: " + err, LogKind::Error); return; }
    } catch (const std::exception& e) {
        Log(std::string("could not read the pose: ") + e.what(), LogKind::Error);
        return;
    }
    const PoseState before = CapturePose();
    g.undoBaseValid = false;
    g.boneEdits.clear();
    size_t unknown = 0;
    for (const auto& [name, e] : pf.bones) {
        if (L->scene.boneIndex(name) < 0) { ++unknown; continue; }
        g.boneEdits[name] = e;
    }
    g.aPose = pf.aPose;
    g.aPoseDeg = pf.aPoseDegrees;
    ApplyPoseLive();
    InvalidatePanel();
    PoseChanged(before);
    Log("loaded the pose from " + path + ": " + std::to_string(pf.bones.size() - unknown) + " bone edit(s)" + (unknown ? ", " + std::to_string(unknown) + " bone(s) this rig does not have skipped" : std::string()), unknown ? LogKind::Warn : LogKind::Good);
}

// The physics switch: on starts every chain at rest under the current pose and a 60 Hz timer; the chains then swing and settle as bones,
// sliders or the A-pose move what they hang from (N gives them a shove).
void SetPhysics(bool on) {
    std::shared_ptr<Loaded> L = Current();
    if (on && (!L || L->scene.chains.empty())) { Log("no physics chains in this character", LogKind::Warn); on = false; }
    g.physics = on;
    if (on) {
        g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)));
        g.physicsLast = GetTickCount64();
        g.physicsAccum = 0.0;
        g.physicsKick = g.physicsMoving = true;
        Log("physics: on (" + std::to_string(L->scene.chains.size()) + " chains; move a bone or press N)", LogKind::Good);
    } else {
        Log("physics: off");
    }
    ApplyPoseLive();
}

// One step of the physics preview (Frame calls it every picture while the switch is on): as many 1/60 s updates as the time asks for
// (at most four), drawn once.
void PhysicsTick() {
    if (!g.physics) return;
    const ULONGLONG now = GetTickCount64();
    g.physicsAccum = std::min(0.1, g.physicsAccum + static_cast<double>(now - g.physicsLast) / 1000.0);
    g.physicsLast = now;
    const int steps = std::min(4, static_cast<int>(g.physicsAccum * 60.0));
    if (steps <= 0) return;
    g.physicsAccum -= steps / 60.0;
    if (!g.physicsMoving && !g.physicsKick) return;   // everything has settled: nothing to draw
    g.physicsKick = false;
    ApplyPoseLive(steps);
}

// The bone tree as list rows. Driven, shape and attach nodes are left out unless asked for (their children move up to the
// nearest shown ancestor), so the tree is the bones a person poses.
struct BoneRow {
    int bone = -1;
    int depth = 0;
    bool kids = false, open = false;
};

bool BoneShown(const assemble::Scene& scene, int i) { return g.showHelpers || !exporter::IsHelperBone(scene.bones[static_cast<size_t>(i)].name); }

std::vector<BoneRow> BuildBoneRows(const assemble::Scene& scene) {
    const size_t n = scene.bones.size();
    std::vector<std::vector<int>> kids(n);
    std::vector<int> roots;
    for (size_t i = 0; i < n; ++i) {
        const int p = scene.bones[i].parent;
        if (p < 0 || static_cast<size_t>(p) >= n || p == static_cast<int>(i)) roots.push_back(static_cast<int>(i));
        else kids[static_cast<size_t>(p)].push_back(static_cast<int>(i));
    }
    std::function<void(int, std::vector<int>&)> shownKids = [&](int i, std::vector<int>& out) {
        for (int c : kids[static_cast<size_t>(i)]) { if (BoneShown(scene, c)) out.push_back(c); else shownKids(c, out); }
    };
    std::vector<BoneRow> rows;
    std::function<void(int, int)> walk = [&](int i, int depth) {
        std::vector<int> vk;
        shownKids(i, vk);
        BoneRow r{i, depth, !vk.empty(), g.openBones.count(scene.bones[static_cast<size_t>(i)].name) != 0};
        rows.push_back(r);
        if (r.open) for (int c : vk) walk(c, depth + 1);
    };
    for (int r : roots) {
        if (BoneShown(scene, r)) walk(r, 0);
        else { std::vector<int> vk; shownKids(r, vk); for (int c : vk) walk(c, 0); }
    }
    return rows;
}

// First look at a character's tree: the top levels open.
void SeedOpenBones(const assemble::Scene& scene) {
    g.openBones.clear();
    std::vector<int> depth(scene.bones.size(), 0);
    for (size_t i = 0; i < scene.bones.size(); ++i) {
        const int p = scene.bones[i].parent;
        depth[i] = p >= 0 && static_cast<size_t>(p) < i ? depth[static_cast<size_t>(p)] + 1 : 0;
        if (depth[i] < 4) g.openBones.insert(scene.bones[i].name);
    }
}

// Selects a bone (by index) and opens the tree down to it so it can be seen.
void SelectBone(const assemble::Scene& scene, int bone) {
    if (bone < 0 || static_cast<size_t>(bone) >= scene.bones.size()) { g.selBoneName.clear(); return; }
    g.selBoneName = scene.bones[static_cast<size_t>(bone)].name;
    for (int a = scene.bones[static_cast<size_t>(bone)].parent; a >= 0 && static_cast<size_t>(a) < scene.bones.size(); a = scene.bones[static_cast<size_t>(a)].parent) {
        g.openBones.insert(scene.bones[static_cast<size_t>(a)].name);
        if (scene.bones[static_cast<size_t>(a)].parent == a) break;
    }
    if (exporter::IsHelperBone(g.selBoneName)) g.showHelpers = true;
    const std::vector<BoneRow> rows = BuildBoneRows(scene);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].bone == bone) { g.boneScroll = static_cast<int>(i) > 3 ? static_cast<int>(i) - 3 : 0; break; }
}

// A body or face slider, the nipple switch or offset: re-shapes the assembled scene in place (a few milliseconds, no loading) and re-skins the
// views. `final` (the slider was let go) also rebuilds the normal map frames, which are left alone while dragging. False when it cannot
// be done and the character has to be assembled again.
bool ReshapeNow(bool final) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.busy || !g.resolver || L->scene.bones.empty()) return false;
    assemble::AssembleOptions ao;
    ao.outfit = L->outfit;
    {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        ao.bodyShape = g.tw.body;
        ao.faceShape = g.tw.face;
        ao.nippleInside = g.tw.nippleInside;
        ao.nippleOffset = g.tw.nippleMm / 1000.0f;
        if (std::fabs(g.tw.areola - g.tw.areolaCard) > 1e-4f) ao.areolaSize = g.tw.areola;
    }
    const double t0 = NowMs();
    if (!assemble::Reshape(L->scene, L->card, *g.resolver, ao)) return false;
    const double t1 = NowMs();
    if (g.gpuOk) g.gpu.Refresh(final);
    g.prof.reshape += t1 - t0;
    g.prof.refresh += NowMs() - t1;
    if (g.expOk && g.expScene && g.expFor == L) {   // the baked "as exported" copy keeps its textures; only its shape follows
        assemble::Reshape(*g.expScene, L->card, *g.resolver, ao);
        g.gpuExp.Refresh(final);
    }
    if (g.physics) g.sim.Reset(L->scene, assemble::BoneWorlds(L->scene, BuildPose(*L)));
    ApplyPoseLive();
    return true;
}

bool IsShapeId(int id) { return (id >= 0 && id <= kNippleMm) || id == kAreola || id == kNippleIn; }

// Something a slider or switch controls changed. Pose controls, the shape sliders and the areola size show at once; the character is only
// assembled again when it cannot be re-shaped in place (after the pointer has been still for a moment).
void TweakEdited(int id, bool released) {
    if (IsLiveId(id)) { ApplyPoseLive(); return; }
    if (id == kAreola && g.expOk) {   // the baked "as exported" copy has the old areola in its textures
        g.expOk = false;
        g.showExp = false;
        g.expFor.reset();
        Log("the areola size changed: press X to bake the as-exported view again");
    }
    if (IsShapeId(id)) { if (!ReshapeNow(released)) ScheduleApplyTweaks(); return; }
    if (released) ScheduleApplyTweaks();
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
        std::lock_guard<std::recursive_mutex> lk(g.mu);
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
        std::lock_guard<std::recursive_mutex> lk(g.mu);
        g.modsInfo = std::to_string(g.mods.guidCount()) + " mods found";
        if (g.mods.guidCount() == 0) g.modsInfo = "NO mods found in that folder - press M and pick the folder that holds the .zipmod files";
        Log(g.modsInfo, g.mods.guidCount() ? LogKind::Good : LogKind::Error);
    } else {
        std::lock_guard<std::recursive_mutex> lk(g.mu);
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
                std::lock_guard<std::recursive_mutex> lk(g.mu);
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
            std::lock_guard<std::recursive_mutex> lk(g.mu);
            g.loaded = out;
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LOADED, 0, 0);
    }).detach();
}

// ---- export ------------------------------------------------------------------------------------------------------------
std::string PickExportParent(HWND owner) {
    if (std::getenv("CARDBRIDGE_TEST_HOOKS") && std::getenv("CARDBRIDGE_EXPORT_PARENT")) return Slashes(std::getenv("CARDBRIDGE_EXPORT_PARENT"));   // scripted runs: no dialog
    if (!g.settings.exportDir.empty() && Exists(g.settings.exportDir)) return g.settings.exportDir;   // set in the Setup tab
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
    const std::string library = g.settings.library, mods = g.settings.mods, author = g.settings.author;
    const std::vector<char> hiddenNow = g.hidden;
    const assemble::Pose pose = BuildPose(*L);
    Log("export started: " + outDir + (pose.empty() ? std::string() : std::string(" (posed)")), LogKind::Info);
    assemble::Scene sceneCopy = L->scene;   // taken here: a slider may re-shape the character while the export runs
    std::thread([L, outDir, stem, library, mods, author, hiddenNow, pose, sceneCopy = std::move(sceneCopy)]() mutable {
        auto t0 = std::chrono::steady_clock::now();
        try {
            assemble::Scene scene = std::move(sceneCopy);   // bake the copy, not what the viewport is drawing
            assemble::BakePose(scene, pose);    // the A-pose and the bone edits become the model's rest pose
            exporter::ApplyPartStateForExport(scene, hiddenNow);   // the PMX starts the way the viewer shows it; morphs switch the rest
            Log("baking the game shader textures");
            // Colour only, graded like the preview (G): the PMX viewer or engine does the lighting over a white material.
            exporter::BakeShaderTextures(scene, 1024, false, nullptr, [](const std::string& m) { Log("  " + m); }, true);
            exporter::ExportOptions eo;
            eo.outDir = outDir;
            eo.baseName = stem;
            eo.outfit = L->outfit;
            eo.assetLibrary = library;
            eo.modsFolder = mods;
            eo.author = author;
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

// X: show the character the way the exported PMX looks (baked textures, plain materials). The bake takes a while, so it runs
// on a thread once per character and grading state; later presses only switch.
void ToggleAsExported() {
    std::shared_ptr<Loaded> L = Current();
    if (!L || L->scene.parts.empty()) return;
    if (g.showExp) { g.showExp = false; g.dirty = true; Log("as exported: off"); return; }
    if (!g.gpu.ready()) { Log("as exported needs the Direct3D 11 view", LogKind::Warn); return; }
    if (g.expOk && g.expFor == L && g.expGrade == exporter::ColourGradeEnabled()) { g.showExp = true; g.dirty = true; Log("as exported: on"); return; }
    if (g.expBusy.exchange(true)) { Log("the export preview is still being baked", LogKind::Warn); return; }
    const bool grade = exporter::ColourGradeEnabled();
    Log(std::string("baking the export preview (colour only, ") + (grade ? "graded" : "not graded") + ")");
    auto copy = std::make_shared<assemble::Scene>(L->scene);
    std::thread([L, grade, copy]() {
        std::shared_ptr<assemble::Scene> sc;
        try {
            sc = copy;
            exporter::BakeShaderTextures(*sc, 1024, false, nullptr, {}, grade);
        } catch (const std::exception& e) {
            Log(std::string("export preview failed: ") + e.what(), LogKind::Error);
            sc.reset();
        }
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            g.expPending = sc; g.expPendingFor = L; g.expPendingGrade = grade;
        }
        PostMessageW(g.hwnd, WM_EXP_READY, 0, 0);
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
    const int row = (y - l.partsTop + static_cast<int>(std::lround(g.partPos * kRowH))) / kRowH;   // the list as it is drawn now (it may be mid-scroll)
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

// ---- skeleton overlay -------------------------------------------------------------------------------------------------
// Draws the bones the last picture was made with (pose included) over it and remembers where each landed, for picking.
void DrawSkeleton(int w, int h) {
    g.boneScreen.clear();
    std::shared_ptr<Loaded> L = Current();
    if (!g.showBones || !g.lastGpu || !L) return;
    exporter::DrawSkeletonOverlay(L->scene, g.boneWorld, *g.lastGpu, g.frame, w, h, g.selBoneName.empty() ? -1 : L->scene.boneIndex(g.selBoneName), g.hoverBone, g.showHelpers, &g.boneScreen);
}

// Turns (or, with Ctrl, moves) a bone by a pointer movement: the bone's tip follows the pointer, as if it were grabbed there. The new
// angles go into the bone's edit and the views re-skin at once.
void DragBone(int bone, float dx, float dy, bool move) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || bone < 0 || static_cast<size_t>(bone) >= L->scene.bones.size() || g.boneWorld.size() != L->scene.bones.size() || !g.lastGpu) return;
    const assemble::Scene& scene = L->scene;
    const size_t n = scene.bones.size();
    // the picture's right and up in the world (the same camera GpuView::Render builds)
    const float yaw = g.cam.yawDeg * 3.14159265f / 180.0f, pit = g.cam.pitchDeg * 3.14159265f / 180.0f;
    const float cs = std::cos(yaw), sn = std::sin(yaw), cp = std::cos(pit), sp = std::sin(pit);
    const Vec3 right{-cs, 0.0f, -sn}, up{sp * sn, cp, -sp * cs};
    // the tip: the nearest descendant that is not on the bone itself (a shown bone before a helper at the same depth; helpers such as
    // cf_s_hand_L stand on their parent, so the search goes on below them), else the parent's direction extended
    const Vec3 pivot = g.boneWorld[static_cast<size_t>(bone)];
    int tipBone = -1;
    {
        auto apart = [&](size_t i) {
            const Vec3 d{g.boneWorld[i].x - pivot.x, g.boneWorld[i].y - pivot.y, g.boneWorld[i].z - pivot.z};
            return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) > 1e-3f;
        };
        std::vector<int> level{bone};
        for (int depth = 0; depth < 4 && tipBone < 0 && !level.empty(); ++depth) {
            std::vector<int> below;
            int shown = -1, any = -1;
            for (size_t i = 0; i < n; ++i) {
                if (static_cast<int>(i) == bone || std::find(level.begin(), level.end(), scene.bones[i].parent) == level.end()) continue;
                below.push_back(static_cast<int>(i));
                if (!apart(i)) continue;
                if (any < 0) any = static_cast<int>(i);
                if (shown < 0 && !exporter::IsHelperBone(scene.bones[i].name)) shown = static_cast<int>(i);
            }
            tipBone = shown >= 0 ? shown : any;
            level = std::move(below);
        }
    }
    Vec3 tip;
    if (tipBone >= 0) tip = g.boneWorld[static_cast<size_t>(tipBone)];
    else {
        const int par = scene.bones[static_cast<size_t>(bone)].parent;
        Vec3 back = par >= 0 ? Vec3{pivot.x - g.boneWorld[static_cast<size_t>(par)].x, pivot.y - g.boneWorld[static_cast<size_t>(par)].y, pivot.z - g.boneWorld[static_cast<size_t>(par)].z} : Vec3{0, -0.05f, 0};
        tip = pivot + normalize(back) * 0.05f;
    }
    Vec3 arm{tip.x - pivot.x, tip.y - pivot.y, tip.z - pivot.z};
    const float armLen = std::sqrt(arm.x * arm.x + arm.y * arm.y + arm.z * arm.z);
    if (armLen < 1e-5f) return;
    float ax, ay, bx, by, depth;
    if (!g.lastGpu->Project(pivot, ax, ay, depth) || !g.lastGpu->Project(tip, bx, by, depth)) return;
    const float pixels = std::max(8.0f, std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)));
    const float perPixel = armLen / pixels;                        // world length of one pixel at the bone
    const Vec3 delta = (right * dx + up * (-dy)) * perPixel;      // the pointer's movement in the world
    assemble::Pose pose = BuildPose(*L);
    assemble::BonePose& e = g.boneEdits[scene.bones[static_cast<size_t>(bone)].name];
    if (move) e.pos = assemble::MoveBone(scene, pose, static_cast<size_t>(bone), delta);
    else {
        const Vec3 dir = normalize(arm);
        const Mat4 turn = assemble::RotationBetween(dir, normalize(arm + delta));
        e.rot = assemble::TurnBone(scene, pose, static_cast<size_t>(bone), turn);
    }
    ApplyPoseLive();
}

// A click in the view on a bone of the overlay selects it (the nearest within a few pixels) and shows it in the Bones tab.
void PickBoneAt(int px, int py) {
    std::shared_ptr<Loaded> L = Current();
    if (!L || g.boneScreen.empty()) return;
    const int best = exporter::PickBone(g.boneScreen, static_cast<float>(px), static_cast<float>(py), 14.0f);
    if (best < 0) return;
    SelectBone(L->scene, best);
    g.tab = TabBones;
    g.showIssues = false;
    Log("bone " + L->scene.bones[static_cast<size_t>(best)].name);
    g.dirty = true;
    InvalidateRect(g.hwnd, nullptr, FALSE);
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
    if (g.showExp && g.expOk && g.expFor == L && g.gpuExp.Render(g.cam, w, h, &g.effHidden, g.frame, g.hoverPart)) { g.lastGpu = &g.gpuExp; DrawSkeleton(w, h); return; }   // as exported
    if (g.shaded && g.gpuOk && g.gpuScene == L) {   // Direct3D 11: the game's shaders at full rate
        if (g.gpu.Render(g.cam, w, h, &g.effHidden, g.frame, g.hoverPart)) { g.lastGpu = &g.gpu; DrawSkeleton(w, h); return; }
        g.gpuOk = false;
    }
    g.lastGpu = nullptr;
    g.boneScreen.clear();
    // Software shaders are slow: only once the view has been still for a moment.
    const bool still = GetTickCount64() - g.lastInput > 300;
    g.vopt.shaded = g.shaded && still;
    if (g.shaded && !still) SetTimer(g.hwnd, 1, 320, nullptr);   // come back and draw the shaded picture
    g.vopt.hiddenParts = &g.effHidden;
    g.vopt.step = g.interacting ? 3 : 1;
    g.vopt.skin = g.skin.empty() ? nullptr : &g.skin;   // the software view shows the pose (and the physics) too
    exporter::RenderView(L->scene, g.cam, w, h, g.vopt, *g.state, g.frame);
}

COLORREF KindColor(LogKind k) {
    switch (k) {
    case LogKind::Good: return ui::kGood;
    case LogKind::Warn: return ui::kAmber;
    case LogKind::Error: return ui::kBad;
    default: return ui::kSub;
    }
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    auto ch = [&](int (*get)(COLORREF)) { return static_cast<int>(std::lround(get(a) + (get(b) - get(a)) * t)); };
    return RGB(ch([](COLORREF c) -> int { return GetRValue(c); }), ch([](COLORREF c) -> int { return GetGValue(c); }), ch([](COLORREF c) -> int { return GetBValue(c); }));
}

bool Touches(const RECT& clip, const RECT& r) { RECT t; return IntersectRect(&t, &clip, &r) != 0; }
bool TouchesY(const RECT& clip, int top, int bottom) { return bottom > clip.top && top < clip.bottom; }

// A scrolled list: which row is first, how far it is pushed up (pixels), and how many rows show.
struct ListView {
    int first = 0, offset = 0, count = 0;
};
int ScrollPx(float pos, int rowH = kRowH) { return static_cast<int>(std::lround(std::max(0.0f, pos) * static_cast<float>(rowH))); }
ListView ViewOf(int top, int bottom, float pos, int rowH = kRowH) {
    const int px = ScrollPx(pos, rowH);
    ListView v;
    v.first = px / rowH;
    v.offset = px % rowH;
    v.count = (bottom - top + v.offset + rowH - 1) / rowH;
    return v;
}

void Button(ui::Surface& s, const RECT& r, const std::string& label, bool hot, bool on, bool enabled, bool primary = false) {
    COLORREF text;
    if (primary && enabled) {
        s.FillGradient(r, 7, hot ? Mix(ui::kAccent, RGB(255, 255, 255), 0.18f) : ui::kAccent, hot ? Mix(ui::kAccent2, RGB(255, 255, 255), 0.18f) : ui::kAccent2);
        text = RGB(12, 14, 24);
    } else {
        s.FillRound(r, 7, !enabled ? ui::kCard : hot ? ui::kLineHi : on ? RGB(34, 48, 76) : ui::kCardHi);
        s.StrokeRound(r, 7, on ? ui::kAccent : ui::kLine);
        text = !enabled ? ui::kFaint : on ? RGB(206, 230, 255) : ui::kText;
    }
    s.TextCentered(primary && enabled ? g.fontBold : g.fontUi, text, r, label);
}

// A check box: a rounded square, filled with a tick when on.
void CheckBox(ui::Surface& s, int x, int y, bool on, COLORREF onColor, bool dimmed = false) {
    const RECT r{x, y, x + 14, y + 14};
    if (on) {
        s.FillRound(r, 4, dimmed ? ui::kFaint : onColor);
        s.Line(static_cast<float>(x) + 3.5f, static_cast<float>(y) + 7.5f, static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, RGB(14, 16, 24), 1.8f);
        s.Line(static_cast<float>(x) + 6.0f, static_cast<float>(y) + 10.0f, static_cast<float>(x) + 10.8f, static_cast<float>(y) + 4.2f, RGB(14, 16, 24), 1.8f);
    } else {
        s.StrokeRound(r, 4, ui::kLineHi);
    }
}

// The open / closed arrow of a group or a tree node.
void Expander(ui::Surface& s, int x, int cy, bool open, COLORREF c) {
    const float fx = static_cast<float>(x), fy = static_cast<float>(cy);
    if (open) s.FillTriangle(fx, fy - 2.0f, fx + 8.0f, fy - 2.0f, fx + 4.0f, fy + 3.0f, c);
    else s.FillTriangle(fx + 1.0f, fy - 4.0f, fx + 1.0f, fy + 4.0f, fx + 6.0f, fy, c);
}

// One slider row: label, the track filled from the card's value to the current one, the thumb, the value. False when the id names no slider.
bool DrawSliderRow(ui::Surface& s, int id, const std::string& label, int y) {
    SliderRef r;
    if (!SliderFor(id, r)) return false;
    const bool changed = TweakChanged(id);
    const COLORREF mark = changed ? ui::kAmber : ui::kAccent;
    const RECT labelClip{12, y, kTrackL - 8, y + kRowH};
    s.Text(g.fontUi, changed ? ui::kAmber : ui::kText, 12, y + 2, label, &labelClip);
    const int mid = y + kRowH / 2;
    auto xOf = [&](float v) { return kTrackL + static_cast<int>(std::lround(std::clamp((v - r.lo) / (r.hi - r.lo), 0.0f, 1.0f) * (kTrackR - kTrackL))); };
    s.FillRound(RECT{kTrackL, mid - 2, kTrackR, mid + 2}, 2, ui::kLine);
    const int a = std::min(xOf(r.def), xOf(*r.v)), b = std::max(xOf(r.def), xOf(*r.v));
    if (b > a) s.FillRound(RECT{a, mid - 2, b, mid + 2}, 2, mark);
    s.Line(static_cast<float>(xOf(r.def)), static_cast<float>(mid - 5), static_cast<float>(xOf(r.def)), static_cast<float>(mid + 5), ui::kDim, 1.0f);   // where the card has it
    s.FillDisc(static_cast<float>(xOf(*r.v)), static_cast<float>(mid), 6.5f, mark);
    s.FillDisc(static_cast<float>(xOf(*r.v)), static_cast<float>(mid), 3.5f, ui::kBg);
    char val[24];
    if (r.fmt) std::snprintf(val, sizeof val, r.fmt, static_cast<double>(*r.v));
    else if (r.percent) std::snprintf(val, sizeof val, "%d", static_cast<int>(std::lround(*r.v * 100)));
    else std::snprintf(val, sizeof val, "%+d", static_cast<int>(std::lround(*r.v)));
    s.TextRight(g.fontMono, changed ? ui::kAmber : ui::kSub, kPanel - 14, y + 3, val);
    return true;
}

// A thin scroll bar at the right edge of a list.
void ScrollBar(ui::Surface& s, int top, int bottom, int rows, int visible, float pos) {
    if (rows <= visible) return;
    const int trackH = bottom - top;
    const int thumbH = std::max(18, trackH * visible / rows);
    const float span = static_cast<float>(std::max(1, rows - visible));
    const int thumbY = top + static_cast<int>(static_cast<float>(trackH - thumbH) * std::clamp(pos / span, 0.0f, 1.0f));
    s.FillRound(RECT{kPanel - 11, thumbY, kPanel - 7, thumbY + thumbH}, 2, ui::kLineHi);
}

// The Bones tab: the skeleton switch row, the bone tree, and under it the editor of the selected bone.
void DrawBonesTab(ui::Surface& s, const RECT& clip, const PanelLayout& l, const Loaded& L) {
    const assemble::Scene& scene = L.scene;
    {   // the two switches
        const int y = l.partsTop;
        CheckBox(s, 12, y + 3, g.showBones, ui::kAmber);
        s.Text(g.fontUi, g.showBones ? ui::kAmber : ui::kText, 32, y + 2, "skeleton in the view (K)");
        CheckBox(s, 214, y + 3, g.showHelpers, ui::kAmber);
        s.Text(g.fontUi, g.showHelpers ? ui::kAmber : ui::kText, 234, y + 2, "helper bones");
    }
    const RECT listClip{6, l.boneListTop, kPanel - 6, l.boneListBottom};
    const std::vector<BoneRow> rows = BuildBoneRows(scene);
    const int visible = std::max(1, (l.boneListBottom - l.boneListTop) / kRowH);
    g.boneScroll = std::max(0, std::min(g.boneScroll, static_cast<int>(rows.size()) - visible));
    g.bonePos = std::clamp(g.bonePos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
    RECT sub;
    if (IntersectRect(&sub, &clip, &listClip)) {
        s.Begin(sub);
        const ListView v = ViewOf(l.boneListTop, l.boneListBottom, g.bonePos);
        for (int i = 0; i < v.count; ++i) {
            const int idx = v.first + i;
            if (idx >= static_cast<int>(rows.size())) break;
            const BoneRow& r = rows[static_cast<size_t>(idx)];
            const std::string& name = scene.bones[static_cast<size_t>(r.bone)].name;
            const int y = l.boneListTop - v.offset + i * kRowH;
            const bool sel = name == g.selBoneName;
            auto edit = g.boneEdits.find(name);
            const bool edited = edit != g.boneEdits.end() && !assemble::IsIdentity(edit->second);
            if (sel || idx == g.hoverRow) {
                s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, sel ? RGB(36, 50, 78) : ui::kCardHi);
                if (sel) s.FillRound(RECT{8, y + 4, 11, y + kRowH - 4}, 1, ui::kAccent);
            }
            const int x0 = 16 + std::min(r.depth, 12) * 10;
            if (r.kids) Expander(s, x0, y + kRowH / 2, r.open, ui::kAccent);
            s.Text(g.fontUi, edited ? ui::kAmber : sel ? RGB(240, 246, 255) : ui::kText, x0 + 14, y + 2, exporter::BoneLabelEn(name), &listClip);
        }
        ScrollBar(s, l.boneListTop, l.boneListBottom, static_cast<int>(rows.size()), visible, g.bonePos);
        s.Begin(clip);
    }
    // the editor
    s.Fill(RECT{10, l.boneEditTop - 3, kPanel - 10, l.boneEditTop - 2}, ui::kLine);
    const RECT headClip{10, l.boneEditTop, kPanel - 10, l.boneEditTop + kRowH};
    const int sel = g.selBoneName.empty() ? -1 : scene.boneIndex(g.selBoneName);
    {   // undo, redo, save and load the pose: always there
        auto ghost = [&](const RECT& r, const char* label, bool enabled) {
            s.FillRound(r, 5, ui::kCardHi);
            s.TextCentered(g.fontUi, enabled ? ui::kText : ui::kFaint, r, label);
        };
        ghost(l.boneUndo, "undo", !g.undoStack.empty());
        ghost(l.boneRedo, "redo", !g.redoStack.empty());
        ghost(l.boneSave, "save pose", true);
        ghost(l.boneLoad, "load pose", true);
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.boneUndo.top + 4, "Ctrl+Z / Y / S / O");
    }
    if (sel < 0) {
        s.Text(g.fontUi, ui::kSub, 12, l.boneEditTop + 2, "click a bone in the list or in the view (turn on the skeleton)", &headClip);
        return;
    }
    s.Text(g.fontBold, ui::kAccent, 12, l.boneEditTop + 2, exporter::BoneLabelEn(g.selBoneName) + "   " + g.selBoneName, &headClip);
    static const char* const labels[kBoneEditCount] = {"Move X", "Move Y", "Move Z", "Rotate X", "Rotate Y", "Rotate Z", "Scale X", "Scale Y", "Scale Z"};
    for (int k = 0; k < kBoneEditCount; ++k)
        if (TouchesY(clip, l.boneEditTop + (1 + k) * kRowH, l.boneEditTop + (2 + k) * kRowH)) DrawSliderRow(s, kBoneEdit + k, labels[k], l.boneEditTop + (1 + k) * kRowH);
    s.FillRound(l.boneResetOne, 5, ui::kCardHi);
    s.TextCentered(g.fontUi, ui::kSub, l.boneResetOne, "reset this bone");
    s.FillRound(l.boneResetAll, 5, ui::kCardHi);
    s.TextCentered(g.fontUi, ui::kSub, l.boneResetAll, "reset every bone");
    s.TextRight(g.fontSmall, ui::kDim, kPanel - 12, l.boneResetOne.top + 4, "wheel: fine step");
}

void DrawHeader(ui::Surface& s, const RECT& clip, const PanelLayout& l, const std::shared_ptr<Loaded>& L, const std::string& modsInfo) {
    (void)modsInfo;
    s.FillGradient(RECT{0, 0, kPanel - 1, kHeaderBottom - 2}, 0, RGB(18, 25, 44), RGB(30, 22, 52));
    s.FillGradient(RECT{0, kHeaderBottom - 2, kPanel - 1, kHeaderBottom}, 0, ui::kAccent, ui::kAccent2);
    s.Logo(l.logo, 12);
    s.StrokeRound(l.logo, 12, ui::kLineHi);
    const int tx = 80, right = kPanel - 12;
    const RECT line{tx, 0, right, kHeaderBottom};
    const int cardW = s.TextWidth(g.fontTitle, "Card");
    s.Text(g.fontTitle, ui::kText, tx, 8, "Card");
    s.Text(g.fontTitle, ui::kAccent, tx + cardW, 8, "Bridge");
    s.TextRight(g.fontSmall, ui::kDim, right, 14, std::string("v") + CARDBRIDGE_VERSION);
    std::string title = L ? L->title : std::string();
    if (L && !L->scene.parts.empty()) {
        if (title.empty()) title = "(unnamed card)";
        s.Text(g.fontBold, ui::kText, tx, 35, title, &line);
        size_t listed = 0;
        for (const auto& p : L->scene.parts) listed += !p.underPiece;
        s.Text(g.fontSmall, ui::kSub, tx, 52,
               Count(Triangles(L->scene)) + " triangles  ·  " + std::to_string(L->scene.materials.size()) + " materials  ·  " + std::to_string(listed) + " parts", &line);
        size_t warn = 0, err = 0;
        for (const auto& i : L->scene.issues) (i.severity == "error" ? err : warn)++;
        s.Text(g.fontSmall, warn + err ? ui::kAmber : ui::kGood, tx, 68, std::to_string(err) + " errors, " + std::to_string(warn) + " warnings  [I]", &line);
        std::string mode = g.showExp && g.expOk ? (g.expGrade ? "as exported (graded)" : "as exported") : g.gpuOk ? "Direct3D 11" : "software";
        mode += g.textured ? "" : "  ·  untextured";
        s.TextRight(g.fontSmall, g.gpuOk ? ui::kAccent : ui::kSub, right, 68, mode);
    } else if (L) {
        // The assembly threw (unsupported card, broken container): show why instead of a row of zeros.
        s.Text(g.fontBold, ui::kBad, tx, 33, "could not assemble this card", &line);
        if (!L->notes.empty()) s.Text(g.fontSmall, RGB(226, 184, 184), tx, 52, ClipText(L->notes[0], 54), &line);
        s.Text(g.fontSmall, ui::kSub, tx, 68, "details in the log below  ·  O opens another card", &line);
    } else {
        s.Text(g.fontBold, ui::kText, tx, 33, "Drop a card here, or press O", &line);
        s.Text(g.fontSmall, g.busy ? ui::kAccent : ui::kSub, tx, 52, ClipText(g.status, 54), &line);
        s.Text(g.fontSmall, ui::kDim, tx, 68, "library: " + (g.settings.library.empty() ? std::string("(not set)") : Shorten(g.settings.library, 42)), &line);
    }
    (void)clip;
}

void DrawPanel(ui::Surface& s, const RECT& clip) {
    std::shared_ptr<Loaded> L = Current();
    std::string modsInfo;
    { std::lock_guard<std::recursive_mutex> lk(g.mu); modsInfo = g.modsInfo; }
    const int H = s.height();
    const PanelLayout l = Lay(H);
    double lapT = NowMs();
    auto lap = [&](int i) { const double t = NowMs(); g.prof.sec[i] += t - lapT; lapT = t; };
    s.Begin(clip);
    s.Fill(clip, ui::kBg);
    lap(0);
    if (clip.top < kHeaderBottom) g.prof.sec[7] += 1.0;

    // header and toolbar ---------------------------------------------------------------------------------------------
    if (TouchesY(clip, 0, kHeaderBottom)) DrawHeader(s, clip, l, L, modsInfo);
    if (TouchesY(clip, l.open.top, l.open.bottom)) {
        Button(s, l.open, "Open", g.hoverButton == 1, false, true);
        Button(s, l.exportPmx, g.exporting ? "Exporting..." : "Export PMX", g.hoverButton == 2, false, L != nullptr && !g.busy && !g.exporting, true);
        Button(s, l.issues, "Issues", g.hoverButton == 3, g.showIssues, L != nullptr);
        Button(s, l.reset, "Reset view", g.hoverButton == 4, false, L != nullptr);
        Button(s, l.all, "All", g.hoverButton == 5, false, L != nullptr);
    }

    lap(1);
    // outfit: the counter and the game's name for the coordinate, over a row of slots that can be clicked (Left / Right still step) ----
    if (TouchesY(clip, l.outfitNameTop - 4, l.outfitChipsTop + kOutfitChipH + 4)) {
        int shown = L ? L->outfit : -1;
        if (g.busy && g.loadingOutfit >= 0) shown = g.loadingOutfit;
        if (L && L->outfits > 0) shown = std::clamp(shown, 0, L->outfits - 1);
        s.Text(g.fontBold, ui::kAccent, 12, l.outfitNameTop, "OUTFIT");
        if (L && L->outfits > 0) {
            // Hovering a slot previews that outfit's name without loading it.
            const int preview = g.hoverOutfit >= 0 && g.hoverOutfit < L->outfits ? g.hoverOutfit : shown;
            const COLORREF nameColor = g.busy ? ui::kAccent : g.hoverOutfit >= 0 ? RGB(246, 249, 255) : ui::kText;
            s.Text(g.fontBold, nameColor, 74, l.outfitNameTop, ClipText(L->card.outfitName(preview), 30));
            char count[32];
            std::snprintf(count, sizeof count, "%d / %d", shown + 1, L->outfits);
            s.TextRight(g.fontBold, ui::kAmber, kPanel - 12, l.outfitNameTop, count);
        } else {
            s.Text(g.fontUi, ui::kDim, 74, l.outfitNameTop, g.busy ? "loading ..." : "no card");
        }
        const int chips = L ? std::min(L->outfits, OutfitChipsPerRow()) : 0;
        for (int i = 0; i < chips; ++i) Button(s, OutfitChipRect(l, i), std::to_string(i + 1), g.hoverOutfit == i, i == shown, L != nullptr && !g.busy);
    }

    lap(2);
    // tabs ----------------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.tabs[0].top - 2, l.tabs[0].bottom + 2)) {
        for (int i = 0; i < TabCount; ++i) {
            const RECT& t = l.tabs[i];
            const bool act = !g.showIssues && g.tab == i, hot = g.hoverButton == 10 + i;
            if (act) {
                s.FillRound(t, 7, ui::kCardHi);
                s.FillGradient(RECT{t.left + 10, t.bottom - 3, t.right - 10, t.bottom - 1}, 1, ui::kAccent, ui::kAccent2);
            } else if (hot && L) {
                s.FillRound(t, 7, ui::kCard);
            }
            s.TextCentered(act ? g.fontBold : g.fontUi, (!L && i != TabSettings) ? ui::kFaint : act ? ui::kText : hot ? ui::kText : ui::kSub, t, kTabNames[i]);
        }
        if (L) {
            std::string right;
            if (g.showIssues) right = std::to_string(L->scene.issues.size()) + " issues";
            else if (g.busy && g.tab != TabParts) right = "shaping";
            else if (g.tweakPending) right = "applying";
            else if (g.tab == TabBones) right = std::to_string(L->scene.bones.size()) + " bones";
            else if (g.tab == TabParts) right = std::to_string(L->scene.parts.size()) + " parts";
            else if (g.tab == TabSettings) right = "saved";
            else right = AnyTweak() ? "tweaked" : "as card";
            s.TextRight(g.fontSmall, g.busy || g.tweakPending ? ui::kAccent : AnyTweak() && g.tab != TabParts && g.tab != TabBones && g.tab != TabSettings ? ui::kAmber : ui::kDim, kPanel - 10, l.partsHeadingTop, right);
        }
    }

    lap(3);
    // the list: parts, issues, sliders or bones ---------------------------------------------------------------------------
    const RECT listCard{6, l.partsTop - 4, kPanel - 6, l.partsBottom + 3};
    if (Touches(clip, listCard)) {
        s.FillRound(listCard, 8, ui::kCard);
        const RECT listClip{6, l.partsTop, kPanel - 6, l.partsBottom};
        RECT sub;
        const bool inList = IntersectRect(&sub, &clip, &listClip) != 0;
        const RECT clipText{10, l.partsTop, kPanel - 14, l.partsBottom};
        if (!L && g.tab != TabSettings) {
            s.Text(g.fontUi, g.busy ? ui::kAccent : ui::kDim, 14, l.partsTop + 4, g.busy ? "loading the card ..." : "no card loaded");
        } else if (g.showIssues) {
            std::vector<std::pair<std::string, LogKind>> lines;
            for (const auto& i : L->scene.issues) {
                std::string t = (i.part.empty() ? "" : "[" + i.part + "] ") + i.message;
                const LogKind k = i.severity == "error" ? LogKind::Error : LogKind::Warn;
                while (t.size() > 50) {
                    size_t cut = t.rfind(' ', 50);
                    if (cut == std::string::npos || cut < 20) cut = 50;
                    lines.push_back({t.substr(0, cut), k});
                    t = t.substr(cut + (t[cut] == ' ' ? 1 : 0));
                }
                lines.push_back({t, k});
            }
            const int visible = std::max(1, (l.partsBottom - l.partsTop) / 16);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(lines.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(lines.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos, 16);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(lines.size())) break;
                    s.Text(g.fontLog, KindColor(lines[static_cast<size_t>(idx)].second), 14, l.partsTop - v.offset + i * 16, lines[static_cast<size_t>(idx)].first, &clipText);
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(lines.size()), visible, g.partPos);
                s.Begin(clip);
            }
        } else if (g.tab == TabBones) {
            DrawBonesTab(s, clip, l, *L);
        } else if (g.tab != TabParts) {
            const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
            const int visible = VisibleRows(l);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(rows.size())) break;
                    const TweakRow& row = rows[static_cast<size_t>(idx)];
                    const int y = l.partsTop - v.offset + i * kRowH;
                    if (!TouchesY(sub, y, y + kRowH)) continue;
                    if (idx == g.hoverRow && row.kind != 4 && !(row.kind == 0 && row.id < 0)) s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, ui::kCardHi);
                    if (row.kind == 0) {
                        std::string name = row.label;
                        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        s.Text(g.fontBold, ui::kAccent, 14, y + 3, name, &clipText);
                        bool any = false;
                        for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) any = any || TweakChanged(rows[k].id);
                        if (row.id >= 0) s.TextRight(g.fontSmall, any ? ui::kAmber : ui::kFaint, kPanel - 16, y + 4, "reset");
                    } else if (row.kind == 4) {
                        s.Text(g.fontSmall, ui::kDim, 14, y + 4, row.label, &clipText);
                    } else if (row.kind == 5) {
                        const RECT btn{12, y + 1, kPanel - 16, y + kRowH - 1};
                        s.FillRound(btn, 5, idx == g.hoverRow ? ui::kLineHi : ui::kCardHi);
                        s.Text(g.fontUi, ui::kText, 20, y + 2, row.label, &clipText);
                    } else if (row.kind == 2) {
                        const bool on = TweakChanged(row.id);
                        const bool failed = row.id >= kLayerBase && row.id < kBoneEdit && !g.gpu.layerOk(static_cast<size_t>(row.id - kLayerBase));
                        CheckBox(s, 14, y + 3, on, failed ? ui::kBad : ui::kAmber);
                        s.Text(g.fontUi, failed ? ui::kBad : on ? ui::kAmber : ui::kText, 36, y + 2, failed ? row.label + "  (does not compile)" : row.label, &clipText);
                    } else {
                        DrawSliderRow(s, row.id, row.label, y);
                    }
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(rows.size()), visible, g.partPos);
                s.Begin(clip);
            }
        } else {
            const std::vector<PartRow> rows = BuildRows(L->scene);
            const int visible = VisibleRows(l);
            g.partScroll = std::max(0, std::min(g.partScroll, static_cast<int>(rows.size()) - visible));
            g.partPos = std::clamp(g.partPos, 0.0f, static_cast<float>(std::max(0, static_cast<int>(rows.size()) - visible)));
            if (inList) {
                s.Begin(sub);
                const ListView v = ViewOf(l.partsTop, l.partsBottom, g.partPos);
                for (int i = 0; i < v.count; ++i) {
                    const int idx = v.first + i;
                    if (idx >= static_cast<int>(rows.size())) break;
                    const PartRow& row = rows[static_cast<size_t>(idx)];
                    const int y = l.partsTop - v.offset + i * kRowH;
                    if (!TouchesY(sub, y, y + kRowH)) continue;
                    const bool hot = idx == g.hoverRow;
                    if (row.part < 0) {
                        if (hot) s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, ui::kCardHi);
                        std::string name = row.group;
                        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        const bool collapsed = g.collapsedGroups.count(row.group) != 0;
                        int count = 0;
                        for (const auto& p : L->scene.parts) if (p.group == row.group && !p.underPiece) ++count;
                        Expander(s, 14, y + kRowH / 2, !collapsed, ui::kAccent);
                        s.Text(g.fontBold, ui::kAccent, 30, y + 3, name);
                        s.TextRight(g.fontSmall, ui::kDim, kPanel - 16, y + 4, std::to_string(count));
                    } else {
                        const auto& p = L->scene.parts[static_cast<size_t>(row.part)];
                        const bool off = static_cast<size_t>(row.part) < g.hidden.size() && g.hidden[static_cast<size_t>(row.part)];
                        const bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
                        if (hot) {
                            s.FillRound(RECT{8, y + 1, kPanel - 8, y + kRowH - 1}, 5, RGB(34, 46, 70));
                            s.FillRound(RECT{8, y + 4, 11, y + kRowH - 4}, 1, ui::kAccent);
                        }
                        const COLORREF col = off ? ui::kDim : !ok ? ui::kBad : ui::kText;
                        CheckBox(s, 16, y + 3, !off, ok ? ui::kAccent : ui::kBad);
                        std::string name = p.nudeSwitch ? std::string("nude  (whole body, under clothes too)") : p.id;
                        if (!ok) name += "  (" + p.status + ")";
                        s.Text(g.fontMono, col, 38, y + 3, name, &clipText);
                        const std::string tris = Count(static_cast<size_t>(row.part) < L->partTriangles.size() ? L->partTriangles[static_cast<size_t>(row.part)] : 0);
                        s.TextRight(g.fontSmall, off ? ui::kFaint : ui::kDim, kPanel - 16, y + 4, tris);
                    }
                }
                ScrollBar(s, l.partsTop, l.partsBottom, static_cast<int>(rows.size()), visible, g.partPos);
                s.Begin(clip);
            }
        }
    }

    lap(4);
    // log --------------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.logTop, l.logBottom)) {
        const RECT logCard{6, l.logTop, kPanel - 6, l.logBottom};
        s.FillRound(logCard, 8, RGB(11, 13, 19));
        s.StrokeRound(logCard, 8, ui::kLine);
        s.Text(g.fontBold, ui::kAccent, 14, l.logTop + 5, "LOG");
        s.TextRight(g.fontSmall, ui::kDim, kPanel - 14, l.logTop + 7, "clear");
        std::lock_guard<std::mutex> lk(g.logMu);
        const int lineH = 15, top = l.logTop + 26;
        const int visible = std::max(1, (l.logBottom - 4 - top) / lineH);
        g.logScroll = std::max(0, std::min(g.logScroll, static_cast<int>(g.logLines.size()) - visible));
        const int end = static_cast<int>(g.logLines.size()) - g.logScroll;
        const int start = std::max(0, end - visible);
        const RECT lc{10, top, kPanel - 10, l.logBottom - 3};
        if (g.logLines.empty()) s.Text(g.fontLog, ui::kDim, 14, top, "no activity yet", &lc);
        for (int i = start; i < end; ++i) {
            const LogLine& line = g.logLines[static_cast<size_t>(i)];
            const int y = top + (i - start) * lineH;
            s.Text(g.fontLog, ui::kFaint, 14, y, line.time, &lc);
            s.Text(g.fontLog, KindColor(line.kind), 78, y, line.text, &lc);
        }
    }

    lap(5);
    // footer -----------------------------------------------------------------------------------------------------------
    if (TouchesY(clip, l.footerTop, H)) {
        s.Text(g.fontSmall, ui::kDim, 12, l.footerTop + 3, ClipText("E export  O open  A all  T tex  H shader  G grade  X as exported", 64));
        std::string hint = "1-7 outfit · L lib · K bones · P pose · Y physics · N shake";
        if (g.busy) hint = "working ...";
        else if (g.exporting) hint = "exporting PMX ...";
        if (L && g.hoverPart >= 0 && static_cast<size_t>(g.hoverPart) < L->scene.parts.size()) {
            const auto& p = L->scene.parts[static_cast<size_t>(g.hoverPart)];
            hint = p.id + "  ·  " + p.status;
            if (static_cast<size_t>(g.hoverPart) < L->partTriangles.size()) hint += "  ·  " + Count(L->partTriangles[static_cast<size_t>(g.hoverPart)]) + " tris";
            if (!p.modName.empty()) hint += "  ·  " + p.modName;
            if (static_cast<size_t>(g.hoverPart) < g.hidden.size() && g.hidden[static_cast<size_t>(g.hoverPart)]) hint += "  ·  hidden";
        } else if (!modsInfo.empty() && !L) {
            hint = modsInfo;
        }
        s.Text(g.fontSmall, g.busy ? ui::kAccent : g.exporting ? ui::kAmber : ui::kSub, 12, l.footerTop + 17, ClipText(hint, 64));
    }
    s.Fill(RECT{kPanel - 1, clip.top, kPanel, clip.bottom}, ui::kLine);
    s.End();
    lap(6);
}

void Paint(HWND hwnd) {
    // The area to paint as separate rectangles (rcPaint is only their bounding box, which would redraw all the panel between a hovered
    // row and the footer): read before BeginPaint, which clears it.
    std::vector<RECT> parts;
    if (HRGN rgn = CreateRectRgn(0, 0, 0, 0)) {
        if (GetUpdateRgn(hwnd, rgn, FALSE) > NULLREGION) {
            const DWORD bytes = GetRegionData(rgn, 0, nullptr);
            std::vector<char> buf(bytes);
            if (bytes && GetRegionData(rgn, bytes, reinterpret_cast<RGNDATA*>(buf.data()))) {
                const RGNDATA* rd = reinterpret_cast<const RGNDATA*>(buf.data());
                const RECT* r = reinterpret_cast<const RECT*>(rd->Buffer);
                parts.assign(r, r + rd->rdh.nCount);
            }
        }
        DeleteObject(rgn);
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    if (parts.empty() || parts.size() > 12) parts.assign(1, ps.rcPaint);   // a complex region: its bounding box will do
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int W = std::max<int>(0, static_cast<int>(rc.right) - kPanel), H = rc.bottom;
    const double tPaint = NowMs();
    const RECT viewport{kPanel, 0, rc.right, rc.bottom};
    const RECT panelRect{0, 0, kPanel, H};
    bool viewDamaged = false;
    for (const RECT& p : parts) { RECT t; viewDamaged = viewDamaged || IntersectRect(&t, &p, &viewport) != 0; }
    if (g.dirty || g.fw != W || g.fh != H) {
        Render(W, H);
        g.dirty = false;
        g.prof.render += NowMs() - tPaint;
        ++g.prof.renders;
    }
    const double tBlit = NowMs();
    if (viewDamaged && !g.frame.empty() && g.fw == W && g.fh == H && W > 0 && H > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        StretchDIBits(dc, kPanel, 0, W, H, 0, 0, W, H, g.frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    const double tPanel = NowMs();
    g.prof.blit += tPanel - tBlit;
    // The panel is drawn off screen, only where it changed, then copied.
    if (g.panel.Resize(kPanel, H)) {   // new surface: blank, so all of it is drawn, though only the damaged part needs showing
        DrawPanel(g.panel, panelRect);
        for (const RECT& p : parts) { RECT t; if (IntersectRect(&t, &p, &panelRect)) g.panel.Present(dc, t, t.left, t.top); }
    } else {
        for (const RECT& p : parts) {
            RECT t;
            if (!IntersectRect(&t, &p, &panelRect)) continue;
            const double tDraw = NowMs();
            DrawPanel(g.panel, t);
            const double tCopy = NowMs();
            g.prof.panelDraw += tCopy - tDraw;
            g.panel.Present(dc, t, t.left, t.top);
            g.prof.panelCopy += NowMs() - tCopy;
        }
    }
    g.prof.panel += NowMs() - tPanel;
    ++g.prof.paints;
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
    g.partPos = 0;
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
    if (row.kind == 5) { if (!right) SettingsAction(row.id); return; }   // outside the lock: it opens dialogs
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    if (row.kind == 4 || (row.kind == 0 && row.id < 0)) return;
    if (row.kind == 2 && row.id >= kLayerBase && row.id < kBoneEdit) {   // a shader layer: no re-assembly, just another frame
        const size_t li = static_cast<size_t>(row.id - kLayerBase);
        g.gpu.SetLayerEnabled(li, !g.gpu.layerEnabled(li));
        Log("layer " + row.label + (g.gpu.layerEnabled(li) ? ": on" : g.gpu.layerOk(li) ? ": off" : ": does not compile"), g.gpu.layerOk(li) ? LogKind::Info : LogKind::Warn);
        if (!g.gpuOk) Log("layers need the Direct3D 11 view", LogKind::Warn);
        g.dirty = true;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    if (row.kind == 0) {
        bool any = false;
        for (size_t k = static_cast<size_t>(idx) + 1; k < rows.size() && rows[k].kind != 0; ++k) {
            any = any || TweakChanged(rows[k].id);
            ResetTweak(rows[k].id);
        }
        if (any) {
            Log("reset " + row.label);
            if (!ReshapeNow(true)) ScheduleApplyTweaks();
        }
        return;
    }
    if (row.kind == 2 && row.id == kPhysics) {
        SetPhysics(right ? false : !g.physics);
        InvalidatePanel();
        return;
    }
    if (row.kind == 2 && row.id == kAPose) {
        g.aPose = right ? false : !g.aPose;
        Log(std::string("A-pose: ") + (g.aPose ? "on" : "off"));
        ApplyPoseLive();
        InvalidatePanel();
        return;
    }
    if (row.kind == 2) {
        if (right) ResetTweak(row.id); else g.tw.nippleInside = !g.tw.nippleInside;
        Log(row.label + ": " + (g.tw.nippleInside ? "on" : "off"));
        TweakEdited(kNippleIn, true);
        return;
    }
    SliderRef r;
    if (!SliderFor(row.id, r)) return;
    if (right) {
        if (TweakChanged(row.id)) { ResetTweak(row.id); Log("reset " + row.label); TweakEdited(row.id, true); }
        return;
    }
    if (x < kTrackL - 8) return;
    g.sliderDrag = row.id;
    *r.v = SliderValueAt(r, x);
    TweakEdited(row.id, false);
    SetCapture(g.hwnd);
    InvalidatePanel();
}

// A click in the Bones tab: the switch row, a tree row (arrow opens, name selects, right-click resets that bone), the editor's fields.
void BonesTabClick(int x, int y, const PanelLayout& l, bool right) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const assemble::Scene& scene = L->scene;
    const POINT pt{x, y};
    if (y >= l.partsTop && y < l.partsTop + kRowH) {
        if (right) return;
        if (PtInRect(&l.boneSkeleton, pt)) { g.showBones = !g.showBones; Log(g.showBones ? "skeleton: on (click a bone to select it)" : "skeleton: off"); }
        else { g.showHelpers = !g.showHelpers; g.boneScroll = 0; }
        g.dirty = true;
        InvalidateAll();
        return;
    }
    if (y >= l.boneListTop && y < l.boneListBottom) {
        const std::vector<BoneRow> rows = BuildBoneRows(scene);
        const int idx = (y - l.boneListTop + static_cast<int>(std::lround(g.bonePos * kRowH))) / kRowH;
        if (idx < 0 || static_cast<size_t>(idx) >= rows.size()) return;
        const BoneRow& r = rows[static_cast<size_t>(idx)];
        const std::string& name = scene.bones[static_cast<size_t>(r.bone)].name;
        const int arrowX = 16 + std::min(r.depth, 12) * 10;
        if (right) { g.boneEdits.erase(name); Log("reset " + name); ApplyPoseLive(); }
        else if (r.kids && x >= arrowX - 3 && x < arrowX + 12) { if (!g.openBones.erase(name)) g.openBones.insert(name); }
        else g.selBoneName = name;
        g.dirty = true;
        InvalidateAll();
        return;
    }
    if (y < l.boneEditTop) return;
    const int row = (y - l.boneEditTop) / kRowH;
    if (row == kBoneEditRows - 1 && !right) {   // undo / redo / save / load
        if (PtInRect(&l.boneUndo, pt)) UndoPose(false);
        else if (PtInRect(&l.boneRedo, pt)) UndoPose(true);
        else if (PtInRect(&l.boneSave, pt)) SavePoseFile();
        else if (PtInRect(&l.boneLoad, pt)) LoadPoseFile();
        InvalidatePanel();
        return;
    }
    if (g.selBoneName.empty()) return;
    if (row >= 1 && row <= kBoneEditCount) {
        const int id = kBoneEdit + row - 1;
        SliderRef r;
        if (!SliderFor(id, r)) return;
        if (right) { *r.v = r.def; ApplyPoseLive(); InvalidatePanel(); return; }
        if (x < kTrackL - 8) return;
        g.sliderDrag = id;
        *r.v = SliderValueAt(r, x);
        SetCapture(g.hwnd);
        ApplyPoseLive();
        InvalidatePanel();
    } else if (row == kBoneEditRows - 2 && !right) {
        if (PtInRect(&l.boneResetOne, pt)) { g.boneEdits.erase(g.selBoneName); Log("reset " + g.selBoneName); }
        else if (PtInRect(&l.boneResetAll, pt)) { g.boneEdits.clear(); Log("reset every bone"); }
        else return;
        ApplyPoseLive();
        InvalidatePanel();
    }
}

void ChooseMods() {
    std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu mods folder");
    if (d.empty()) return;
    g.settings.mods = d;
    g.settings.noMods = false;
    SaveSettings(g.settings);
    g.resolver.reset();   // the next load indexes the new folder
    Log("mods folder: " + d, LogKind::Good);
    if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
    else InvalidatePanel();
}

// Builds the asset library from the user's own Koikatsu folder (the only thing that ever reads the game), on a thread, with the progress in
// the log. When it is done WM_LIBRARY_DONE opens the card that was waiting.
void StartLibraryImport(const std::string& game) {
    if (g.busy.exchange(true)) { Log("still busy; wait for it to finish", LogKind::Warn); return; }
    const std::string dest = DefaultLibraryDir();
    g.status = "copying the game data into the library ...";
    Log("building the asset library from " + game, LogKind::Info);
    InvalidatePanel();
    std::thread([game, dest]() {
        bool ok = false;
        try {
            library::ImportOptions o;
            o.gameRoot = game;
            o.libraryRoot = dest;
            o.profile = library::Profile::Full;
            auto r = library::ImportLibrary(o, [](size_t done, size_t total) { if (done % 25 == 0 || done == total) Log("  copying game data " + std::to_string(done) + " / " + std::to_string(total)); });
            for (const auto& issue : r.issues) Log("  " + issue, LogKind::Warn);
            ok = r.files > 0 && library::CheckLibrary(dest, false).ok;
            Log(ok ? "asset library ready: " + dest : std::string("the library import failed: is that the Koikatsu folder (it has abdata inside)?"), ok ? LogKind::Good : LogKind::Error);
        } catch (const std::exception& e) {
            Log(std::string("the library import failed: ") + e.what(), LogKind::Error);
        }
        g.busy = false;
        PostMessageW(g.hwnd, WM_LIBRARY_DONE, 0, ok ? 1 : 0);
    }).detach();
}

// The all-in-one exe carries the library inside itself: unpack it once into the viewer's own folder (no dialogs).
void StartEmbeddedUnpack() {
    if (g.busy.exchange(true)) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string exePath = Slashes(WideToUtf8(exe));
    const std::string dest = DefaultLibraryDir();
    g.status = "unpacking the game data that comes with CardBridge (first run only) ...";
    Log("unpacking the library that is inside this exe into " + dest);
    InvalidatePanel();
    std::thread([exePath, dest]() {
        bool ok = false;
        std::string why;
        try {
            const std::string parent = dest.substr(0, dest.find_last_of('/'));
            ok = library::ExtractEmbeddedLibrary(exePath, parent, [](size_t done, size_t total) { if (done % 25 == 0 || done == total) Log("  unpacking " + std::to_string(done) + " / " + std::to_string(total)); }, &why)
                 && library::CheckLibrary(dest, false).ok;
        } catch (const std::exception& e) {
            why = e.what();
        }
        Log(ok ? "asset library ready: " + dest : "could not unpack the library: " + why, ok ? LogKind::Good : LogKind::Error);
        g.busy = false;
        PostMessageW(g.hwnd, WM_LIBRARY_DONE, 0, ok ? 1 : 0);
    }).detach();
}

// No library yet (first run) or the user asked for one (L): build it from the game folder, or point at one that exists.
void SetupLibrary(bool offerExisting) {
    if (std::getenv("CARDBRIDGE_TEST_HOOKS") && std::getenv("CARDBRIDGE_SETUP_GAME")) { StartLibraryImport(Slashes(std::getenv("CARDBRIDGE_SETUP_GAME"))); return; }   // scripted runs: no dialogs
    if (offerExisting) {
        const int a = MessageBoxW(g.hwnd, L"Yes: build the library from your Koikatsu folder (about 0.9 GB, copied once).\nNo: use a library folder that already exists.", L"CardBridge - asset library", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDNO) {
            const std::string d = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder");
            if (d.empty()) return;
            g.settings.library = d;
            SaveSettings(g.settings);
            Log("asset library: " + d, LogKind::Good);
            if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
            else InvalidatePanel();
            return;
        }
    } else {
        MessageBoxW(g.hwnd, L"CardBridge needs a copy of the base-game data (skeleton, body, shaders, hair and clothes: about 0.9 GB).\nIt is copied once from your own Koikatsu install and never needs the game again.\n\nNext: choose your Koikatsu folder (the one that contains \"abdata\").", L"CardBridge - first run", MB_OK | MB_ICONINFORMATION);
    }
    const std::string game = PickFolder(g.hwnd, L"Choose your Koikatsu folder (it contains abdata)");
    if (game.empty()) { g.status = "no asset library: press L to set one up"; InvalidatePanel(); return; }
    StartLibraryImport(game);
}

void ChooseLibrary() { SetupLibrary(true); }

// A small modal box with one text field; empty when cancelled.
std::string PromptText(HWND owner, const wchar_t* title, const wchar_t* label, const std::string& initial) {
    struct State { std::wstring text; bool ok = false, done = false; HWND edit = nullptr; };
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            State* st = reinterpret_cast<State*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            switch (m) {
            case WM_CREATE: SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams)); return 0;
            case WM_COMMAND:
                if (st && LOWORD(w) == IDOK) { wchar_t buf[512] = L""; GetWindowTextW(st->edit, buf, 512); st->text = buf; st->ok = true; st->done = true; DestroyWindow(h); }
                else if (st && LOWORD(w) == IDCANCEL) { st->done = true; DestroyWindow(h); }
                return 0;
            case WM_CLOSE: if (st) st->done = true; DestroyWindow(h); return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CardBridgePrompt";
        RegisterClassW(&wc);
        registered = true;
    }
    State st;
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int W = 420, H = 130;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CardBridgePrompt", title, WS_POPUP | WS_CAPTION | WS_SYSMENU, orc.left + (orc.right - orc.left - W) / 2, orc.top + (orc.bottom - orc.top - H) / 2, W, H, owner, nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return {};
    HWND lab = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE, 12, 10, W - 36, 18, dlg, nullptr, nullptr, nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(initial).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 32, W - 36, 24, dlg, nullptr, nullptr, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 36 - 160, 66, 76, 26, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - 36 - 78, 66, 76, 26, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
    for (HWND c : {lab, st.edit, ok, cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g.fontUi), TRUE);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);
    MSG m;
    while (!st.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return st.ok ? WideToUtf8(st.text) : std::string();
}

// (Re)loads the shader layers from the default places and the user's extra folder.
void ReloadLayers(bool announce) {
    g.layerDefs = exporter::LoadShaderLayers(LayerFolders(g.settings.library, g.settings.layers));
    std::string report;
    g.gpu.SetLayers(g.layerDefs, &report);
    if (!g.layerDefs.empty()) {
        std::string names;
        for (const auto& d : g.layerDefs) names += (names.empty() ? "" : ", ") + d.name;
        Log(std::to_string(g.layerDefs.size()) + " shader layer(s): " + names + "  (V toggles, Look tab lists them)", LogKind::Good);
        for (size_t at = 0; at < report.size();) {
            size_t end = report.find('\n', at);
            if (end == std::string::npos) end = report.size();
            if (end > at) Log("layer failed: " + report.substr(at, end - at), LogKind::Warn);
            at = end + 1;
        }
    } else if (announce) {
        Log("no shader layers found", LogKind::Warn);
    }
    g.dirty = true;
}

// The Setup tab: what a person may need to change. Each saves to viewer.json at once.
void SettingsAction(int id) {
    auto reloadCard = [] { g.resolver.reset(); if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true); };
    switch (id) {
    case kSetLibChoose: {
        const std::string d = PickFolder(g.hwnd, L"Choose the CardBridge asset library folder (it contains abdata)");
        if (d.empty()) break;
        if (!Exists(d + "/abdata")) { Log("that folder has no abdata inside: " + d, LogKind::Error); break; }
        g.settings.library = d;
        SaveSettings(g.settings);
        Log("asset library: " + d, LogKind::Good);
        reloadCard();
        break;
    }
    case kSetLibBuild: {
        const std::string game = PickFolder(g.hwnd, L"Choose your Koikatsu folder (it contains abdata)");
        if (!game.empty()) StartLibraryImport(game);
        break;
    }
    case kSetModsChoose: ChooseMods(); break;
    case kSetModsReindex:
        fileio::RemoveFile(ModCachePath());
        Log("the mod index will be built again", LogKind::Info);
        reloadCard();
        break;
    case kSetModsClear:
        g.settings.mods.clear();
        g.settings.noMods = true;
        SaveSettings(g.settings);
        Log("no mods folder: base-game items only", LogKind::Info);
        reloadCard();
        break;
    case kSetAuthor: {
        const std::string cur = g.settings.author.empty() ? std::string(CARDBRIDGE_AUTHOR) : g.settings.author;
        const std::string a = PromptText(g.hwnd, L"CardBridge - author", L"Name written into the model information of every exported PMX:", cur);
        if (a.empty()) break;
        g.settings.author = a == CARDBRIDGE_AUTHOR ? std::string() : a;
        SaveSettings(g.settings);
        Log("author: " + a, LogKind::Good);
        break;
    }
    case kSetExportDir: {
        const std::string d = PickFolder(g.hwnd, L"Choose the folder exports go into");
        if (d.empty()) break;
        g.settings.exportDir = d;
        SaveSettings(g.settings);
        Log("exports go to " + d, LogKind::Good);
        break;
    }
    case kSetExportAsk:
        g.settings.exportDir.clear();
        SaveSettings(g.settings);
        Log("the export folder is asked every time");
        break;
    case kSetLayers: {
        const std::string d = PickFolder(g.hwnd, L"Choose a folder of shader layers (.hlsl files)");
        if (d.empty()) break;
        g.settings.layers = d;
        SaveSettings(g.settings);
        ReloadLayers(true);
        break;
    }
    case kSetLayersDefault:
        g.settings.layers.clear();
        SaveSettings(g.settings);
        ReloadLayers(true);
        break;
    case kSetOpenFolder: {
        const std::string dir = SettingsPath().substr(0, SettingsPath().find_last_of('/'));
        fileio::CreateDirectories(dir);
        ShellExecuteW(g.hwnd, L"open", Utf8ToWide(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kSetForget:
        g.settings = Settings();
        SaveSettings(g.settings);
        Log("settings forgotten: the library and the mods folder are looked for again at the next start (the library on disk stays)", LogKind::Warn);
        break;
    default: break;
    }
    InvalidatePanel();
}

// ---- one picture -------------------------------------------------------------------------------------------------------
// A slider's row repaints while it is dragged (and the tab heading, whose "tweaked" label follows it); nothing else.
void InvalidateSliderRow(int id) {
    const PanelLayout l = Lay(ClientHeight());
    if (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) {
        const int y = l.boneEditTop + (1 + id - kBoneEdit) * kRowH;
        InvalidatePanelBand(y, y + kRowH);
        return;
    }
    const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == 1 && rows[i].id == id) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - ScrollPx(g.partPos);
            InvalidatePanelBand(std::max(l.partsTop, y), std::min(l.partsBottom, y + kRowH));
            break;
        }
    RECT heading{kPanel - 80, l.partsHeadingTop - 2, kPanel, l.partsHeadingTop + 16};
    InvalidateRect(g.hwnd, &heading, FALSE);
}

// Mouse movement during a drag is only noted by the window procedure (dragPending); this applies it, once per picture, so a fast mouse
// cannot queue up work faster than it can be drawn.
void ApplyPending() {
    if (!g.dragPending) return;
    g.dragPending = false;
    if (g.sliderDrag >= 0) {
        SliderRef r;
        if (SliderFor(g.sliderDrag, r)) { std::lock_guard<std::recursive_mutex> lk(g.mu); *r.v = SliderValueAt(r, g.dragX); }
        TweakEdited(g.sliderDrag, false);
        InvalidateSliderRow(g.sliderDrag);
    } else if (g.drag == 3 && (g.boneDragDx != 0.0f || g.boneDragDy != 0.0f)) {
        const float dx = g.boneDragDx, dy = g.boneDragDy;
        g.boneDragDx = g.boneDragDy = 0.0f;
        DragBone(g.dragBone, dx, dy, g.dragMove);
        InvalidateList();
    }
}

// Moves a scroll position towards its target; false when it is there.
bool EaseScroll(float& pos, int target, float dt) {
    const float diff = static_cast<float>(target) - pos;
    if (std::fabs(diff) < 0.02f) {
        if (pos == static_cast<float>(target)) return false;
        pos = static_cast<float>(target);
        return true;
    }
    pos += diff * (1.0f - std::exp(-dt * 20.0f));
    return true;
}

// Everything one picture needs: the drag movement, the physics, the scrolling, then the paint of whatever was invalidated.
void Frame() {
    const ULONGLONG now = GetTickCount64();
    const float dt = g.lastFrameTick ? std::min(1.0f / 30.0f, static_cast<float>(now - g.lastFrameTick) / 1000.0f) : 1.0f / 60.0f;
    g.lastFrameTick = now;
    ApplyPending();
    PhysicsTick();
    const bool a = EaseScroll(g.partPos, g.partScroll, dt);
    const bool b = EaseScroll(g.bonePos, g.boneScroll, dt);
    g.animating = a || b;
    if (g.animating) InvalidateList();
    if (GetUpdateRect(g.hwnd, nullptr, FALSE)) UpdateWindow(g.hwnd);
}

// What a scripted run needs to aim its mouse: the panel's tabs, the rows of the current tab with their centres, the bone list and editor,
// where the overlay drew some bones in the view, and a few state flags. Written as JSON to CARDBRIDGE_TEST_LAYOUT.
void DumpTestLayout() {
    const char* path = std::getenv("CARDBRIDGE_TEST_LAYOUT");
    if (!path) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const PanelLayout l = Lay(rc.bottom);
    nlohmann::json j;
    j["client"] = {rc.right, rc.bottom};
    j["panel"] = kPanel;
    j["track"] = {kTrackL, kTrackR};
    j["rowH"] = kRowH;
    j["tab"] = g.tab;
    j["tabs"] = nlohmann::json::array();
    for (int i = 0; i < TabCount; ++i) j["tabs"].push_back({(l.tabs[i].left + l.tabs[i].right) / 2, (l.tabs[i].top + l.tabs[i].bottom) / 2});
    j["buttons"] = {{"open", {(l.open.left + l.open.right) / 2, (l.open.top + l.open.bottom) / 2}}, {"export", {(l.exportPmx.left + l.exportPmx.right) / 2, (l.exportPmx.top + l.exportPmx.bottom) / 2}}};
    std::shared_ptr<Loaded> L = Current();
    j["rows"] = nlohmann::json::array();
    if (L && g.tab != TabParts && g.tab != TabBones) {
        const std::vector<TweakRow> rows = BuildTweakRows(g.tab);
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.partsTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.partPos * kRowH)) + kRowH / 2;
            j["rows"].push_back({{"id", rows[i].id}, {"kind", rows[i].kind}, {"label", rows[i].label}, {"y", y >= l.partsTop && y < l.partsBottom ? y : -1}});
        }
    }
    j["bones"] = nlohmann::json::object();
    if (L && g.tab == TabBones) {
        const std::vector<BoneRow> rows = BuildBoneRows(L->scene);
        j["bones"]["list"] = nlohmann::json::array();
        for (size_t i = 0; i < rows.size(); ++i) {
            const int y = l.boneListTop + static_cast<int>(i) * kRowH - static_cast<int>(std::lround(g.bonePos * kRowH)) + kRowH / 2;
            if (y >= l.boneListTop && y < l.boneListBottom) j["bones"]["list"].push_back({{"name", L->scene.bones[static_cast<size_t>(rows[i].bone)].name}, {"y", y}, {"arrowX", 16 + std::min(rows[i].depth, 12) * 10 + 4}});
        }
        j["bones"]["editTop"] = l.boneEditTop;
        j["bones"]["fieldY"] = nlohmann::json::array();
        for (int k = 0; k < kBoneEditCount; ++k) j["bones"]["fieldY"].push_back(l.boneEditTop + (1 + k) * kRowH + kRowH / 2);
        j["bones"]["resetY"] = l.boneEditTop + 10 * kRowH + kRowH / 2;
        j["bones"]["actionY"] = l.boneEditTop + 11 * kRowH + kRowH / 2;
        j["bones"]["switchY"] = l.partsTop + kRowH / 2;
    }
    j["view"] = nlohmann::json::object();
    if (L)
        for (const char* name : {"cf_j_hand_L", "cf_j_hand_R", "cf_j_forearm01_L", "cf_j_arm00_L", "cf_j_head", "cf_j_hips", "cf_j_leg01_L"}) {
            const int b = L->scene.boneIndex(name);
            if (b >= 0 && static_cast<size_t>(b) < g.boneScreen.size() && g.boneScreen[static_cast<size_t>(b)].first >= 0)
                j["view"][name] = {static_cast<int>(g.boneScreen[static_cast<size_t>(b)].first) + kPanel, static_cast<int>(g.boneScreen[static_cast<size_t>(b)].second)};
        }
    j["state"] = {{"loaded", L != nullptr}, {"busy", g.busy.load()}, {"gpu", g.gpuOk}, {"physics", g.physics}, {"aPose", g.aPose}, {"showBones", g.showBones},
                  {"showExp", g.showExp}, {"expOk", g.expOk}, {"exporting", g.exporting.load()}, {"undo", g.undoStack.size()}, {"redo", g.redoStack.size()}, {"edits", static_cast<size_t>(std::count_if(g.boneEdits.begin(), g.boneEdits.end(), [](const auto& e) { return !assemble::IsIdentity(e.second); }))}, {"selBone", g.selBoneName}};
    if (L) j["state"]["bones"] = L->scene.bones.size();
    const std::string text = j.dump(2);
    fileio::WriteFile(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
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
        KillTimer(hwnd, 1); g.dirty = true; InvalidateView(); return 0;
    case WM_LOG: InvalidateLog(); return 0;
    case WM_TEST: {
        static const bool hooks = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
        if (!hooks) return 0;
        if (wp == 1) UndoPose(false);
        else if (wp == 2) UndoPose(true);
        else if (wp == 3) SavePoseFile();
        else if (wp == 4) LoadPoseFile();
        else if (wp == 10) DumpTestLayout();
        else if (wp == 11) {   // the nude body on, and the camera close on the chest (lp = pan up, in thousandths of the window height)
            if (std::shared_ptr<Loaded> L = Current())
                for (size_t i = 0; i < L->scene.parts.size() && i < g.hidden.size(); ++i) {
                    const auto& p = L->scene.parts[i];
                    if (p.nudeSwitch) g.hidden[i] = 0;
                    else if (p.group == "Outfit" || p.group == "Accessories") g.hidden[i] = 1;
                }
            g.cam = {};
            g.cam.zoom = 3.0f;
            g.cam.panY = static_cast<float>(lp) / 1000.0f;
            g.dirty = true;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: g.dirty = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {kPanel + 380, 620};
        return 0;
    }
    case WM_EXPORT_DONE: InvalidatePanel(); return 0;
    case WM_EXP_READY: {
        std::shared_ptr<assemble::Scene> sc;
        {
            std::lock_guard<std::recursive_mutex> lock(g.mu);
            sc = std::move(g.expPending);
            g.expFor = g.expPendingFor; g.expGrade = g.expPendingGrade;
            g.expPendingFor.reset();
        }
        g.expBusy = false;
        g.expOk = false;
        std::string why;
        if (sc && (g.gpuExp.ready() || g.gpuExp.Init(&why))) {
            g.gpuExp.SetPlain(true);
            if (g.gpuExp.Load(*sc, &why)) { g.expScene = sc; g.expOk = true; g.showExp = g.expFor == Current(); ApplyPoseLive(); Log("as exported: on (X switches back)", LogKind::Good); }
            else Log("export preview upload failed: " + why, LogKind::Warn);
        } else if (sc) Log("export preview: no Direct3D 11 device (" + why + ")", LogKind::Warn);
        g.dirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
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
        g.partPos = 0;
        g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
        g.loadingOutfit = -1;
        g.soloPart = -1;
        g.state = exporter::NewViewState();
        g.gpuOk = false;
        g.gpuScene.reset();
        g.showExp = g.expOk = false;   // the baked copy belongs to the previous character
        g.expFor.reset();
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
        if (L && !g.applyKeep) { g.boneEdits.clear(); g.undoStack.clear(); g.redoStack.clear(); g.selBoneName.clear(); g.boneScroll = 0; g.bonePos = 0; SeedOpenBones(L->scene); }
        else if (L && g.openBones.empty()) SeedOpenBones(L->scene);
        if (g.physics) { g.physics = false; SetPhysics(true); }
        else ApplyPoseLive();
        if (L && !g.wantPose.empty()) { const std::string pose = g.wantPose; g.wantPose.clear(); LoadPoseFile(pose); }
        g.dirty = true;
        if (L) SetWindowTextW(hwnd, Utf8ToWide("CardBridge " CARDBRIDGE_VERSION " - " + (L->title.empty() ? g.cardPath : L->title)).c_str());
        InvalidateRect(hwnd, nullptr, FALSE);
        static bool benchStarted = false;
        if (L && !L->scene.parts.empty() && !benchStarted && std::getenv("CARDBRIDGE_BENCH")) { benchStarted = true; PostMessageW(hwnd, WM_BENCH, 0, 0); }
        return 0;
    }
    case WM_BENCH: RunBench(); return 0;
    case WM_SETUP_LIBRARY: SetupLibrary(false); return 0;
    case WM_LIBRARY_DONE:
        if (lp) {
            g.settings.library = DefaultLibraryDir();
            SaveSettings(g.settings);
            g.resolver.reset();   // the next load builds its resolver over the new library
            if (exporter::LoadColourGrade(g.settings.library)) Log("colour grading loaded (G toggles it)", LogKind::Good);
            g.status = "drop a Koikatsu card on this window";
            if (!g.pendingCard.empty()) { const std::string card = g.pendingCard; g.pendingCard.clear(); StartLoad(card, g.pendingOutfit); }
            else if (!g.cardPath.empty()) StartLoad(g.cardPath, -1, true);
        } else {
            g.status = "library setup failed: press L to try again";
        }
        InvalidatePanel();
        return 0;
    case WM_DROPFILES: {
        wchar_t buf[1024];
        if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, buf, 1024)) { g.cam = {}; StartLoad(Slashes(WideToUtf8(buf)), -1); }
        DragFinish(reinterpret_cast<HDROP>(wp));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        g.undoBase = CapturePose();
        g.undoBaseValid = true;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
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
                g.partPos = 0;
                g.hoverRow = g.hoverPart = -1;
                InvalidatePanel();
                return 0;
            }
            int chip = OutfitChipAt(l, x, y);
            if (chip >= 0) { ChooseOutfit(chip); return 0; }
            POINT p{x, y};
            if (PtInRect(&l.clearLog, p)) { ClearLog(); return 0; }
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) { BonesTabClick(x, y, l, false); return 0; }
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, false); return 0; }
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
        if (g.showBones && !g.boneScreen.empty()) {   // a bone under the pointer is grabbed: dragging turns it, Ctrl+drag moves it
            const int hit = exporter::PickBone(g.boneScreen, static_cast<float>(x - kPanel), static_cast<float>(y), 12.0f);
            if (std::shared_ptr<Loaded> L = Current(); L && hit >= 0) {
                g.selBoneName = L->scene.bones[static_cast<size_t>(hit)].name;
                g.dragBone = hit;
                g.dragMove = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                g.drag = 3; g.last = {x, y}; g.interacting = true; SetCapture(hwnd);
                InvalidatePanel();
                return 0;
            }
        }
        g.drag = 1; g.last = {x, y}; g.downAt = {x, y}; g.interacting = true; SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        g.undoBase = CapturePose();
        g.undoBaseValid = true;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) { BonesTabClick(x, y, l, true); return 0; }
            if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) { TweakClick(x, y, l, true); return 0; }
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
        if (g.sliderDrag >= 0) { g.dragX = x; g.dragPending = true; return 0; }   // applied by Frame, once per picture
        if (g.drag == 3) {
            g.boneDragDx += static_cast<float>(x - g.last.x);
            g.boneDragDy += static_cast<float>(y - g.last.y);
            g.last = {x, y};
            g.dragPending = true;
            return 0;
        }
        if (g.drag) {
            float dx = static_cast<float>(x - g.last.x), dy = static_cast<float>(y - g.last.y);
            if (g.drag == 1) { g.cam.yawDeg -= dx * 0.5f; g.cam.pitchDeg = std::max(-89.0f, std::min(89.0f, g.cam.pitchDeg - dy * 0.5f)); }
            else { RECT rc; GetClientRect(hwnd, &rc); g.cam.panX += dx / rc.bottom; g.cam.panY += dy / rc.bottom; }
            g.last = {x, y};
            g.lastInput = GetTickCount64();
            g.dirty = true;
            InvalidateView();
            return 0;
        }
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PanelLayout l = Lay(rc.bottom);
        int oldRow = g.hoverRow, oldPart = g.hoverPart, oldButton = g.hoverButton, oldOutfit = g.hoverOutfit;
        int hoverBone = -1;   // the pointer over a bone of the overlay in the view, or over its row in the tree
        if (x >= kPanel && g.showBones) hoverBone = exporter::PickBone(g.boneScreen, static_cast<float>(x - kPanel), static_cast<float>(y), 12.0f);
        else if (x < kPanel && g.tab == TabBones && y >= l.boneListTop && y < l.boneListBottom)
            if (std::shared_ptr<Loaded> LL = Current()) {
                const std::vector<BoneRow> rows = BuildBoneRows(LL->scene);
                const int ri = (y - l.boneListTop + ScrollPx(g.bonePos)) / kRowH;
                if (ri >= 0 && static_cast<size_t>(ri) < rows.size()) hoverBone = rows[static_cast<size_t>(ri)].bone;
            }
        if (hoverBone != g.hoverBone) {
            g.hoverBone = hoverBone;
            if (g.showBones) { g.dirty = true; InvalidateView(); }
        }
        g.hoverButton = x < kPanel ? ButtonAt(l, x, y) : 0;
        g.hoverOutfit = x < kPanel ? OutfitChipAt(l, x, y) : -1;
        g.hoverRow = -1;
        g.hoverPart = -1;
        if (x < kPanel) {
            std::shared_ptr<Loaded> L = Current();
            if (L && !g.showIssues && g.tab == TabBones) {
                if (y >= l.boneListTop && y < l.boneListBottom) g.hoverRow = (y - l.boneListTop + ScrollPx(g.bonePos)) / kRowH;
            } else if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) {
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
        // Repaint only what changed: the old and new row, the buttons band, the footer's hint.
        if (oldRow != g.hoverRow) {
            const bool bones = g.tab == TabBones;
            const int top = bones ? l.boneListTop : l.partsTop, bottom = bones ? l.boneListBottom : l.partsBottom;
            const int px = ScrollPx(bones ? g.bonePos : g.partPos);
            for (int idx : {oldRow, g.hoverRow})
                if (idx >= 0) InvalidatePanelBand(std::max(top, top + idx * kRowH - px), std::min(bottom, top + (idx + 1) * kRowH - px));
        }
        if (oldButton != g.hoverButton || oldOutfit != g.hoverOutfit) InvalidatePanelBand(l.open.top - 2, l.tabs[0].bottom + 2);
        if (oldPart != g.hoverPart) {
            g.lastInput = GetTickCount64();
            g.dirty = true;
            InvalidateView();
            InvalidateFooter();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.hoverBone >= 0) { g.hoverBone = -1; g.dirty = true; InvalidateView(); }
        if (g.hoverRow >= 0 || g.hoverPart >= 0 || g.hoverButton || g.hoverOutfit >= 0) {
            bool part = g.hoverPart >= 0;
            g.hoverRow = g.hoverPart = g.hoverOutfit = -1;
            g.hoverButton = 0;
            if (part) { g.dirty = true; InvalidateView(); }
            InvalidatePanel();
        }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ApplyPending();   // the last movement of the drag counts
        CommitPoseEdit();
        if (g.sliderDrag >= 0) {
            SliderRef r;
            const bool live = IsLiveId(g.sliderDrag);
            if (!live && SliderFor(g.sliderDrag, r)) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "slider %d set to %.3f (card %.3f)", g.sliderDrag, *r.v, r.def);
                Log(msg);
            }
            const int released = g.sliderDrag;
            g.sliderDrag = -1;
            ReleaseCapture();
            TweakEdited(released, true);
            InvalidateList();   // the group's "reset" mark follows the sliders
            return 0;
        }
        if (g.drag == 3) {
            if (std::shared_ptr<Loaded> LL = Current(); LL && g.dragBone >= 0 && static_cast<size_t>(g.dragBone) < LL->scene.bones.size()) Log((g.dragMove ? "moved " : "turned ") + LL->scene.bones[static_cast<size_t>(g.dragBone)].name);
            g.dragBone = -1; g.drag = 0; g.interacting = false; ReleaseCapture();
            g.dirty = true; InvalidateView();
            return 0;
        }
        if (g.drag == 0) return 0;   // a click on the panel: nothing to finish, the picture stays as it is
        if (msg == WM_LBUTTONUP && g.drag == 1 && g.showBones && std::abs(GET_X_LPARAM(lp) - g.downAt.x) < 4 && std::abs(GET_Y_LPARAM(lp) - g.downAt.y) < 4)
            PickBoneAt(GET_X_LPARAM(lp) - kPanel, GET_Y_LPARAM(lp));   // a click, not a drag
        g.drag = 0; g.interacting = false; g.lastInput = GetTickCount64(); ReleaseCapture();
        g.dirty = true; InvalidateView();
        return 0;
    case WM_MOUSEWHEEL: {
        short d = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x < kPanel) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            std::shared_ptr<Loaded> L = Current();
            if (pt.y >= l.logTop) {
                std::lock_guard<std::mutex> lk(g.logMu);   // Log() touches logScroll from worker threads
                int visible = std::max(1, (l.logBottom - (l.logTop + 24)) / 15);
                int maxScroll = std::max(0, static_cast<int>(g.logLines.size()) - visible);
                g.logScroll = std::max(0, std::min(maxScroll, g.logScroll + (d > 0 ? 3 : -3)));
                InvalidateLog();
            } else if (L) {
                if (g.showIssues) {
                    g.partScroll = std::max(0, g.partScroll - (d > 0 ? 3 : -3));
                } else if (g.tab == TabBones) {
                    if (pt.y >= l.boneEditTop && !g.selBoneName.empty()) {   // over a field: a fine step (Shift: ten)
                        const int row = (pt.y - l.boneEditTop) / kRowH;
                        SliderRef r;
                        if (row >= 1 && row <= kBoneEditCount && SliderFor(kBoneEdit + row - 1, r)) {
                            const float step = r.step * ((GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f);
                            const PoseState before = CapturePose();
                            *r.v = std::clamp(*r.v + (d > 0 ? step : -step), r.lo, r.hi);
                            ApplyPoseLive();
                            PoseChanged(before, true);
                            InvalidateList();
                        }
                    } else if (BuildBoneRows(L->scene).size() > 0) {
                        const int visible = std::max(1, (l.boneListBottom - l.boneListTop) / kRowH);
                        const int maxScroll = std::max(0, static_cast<int>(BuildBoneRows(L->scene).size()) - visible);
                        g.boneScroll = std::max(0, std::min(maxScroll, g.boneScroll - (d > 0 ? 3 : -3)));   // Frame eases the list there
                    }
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
        if (pt.x >= kPanel) { g.dirty = true; InvalidateView(); }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT rc;
            GetClientRect(hwnd, &rc);
            PanelLayout l = Lay(rc.bottom);
            bool hand = ButtonAt(l, p.x, p.y) != 0 || PtInRect(&l.clearLog, p) != 0 ||
                        OutfitChipAt(l, p.x, p.y) >= 0;
            if (p.x < kPanel && !hand) {
                std::shared_ptr<Loaded> L = Current();
                if ((L || g.tab == TabSettings) && !g.showIssues && g.tab != TabParts) {
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
        if (GetKeyState(VK_CONTROL) & 0x8000) {   // Ctrl+Z undo (Shift: redo), Ctrl+Y redo, Ctrl+S save the pose, Ctrl+O load a pose
            if (wp == 'Z') UndoPose((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            else if (wp == 'Y') UndoPose(true);
            else if (wp == 'S') SavePoseFile();
            else if (wp == 'O') LoadPoseFile();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == 'T') { g.textured = !g.textured; g.dirty = true; }
        else if (wp == 'H') { g.shaded = !g.shaded; g.dirty = true; }
        else if (wp == 'V') {   // every shader layer on / off
            bool any = false;
            for (size_t i = 0; i < g.gpu.layerCount(); ++i) any = any || g.gpu.layerEnabled(i);
            for (size_t i = 0; i < g.gpu.layerCount(); ++i) g.gpu.SetLayerEnabled(i, !any);
            Log(g.gpu.layerCount() == 0 ? std::string("no shader layers (put .hlsl files in the layers folder)") : std::string(any ? "shader layers off" : "shader layers on"));
            g.dirty = true;
        }
        else if (wp == 'G') { exporter::SetColourGradeEnabled(!exporter::ColourGradeEnabled()); g.dirty = true; }
        else if (wp == 'X') ToggleAsExported();
        else if (wp == 'K') { g.showBones = !g.showBones; Log(g.showBones ? "skeleton: on (click a bone to select it)" : "skeleton: off"); g.dirty = true; }
        else if (wp == 'Y') { SetPhysics(!g.physics); InvalidatePanel(); }
        else if (wp == 'N') { if (g.physics) { g.sim.Nudge({0.004f, 0.0f, 0.002f}); g.physicsKick = g.physicsMoving = true; Log("physics: shake"); } else Log("physics is off (Y)", LogKind::Warn); }
        else if (wp == 'P') { const PoseState before = CapturePose(); g.aPose = !g.aPose; Log(std::string("A-pose: ") + (g.aPose ? "on" : "off")); ApplyPoseLive(); PoseChanged(before); }
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

// ---- benchmark -------------------------------------------------------------------------------------------------------
// CARDBRIDGE_BENCH=<file.json>: once the first card is up, plays a scripted session through the window procedure (orbit, hover over the
// parts, tab switches, a slider drag, a flood of mouse moves, log lines, wheel scrolling), times every step from input to the finished
// paint and writes the numbers, then closes the window.
void FlushFrame() { Frame(); }

// One round of the main loop: the queued messages, then a picture.
void PumpOnce() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Frame();
}

// Runs the main loop until a paint has finished (or a few seconds passed).
void PumpUntilPaint() {
    const long p0 = g.prof.paints;
    const ULONGLONG t0 = GetTickCount64();
    while (g.prof.paints == p0 && GetTickCount64() - t0 < 5000) {
        PumpOnce();
        if (g.prof.paints == p0) MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
}

void RunBench() {
    const char* outPath = std::getenv("CARDBRIDGE_BENCH");
    std::shared_ptr<Loaded> L = Current();
    if (!outPath || !L || L->scene.parts.empty()) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    struct Phase { std::vector<double> ms; double render = 0, blit = 0, panel = 0, panelDraw = 0, panelCopy = 0, reshape = 0, refresh = 0; std::vector<double> sec = std::vector<double>(8, 0.0); long paints = 0, renders = 0; };
    std::map<std::string, Phase> phases;
    auto run = [&](const char* name, int n, const std::function<void(int)>& step) {
        Phase& ph = phases[name];
        const App::Prof p0 = g.prof;
        for (int i = 0; i < n; ++i) {
            const double t = NowMs();
            step(i);
            ph.ms.push_back(NowMs() - t);
        }
        ph.render = g.prof.render - p0.render; ph.blit = g.prof.blit - p0.blit; ph.panel = g.prof.panel - p0.panel;
        ph.reshape = g.prof.reshape - p0.reshape; ph.refresh = g.prof.refresh - p0.refresh;
        for (int k = 0; k < 8; ++k) ph.sec[static_cast<size_t>(k)] = g.prof.sec[k] - p0.sec[k];
        ph.panelDraw = g.prof.panelDraw - p0.panelDraw; ph.panelCopy = g.prof.panelCopy - p0.panelCopy;
        ph.paints = g.prof.paints - p0.paints; ph.renders = g.prof.renders - p0.renders;
    };
    auto send = [&](UINT m, WPARAM w, int x, int y) { SendMessageW(g.hwnd, m, w, MAKELPARAM(x, y)); };
    FlushFrame();
    PanelLayout l = Lay(rc.bottom);
    const int vx = kPanel + (rc.right - kPanel) / 2, vy = rc.bottom / 2;

    run("orbit", 80, [&](int i) {
        if (i == 0) send(WM_LBUTTONDOWN, MK_LBUTTON, vx, vy);
        send(WM_MOUSEMOVE, MK_LBUTTON, vx + i * 4, vy + (i % 7));
        FlushFrame();
        if (i == 79) send(WM_LBUTTONUP, 0, vx + i * 4, vy);
    });
    FlushFrame();
    g.tab = TabParts; g.showIssues = false;
    run("hover_parts", 120, [&](int i) {
        send(WM_MOUSEMOVE, 0, 100, l.partsTop + 4 + (i * 7) % std::max(40, l.partsBottom - l.partsTop - 8));
        FlushFrame();
    });
    send(WM_MOUSEMOVE, 0, vx, vy);
    run("tab_switch", 50, [&](int i) {
        const RECT t = l.tabs[i % TabCount];
        send(WM_LBUTTONDOWN, MK_LBUTTON, (t.left + t.right) / 2, (t.top + t.bottom) / 2);
        send(WM_LBUTTONUP, 0, (t.left + t.right) / 2, (t.top + t.bottom) / 2);
        FlushFrame();
    });
    // a body slider: press on its track, then drag back and forth
    g.tab = TabBody; g.partScroll = 0; g.partPos = 0; g.showIssues = false;
    FlushFrame();
    {
        const std::vector<TweakRow> rows = BuildTweakRows(TabBody);
        int first = -1;
        for (size_t i = 0; i < rows.size(); ++i) if (rows[i].kind == 1) { first = static_cast<int>(i); break; }
        if (first >= 0) {
            l = Lay(rc.bottom);
            const int y = l.partsTop + first * kRowH + kRowH / 2;
            const int a = kTrackL + (kTrackR - kTrackL) * 4 / 10, b = kTrackL + (kTrackR - kTrackL) * 9 / 10;
            send(WM_LBUTTONDOWN, MK_LBUTTON, a, y);
            run("slider_drag", 60, [&](int i) {
                send(WM_MOUSEMOVE, MK_LBUTTON, a + (b - a) * (i % 20) / 20, y);
                FlushFrame();
            });
            run("slider_flood", 8, [&](int) {   // 25 moves arrive at once, as from a fast mouse: time to the next finished paint
                for (int k = 0; k < 25; ++k) PostMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(a + (b - a) * k / 25, y));
                PumpUntilPaint();
            });
            send(WM_LBUTTONUP, 0, b, y);
            FlushFrame();
            run("slider_reset", 1, [&](int) { send(WM_RBUTTONDOWN, MK_RBUTTON, 60, y); send(WM_RBUTTONUP, 0, 60, y); FlushFrame(); });
        }
    }
    g.tab = TabParts; g.partScroll = 0; g.partPos = 0;
    FlushFrame();
    run("log_line", 60, [&](int i) {
        Log("benchmark line " + std::to_string(i) + " - a message about as long as the ones the loader writes");
        PumpUntilPaint();
    });
    {
        POINT pt{100, l.partsTop + 40};
        ClientToScreen(g.hwnd, &pt);
        run("wheel", 60, [&](int i) {
            SendMessageW(g.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (i / 15) % 2 ? WHEEL_DELTA : -WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
            FlushFrame();
        });
    }
    run("view_idle_repaint", 40, [&](int) { g.dirty = true; InvalidateRect(g.hwnd, nullptr, FALSE); FlushFrame(); });

    nlohmann::json j;
    j["client"] = {rc.right, rc.bottom};
    j["gpu"] = g.gpuOk;
    j["triangles"] = Triangles(L->scene);
    for (auto& [name, ph] : phases) {
        std::vector<double> v = ph.ms;
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        auto pct = [&](double q) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
        j["phases"][name] = {{"n", v.size()}, {"mean_ms", v.empty() ? 0.0 : sum / static_cast<double>(v.size())}, {"p50_ms", pct(0.5)}, {"p95_ms", pct(0.95)}, {"max_ms", v.empty() ? 0.0 : v.back()},
                             {"paints", ph.paints}, {"renders", ph.renders}, {"render_ms_total", ph.render}, {"blit_ms_total", ph.blit}, {"panel_ms_total", ph.panel}, {"panel_draw_ms_total", ph.panelDraw}, {"secs", ph.sec}, {"panel_copy_ms_total", ph.panelCopy}, {"reshape_ms_total", ph.reshape}, {"refresh_ms_total", ph.refresh}};
    }
    const std::string text = j.dump(2);
    fileio::WriteFile(outPath, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
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
        else if (a == "--pose" && i + 1 < argc) g.wantPose = Slashes(WideToUtf8(argvw[++i]));
        else if (a == "--outfit" && i + 1 < argc) g.wantOutfit = std::atoi(WideToUtf8(argvw[++i]).c_str());
        else card = Slashes(a);
    }
    LocalFree(argvw);
    SaveSettings(g.settings);

    ui::Startup();
    ui::LoadLogo(inst, 2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CardBridgeViewer";
    RegisterClassW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"CardBridge " L"viewer", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1280, 820, nullptr, nullptr, inst, nullptr);
    if (wc.hIcon) SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    ShowWindow(g.hwnd, show);
    g.fontTitle = CreateFontW(-21, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
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
    if (!g.settings.mods.empty()) g.settings.noMods = false;
    if (!g.settings.noMods && (g.settings.mods.empty() || !Exists(g.settings.mods))) g.settings.mods = Exists(CachedModsRoot()) ? CachedModsRoot() : std::string();
    if (g.settings.mods.empty() && !g.settings.noMods) {
        // Optional: without mods only base-game items resolve.
        std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu mods folder (Cancel to use base-game items only)");
        if (!d.empty()) g.settings.mods = d;
        else g.settings.noMods = true;   // asked once; the Setup tab changes it
    }
    SaveSettings(g.settings);
    Log("library: " + (g.settings.library.empty() ? std::string("(not set)") : g.settings.library));
    Log("mods: " + (g.settings.mods.empty() ? std::string("(not set)") : g.settings.mods));
    ReloadLayers(false);
    if (g.settings.library.empty()) {   // first run: build the library from the user's game, then open the card
        g.pendingCard = card;
        g.pendingOutfit = g.wantOutfit;
        wchar_t selfExe[MAX_PATH];
        GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
        if (library::HasEmbeddedLibrary(Slashes(WideToUtf8(selfExe)))) StartEmbeddedUnpack();
        else PostMessageW(g.hwnd, WM_SETUP_LIBRARY, 0, 0);
    } else if (!card.empty()) {
        StartLoad(card, g.wantOutfit);
    }
    // The main loop: take every queued message (they only record what the user did), then draw one picture. Input can never starve the
    // drawing, because a picture is made after each batch whatever else is still arriving.
    MSG m;
    bool quit = false;
    while (!quit) {
        const double t0 = NowMs();
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (NowMs() - t0 > 6.0) break;
        }
        if (quit) break;
        Frame();
        const bool again = g.physics || g.animating || g.dragPending;
        if (!again) MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        else MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, 0);
    }
    g.panel.Reset();   // GDI+ objects must go before GDI+ does
    for (HFONT* f : {&g.fontTitle, &g.fontUi, &g.fontBold, &g.fontMono, &g.fontLog, &g.fontSmall})
        if (*f) DeleteObject(*f);
    ui::Shutdown();
    return 0;
}
