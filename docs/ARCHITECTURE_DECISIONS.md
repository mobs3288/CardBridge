# CardBridge — Architecture Decisions (Material Workbench)

Decisions that would be expensive to reverse. Rejected alternatives are kept
so the next person does not re-litigate them. Each entry is dated and refers
to the `feature/material-workbench` branch.

---

## ADR-001 — Workbench is an override layer over `SceneMaterial`, not a new material type

**Date:** Stage 0 (investigation).
**Status:** proposed — implementation in Stage 1.

**Context.** Three material types already exist (§2.1 of
`MATERIAL_EDITOR.md`): raw `unity::MaterialData`, imported
`assemble::SceneMaterial`, and card-side `card::MaterialEdits`. Both renderers
and the exporter read `SceneMaterial` and nothing else.

**Decision.** Add a fourth concept — *interpreted* + *override* — that lives
beside `SceneMaterial` and writes into it through one function
(`ApplyOverrides`). The scene tuple `(Scene, SceneMaterial, SceneTexture,
SceneShader)` is never restructured.

**Rejected alternatives.**

- *Add fields to `SceneMaterial` directly* (e.g. `roughness`, `transmission`).
  Rejected: it pollutes the tiny shared scene struct with workbench-only
  concepts, forces every cache (`bakedCache`, shader caches) to reason about
  them, and makes "reset to original" impossible without a second copy.
- *Replace `SceneMaterial` with a Workbench-owned struct.* Rejected: the
  exporter, both renderers, and the assembler all construct it; a swap is a
  rewrite with regression risk far beyond the feature.

**Consequences.** Originals are a snapshot taken once at load; every edit is
a sparse map write. Reset = erase. Undo = delta stack. The interpretive
layer can evolve without touching the scene.

---

## ADR-002 — Live updates are same-frame `props` writes, not reloads

**Date:** Stage 0.
**Status:** accepted — verified in Stage 1/2; amended by ADR-009 for base
colour on shaders that ignore `_Color`.

**Context.** The spec requires material updates without rebuilding geometry,
re-importing the character, re-uploading unchanged textures, or recompiling
shaders.

**Decision.** `GpuView::Render` already rebuilds per-material constant
buffers *once per frame* from `mat.props` (`GpuView.cpp:1419-1435`), and the
software renderer does the same on the CPU (`FillConstants`,
`Preview.cpp:385-426`). Editing `props` / `diffuse` / `shadow` /
`specular` / `doubleSided` / `alpha` therefore becomes visible on the next
frame with **no resource lifecycle changes**.

Texture *re-assignment* to an existing scene texture is the same. Texture
*replacement* with new pixels is the only case needing new plumbing: one
`EnsureTexture`/`UploadTexture` call (GPU) + one `ViewState` mip
invalidation (CPU), reusing `MakeTexture` / `BuildMips`.

**Consequences.** No geometry rebuild, no shader recompile, no texture
re-upload for parameter edits. The invariant is testable: a parameter-only
edit must not call `Load`, `Unload`, or `CreateVertexShader` (assert via
`CARDBRIDGE_DEBUG_*` hooks in Stage 7).

---

## ADR-003 — Preview stays the game's shader; Workbench shading is a fallback path

**Date:** Stage 0.
**Status:** proposed — implementation in Stages 4-5.

**Context.** CardBridge's core promise is "the viewer shows what the game's
shader draws" (`Preview.h`, README). The game's Shader Forge shaders do not
know metallic/roughness/transmission/subsurface.

**Decision.** **Lit mode always runs the game's shader** with the original
(or colour-overridden) props. Workbench-only shading (diagnostic modes,
transmission, skin wrap) runs in a separate *fallback shader* — runtime
HLSL in the style of `fbVs/fbPs` (`GpuView.cpp:221-286`) compiled via the
existing `d3dcompiler_47.dll` path — that receives the interpreted params
through its own per-material cbuffer. The game's shader path is never
modified.

