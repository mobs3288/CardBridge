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
    std::map<std::string, assemble::PartPose> parts;   // gizmo edits, keyed by scene part id
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
    auto samePart = [](const assemble::PartPose& x, const assemble::PartPose& y) {
        if (x.pos.x != y.pos.x || x.pos.y != y.pos.y || x.pos.z != y.pos.z || x.pivot.x != y.pivot.x || x.pivot.y != y.pivot.y || x.pivot.z != y.pivot.z) return false;
        for (int i = 0; i < 16; ++i) if (x.rot.m[i] != y.rot.m[i]) return false;
        return true;
    };
    for (const auto& [id, e] : a.parts) {
        auto it = b.parts.find(id);
        const assemble::PartPose other = it == b.parts.end() ? assemble::PartPose{} : it->second;
        if (!samePart(e, other)) return false;
    }
    for (const auto& [id, e] : b.parts)
        if (!a.parts.count(id) && !assemble::IsIdentity(e)) return false;
    return true;
}

// ---- the loaded character ------------------------------------------------------------------------------------------
struct Loaded {
    assemble::Scene scene;
    card::Card card;
    // Non-null when opened from a native .pmx. The document is authoritative
    // for everything the scene cannot express; edits are copied back into a
    // copy of it at export time, never rebuilt through the card pipeline.
    std::shared_ptr<pmx::Model> pmxDoc;
    std::string pmxDir;
    bool NativePmx() const { return pmxDoc != nullptr; }
    std::string title;
    int outfit = 0, outfits = 0;
    std::vector<size_t> partTriangles;
    std::vector<std::string> notes;   // card-level problems
    std::string clothing;             // the outfit file applied in memory, if any (never saved)
};

// A GPU copy built entirely on the export-bake worker. The worker owns its own
// GpuView (and D3D11 device), so the UI thread keeps rendering the previous view
// while the new one uploads. Adopted - and the old view released - in WM_EXP_READY.
struct PendingExportUpload {
    std::shared_ptr<assemble::Scene> scene;
    std::shared_ptr<Loaded> forCharacter;
    std::unique_ptr<exporter::GpuView> view;
    bool grade = false;
    bool ok = false;
    std::string why;
};

// ---- the Assets tab: card-independent KK/KKS asset catalog ------------------
struct AssetBrowserState {
    std::shared_ptr<library::AssetCatalog> catalog;
    std::string search;
    bool editing = false;
    library::AssetCategory filter = library::AssetCategory::Count;
    int selected = -1;
    float listPos = 0;
    bool loading = false, loaded = false, failed = false;
    std::string error;
    std::string thumbKey;                 // which record the pixels below belong to
    std::vector<uint8_t> thumbRgba;
    int thumbW = 0, thumbH = 0;
};

// ---- the Optimize tab: triangle reduction & LOD generation ------------------
// What Apply commits the export to (an empty OptApplied = the export writes the original, the default).
struct OptApplied {
    bool set = false;              // set by the tab's Apply button; cleared by Reset
    float reduction = 0.0f;        // 0..1 fraction of triangles to remove from the written PMX
    std::vector<exporter::ExportOptions::LodTarget> lods;   // selected LOD targets in LOD 1..4 order (label + remaining fraction)
};

// One LOD level as generated this session. The PMX files are only written on export; this cache is for the tab's previews and stats.
struct OptLod {
    std::shared_ptr<assemble::Scene> scene;   // null when not generated; kept for the individual LOD previews
    size_t triangles = 0, vertices = 0;
    double seconds = 0.0;
    bool pending = false;          // generation queued or running
    bool failed = false;
};

struct OptState {
    int preset = 2;                // 0..4 = mesh::kSimplifyPresets, 5 = Custom
    float reduction = 0.4f;        // the slider; 0..1 (shown as 0..100%)
    bool lodSelected[4] = {true, true, true, true};        // LOD 1..4 checkboxes
    float lodTarget[4] = {0.6f, 0.3f, 0.15f, 0.05f};       // the remaining-fraction target per level (independent, 5..95%)
    OptLod lods[4];
    OptApplied applied;            // what the next export writes
    bool busy = false;             // a generation is running (slider preview or LODs)
    bool stale = true;             // the settings changed since the preview was generated
    std::shared_ptr<assemble::Scene> simplified;   // the generated preview scene at `simplifiedFor` (textures included: the GPU view reads them)
    float simplifiedFor = -1.0f;   // the reduction the cached preview/stats were built for
    size_t simplifiedTris = 0, simplifiedVerts = 0;
    double simplifiedSeconds = 0.0;
    int previewLod = 0;            // 0 = original, 1 = simplified preview, 2..5 = LOD 1..4
    int hover = -1;                // hovered control for paint feedback (kOpt* ids)
    int dragX = 0;                 // the slider drag's pointer x
    int dragging = 0;              // 1 while the slider is dragged
    // WM_OPT_READY hands the generated results from the worker to the UI thread:
    int pendingPreviewFor = -1;    // the slider reduction snapshot the worker worked on (-1 = LODs only)
};

