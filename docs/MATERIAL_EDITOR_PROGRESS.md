# CardBridge — Material Workbench: Progress Log

Stage-by-stage record. Each stage ends with: compile result, test result,
files changed, decisions made, remaining limitations. No stage is marked
complete on the basis of UI existing; behaviour is checked.

---

## Stage 0 — Investigation (2026-11-09, branch `feature/material-workbench`)

**Deliverables (docs only, no source changes):**

- `docs/MATERIAL_EDITOR.md` — architecture assessment (§2, verified against
  source), data model (§3), field mapping incl. PMX capabilities (§3.4),
  renderer analysis (§2.4), implementation plan (§9), testing strategy (§8),
  performance/compatibility (§7), risks (§10), non-goals (§11).
- `docs/ARCHITECTURE_DECISIONS.md` — ADR-001…ADR-008.
- `docs/MATERIAL_EDITOR_PROGRESS.md` — this file.

**What was verified (not inferred):**

- Material data structures: `unity::MaterialData` (UnityScene.h:56-73),
  `assemble::SceneMaterial` (Scene.h:55-80), `card::MaterialEdits`
  (MaterialEdits.h:16-66).
- Koikatsu shader import: `shaderFor`/`shaderByName` + `materialFor`
  (Assembler.cpp:631-700, 821-1041); DXBC stored in `SceneShader::blob`,
  parsed by `shader/UnityShader.*`, executed by D3D11 (GPU) and the SM4
  interpreter (CPU).
- Material property extraction: colours/floats/textures/texST → `props`/
  `texProps`; run-time (card) overrides; shader defaults; derived
  shadow/specular/outline; main-texture selection + colour-mask bake;
  MaterialEditor override path.
- Texture import/storage: `DecodeTexture` formats (Alpha8, RGB24, RGBA32,
  ARGB32, DXT1, DXT5, BGRA32) + BC7 (`third_party/bc7decomp`); `SceneTexture`
  (size, `srgb`, `clamp`, source, hash); shader maps ≤1024 px.
- Mesh→material: `SceneSubmesh::material` index only; shared instances by
  construction; no per-vertex ids.
- Viewport rendering: `GpuView::Load/Render` (constant buffers rebuilt once
  per material per frame, GpuView.cpp:1419-1435) and software `RenderView`
  (`FillConstants`, Preview.cpp:385-426). Conclusion: colour/param edits are
  same-frame updates; texture *replacement* needs one new upload call.
- GPU skinning (`MakeSkinCompute`, `SetSkin:672-776`) and CPU fallback
  (`ViewState` mips) are untouched by material edits.
- Shader management: DXBC forwarded passes + runtime HLSL layers
  (`ShaderLayer.h`); ADR-003 selects a new fallback shader for Workbench
  modes.
- PMX export: `MmdMaterialLook` (Exporter.cpp:161-225) + `ExportScene`
  (408-781) + `BakeShaderTextures` (Preview.h:65-76). PMX carries diffuse/
  alpha, ambient, specular, draw flags, edge, one texture, sphere, toon.
  Everything else is manifest-only + warning.
- UI: `viewer/main.cpp` (4028 lines) + `viewer/Ui.*`, 6 tabs (292-293),
  `PanelLayout` (308-363), tweak-list slider machinery, pose undo/redo
  (433-436). No colour picker exists; HSV/HEX must be drawn from primitives.
- Project serialization: no project file; `viewer.json` (settings),
  `PoseFile` JSON (Pose.h:84-95), export manifest (Exporter.cpp:306-406).
  New `cardbridge-materials` v1 JSON proposed.
- Test harness: `tests/cardbridge_tests.cpp` (3082 lines), `CHECK`/`test`,
  unit tests need no files, integration gated by
  `CARDBRIDGE_TEST_LIBRARY/_MODS/_CARD/_GOLDEN/_OUT`.

**Baseline (prebuilt `build\bin`, no source changed):**

```
1659 checks, 0 failed, 1 integration suite(s) skipped
```

