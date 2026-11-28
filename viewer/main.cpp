// CardBridge viewer: opens a Koikatsu card, assembles it exactly as the exporter does, and shows it in a window so every
// card can be checked against its picture without exporting. Windows only (plain Win32 + GDI, no extra dependencies).
//
//   cardbridge-viewer [card.png] [--library <dir>] [--mods <dir>] [--outfit N] [--clothes <outfit.png>]
//
// Library and mods folders are remembered in %LOCALAPPDATA%\CardBridge\viewer.json. Drop a card on the window to open it.
// Dropping an outfit file (KKCLOTHING .png) onto a loaded character asks how to import it (see below);
// import is always in memory only (the card on disk is never saved), and it stays pinned to that one outfit
// slot: switching to another slot shows that slot's own clothing again, and switching back to the slot shows the imported file again.
// A Koikatsu Coordinate (outfit) file (【KoiKatuClothes】, what Koikatsu itself saves as a coordinate file) dropped
// onto the viewer always asks how to import it: Import as Clothing (outfit contents only, merged as the clothing
// import does - hair is kept) or Import as Coordinate (the whole coordinate replaces the slot, accessories included,
// including hair-type). With no character on screen (or while the current load is still running) the dialog explains
// there is nothing to import onto yet and offers only Cancel. A dropped character card opens as a character, as usual.
// In the Parts list the import is its own "Added outfit" group, apart from the base model's Body/Head/Hair/Outfit/
// Accessories: its header checkbox toggles every added part at once, each item keeps its own checkbox, the slot
// marked * is the base outfit it replaced (shown as OUTFIT (REPLACED)), and < > move it to another slot.
// D (or the Reset clothing button) drops the imported file and returns the current outfit to the card's own clothing.
//
//   mouse: left drag orbit, right/middle drag pan, wheel zoom, hover a part to light it up in the model,
//          click a part to hide/show it, right-click a part to solo it
//          part rows carry move, rotate and resize chips on their right end plus a delete chip: click a gizmo chip to hold
//          that part with the X/Y/Z gizmo in the view — drag an arrow to move it, a ring to rotate it, a square handle to
//          resize it along one axis; right-click the chip to reset that one edit, Esc (or the chip again) deselects.
//          Clicking the x chip deletes the part from the preview and the export (Ctrl+Z brings it back).
//          the view's top-right corner shows an always-on navigation widget (Blender-style, like the Project K editor):
//          click an axis ball to look from that side (drag anywhere in it to orbit)
//   keys:  Left/Right or 1-7 outfit, T textures, H the game shaders on/off, V extra shader layers on/off (layers/*.hlsl),
//          G colour grading on/off, X (or the MMD button) "as exported": the PMX as MikuMikuDance shows it (toon, edge, self-shadow, ground shadow),
//          K the skeleton over the model (click a bone to select it, Bones tab edits it), P automatic A-pose,
//          R reset view, D reset clothing to the card default, F/S/B front/side/back,
//          I issues, A show every part, E export the current scene to PMX, C clear the log, O open a card,
//          M mods folder, L asset library folder
//          Shift+Left-click any slider to type its value with the keyboard (Enter commits, Esc restores)

#include "assemble/Assembler.h"
#include "assemble/ProportionTransfer.h"
#include "measure/TransferMeasure.h"
#include "assemble/JiggleSettings.h"
#include "material/MaterialWorkbench.h"
#include "material/UvDiagnostics.h"
#include "assemble/Pose.h"
#include "assemble/PhysicsSim.h"
#include "export/BoneLabels.h"
#include "export/MorphCatalog.h"
#include "export/SkeletonOverlay.h"
#include "export/ColourGrade.h"
#include "export/BakeQuality.h"
#include "export/Exporter.h"
#include "export/ModelExport.h"
#include "export/GpuView.h"
#include "export/MoveGizmo.h"
#include "export/RotateGizmo.h"
#include "export/ScaleGizmo.h"
#include "card/CardFile.h"
#include "card/SelectiveImport.h"
#include "card/ClothingExport.h"
#include "card/MaterialEdits.h"
#include "export/OutfitIcon.h"
#include "core/FileIo.h"
#include "pmx/PmxReader.h"
#include "pmx/PmxToScene.h"
#include "pmx/PmxAssetApply.h"
#include "pmx/PmxTransform.h"
#include "library/AssetCatalog.h"
#include "core/Timing.h"
#include "export/Preview.h"
#include "export/Translucency.h"
#include "export/SkinShade.h"
#include "export/SkinAoBake.h"
#include "mesh/Simplify.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "shape/Shape.h"
#include "shape/BodyMeasurements.h"
#include "measure/BodyMeasure.h"
#include "Ui.h"
#include "core/Version.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <functional>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <condition_variable>
#include <thread>
#include <vector>

