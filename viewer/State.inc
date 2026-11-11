// viewer fragment: log types, pose, loaded, layout, App (included in order by main.cpp)

// ---- live log ---------------------------------------------------------------------------------------------------------
enum class LogKind { Info, Good, Warn, Error };
struct LogLine {
    std::string time, text;
    LogKind kind = LogKind::Info;
};

// ---- the pose the user has made: bone edits and the A-pose, what undo and redo move between ----
struct PoseState {
    std::map<std::string, assemble::BonePose> edits;
    bool aPose = true;   // MikuMikuDance's rest pose: a motion (VMD) is keyed for arms about 40 degrees down
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
    std::string clothing;             // the outfit file applied in memory, if any (never saved)
};

constexpr int kPanel = 380;
constexpr int kRowH = 20;

struct PartRow {
    int part = -1;        // -1 = a group header
    std::string group;    // display group (base groups plus "Added outfit")
    bool replaced = false;   // header-only: the base outfit this slot replaced (no parts follow)
};

// The display group of a part: added-clothing parts list under "Added outfit"
// apart from the base model's groups (Body/Head/Hair/Outfit/Accessories).
std::string DisplayGroup(const assemble::ScenePart& p) { return p.fromClothing ? "Added outfit" : p.group; }

// ---- tweaks: the sliders and switches on top of what the card says --------------------------------------------------------
// Changing one re-assembles the character (a few seconds, after the pointer has been still for a moment); the exported PMX is
// the re-assembled scene, so it carries the tweaked shape.
struct Tweaks {
    std::vector<float> body, face, bodyCard, faceCard;   // current slider values and the card's own (the defaults)
    bool nippleInside = false;
    float nippleMm = 0.0f;                                // nipple bones along the chest normal, millimetres
    float areola = 0.0f, areolaCard = 0.0f;              // areola size (a material property, re-shaped live)
    float skinShade = 1.5f;                                // automatic cleavage shading baked into the skin texture (0..2); saved in the viewer's settings, see Settings
    float skinShadeUnder = 1.0f;                           // the fold under the breasts (0..2); saved in the viewer's settings
    float skinShadeTop = 1.2f;                             // the top extent of the cleavage: 0 = none, 1 = the full breast line, 2 = toward the base of the neck; saved in the viewer's settings
    float skinShadeWidth = 0.6f;                          // how wide the cleavage line runs (0.1 = hairline, 1 = default, 2 = broad); saved in the viewer's settings
    bool pinkNipples = true;                              // automatic pink nipple / areola tint (off: the card's own colour)
    Vec3 nippleColour{assemble::kDefaultNippleColour.x, assemble::kDefaultNippleColour.y,
                      assemble::kDefaultNippleColour.z};   // the tint when pinkNipples is on
};
enum Tab { TabParts = 0, TabBody, TabFace, TabLook, TabBones, TabMaterials, TabSettings, TabCount };
const char* const kTabNames[TabCount] = {"Parts", "Body", "Face", "Look", "Bones", "Mats", "Setup"};
// Slider ids: body 0..43, face 100..151, then the look sliders; switches from 300.
// Pose controls change the picture at once (no re-assembly): the A-pose switch and angle, and the nine fields of the selected bone (10000..10008).
// Settings tab actions (above every other id, so the slider and switch code never mistakes one for its own).
constexpr int kSetLibChoose = 20000, kSetLibBuild = 20001, kSetModsChoose = 20002, kSetModsReindex = 20003, kSetModsClear = 20004, kSetExportDir = 20006,
              kSetExportAsk = 20007, kSetLayers = 20008, kSetLayersDefault = 20009, kSetOpenFolder = 20010, kSetForget = 20011;
constexpr int kSetGpuAuto = 20012, kSetGpuHardware = 20013, kSetGpuWarp = 20014, kSetGpuSoftware = 20015, kSetSkinGpu = 20016, kSetSkinCpu = 20017, kSetSkinAuto = 20018;
void SettingsAction(int id);
constexpr int kFaceBase = 100, kNippleMm = 200, kAreola = 201, kAPoseDeg = 202, kHeightCm = 203, kSag = 204,
              kSkinShade = 205, kNipplePink = 206, kNippleR = 207, kNippleG = 208, kNippleB = 209,
              kSkinShadeUnder = 210, kSkinShadeTop = 211, kSkinShadeWidth = 212,
              kNippleIn = 300, kAPose = 301, kPhysics = 302, kJiggleBust = 303, kJiggleButt = 304, kJiggleIntensity = 305, kAutoCloth = 306,
              kLayerBase = 400, kBoneEdit = 10000, kBoneEditCount = 9, kMatBase = 5000, kMatCount = 9;   // kAutoCloth: synthesized cloth skeleton (Look tab); switches from 400: the shader layers
