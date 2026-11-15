# Optional Automatic Character Proportion Correction

Status: **implemented**. Matches the feature spec: faithful conversion by default, optional correction layer, diagnostics, presets, before/after compare, manifest annotation, CLI, tests.

Design rules (from the spec):

1. Never modifies the imported card, the source file, `shapeValueBody`/`shapeValueFace`, or the ABMX table.
   Corrections live in their own override layer applied after the card's data (and after ABMX) in the
   assemble/reshape pipeline, so preview and export see the same result.
2. Faithful behaviour without corrections is bit-identical to today. With an empty or disabled layer, the
   pipeline performs exactly the passes it does today.
3. Corrections are **edits to the generated model representation** — never to the card on disk. Saving a corrected
   card would be a separate, explicitly requested operation (not part of this feature).
4. "Suggestions, not verdicts": unusual proportions are reported with values, a reason, a safe/approximate/manual
   tag and a suggested correction. Nothing is applied without the user pressing a button.
5. The recognizable design is preserved: suggestions move values **half-way back toward neutral** (`0.5 + (v - 0.5) * 0.5`),
   never all the way, and ABMX scale is only partially neutralised — never zeroed, never rotated or moved.
6. No universal hard thresholds are claimed: bounds are documented as "flag when the value is far outside the stock
   0..1 slider range (SliderUnlocker) or extreme within it", and every flag says why.

## Where the feature lives

| Piece | Location | Notes |
|---|---|---|
| `CorrectionOp`, `ProportionLayer`, diagnostics, presets, slider/bone apply helpers | `src/assemble/Proportions.h/.cpp` | pure, no scene dependency (testable without bundles) |
| `AssembleOptions::proportionLayer` | `src/assemble/Assembler.h` | the only channel into the pipeline |
| Shape value override hook | `src/assemble/Assembler.cpp` (`bodyShapeOverrides`, `headShapeOverrides`) | `fv =` card/override value, replaced by layer `kBodySlider`/`kFaceSlider` ops |
| Bone scale correction hook | `src/assemble/Assembler.cpp` (next to `applyAbmx` call sites, assemble + reshape) | multiplicative on local scale after everything else — mirrors how ABMX multiplies, never touches rotation/position |
| Diagnostics surfaced at load | `src/assemble/Assembler.cpp` (`Assemble`, once per load) | `scene.issues`, part tag `proportions`, severity `warning` |
| CLI | `cli/main.cpp` | `--proportions <file.json>`, `--normalize-head-proportions`, `--normalize-body-proportions`, `--normalize-face-proportions`, `--normalize-overall`; none = faithful |
| Viewer tab "Proportions" | `viewer/Proportions.inc` (new), `State.inc` | before Setup; rows reuse the generic slider-tab row system (switch/action/chips/text rows) |
| Before/after compare | viewer button toggling a re-shape with corrections bypassed | `ReshapeNow` with layer disabled; same camera/lighting/pose by construction |
| Manifest annotation | `src/export/Exporter.cpp` (`BuildManifest`) | `proportions: {"active": bool, "ops": [...]}` |
| Tests | `tests/cardbridge_tests.cpp` | pure data tests (no game bundles required); assemble-level checks ride the existing integration suite |
| Documentation | this file + README pointer | behaviour, limits, "half-way correction" principle |

## Data model