**Build note.** This machine has no C++/cmake toolchain on `PATH`; the
portable toolchain is in `D:\Project K\tools\` (`cmake\bin`,
`ninja`, `w64devkit\bin`, MinGW GCC 16.2). Build with those prepended:

```powershell
$env:Path = "D:\Project K\tools\cmake\bin;D:\Project K\tools\ninja;D:\Project K\tools\w64devkit\bin;" + $env:Path
.\build.ps1 -Test
```

All later stages use this invocation.

**Open questions for later stages:**

1. Module name: `src/material/` (ADR-006) vs `src/workbench/`. Decide at
   Stage 1 and keep it — either is fine, consistency is what matters.
2. Whether HSV needs a dedicated widget or three gradient rects suffice
   (Stage 2, depends on how the primitive draw helpers behave).
3. Whether split before/after on GPU needs scissor rects or a second
   `GpuView` instance (Stage 5 — software first).
4. Whether texture `wrap`/`filter` editing is worth exposing beyond the
   existing `srgb`/`clamp` flags (Stage 3).

---

## Stage 1 — Material foundation
Status: **done** (2026-11-09).

**Files changed:**
- New `src/material/MaterialWorkbench.h` + `src/material/MaterialWorkbench.cpp`
  (UI-free, ADR-006): `MaterialClass`, `MaterialOverride` (sparse),
  `WorkbenchState` (originals + overrides + 200-deep undo/redo),
  `CaptureOriginals`/`ApplyOverrides` (restore-from-original then apply only
  overridden fields), reset (parameter/material/all), copy/paste/multi-apply,
  `PresetForClass` (11 classes, colour-preserving), classification
  heuristic (exporter-trusted substrings; Metal/Plastic/Leather/Rubber are
  user-set only), texture helpers (`SetTextureEnabled`, `RebindTexture`,
  `ReplaceTexture` with FNV-1a hash), `UnsupportedFields`/`ReportUnsupported`,
  and the `cardbridge-materials` v1 JSON (`MaterialFileToJson`/`FromJson`,
  `MakeMaterialFile`, `ApplyMaterialFile` keyed by part + material name,
  tolerant reader, unknown materials warned not fatal).
- `src/export/Exporter.h` / `.cpp`: `ExportOptions::materialFile` +
  `materialWorkbench` (JSON object); `BuildManifest` emits a
  `material_workbench` section when present (file name + overrides).
- `cli/main.cpp`: `--materials <file.json>` — captured after assembly,
  applied after the pose bake and before `ApplyPartStateForExport` (so
  `BakeShaderTextures` sees edited colours), unknown entries logged, sha
  mismatch warned-not-enforced, provenance passed to the exporter.
- `CMakeLists.txt`: `src/material/MaterialWorkbench.cpp` added to
  `cardbridge_core`. No new dependency.
- `tests/cardbridge_tests.cpp`: 10 new test blocks (see below).
- No viewer change, by design.

**Mapping notes (docs/MATERIAL_EDITOR.md §3.4):**
- Base colour → `diffuse` + `_Color` (when present); opacity → `diffuse.w` +
  `_Color.w`; specular → `_SpecularColor` + scaled derived gloss; roughness →
  `_SpecularPower` inverse (`strength = 1 - roughness`, 0..1, the same clamp
  the assembler uses); metallic → best-effort `_Metallic` write (preview-only,
  PMX unsupported).
- Preview-only (PMX cannot carry them): metallic, roughness, emission
  (written to `_EmissionColor` when the shader reads it), normal intensity,
  transmission, subsurface, anisotropic highlight, classification. Each set
  field produces a named `scene.warn` via `ReportUnsupported` (lands in the
  manifest issues) — never silently dropped.
- Texture enable/disable writes `texProps[slot].texture = -1`; rebind binds an
  existing scene texture by index or by name (JSON); replace appends a new
  `SceneTexture` with FNV-1a hash. File-based texture replacement (user
  picker) is Stage 3; the binding plumbing is done.

**Test results:**
```
1772 checks, 0 failed, 1 integration suite(s) skipped   (baseline was 1659)
```
New checks (all passing, no game files needed):
- classification table for skin/hair/main_item/main_opaque/main_alpha/toon_eye/
  toon_nose/glasses/mnpb + Generic fallback + `ClassFromName`
- every preset sets shading and leaves colour alone; Fabric carries
  transmission, Skin carries subsurface
- base colour / opacity write diffuse + `_Color` only
- roughness → `_SpecularPower`, specular → colour + derived gloss
- reset restores the original bit-for-bit; param reset keeps siblings
- JSON round-trip (incl. `card_sha256`, part/material key), version/format
  rejection with reasons, unknown-field tolerance, unknown material warned
- undo/redo one step per edit, redo dies on a new edit
- texture enable/disable, rebind, replace, missing warned-not-crashed
- shared instances (2 submeshes edited together), part lookup, copy/paste,
  multi-apply, deterministic `MaterialIdColor`
- PMX: `MmdMaterialLook` carries the edited diffuse/alpha; real
  `ExportScene` writes a PMX the independent reader can read and a manifest
  whose `material_workbench` names the file and the material; unsupported
  fields reported via `scene.issues`.

**Remaining limitations:** no viewer UI (Stage 2), no texture file picker or
GPU texture upload API (Stage 3), no diagnostic modes (Stage 4), no
transmission/skin preview (Stage 5), PMX `memo` classification and the
`--no-bake`/`--no-toon-ramp` interactions are Stage 7. The gated
card→PMX integration suite still needs `CARDBRIDGE_TEST_LIBRARY/_MODS/_CARD`
to exercise `--materials` against a real card; the synthetic export test
covers the same code path without game files.

Exit criteria met: fixture JSON changes exported PMX colours/alpha, the
manifest says so, full suite green, no viewer change.

---

## Stage 2 — Material Inspector (viewer)
Status: **done, corrected after the first live check** (2026-11-09).

**Files changed:**
- `viewer/main.cpp`:
  - New `TabMaterials` (“Mats” truncated tab label) between Bones and Setup;
    `kTabNames`/`TabCount` and panel tabs updated (7 tabs). The pitch was
    narrowed from 51 to 44 px (42 px pill, 2 px gap) after the first live check:
    with 51 px the last tab ended at x 365 and the right-hand status line
    ("0 edited") was drawn over it.
  - `App` gains `material::WorkbenchState mat`, `selMaterial`/`matFilter`/
    `matScroll`/`matPos`, 9 slider buffers, and HEX entry state.
  - `CapturesOriginals` on every `WM_LOADED` (fresh character) and resets
    selection/filter/scroll/input; export snapshots `mat` + `card.sha256`
    for the manifest.
  - `LayoutFor(H, boneTab, editRows)` + `Lay()` give Materials its own 13-row
    editor (filter row + 5-row list + inspector). List reuses
    `matListTop/Bottom`; editor reuses an offset 13-row block
    (`matEditTop = boneEditTop`). Filter/class/preset/texture/reset/undo hit
    rects added to `PanelLayout`.
  - `SliderFor` extended to `kMatBase = 5000` (R,G,B,opacity,roughness,
    metallic,specular,emission,normal). `TweakChanged`/`AnyTweak` show the
    amber “changed” marker; `IsLiveId` includes material ids so they update
    live without reassembly; `TweakEdited` routes material ids to
    `MaterialSliderEdited` (write `SceneMaterial` via `ApplyOverrides`, mark
    dirty, invalidate as-exported bake).
  - `DrawMaterialsTab` mirroring `DrawBonesTab`: filter row (“Filter: All /
    class name”), material rows (index + name + `[Class]`, `xN` shared badge,
    amber when edited, selection bar), header (display name + class), class /
    preset / MainTex enable row, R/G/B sliders, HEX swatch + text, 6 shading
    sliders, reset/reset-all/undo/redo row.
  - `MaterialsTabClick`: filter cycles (All → 11 classes), list selects
    (syncs buffers), class cycles, preset applies `PresetForClass`
    (colour-preserving), MainTex enable/disable via `SetTextureEnabled`,
    sliders drag-start pushes one undo step then live-applies, HEX click
    starts keyboard entry, texture row toggles, resets and undo/redo buttons.
  - `SyncMatBuf()` syncs slider buffers from `EffectiveOf` (unless that
    slider is being dragged) so thumb position and “where the card had it”
    marker follow edits, resets and undo.
  - HEX: click the HEX row to edit; type 6 hex digits (live-apply, one undo
    step per entry), Backspace, Enter/Escape to end. Editor captures
    `CARDBRIDGE_MATERIALS_PATH` for scripted save/load.
  - `Render()` highlights the part that owns the selected material
    (`hoverPart` equivalent) so selection is visible in the viewport on both
    GPU and software paths.
  - `Frame()` eases `matPos`; `InvalidateSliderRow()` knows material rows;
    mouse-move hover over the material list; wheel scrolls the material list;
    `WM_SETCURSOR` hand over the Materials editor; `Ctrl+Z/Y/S/O` on the
    Materials tab are material undo/redo/save/load (other tabs keep pose
    bindings); `SaveMatFile`/`LoadMatFile` mirror the pose file flow.
  - `StartExport()` snapshots `mat`, calls `ReportUnsupported` on the export
    copy (unsupported fields appear as manifest issues) and fills
    `eo.materialFile`/`eo.materialWorkbench` from `MakeMaterialFile` so the
    `<name>.cardbridge.json` carries a `material_workbench` section.
  - `DumpTestLayout()` exposes `materials` (list rows/y, `fieldY[9]`, `hexY`,
    `classY`, `resetY`, `filterY`, `sel`, `overrides`, `undo`/`redo`) and
    `state.{selMaterial,matUndo,matRedo,matOverrides}` for the smoke script.
    `WM_TEST` 13/14/15/16 are material undo/redo/save/load.
- Core additions for the tint path (correction below):
  `src/material/MaterialWorkbench.{h,cpp}` (`MakeTintedTexture`,
  `PixelIgnoresColor`, tint caches, `changedTextures`, `CaptureOriginals`
  resets), `src/export/GpuView.{h,cpp}` (`UpdateTextures`),
  `src/export/Preview.{h,cpp}` (`InvalidateViewTexture`).
- `tools/viewer_smoke.ps1`: Materials step (tab select, list visible, editor
  rows, select material, drag Red slider, picture change, single undo step,
  undo restores) + Setup index moved to 6.
- No core changes in the first pass (Stage 1 already green): `1772 checks,
  0 failed`. The correction below added the tint path and two tests.

**Behaviour notes:**
- RGB sliders (0–255) are the primary colour control; HEX is display + live
  typed entry. HSV sliders were not added (sliders + HEX already satisfy
  the stage’s “where practical” and reuse `DrawSliderRow` exactly; HSV is
  deferred as a number-for-number complement).
- Right-click on a material slider resets that slider to the original
  (pushes one undo step).

**Test results:** build + full unit suite green (`1772 checks, 0 failed`).
Smoke script updated; full GUI walk needs a real card + library and is left
for manual validation / gated CI (same as previous stages).

Exit criteria: colour changes in real time (same-frame `ApplyOverrides`),
undo/redo per gesture + Ctrl bindings, resets restore originals, other tabs
untouched (only tab enum + shared `AnyTweak` inclusion of `mat.IsDirty()`).

### Stage 2 correction — the first live check found the colour path dead

The user reported: “all not work, and all hex is FFFFF, and all slider doesnt
do shit to any changes to card”. Reproduced and fixed:

1. **HEX FFFFF / wrong values.** `EffectiveOf` preferred `diffuse`; a real
   assembled material keeps its colour in `_Color` and leaves `diffuse` white
   for PMX. The inspector now prefers a stored `_Color`, then falls back to
   `diffuse`. `ApplyOverrides` always writes `_Color` (adding it when the
   material did not store one) so an edit cannot be invisible.
2. **RGB text showed `0` / `536870912`.** `%d` was used with a `double`
   argument. Changed to `%.0f`.
3. **The material row overlapped the shader name.** Shader name moved into
   the heading line; heading clipped to 30 chars.
4. **The tab/status overlap** described above (44 px pitch).
5. **The sliders still did nothing on body/face/outfit.** `out/mats_debug.txt`
   (temporary debug dump, removed after diagnosis) showed the forward pixel
   programs of `main_skin`, `main_opaque` and `main_alpha` never read
   `_Color`: their create jobs already painted the card colour into
   `_MainTex`, so a `_Color`/`diffuse` write cannot show.

**Base-colour routing (the actual fix).** `MaterialWorkbench.cpp` now
analyses the forward pixel program of each `SceneShader` once
(`PixelIgnoresColor`, parsing the DXBC cbuffer operands; cached in
`WorkbenchState::readsColor` as unknown / reads / ignores / unanalysable):

- reads `_Color` (or unanalysable): the cheap same-frame `props` write, as
  before;
- provably ignores `_Color` and has a main texture: append one tinted copy of
  `_MainTex` per edited material (`name + "~wb"`), made by
  `MakeTintedTexture` (multiplies RGBA by the requested/original ratio,
  clamps, keeps alpha, recomputes `pixelHash`), bind it as `_MainTex` and
  write white `diffuse` so the tint is not applied twice. The copy is cached
  (`tintTextures`, `tintCache`: source index, ratio, source hash) and only
  rebuilt when the ratio or the source pixels change. Changed indices are
  reported in `WorkbenchState::changedTextures`.

**Consumers of `changedTextures`.** `GpuView::UpdateTextures` re-uploads only
the named textures (no `Load`, no shader/mesh work), and
`exporter::InvalidateViewTexture` drops the software renderer's cached mip
chain. The viewer's `SyncMaterialTextures()` calls both after every
`ApplyOverrides` / material-file load / texture toggle / reset path. Export
and the as-exported bake copy the live scene, so they see the tinted texture;
`BakeShaderTextures` then carries it into the PMX and white PMX diffuse, as
before.

**Tests.** Two new unit blocks: `MakeTintedTexture` (known pixels, channel
ratio, alpha, clamp, fresh pixels/hash, identity, malformed input) and an
unanalysable shader staying on the `_Color` path with no tint copy.

**Results:**
```
1788 checks, 0 failed, 1 integration suite(s) skipped
viewer_smoke.ps1 -Only materials: 12 checks, 0 failed
  materials: the picture changed 2.50% of the view
  materials: undo restores the picture (view diff 0.000%)