**Rejected alternative.** ShaderLayers (`src/export/ShaderLayer.h`) for
transmission: they blend *over* the game shader output rather than replacing
it, cannot do two-sided backface tests in the material's own quad, and are
not exported — right tool for "rim light", wrong tool for "diagnostic
transmission mode".

**Consequences.** Two things to maintain: the game-shader path (untouched)
and the fallback path (new). Documented in-UI: "Lit = game's shader.
Other modes = Workbench approximation."

---

## ADR-004 — Fabric transmission and skin shading are wrap-light approximations

**Date:** Stage 0.
**Status:** proposed — implementation in Stage 5.

**Context.** PMX has no transmission, no subsurface, no thickness. Physically
correct thin-film/sub-surface rendering would need a new renderer.

**Decision.** Implement both as a normal-dependent wrap-light term in the
fallback shader:

```
wrap(x) = clamp((x + t) / (1 + t)),  t = 0.5 * (1 - strength)
transmission = transColor * strength * wrap(dot(N, L_back)) * exp(-thickness)
skin         = sssColor * strength * wrap(dot(N, L))  (front or back light)
```

Two-sided when the material's `doubleSided` allows it (flip N for back
faces). This is *not* alpha transparency: opacity is unchanged.

**Documented approximation.** No refraction, no volume, no multiple scattering.
Front light hides the effect; backlight shows it. Thin fabric transmits,
thick does not; dark fabric keeps its albedo and transmits less. That is the
spec's expected result set (§8/§9) with a one-term shader.

---

## ADR-005 — Material persistence is a sibling JSON, never a card edit

**Date:** Stage 0.
**Status:** proposed — implementation in Stage 1.

**Context.** No project file exists; the card `.png` must never be written
(spec §18, README "What it deliberately does not do").

**Decision.** New versioned file `cardbridge-materials` v1 (shape in
`MATERIAL_EDITOR.md` §3.5), keyed by `(part, material name)`, sparse. The
viewer saves/loads it (buttons next to the pose ones, `viewer/main.cpp`:
`boneSave`/`boneLoad`). The CLI accepts `convert --materials <file>`.
The exporter records `material_workbench` provenance in the manifest and
warns (via `scene.warn`) for every unsupported field.

**Rejected alternative.** Embedding overrides in the manifest JSON alone:
the manifest describes an export that already happened; it cannot be re-applied
to a new assembly. A standalone file can.

---

## ADR-006 — New module `src/material/`, UI-free like `assemble::Pose`

**Date:** Stage 0.
**Status:** proposed — implementation in Stage 1.

**Context.** `cardbridge_core` is deliberately UI-free/renderer-free
(`CMakeLists.txt:58-60`, "plan section 16.1"). `assemble::Pose` is the
template: pure logic + tiny JSON, used by viewer, CLI and tests.

**Decision.** All Workbench logic (originals, overrides, classification,
presets, JSON, PMX mapping, UV diagnostics) lives in `src/material/` and
depends only on `assemble/Scene.h` + `core/` + nlohmann/json. `viewer/` and
`cli/` are shells over it. The same module is unit-tested without a window,
without a device, without game files.

**Consequences.** `CMakeLists.txt` adds `src/material/*.cpp` to
`cardbridge_core`. No new third-party dependency (json and stb_image are
already fetched at build time).

---

## ADR-007 — UV inspection is read-only; diagnostics are pure CPU functions

**Date:** Stage 0.
**Status:** proposed — implementation in Stage 3.

**Context.** Spec §10: "Initially, UV inspection is sufficient. Do not
implement full UV editing unless the existing architecture makes it
straightforward and the core functionality is complete."

**Decision.** Stage 3 ships a read-only canvas + five diagnostics (missing,
out-of-range, stretch, overlap, seams) as pure functions over `SceneMesh`.
Object stays: opening the UV editor never mutates `SceneVertex::uv`.
UV *editing* would need a vertex re-upload path (`Load`/`Refresh`) that does
not exist for UVs — deferred indefinitely.

