# Body Proportion & Measurement Transfer — technical proposal

Status: **proposal only / awaiting review. No implementation has started.**

Date: 2026-11-24. Target version: 0.7.31 (working-tree `VERSION` is user-modified — do not touch).

## 1. Objective

Let a user pick a reference card and a target card, choose individual body measurements,
and make the target adopt the reference's measurements **without adopting the reference's
face, hair, clothes, textures, materials, accessories or identity**. The change must be
non-destructive, non-accumulating, visible in the preview, and baked into the exported PMX.

This is a *measurement* transfer, not a skeleton/model swap. The target keeps its own bone
hierarchy and names; only driving values change.

## 2. What the existing architecture already gives us

Investigated in the current tree (file:line anchors):

| Concern | Existing facility | Where |
|---|---|---|
| Card data | 44 body + 52 face slider floats, ABMX table; **no geometry in a card** | `src/card/CardFile.h:58-97`, `src/card/Abmx.h` |
| Slider → bone transform | Game-derived generated formulas `BodyUpdate/HeadUpdate`, keyframe tables from the library | `src/shape/ShapeBodyGenerated.inc:4-51`, `src/shape/Shape.cpp:53-105,178-210`, `src/assemble/Assembler.cpp:241-334,338-434` |
| Body mesh | shared nude `o_body_a` part `"body"`, skinned to shared rig by name | `src/assemble/Assembler.cpp:2797-2948`, `Builder::addRenderer` at `:1529-1780` |
| Re-shape without re-assembly | `SceneVertex::local/nlocal` bind frames; `assemble::Reshape()` rebuilds skeleton then re-skins **all** meshes | `src/assemble/Scene.h:27-38`, `Assembler.cpp:4637-4749`; identity re-shape is unit-tested (`tests/cardbridge_tests.cpp:10940`) |
| Bone scale override | `ProportionLayer`/`CorrectionOp` (slider absolute value + bone scale multiplier), applied after shape and ABMX in both `Assemble()` and `Reshape()` | `src/assemble/Proportions.h:23-60`, `Proportions.cpp:209-227`, `Assembler.cpp:141-156,2576,2666,4653,4683` |
| Faithful default | empty/disabled layer is bit-identical; unit-tested | `tests/cardbridge_tests.cpp:5112-5137` |
| Measurement | geometry slicer: height + bust/waist/hip circumferences from the **body part** mesh; 160 slices, arm-dominant triangles excluded, plausibility gate | `src/measure/BodyMeasure.cpp:47-437`, `BodyMeasure.h:17-37` |
| cm solver | 1-D bisection over 1–2 coupled sliders (`Bust→4`, `Waist→22/23`, `Hip→24/25`), calls `ReshapeNow` repeatedly | `viewer/Tweaks.inc:59-127`, `src/shape/BodyMeasurements.h:24-91` |
| Clothes/accessories | share rig bone indices/weights; re-skinned by the same `Reshape()`; accessories follow attach nodes via parent-relative offsets | `Assembler.cpp:1637-1699,3144-3185`, `Reshape()` `:4686-4696` |
| PMX export | writes baked `SceneVertex::pos` and `SceneBone::pos`; PMX has **no bone-scale channel**; `ExportOptions::correctionLayer` only records the manifest | `src/export/Exporter.cpp:588-609,795-860`, `src/pmx/PmxWriter.cpp:214,296` |
| Viewer state | one `Loaded` (target) in `g.loaded`, `Current()`; separate per-card `propSugs`; sliders in `g.tw.body/face`, card originals in `bodyCard/faceCard`; export snapshots the already-reshaped scene | `viewer/State.inc:32-46,103-121,319-404,725-728`, `viewer/Export.inc:34-155` |
| Reference-without-reload precedent | Selective Import loads a **second** card into its own scene on a worker and never replaces `g.loaded` | `viewer/Selective.inc:461-499,1215-1244` |
| UI primitives | generic tweak rows + `DrawMenu` dropdown + `SelView` 3-view compare | `viewer/Widgets.inc:52-60`, `MaterialsHelpers.inc:371-561`, `Selective.inc:701-755` |

