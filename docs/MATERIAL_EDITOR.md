# CardBridge — Material Workbench

**Status: Stage 0 complete (investigation). No implementation yet.**
**Branch:** `feature/material-workbench`

This document is the living architecture and implementation specification for the
Material Workbench. It records what exists in the codebase today, what the
Workbench will add, and in what order. Implementation decisions that are easy to
change live here; decisions that would be expensive to reverse go in
`ARCHITECTURE_DECISIONS.md`. Progress is tracked in
`MATERIAL_EDITOR_PROGRESS.md`.

---

## 1. What the Workbench is (and is not)

The Workbench is a focused material authoring and diagnostic surface inside the
existing CardBridge viewer. It lets a user:

- Pick one imported material and change its colour and a small, well-chosen set
  of shading parameters in real time.
- Classify the material (Skin / Fabric / Hair / Eyes / Eyelashes / Metal /
  Plastic / Leather / Glass / Rubber / Generic) and apply a preset that sets
  sensible defaults.
- Inspect the textures a material actually samples, and the UV layout those
  textures are sampled with.
- Preview fabric translucency with a backlight, and rough skin response, using
  cheap approximations that run in the existing renderers.
- Compare original vs edited appearance in the same camera and light.
- Save overrides as JSON, restore originals, and export supported fields to PMX.

It is **not** Blender, Substance Painter, a node shader editor, or a texture
painting app. Scope control is a feature (§12). Anything that needs a new
renderer, a new dependency, or a rewrite of the conversion pipeline is
explicitly deferred.

---

## 2. Architecture assessment (Stage 0, verified against source)

All references below were read, not inferred. Line numbers are from the
`feature/material-workbench` HEAD at the time of writing.

### 2.1 Where materials live today

There are **three** material representations in the tree, and no fourth one
that is editable. The Workbench must sit alongside them, not inside them.

| Layer | Type | File | What it holds |
|---|---|---|---|
| Raw Unity asset | `unity::MaterialData` | `src/unity/UnityScene.h:56-73` | Name, shader name, `textures` (property → PPtr), `colors` (property → Vec4), `floats`, `texST` (scale/offset), render queue, shader path id. This is what the bundle stored. Read-only input. |
| Imported (post-assembly) | `assemble::SceneMaterial` | `src/assemble/Scene.h:55-80` | One entry per material instance in the assembled scene: `name`, `shader` (string name), `shaderIndex` (into `Scene::shaders`), `texture` (the PMX-facing main texture), `diffuse`, `doubleSided`, `alpha`, `renderQueue`, derived `shadow`/`specular`/`outlineColor`/`outlineWidth`, the full property list (`props`: colour/float/vector in `.x`), texture bindings (`texProps`: property → scene texture + ST), `toon` ramp, `bakedLit`, `cardAreola`, `hairTinted`. This is what both renderers and the exporter read. **This is the object the Workbench will edit — via an override layer, never in place.** |
| Card-side overrides | `card::MaterialEdits` | `src/card/MaterialEdits.h:16-66`, `.cpp:83-187` | What the MaterialEditor plugin stored in the card's KKEx block: per-material texture replacements (with embedded images), float/colour/shader overrides, renderer enable flags, plus KSOX/KCOX skin and clothing overlays. The assembler already applies these when building `SceneMaterial` (see §2.3). |

Supporting types in the same header (`src/assemble/Scene.h`):

- `SceneTexture` (38-45): `name`, `source` (bundle provenance), `width`/`height`,
  `rgba` (top row first), `clamp` (wrap mode), `srgb`, `pixelHash`.
- `SceneShader` (98-107): `name`, decompressed DXBC `blob`, `propDefaults`
  (declared defaults), `texDefaults` (white/black/gray/bump), `cull`,
  `cullProperty`, `zWrite`, `queue`.
- `SceneMesh` (125-132) + `SceneVertex` (24-35): `pos`, `normal`, `uv` and the
  three extra sets `uv1`/`uv2`/`uv3` (TEXCOORD1-3), skin weights, `submeshes`
  (material index + indices). UVs are Unity convention (v up); the PMX writer
  flips v.
- `Scene::materials` is indexed by `SceneSubmesh::material`; `Scene::parts`
  group meshes for the viewer's checklist and for export inclusion.

**Gap:** there is no classification, no preset, no interpreted/PBR view, no
override store, and no undo for materials. `SceneMaterial` is mutated today
only during assembly (`materialFor`, `Assembler.cpp:821-1041`) and during the
export bake (`BakeShaderTextures`, see §2.5). After assembly the scene is
treated as read-only by the exporter and as render input by the viewer.

### 2.2 Koikatsu shader import and property extraction

- Bundles are UnityFS 6 / Unity 5.6.2f1. Meshes, materials and textures are
  decoded from serialized files (`src/unity/UnityScene.cpp`, `readMaterial`,
  `readTexture`, `readShaderBlob`, `readShaderRenderState`).
- The compiled DXBC programs travel inside `SceneShader::blob`, byte-for-byte
  what Direct3D 11 takes. `src/shader/UnityShader.{h,cpp}` parses the program
  blob into `UnityProgram`s (cbuffer layouts, texture bindings); `PickForwardPass`
  selects the main forward pass (largest pixel program without shadow-map input
  + matching vertex program).
- Two renderers consume the same blob:
  - **GPU (primary):** `GpuView::Load` (`src/export/GpuView.cpp:1020-1168`)
    creates VS/PS and input layouts from the DXBC, uploads every texture the
    materials bind (`texProps` + main `texture` + globals), and uploads
    geometry (with tangents computed from UV layout). After that, `Render`
    (`GpuView.cpp:1170-1555`) fills constant buffers **once per material per
    frame** (`matDone`, lines 1419-1435): `detail::FillShaderConstants` walks
    the shader's cbuffer params and copies from `mat.props` (with a second
    lookup in `scene.globalProps`), injects `_LightColor0`,
    `_WorldSpaceLightPos0`, `_WorldSpaceCameraPos`, matrices, and `*_ST`
    scale/offset from `texProps`. Then `UpdateSubresource` per slot. The
    game shader then runs on the real GPU.
  - **Software (fallback + bake):** `RenderView` / `RenderPreviewPng` /
    `BakeShaderTextures` (`src/export/Preview.cpp`, declared in `Preview.h`).
    `RenderView:557+` runs the same DXBC through the SM4 interpreter
    (`src/shader/Sm4.*`) per pixel, with `FillConstants:385-426` doing the
    same cbuffer fill on the CPU. `BuildMips`/`SampleBilinear` (Preview.cpp:
    95-127) do mipmapped, bilinear texture sampling. This is the renderer used
    when there is no D3D11 device and the one that runs the export bake.

