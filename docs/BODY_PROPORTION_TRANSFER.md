# Body Proportion Transfer

Status: implemented (0.7.32+). Proposal: [`BODY_PROPORTION_TRANSFER_PLAN.md`](BODY_PROPORTION_TRANSFER_PLAN.md).

Transfers selected body measurements from a **reference card** to the **target card on screen**,
without copying the reference's appearance. The target keeps its face, hair, clothes, textures,
materials, accessories, bone hierarchy and names; only the chosen body measurements change.

## How to use it (viewer)

1. Load the target character as usual (drop the card on the window).
2. Open the **Transfer** tab.
3. Press **Load reference card...** and choose the card with the measurements you want. It is
   assembled and measured on a worker; the displayed target is not replaced. You can load several
   references and click one in the list to select it.
4. Tick the measurements to transfer. Each row shows the reference value, the target's current
   value, and an editable requested value (cm, or `x` for the head-to-body ratio). **Select all** /
   **Deselect all** are provided.
5. Press **Apply Transfer**. By default it copies the reference's control values for the selected
   measurements — fast and safe. **Measured refinement** is an opt-in switch: it measures the result
   and tries to close the gap, but a 1-D solve on one measurement can fight another (a bust solve can
   flatten the chest), so use it only for a small selection. Unreachable targets are reported with
   their residual instead of being silently faked.
6. **Nude body preview** hides the outfit and accessories so you can judge the body proportions
   themselves (the measurements are all taken from the nude `body` part). **Preview** switches between
   the original and transferred character. Reset restores the target's
   own sliders. Export writes the character currently shown; the manifest's `bodyTransfer` block
   records the spec that was applied.

Nothing is written back to either card file. The transfer lives in a separate layer, like the
existing Proportions corrections; transfer ops win on any slider or bone they also touch, so the two
features cannot compound.

## Measurements and where they come from

| Measurement | Source in the assembled scene | Applied through |
|---|---|---|
| Height | body mesh bounding height | body slider 0 (`cf_n_height`) |
| Head size | head/face part bounding box | body slider 1 (`cf_s_head`) + ABMX scale where needed |
| Head-to-body ratio | head box height / body height (derived) | head size, reported after height |
| Bust / Waist / Hip | torso contour slices (`measure::Measure`) | body sliders 4 / 22+23 / 24+25 |
| Shoulder width | `cf_s_shoulder02_L/R` bone distance | body sliders 37, 14, 16, 38 |
| Neck | skin contour at `cf_s_neck` height | body sliders 2, 3 |
| Arm / upper arm / lower arm | summed `cf_s_arm* / cf_s_forearm* / cf_s_wrist` bone segments | ABMX-style `length` on the child bones |
| Leg length | summed `cf_s_thigh* / cf_s_leg*` bone segments | ABMX-style `length` on the child bones |
| Thigh / Calf | limb contour slices (`measure::LimbPerimeterAt`) | body sliders 28+29 / 32+33 |

Applying always uses absolute reference values, not multipliers on the target's current values, so
repeated applies and reset do not accumulate scaling error. Bone-length transfers use the ratio of
the two scenes' local bone offsets; the referenced code path is `assemble::transfer::Build` and
`Refine` in `src/assemble/ProportionTransfer.cpp`.

## Headless / batch

```
cardbridge measure <card.png> [--library <dir>] [--mods <dir>] [--json]
cardbridge transfer <reference.png> <target.png> <spec.json> [--select height,head,bust,...] [--refine]
cardbridge convert <target.png> --library <dir> --out <dir> --transfer <spec.json>
```

`measure` prints the 14 values; `transfer` assembles both cards, measures them, writes a spec and
prints the achieved values. `--select` limits the transfer to a comma-separated list of names or
indices (all valid measurements by default). `--refine` opts into the slow measure-and-correct pass;
it is **off by default** because a 1-D solve on one measurement can fight another and drive a slider
to an extreme (a bust solve can flatten the chest). `convert --transfer` applies a saved spec; the PMX carries the baked
geometry and the manifest records `proportions` (ops) plus `bodyTransfer` (the whole spec).

## Honest limitations

* A card has no geometry: a reference must be assembled against the CardBridge library before it can
  be measured. A missing body part or an unusable contour leaves that row unavailable.
* Measurements are coupled. Head-to-body ratio is derived from height + head size; if both are
  selected, it is reported rather than solved independently. Bust/waist/hip width and depth sliders
  move together.
* Slider values are not centimetres. `typeBone`, head-item correction tables and SliderUnlocker make
  the same slider value land differently on two cards. Refinement closes the gap where possible;
  otherwise the row is marked approximate with the nearest achieved value.
* Scaling the head bone scales the target's head, face and hair together. Face features keep their
  authored relative positions and can look stretched at extreme ratios — no new geometry is invented.
* Clothing is re-associated when Apply re-assembles the target, and the existing garment-intersection
  correction is available on export. Very different bodies can still leave poke-through or gaps;
  there is no automatic re-tailoring of garments.
* ABMX rotation and position are never transferred; a conflict is reported for manual review.
  Skirt (`cf_d_sk_*`) X shrink is guarded as in the normal ABMX path.
* Viewer height has a second, uniform-scale knob (the Look tab's Height cm). The transfer deliberately
  uses only the body slider so it does not rescale the head and clothes.
* Native PMX imports are not reshapable; this feature is for imported cards.

## Tests

`tests/cardbridge_tests.cpp` covers measurement extraction from synthetic mesh/bone geometry,
transfer-op precedence and JSON round-trips (including bone length/position), absolute and
idempotent Build results, reset, and measured refinement convergence. Run `build.ps1 -Test`.
Visual checks with real cards (head size, clothing alignment, accessories, PMX export) need the user's
cards and library and are not automated.