constexpr int kPanel = 380;
constexpr int kRowH = 20;
constexpr int kTabBottom = 278;   // the tab strip's bottom edge (kTabRowTop + 3 rows x kTabCellH + 2 x kTabRowGap = 192+78+8)

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
    float skinAo = 0.85f;                                // Koikatsu's own look baked in (base body normal lighting + the card's detail line mask); 0 = off; saved in the viewer's settings
    float noseRidge = 1.0f;                              // head-mod centre nose shading: 1 = as authored, 0 = off, 2 = stronger (per card)
    float noseRidgeApplied = 1.0f;                       // the last value the user confirmed (the popup can revert to this)
    float noseRidgeOpacity = 1.0f;                       // independent opacity layer: 0 = invisible, 1 = fully visible
    float transSizeCm = 4.0f;                             // translucency brush radius, centimetres
    float transStrength = 0.65f;                          // how see-through a fully painted texel becomes (0..1, like the morph weights)
    bool pinkNipples = true;                              // automatic pink nipple / areola tint (off: the card's own colour)
    Vec3 nippleColour{assemble::kDefaultNippleColour.x, assemble::kDefaultNippleColour.y,
                      assemble::kDefaultNippleColour.z};   // the tint when pinkNipples is on
};
enum Tab { TabParts = 0, TabBody, TabFace, TabLook, TabMorphs, TabBones, TabMaterials, TabProportions, TabOptimize, TabBodyTransfer, TabSettings, TabAssets, TabCount };
const char* const kTabNames[TabCount] = {"Parts", "Body", "Face", "Look", "Morphs", "Bones", "Materials", "Proportions", "Optimize", "Transfer", "Setup", "Assets"};
// Body Transfer tab controls. The measurements use two id ranges: one checkbox row and one editable
// value row per measurement. All ids sit in 24000..24999, clear of every other control range.
constexpr int kBTReference = 24000, kBTTarget = 24001, kBTRefresh = 24002, kBTOpenRef = 24003, kBTSaveSpec = 24004, kBTLoadSpec = 24005,
              kBTSelectAll = 24010, kBTDeselectAll = 24011, kBTApply = 24012, kBTReset = 24013, kBTPreview = 24014,
              kBTRefine = 24015, kBTNude = 24016,
              kBTMeasure0 = 24100, kBTMeasureMax = 24199, kBTSelect0 = 24200, kBTSelectMax = 24299;
inline bool IsBodyTransferId(int id) { return id >= kBTMeasure0 && id < kBTMeasureMax; }
inline int BodyTransferMeasureOf(int id) { return id >= kBTMeasure0 && id < kBTMeasureMax ? id - kBTMeasure0 : -1; }
// Slider ids: body 0..43, face 100..151, then the look sliders; switches from 300.
// Pose controls change the picture at once (no re-assembly): the A-pose switch and angle, and the nine fields of the selected bone (10000..10008).
// Settings tab actions (above every other id, so the slider and switch code never mistakes one for its own).
constexpr int kSetLibChoose = 20000, kSetLibBuild = 20001, kSetModsChoose = 20002, kSetModsReindex = 20003, kSetModsClear = 20004, kSetExportDir = 20006,
              kSetExportAsk = 20007, kSetLayers = 20008, kSetLayersDefault = 20009, kSetOpenFolder = 20010, kSetForget = 20011;
constexpr int kSetGpuAuto = 20012, kSetGpuHardware = 20013, kSetGpuWarp = 20014, kSetGpuSoftware = 20015, kSetSkinGpu = 20016, kSetSkinCpu = 20017, kSetSkinAuto = 20018;
constexpr int kSetProfileKK = 20021, kSetProfileKKS = 20022;
// Direct per-profile folder pickers (no need to switch profiles first).
constexpr int kSetKKLib = 20023, kSetKKMods = 20024, kSetKKSMods = 20025, kSetKKSLib = 20026, kSetModsBoth = 20027;
constexpr int kTransClear = 20019;   // the Look tab's "clear the brush from every garment" button
// The Optimize tab's controls (its own range; nothing else uses it). 31000..31005 presets, 31010..31013 LOD checkboxes,
// 31020 Generate LODs, 31021 Apply, 31022 Reset, 31030..31035 preview chips (original, simplified, LOD 1..4).
constexpr int kOptPresetOriginal = 31000, kOptPresetHigh = 31001, kOptPresetBalanced = 31002, kOptPresetPerformance = 31003, kOptPresetAggressive = 31004, kOptPresetCustom = 31005;
constexpr int kOptLodBase = 31010, kOptGenerate = 31020, kOptApply = 31021, kOptReset = 31022, kOptPreviewBase = 31030;
constexpr int kOptLodDown = 31040, kOptLodUp = 31050;   // per-level target steppers: 31040..31043, 31050..31053 (LOD 1..4)

// What the Optimize worker hands to the UI thread on WM_OPT_READY (like expPending for the export bake).
struct OptPending {
    int kind = 0;                    // 0 = slider preview, 1 = LOD generation
    std::shared_ptr<assemble::Scene> simplified;   // kind 0: the generated preview (null on cancel/failure)
    float simplifiedFor = 0.0f;
    mesh::SceneSimplifyStats simplifiedStats;      // kind 0
    std::vector<mesh::LodResult> lods;             // kind 1
    bool done = false;
    bool cancelled = false;
};