struct TweakRow {
    int kind = 0;        // 0 group header (id -1: nothing to reset), 1 slider, 2 switch, 4 plain text, 5 a settings action (a button row)
    int id = 0;
    std::string label, group;
};

struct PanelLayout {
    RECT logo{}, tabs[TabCount]{};
    RECT open{}, exportPmx{}, issues{}, reset{}, all{}, asExported{};
    RECT clothingReset{}, clothingPrev{}, clothingNext{};
    int outfitNameTop = 0, outfitChipsTop = 0, outfitClothTop = 0;
    int partsHeadingTop = 0, partsTop = 0, partsBottom = 0, logTop = 0, logBottom = 0, footerTop = 0;
    int boneListTop = 0, boneListBottom = 0, boneEditTop = 0;   // Bones tab: a switch row, the tree, then the editor of the selected bone
    int matListTop = 0, matListBottom = 0;                      // Materials tab (the launcher): the list under the filter + open rows
    RECT clearLog{};
    // Bones tab hit areas (the click code and the drawing use the same ones): the two switches, the reset row, the undo / redo / save / load row
    RECT boneSkeleton{}, boneHelpers{}, boneResetOne{}, boneResetAll{}, boneUndo{}, boneRedo{}, boneSave{}, boneLoad{};
    // Materials tab hit areas: the filter dropdown and the button that opens the Material Workbench window
    // (the editor itself lives in that window, so it has no panel hit areas).
    RECT matFilter{}, matOpen{};
};

constexpr int kBoneEditRows = 12;   // heading, nine fields, the reset row, the undo / redo / save / load row
constexpr int kHeaderBottom = 90;   // the header band (logo, name, stats) ends here

PanelLayout LayoutFor(int H, bool boneTab = false, int editRows = kBoneEditRows) {
    PanelLayout l;
    l.logo = {12, 12, 68, 68};
    const int by = 98, bh = 26;
    l.open = {10, by, 58, by + bh};
    l.exportPmx = {64, by, 156, by + bh};
    l.issues = {162, by, 214, by + bh};
    l.reset = {220, by, 288, by + bh};
    l.all = {294, by, 324, by + bh};
    l.asExported = {330, by, 370, by + bh};
    // The outfit picker sits between the header buttons and the part list: a name line, then a row of numbered slots,
    // then the imported-outfit status line with its reset button.
    l.outfitNameTop = 138;
    l.outfitChipsTop = 160;
    l.outfitClothTop = 184;
    l.clothingPrev = {148, l.outfitClothTop, 172, l.outfitClothTop + 20};
    l.clothingNext = {176, l.outfitClothTop, 200, l.outfitClothTop + 20};
    l.clothingReset = {206, l.outfitClothTop, kPanel - 10, l.outfitClothTop + 20};
    // Seven tabs, 42 px each with a 2 px gap, so the right-hand status line
    // ("34 parts", "0 edited", ...) keeps its own room instead of overlapping
    // the last tab.
    for (int i = 0; i < TabCount; ++i) l.tabs[i] = {8 + i * 44, 216, 50 + i * 44, 240};
    l.partsHeadingTop = 222;
    l.partsTop = 248;
    const int logH = boneTab ? 100 : std::clamp(H / 3, 130, 330);   // the bone/material editor needs the room
    l.footerTop = H - 36;
    l.logBottom = l.footerTop - 4;
    l.logTop = std::max(l.partsTop + 90, l.logBottom - logH);
    l.partsBottom = std::max(l.partsTop + kRowH, l.logTop - 10);
    l.boneEditTop = std::max(l.partsTop + 3 * kRowH, l.partsBottom - editRows * kRowH - 4);
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
    l.matFilter = {10, l.partsTop, 200, l.partsTop + kRowH};
    l.matOpen = {10, l.partsTop + kRowH + 4, kPanel - 10, l.partsTop + kRowH + 30};
    l.matListTop = l.partsTop + kRowH + 34;
    l.matListBottom = l.partsBottom;
    return l;
}