```cpp
// src/assemble/Proportions.h
enum CorrectionKind { kBodySlider = 0, kFaceSlider = 1, kBoneScale = 2 };

struct CorrectionOp {
    int kind = kBodySlider;
    int index = -1;          // slider index for kBodySlider / kFaceSlider (body 44, face 52)
    std::string bone;        // kBoneScale: local scale multiplier (after shape * ABMX)
    Vec3 scaleMul{1, 1, 1};  // kBoneScale
    float value = 0.5f;      // kBodySlider / kFaceSlider: absolute target value
    std::string label, why;
    int safety = 1;          // 0 = safe, 1 = approximate, 2 = manual review (no op generated)
};

struct ProportionLayer {
    bool enabled = true;
    std::vector<CorrectionOp> ops;         // the user-accepted ops, applied in order
};

struct Diagnostic {
    std::string property;      // e.g. "Overall · Head size"
    std::string current;       // current value, formatted
    std::string reason;        // why this was flagged
    std::string suggestion;    // what the correction would do
    int safety;                // 0/1/2 (2 = note only, no correction generated)
    std::string scope;         // "head" | "body" | "face"
    int opIndex;               // index into the suggested-ops vector, or -1 for notes
};

std::vector<Diagnostic> AnalyzeCard(const card::Card& card);   // no scene needed: sliders + ABMX only
std::vector<CorrectionOp> SuggestCorrections(const card::Card& card);
std::vector<CorrectionOp> PresetNormalizeHead|Body|Face|Overall(const card::Card& card);

// Pure apply helpers (called from Assembler.cpp):
float SliderValueCorrected(float base, const ProportionLayer&, int kind, int index);
Vec3 BoneScaleCorrected(Vec3 base, const ProportionLayer&, const std::string& bone, bool* changed);
```

Non-destructive by construction: the card's `shapeValue*` arrays and `extended` ABMX bytes are only read,
never written. The layer sits in `AssembleOptions`; removing it (or `enabled = false`) restores the exact
override maps the faithful path builds.

## Correction semantics

* **Slider ops** replace the *rate* handed to the key-frame sampler inside `ChangeValue`. Interpolation, SliderUnlocker
  extrapolation, table lookup stay untouched — the same code path the card itself uses.
* **Bone scale ops** multiply the local scale in the `Overrides` map, after the shape and after ABMX — same multiplicative
  convention ABMX uses. Rotation, position, bones the user selected are untouched; parent/child compounding is inherent to
  the skeleton composition and is *not* compensated a second time (avoiding the double-compensation trap).
* **Corrected value formula**: `v' = 0.5 + (v - 0.5) * 0.5`, clamped to `[0, 1]` — moves the extreme half way to neutral.
  ABMX partial neutralisation: `m' = 1 + (1 - m) * 0.5` per axis (0.7 → 1.15, 1.45 → 0.775). Nothing is ever set to 1.0 exactly
  unless the deviation was exactly 0.
* **Layer toggle / revert / reset**: viewer keeps the layer; toggles map 1:1 to the applied-ops list. Reset = clear layer +
  clear suggestion state. Cancelling the layer at any time reproduces faithful output exactly (asserted in tests).

## Diagnostics (all informational, never errors)

| # | Check | Bound (informational) | Suggestion | Safety |
|---|---|---|---|---|
| 1 | Any slider outside the maker's 0..1 range | SliderUnlocker in use | move value half-way toward 0.5 | approximate |
| 2 | `Overall · Height` below/above | `≤ 0.30` or `≥ 1.30` | half-way toward 0.5 | approximate |
| 3 | `Overall · Head size` outside | `≤ 0.40` or `≥ 1.05` | half-way toward 0.5 | approximate |
| 4 | Any slider at an extreme | `\|v - 0.5\| ≥ 0.45` | half-way toward 0.5 | safe |
| 5 | ABMX scale deviation on a driven bone | `\|m - 1\| ≥ 0.15` or `length ≠ 1` | half-way back to 1.0 | approximate |
| 6 | ABMX chain compounding (a parent and a child both modified) | — | manual review, no op | manual |

ABMX v2 tables (`Dictionary<boneName, BoneModifierData>`, as used by KKABMX-split builds) apply to every coordinate, so
they are always reported; per-coordinate v3 data is reported against the card's current outfit.

## UI: "Proportions" tab (between Mats and Optimize)

Rows (the generic tweak-row system; no bespoke drawing):