// ---- the Selective Import window (Selective.inc) ------------------------------------------------
// A second top-level window, like the Material Workbench: a dedicated multi-view outfit composer for
// the clothing import. The user picks, per component, Add / Replace / Keep existing / Skip;
// three real 3D views (current character, imported source, proposed result) render it. Nothing is
// applied until Apply; Cancel discards the whole thing.
struct SelView {
    exporter::ViewCamera cam;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;        // the size `frame` was rendered at (guarded by frameMtx)
    std::mutex frameMtx;        // the render worker swaps `frame` while WM_PAINT blits it
    std::atomic<bool> dirty{true};   // the picture needs re-rendering (scene/pose/camera/highlights changed)
    // Every view owns its Direct3D device. (View 0 used to render through the main window's device, but
    // the pictures are produced on the render worker now, and a device context is not safe to use from two threads.)
    // The device lives behind a shared_ptr so SelectiveClose can hand it to a detached cleaner thread that
    // waits for the render worker's exit and then releases it — the window closes without waiting on a GPU call.
    std::shared_ptr<exporter::GpuView> gpu = std::make_shared<exporter::GpuView>();
    std::atomic<bool> gpuOk{false};
    std::unique_ptr<exporter::ViewState, exporter::ViewStateDeleter> swState = exporter::NewViewState();   // the software fallback's cache (recreated lazily after a steal)
    std::shared_ptr<const assemble::Scene> scene;   // null until the worker finishes (for view 0: aliases the loaded character)
    std::vector<char> hidden;                  // effective hidden parts for this view's scene
    std::atomic<int> state{0};              // 0 idle, 1 building, 2 ready, 3 failed
    std::string error;
    // build progress (the workers report; WM_PAINT draws it — views 1 and 2 show the bar while assembling)
    std::atomic<double> progFrac{-1.0};     // <0: nothing deterministic yet (indeterminate marquee)
    std::atomic<uint64_t> progTicks{0};     // phase counter for the indeterminate/marquee animation
    std::atomic<uint64_t> progPostedMs{0};  // throttles WM_SEL_FRAME posts between progress events (10 Hz)
    std::mutex progMtx;                     // guards progStage
    std::string progStage;
    std::vector<std::vector<int>> compParts;   // per component index: scene part indices (parallel to comps)
    std::vector<Mat4> previewSkin;             // views 1 and 2: the shared A-pose's skin matrices (empty = rest pose)
    std::map<std::string, int> placedAcc;      // source accessory key -> merged slot (where each chosen accessory landed)
};

struct SelectiveComp {
    card::SelectiveComponent comp;
    std::string sourceName;    // the source item's display name (resolved from the previews; falls back to the slot label)
    std::string existingName;  // the counterpart's display name (empty when there is no counterpart)
    std::string sourceStatus;  // "resolved-mod", "missing-mod", ... - shown on a missing item
    std::string modName;       // the mod the source item comes from, when known
    bool missing = false;      // the source item failed to resolve (nothing to import / blocked)
};

struct SelectiveState {
    HWND hwnd = nullptr;
    bool open = false;
    int session = 0;   // incremented per open; stale build messages from a previous window are ignored
    std::mutex selGpuMu;   // uploads (worker threads) vs. rendering (UI thread) vs. drop-on-close
    // the source file and where it will land
    std::string srcPath;
    bool srcIsCoordinate = false;
    std::string srcName;
    int srcOutfit = -1;      // coordinate sources: which coordinate of the source (-1 until resolved)
    int dstOutfit = -1;      // the outfit slot the import lands on (the on-screen outfit)
    // the three views: current character / imported source / proposed result
    SelView views[3];
    std::vector<SelectiveComp> comps;
    std::map<std::string, card::SelectiveAction> decisions;
    std::atomic<int> hoverComp{-1}, selComp{-1};   // hover (transient) and selected (sticky, -1 = none)
    std::atomic<int> hoverAction{0};               // hit id of a row/right-column/link button (0 = none; see SelBtn)
    int listScroll = 0;
    float listPos = 0;
    bool linkCams = true, aPose = true;
    int dragging = 0;                   // 1 orbit, 2 pan (which button)
    int dragView = -1;                  // which viewport the drag started in
    int dragX = 0, dragY = 0;           // last pointer position
    POINT downAt{0, 0};                 // press position (for the click-vs-drag distinction)
    bool dragMoved = false;
    // the render worker: all Direct3D work happens here (never on the UI thread), so hover, camera drags
    // and list scrolling never wait for a picture. WM_SEL_FRAME wakes the paint path when fresh frames land.
    std::thread renderWorker;
    std::mutex renderMtx;
    std::condition_variable renderCv;
    bool renderStop = false;       // set on close/reset; the worker bails between pictures and exits
    // the build debounce (a decision change rebuilds the proposed scene 250 ms later)
    ULONGLONG debounceTick = 0;
    int proposalGen = 0;                // requests outstanding for the proposed scene (generation counter)
    bool proposalQueued = false;        // a rebuild is pending the debounce
    bool buildingProposal = false;      // a proposal worker is running
    std::string proposalKey;            // canonical key of the last queued decision set (sentinel "\x01" = never): unchanged toggles skip the rebuild
    std::map<std::string, std::shared_ptr<assemble::Scene>> proposalCache;   // the assembled scene per effective key, so revisiting an earlier choice is instant
    std::vector<std::string> proposalCacheLru;
    std::map<std::string, std::shared_ptr<assemble::Scene>> importedCache;   // the imported (view 1) scene per source+outfit — identical regardless of decisions
    std::vector<std::string> importedCacheLru;
    // validation for the Apply button (recomputed at paint; error text blocks Apply)
    std::string validation;             // empty = apply is fine
    bool validationError = false;
    std::vector<std::string> notes;     // warning lines for the user (hair replaced, relocated, ...)
    std::string status;                 // footer line
    ULONGLONG lastHoverPick = 0;        // the pick throttle
};
void SettingsAction(int id);
void MorphSlideEdited();   // Morphs tab: a slider moved, or the tab cleared the weights
void LoadMorphCatalog();   // after a character is built: collect the exported PMX's vertex morphs (weights all 0)
void BeginMorphEdit(int morphIndex);   // Morphs tab: type a display name for the morph (Shift+click its row)
void EndMorphEdit(bool commit);        // Enter commits (the alias is kept in settings); Esc restores the original
bool MorphEditKey(UINT wp);            // while the rename box is open the keyboard goes to it
bool MorphEditChar(wchar_t ch);        // typed characters; called from WM_CHAR
constexpr int kFaceBase = 100, kNippleMm = 200, kAreola = 201, kAPoseDeg = 202, kHeightCm = 203, kSag = 204,
              kSkinShade = 205, kNipplePink = 206, kNippleR = 207, kNippleG = 208, kNippleB = 209,
               kSkinShadeUnder = 210, kSkinShadeTop = 211, kSkinShadeWidth = 212, kSkinAo = 213,
               kTransStrength = 214, kTransSize = 215, kNoseRidge = 216, kNoseRidgeOpacity = 217,
              kNippleIn = 300, kAPose = 301, kPhysics = 302, kJiggleBust = 303, kJiggleButt = 304, kAutoCloth = 306,
              kOriginalGarment = 309, kBakeQuality = 310, kBakeUpscale = 311, kBakeUpscalerModel = 312,   // Look tab: bake quality / upscale settings
              kJiggleMass = 320, kJiggleStiffness = 321, kJiggleDamping = 322, kJiggleGravity = 323,
              kJiggleAmpVertical = 324, kJiggleAmpHorizontal = 325, kJiggleAmpDepth = 326,
              kJiggleRotation = 327, kJiggleStrength = 328, kJiggleSimHz = 329, kJigglePreset = 330,
              kTransBrush = 307, kTransErase = 308,
              kLayerBase = 400, kBoneEdit = 10000, kBoneEditCount = 9, kMatBase = 5000, kMatCount = 9,   // kAutoCloth: synthesized cloth skeleton (Look tab); switches from 400: the shader layers
              kPropMaster = 23100, kPropSugs0 = 23101, kPropSugsMax = 23199,   // Proportions tab: master switch, then one switch per suggested correction
               kPropActSuggest = 23200, kPropActApply = 23201, kPropActReset = 23202,
               kPropPresetHead = 23210, kPropPresetBody = 23211, kPropPresetFace = 23212, kPropPresetOverall = 23213,
               kBustBand = 21000, kBustCup = 21001, kNippleSize = 21002,   // preset rows (chips), not sliders: 21000+ keeps them clear of every slider and action id
              kMorphBase = 30000, kMorphResetAll = 23000,   // the Morphs tab: one slider per morph of the exported PMX (30000..), the reset-all button
              kBwhBust = 22100, kBwhWaist = 22101, kBwhHip = 22102;   // the Body measurements macro rows: one number per measurement, not a card index