**Consequences.** All five diagnostics are unit-testable on synthetic meshes
(same pattern as `HasCutout`/`HasInnerLayer` tests).

---

## ADR-008 — Diagnostic modes are a `ViewOptions::mode`, not new renderers

**Date:** Stage 0.
**Status:** accepted — implemented in Stage 4 (2026-11-10).

**Context.** `ViewOptions` (`Preview.h:53-63`) already carries `textured`,
`shaded`, `step`, `highlightPart`, `skin`. The software raster loop
(`Preview.cpp:644-800`) is per-pixel and already branches on those flags.

**Decision.** One enum `ViewMode` with the spec's 11 modes plus `SkinAO`
(2026-11-12, added to check the baked Koikatsu AO on the body skin); the
software path branches per pixel; the GPU path uses the ADR-003 fallback shader. The
viewport gets a mode strip chosen GDI-side (bottom of the 3D view, the
  same DC the skeleton overlay uses) and the viewport shows a MaterialID
legend (drawn only in MaterialID mode) for Material ID.

**Stage 4 implementation notes.** The enum lives in `src/export/ViewMode.h`
with a stable `ViewModeName` token persisted as `viewMode` in `viewer.json`
(`"lit"` default). The interpreted per-material params come through
`ViewOptions::diagnostics` / `GpuView::SetDiagnosticMode`, filled from
`MaterialWorkbench::EffectiveMaterial` every frame — the diagnostic modes
never re-derive or duplicate interpreted state. Lit never enters the
diagnostic shader on the GPU path; Lit's output is unchanged.

**Consequences.** No new window, no new renderer, no stored-property
mutation: diagnostic modes never write back to the scene. GPU wireframe is
line-fill (overdraw) rather than per-pixel — documented. The one open
point is verification: unit tests cover the software path deterministically;
the GPU path has not been seen on screen yet (Deadlock running at session
end).

---

## ADR-009 — Base colour falls back to a tinted `_MainTex` copy when the shader ignores `_Color`

**Date:** Stage 2 correction (2026-11-09).
**Status:** accepted — implemented and covered by unit + smoke tests.

**Context.** The first live check showed the Workbench colour sliders doing
nothing on body, face and outfit. The inspectable scene had `_Color` props
for some materials but not for the create-job results; a bytecode dump
(`out/mats_debug.txt`) proved that the forward pixel programs of
`main_skin`, `main_opaque` and `main_alpha` never read `_Color` at all — the
card's colour is baked into `_MainTex` by the create shader. ADR-002's
same-frame `props` write is therefore insufficient for the most visible
materials.

**Decision.** Keep ADR-002 as the fast path, but route base colour by what the
forward pixel program actually reads:

1. Analyse each `SceneShader` once (DXBC cbuffer operands of the selected
   forward pixel program) and cache the result: reads `_Color`, ignores it,
   or unanalysable.
2. Reads / unanalysable → write `_Color` + `diffuse` exactly as before.
3. Provably ignores → build one tinted copy of the material's main texture
   (`MakeTintedTexture`: RGBA × requested/original ratio, clamp, keep alpha,
   recompute `pixelHash`), bind it as `_MainTex`, force `diffuse` white, and
   report the copy in `WorkbenchState::changedTextures`.

`GpuView::UpdateTextures(indices)` re-uploads only the named textures and
`exporter::InvalidateViewTexture` drops only the named CPU mip chains. The
copy is cached by source texture, ratio and source pixel hash, so a slider
drag rebuilds it but a roughness change or an unrelated frame does not.