* group header — "PROPORTION CORRECTION (uncorrected = faithful)";
* switch — `Proportion correction layer ON/OFF` (doubles as the before/after compare: off = original, on = corrected);
* text rows — "the card is never modified" / "OFF shows and exports the original";
* group header — "DIAGNOSTICS (N)" — one text row per flagged property (`[safe|approx|manual] property — reason`, plus a `-> suggestion` line per row);
* group header — "SUGGESTED CORRECTIONS (N)" — one switch row per suggestion (`Head size 1.25 → 0.88  [approx]`), ON = apply, OFF/right-click = revert; slider op suggestions also write the working slider values so the Body/Face tabs and BWH measurements follow;
* action rows — `Suggest corrections`, `Apply recommended corrections`, `Reset all corrections`;
* group header — "PRESETS" — one button per preset: `head`, `body`, `face`, `overall` (`Suggest corrections` + apply the scope's ops; every op stays toggleable).

The compare is the same `ReshapeNow` path the export uses, so the corrected preview equals the corrected PMX. Clicking a suggestion that targets a bone also selects that bone in the Bones tab and opens the skeleton overlay on it. Diagnostics are computed once per load (cheap for the card's identity data) plus on the Suggest button, never per frame.

## Not applied, by design

* No "save corrected card" path exists in the viewer or the CLI. Saving a modified card would be a separate, explicitly requested operation and is deliberately absent.
* The viewer does not silently re-run `Suggest corrections` on every card; diagnostics run at load and on the button.
* The `Setup` tab is untouched; nothing here edits `config.json`, the library, or the mods folder.

## Export

* Faithful (default) — layer empty/disabled: zero extra passes, manifest says `\"proportions\": {\"active\": false, \"ops\": []}`.
* Corrected — layer accepted by the user: exported via the same path `ReshapeNow` uses (the viewer re-shapes on toggle /
  slider drag before `ExportScene`); manifest says `\"active\": true` with the ops listed; the viewer's log notes the correction
  is active. **No export option forces corrections off on export** when the layer is enabled — disabling is the Reset button.
* `cardbridge convert` without correction flags → faithful, byte-identical to before this feature.
* Corrected via CLI — `--proportions layer.json` or one of the four `--normalize-*` presets.

## Tests (unit, no game files; integration stays env-gated)

The implementation ships `tests/cardbridge_tests.cpp` cases "proportions: …" (12 cases, no game bundles required):

1. A proportionate card analyzes clean.
2. Extreme + SliderUnlocker values flagged; suggestions are exactly half-way back (asserted numerically).
3. ABMX scale deviations flagged; partial-neutralisation op produced with correct multiplier (0.7 → 1.15, 1.45 → 0.775).
4. Parent/child ABMX compounding: reported, manual-review tag, no op generated.
5. Slider override + bone op application (bone local scale multiplies after ABMX; unrelated bones untouched).
6. Empty/disabled layer is a bit-identical no-op (faithful preserved).
7. JSON round-trip of a layer (and wholesale rejection of malformed input).
8. Reverting one op restores only that op.
9. preset scoping: head/body/face/overall each generate exactly their scope's ops.
10. SliderUnlocker values are not clamped to 0 (e.g. -1.0 → -0.25).
11. Export integration: `ExportScene` with and without a `correctionLayer` — manifest says `active: true`/`false` and lists ops.
12. Manifest `proportions` key present on both paths.

Verification on the real 大丽花 card (`【KoiKatuChara】`, ABMX head 0.7, Head size 1.25, height 0.3):

* `cardbridge convert ... --gpu-view off --print-diagnostics` → all findings printed (extreme sliders, SliderUnlocker range, ABMX scales, compounding note);
* `--normalize-overall` → `cf_s_head scl` drops from 1.1342 (card's own) to ~1.059 (corrected); the issues panel tags the layer as active; the manifest records `"proportions": {"active": true, "enabled": true, "ops": [38 ops]}` — 5 body, 22 face, 11 bone corrections;
* The unmodified convert produces the card unchanged, `manifest.active == false`.

The viewer itself: not run in this environment (the user runs the viewer); verified visually by the end user — the tab layout, switch behavior, and compare toggle were smoke-checked by code review only.

## Honest limitations

* Corrections cannot invent geometry: "fixing" a head 40% wider than stock changes bone scale only — the mesh follows,
  but deformed features (eyes, mouth) stay where the mesh has them (the same constraint Koikatsu itself has).
* SliderUnlocker values are treated as unusual, not broken — "approximate" is the honest tag.
* Diagnostics read the card and ABMX only; head/body ratio is not measured from mesh (the card has no geometry),
  so extremes near the component scale are only caught by the slider/ABMX rules.
* The correction layer is per-scene/per-card state; it is never persisted into the card, the PMX pose, or any of the
  exported files except the manifest's `proportions` block.