constexpr int kBodyBust = 4;   // the Bust > Size slider in shape::BodySliders() (body slider ids are the shape's index, 0..43)
struct TweakRow {
    int kind = 0;        // 0 group header (id -1: nothing to reset), 1 slider, 2 switch, 4 plain text, 5 a settings action (a button row), 6 a preset-chips row (band / cup / nipple size)
    int id = 0;
    std::string label, group;
};

struct PanelLayout {
    RECT logo{}, tabs[TabCount]{};   // the tab strip: 3 rows x 5 pill tabs, equal cells, no scrolling
    RECT open{}, exportPmx{}, issues{}, reset{}, all{}, asExported{};
    RECT clothingReset{}, clothingExport{}, clothingPrev{}, clothingNext{};
    int outfitNameTop = 0, outfitChipsTop = 0, outfitClothTop = 0;
    int partsTop = 0, partsBottom = 0, logTop = 0, logBottom = 0, footerTop = 0;
    int boneListTop = 0, boneListBottom = 0, boneEditTop = 0;   // Bones tab: a switch row, the tree, then the editor of the selected bone
    int matListTop = 0, matListBottom = 0;                      // Materials tab (the launcher): the list under the filter + open rows
    RECT clearLog{};
    // Bones tab hit areas (the click code and the drawing use the same ones): the two switches, the reset row, the undo / redo / save / load row
    RECT boneSkeleton{}, boneHelpers{}, boneResetOne{}, boneResetAll{}, boneUndo{}, boneRedo{}, boneSave{}, boneLoad{};
    // Materials tab hit areas: the filter dropdown and the button that opens the Material Workbench window
    // (the editor itself lives in that window, so it has no panel hit areas).
    RECT matFilter{}, matOpen{};
    // Assets tab: a search row, category chips, the result list, then the detail/preview card.
    RECT assetSearch{}, assetCats{}, assetDetail{};
    int assetListTop = 0, assetListBottom = 0;
};

constexpr int kBoneEditRows = 12;   // heading, nine fields, the reset row, the undo / redo / save / load row
constexpr int kHeaderBottom = 64;   // the header band (logo, wordmark, card title) ends here
constexpr int kTabRowTop = 192;     // the tab strip's three rows of pills
constexpr int kTabCellH = 26;
constexpr int kTabRowGap = 4;

PanelLayout LayoutFor(int H, bool boneTab = false, int editRows = kBoneEditRows);   // defined after App (needs g)

// One candidate reference card for the Body Transfer tab. Loading it never touches g.loaded: it gets
// its own parsed card + assembled scene, exactly like Selective Import's source view.
struct TransferRefEntry {
    std::string path, sha256, name, error;
    std::shared_ptr<card::Card> card;
    std::shared_ptr<assemble::Scene> scene;
    measure::TransferSet measurements{};
    bool loading = false, ready = false;
};

