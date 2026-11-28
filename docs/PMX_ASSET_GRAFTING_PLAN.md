# PMX Asset Grafting Compatibility & Import Pipeline

**Status: Stages A–H implemented** (2026-11-28). No viewer GUI smoke test was automated; rendered mannequin preview remains a known limitation. This document is the plan for extending the Phase 4 foundation (`library::LoadAssetTemplate`, `pmx::AnalyzeAssetFit`, `pmx::ApplyAsset`) into a generic PMX asset integration tool. Stages are independently testable; each ends with a build + test run. No stage is marked complete without tests.

## Investigation summary (current source)

- **Already implemented:** card-free asset extraction (`src/library/AssetTemplate.cpp`), strict bone-name compatibility (`src/pmx/PmxAssetApply.cpp:14-96`), append-only geometry/material application (`:98-157`), CLI `pmx apply-asset`, three unit tests + a real-library asset test.
- **Architectural, not missing:** the PMX document is preservation-first (`PmxWriter.h:151-181`); append-only mutation is the correct integrity strategy. `Resolver`/`UnityFile` already expose materials/textures card-free. `GraftLayerScene` (`Assembler.cpp:2322-2522`) shows name-exact bone remapping but is static and card-bound.
- **Missing functionality:** alias registry, normalized/multi-stage matching, geometric fallback, confidence reporting, generic transforms, real material/texture import, viewer workflow, CLI options.
- **Regression risks:** appending bones must never renumber originals; index-size capacity must be checked; transform must not alter destination bones; JSON output must stay clean; card conversion and PMX round-trip must remain byte-stable.

## Reuse plan

`core/Math.h`, `PmxWriter.h` types, `Resolver`/`UnityFile` material+texture readers, `ExportDocument` texture copy loop, `CHECK` test harness, `cmdPmx` CLI pattern. No existing working component is rewritten.

## Stages

| Stage | Scope | Status |
|---|---|---|
| A | Centralized bone alias registry + normalization (JP/EN MMD, KK/KKS, common variants; side/twist/IK distinctions preserved) | **implemented** |
| B | Multi-stage deterministic matcher + conservative geometric fallback; structured mapping records | **implemented** |
| C | Expanded compatibility report (classifications, confidence, hierarchy, capacity, materials) | **implemented** (material note only; extraction is Stage E) |
| D | Generic transform/alignment API (translation, rotation, uniform/non-uniform scale, normals) wired into apply | **implemented** |
| E | Material + texture extraction/import with collision-safe paths and graceful missing textures | **implemented** (E1–E7) |
| F | Viewer preview/compatibility/apply/undo/save workflow | **implemented** — Assets-tab fit report, 9 transform fields, Apply worker, document undo, texture overrides on save |
| G | CLI enhancements + JSON report + meaningful exit codes + full regression | **implemented** — apply-asset/apply-outfit/graft-bones; 19,725 checks pass |
| H | Explicit opt-in bone/chain grafting (new bones appended, references remapped, validated) | **implemented** — validated append-only bones, optional half-weight remap, CLI spec file |

### Stage A–D results
- New files: `src/pmx/BoneAliases.{h,cpp}`, `BoneMatcher.{h,cpp}`, `PmxTransform.{h,cpp}`; extended `AssetTemplate`, `PmxAssetApply`, CLI and tests.
- `src/pmx/BoneAliases.cpp`: Japanese (頭/上半身/ひざ…), English and KK names resolve to one semantic id; side/twist/IK/helper qualifiers are preserved; short Latin aliases cannot match inside longer names ( forearm → elbow, not arm ).
- `src/pmx/BoneMatcher.cpp`: exact → normalized → alias → hierarchy → geometric; every mapping records method, confidence, deviation and ambiguity; ambiguous or distant candidates are never accepted.
- `src/pmx/PmxAssetApply.cpp`: `FitStatus` classifications, hierarchy agreement, index-capacity check, structured mapping list; `ApplyAsset` uses only resolved mappings.
- `src/pmx/PmxTransform.cpp`: point/normal transform (inverse-transpose scale + Euler rotation + translation), returns a copy; skin weights untouched.
- CLI: `--auto/--geometric`, `--map "src>dst,..."`, `--tx/ty/tz/rx/ry/rz/sx/sy/sz`, `--json-report`; exit 3 incompatible, 4 ambiguous. Progress stays on stderr.
- Tests: **19,650 checks, 0 failed**. New coverage for aliases, side/twist/IK distinctions, all match stages, geometric ambiguity, transforms and normal handling. Real-library asset extraction still passes.
- CLI on the supplied Rin PMX with `--auto --json-report`: `N_move` reports `multiple geometric candidates`, exit 4 — a structured, explicit result instead of a hard rejection.

