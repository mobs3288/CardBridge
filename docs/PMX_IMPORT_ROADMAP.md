# Native PMX Import & Unified Editor — implementation roadmap

**Status: Phases 1–3 complete** (Phase 3 on 2026-11-24). This document tracks the architectural expansion described in the project brief. Each stage ends with: architecture findings, implementation summary, files changed, decisions, test results, known limitations, remaining tasks. No stage is marked complete on the basis of code existing — behaviour is checked.

**Hard rules:** CardBridge stays one application; Koikatsu is an asset *source*, never a runtime dependency of PMX editing; imported data that CardBridge cannot interpret is preserved, never silently dropped; never assume two models share a skeleton; never route an imported PMX through the Koikatsu assembly pipeline for round-trip export.

---

## 0. Architecture findings (from source, 2026-11-21)

### 0.1 There is no PMX importer in the core — but three good starting points exist
- `src/pmx/PmxWriter.{h,cpp}` is a **write-only**, validated PMX 2.0 serializer. Its `Model`/`Vertex`/`Bone`/`Morph`/`RigidBody`/`Joint`/`DisplayFrame` structs (`PmxWriter.h:13-127`) are already the flat representation the exporter fills (`src/export/Exporter.cpp:696-1106`). No code in `src/` reads a `.pmx`.
- `tests/PmxReader.h` is an independent, header-only reader used by ~12 unit tests and ~30 untracked probe tools. It handles both encodings, all index sizes, BDEF1/2/4/SDEF/QDEF (SDEF params discarded), all 11 morph types for *stream alignment*, IK, frames, rigid bodies and joints — but throws away additional UVs, local/fixed axes, external parent keys and non-vertex/UV morph payloads.
- `D:\Project K\engine\src\assets\PmxFile.{h,cpp}` is the most complete native parser (all deform types incl. SDEF params, all 11 morph types, local axes, 2.0/2.1), already used as the independent validator by `tests/pmx_crosscheck.cpp`. It is **engine-tree code** (glm vectors, engine `core/FileIo.h`), read-only, and *discards display frames* (`PmxFile.cpp:397-410`). It is a reference implementation, not a CardBridge runtime dependency.

### 0.2 The scene/exporter split is the seam to build on
- `src/assemble/Scene.h:12-14` defines `assemble::Scene` as an engine-neutral hub ("the exporter and the future engine importer both read this"). `SceneVertex` (`:24-35`) has exactly 4 bone slots + 4 UV sets; `SceneBone`, `SceneMaterial`, `SceneTexture`, `SceneBlendShape`, `SceneChain` cover the editor's needs.
- Export is a one-way projection `ExportSceneImpl(const Scene&, …) -> pmx::Model -> WritePmx` (`Exporter.cpp:696-1106`). No function in the writer consumes a card or bundle.
- Reusable unchanged for a generic editor: `Pose.*` (arbitrary-bone posing), `PmxPhysics.*`, `PmxIk.*`, `MorphCatalog.*`, `MmdNames.*`, `Simplify.*`, `Lod.cpp`, `Occlusion.*`, `GpuView.*`, `Preview.*`, gizmos, `MaterialWorkbench.*`. Tightly KK-coupled: `Assembler.cpp`, `src/unity/*`, `src/card/*`, `src/resolve/*`, `src/shape/*`, hard-coded name tables in `MmdNames.cpp`/`BoneLabels.cpp`/`MorphCatalog.cpp`.

### 0.3 What the existing writer cannot preserve (verified against the supplied sample)
`E:\MikuMikuDanceE_v932x64\Model\TohsakaRin_by_流云景_fcad049c7ca824bb5887a357c7c0fa33\TohsakaRin.pmx` parses cleanly (3,778,833 bytes, zero trailing bytes) with:

| Feature | Sample | Current writer |
|---|---|---|
| PMX 2.0, UTF-16LE, index sizes 2/1/1/2/1/2 | yes | hard-codes 2.0/UTF-16LE and derives sizes — a 2.1 or UTF-8 input is silently converted |
| Additional UV channels | **2** | header byte hard-coded 0 (`PmxWriter.cpp:144`), channel data never written |
| Deform types | BDEF1 9,467 / BDEF2 10,952 / BDEF4 11,523 | SDEF/QDEF collapsed; weights renormalised (`PmxWriter.cpp:175,180`) |
| Bones | 453; 41 local axes, 4 fixed axes, 26 append, 4 IK | local axis (0x0800), external parent (0x2000) and translation-append (0x0200) flags never emitted; append flag bits forced |
| Morphs | 83 (80 vertex, 3 material) | bone/flip/impulse morphs unrepresentable; kind inferred from which vector is non-empty |
| Display frames | **14 frames / 507 entries** | synthesised defaults when `frames` empty; frame entry *order* lost (separate bone/morph vectors) |
| Materials | 24, 10 sphere-mapped, Japanese names | shared-toon index forced to toon01; empty nameEn replaced by local name |
| Physics | 344 rigid bodies, 500 spring joints | joint type forced to 0 |
| Metadata | Japanese comment (multi-line) | preserved as text, but no encoding/version record |