struct App {
    HWND hwnd = nullptr;
    Settings settings;
    std::string cardPath;
    // An outfit file applied in memory only (never saved as a card): the path
    // of the clothing file and the outfit slot it overwrote. Empty = none.
    std::string clothingPath, clothingName;
    int clothingOutfit = -1;
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
    int viewMode = 0;                // exporter::ViewMode (0 = Lit, the default); persisted in viewer.json ("viewMode")
    bool viewIsolate = false;        // MaterialID mode: hide every part not using the selected material (session only)
    int viewHoverCell = -1;          // the mode strip cell the cursor is over (-1 = none); repaint only that band
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
    int hoverRow = -1, hoverPart = -1, hoverButton = 0;   // button: 0 none, 1 open, 2 export, 3 issues, 4 reset, 5 all, 6 MMD, 7 reset clothing, 10+ tabs
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
    bool aPose = true;                            // on by default: the PMX rest pose a motion expects (see PoseState)
    float aPoseDeg = 40.0f;
    bool showBones = false, showHelpers = false;
    std::string selBoneName;                  // the selected bone, by name
    std::set<std::string> openBones;          // tree nodes that are open
    bool bonesSeeded = false;                 // openBones was given its first value for this character
    // Material workbench (Stage 2): the override state for the loaded character.
    material::WorkbenchState mat;             // originals snapshot + sparse overrides + undo/redo
    int selMaterial = -1;                     // selected material index (-1 = none)
    int matFilter = -1;                       // -1 = all classes, else static_cast<material::MaterialClass>
    int matScroll = 0;                        // material list scroll target, in rows
    float matPos = 0;                         // eased position
    float matBuf[kMatCount] = {};             // slider buffers: R,G,B,opacity,rough,metallic,specular,emission,normal
    bool matHexEditing = false;               // HEX text entry in the materials editor
    std::string matHexBuf;
    bool matHexUndoPushed = false;
    // Materials tab dropdowns + colour picker (popups over the editor).
    bool matClassDropOpen = false;
    bool matFilterDropOpen = false;
    int matDropHover = -1;                    // row under the cursor in an open dropdown
    bool matPickerOpen = false;
    int matPickerDrag = 0;                    // 0 none, 1 = SV square, 2 = hue bar
    bool matPickerUndoPushed = false;         // one undo step per picker session
    // Texture inspector + UV canvas (Stage 3): false = shading sliders, true = inspector.
    bool matInspectOpen = false;
    int matTexChannel = 0;                    // 0 all, 1 R, 2 G, 3 B, 4 A
    int matTexSlot = -1;                      // selected texProps slot (-1 = auto _MainTex)
    bool matTexSlotDropOpen = false;
    float uvScale = 0.0f, uvPanX = 0.0f, uvPanY = 0.0f;   // UV canvas affine (px per UV unit)
    int matUvDrag = 0;
    int matUvSelIsland = -1;
    material::UvReport matUvReport;
    std::vector<std::pair<uint32_t, uint32_t>> matUvSeams;
    int matUvCacheMat = -2, matUvCacheMesh = -2;
    std::vector<char> matUvSelMask;          // per draw-triangle of the cached selection: 1 = the selected island
    POINT uvDown{};
    float uvPan0X = 0, uvPan0Y = 0;
    int matUvTrisCache = -2;                     // selMaterial the cached list belongs to (-2 = nothing cached)
    std::vector<material::UvDrawTri> matUvTris;   // that material's draw triangles, in draw order (same order as the mask)
    // The Material Workbench window: the full editor lives there, the side panel
    // keeps a launcher (filter + list + open button). Created on first open.
    HWND matHwnd = nullptr;
    HINSTANCE hinst = nullptr;
    ui::Surface wb;                          // the workbench's own off-screen surface
    int wbHoverRow = -1;                     // material list row under the cursor in the workbench
    int wbHoverBtn = 0;                      // workbench button under the cursor (0 = none)
    int wbMatScroll = 0;                     // the workbench list's scroll target, in rows
    float wbMatPos = 0;                      // eased position (the panel's list has its own)
    int dragTrackL = 0, dragTrackR = 0;      // the track a slider drag is using (0 = the panel's kTrackL / kTrackR)
    std::vector<Mat4> skin;                   // current skin matrices (empty = rest pose)
    std::vector<Vec3> boneWorld;              // posed bone positions, for the overlay
    std::vector<std::pair<float, float>> boneScreen;   // where the overlay put each bone in the last picture (x < 0: not drawn)
    exporter::GpuView* lastGpu = nullptr;     // the view that drew the last picture
    POINT downAt{};                           // where the left button went down in the view (a click without a drag picks a bone)
    int boneScroll = 0;
    int hoverBone = -1;                       // the bone under the pointer (in the view or the tree), lit up in the overlay
    int dragBone = -1;                        // the bone being turned (or moved) with the left button in the view
    bool dragMove = false;
    // The accessory move gizmo: shown in the view when an accessory part is selected in the list (its chip);
    // dragging an axis moves the accessory (as bone edits on its root bones, so undo / save / export see it).
    int viewHoverBall = -1;   // the navigation widget's ball under the pointer (0 +X, 1 +Y, 2 +Z, 3 -X, 4 -Y, 5 -Z); -1 = none
    int selAccessory = -1;                    // scene.parts index of the accessory the gizmo holds, -1 = none
    int gizmoHoverAxis = -1;                  // the axis under the pointer in the view (-1 none, 0 X, 1 Y, 2 Z)
    int gizmoAxis = -1;                       // the axis being dragged, -1 = none
    exporter::MoveGizmoState gizmoState;      // where the last picture drew the gizmo (x < 0: not drawn)
    float gizmoAcc[3] = {0, 0, 0};            // world delta applied during the current drag (for the log line)
    float gizmoDragDx = 0.0f, gizmoDragDy = 0.0f;   // pointer movement of the current drag, applied once per picture
    int hoverAccChip = -1;                    // the accessory chip under the pointer in the panel, -1 = none
    // Physics preview: the DynamicBone chains simulated live while the switch is on.
    assemble::PhysicsSim sim;
    bool physics = false, physicsMoving = false, physicsKick = false;
    bool jiggleBust = true, jiggleButt = true;   // the exported PMX's jiggle physics: both on by default so a shake shows bust + butt
    float jiggleIntensity = 0.5f;                 // how wide it jiggles (0.5 = gentle; 1 = the game's own)
    bool autoCloth = false;                       // Look tab: synthesize bones/physics for hanging cloth without authored DynamicBone (sleeves, wings, tails) — off by default to match the CLI
    float heightCm = 160.0f, baseHeightCm = 160.0f;   // target height (cm) the whole model scales to; base = its measured rest height
    float sag = 1.0f;                                 // static hang scale (1 = the card's own, 0 = none, 2 = double)
    float sagDefault = 1.0f;                          // per-card reset for the sag slider, from the card's own chains/size
    ULONGLONG physicsLast = 0;
    double physicsAccum = 0.0;
    std::vector<Mat4> poseWorld;