Conclusion: the feature is a **new transfer layer + new measurement extractors + a new viewer
tab**, built on the existing non-destructive override pipeline. Nothing needs BepInEx, a
running Koikatsu, or a new runtime dependency.

## 3. Proposed design

### 3.1 Measurement set (new, in `src/measure/`)

Extend `measure::Result` with a richer, card-agnostic record (keep the old struct/API
unchanged so existing BWH rows and tests keep working):

```cpp
struct TransferMeasure {
    enum Id { Height, HeadSize, HeadBodyRatio, Bust, Waist, Hip,
              ShoulderWidth, Neck, ArmLength, UpperArm, LowerArm,
              LegLength, Thigh, Calf, Count };
    Id id; const char* label; const char* unit;   // "cm" | "ratio" | "unitless"
    float value = 0; bool valid = false;
    Confidence confidence;  // MeshMeasured | BoneDerived | Approximate
    std::string detail;     // how it was obtained, for the UI tooltip
};
using MeasurementSet = std::array<TransferMeasure, TransferMeasure::Count>;
```

`MeasureTransfer(scene, card)` fills it:

* **Geometry-based** (reuse `measure::Measure` and `SliceChainsForDebug`):
  height, bust, waist, hip. `TorsoPerimeterAt` already exposes arbitrary-height contours.
* **Head**: AABB of parts in groups `Head`/`Face` (same traversal as
  `assemble::SceneHeightMeters`, `Pose.cpp:333-344`); height/width in cm, plus
  head-height ÷ total height. New helper `measure::HeadExtent(scene)`.
* **Bone-derived lengths**: from `SceneBone::pos`/`world` (rest matrices) on both sides:
  `cf_s_shoulder02_L/R` (shoulder width), `cf_s_neck` (neck),
  `cf_s_arm01/02/03 → cf_s_forearm01/02 → cf_s_wrist` (arm, upper arm, lower arm),
  `cf_s_thigh01/02/03 → cf_s_leg01/02/03` (leg). Names come from
  `ShapeBodyGenerated.inc:4-19`; left/right averaged. This is geometry of the assembled
  skeleton, not a blind assumption about bone scale.
* **Limb girth**: new `measure::LimbPerimeterAt(scene, boneName, y)` reusing the existing
  slice/weld/chain code but selecting the chain whose centroid is nearest that limb's bone
  XZ at height `y` (mid-`cf_s_thigh02_*`, mid-`cf_s_leg02_*`), instead of the axis-centred
  trunk chain (`BodyMeasure.cpp:318-327`). Exposed for unit tests.

### 3.2 Transfer spec (new, in `src/assemble/`)

`transfer::Spec` is plain data, independent of the viewer:

```cpp
struct Spec {
    std::string referenceSha256, referenceName, targetSha256;
    std::array<bool, N> selected{};             // per-measurement checkboxes
    std::array<float, N> targetValue{};         // editable, prefilled from the reference
    std::array<bool, N> hitTarget{};            // refine to measured value?
    // Result of building the spec (recomputed, never hand-edited):
    std::vector<assemble::CorrectionOp> sliderOps, boneOps;
    std::vector<std::string> notes;             // "approximate", "unreachable", conflicts
};
```

Build algorithm (`transfer::Build(referenceScene, targetScene, cardRef, cardTarget, spec)`):

1. Extract both measurement sets. For every selected measurement where the reference value
   is valid, set `targetValue = reference.value` (shown in the UI).