using namespace cardbridge;

namespace {

#include "Util.inc"
#include "State.inc"
#include "Progress.inc"
#include "Proportions.inc"
#include "Tweaks.inc"
#include "BodyTransfer.inc"
#include "Morphs.inc"
#include "MaterialsHelpers.inc"
#include "Pose.inc"
#include "SliderEdit.inc"
#include "Export.inc"
#include "Parts.inc"
#include "Gizmo.inc"
#include "Paint.inc"
#include "ModeStrip.inc"
#include "AxisWidget.inc"
#include "View.inc"
#include "Widgets.inc"
#include "Explorer.inc"
#include "Assets.inc"
#include "Optimize.inc"
#include "MaterialsDraw.inc"
#include "Workbench.inc"
#include "BonesDraw.inc"
#include "Panel.inc"
#include "Clicks.inc"
#include "Window.inc"
#include "Selective.inc"
#include "Bench.inc"
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    LPWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string card;
    g.settings = LoadSettings();
    g.viewMode = static_cast<int>(exporter::ViewModeFromName(g.settings.viewMode.c_str()));   // the persisted viewport diagnostic mode (spec S13)
    for (int i = 1; i < argc; ++i) {
        std::string a = WideToUtf8(argvw[i]);
        if (a == "--library" && i + 1 < argc) {
            g.settings.library = Slashes(WideToUtf8(argvw[++i]));
            g.cliLibraryOverride = true;   // never persist this or auto-switch profiles
            const auto info = library::ReadGameInfo(g.settings.library);
            if (info.game != library::SourceGame::Unknown) g.settings.activeProfile = ProfileKey(info.game);
        } else if (a == "--mods" && i + 1 < argc) {
            g.settings.mods = Slashes(WideToUtf8(argvw[++i]));
            g.cliModsOverride = true;
        }
        else if (a == "--pose" && i + 1 < argc) g.wantPose = Slashes(WideToUtf8(argvw[++i]));
        else if (a == "--outfit" && i + 1 < argc) g.wantOutfit = std::atoi(WideToUtf8(argvw[++i]).c_str());
        else if (a == "--clothes" && i + 1 < argc) {
            ClothingPin pin;
            pin.path = Slashes(WideToUtf8(argvw[++i]));
            pin.name = fileio::FileName(pin.path);
            pin.mode = 0;
            g.clothingPins.push_back(std::move(pin));   // --clothes replaces everything, like the drop dialog's Replace All
        }
        else if (a.rfind("--", 0) == 0) continue;
        else card = Slashes(a);
    }
    LocalFree(argvw);
    // --clothes pins to --outfit when given, otherwise to the card's current slot on load (StartLoad pins it there).
    if (!g.clothingPins.empty() && g.wantOutfit >= 0) g.clothingPins.back().dstOutfit = g.wantOutfit;
    g.jiggle = g.settings.jiggle;   // the live settings start from the persisted ones (the Look tab edits them)
    g.braAwarePhysics = g.settings.braAwarePhysics;
    SaveSettings(g.settings);

