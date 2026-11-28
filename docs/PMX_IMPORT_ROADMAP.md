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
| **4 — Cross-model transfer** | Accessory attachment, bone mapping/compatibility report, skeleton analysis, weight transfer, manual fitting tools | **complete** |
| 5 — Advanced editing | Morph, physics, mesh, advanced material/texture editors | pending |
| 6 — Final integration | Unified open/export/project workflow, UI refinement, performance, full regression | pending |
| **7 — Koikatsu body restoration for imported PMX** | Restore hidden/missing KK/KKS body geometry when clothing or parts are removed from an imported PMX, preserving the imported character's proportions, skeleton, weights and morphs | **in progress** (foundation) |

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

---

## Phase 4 — Cross-model asset transfer (2026-11-27)

### Investigation summary
- Every primitive needed for a card-free single-asset load exists: `Resolver::openBundle/openItemBundle` accept a synthetic `ResolvedItem`, and `KkBodyTemplate.cpp:24-89` proves `Resolver`/`UnityFile` extraction without a card. `GraftLayerScene` (`Assembler.cpp:2322-2522`) shows name-exact bone mapping + weight remapping but is file-local `static` and KK-skeleton-shaped; `Assemble`/`Builder` are card-bound.
- The imported document is append-friendly: `PmxWriter` `SourceFormat` preserves index sizes and all source-only sections; `PmxBodyRestore` proves append-only mutation. There was no card-free asset extractor, no compatibility report type and no Scene→document write-back beyond materials/body.
- Viewer gizmos (`Gizmo.inc`) work on arbitrary scene parts, but native export ignores part transforms and the Model Explorer has no part rows/chips.

### Proposed approach
Asset catalog record → card-free prefab/geometry extractor → explicit compatibility analysis (required/available bones, matched rest positions, index-size capacity) → conservative translation/scale fit → append-only PMX application (new vertices, indices, one material; no original data reordered) → CLI first, viewer UI later. Assets needing new bones, chains or a rotation fit are rejected with precise errors rather than silently approximated.

### Files changed
- New: `src/library/AssetTemplate.{h,cpp}`, `src/pmx/PmxAssetApply.{h,cpp}`.
- Modified: `CMakeLists.txt`, `cli/main.cpp` (`pmx apply-asset`; asset progress moved off stdout so `--json` stays valid), `tests/cardbridge_tests.cpp`, this roadmap.

### Results
- Build clean; **19,615 checks, 0 failed** (1 integration suite skipped). Three new tests: append-only apply with weight remap and measured scale, missing-bone rejection, and a real catalog asset loaded from `library_full` without a card.
- CLI `assets list --category accessories --json` returns clean JSON; `pmx apply-asset` on the supplied Rin PMX reports `incompatible skeleton: 0 of 1 required bones are present` (`N_move` missing) and exits 3 — the intended explicit fallback.

### Known limitations
- Strict name-exact (case-insensitive local/English) bone matching only; no MMD-Japanese↔KK alias table or geometric fallback, so most third-party PMX models will be rejected with a clear message.
- No asset textures are copied into the PMX document yet; the applied material is plain white. No new bones/chains/physics are grafted; assets requiring them are rejected.
- No viewer "Apply asset" action, compatibility panel or transform editor yet; the native export branch does not bake part transforms. `ExportDocument` still reads texture files only from disk.

### Next steps in Phase 4
- MMD↔KK alias/compatibility layer and a warned geometric fallback; asset texture write path; new-bone/chain grafting; viewer Assets-tab "Apply" with a compatibility report and a transform editor whose values are baked into the document at apply time.

### Compatibility pipeline expansion (2026-11-27)
Stages A–D of `docs/PMX_ASSET_GRAFTING_PLAN.md` landed: bone alias registry, multi-stage matcher with conservative geometric fallback, expanded fit classifications and a generic transform API; CLI gained `--auto`, `--map`, transform flags and `--json-report` (exit 3 incompatible / 4 ambiguous). Tests: **19,650 checks, 0 failed**. Stage E (material/texture import) is now implemented: card-free material extraction, in-memory PNG textures, collision-safe export and multi-material PMX append; **19,690 checks pass**. Viewer workflow and opt-in bone grafting remain pending with documented blockers in that plan.