// Per-card, non-destructive state of the Body Transfer tab. `appliedSpec` is the layer the preview was
// built with; `spec` is what the user is editing. Reset drops the spec and restores the card's sliders.
struct BodyTransferState {
    std::vector<TransferRefEntry> refs;
    int selectedRef = -1;
    assemble::transfer::Spec spec;
    assemble::transfer::Spec appliedSpec;
    std::vector<float> appliedBody;           // the working slider vector captured when Apply ran
    bool previewOriginal = false;
    bool nudePreview = false;                 // hide outfit/accessories so the transferred body is visible
    std::vector<char> savedHidden;            // the visibility state restored when nude preview is turned off
    bool refine = false;                      // measured correction is opt-in: the 1-D solves can fight each other
    bool applied = false;
    int loadGeneration = 0;
    std::string status;
};

// One entry of the Proportions tab's suggestion list: the op plus whether the user wants it applied.
struct PropSugItem {
    assemble::CorrectionOp op;
    bool applied = false;
};

// An outfit file (or a Coordinate PNG) applied in memory only (never saved as a card): the path, the
// outfit slot it overwrites, and — for a Selective Import — the per-component choices. Layers are
// applied in order on every reload, so Selective Imports from several cards compose into one outfit
// (each reload re-applies the stack from the card on disk, in order). A Replace All / Coordinate
// import is always a single layer (it replaces the whole stack, the classic single-import overwrite).
// clothingMode: 0 = outfit import; 1 = Coordinate import (the whole coordinate replaces the slot,
// including accessories); 2 = selective import. clothingSrcOutfit: for a Coordinate source, which
// coordinate of the source card is imported (-1 = the source's own current outfit).
struct ClothingPin {
    std::string path, name;                     // the source file and its display label
    int mode = 0;                                // 0 clothing, 1 coordinate, 2 selective, 3 selective layer (a component grafted on top of the shown outfit)
    int srcOutfit = -1;                          // coordinate sources: which coordinate of the source (-1 = current)
    int dstOutfit = -1;                          // the outfit slot the layer targets (pinned to the slot on screen at Apply time; -1 = wherever the card is shown)
    std::map<std::string, card::SelectiveAction> decisions;   // mode == 2: the per-component choices
    std::vector<int> layerSlots;                 // mode == 3: clothes slot indices (0..8) the source contributes as layers
};