### 0.4 Asset library / cross-model transfer findings
- `src/library/Library.*` copies an `abdata` tree once and never needs the game at runtime; `ModIndex.*` indexes `.zipmod` manifests (GUID, name, version, author, game tags, shader list, `(category,id)` list ids) into `%LOCALAPPDATA%\CardBridge\modindex.json`; `ListTable.*` loads base-game MessagePack tables and mod CSV rows; `Resolver.*` maps card part ids → bundle/asset with caches. All offline.
- **No enumerable asset catalog exists**: `ListTable` only has exact `findBase`/`findMod`, no iteration/search, no thumbnails, no dependency graph, no card-independent `loadAsset`. `docs/CLOTHING_EXPORT.md:76-77` states this explicitly.
- Reusable for cross-model transfer today: `GraftLayerScene` (`Assembler.cpp:2238+`, bone-name map + weight remap), `FixGarmentIntersections` (`GarmentFix.cpp`), `SeparateCoincidentLayers` (`GarmentLayers.cpp`), `AnalyzeImportedRig` (`CustomHairDiag.cpp:26`), `MmdNames` tables, `SelectiveComponent` pick-list model (`SelectiveImport.h:41-54`).

### 0.5 Viewer / CLI / test findings
- Viewer is one Win32 translation unit (`viewer/main.cpp` + ordered `.inc` files) with a single global `App g` (`State.inc:310-549`), immediate-mode UI, D3D11 + software renderer, 10 tabs, undo/redo for pose and material edits, **no document/project file**. `StartLoad` (`Pose.inc:553`) only accepts a card PNG.
- Renderer, gizmos, bone tree, morph sliders, material workbench and optimise tab all operate on `assemble::Scene` only — a PMX→Scene bridge makes them work unchanged (Phase 2).
- CLI is one `cli/main.cpp` with 8 commands and 0/1/2/3 exit codes; new commands need only a handler + `usage()` line + dispatch entry.
- Tests: custom `CHECK`/`test()` harness in `tests/cardbridge_tests.cpp` (226 tests); `build.ps1 -Test` runs only that binary; integration tests are gated on `CARDBRIDGE_TEST_LIBRARY/_MODS/_CARD`. Toolchain: w64devkit g++ + Ninja at `D:\Project K\tools\w64devkit\bin`; path contains spaces, so CMake uses a hand-written windres command.

---

## 1. Phase plan

| Phase | Goal | Status |
|---|---|---|
| **1 — Native PMX foundation** | Lossless document model, native importer, preservation layer, round-trip export, regression tests, `pmx info`/`pmx roundtrip` CLI | **complete** |
| **2 — Unified editing foundation** | PMX→Scene bridge, load `.pmx` in viewer, Model Explorer, materials/textures, existing bone/pose editor, validation | **complete** |
| **3 — Asset library** | Enumerable KK/KKS catalog, category taxonomy, thumbnails, dependency index, asset browser, dedup/caching | **complete** |
| 4 — Cross-model transfer | Accessory attachment, bone mapping/compatibility report, skeleton analysis, weight transfer, manual fitting tools | pending |
| 5 — Advanced editing | Morph, physics, mesh, advanced material/texture editors | pending |
| 6 — Final integration | Unified open/export/project workflow, UI refinement, performance, full regression | pending |

---

## Phase 1 detail

### Decisions
1. **Extend `pmx::Model`, do not create a second document type.** It is already the export representation; adding inert-by-default fields keeps every existing generated export byte-identical and gives import/edit/export one representation.
2. **Preservation is explicit format metadata on the document** (`Model::format`): original version, encoding, all 8 globals bytes/index sizes, and any bytes after the joints (PMX 2.1 soft bodies / future sections). In preservation mode the writer re-emits those values verbatim and writes stored deform types/flags/weights without renormalising.
3. **Unknown weight/morph types fail with a precise `FormatError`** (safe handling) rather than guessing a length; fully parsed-but-uninterpreted data (soft bodies, reserved flag bits, local axes, external parent keys, impulse local flag) is retained verbatim.
4. **The engine loader is not a runtime dependency.** It stays the independent cross-check oracle; the new reader is self-contained in `cardbridge_core`.
5. **Round-trip fidelity is structural, not byte-identity** (per the brief: "preserve functional structure and appearance", "compare structural data"). Text is stored as UTF-8 internally and re-encoded.

