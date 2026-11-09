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
Status: **not started.**

Todo (from `MATERIAL_EDITOR.md` §9):

- [ ] `src/material/` module: originals snapshot, sparse overrides,
      classification, presets.
- [ ] `ApplyOverrides` → live `SceneMaterial` write path.
- [ ] Reset (parameter / material / all), copy/paste, multi-apply.
- [ ] `cardbridge-materials` v1 JSON (`MaterialFileToJson`/`FromJson`).
- [ ] CLI `convert --materials`; manifest `material_workbench` section;
      unsupported-field warnings.
- [ ] Unit tests (classification, presets, edits, restoration, JSON
      round-trip, undo, texture assignment, shared instances, PMX mapping).
- [ ] Gated integration: convert with fixture JSON, diff PMX + manifest.

Exit criteria: fixture JSON changes exported PMX colours/alpha; full suite
green; no viewer change.

---

## Stage 2 — Material Inspector (viewer)
Status: **not started.**

- [ ] `Materials` tab + material list + selection + shared-instance badge.
- [ ] Classifier picker, preset picker, RGB + HEX colour rows (HSV if simple).
- [ ] Shading sliders, texture-slot rows, Reset buttons — all through
      `ApplyOverrides` + dirty flag (same-frame on both renderers).
- [ ] Drag-start undo snapshot; amber "changed" marker.
- [ ] `viewer_smoke.ps1` `materials` step.

---

## Stage 3 — Texture and UV tools
Status: **not started.**

- [ ] Texture Inspector (preview, channels, slots, enable/disable, replace).
- [ ] Record texture format string in `SceneTexture` at assembly.
- [ ] `GpuView::EnsureTexture`/`UploadTexture` + `ViewState` invalidation.
- [ ] UV canvas (triangles/wireframe/texture bg, zoom/pan/fit).
- [ ] Five diagnostics (missing, out-of-range, stretch, overlap, seams) as
      pure functions + synthetic-mesh unit tests.

---

## Stage 4 — Rendering diagnostics
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