struct App {
    HWND hwnd = nullptr;
    Settings settings;
    bool cliLibraryOverride = false;   // --library on the command line: never auto-switch profiles or persist its path
    bool cliModsOverride = false;      // --mods on the command line
    std::string cardPath;
    std::vector<ClothingPin> clothingPins;   // the imported-clothing layers of the outfit on screen, in apply order (first to last); empty = none
    int wantOutfit = -1;
    std::string wantPose;                     // --pose <file>: a pose to load once the first card is up
    std::string pendingCard;                  // a card to open once the library has been set up
    int pendingOutfit = -1;
    // Library state. The mod database has its own lifecycle: it is keyed only
    // by the mod source, never by the card's game or the library path. The
    // resolver is the card-specific part (library + lists + game filter) and
    // is rebuilt cheaply on a card/profile switch while the index is reused.
    resolve::ModIndexCache modsCache;
    resolve::ListTable lists;
    std::shared_ptr<resolve::Resolver> resolver;   // shared with the worker threads (assembly in flight keeps it alive)
    std::string resolverKey;                       // library|mods pair this resolver was built for — not an index key

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
    std::shared_ptr<PendingExportUpload> expUploadPending;   // worker-built GPU copy, adopted on WM_EXP_READY
    std::shared_ptr<Loaded> expFor, expPendingFor;           // the loaded character the baked copy belongs to
    bool showExp = false, expOk = false, expGrade = false, expPendingGrade = false;
    OptState opt;                      // the Optimize tab (triangle reduction, presets, LODs)
    OptPending optPending;             // what the Optimize worker produced; adopted on WM_OPT_READY
    SelectiveState sel;                // the Selective Import window (Selective.inc)
    AssetBrowserState assets;          // the Assets tab (card-independent KK/KKS catalog)
    std::atomic<bool> expBusy{false};
    std::atomic<int> expGen{0};                // bumped on a new character or a new bake: a stale worker may not publish
    exporter::ViewOptions vopt;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;
    int viewMode = 0;                // exporter::ViewMode (0 = Lit, the default); persisted in viewer.json ("viewMode")
    bool viewIsolate = false;        // MaterialID mode: hide every part not using the selected material (session only)
    int viewHoverCell = -1;          // the mode strip cell the cursor is over (-1 = none); repaint only that band
    ULONGLONG lastInput = 0;   // tick of the last camera change: the game shaders run only once the view has been still for a moment
    bool interacting = false, dirty = true, showIssues = false, textured = true, shaded = true;
    bool shiftDown = false, ctrlDown = false;   // tracked here (a posted key works like a real one); read these, not GetKeyState
    POINT last{};
    int drag = 0;   // 1 orbit, 2 pan
    int partScroll = 0, logScroll = 0;        // list scroll targets, in rows
    float partPos = 0, bonePos = 0;           // what the lists show now: the targets, eased (smooth scrolling)
    ui::Surface panel;                        // the panel, drawn off screen and copied where it changed
    // Mouse movement during a drag is only noted here; Frame() applies it once per picture, so a fast mouse cannot queue up work.
    bool dragPending = false;
    int dragX = 0;                            // slider drag: the pointer's x
    float boneDragDx = 0, boneDragDy = 0;     // bone drag: movement since the last picture
    double lastFrameTick = 0.0;              // NowMs() of the previous picture
    bool animating = false;                   // a list is still easing towards its scroll target
    int hoverRow = -1, hoverPart = -1, hoverButton = 0;   // button: 0 none, 1 open, 2 export, 3 issues, 4 reset, 5 all, 6 MMD, 7 reset clothing, 10+ tabs, 21 export clothing
    int hoverOutfit = -1;                 // slot the cursor is over, for the name line
    int loadingOutfit = -1;               // outfit the worker is assembling, or -1
    int soloPart = -1;
    bool listScroll = false;       // the gesture became a list scroll (the row's click is swallowed)
    int listPressY = -1;           // the y of the press in the list, or -1 when the list is not pressed
    int listPressX = 0;            // the x of the press
    float listPressPos = 0.0f;     // partPos at the press: the list offset the drag maps from
    std::set<std::string> collapsedGroups;
    std::set<std::string> collapsedMorphs;   // morph groups folded in the Morphs tab (Brows / Eyes / Mouth / Other)
    std::set<std::string> explorerCollapsed; // Model Explorer sections folded (native PMX models)
    Tweaks tw;
    // Proportion correction (optional; faithful when empty). The card is never modified: corrections live in a
    // separate layer applied in the assemble/reshape pipeline. Per-card state; suggestions are recomputed on demand.
    bool propEnabled = true;                               // master layer switch; no applied ops => faithful regardless
    std::string propCardKey;                               // the card the layer currently belongs to (a different card clears it)
    std::vector<PropSugItem> propSugs;                     // the current suggestion list, with applied flags
    std::vector<assemble::ProportionFinding> propFindings;   // diagnostics, computed once per load (or on demand)
    // Body Transfer: reference cards and the editable measurement transfer (never modifies the card).
    BodyTransferState bt;
    // The Body tab's "Body measurements" group reads the skin. Re-measured on every reshape (the scene holds the rest
    // pose, so bone pose edits don't skew it); the baseline is the card's own measurement, captured on load.
    measure::Result bwhMeasure;
    float bwhCardDef[3] = {0, 0, 0};            // the card's own bust / waist / hip, in metres (the row's "unchanged" marker)
    bool bwhCardDefSet = false;                 // baseline captured (set false when a fresh card is loaded)
    bool bwhSolving = false;                    // typed cm entry is re-shaping many times in a row: keep the screen still
    std::array<float, 2> bwhDragBase{};         // the coupled sliders' values when a drag began (the delta mapping anchor)
    int bwhDragId = -1;                         // the BWH row being dragged, or -1
    // The Morphs tab: every vertex morph the exported PMX carries (the game's FaceBlendShape expressions, the raw blend shapes
    // and the MMD standard ones, exactly as the export picks them), accumulated per weight on top of the rest shape. Weights are
    // live-only: they are not exported and not saved (the PMX carries the morphs; MMD plays them). morphDelta is the per-mesh
    // accumulation at the current weights, handed to the renderers.
    exporter::LiveMorphCatalog morphCatalog;
    std::vector<float> morphWeights;              // parallel to `morphCatalog.morphs`; 0..1, the sliders show 0..100
    std::vector<std::vector<Vec3>> morphDelta;    // parallel to scene.meshes; empty for meshes no active morph touches
    int tab = TabParts;
    int optScroll = 0;                      // the Optimize tab's vertical scroll, in pixels (wheel when the window is short)
    int sliderDrag = -1;                      // slider id being dragged, or -1
    // Shift+Left-click on a slider: type its value with the keyboard. The edit is modal for the mouse/keyboard until
    // Enter commits (the typed number, clamped and snapped to the slider's step), Esc restores the value it had, or any
    // click ends it (committing). Live upload happens on commit only; the row shows the typed text with a caret.
    int sliderEdit = -1;                      // slider id being typed into, or -1
    std::string sliderEditBuf;                // the text typed so far
    float sliderEditOrig = 0;                 // the stored value when the edit began (Esc restores it)
    bool sliderEditMatPushed = false;         // one material-undo step per typing session
    PoseState sliderEditPoseBefore;           // the pose when the edit began (a pose slider pushes one undo step on commit)
    // Shift+Left-click on a morph row: type a display name for it. Enter commits (the alias is kept in the viewer's settings,
    // so it survives restarts), Esc puts the original back. The PMX is untouched - MMD motions key morphs by name.
    int morphEdit = -1;                       // morph index being renamed, or -1
    std::string morphEditBuf;                 // the text typed so far
    std::string morphEditOrig;                // the alias when the edit began (Esc restores it)
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
    // The part move/rotate gizmo: shown in the view when any part is selected in the list (its chips); dragging an axis
    // moves it, dragging a ring rotates it. The edit is a rigid bind-space transform on that part's own vertices, so a
    // garment sharing the body's bones moves alone yet stays glued to the character through any bone pose.
    int viewHoverBall = -1;   // the navigation widget's ball under the pointer (0 +X, 1 +Y, 2 +Z, 3 -X, 4 -Y, 5 -Z); -1 = none
    std::map<std::string, assemble::PartPose> partEdits;   // gizmo edits, keyed by scene part id (undo / save / export see them)
    int selGizmoPart = -1;                    // scene.parts index of the part the gizmo holds, -1 = none
    int gizmoMode = 0;                        // 0 = move (arrows), 1 = rotate (rings): which gizmo the chips on the row picked
    int gizmoHoverAxis = -1;                  // the axis under the pointer in the view (-1 none, 0 X, 1 Y, 2 Z)
    int gizmoAxis = -1;                       // the axis being dragged, -1 = none
    exporter::MoveGizmoState gizmoState;      // where the last picture drew the move gizmo (x < 0: not drawn)
    float gizmoAcc[3] = {0, 0, 0};            // world delta applied during the current drag (for the log line)
    int rotGizmoHoverAxis = -1;               // the ring under the pointer in the view (-1 none, 0 X, 1 Y, 2 Z)
    int rotGizmoAxis = -1;                    // the ring being dragged, -1 = none
    exporter::RotateGizmoState rotGizmoState; // where the last picture drew the rotate gizmo (x < 0: not drawn)
    float rotGizmoAcc[3] = {0, 0, 0};         // degrees turned about each axis during the current drag (for the log line)
    float rotGizmoTheta = 0.0f;               // the pointer's angle in the ring plane at the last applied step of the drag
    float rotGizmoPtrX = 0.0f, rotGizmoPtrY = 0.0f;   // the last pointer position of the current ring drag (view pixels)
    bool transMmdWasActive = false;           // the stroke started while the "as exported" view showed (so the bake returns to it after release)
    bool transRebakePending = false;          // the brush changed pixels; rebake the exported preview instead of staying on the interactive view
    float gizmoDragDx = 0.0f, gizmoDragDy = 0.0f;   // pointer movement of the current drag, applied once per picture
    int hoverAccChip = -1;                    // the accessory move chip under the pointer in the panel, -1 = none
    int hoverRotChip = -1;                    // the accessory rotate chip under the pointer in the panel, -1 = none
    // Translucent clothes (Look tab): the brush paints the see-through area of the garment under the cursor. The mask is
    // baked into the garment's own texture (see export/Translucency.cpp): session-only, kept in the scene, undone by the toggle.
    bool transBrush = false;                  // the toggle: off by default, like every brush that changes pixels
    bool transErase = false;                  // erase the paint instead of laying it down (right-drag always erases)
    bool transPaintPending = false;           // the pointer moved during a stroke; applied once per picture
    int transPaintX = 0, transPaintY = 0;     // the pointer in view pixels (the stroke's current position)
    bool transEraseStroke = false;            // the current stroke erases (right-drag or the erase switch)
    int paintPart = -1;                     // the part the current stroke paints (-1 = none); its front and back shells share the atlas and are painted together
    float paintAnchorU = 0.0f, paintAnchorV = 0.0f;   // the last stamped UV: the stroke interpolates from here
    bool paintAnchorSet = false;
    int transHoverX = -1, transHoverY = -1;   // the pointer in view pixels for the brush ring; -1 when not over the view or the mode is off
    // Physics preview: the DynamicBone chains simulated live while the switch is on.
    assemble::PhysicsSim sim;
    bool physics = false, physicsMoving = false, physicsKick = false;
    bool jiggleBust = true, jiggleButt = false;   // the exported PMX's jiggle physics: the bust jiggle is on by default, the butt jiggle is off (the card's butt sway reads as the back of the skirt vibrating; flip in the Look tab or --jiggle-butt)
    assemble::JiggleSettings jiggle;   // the physical settings the preview and the export share (Strength replaces the old jiggle intensity)
    bool autoCloth = false;                       // Look tab: synthesize bones/physics for hanging cloth without authored DynamicBone (sleeves, wings, tails) — off by default to match the CLI
    bool originalGarment = true;                  // Look tab: keep the card's own garment vertices, the drape exactly as the viewer shows it (no vertex fix); off = run the clipping correction in the preview and the export
    int bakeQuality = 0;                          // Look tab: shader bake quality, 0 = standard (1024), 1 = high (2048), 2 = ultra (4096); same presets as the CLI
    bool upscaleTextures = false;                 // Look tab: upscale the PMX's colour textures 2x (open source, optional)
    bool upscaleUseEsrgan = false;                // Look tab: Real-ESRGAN anime model instead of waifu2x
    float heightCm = 160.0f, baseHeightCm = 160.0f;   // target height (cm) the whole model scales to; base = its measured rest height
    float sag = 1.0f;                                 // static hang scale (1 = the card's own, 0 = none, 2 = double)
    float sagDefault = 1.0f;                          // per-card reset for the sag slider, from the card's own chains/size
    double physicsLast = 0.0;                         // NowMs() of the last physics tick
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
    HFONT fontTitle = nullptr, fontUi = nullptr, fontBold = nullptr, fontMono = nullptr, fontLog = nullptr, fontSmall = nullptr, fontSemi = nullptr;   // fontSemi: 11 px semibold, section captions
};
// Intentionally never destroyed: workers are detached and may still be unwinding
// while the process is exiting. A leaked singleton has no destructor for them to race with.
App& g = *new App();