2. Map each measurement to its owning control(s) (table below) and produce **absolute**
   overrides — never multiplicative edits of the target's current values:
   * copy the reference's **body slider** values for girth/head/height controls;
   * for length-type measurements, compute `ratio = refSegmentLength / targetSegmentLength`
     and produce ABMX-style **bone `length`** multipliers on the child bones of each chain
     (ABMX `length` scales a bone's offset from its parent — `applyAbmx`, `Assembler.cpp:132`);
   * for head size, additionally copy reference ABMX `scale` on `cf_s_head`/face-root bones
     when the slider alone cannot close the gap.
3. Leave all **face** sliders, hair/clothing selections, textures, materials, accessories,
   attachment nodes, bone names and hierarchy untouched. Never transfer ABMX rotation.
4. **Geometry refinement** (optional per measurement): build a trial scene with the
   overrides, re-measure, and run a bounded 1-D bisection on the owning slider(s) / length
   multiplier to reduce `|achieved - reference|`; cap iterations (reuse the 14-iteration
   pattern from `Tweaks.inc:109-115`) and report the residual honestly instead of claiming
   an exact match. This is the "based on actual model geometry" half of the design.
5. Over-determined combinations get explicit priority: height → head size → head/body ratio
   (ratio is a *reported* outcome when both height and head size are selected; it is solved
   only when it is the sole head-related selection).

Control map (slider indices from `Shape.cpp:178-191`):

| Measurement | Primary control | Secondary / fallback |
|---|---|---|
| Height | body 0 `Height` (`cf_n_height`) | viewer root height scale deliberately **not** used (it scales head + clothes too) |
| Head size | body 1 `Head size` (`cf_s_head`) | ABMX `cf_s_head` / face-root scale |
| Head-to-body ratio | derived from 1 + 0 | report achieved ratio |
| Bust | body 4 | — |
| Waist | body 22 + 23 | — |
| Hip | body 24 + 25 | 26/27 (butt) deliberately excluded |
| Shoulder width | body 37 + 14 | 16, 38 |
| Neck | body 2 + 3 | ABMX `cf_s_neck` scale |
| Arm length | ABMX `length` on `cf_s_arm02/03`, `cf_s_forearm01/02`, `cf_s_wrist` | — |
| Upper arm | ABMX `length` on `cf_s_arm02/03` | body 39/40 for girth |
| Lower arm | ABMX `length` on `cf_s_forearm01/02`, `cf_s_wrist` | body 41/42 for girth |
| Leg length | ABMX `length` on `cf_s_thigh02/03`, `cf_s_leg01/02/03` | — |
| Thigh | body 28 + 29 | ABMX length on `cf_s_thigh02_*` |
| Calf | body 32 + 33 | ABMX length on `cf_s_leg02_*` |

### 3.3 Applying the transfer — extend the existing layer, not a second pipeline

* Extend `assemble::CorrectionOp` (`Proportions.h:31-40`) with optional bone `length`
  and additive `position` channels plus an origin tag (`normalize` / `transfer`).
  Add an `applyBoneTransforms()` sibling of `applyProportionBoneScales()` in
  `Assembler.cpp:141-156`, called at the same four sites, that sets length/position after
  shape + ABMX. JSON (`Proportions.cpp:229-292`) gains optional fields; old files still parse.
* `ProportionLayer::forSlider()` prefers a transfer op over a normalize op on the same
  index; `BoneScaleCorrected()` applies transfer multipliers after normalize ones. When the
  viewer merges the two sets it **drops conflicting normalize ops** for indices/bones owned
  by the transfer, so there is no compounding and existing first-match tests remain valid.
* Preserved guarantee: empty/disabled layer ⇒ identical pipeline (the invariant already
  proven at `tests/cardbridge_tests.cpp:5112-5137`).
* Non-destructive state lives next to `g.propSugs` in `App` (`State.inc:391-404`):
  `transferState { referenceKey, spec, dirty, previewOriginal }`, keyed by reference
  SHA-256. `g.tw.body` holds working values; `g.tw.bodyCard` remains the untouched target
  original. **Reset** restores `g.tw.body = g.tw.bodyCard` and drops the transfer ops;
  applying twice recomputes absolute targets from the reference, so no scaling accumulates.

### 3.4 Reference and target selection (viewer)

There is no loaded-card list today. Add a small bounded list
`std::vector<ReferenceEntry> g.references {path, sha256, name, shared_ptr<Card>, shared_ptr<Scene>, MeasurementSet, thumbnail?}`
following the `Selective.inc:461-499` worker pattern: loading a reference **does not**
replace `g.loaded`. The reference selector uses `DrawMenu` (`Widgets.inc:52-60`), the same
pattern as the material-filter dropdown. The target selector lists the current card plus
loaded references; promoting a reference to target calls the existing `StartLoad()` — one
target is displayed at a time (honest limitation).

### 3.5 UI: dedicated "Body Transfer" tab

* Extend `enum Tab`/`kTabNames` (`State.inc:122-123`) and grow the strip to 3 rows
  (`kTabBottom` 248 → 278; `partsTop` follows automatically at `:601`).
  **ID collision to fix**: the 11th tab would hit button id 20, currently the Materials
  workbench launcher (`Parts.inc:87-92`, `Window.inc:700`, `MaterialsDraw.inc:57`,
  `Window.inc:1071`). Move that launcher to id 60 in those four places (or remap all tabs
  to 40+i); do not leave the collision unresolved.
* Draw via the generic `BuildTweakRows` path (`MaterialsHelpers.inc:371-561`) plus a
  `BodyTransferClick` handler mirroring `Proportions.inc:136-183`. Rows:
  - Reference selector / target selector / status ("reference measured", "target measured")
  - 14 per-measurement rows: checkbox (select), reference value, target achieved value,
    editable target value, validity/confidence tag
  - Select All / Deselect All
  - Apply Transfer, Reset to Original
  - Before/after preview toggle
* **Before/after preview**: phase 1 is a toggle that switches the working values between
  the target's card originals and the transfer values and calls `ReshapeNow()` — the same
  on/off pattern as the existing layer, so what you see is what exports; show measured
  before/after values side by side. Phase 2 (only if wanted) can reuse the 3-view
  `SelRenderViewNow` rendering for a true side-by-side viewport.
* Apply does a **full re-assembly** (`StartLoad(..., keepTweaks=true)` with the transfer state
  injected into `AssembleOptions`), not just `Reshape()`: masks, under-piece classification,
  layer separation and accessory placement must be recomputed against the new body.
  `ReshapeNow()` remains the fast path for interactive value edits. Export already picks up
  the reshaped scene plus `ProportionLayerNow()` (`Export.inc:84,91,124-155`), so no
  separate export pathway is needed; add a guard so export forces re-assembly if the
  transfer state is dirty.

### 3.6 Clothing and accessories (the highest-risk area)

Garments share the rig and re-skin with it, so they follow large bone changes; but:

* body-suppression masks / under-piece copies (`Assembler.cpp:4267-4384`,
  `BodySuppression.cpp`) are computed for the target's **original** body in `Assemble()`;
  `Reshape()` does not recompute them. Full re-assembly on Apply is therefore mandatory.
* Run the existing opt-in `FixGarmentIntersections()` (`GarmentFix.cpp:126+`, already called
  on the export copy at `Export.inc:101-105`) for transfer exports, and report how many
  vertices were pushed out. This is the pragmatic mitigation for poke-through, not a
  guarantee: it cannot re-tailor a garment authored for a very different body.
* Never transfer bone-scale ops onto `cf_d_sk_*` skirt bones; `applyAbmx()` has a deliberate
  skirt-X pinch guard (`Assembler.cpp:125-129`) that `applyProportionBoneScales()` lacks.
  Leg/thigh transfers must not contract skirt fabric.
* Head-size transfer moves the head attach node, so hair and head accessories follow it
  (their geometry/identity stay the target's); face sliders stay target's, so the face keeps
  its design but is scaled — flagged as approximate for extreme ratios.

### 3.7 CLI / batch

Add two headless verbs (all offline, library only):
`cardbridge measure <card.png> [--json]` → measurement set; and
`cardbridge transfer --reference A.png --target B.png --select height,head,bust,... [--json spec]`
→ writes a transfer spec, applied on export with `cardbridge convert --transfer spec.json`
(parallel to `--proportions`, `cli/main.cpp:160-167,850-874,1183`). This supports the
"multiple converted cards" test matrix without driving the UI. New flags must be added to
the bool-flag list at `cli/main.cpp:1424`.

## 4. Known limitations to state up front

1. A card has no geometry. Reference measurements exist only after assembling it against
   the same library; a reference with a missing library/part yields `valid=false` rows.
2. Not all 14 items are independent. Head-to-body ratio is derived; bust/waist/hip and
   width/depth sliders are coupled. Conflicting selections are resolved by the priority rule
   and the achieved value is always shown next to the requested one.
3. Slider values produce a skeleton, not guaranteed centimetres: `typeBone`, `correctHeadSize`,
   the head item's shape table and SliderUnlocker extrapolation differ between cards. The
   refinement pass closes the gap where it can; otherwise the row is tagged *approximate*
   with the residual — no silent claim of exactness.
4. Length transfers need ABMX-style `length` on named bones; bone **scale alone cannot** do
   them, which is why the layer must grow a transform channel.
5. Face features cannot be re-modelled. Scaling `cf_s_head` scales the target's face/hair;
   eyes and mouth keep their authored relative positions (same constraint as the existing
   correction feature — `docs/PROPORTION_CORRECTION.md:162-165`).
6. Clothing fit cannot be guaranteed for extreme transfers; masks and under-piece coverage
   are target-specific. Poke-through or gaps may remain even after garment-fix.
7. Target ABMX rotation/position is never transferred; if it conflicts with a transferred
   scale/length, the row is marked *manual review* and only compatible channels change.
8. Viewer height has two knobs (body slider 0 and a uniform root scale, `Pose.inc:18-29`).
   The transfer uses only body slider 0; the uniform scale remains a user-level tweak.
9. `chainsSettled` scenes cannot be re-shaped (`Assembler.cpp:4638`); Apply must re-assemble.
10. PMX has no bone-scale channel: a transfer that is not applied to the scene before
    `ExportScene()` changes the manifest but not the model. GUI/CLI must prevent that.
11. Native PMX imports are not reshapable (`PmxToScene.cpp:105-137`); the feature is for
    imported cards only.
12. Limb perimeter extraction is new and may be unreliable where arms/skirts overlap the
    slice; it will be validated by unit tests before being offered as an exact target.

## 5. Phasing and acceptance

**Phase A — engine, no UI** (new `src/measure/TransferMeasure.*`,
`src/assemble/ProportionTransfer.*`, layer extension, CLI verbs, unit tests).
Acceptance: measurements on synthetic/procedural scenes; transfer/idempotence/reset tests;
faithful path still bit-identical; manifest records the transfer; PMX export test with a
minimal scene (pattern from `tests/cardbridge_tests.cpp:5207-5277`).

**Phase B — viewer** (reference list, new tab, rows, select-all, apply/reset, preview toggle).
Acceptance: reference load leaves target intact; unselected measurements are byte-identical
in their sliders; export matches preview; no tab-ID collisions.

**Phase C — verification with real cards** (user-supplied cards with markedly different
proportions + their library). Check: head size, body ratios, BWH, clothing alignment, bone
hierarchy/naming, accessory placement, PMX export correctness, and no unintended change to
face/hair/textures. Visual confirmation is done by the user in the running viewer — no
screenshot automation. Record results in `docs/BODY_PROPORTION_TRANSFER.md` and `CHANGELOG.md`.

No version bump / commit is included in this proposal; that happens with the finished change.

## 6. Decisions needed before implementation

1. **Fidelity policy**: copy the reference's slider/ABMX values (deterministic, preserves
   design) with optional measured refinement — as proposed — or attempt a full
   multi-variable least-squares fit to all selected centimetre targets (more exact in theory,
   much slower and more fragile)?
2. **Strongest interpretation of "clothing alignment"**: accept re-assembly + garment-fix
   with residual poke-through reported, or require a stricter garment re-fit pass (significantly
   more work and a new subsystem)?
3. **Height knob**: confirm body slider 0 only (proposed), or allow the uniform root scale
   as an additional transfer channel.
4. **Preview**: toggle + numeric before/after (proposed phase 1) sufficient, or is a true
   side-by-side two-view render required from the start?
5. **Scope of "head size"**: head bone scale only, or also correct the target's face slider
   proportions (the latter changes facial identity and is **not** proposed).