    // Frame timing (CARDBRIDGE_BENCH): milliseconds spent in the three parts of a paint, summed, and how many paints ran.
    struct Prof { double render = 0, blit = 0, panel = 0, panelDraw = 0, panelCopy = 0, reshape = 0, refresh = 0, sec[8] = {}; long paints = 0, renders = 0; } prof;

    // ---- global loading bar (every long job: card / outfit / costume loads,
    // mod indexing, GPU uploads, bakes, preview renders, library builds, export) ----
    // Workers report through ProgSet(); the UI thread paints the bar from a
    // snapshot. Cancelling sets progCancel; workers poll it at stage / item
    // boundaries (the core callbacks throw cardbridge::Cancelled).
    std::atomic<bool> progActive{false};
    std::atomic<bool> progCancel{false};
    std::mutex progMu;
    std::string progTitle, progStage;
    double progFrac = -1.0;   // <0 = indeterminate
    int progGen = 0;          // bumped per job, so a stale worker cannot report
    bool progHoverCancel = false;

    std::mutex logMu;
    std::vector<LogLine> logLines;
    HFONT fontTitle = nullptr, fontUi = nullptr, fontBold = nullptr, fontMono = nullptr, fontLog = nullptr, fontSmall = nullptr;
};
App g;

PanelLayout Lay(int H) { return LayoutFor(H, g.tab == TabBones && !g.showIssues, kBoneEditRows); }

// ---- the Material Workbench window's layout ---------------------------------------------------------------------------
// A material list on the left, the editor of the selected material on the right. Computed from the client size, so the
// drawing code and the click code can never disagree about where anything is (both call this).
struct WbLayout {
    int W = 0, H = 0;
    RECT filter{};                       // the filter dropdown button (left column)
    int listTop = 0, listBottom = 0;     // the material list's rows
    int listX0 = 6, listX1 = 300;        // the list card (its right edge also carries the scroll bar)
    int edX = 320, edW = 500;            // the editor's content box
    int edTop = 46;                      // the heading row of the editor
    RECT classBtn{}, presetBtn{}, texBtn{}, pickBtn{}, inspectBtn{};
    RECT resetThis{}, resetAll{}, undoBtn{}, redoBtn{}, saveBtn{}, loadBtn{};
    int trackL = 0, trackR = 0, valueR = 0;   // slider track and where its value is right-aligned
    int canvasTop = 0, canvasH = 0;      // the texture inspector's canvases
    RECT preview{}, uv{};
    RECT chan[5]{}, slotBtn{}, enableBtn{}, replaceBtn{}, fitBtn{};
};