// The label for the pinned clothing on screen: empty when nothing is pinned; the last layer's name with
// a "(+N more)" suffix when several Selective Import layers are stacked.
std::string ClothingDisplayName() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    const size_t n = g.clothingPins.size();
    if (n == 0) return {};
    const std::string& last = g.clothingPins.back().name;
    return n == 1 ? last : last + " (+" + std::to_string(n - 1) + " more)";
}

PanelLayout LayoutFor(int H, bool boneTab, int editRows) {
    PanelLayout l;
    l.logo = {12, 10, 50, 50};
    // Header band 0..kHeaderBottom; the toolbar sits 8 px below it.
    const int by = kHeaderBottom + 8, bh = 30;
    l.open = {10, by, 62, by + bh};
    l.exportPmx = {68, by, 162, by + bh};
    l.issues = {168, by, 222, by + bh};
    l.reset = {228, by, 298, by + bh};
    l.all = {304, by, 330, by + bh};
    l.asExported = {336, by, 370, by + bh};
    // The outfit picker: a name line, then a row of numbered slots, then the imported-outfit status line with its reset button.
    l.outfitNameTop = 116;
    l.outfitChipsTop = 136;
    l.outfitClothTop = 164;
    l.clothingPrev = {148, l.outfitClothTop, 172, l.outfitClothTop + 20};
    l.clothingNext = {176, l.outfitClothTop, 200, l.outfitClothTop + 20};
    l.clothingExport = {206, l.outfitClothTop, 292, l.outfitClothTop + 20};
    l.clothingReset = {296, l.outfitClothTop, kPanel - 10, l.outfitClothTop + 20};
    // The tab strip: three rows of five equal pill tabs (no scrolling). The third row is Transfer + Setup.
    constexpr int kTabCellW = 68, kTabGap = 5;
    for (int i = 0; i < TabCount; ++i) {
        const int col = i % 5, row = i / 5;
        const int x = 10 + col * (kTabCellW + kTabGap), y = kTabRowTop + row * (kTabCellH + kTabRowGap);
        l.tabs[i] = {x, y, x + kTabCellW, y + kTabCellH};
    }
    l.partsTop = kTabBottom + 8;
    const int logH = boneTab ? 100 : std::clamp(H / 3, 130, 330);   // the bone/material editor needs the room
    l.footerTop = H - 36;
    l.logBottom = l.footerTop - 4;
    l.logTop = std::max(l.partsTop + 90, l.logBottom - logH);
    l.partsBottom = std::max(l.partsTop + kRowH, l.logTop - 10);
    l.boneEditTop = std::max(l.partsTop + 3 * kRowH, l.partsBottom - editRows * kRowH - 4);
    l.boneListTop = l.partsTop + kRowH;
    l.boneListBottom = l.boneEditTop - 4;
    l.clearLog = {kPanel - 46, l.logTop + 3, kPanel - 8, l.logTop + 23};
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
    // Assets tab: search + category chips, a scrolling result list, then a detail/preview card.
    l.assetSearch = {10, l.partsTop, kPanel - 10, l.partsTop + kRowH};
    l.assetCats = {10, l.partsTop + kRowH + 4, kPanel - 10, l.partsTop + 2 * kRowH + 8};
    l.assetListTop = l.assetCats.bottom + 6;
    l.assetListBottom = std::max(l.assetListTop + kRowH, l.partsBottom - 6 * kRowH);
    l.assetDetail = {6, l.assetListBottom + 4, kPanel - 6, l.partsBottom};
    return l;
}


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
    l.texBtn = {x + 234, top + kRowH, std::min(x + w, x + 384), top + 2 * kRowH};
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
constexpr UINT WM_OPT_READY = WM_APP + 8;       // the Optimize generation finished (wParam: 1 cancelled, 2 failed)
constexpr UINT WM_SEL_READY = WM_APP + 9;      // the Selective Import window's workers finished (wParam: 0 = imported source view, 1 = proposed scene; lParam = session)
constexpr UINT WM_SEL_FRAME = WM_APP + 10;     // the render worker produced fresh pictures; repaint
constexpr UINT WM_BT_READY = WM_APP + 11;      // a reference card finished loading for the Body Transfer tab
constexpr UINT WM_ASSETS_READY = WM_APP + 12;  // the asset catalog build finished (wParam 1 = failed)
constexpr UINT WM_ASSETS_FRAME = WM_APP + 13;  // a thumbnail decode finished (wParam: 1 = no image)
// Test hook (viewer_smoke.ps1): only answered when CARDBRIDGE_TEST_HOOKS is set in the environment, so a normal run ignores it.
// wParam: 1 undo, 2 redo, 3 save the pose, 4 load the pose (both to CARDBRIDGE_POSE_PATH), 10 write the layout JSON to CARDBRIDGE_TEST_LAYOUT, 11 nude body on, clothes and accessories off, the camera on the chest, 12 reset the imported outfit to the card default.
constexpr UINT WM_TEST = WM_APP + 40;
constexpr UINT WM_BENCH = WM_APP + 41;   // CARDBRIDGE_BENCH: run the scripted timing session (see RunBench)
void RunBench();
void SelectiveOpen(const std::string& path);   // the Selective Import window (Selective.inc): per-component outfit import preview
void SelectiveClose();
void SelectiveInvalidateView();
void SelectivePoseChanged();                   // called when the pose moved: reskins the static previews
void SelApply();                               // apply the selective import (through the validation guard)
std::vector<TweakRow> BuildBodyTransferRows();   // Body Transfer tab (BodyTransfer.inc)
void BodyTransferReady(int index, int generation);
constexpr UINT_PTR kTimerApplyTweaks = 2;

// Defined in Clicks.inc; declared here because Pose.inc (the card loader) runs
// before the click handlers in the viewer's include order.
void StoreActiveProfile();
void ApplyProfile(const std::string& key, bool announce = true);
void ActivateProfileForCard(card::CardFamily family);
void WarnLibraryMismatch(card::CardFamily family);

std::shared_ptr<Loaded> Current() {
    std::lock_guard<std::recursive_mutex> lk(g.mu);
    return g.loaded;
}