- Property extraction during assembly (`Assembler.cpp:821-1041`, function
  `materialFor`, which is the single place `SceneMaterial`s are created):
  1. Read `MaterialData` (`m->colors`, `m->floats`, `m->textures`, `texST`).
  2. Apply run-time overrides (`runtime`: card face/body colours, hair tint,
     blush alpha) — e.g. hair tint overwrites `_Color` and sets
     `hairTinted` (862-875).
  3. Bind shader textures (downscaled to ≤1024 px, `shaderTexture:706-743`),
     extra run-time textures (`extraTex`, e.g. lips, nipple/underhair maps,
     eye iris placement) and ST overrides (`stOverride`).
  4. Fill shader-declared defaults for properties the material does not
     store (910-918) — "what the game draws with".
  5. Derive `shadow`/`specular`/`outline*` from `_ShadowColor`, `_LineColor`,
     `_SpecularColor × _SpecularPower`, `_LineWidth(S)` (902-924).
  6. Pick the PMX-facing main texture: list-row textures, colour-mask tint
     bake (`bake:806-817`, cached by recipe), or the prefab's `_MainTex`
     (`textureFor`). Then force `texProps["_MainTex"]` to that texture
     (966-970) — so the shader sees the same pixels the PMX will carry.
  7. Apply MaterialEditor overrides from the card (`edits`): shader switch
     (975-994), float/colour props (999-1000), per-texture-property
     replacements with tiling (1002-1037, with the embedded-image colour-space
     handling at 1005-1015).

- MaterialEditor shader names seen in code and tests: `Shader Forge/main_item`,
  `main_hair`, `main_hair_front`, `skin`, `toon_eye*`, `toon_nose`, `glasses`,
  `main_opaque`, `main_alpha`, `mnpb`, `shadowcast`. Heuristics in
  `Exporter.cpp` (`MmdMaterialLook:161-225`, `IsTooth`, `HasCutout:113-132`,
  `HasInnerLayer:137-159`) already branch on `mat.shader.find(...)` — the
  Workbench's classifier should reuse these substrings, not invent new ones.

### 2.3 Mesh → material relationships

`SceneSubmesh { material, indices }` is the only mesh→material edge. One mesh
can carry several submeshes (several materials); one material can be shared by
many submeshes and meshes. There is no per-vertex material id and no shared
instance graph beyond the index. Consequences for the Workbench:

- Editing "material N" affects every submesh that names it. The hierarchy must
  show that (shared-instance warning) and never silently merge materials.
- Material ID visualisation is free: the index is already known per submesh.
- Selection from the viewport (picking) needs a CPU-side triangle→submesh
  test — the software renderer already projects triangles; the GPU path would
  need a readback or a CPU re-projection of the same triangle (the existing
  `GpuView::Project` + `BoneScreen` pattern in the viewer covers picking).

### 2.4 How material updates can reach the renderer today

| Edit kind | GPU path | Software path | Verdict |
|---|---|---|---|
| `props` value (colour/float, incl. `_Color`, `_ShadowColor`, `_SpecularColor`, `_rimpower`, `_Cutoff`, …), `diffuse`, `shadow`, `specular`, `outline*`, `doubleSided`, `alpha` | Next `Render()` reads them when building the per-frame cbuffers. No shader recompile, no geometry rebuild, no texture re-upload. (`matDone` per frame ⇒ live.) | Next `RenderView()` fills the CPU cbuffers from the same fields. | **Already live.** The Workbench only needs to write these fields (through its override layer) and mark the viewer dirty. |
| `texProps` binding to an *existing* scene texture (re-assign among textures already in the scene) | `PSSetShaderResources` per draw reads the GPU texture bound to that scene index. If the target texture was uploaded at `Load`, re-binding is just a different SRV. **But** `Load` only uploads textures that *some* material bound at load time; a texture that no material referenced is never uploaded. | `MatTextures::Sample` lazily builds mips for any scene texture (`Preview.cpp:454-458`), so any scene-texture index works without a rebuild. | **Mostly live on GPU** if the target was already uploaded; otherwise needs a new `GpuView::EnsureTexture(ti)` helper (see §3). |
| Replacing a texture with *new pixels* (user-supplied file, or a tinted copy) | No API exists in Stage 0. Stage 2 added `GpuView::UpdateTextures(indices)`: it recreates only the named SRVs/textures through the same `MakeTexture` path, without a `Load`. | Write the pixels into a `SceneTexture` + invalidate its `ViewState` mips (`InvalidateViewTexture`, the `built` flag). | **Implemented in Stage 2** for tint copies; Stage 3 reuses it for a file picker. |
| Vertex/UV edits | Would need a geometry re-upload (`Load` or `Refresh`, which only handles skinning). | Would need re-projection (already per-frame). | **Out of scope** (§11) except for read-only UV display. |

**Bottom line:** colour/parameter editing is a same-frame update on both
renderers with zero asset rebuilds. Texture *replacement* is the only edit
that needs new renderer plumbing, and it is small.

### 2.5 PMX export of materials

`Exporter::ExportScene` (`src/export/Exporter.cpp:408-781`) builds `pmx::Model`
(`src/pmx/PmxWriter.h:21-36`) from the scene:

1. `MmdMaterialLook(scene, mat)` (Exporter.cpp:161-225) maps one
   `SceneMaterial` to a `MmdLook`: `diffuse` (with the glasses `_Color` alpha
   special case), `ambient` = diffuse × shadow colour (or white when a toon
   ramp or `bakedLit` applies), `specular` (zeroed — the bake leaves the
   game's highlight out), draw flags (double-sided, ground/self shadow, edge
   suppression for cutout / inner-layer / hair / overlay / transparent),
   edge colour/size (0.6), sphere-map kind (tooth multiply / hair ring /
   gloss) and strength.
2. The material's texture is written to `textures/*.png`. For `main_item`
   materials the alpha is additionally thresholded at 0.25 (`kItemCutout`,
   line 90) so MMD does not blend it see-through.