    ui::Startup();
    ui::LoadLogo(inst, 2);
    g.hinst = inst;
    // A private test instance (CARDBRIDGE_TEST_HOOKS) never takes the screen: the window is created off-screen, with no activation,
    // so a script driving it never interrupts whatever the user is doing.
    const bool testQuiet = std::getenv("CARDBRIDGE_TEST_HOOKS") != nullptr;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CardBridgeViewer";
    RegisterClassW(&wc);
    {   // the Material Workbench window's class; the window itself opens on first click
        WNDCLASSW wcm{};
        wcm.lpfnWndProc = MatWndProc;
        wcm.hInstance = inst;
        wcm.hCursor = wc.hCursor;
        wcm.hIcon = wc.hIcon;
        wcm.lpszClassName = kWbClassName;
        RegisterClassW(&wcm);
    }
    {   // the Selective Import window's class; the window itself opens on first drop of an outfit file
        WNDCLASSW wcs{};
        wcs.lpfnWndProc = SelWndProc;
        wcs.hInstance = inst;
        wcs.hCursor = wc.hCursor;
        wcs.hIcon = wc.hIcon;
        wcs.lpszClassName = kSelClassName;
        RegisterClassW(&wcs);
    }
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES | (testQuiet ? WS_EX_NOACTIVATE : 0), wc.lpszClassName, L"CardBridge",
                             WS_OVERLAPPEDWINDOW | (testQuiet ? 0 : WS_VISIBLE), testQuiet ? -32000 : CW_USEDEFAULT, testQuiet ? -32000 : CW_USEDEFAULT,
                             1280, 820, nullptr, nullptr, inst, nullptr);
    if (wc.hIcon) SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    ShowWindow(g.hwnd, testQuiet ? SW_SHOWNOACTIVATE : show);
    g.fontTitle = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Semibold");
    g.fontUi = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g.fontLog = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g.fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontSemi = CreateFontW(-11, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Semibold");
    std::string gpuWhy;
    exporter::GpuOptions go;
    go.preference = g.settings.gpu.empty() ? "auto" : g.settings.gpu;
    go.skin = g.settings.skin.empty() ? "gpu" : g.settings.skin;
    if (g.gpu.Init(go, &gpuWhy)) {
        const char* what = g.gpu.kind() == exporter::GpuKind::Warp ? "WARP on the CPU (no real GPU found; the supported CPU fallback)" :
                           g.gpu.kind() == exporter::GpuKind::Hardware ? "the real GPU" : "Direct3D 11";
        Log("Direct3D 11 ready: " + g.gpu.adapter() + " (" + what + ")", LogKind::Good);
    } else Log("Direct3D 11 unavailable (" + gpuWhy + "); software renderer (the CPU fallback)", LogKind::Warn);
    CoInitialize(nullptr);
    if (g.settings.library.empty() || !Exists(g.settings.library)) g.settings.library = FindLibrary();
    if (!g.settings.mods.empty()) g.settings.noMods = false;
    if (!g.settings.noMods && (g.settings.mods.empty() || !Exists(g.settings.mods))) g.settings.mods = Exists(CachedModsRoot()) ? CachedModsRoot() : std::string();
    if (g.settings.mods.empty() && !g.settings.noMods) {
        // Optional: without mods only base-game items resolve.
        std::string d = PickFolder(g.hwnd, L"Choose your Koikatsu / Koikatsu Sunshine mods folder (Cancel to use base-game items only)");
        if (!d.empty()) g.settings.mods = d;
        else g.settings.noMods = true;   // asked once; the Setup tab changes it
    }
    if (!g.cliLibraryOverride && !g.cliModsOverride) {
        StoreActiveProfile();   // persist first-run defaults into the active game profile
        SaveSettings(g.settings);
    }
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
            // Measure each synchronous handler: anything long here is a window that stops answering
            // WM_NULL (Windows "Not Responding"), even when the loading itself runs on a worker.
            const bool timing = cbtime::Recorder::Instance().Enabled();
            cbtime::Scope msgTime(timing ? std::string("UI: message ") + std::to_string(m.message) : std::string());
            DispatchMessageW(&m);
            msgTime.Stop();
            if (NowMs() - t0 > 6.0) break;
        }
        if (quit) break;
        {
            cbtime::Scope frameTime("UI: Frame");
            Frame();
        }
        const bool physicsLive = g.physics && (g.physicsMoving || g.physicsKick);   // the jiggle is still moving: repaint like a game loop
        const bool again = g.animating || g.dragPending || g.progActive.load() || physicsLive;
        if (!again) MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        else MsgWaitForMultipleObjectsEx(0, nullptr, physicsLive ? 8 : 33, QS_ALLINPUT, 0);
    }
    // Tell any detached loader/bake workers to stop at their next cancellation
    // point. They are never joined: shutting down must not wait for a heavy bake,
    // and the App singleton is deliberately immortal so they cannot touch a destroyed g.
    g.progCancel.store(true);
    g.progActive.store(false);
    g.panel.Reset();   // GDI+ objects must go before GDI+ does
    g.wb.Reset();
    cbtime::Report();
    for (HFONT* f : {&g.fontTitle, &g.fontUi, &g.fontBold, &g.fontMono, &g.fontLog, &g.fontSmall, &g.fontSemi})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    ui::Shutdown();
    return 0;
}
