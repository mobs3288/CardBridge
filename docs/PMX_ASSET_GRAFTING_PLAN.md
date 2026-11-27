# PMX Asset Grafting Compatibility & Import Pipeline

**Status: Stages A–D implemented; G partial; E/F/H pending with blockers documented** (2026-11-27). This document is the plan for extending the Phase 4 foundation (`library::LoadAssetTemplate`, `pmx::AnalyzeAssetFit`, `pmx::ApplyAsset`) into a generic PMX asset integration tool. Stages are independently testable; each ends with a build + test run. No stage is marked complete without tests.

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
| E | Material + texture extraction/import with collision-safe paths and graceful missing textures | **pending** — needs an in-memory texture write path in `ExportDocument` |
| F | Viewer preview/compatibility/apply/undo/save workflow | **pending** — needs a Scene→document write path and a new custom message |
| G | CLI enhancements + JSON report + meaningful exit codes + full regression | **partial** — options, JSON report and exit codes landed; full regression below |
| H | Explicit opt-in bone/chain grafting (new bones appended, references remapped, validated) | **pending** — depends on E/F plumbing and reference validation |

### Stage A–D results
- New files: `src/pmx/BoneAliases.{h,cpp}`, `BoneMatcher.{h,cpp}`, `PmxTransform.{h,cpp}`; extended `AssetTemplate`, `PmxAssetApply`, CLI and tests.
- `src/pmx/BoneAliases.cpp`: Japanese (頭/上半身/ひざ…), English and KK names resolve to one semantic id; side/twist/IK/helper qualifiers are preserved; short Latin aliases cannot match inside longer names ( forearm → elbow, not arm ).
- `src/pmx/BoneMatcher.cpp`: exact → normalized → alias → hierarchy → geometric; every mapping records method, confidence, deviation and ambiguity; ambiguous or distant candidates are never accepted.
- `src/pmx/PmxAssetApply.cpp`: `FitStatus` classifications, hierarchy agreement, index-capacity check, structured mapping list; `ApplyAsset` uses only resolved mappings.
- `src/pmx/PmxTransform.cpp`: point/normal transform (inverse-transpose scale + Euler rotation + translation), returns a copy; skin weights untouched.
- CLI: `--auto/--geometric`, `--map "src>dst,..."`, `--tx/ty/tz/rx/ry/rz/sx/sy/sz`, `--json-report`; exit 3 incompatible, 4 ambiguous. Progress stays on stderr.
- Tests: **19,650 checks, 0 failed**. New coverage for aliases, side/twist/IK distinctions, all match stages, geometric ambiguity, transforms and normal handling. Real-library asset extraction still passes.
- CLI on the supplied Rin PMX with `--auto --json-report`: `N_move` reports `multiple geometric candidates`, exit 4 — a structured, explicit result instead of a hard rejection.

### Blockers for remaining stages
- **E:** `ExportDocument` (`PmxToScene.cpp:222-254`) copies texture files only from disk; `AssetTemplate` carries no decoded texture bytes. Need to add an in-memory texture write path and material property extraction before a collision-safe texture import can be correct.
- **F:** native export (`viewer/Export.inc:43-72`) is document-authoritative and there is no general Scene→document write path; applying an asset in the viewer requires a new worker + message + undo stack entry. Independent of A–D.
- **H:** appending bones requires validating every existing reference (IK, rigid bodies, joints, frame items, morph offsets) against appended indices; must be separate from ordinary apply and therefore follows E/F validation patterns.

## Hard rules

Append-only for ordinary asset application; original document sections byte-identical. New bones only via the separate grafting operation. Ambiguous mappings are never silently accepted. No model-specific offsets or hardcoded PMX paths. Compatibility failures leave the document unchanged.

## Known blockers to record if encountered

- Viewer apply/undo needs a Scene→document write path and a new custom message; it is independent of A–E and can follow them.
- Texture import needs an in-memory texture write path in `ExportDocument`; the current loop reads only files on disk.