### Stage E results (E1–E7)
- `AssetTemplate` now carries extracted material records (diffuse/ambient/specular/shininess/edge/alpha/flags/texture roles), in-memory encoded textures, per-submesh material indices and index ranges. `LoadAssetTemplate` reads them card-free from `RendererInfo.materials` + `SubmeshMaterialSlot` + `MaterialData`, deduplicates textures by source id, and records warnings.
- Texture bytes: decoded via the existing `DecodeTexture` and re-encoded to PNG with `exportcommon::EncodePng` (Unity compressed formats are never written raw under a `.png` name). Original encoded bytes are not exposed by the current Unity reader — documented limitation.
- `ExportDocument` gained a 5th overload taking per-index in-memory texture bytes; the 4-arg disk-backed path is unchanged. Collision handling is case-insensitive, name-based and content-deduplicated for in-memory entries; path traversal is sanitized.
- `AnalyzeAssetFit` now checks texture/material index capacity and reports material/texture counts. `ApplyAsset` appends one PMX material per source submesh with correct `indexCount`, texture indices, alpha and edge/draw flags; all original sections remain untouched.
- CLI apply is transactional (working copy committed only on success), writes textures via the in-memory export path, supports `--no-textures`, and reports material/texture counts in `--json-report`.
- Tests: **19,690 checks, 0 failed**. New coverage: in-memory export collisions/portability, multi-material appends, alpha preservation, transform payload preservation, original-section integrity and real-library material/texture extraction.
- Real-world CLI: accessory `カフス右用` applied to the supplied third-party Rin PMX with a manual bone map: +399 vertices, +930 indices, +1 material, +4 textures, all resolved in `textures/`.

### Known Stage E limitations
- Source shader features that PMX cannot express (MaterialEditor overrides, custom lighting, some alpha-test semantics) are reported as warnings, not silently substituted.
- Extracted texture filenames are source-id based (e.g. path id), which is deterministic and collision-safe but not human-friendly.
- No rendered mannequin preview of the applied asset (Stage F).

### Phase 4 completion results (F/G/H + clothing bridge)
- **Clothing files:** `card::EnumerateClothingAssets` turns a `ClothingFile`'s clothing/accessory slots into catalog records with no `Card`; `Resolver::resolve(rows, ref)` is the new card-free overload (the Card overload forwards to it). CLI `pmx apply-outfit <in> <out> <file.png>` grafts every resolvable item transactionally.
- **Viewer:** Assets tab now analyzes a selected asset on a worker (status, matched bones, warnings), exposes nine transform fields (translation/rotation/scale) and an Apply button. Apply runs on a worker and commits to a working copy only on success; Ctrl+Z restores the previous document and in-memory texture overrides. `Export.inc` passes those overrides to `ExportDocument`, so applied textures survive save.
- **Bone grafting:** `pmx::AnalyzeBoneGraft`/`AppendBones` validate capacity, names, parent/tail/append/IK references, layer ordering and cycles, then append bones at the end. Existing indices and document sections are never renumbered; optional `remapWeights` moves half of a source bone's weights to the new bone. CLI `pmx graft-bones <in> <out> --spec <file.json>` requires explicit `"confirmed": true`.
- Tests: **19,725 checks, 0 failed** (1 integration suite skipped). New coverage: card-free clothing enumeration, append-only bone graft, invalid graft rejection, transform payload preservation.
- Limitations: no GUI smoke test for the new Assets-tab controls; rendered mannequin preview is still absent; the viewer has no bone-graft UI (CLI only).

## Stage E plan (material & texture pipeline)

- **E1 Data model:** `AssetTemplate` gains material records (name, shader, diffuse/specular/ambient/edge, alpha, flags, texture roles) and in-memory texture payloads (source id, name, format, role, origin, encoded bytes, dimensions), plus per-submesh material indices and index ranges. `PmxTransform` copies them automatically (`out = source`).
- **E2 Card-free extraction:** `LoadAssetTemplate` reads `RendererInfo.materials` + `SubmeshMaterialSlot` + `MaterialData` (no card), records submesh ranges in index order, resolves textures through `Resolver` (`findAsset`/`findTexture`/loose zip PNG), deduplicates by source id, and reports missing/invalid data in `AssetTemplate::warnings`.
- **E3 Texture bytes:** prefer original encoded bytes for loose ZIP PNGs; otherwise decode with the existing `DecodeTexture` and encode to PNG with `exportcommon::EncodePng`. Compressed Unity formats are never written raw under a `.png` name.
- **E4 ExportDocument overload:** keep the 4-arg disk-backed function unchanged; add a texture-override overload that writes in-memory bytes with collision-safe, case-insensitive, deterministic relative paths; content dedup only for overrides.
- **E5 Material integration:** `ApplyAsset` appends one PMX material per extracted submesh with correct `indexCount`, diffuse/specular/ambient/edge/flags and texture indices; texture and material index capacity are checked in `AnalyzeAssetFit`. Original materials/textures are never edited.
- **E6 Transactional CLI:** apply to a working copy, commit only on success; `--no-textures` keeps the old direct-write layout; `--json-report` adds material/texture diagnostics; progress stays on stderr.
- **E7 Tests:** synthetic multi-material/texture extraction, in-memory export, collision/path-traversal, alpha preservation, capacity checks, unchanged original sections, plus real-library asset extraction; full regression gate at 19,650 checks.

## Hard rules

Append-only for ordinary asset application; original document sections byte-identical. New bones only via the separate grafting operation. Ambiguous mappings are never silently accepted. No model-specific offsets or hardcoded PMX paths. Compatibility failures leave the document unchanged.

## Known blockers to record if encountered

- Viewer apply/undo needs a Scene→document write path and a new custom message; it is independent of A–E and can follow them.
- Texture import needs an in-memory texture write path in `ExportDocument`; the current loop reads only files on disk.