### Phase 4 completion (2026-11-28)
Remaining gaps were closed: card-free `ClothingFile` → asset enumeration and `pmx apply-outfit`; viewer Assets-tab compatibility/transform/apply/undo/save workflow; and explicit opt-in bone/chain grafting with full reference validation (`pmx graft-bones`). See `docs/PMX_ASSET_GRAFTING_PLAN.md`. Tests: **19,725 checks, 0 failed**. Viewer workflow is implemented but not GUI-smoke-tested; rendered mannequin preview remains a limitation.

### Remaining implementation tasks
- **Phase 5:** morph, physics, mesh and advanced material/texture editors; bake poses into the document.
- **Phase 6:** unified project/import/export workflow, UI refinement, performance, full regression.

### Phase 7 remainder
- Model Explorer deletion/restore actions and native-export application of the deletion set are still pending.

---

## Phase 7 — Koikatsu Body Restoration for Imported PMX (2026-11-27)

### Architecture findings (mandatory investigation)
- Imported PMX: `PmxReader`/`PmxToScene` produce **one** `SceneMesh` + **one** `ScenePart` (`id="model"`, `status="native-pmx"`), `reshapable=false`; no under-piece, nude switch, `coveredBy`, chains or create jobs (`src/pmx/PmxToScene.cpp:119-188`). Source-specific data stays in `pmx::Model` (ADR-013).
- Deletion is a **viewer id-set**, not geometry: `PoseState::deletedParts` (`viewer/State.inc:14-20`), `PartDeleted`/`HiddenPartsNow`/`DeletePart` (`viewer/Parts.inc:6-51`), undo/pose JSON (`viewer/Pose.inc:86-209`, `src/assemble/Pose.cpp:551-627`). The card export applies `exporter::ApplyPartStateForExport` (`src/export/Preview.cpp:1381-1404`); the native-PMX export branch **ignores deletions** (`viewer/Export.inc:43-72`). No "restore body" action exists.
- Under-piece/nudity infrastructure (`src/assemble/BodySuppression.cpp:45-308`, `Scene.h:372-381`, `Exporter.cpp:1109-1168`) is card-assembled-scene only; it can copy existing triangles but cannot invent missing geometry. `SuppressBodyUnderGarments` hard-codes part id `"body"`.
- Body geometry assembly lives inside `assemble::Assemble(const Card&, Resolver&, ...)` (`src/assemble/Assembler.cpp:2826-3086`) and `Builder::addRenderer` (`:1544+`), both card-bound. There is **no card-free body extractor**.
- Card-free primitives do exist: `Resolver::openBaseBundle("chara/oo_base.unity3d")` (`Resolver.h:80`), `UnityFile::{findPrefabRoot,renderersUnder,readMesh,readMaterial,readTexture}` (`UnityScene.h:155-178`), `DecodeBlendChannels` for KKS (`UnityScene.cpp:55`). KK uses `m_Skin` channels; KKS uses channels 12/13 (`docs/KKS_FORMAT_FEASIBILITY.md:71-93`).
- Measurement/transfer machinery (`measure::*`, `assemble::transfer::*`, Body Transfer tab) assumes KK part ids, `cf_s_*` bones and card slider vectors; imported PMX has none, so it cannot be applied directly (`docs/BODY_PROPORTION_TRANSFER.md:87`).

### Root cause / feasibility
The missing capability is twofold: (1) **no card-free extraction of KK/KKS body geometry**, and (2) **no document-level write path** that can append restored geometry without breaking the imported PMX's existing morph offsets, physics references and `SourceFormat`. The preservation-first document (`PmxWriter.h:149-181`) is the right place to append, because appending vertices/indices/materials leaves all existing indices valid. The hard part is **proportion/skeleton compatibility**: an arbitrary PMX cannot be blindly fitted to a default KK body without violating the brief, so mapping must be explicit, measured and fail loudly when bones cannot be matched.