### Acceptance criteria
- Parse PMX 2.0 and 2.1, both encodings, all index sizes, all deform and morph types.
- Import → export → reimport preserves counts, names, flags, weights, UV channels, morph payloads, frames, physics and trailing bytes.
- The supplied Rin model round-trips with every section count and feature histogram identical.
- Existing 226 unit tests and the CLI/conversion path still pass unchanged.
- A real-model check is recorded in this document with measured numbers.

### Files changed
- `src/pmx/PmxWriter.h`, `src/pmx/PmxWriter.cpp` — preservation fields on the document; serializer honours source version/encoding/globals/index sizes, deform types, raw weights, SDEF/QDEF, additional UVs, bone flags/local axes/external parent keys, all 11 morph types, interleaved frame items, joint types, extra globals and trailing bytes. Legacy generated exports unchanged.
- `src/pmx/PmxReader.h`, `src/pmx/PmxReader.cpp` — new native importer (self-contained in `cardbridge_core`, bounded reads, precise `FormatError`s).
- `CMakeLists.txt` — reader registered in `cardbridge_core`.
- `cli/main.cpp` — `pmx info [--json]` and `pmx roundtrip <in> <out>`.
- `tests/cardbridge_tests.cpp` — `pmxImportTests()` with five new tests and the real-model check.
- `docs/ARCHITECTURE_DECISIONS.md` (ADR-011/012), `CHANGELOG.md`, `README.md`, `docs/PMX_IMPORT_ROADMAP.md`.

### Results (2026-11-21)
- Build: clean with the w64devkit/Ninja toolchain; no new warnings in the changed core files.
- `cardbridge_tests.exe`: **18,999 checks, 0 failed** (1 integration suite skipped, as before; 5 new PMX tests all PASS).
- Real model from the brief: `pmx info` reports PMX 2.0, UTF-16LE, index sizes 2/1/1/2/1/2, 2 additional UV channels, 31,942 vertices, 142,152 indices, 13 textures, 24 materials, 453 bones (41 local axes / 4 fixed axes / 4 IK / 26 append), 83 morphs (80 vertex + 3 material), 14 display frames with 507 items, 344 rigid bodies, 500 joints.
- `pmx roundtrip`: **3,778,833 bytes → 3,778,833 bytes, byte-identical**; reimport structure preserved.
- Independent engine loader (`tools/_pmxdump.cpp`) produces identical material dumps for the original and the round-tripped file.
- The independent test reader agrees with the native importer on the synthetic 2.0 fixture (deform types 3/4 and impulse morph type 10 included).

### Known limitations
- Round-trip is structural and, for the tested sample, byte-identical; text stored internally as UTF-8 may re-encode differently for malformed UTF-16 (unpaired surrogates). Valid PMX text is unaffected.
- PMX 2.1 soft bodies and globals beyond the first eight are preserved byte-for-byte but not interpreted or editable yet.
- Reserved/unknown *structural* tokens (future deform/morph/joint types) are rejected with a precise error rather than guessed — correct but not forward-compatible beyond those types.
- (Phase 1 limitation, resolved in Phase 2) Nothing in the viewer could open a `.pmx`.

---

## Phase 2 — Unified editing foundation (2026-11-22)

### Implementation summary
- New `src/pmx/PmxToScene.{h,cpp}`: `BuildScene` maps the imported document into `assemble::Scene` (textures decoded from the PMX folder, materials, bones, vertices with deform weights, meshes/submeshes, vertex morphs as blend shapes, group morphs as expressions, one part), reporting every source-only feature as a warning. `ApplySceneMaterials` copies workbench edits back into the document; `ExportDocument` writes a self-contained folder (`<name>.pmx` + `textures/`) with relative texture paths.
- Viewer: `.pmx` accepted by Open / drag-drop / command line (`StartLoadPmx`, no resolver or card); native models keep their document in `Loaded`; the Parts tab is labelled **Model** and renders a Model Explorer (collapse-able sections for meshes, materials, bone hierarchy, morphs, physics, textures and display frames; clicking a bone opens Bones, a material opens the Workbench, a morph opens Morphs). Card-only proportion state is cleared for native models.
- Export from a native PMX writes from the document with edited materials applied — never through the Koikatsu assembly pipeline.