**Consequences.** Colour edits now change pixels on every material with a
main texture, on both renderers and in the PMX bake, without a scene reload
or shader recompile. Costs: one extra scene texture per tint-edited material
(allocated once, reused); a multiplicative approximation (zero channels
cannot be raised; hue shifts are not colour-managed); opacity on shaders that
do not blend the main texture remains cutout-only or a no-op. Unparseable
shaders keep the old path rather than guessing — no crash, no corruption.

**Alternatives rejected.** Rewriting the card's baked create texture in place
(breaks the "never modify the source card/assets" rule and invalidates every
material sharing it); a runtime HLSL replacement shader for the whole
character (ADR-003 reserves that for Workbench-only shading, not for live
colour edits); recolouring only in the PMX (leaves the inspector preview
lying).

---

## ADR-010 — The editor lives in a second top-level window; the panel's Materials tab is a launcher

**Date:** workbench-window change (2026-11-10).
**Status:** accepted — implemented, unit + smoke tested.

**Context.** Stage 2/3 put the whole editor (13 rows, the colour picker, the
texture inspector and its UV canvas) inside the 380 px side panel. The spec
asks for a workbench "as big as the main viewer"; squeezing a list, a
600 px-wide editor, two canvases and a picker into 380 px made every row
fight for space, and the 3D view could never be shown next to the edits.

**Decision.** One second top-level window, `CardBridgeMaterialWorkbench`,
created lazily by the panel's **Open Material Workbench** button:

- **Same thread, same message loop, same `App`.** No second render loop and
  no second state: `WbPaint` draws `g.wb` (a second `ui::Surface`) from the
  same globals the panel reads. `MatWndProc` handles paint/input only;
  `Frame`, `ApplyPending` and the export/GPU paths stay in the viewer's.
- **Geometry.** It mirrors the viewer's window rect to its right, clamped to
  the monitor's work area, minimum 920×600 (client) — so it is "as big as
  the main viewer" without ever covering it. `WbLayoutFor(W, H)` is the one
  source of layout for draw, click and the test dump.
- **Lifetime.** `WM_CLOSE` *hides* (scroll, selection and the drawn surface
  survive; the launcher shows it again). Only the viewer's `WM_DESTROY`
  ends the process: it destroys the workbench first, then posts quit. The
  workbench's own `WM_DESTROY` never posts quit.
- **Shared vs. per-window state.** Selection, edits, filter, class/picker/
  inspect flags are shared (there is one selection and one edit set, so both
  windows always agree). Hover, list scroll and the slider drag track are
  per-window (`g.hoverRow` vs `g.wbHoverRow`, `g.matPos` vs `g.wbMatPos`,
  `g.dragTrackL/R` recorded by whichever window started the drag).
- **Popup geometry** (class/filter/slot dropdowns, colour picker) is
  single-sourced in `MaterialsHelpers.inc` from a `WbLayout`, so the window
  that draws a popup hits exactly what it drew.

**Rejected alternatives.**

- *Enlarging the side panel* — rejected: it is `kPanel` wide by design for
  every other tab; widening it for one tab changes the whole layout.
- *A second thread / second message pump* — rejected: two pumps over one `App`
  need locks on every shared field (the export path already takes `g.mu`);
  a same-thread window costs nothing and cannot deadlock.
- *Moving the 3D view into the workbench* — rejected: the viewport belongs to
  the viewer (ADR-008 keeps one view), and the edits must be visible in it.
- *Destroying the window on close* — rejected: it drops scroll/selection and
  re-registers the class on every open for no benefit.

**Consequences.** The panel's Materials tab keeps the class filter, the
material list and the count (it stays useful while the workbench is open, and
selecting a material in either list reveals it in both). Every material edit
moved out of `Panel.inc`/`Clicks.inc` into `viewer/Workbench.inc`; the panel
lost ~300 lines. The scripted run drives the second window by class name
(`WM_TEST` is answered by both), and the layout dump gained a `workbench`
section beside `materials`. Known cost: `g.matFilterDropOpen` is one flag for
two windows — acceptable because the dropdown is modal and only one list is
clickable at a time.