```
The smoke screenshot confirms the bold Red drag (250 → 8) turned the body's
skin teal through the tint copy, while the outfit was untouched, and undo
restored the original skin exactly. The remaining visual confirmation has to
happen on the user's own card/viewer.

**Limitations of the correction.**
- Multiplicative ratio: a black original channel cannot be brightened (the
  ratio is kept at 1 for a zero source); hue shifts are approximate, not a
  colour-space-correct relight.
- One extra scene texture per tint-edited material. It is reused, not
  reallocated per frame, and only re-uploaded when its pixels change.
- Opacity on `main_skin` / `main_opaque` may be a no-op or cutout-only: those
  programs do not alpha-blend the main texture.
- A shader whose bytecode cannot be parsed stays on the `_Color` path; if it
  also ignores `_Color` in practice, the colour edit still cannot show. It
  does not crash or corrupt the scene.
- The tint copy is an override-layer artefact; it is regenerated from the
  card when a material file is re-applied, never written into the card.

### Stage 2 addendum — class dropdown + colour picker (2026-11-09)

The click-cycling Class (and Filter) buttons were replaced with direct-pick
dropdowns: clicking `Class: … v` / `Filter: … v` opens a modal list of all
11 classes (plus All for the filter); picking a row applies it in one click.
The base colour got a visual picker next to the HEX entry: a `Color` button
toggles an HSV popup (saturation/value square + hue bar + preview + Done),
dragging updates the base colour live through `ApplyOverrides` with one undo
step per session. HSV math lives in `src/material` (`RgbToHsv`/`HsvToRgb`,
unit tested); the viewer only draws. The smoke script now verifies the
dropdown opens/picks Metal in one click and the picker drag changes the picture.

---

## Stage 3 — Texture and UV tools
Status: **done** (2026-11-09, branch `feature/material-workbench`).

**Core (UI-free):** `SceneTexture::format` (`src/assemble/Scene.h`) recorded
from the Unity format at assembly (`unity::FormatName`, `Assembler`
`textureFor`/`shaderTexture`, `RGBA8` default for baked/replaced copies,
`RGBA8 (tint copy)` for tint copies). `GpuView::EnsureTexture`/`UploadTexture`
wrap `UpdateTextures` (Stage 2) for the texture-replace path. New module
`src/material/UvDiagnostics.*`: missing / out-of-range (margin 0.02) /
stretch (texel density + edge ratio) / islands + bbox overlap / seams
(boundary edges), plus `UvTrisForMaterial` and `DiagnoseUv` aggregator.

**Viewer (Materials tab, new `Texture` view toggled from the HEX row):**
channel (All/R/G/B/A) + slot dropdown (defaults to `_MainTex`), preview with
checkerboard + per-pixel channel filtering, UV canvas (texture background +
wireframe, out-of-range red, seams amber, drag = pan, wheel = zoom, click =
pick island, `Fit UV`), one-line info (name, size, format, sRGB/clamp,
hash) + diagnostics line (counts + stretch range), Enable checkbox,
Replace (file dialog → `DecodeImageBytes` → `ReplaceTexture` → `EnsureTexture`),
Fit UV. `DumpTestLayout` exposes inspector/UV state; the smoke script checks
open → format recorded → UV drag → close.

**Tests:** 2 new blocks (format names; five diagnostics on synthetic meshes
incl. closed-tetra seams, island overlap, stretch contrast, per-material tris).
Full suite + Materials smoke green.

**Limitations:** slot dropdown shows 6 rows max (no scroll); UV canvas draws
the first 2000 triangles of the material (note in-canvas) and caches
diagnostics per material/mesh (recomputed on reselection); islands highlight
by bounding-box pick; filtering/wrap stay read-only (flag + re-upload needs
sampler-state work in both renderers, as planned); UV edit stays read-only
(No editing by design). Replaced textures ride the existing tint path, so
an active base-colour edit still tints the replacement.

---

## Stage 3.5 — The Material Workbench gets its own window
Status: **done** (2026-11-10, branch `feature/material-workbench`).

Requested after Stage 3: the Materials tab should open a *dedicated window*
("as big as the main viewer") instead of editing inside the 380 px panel.
ADR-010 records the decision; this is the record of the change.

**Files changed:**
- New `viewer/Workbench.inc` (the whole window):
  - `WbButtonId`/`WbButtons`/`WbButtonAt`/`WbButtonRect` — one button list the
    draw code, the click code, the hover repaint and the cursor all read.
  - `WbNow()`/`WbLayoutFor` — one layout for a given client size.
    `InvalidateWbBand`/`InvalidateWbList` repaint a band or the list card.
  - `RevealSelectedMaterial()` — scrolls *both* lists to the selection.
  - `DrawMatPicker` (HSV popup), `DrawUvCanvas`, `DrawWorkbenchEditor`,
    `DrawWorkbench` (header, list card, editor card, popups last).
  - `WbPaint` (update-region split like `Paint`; a new/resized surface
    redraws everything), `WbClick` (modal popups first, then filter, list,
    rows, sliders with `g.dragTrackL/R`, inspector, reset/undo/save/load),
    `WbPickUvIsland`.
  - `MatWndProc`: paint, size, min size 920×600, `WM_TEST` (10/13–16),
    `WM_CLOSE` → hide, input (incl. HEX keys, Esc, Ctrl+Z/Y/S/O),
    `WM_DESTROY` → **no** `PostQuitMessage`.
  - `OpenWorkbench()` — registers the class (also guarded in `wWinMain`),
    mirrors the viewer rect to its right clamped to the work area, shows or
    restores, reveals the selection, focuses.
  - `DumpWorkbenchLayout()` — the `workbench` section of the layout dump.
- `viewer/MaterialsDraw.inc` rewritten as the launcher: `DrawMatList`
  (shared list renderer: rows, selection bar, edited tint, scroll bar) +
  `DrawMaterialsTab` (filter + count, **Open Material Workbench** button
  (hover id 20), list, filter dropdown last).
- `viewer/Clicks.inc`: `MaterialsTabClick` slimmed from ~220 lines to the
  filter dropdown + hex-end + list select (the editor moved to `WbClick`).
- `viewer/State.inc`: `PanelLayout` keeps only `matFilter`/`matOpen`/
  `matListTop`/`matListBottom`; `App` gains `matHwnd`, `hinst`, `wb`
  (second surface), `wbHoverRow`, `wbHoverBtn`, `wbMatScroll/wbMatPos`,
  `dragTrackL/R`; new `WbLayout` + `WbLayoutFor`.
- `viewer/Window.inc`: material slider invalidation goes to the workbench
  band; `ApplyPending` uses the drag's own track; `Frame` eases the
  workbench list scroll; `DumpTestLayout` writes a launcher `materials`
  section + `workbench`; button 20 opens the workbench; hover/picker/UV
  handling left the main `WndProc`; HEX/Esc now `InvalidateMat()`;
  `WM_DESTROY` destroys the workbench first, then quits.
- `viewer/MaterialsHelpers.inc`: popup geometry from `WbLayout`
  (`MatClassDropRect`, `MatFilterDropRect(x,y,w)`, `MatTexDropRect`,
  `MatPicker*`), shared `DrawMatFilterDrop`, `InvalidateMat()` (both
  windows), dialog owners `g.matHwnd ? g.matHwnd : g.hwnd`.
- `viewer/Parts.inc`: `SliderValueAt(r, x, trackL, trackR)`; `ButtonAt`
  returns 20 for `l.matOpen` on the Materials tab (checked before the
  `button >= 10` tab branch).
- `viewer/Widgets.inc`: `DrawSliderRow` takes track ends + value right +
  label x; `ScrollBar` takes a right edge.
- `viewer/Progress.inc`: `InvalidateWorkbench`/`InvalidateMat`;
  `InvalidateAll` covers both windows; fwd decl `InvalidateSliderRow`.
- `viewer/main.cpp`: includes `Workbench.inc`, registers the class,
  `g.hinst`, `g.wb.Reset()` at shutdown.
- `tools/viewer_smoke.ps1`: `$script:target` (Click/Drag/Hook/Shot aim at
  the viewer or the workbench; `Shot` takes an optional hwnd so view diffs
  still shoot the viewer), `V.ByClass` (EnumWindows — PowerShell marshals a
  `$null` string arg as `""`, so `FindWindowW(cls, $null)` never matches),
  the materials step retargets to the workbench: open → find by class →
  `workbench.*` coords → drag/undo/picker/inspector/UV → hide via `WM_CLOSE`
  → reopen from the launcher → target back to the viewer. Plus an `apose`
  check fix (see below).

**Decisions:**
- Same thread and message loop as the viewer (ADR-010): no second state, no
  lock on `g.mu`, no deadlock risk with the export path.
- `WM_CLOSE` hides instead of destroying: scroll, selection and the drawn
  surface survive; only the viewer's `WM_DESTROY` quits the process.
- Shared: selection, edits, filter, class/picker/inspect flags.
  Per-window: hover, list scroll, slider drag track.
- Popup geometry single-sourced from `WbLayout` so draw and click agree.

**Test results:**
```
1826 checks, 0 failed, 1 integration suite(s) skipped
viewer_smoke.ps1 -Only materials,export: 35 checks, 0 failed
viewer_smoke.ps1 (full):                          69 checks, 0 failed
```
The materials step now covers the launcher (list, filter, button), the
window opening at 1264×781 (as big as the viewer), the workbench's own list
selecting, a Red drag changing 2.50% of the view in one undo step, undo
restoring it exactly, the class dropdown picking Metal in one click, the
picker drag changing 2.55%, the inspector's format/UV drag, save/load row
present, `WM_CLOSE` hiding and the launcher reopening it (selection kept).

**Two smoke fixes along the way (not regressions):**
- `apose: the switch is on` failed because commit `9a160c1` made A-pose
  default *on*, so one `P` press turns it off. The check now asserts the
  switch *toggled* (it still requires the picture to change).
- `FindWindowW(cls, $null)` never matched (PowerShell passes `""`), so the
  workbench was "not found" even when open; `V.ByClass` uses `EnumWindows`.

**Limitations:** the filter dropdown is one shared flag for two windows
(modal, only one list clickable at a time — documented in ADR-010); the
workbench is one window per process (no multiple workbenches); the window is
not persisted (position/size are re-derived from the viewer on first open);
the panel keeps no editor at all, so with the workbench closed the panel
shows only the list (by design — the button reopens it).

### Workbench-window fixes found on first live look (2026-11-10)

Two defects surfaced when the user opened the Texture inspector:

1. **The UV canvas's texture was vertically flipped against its own
   wireframe** ("the mapping is offside"). The background sampler used
   `ty = v * h` after a `1 - v` flip; the viewer's `SampleBilinear` maps
   Unity `v=1` to texture row 0 (`fy = (1 - v) * h`), so the background now
   uses `(1 - vv) * tex->height` — the same picture the 3D view shows, and
   the same frame as `toPx`, so zoom/pan can never drift the two apart.
2. **`stretch 48467.3-166219488.0`** — texel density is unit-dependent
   (`uv area × texels² / 3D area` explodes on near-degenerate 3D triangles).
   Triangles with `dA <= 1e-9` are now skipped and min/max are reported as
   multiples of the mesh mean (`meanDensity == 1`; `stretch 0.02-53.72x of
   mean` on the demo card). Unit tests still green (inequality checks).

Verified live (screenshot + smoke); the inspector still opens/closes and
exports fine.

### Workbench performance pass (2026-11-10)

The inspector lagged on every pan/zoom drag of the UV canvas and on picker
drags over the preview. Two causes, both fixed:

1. **Per-destination-pixel drawing.** The UV background and the preview
   thumbnail each drew every 2×2 block with its own GDI `ExtTextOut` call —
   ~130k calls per repaint, and repaints happen on *every* mouse-move frame
   of a drag. New `ui::Surface::BlitWrappedTexture(...)` bakes the mapping
   once into a persistent scratch DIB and blits it in one call (channel
   filter and checkerboard/solid base preserved). ~5× fewer pixels touched
   than 2×2 cells, one blit instead of a hundred thousand Fill calls.
2. **`Gdiplus::Pen` created per `Line()`** — every seam and every wireframe
   edge built a Pen per frame. The Surface keeps a one-entry pen cache.

Also: the draw-triangle list (`UvTrisForMaterial`) is cached per selection
(cleared at the five selection-change sites) instead of being rebuilt on
every canvas paint, and the island-pick mask is now indexed exactly like
that cached list — previously the sel flag read a mesh-flattened ordinal,
so the highlight could land on the wrong island.

Build + unit tests green (1826 checks). Live feel check pending the game
(Deadlock) being closed; report whether the pan/zoom is smooth now.

---
Status: **not started.**

- [ ] `ViewMode` enum (11 modes) in software renderer.
- [ ] GPU fallback shader for the same modes (ADR-003).
- [ ] Material ID (deterministic colour, legend, isolate via `hiddenParts`).
- [ ] Mode selector strip.

---

## Stage 5 — Advanced preview
Status: **not started.**

- [ ] Studio light presets + one-light controls + turntable.
- [ ] Fabric transmission + skin wrap (ADR-004), both renderers.
- [ ] Before/after toggle, then split (software first).
- [ ] Transmission thin-vs-thick unit test.

---

## Stage 6 — Comparison and usability
Status: **not started.**

- [ ] Split before/after on GPU; isolate/hide-others; rename/duplicate;
      copy/paste; apply-to-multiple; undo/redo polish; search.

---

## Stage 7 — Export integration and hardening
Status: **not started.**

- [ ] Full PMX mapping (§3.4) + `memo` classification + manifest warnings.
- [ ] Persistence round-trip (viewer save → reopen; CLI `--materials`).
- [ ] Full regression: unit + gated integration + PMX cross-check;
      `viewer_smoke.ps1` full pass (base + modded, GPU + CPU).
- [ ] Performance review (no recompile / rebuild / re-upload on param edits).
- [ ] Final report (spec §24): implemented / partial / approximations /
      unsupported / modified files / dependencies / tests / manual
      validation / performance / limitations. README "Known limitations"
      updated.