### Files changed
- New: `src/pmx/PmxToScene.{h,cpp}`, `viewer/Explorer.inc`.
- Modified: `CMakeLists.txt`, `viewer/main.cpp`, `viewer/State.inc`, `viewer/Pose.inc`, `viewer/Util.inc`, `viewer/Window.inc`, `viewer/Panel.inc`, `viewer/Export.inc`, `tests/cardbridge_tests.cpp`, `docs/ARCHITECTURE_DECISIONS.md` (ADR-013), `CHANGELOG.md`, `README.md`, this roadmap.

### Results
- Build clean (w64devkit/Ninja, `-j4`), no new warnings.
- `cardbridge_tests.exe`: **19,110 checks, 0 failed** (1 integration suite skipped). Four new Phase 2 tests pass, plus the real Rin model now goes through `BuildScene` in the round-trip test: 31,942 vertices, 453 bones, 24 materials, 80 vertex-morph shapes.
- Source-only data still survives an edited export (`trailing`, local axes, external parent keys, all morph types, frames, physics, textures).

### Known limitations
- Posing is preview-only in this phase: bone edits are not baked into the exported document's rest pose (Phase 5).
- Physics, display frames and non-vertex morphs are visible in the Model Explorer as read-only summaries; editing them is Phase 5.
- The GUI was not driven automatically for this phase (no headless PMX smoke script); verification is via the unit tests and the bridge's real-model check.

---

## Phase 3 — Koikatsu Asset Library (2026-11-24)

### Implementation summary
- `src/library/AssetCatalog.{h,cpp}`: an enumerable, card-independent catalog built from all base list-table rows and every installed KK/KKS zipmod. Records carry display name, numeric category and friendly subcategory, source (base or mod GUID/name/author/path), bundle/asset, `ThumbAB`/`ThumbTex`, attachment `Parent`, texture/mask dependencies and authored flags. A JSON cache keyed on the library's list-table files and the mods-folder fingerprint makes repeat opens free of game-file reads; duplicate keys are collapsed.
- `ListTable::all()/modRowCount()` and `ModIndex::allEntries()` were added as stable enumeration APIs (no existing behaviour changed).
- `LoadAssetThumbnail` decodes a list-row `ThumbTex` straight from the base library bundle or the owning zipmod.
- CLI: `assets build` and `assets list` with `--category`, `--search`, `--limit`, `--json`, `--cache`.
- Viewer: a 12th **Assets** tab with a search box, category chips, a scrolling result list and a metadata/preview card (including a decoded thumbnail when available). It builds the catalog on first use and needs neither a card nor a running game.

### Files changed
- New: `src/library/AssetCatalog.{h,cpp}`, `viewer/Assets.inc`.
- Modified: `CMakeLists.txt`, `src/resolve/ListTable.{h,cpp}`, `src/resolve/ModIndex.{h,cpp}`, `cli/main.cpp`, `viewer/{main.cpp,State.inc,Panel.inc,Window.inc}`, `tests/cardbridge_tests.cpp`, `CHANGELOG.md`, `README.md`, `VERSION`, this roadmap.

### Results
- Build clean; **19,442 checks, 0 failed** (1 integration suite skipped). New catalog tests cover building from a synthetic zipmod, metadata/classification, query filtering, cache round-trip and the earlier mod-index cache tests still pass.
- Real library (`library_full`, no mods): 1,595 items — Clothing 422, Hair 320, Accessories 212, Body 77, Textures 439, Other 125. `assets list --category clothing --limit 5` returns the expected Japanese display names.

### Known limitations
- No per-asset *rendered* mannequin preview yet (only the list row's authored `ThumbTex`); a rendered preview needs the scene-assembly work of Phase 4.
- The category taxonomy is derived from the numeric categories actually present in source/docs; unknown custom mod categories fall under "Other" with their numeric id, not a guessed name.
- The GUI was not driven automatically for this phase; verification is via unit tests and CLI runs.

### Remaining implementation tasks
- **Phase 4:** accessory attachment, bone mapping/compatibility report, skeleton analysis, weight transfer, manual fitting; rendered asset preview.
- **Phase 5:** morph, physics, mesh and advanced material/texture editors; bake poses into the document.
- **Phase 6:** unified project/import/export workflow, UI refinement, performance, full regression.