3. `pmx::Material` fields written: `diffuse`, `ambient`, `specular`,
   `specularPower`, `drawFlags`, `edgeColor`, `edgeSize`, `texture`,
   `sphere`/`sphereMode`, `toon` (the material's own ramp texture when
   `mat.toon` is non-empty and not `bakedLit`), `memo` ("part=…; material=…;
   shader=…").
4. Pre-export the exporter usually runs `BakeShaderTextures`
   (`Preview.h:65-76`): the game's shader is run over the mesh's UV layout
   (flat sheet, lit from front; or `lit` on the real mesh) and the result
   *replaces* the material's texture and (with `toon`) measures the
   shader's shade-vs-N.L curve into `mat.toon` (32 rows). This is why the
   PMX colour matches the maker camera: the PMX texture *is* the game's
   shader output, and the material is written near-white so engines that
   tint by diffuse show it unchanged. The viewer can show exactly this with
   `GpuView::SetPlain(true)` ("as exported", `GpuView.h:122-126`).

**What PMX can carry** (and therefore what an override may map to): base
colour + alpha (diffuse), ambient tint (→ shadow colour), specular colour +
power, double-sided flag, shadow/edge flags, edge colour + size, one texture,
one sphere map (multiply/add), one toon ramp. **What it cannot carry:**
metallic/roughness as such, transmission, thickness, subsurface, anisotropy,
normal intensity, per-channel textures (normal/roughness/AO/…), light
direction, or any Workbench classification. Those stay in the Workbench JSON
and must produce explicit warnings at export, never silent loss (§5.4).

`BuildManifest` (Exporter.cpp:306-406) writes `<name>.cardbridge.json`
(schema_version 1): tool/input/character/provenance/export header, parts
(with optional heel provenance), morph names, texture list (name, size,
source bundle, pixel hash), counts, pose, issues, validation. This is the
natural home for Workbench provenance (which overrides were applied).

### 2.6 Textures: import, storage, formats

- Decoded by `unity::DecodeTexture` (`src/unity/TextureDecode.h:15`):
  Alpha8, RGB24, RGBA32, ARGB32, DXT1, DXT5, BGRA32 — "everything seen in
  the sampled bundles". BC7 exists only in `third_party/bc7decomp` for the
  few face textures that use it.
- Stored as `SceneTexture` (RGBA8, top row first). `srgb` flag comes from
  Unity `m_ColorSpace`; `clamp` from `m_WrapMode`. Shader maps are capped at
  1024 px (§2.2); the main texture keeps full resolution.
- The viewer has **no texture inspector today**: no preview, no resolution/
  format display, no channel view. The TextureDecode path gives us width,
  height and format for free; `SceneTexture` gives us the pixels.
- "Logical channels" (base colour, normal, roughness, …) do not exist as a
  concept. What exists: arbitrary `texProps` bindings (`_MainTex`,
  `_NormalMap`, `_LineMask`, ramps, gloss, …). The Workbench should map
  property names to logical slots heuristically and show "not bound" for the
  rest — not invent pixels.

### 2.7 UVs

Vertices carry `uv` + `uv1`..`uv3` (Scene.h:27-29). That is all. There is no
UV editor, no UV wireframe, no islands, no diagnostics. UVs are the one place
where the spec asks for a new 2D view. The data needed is already in memory:
per-mesh vertex UVs + submesh indices give triangles; `HasCutout`
(Exporter.cpp:113-132) already walks triangles and samples UV space, and
`Preview.cpp`'s bake walks the same layout for texture generation. UV
diagnostics (stretch, overlap, missing, out-of-range, seams) are therefore
pure CPU geometry over `SceneMesh` — no renderer change needed beyond a 2D
draw surface.

### 2.8 Viewer UI architecture

- `viewer/main.cpp` (4028 lines) is a single Win32 window with an immediate-
  mode panel rendered into `ui::Surface` (`viewer/Ui.h`, `Ui.cpp`) — an
  off-screen surface GDI draws text into and GDI+ draws shapes into, clipped
  and copied where changed. No retained widgets, no Qt/ImGui.
- Tabs today (`viewer/main.cpp:292-293`): `Parts`, `Body`, `Face`, `Look`,
  `Bones`, `Setup`. `PanelLayout` (`308-363`) lays out header buttons, outfit
  chips, tab strip, part list / tweak list, log, footer. Draw functions and
  click dispatch are hand-rolled per tab (`DrawBonesTab:1842`, `TweakClick`,
  `BonesTabClick`, the tab hit-test in `DlgProc`).
- `Tweaks`/`SliderRef` implement a generic slider+switch system (body sliders
  0-43, face 100-151, look sliders 200-206, switches from 300, shader layers
  from 400, bone fields 10000+, settings actions 20000+). Body/face sliders
  re-shape live via `App::mu` + `Reshape` without reassembly
  (see header comment, main.cpp:1-22).
- Shift+Left-click on any slider row starts in-place keyboard entry of the
  value (`BeginSliderEdit` in viewer/SliderEdit.inc): the row draws as a text
  box, keys are modal (`SliderEditKey`), Enter commits (clamped to the range,
  snapped to the step) and Esc restores. `kind == 6` rows are preset chips
  (`DrawPresetChips`): "Band:" / "Cup:" for the bust size (slider 4, mapped
  linearly over 0..1 in 72 band\u00d7cup steps) and "Size:" (S/M/L/XL) for the
  nipple offset (kNippleMm, 200).
- Bone undo/redo exists for poses (`undoStack`/`redoStack`, main.cpp:433-436,
  `CapturePose`/`UndoPose`). It is pose-only; material undo must be a
  separate, analogous stack (see §4).
- The 3D view is the right half of the window; `GpuView` is preferred,
  `RenderView` is the fallback; `highlightPart` (hover tint + silhouette ring,
  `GpuView.cpp:1511-1544`) is the existing selection-feedback primitive.
- There is no colour picker widget. `Ui.h` exposes only `Fill`/`FillRound`/
  `StrokeRound`/`FillGradient`/`FillDisc`/`FillTriangle`/`Line`/`ProgressBar`/
  `Logo` and text. Every Workbench control (RGB, HSV, HEX, sliders, channel
  toggles, UV canvas) must be drawn with these primitives and fed by the
  existing click/mouse dispatch. This is the single largest piece of new UI
  code and the reason Stage 2 must land a *thin* inspector before richer
  widgets.

> **Note (2026-11-10, ADR-010):** the source was split after Stage 2 —
> `viewer/main.cpp` is now a hub that `#include`s grouped fragments
> (`Util/State/Materials*/Parts/Panel/Clicks/Window/Workbench.inc`), and the
> material *editor* lives in a second top-level window
> (`viewer/Workbench.inc`); the panel's Materials tab keeps only the filter,
> the list and the launcher button. The tabs are now 7 (`Parts, Body, Face,
> Look, Bones, Mats, Setup`).

### 2.9 Project serialization today

There is no `.cardbridge` project file. Three separate things exist:

1. `viewer.json` (`%LOCALAPPDATA%\CardBridge\viewer.json`): library/mods/
   export/pipeline settings only (`App::Settings`, main.cpp:122-129).
2. Pose files: `PoseFile` JSON (`src/assemble/Pose.h:84-95`), saved/loaded by
   the viewer, baked by `convert --pose`. Saved by bone *name*.
3. Export manifest: `<name>.cardbridge.json` (§2.5) + `<name>.pmx` +
   `textures/*.png`.

Material persistence therefore needs a **new, versioned JSON** (analogous to
`PoseFile` but for materials), saved by *material name + part id*, and a
load path that reapplies it after assembly. The CLI gets
`convert --materials <file.json>`; the viewer gets Save/Load buttons next to
the pose ones. Manifest records the applied file name and a count of
overrides (see §5.3).

### 2.10 GPU skinning, CPU fallback, resource lifecycle

- GPU skinning: compute shader + structured buffers (`GpuView.cpp:288-…`,
  `MakeSkinCompute`, `SetSkin:672-776`, `SkinMemoryStats`). Pose edits never
  rebuild geometry. Material edits do not touch this path at all.
- `GpuView::Load` uploads everything; `Unload` releases it; `Refresh(bool)`
  re-uploads positions/normals/tangents after a re-shape. Material edits need
  neither — except texture replacement, which needs the new
  `EnsureTexture`/`UploadTexture` call (§2.4).
- CPU fallback `RenderView` has no persistent GPU resources; its only cache
  is `ViewState` mip chains, invalidated when textures change.
- Both paths share `detail::FillShaderConstants` (declared in GpuView.cpp:25-34,
  defined in Preview.cpp) and the tangents helper — so a new material field
  that should reach the shader must be added in exactly one place
  (`FillConstants`) and both renderers pick it up.

### 2.11 Shader management and extra layers

- Extra HLSL shading layers (`src/export/ShaderLayer.h`): files with a
  `// layer:` / `// blend:` / `// default:` header + `float4 ps(VO i)`,
  compiled at run time via `d3dcompiler_47.dll`, drawn over the game shaders
  (GPU only, `GpuView::SetLayers`). They see the matrix, light, view
  direction, material colours, time, and `mainTex`, but **not** the game's
  pixel output — they are blended on top. They are **not exported to PMX**.
- This is the pragmatic home for a first Backlight/Transmission preview: a
  layer can add a wrap-light term from a user light direction. It cannot
  *replace* the game's shader output, so a true diagnostic "transmission"
  mode needs a fallback-shader path in `GpuView` (see §6) — a decision to
  make deliberately, not accidentally.

### 2.12 Tests today

`tests/cardbridge_tests.cpp` (3082 lines) is a single-binary harness with a
`CHECK` macro and `test(name, fn)` runner. Unit tests need no game files;
integration tests (card → PMX, golden manifest) run only when
`CARDBRIDGE_TEST_LIBRARY` / `_MODS` / `_CARD` / `_GOLDEN` / `_OUT` are set.
Baseline on this branch (prebuilt binaries in `build\bin`):

```
1659 checks, 0 failed, 1 integration suite(s) skipped
```

`build.ps1 -Test` builds all four targets (core, CLI, viewer, tests) and runs
the test binary. It needs `cmake`/`ninja`/a C++20 compiler on `PATH`; on
this machine the portable toolchain lives in `D:\Project K\tools\`
(`cmake\bin`, `ninja`, `w64devkit\bin`) and is prepended when building (the
repository's own script only works on machines where those are installed).
The PMX cross-check (`cardbridge_pmx_crosscheck`) independently reads a
written PMX with Project K's loader and is the export-correctness oracle.

### 2.13 What does not exist yet (explicitly)

- No `MaterialClassification`, preset, or interpreted material type.
- No material override store, undo, dirty tracking, or copy/paste.
- No material hierarchy/mesh hierarchy widget, search, or filter.
- No colour picker (RGB/HSV/HEX), no texture slot UI, no channel inspector.
- No UV view of any kind.
- No diagnostic render modes beyond `textured`/`shaded`/`plain` (+ highlight).
- No studio-light presets, no turntable, no before/after split.
- No vendor-neutral PBR for preview: preview *is* the game's shader (GPU) or
  its SM4 interpreter (CPU), or the baked PMX look (`plain`). The Workbench
  must preserve that property — editing a material changes what the game's
  shader receives, it does not swap in a foreign shader.

---

## 3. Proposed data model

New module (UI-free, like `assemble::Pose`): `src/material/` (or
`src/workbench/` if the team prefers one name — pick one and keep it).

### 3.1 Three tiers (spec §5)

```
Original  (immutable after assembly)
  └─ snapshot of assemble::SceneMaterial at load: name, shader name +
     shaderIndex, texture index, diffuse, props (all), texProps (all),
     shadow/specular/outline*, doubleSided, alpha.
     Stored per material index; never mutated after capture.

Interpreted  (derived, read-only)
  └─ the Workbench's understanding of "what is this material?":
     classification, base colour (from diffuse or _Color), opacity, emissive
     colour+intensity, roughness/specular (estimated from _SpecularColor /
     _SpecularPower and the toon ramp when present), normal intensity
     (estimate), texture slots mapped from texProps by name.
     Recomputed from Original + Override when either changes.
     Never written to the scene directly.

Override  (user-owned, sparse)
  └─ only fields the user touched: classification, base colour, opacity,
     metallic, roughness, specular, emissive colour+intensity, normal
     intensity, transmission params (fabric/skin), per-slot texture
     on/off + replacement index, preset name, notes.
     Keyed by material index; serialised by part + material name.
```

**Effective material** = Original ⊕ Override → the `SceneMaterial` the
renderers read. Application is a pure function
`ApplyOverrides(scene, overrides)` that writes *only* overridden fields into
the live scene (see §3.3). Reset = drop the override. "Reset one parameter" =
erase that field. Originals are never reconstructed from the scene — they are
the snapshot, so presets can never destroy them.

### 3.2 Classification and presets

Classification is **independent of Koikatsu shader identity** (spec §6):
`MaterialClass { Skin, Fabric, Hair, Eyes, Eyelashes, Metal, Plastic,
Leather, Glass, Rubber, Generic }`.

Auto-classification (best-effort, always user-overridable) uses the same
substrings the exporter already trusts:

- `skin` → Skin; `hair` → Hair; `toon_eye` / `eye` → Eyes; `toon_nose` /
  `brow`/`lash`/`eyeline` → Eyelashes; `main_item` / `main_opaque` /
  `main_alpha` → Fabric; `glasses` → Glass; `mnpb` → Generic; else Generic.

Presets set the **interpreted** defaults (colour is *not* overwritten by a
preset unless the user asks — preset writes the classification + shading
parameters, colour stays). Each preset is a `MaterialOverride` with the
class's standard fields filled; the user can then tweak any single field.

### 3.3 Live application (the critical path)

New core API (no UI, testable without a window):

```cpp
// src/material/MaterialWorkbench.h (proposed)
namespace cardbridge::material {
  struct WorkbenchMaterial  { /* interpreted view, per material */ };
  struct WorkbenchState {
    std::vector<WorkbenchMaterial>   originals;   // snapshot at load
    std::vector<std::optional<MaterialOverride>> overrides; // sparse
    // undo/redo stacks of Override-deltas
  };
  void   CaptureOriginals(const assemble::Scene&);
  void   ApplyOverrides(assemble::Scene&, WorkbenchState&); // writes scene
  void   ResetMaterial / ResetParameter;
  // classification, preset application, copy/paste, multi-apply
  // JSON load/save (PoseFile-style)
  // PMX export mapping + unsupported-property warnings
  // UV diagnostics (pure functions over SceneMesh) + texture slot mapping
}
```

Application rules:

- Texture unchanged ⇒ no GPU work at all: the renderers read the new
  `props`/`diffuse` next frame (§2.4).
- Texture re-assigned to an existing scene texture ⇒ same; GPU only needs
  the `EnsureTexture` guard for textures never uploaded (§2.4).
- Texture replaced with new pixels ⇒ one new `GpuView::UploadTexture`-style
  call + `ViewState` mip invalidation. Geometry, shaders, skinning untouched.
- No shader recompilation for parameter edits; shader is only re-selected via
  `shaderByName` when the user explicitly picks a different Koikatsu shader
  (which already exists as a MaterialEditor override path,
  Assembler.cpp:975-994 — the Workbench reuses it through `shaderIndex`).

### 3.4 Field mapping (interpreted → scene → PMX)

| Workbench field | Scene (`SceneMaterial`) write | Preview effect | PMX write | Unsupported in PMX? |
|---|---|---|---|---|
| Base colour | `_Color` prop + `diffuse` when the forward pixel program reads `_Color`; otherwise a tinted copy of `_MainTex` (original pixels × requested/original ratio) bound as the main texture, with white `diffuse` | Multiplies the main texture in the game shader + fallback shade | `diffuse` (MmdLook); for the tint path the colour is already in the baked/main texture and PMX diffuse stays white | No |
| Opacity | `diffuse.w` (+ `_Color.w` when present) | Alpha blend / cutout | `diffuse.w` + draw flags | No |
| Specular / power | `_SpecularColor`, `_SpecularPower` | Game shader highlight | Sphere gloss strength (derived, as today) | Specular itself not carried — sphere is the carrier |
| Roughness | `_SpecularPower` inverse (approximation) | Highlight width in game shader | Sphere strength | **Yes** — manifest + warning |
| Metallic | No game-shader equivalent | None (preview-only) | None | **Yes** — manifest + warning |
| Emissive colour/intensity | `_EmissionColor` prop (if the shader reads it) | Game shader emission | None | **Yes** — manifest + warning |
| Normal intensity | No scene field; preview-only via layer/fallback | Normal-map strength in preview | None | **Yes** |
| Transmission (fabric) | No scene field; preview-only | Backlight wrap term | None | **Yes** |
| Thickness, backface | No scene field; preview-only | Same | None | **Yes** |
| Subsurface (skin) | No scene field; preview-only | Wrap/backlight tint | None | **Yes** |
| Anisotropic highlight (hair) | No scene field; preview-only | Sphere ring stays | Keep hair sphere (mode 3) | **Yes** |
| Texture enable/disable | `texProps[slot].texture = -1` | Sampler falls back to shader default | Falls back to white | No (white) |
| Texture replace | New `SceneTexture` + bind | Same pixels in preview + PMX | New `textures/*.png` | No |
| Classification | Workbench only | Drives presets + hint | `memo` in PMX material | **Yes** — manifest only + `memo` |

**Base-colour routing (Stage 2 correction).** Writing `_Color`/`diffuse` is
enough only when the forward pixel program actually reads `_Color`. The game's
`main_skin`, `main_opaque` and `main_alpha` do not: their create jobs painted
the card colour into `_MainTex`, which is why a `_Color` edit looked dead on
body/face/outfit. The Workbench analyses the forward program's DXBC cbuffer
operands once per shader and picks the path:

- reads `_Color`, or the bytecode cannot be parsed: `_Color` + `diffuse`;
- provably ignores `_Color` with a main texture: one tinted copy of the main
  texture per material (`…~wb`, multiplied RGBA, clamped, alpha kept,
  `pixelHash` recomputed), bound as `_MainTex`, `diffuse` forced white. The
  copy is cached by source index/ratio/source hash and only rebuilt when one
  changes; `WorkbenchState::changedTextures` tells the viewer which indices to
  re-upload (`GpuView::UpdateTextures`) and whose CPU mips to invalidate
  (`InvalidateViewTexture`).

Consequences: a tint edit appends one scene texture per edited material (not
per frame); a black source channel cannot be brightened (zero stays zero);
opacity on `main_skin`/`main_opaque` may be a no-op or cutout-only because
those programs do not alpha-blend the main texture.

Every "**Yes**" row must (a) still preview in CardBridge, (b) be written to
the JSON, (c) appear in the manifest's `material_workbench` section, and
(d) produce a named warning at export (`scene.warn`) — never silent loss.

### 3.5 Serialization (new file format)

```jsonc
// <name>.cardbridge.materials.json  (also usable as --materials input)
{
  "format": "cardbridge-materials",
  "version": 1,
  "card_sha256": "…",               // optional binding; warn if mismatched
  "materials": [
    { "part": "outfit0.top", "material": "mf_m_…", "shader": "…",
      "class": "Fabric",
      "base_color": [1,1,1,1],
      "opacity": 1, "roughness": 0.8, "specular": [r,g,b], "metallic": 0,
      "emission_color": [0,0,0], "emission_intensity": 0,
      "normal_intensity": 1,
      "transmission": {"strength": 0, "color": [1,1,1], "thickness": 1, "backface": 1},
      "subsurface":   {"color": [1,0.7,0.6], "strength": 0},
      "textures": { "MainTex": {"enabled": true, "texture": "borg_texture.png"} },
      "preset": "Fabric/Default"
    }
  ]
}
```

Rules: sparse (only touched fields serialised), keyed by part + material
name (stable across reassembly), tolerant reader (unknown fields ignored,
unknown materials warned + skipped), version checked (`format`/`version`),
and never writes a card file back — the source card stays untouched.

### 3.6 Undo / dirty / shared instances

- Undo/redo: one entry per *gesture* (drag start → drag end is one step,
  mirroring `PoseChanged`/`undoBase`, viewer/main.cpp:955-983). Stack capped
  (200, same as poses). Entries are `Override` deltas, not full snapshots.
- Dirty tracking: `WorkbenchState::dirty` (compare overrides vs originals).
  The viewer shows the amber "changed" marker the sliders already use
  (`ui::kAmber`, `DrawSliderRow`).
- Shared instances: hierarchy shows "shared by N submeshes" and a confirm-
  style inline note before editing a shared material. No auto-merge, no
  auto-split. Copy/paste = copy the `MaterialOverride` by value.

---

## 4. Rendering modes, lighting, comparison

### 4.1 Diagnostic modes (spec §13)

Implement in both renderers, driven by one enum:

```
Lit, Unlit, BaseColor, Wireframe, Normal, Roughness, Metallic, Opacity,
UVChecker, Transmission, MaterialID
```

- **Software `RenderView`:** add a `ViewOptions::mode` that, per pixel,
  selects what to write instead of the shaded colour. Trivial against the
  existing raster loop (`Preview.cpp:644-800`): base colour, unlit texture,
  normal (from `v.normal`), roughness/metallic (from interpreted fields),
  opacity (alpha), UV checker (from `uv`), material ID (from submesh index).
  Wireframe = skip shading, draw edges only.
- **GPU `GpuView::Render`:** easiest path is a new **fallback shader** in the
  style of `fbVs/fbPs` (`GpuView.cpp:221-286`): one additional runtime-
  compiled HLSL program that reads the same interpolated attributes
  (position, normal, uv, material's interpreted params via its own cbuffer)
  and writes the selected diagnostic colour. It must read interpreted params
  (roughness/metallic/…) that the game shader never sees — passed through a
  per-material cbuffer the Workbench fills, so the game shader path is
  untouched.
- **Material ID:** unique colour per material index (hash → RGB), legend in
  the panel (drawn with `ui::Surface`), highlight selected, isolate mode
  hides non-selected parts via the existing `hiddenParts` flags. Zero writes
  to `SceneMaterial`.
- **Fabric Transmission mode:** normal-dependent wrap-light term from a
  user light direction (dot(N,L) wrapped, scaled by `transmission`),
  two-sided (flip normal for back faces when `backface`), tinted by
  `transmission colour`, attenuated by `thickness`. This is the
  "physically motivated approximation" for this codebase: it is *not* alpha
  transparency, it is a lighting term — documented as such in the UI and
  HERE.

### 4.2 Fabric transmission + skin shading (spec §8/§9)

Both are **preview-only lighting approximations**, implemented in the same
fallback-shader path as §4.1 so the game's own shader is never rewritten:

- Fabric: `L_trans = transColor × transStrength × wrap(N·L_back) ×
  exp(-thickness)` added to the lit colour; `wrap(x) = clamp((x + t)/(1+t))`
  with `t = 0.5·(1-transStrength)`. Two-sided rendering when `backface`
  is on (the material's `doubleSided` is respected, not forced).
- Skin: same wrap term with a `subsurface colour × strength` tint, plus the
  existing `roughness`/`specular` from the interpreted material. No volume
  rendering, no refraction. Backlight preset (light behind the model) makes
  both visible; front light hides them, which is the correct demo.

These run on GPU (fallback shader) and on CPU (the same formula in
`RenderView`'s non-shaded path). Document every approximation in the
Workbench panel's About text and in `ARCHITECTURE_DECISIONS.md`.

### 4.3 Studio lighting (spec §14)

Presets map to **one directional light** the preview renderers already take
(`ViewCamera` + the light derived from it, `Preview.cpp:587-590`,
`GpuView.cpp:1202-1206`):

| Preset | Light direction (toward light) |
|---|---|
| Neutral Studio | 45° front-left-up, intensity 1, white |
| Warm / Cool | Same direction, warm (1, 0.92, 0.78) / cool (0.78, 0.86, 1) |
| Front | Straight along view (current behaviour) |
| Side | 90° from view |
| Backlight | Behind the model (toward camera would be front — so negate) |

User adjustments (direction sliders, intensity, colour, exposure via a
multiplier on the final colour) write to the viewer's camera/light state,
not the scene. Turntable = yaw animation (existing `cam.yawDeg`), no new
system.

### 4.4 Before / after (spec §15)

- Toggle: `showOriginal` flag swaps between the live scene (with overrides)
  and a **second, frozen copy** of the original scene — no, the frozen copy
  is expensive. Cheaper and correct: when `showOriginal` is on, the
  Workbench skips `ApplyOverrides` for the draw (the originals are still
  untouched in the snapshot). Same geometry, same camera, same light.
- Split: two `Render` calls with a clip rect (left = original, right =
  edited). The software renderer supports this today (it takes a viewport);
  the GPU path needs the same `RSSetScissorRects` trick it already uses for
  the highlight ring. Synchronised camera is free — it is the same `cam`.
- No geometry duplication in either case.

---

## 5. Texture Inspector and UV Editor

### 5.1 Texture Inspector (spec §11)

Data already available: `SceneTexture` pixels + size + `srgb`/`clamp` +
`source` + `pixelHash`. UI: a 2D canvas (a `Ui::Surface` region in the
workbench window, ADR-010) showing:

- Preview (blit + upscale, checkerboard for alpha).
- Resolution, format (recorded in `SceneTexture::format` at assembly since
  Stage 3: Unity name via `unity::FormatName`, `RGBA8` for decoded/baked,
  `RGBA8 (tint copy)` / `PNG (replaced)` for Workbench copies), sRGB/linear, wrap mode, source bundle.
- Channel views: R / G / B / A alone, and alpha-on-checkerboard. Pure pixel
  re-mapping on the CPU before blit — no renderer involvement.
- Slot assignment: the interpreted texture slots (MainTex / Normal /
  Roughness / … mapped from `texProps` names, §2.6) with enable/disable
  (bind `-1`) and replace (file picker → decode via `DecodeImageBytes`,
  `card/MaterialEdits.cpp:57-64` → new `SceneTexture` → §3.3 texture path).
- Filtering/wrap options: shown read-only from `srgb`/`clamp` (changing
  them needs a sampler-state change in both renderers — wrap is already
  respected per-material via `samplerWrap`/`samplerClamp` in GpuView and the
  `clamp` flag in `SampleBilinear`; exposing it is "write the flag and
  re-upload one texture").

### 5.2 UV Editor — inspection only (spec §10)

Read-only by design (spec: "do not implement full UV editing unless the
architecture makes it straightforward and core functionality is complete" —
it does not, and core is not complete at Stage 3).

- Canvas: draw the selected mesh's triangles in UV space over (a)
  checkerboard, (b) the assigned texture (blit), or (c) plain wireframe.
  Zoom/pan/fit are the same affine as the 3D camera (scale + pan), applied
  to UV coords. Out-of-range (>0..1) triangles drawn in red, mirroring the
  out-of-range highlight requirement. Wireframe toggle, texture toggle.
- Islands: connected components over triangle adjacency (vertex-index
  adjacency). Selecting an island highlights its triangles and draws the
  seam edges of the bounding island. Multiple islands shown dashed.
- Diagnostics (pure CPU, testable):
  - **Missing UV:** any vertex with non-finite UV.
  - **Out-of-range:** any UV outside [-margin, 1+margin] (margin like the
    exporter's 64-texel cutout epsilon).
  - **Stretch:** per-triangle area ratio (UV area × texture size vs screen
    area) and edge-length ratio; report worst offenders + a heat overlay.
    The same area math already exists in `HasCutout` and the bake's LOD
    computation.
  - **Overlap:** bounding-box overlap between islands (cheap, runs on
    islands not triangles). Mirrored/overlapping UVs are *reported*, not
    treated as errors — Koikatsu bodies do this deliberately.
  - **Seams:** edges used by exactly one triangle (boundary edges) drawn
    in a distinct colour.

All five must be unit-testable without a window (pure functions over
`SceneMesh`), mirroring how `HasCutout`/`HasInnerLayer` are tested.

---

## 6. Viewer layout (spec §17)

**Amended by ADR-010 (2026-11-10): the editor now lives in its own window.**
The original single-panel plan below is kept for what it still describes (the
tab, the list, the viewport strip); the right-hand inspector and the
UV/texture area moved into the **Material Workbench** window.

The side panel keeps one new tab, `Materials` (tab strip grows from 6 to 7;
`TabCount` 6→7). It is a *launcher*, not the editor:

- class filter dropdown (same widget the workbench uses),
- **Open Material Workbench** button → second top-level window
  (`viewer/Workbench.inc`, class `CardBridgeMaterialWorkbench`, ADR-010),
- the material list (index, name, `[Class]`, `xN` shared badge, amber when
  edited, selection bar) — selecting here reveals the material in the
  workbench's list and vice versa.

The workbench window mirrors the viewer's size (to its right, inside the
monitor work area, min 920×600) and holds the whole editor:

```
LEFT   Filter + material list (own scroll, own hover)
RIGHT  heading (name [class] + source shader)
       class / preset / _MainTex row
       R, G, B sliders · HEX + Color picker + Texture view toggle
       shading sliders  ──or──  texture inspector (channel + slot,
                               preview, UV canvas, diagnostics, actions)
       reset · reset all · undo · redo · Save · Load
```

`WbLayoutFor(W, H)` gives both columns for a given client size; the draw
code, the click code and the test layout dump all call it, so they cannot
disagree.

The 3D viewport stays in the viewer's window (ADR-008: one view). The
viewport carries two overlays of its own:

- **The diagnostic mode strip** at the very bottom of the 3D view (GDI over
  the DIBi-blit, `viewer/ModeStrip.inc`): one button per `ViewMode` plus an
  **Isolate** toggle (hide every part not using the selected material;
  session only). The mode persists in `viewer.json` (`viewMode`).
- **The MaterialID legend** (top-left, only in MaterialID mode): one row
  per material — swatch, index, name, `×N` — hovering/clicking rows is
  the panel list's job.

Panel is `kPanel` wide today (~380 px):

- The tab's list region is the launcher list described above. The lighting
  presets and before/after toggle (Stage 5) land in the same viewport bar.

Every new widget is drawn with `ui::Surface` primitives (§2.8). This is the
hardest UI constraint in the project and the plan respects it: sliders first
(Stage 2), colour picker (Stage 2), UV canvas (Stage 3), own window
(ADR-010) — each ships usable without waiting for the others.

---

## 7. Performance and compatibility (§19)

- No shader recompilation for parameter edits (§2.4). No geometry rebuild,
  no texture re-upload, no CPU/GPU stalls. The per-frame cbuffer fill grows
  by at most one extra row of interpreted params for the fallback shader —
  negligible and per-material, not per-pixel.
- GPU resource caching: existing caches (`texMap`, `bakedCache`,
  `shaderCache`, `decodedBytes` LRU in Assembler.cpp) are untouched. The new
  texture-upload call follows the same `MakeTexture` path as `Load`.
- Render-thread safety: all Workbench mutations happen on the UI thread
  (like pose edits, guarded by `App::mu`), then `dirty = true` triggers one
  `Frame()`. No background renderer threads are introduced.
- CPU fallback and GPU skinning are untouched by construction: the
  Workbench writes only `SceneMaterial` fields (and new texture pixels), both
  of which are already inputs to both paths.
- Hardware: no ray tracing, no new API level. The fallback shader is
  `vs_4_0`/`ps_4_0` HLSL compiled via the same `d3dcompiler_47.dll` path as
  the existing fallback/outline/layers.

---

## 8. Testing (§20)

Two levels, following the existing split:

**Unit (no game files)** — extend `tests/cardbridge_tests.cpp`:

- `Material classification`: shader-name → class table (all names in §2.2).
- `Preset application`: each preset writes the expected `MaterialOverride`
  fields and leaves colour alone.
- `Colour/property edits`: `ApplyOverrides` writes the right `SceneMaterial`
  fields (incl. `_Color` linear path) and nothing else.
- `Original restoration`: reset-one, reset-material, reset-all return the
  scene to the captured originals bit-for-bit.
- `Serialization`: JSON round-trip (save → load → identical overrides),
  version mismatch rejection, unknown-field tolerance, unknown-material
  warning.
- `Undo/redo`: gesture coalescing, stack cap, redo invalidation on new edit.
- `Texture assignment`: enable/disable writes `-1`; replace writes a new
  `SceneTexture` and binds it; missing texture is warned, not crashed.
- `Shared material instances`: editing one material affects all its
  submeshes; material-ID colour assignment is deterministic.
- `UV diagnostics`: synthetic quad/strip fixtures for missing / out-of-range /
  stretch / overlap / seam (same style as the `HasCutout` test).
- `PMX export mapping`: each §3.4 row — supported fields reach
  `pmx::Material`, unsupported fields produce the `scene.warn` + manifest
  entry. Unsupported-property reporting is a first-class test, not a
  footnote.

**Integration (needs game files)** — the existing gated suite: card → PMX
against a real library/mods/card, golden manifest comparison, and
`cardbridge_pmx_crosscheck`. The new integration check: convert a known card
with a Workbench JSON and diff the PMX material (diffuse, draw flags,
texture) and the manifest's `material_workbench` section.

**Manual:** `viewer_smoke.ps1`-style walk over real Koikatsu cards (base game
+ modded clothing), GPU and CPU paths, after each stage (§9).

Regression rule (spec §20): no existing test is removed or weakened. If a
stage breaks one, the stage is wrong, not the test.

---

## 9. Implementation plan (Stages 0–7)

Each stage: compile → run tests → review diff → check regressions → record
in `MATERIAL_EDITOR_PROGRESS.md` → report limitations. A stage that reveals
an architectural problem stops the line; later stages do not build on it.

### Stage 0 — Investigation (this document)

- [x] Architecture assessment (§2) — done here.
- [x] Data model proposal (§3) with field mapping (§3.4) and PMX
      capabilities (§2.5).
- [x] Implementation plan (§9) + ADRs (`ARCHITECTURE_DECISIONS.md`).
- Baseline recorded: branch `feature/material-workbench`, prebuilt test
  binary 1659 checks / 0 failed / 1 suite skipped (§2.12).

### Stage 1 — Material foundation

1. New module `src/material/` (UI-free):
   - `MaterialClass`, `MaterialOverride`, interpreted view, `WorkbenchState`
     (originals snapshot + sparse overrides + undo/redo + dirty).
   - `CaptureOriginals` at viewer load / CLI assemble; `ApplyOverrides`;
     reset-one / reset-material / reset-all; copy/paste; multi-apply.
   - `MaterialFile` JSON (format `cardbridge-materials`, version 1) +
     `MaterialFileToJson` / `FromJson` (mirror `PoseFile`, Pose.h:84-95) +
     save/load helpers + provenance fields.
   - Classification heuristic (§3.2) + presets (§3.2) + `MmdMaterialLook`-
     aware export mapping + unsupported-property warnings (§3.4).
2. CLI: `cardbridge convert --materials <file.json>` →
   `ExportOptions::materialFile` → apply after `Assemble`/`BakePose`,
   before `ApplyPartStateForExport`. Manifest `material_workbench` section.
3. Exporter: write the manifest section; emit warnings for §3.4 "Yes" rows.
4. Tests: §8 unit list (everything except rendering) + one integration
   (convert with a fixture JSON, diff PMX material + manifest).
5. Docs: `ARCHITECTURE_DECISIONS.md` entries for the module split and the
   JSON format; progress log update.

*Exit criteria:* a fixture JSON changes exported PMX colours/alpha and the
manifest says so; every unit test passes; full suite green; no viewer
change yet (deliberately — foundation first).

### Stage 2 — Material Inspector (viewer)

1. `viewer/main.cpp`: new `Materials` tab; material list (from the selected
   part's submeshes → materials); selection state; hierarchy header showing
   name/classification/shader/shared-count.
2. Inspector via the existing tweak-list machinery:
   classification picker, preset picker, RGB + HEX colour rows (HSV if the
   draw cost is acceptable — three `FillGradient` rects, no new widget
   library), shading sliders (metallic/roughness/specular/opacity/emission/
   normal intensity), Reset (parameter / material / all) buttons.
3. Live link: slider drag → `ApplyOverrides` → `dirty` → same-frame GPU
   cbuffer update (§2.4). Drag-start snapshot for one undo step (§3.6).
4. Classification filters in the list header (by `MaterialClass`).
5. Tests: same unit suite still green; `viewer_smoke.ps1` gets a `materials`
   step (select material, drag colour, screenshot diff).

*Exit criteria:* change a colour in real time on a real card; undo/redo
works; reset restores; no other tab regresses.

### Stage 3 — Texture and UV tools

1. Texture Inspector (§5.1): preview canvas, channel views, slot table,
   enable/disable, replace (file → `DecodeImageBytes` → new texture).
   Includes the `SceneTexture` format string addition (record the decoded
   format at assembly — today only dimensions survive).
2. New `GpuView::EnsureTexture`/`UploadTexture` + `ViewState` mip
   invalidation (§3.3). CLI unaffected.
3. UV canvas (§5.2): triangles + wireframe + texture background, zoom/pan/
   fit, island extraction + seam emphasis, all five diagnostics as pure
   functions + unit tests on synthetic meshes.
4. Tests: texture replacement round-trip (software renderer shows it, PMX
   carries it); UV diagnostics fixtures.

*Exit criteria:* inspect any material's UVs; replace a texture and see it in
both renderers and in PMX; out-of-range UVs can be spotted visually.

### Stage 4 — Rendering diagnostics (implemented 2026-11-10, see MATERIAL_EDITOR_PROGRESS.md)

1. `ViewMode` enum (11 modes) in software renderer — done, selected via
   `ViewOptions::mode`, diagnostics via `ViewOptions::diagnostics`.
2. GPU fallback shader for the same modes — done (runtime HLSL via
   d3dcompiler_47; wireframe uses a FillMode rasterizer). Game-shader path untouched in Lit.
3. Material ID: deterministic colour per material (`MaterialIdColor`), legend
   overlay, isolate mode (via `hiddenParts`). Shared-material calling out.
4. Mode selector strip in the viewport + `viewMode` persisted in `viewer.json`.
   Tests: software-mode unit checks (render a synthetic scene, assert pixel colours for each mode) — 14 checks.

*Exit criteria met (CPU path):* every mode shows something correct and
deterministic; MaterialID identifies distinct materials in the test suite.
The GPU path compiles and is wired the same way, but its on-screen look is
unverified until the user closes Deadlock and runs a window.

### Stage 5 — Advanced preview

1. Studio light presets + direction/intensity/colour/exposure (one light,
   §4.3) + turntable.
2. Fabric transmission + skin wrap (§4.2) in the fallback shader + software
   path, driven by the interpreted transmission/sub-surface fields.
   Documented approximations (UI text + ADR).
3. Before/after toggle now, split viewport when the GPU scissor trick is
   proven (software first).
4. Tests: transmission with thin vs thick settings produces measurably
   different pixels (software path, deterministic fixture).

*Exit criteria:* backlight visibly separates thin/thick fabric; skin wrap is
visible from behind and invisible from the front.

### Stage 6 — Comparison and usability

1. Full before/after split (both renderers), synchronised camera.
2. Material isolation / hide-others (via `hiddenParts`), rename/duplicate
   (name is display-only; duplicate = new override entry, never a new
   scene material — see ADR), copy/paste, apply-to-multiple.
3. Undo/redo/saved-file polish (dirty badge, Save/Load buttons next to the
   pose ones), search in the material list.
4. Tests: multi-apply writes all targets; rename survives save/load;
   undo depth matches gesture count.

*Exit criteria:* spec §24 items 1-11 all achievable by hand on a real card.

### Stage 7 — Export integration and hardening

1. PMX mapping for every Stage 2-5 field (§3.4) incl. `memo` classification
   + manifest warnings for the unsupported rows. `--no-toon-ramp` /
   `--no-bake` interactions verified.
2. Full persistence round-trip: save in viewer → reopen → identical
   effective materials; CLI `--materials` applies the same file.
3. Full regression suite (unit + gated integration + PMX cross-check) green;
   `viewer_smoke.ps1` full pass on a base-game and a modded card, GPU and
   CPU paths.
4. Performance review: measure per-frame cbuffer fill growth; confirm no
   shader recompile, no geometry rebuild, no texture re-upload on
   parameter-only edits (log lines / `CARDBRIDGE_DEBUG_*` hooks).
5. Documentation: final report in `MATERIAL_EDITOR_PROGRESS.md`
   (implemented / partial / approximations / unsupported / modified files /
   dependencies / test results / manual validation / performance / remaining
   limitations per spec §24). README "Known limitations" updated for
   material-workbench behaviour.

*Exit criteria:* spec §13 checklist (1-13) passes by hand + by test.

---

## 10. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| `viewer/main.cpp` is a 4k-line hand-rolled UI; each new drag interaction is a new mouse-dispatch case. | Buggy hit-testing, tab regressions. | Stages 2/3/5 each add one interaction family only; `viewer_smoke.ps1` gains a step per stage; no other tab touched in the same diff. |
| HSV/HEX widgets do not exist and `Ui` has only shape+text primitives. | Colour editing feels broken if the widget is poor. | Sliders + HEX first (trivial with primitives); HSV as three gradient rects only if it stays simple. |
| The game's shader is opaque: roughness/metallic/transmission cannot change what it computes. | Preview lies if we pretend otherwise. | Interpreted params drive the *fallback* preview modes only; the Lit mode always runs the game's shader with the original props. Documented in-UI. |
| Concurrent edits from the bake thread (as-exported) vs the UI thread. | Torn scene reads. | All edits under `App::mu`, same as poses; bake copies the scene first (`expScene` pattern, main.cpp:1448). |
| PMX loses most interpreted fields. | User thinks the export "kept" transmission. | Manifest `material_workbench` + per-export warnings (§3.4); export dialog lists dropped fields. |
| Texture replacement with arbitrary user images (size/colour-space mismatch). | Washed-out or tiled wrong. | Force sRGB for MainTex replacements (same rule as MaterialEditor `forceSrgb`, Assembler.cpp:1005-1015); cap at 2048 px on the same shrink path as `shaderTexture`. |

---

## 11. Non-goals (scope control, spec §23)

- No node-based shader editor, no PBR authoring suite.
- No texture painting.
- No general renderer (no ray tracing, no new backends).
- No UV *editing* (Stage 3 is inspection + diagnostics only).
- No character-card writing: the Workbench never modifies the `.png` card.
  It writes its own JSON + the PMX export.