WbLayout WbLayoutFor(int W, int H) {
    WbLayout l;
    l.W = W;
    l.H = H;
    l.filter = {10, 46, 210, 46 + kRowH};
    l.listTop = 76;
    l.listBottom = std::max(l.listTop + kRowH, H - 14);
    l.edX = 320;
    l.edW = std::max(360, W - l.edX - 20);
    l.edTop = 46;
    const int x = l.edX, w = l.edW, top = l.edTop;
    l.trackL = x + 150;
    l.trackR = x + w - 90;
    l.valueR = x + w;
    // Row 1: class / preset / texture.
    l.classBtn = {x, top + kRowH, x + 130, top + 2 * kRowH};
    l.presetBtn = {x + 134, top + kRowH, x + 230, top + 2 * kRowH};
    l.texBtn = {x + 234, top + kRowH, x + w, top + 2 * kRowH};
    // Row 5: HEX + the colour picker and the texture view toggles.
    const int hy = top + 5 * kRowH;
    l.pickBtn = {x + w - 110, hy, x + w - 60, hy + kRowH};
    l.inspectBtn = {x + w - 56, hy, x + w, hy + kRowH};
    // Row 2 of the inspector: channel buttons and the texture slot dropdown.
    const int cy = top + 2 * kRowH;
    for (int i = 0; i < 5; ++i) l.chan[i] = {x + i * 38, cy, x + i * 38 + 36, cy + kRowH};
    l.slotBtn = {x + 200, cy, x + w, cy + kRowH};
    // The canvases take whatever height is left over (rows 6 on).
    l.canvasTop = top + 6 * kRowH;
    l.canvasH = std::max(160, H - l.canvasTop - 96);
    const int pw = std::min(200, w / 3);   // the preview thumbnail's column
    l.preview = {x, l.canvasTop, x + pw, l.canvasTop + l.canvasH};
    l.uv = {x + pw + 10, l.canvasTop, x + w, l.canvasTop + l.canvasH};
    const int ay = l.canvasTop + l.canvasH + 34;   // info, then actions, under the canvases
    l.enableBtn = {x, ay, x + 20, ay + kRowH};
    l.replaceBtn = {x + 160, ay, x + 260, ay + kRowH};
    l.fitBtn = {x + w - 100, ay, x + w, ay + kRowH};
    // The reset / undo / save / load row: straight under the sliders in the shading
    // view, pushed down by the canvases in the texture view.
    const int ry = g.matInspectOpen ? ay + kRowH + 6 : top + 12 * kRowH;
    l.resetThis = {x, ry, x + 85, ry + kRowH};
    l.resetAll = {x + 89, ry, x + 174, ry + kRowH};
    l.undoBtn = {x + 178, ry, x + 234, ry + kRowH};
    l.redoBtn = {x + 238, ry, x + 300, ry + kRowH};
    l.saveBtn = {x + w - 184, ry, x + w - 94, ry + kRowH};
    l.loadBtn = {x + w - 90, ry, x + w, ry + kRowH};
    return l;
}

constexpr UINT WM_LOADED = WM_APP + 1;
constexpr UINT WM_LOG = WM_APP + 2;
constexpr UINT WM_EXPORT_DONE = WM_APP + 3;
constexpr UINT WM_EXP_READY = WM_APP + 4;
constexpr UINT WM_LIBRARY_DONE = WM_APP + 5;    // the library import finished (lParam 1 = ok, 2 = cancelled)
constexpr UINT WM_SETUP_LIBRARY = WM_APP + 6;   // first run: no library yet, offer to build one
constexpr UINT WM_PROGRESS = WM_APP + 7;        // a worker reported progress: repaint the bar
// Test hook (viewer_smoke.ps1): only answered when CARDBRIDGE_TEST_HOOKS is set in the environment, so a normal run ignores it.
// wParam: 1 undo, 2 redo, 3 save the pose, 4 load the pose (both to CARDBRIDGE_POSE_PATH), 10 write the layout JSON to CARDBRIDGE_TEST_LAYOUT, 11 nude body on, clothes and accessories off, the camera on the chest, 12 reset the imported outfit to the card default.
constexpr UINT WM_TEST = WM_APP + 40;
constexpr UINT WM_BENCH = WM_APP + 41;   // CARDBRIDGE_BENCH: run the scripted timing session (see RunBench)
void RunBench();
constexpr UINT_PTR kTimerApplyTweaks = 2;

std::shared_ptr<Loaded> Current() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    return g.loaded;
}