### Proposed approach (incremental)
1. **Card-free body template** (`src/assemble/KkBodyTemplate.*`): extract body mesh + bone names + skin weights from `oo_base.unity3d` through `Resolver`/`UnityFile`, for both KK and KKS. No `Card` involved.
2. **Document-level restoration** (`src/pmx/PmxBodyRestore.*`): map KK bone names onto the PMX bone list (exact → case-insensitive → English), derive a conservative translation/uniform-scale fit from matched rest-bone positions, and **append** body vertices, indices and one material to `pmx::Model`. Never reorders or rewrites original data; existing morph offsets, frames and physics stay valid. Low-confidence fits are rejected with a precise compatibility error, not silently applied.
3. **CLI** `pmx restore-body <in> <out> --library ...` to make the foundation testable without GUI.
4. **Viewer + deletion/export integration** (later in this phase): Model Explorer actions for "remove selected clothing / remove all clothing / remove accessories / restore body"; native export must apply the deletion set and the restored document. Hidden body regions under removed garments are the restored template geometry.
5. **Body textures/morphs** (later in this phase): copy decoded `cf_body` textures into the export folder and add morphs for restored vertices only when the source provides them.

### Known limitations of the foundation
- No rendered mannequin preview; no automatic proportion inference from arbitrary PMX bones beyond matched-bone translation/uniform scale. No rotation fitting — a rig with a different axis convention is rejected.
- Restored body material carries no KK textures yet (new textures are not copied into the PMX document in this increment).
- Viewer deletion/restoration UI is not wired yet; the foundation is exercised through the CLI.

### Implementation summary (landed)
- `src/assemble/KkBodyTemplate.{h,cpp}`: card-free extraction of the largest skinned body mesh under `p_cf_body_00` from `<library>/abdata/chara/oo_base.unity3d`, using `Resolver::openBaseBundle` + `UnityFile::{findPrefabRoot,renderersUnder,readMesh,worldMatrix}`. Produces vertices, indices, renderer bone names and rest-world bone positions; handles KK `m_Skin` and KKS blend channels through the shared reader. No `Card` involved.
- `src/pmx/PmxBodyRestore.{h,cpp}`: append-only document edit. Maps KK bone names onto PMX bones (local/English, case-insensitive), requires at least six matches, computes translation and a bounded uniform scale from matched rest-bone positions, appends body vertices/indices and one material. Original vertices, indices, morph offsets, frames, rigid bodies, joints and `SourceFormat` are untouched; vertices whose influences cannot be mapped are skipped and counted.
- CLI: `pmx restore-body <in.pmx> <out.pmx> --library <dir> [--mods <dir>]`.
- Tests: synthetic append-only preservation, incompatible-skeleton rejection, and a real-library body-template extraction test gated on `CARDBRIDGE_TEST_LIBRARY`.

### Files changed
- New: `src/assemble/KkBodyTemplate.{h,cpp}`, `src/pmx/PmxBodyRestore.{h,cpp}`.
- Modified: `CMakeLists.txt`, `cli/main.cpp`, `tests/cardbridge_tests.cpp`, this roadmap.

### Results
- Build clean; **19,585 checks, 0 failed** (1 integration suite skipped). Real `library_full` body template loaded: 100 body bones, thousands of vertices/indices (verified by the gated test).
- The supplied Rin PMX reports `skeleton incompatible: only 0 of 100 body bones have a matching name in this PMX` — the expected, explicit fallback for a model with Japanese MMD bone names; no proportions are silently changed.

### Known limitations
- Bone-name compatibility is currently exact/case-insensitive only; no MMD-Japanese→KK alias table or geometric fallback exists yet, so many third-party PMX models will be rejected with a clear message.
- A fit is translation + bounded uniform scale only; no rotation fitting. Restored geometry carries a plain material — KK body textures are not copied into the PMX document in this increment.
- Viewer deletion/restoration UI and native-export application of restored geometry are not wired yet; the foundation is exercised through the CLI.

### Next steps in Phase 7
- Add a general MMD↔KK bone alias/compatibility layer (not model-specific), then a warned geometric fallback for unmatched rigs.
- Wire Model Explorer actions (remove selected/all clothing, remove accessories, restore body) and make native export apply the deletion set plus the restored document.
- Copy decoded `cf_body` textures into the export folder and add restored-body morphs only where the source provides them.
