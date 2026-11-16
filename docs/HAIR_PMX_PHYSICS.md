# Hair physics for the PMX export — shoulder-length hair resting on the shoulders

**Status:** fixed and validated in the exported PMX (see "Validation"). Companion to the skirt / garment work in the same file.
**Card used as the regression test:** `KK_236802.png` (Oosuki Mamako), outfit slot **1** — `cardbridge convert … --outfit 1`.
The original report was against the default outfit (6, the swimsuit); both were re-exported after the fix and both are calm.

## Symptoms (as reported)

- Shoulder-length hair floated away from the shoulders instead of resting on them.
- The hair sat in an exaggerated outward arch even standing still.
- The hair moved "independently" and twitched / oscillated without any meaningful character motion.
- It did not settle under gravity; the silhouette differed from the hairstyle.

## Root causes (found by inspecting the export and simulating the exported file offline)

The hair the game ships with consists of custom-skeleton items whose DynamicBone components carry **no gravity**
(`m_Gravity = (0, 0, 0)`, `m_Force ≈ (0, -0.0005, 0)`, damping 0.2 per frame, elasticity 0.1–0.2, stiffness 0.1,
`inert = 0.7`). The authored drape **is** the rest pose; the game never falls it under real gravity.

MikuMikuDance, however, applies its **own world gravity** (`0, -9.8 × 10, 0` — saba, the reference implementation of
MMD's physics, sets exactly this) to every dynamic rigid body, on top of whatever the component says. The exported
hair chains therefore fell from an authored pose that was never an equilibrium of the sim they were exported into.

Concretely, four things compounded in the exporter:

1. **Joint springs far too weak.** `stiffnessK = min(max(inertia·ω², lever·g/droop), own·15²)`.
   The cap `own·15²` (radians/s) throttled the spring to a couple of N·m/rad for a link that weighs ~0.2–1 kg,
   while the gravity torque about the joint for a strand fanned away from vertical is tens of N·m.
   The outfit-hair path (all of these items hang under `outfit*` parts) then overrode the spring with a flat `5.0`
   (the garment branch, intended for cloth, chosen because the hair items are outfit accessories).
   Result: gravity wins outright and the links fell onto the ±34° joint stops.
2. **No usable fallback.** The `holds` test (lock the joint to zero range instead of letting it rest on the stop
   and shiver) was disabled for outfit parts by the garment exemption — so hair got the worst of both worlds:
   spring too weak to hold it, and no lock to save it from the stop. In MMD the strands sat on the stops, which is
   the "arch": each of the 3–4 links stopped at 34° below vertical, compounding to a wide outward sweep.
   The stop itself, being a hard limit the spring kept pulling across, is the "twitching / sentient" behaviour.
3. **Damping too weak.** `1 − (1−c)^60` per frame maps the component's retention to a Bullet `v *= (1−d)^dt`,
   but the outfit-cloth clamp `damp = min(damp, 0.75)` applied to the hair (it too is an `outfit*` part)
   left the hair to ring at the limit stops instead of decaying.
4. **The component's per-frame retention vs Bullet's pow-damping mismatch is not the mechanism here,
   but the numbers they produced are the ones the export used**, and they are documented above because the
   garment damping was later reworked for the skirt (see "Co-work in the same file").

Additionally, during the investigation a fifth finding surfaced: the generated **limb colliders** (shoulder→elbow
capsules, radius 0.04 · scale) are met unconditionally by every non-jiggle chain, and the extreme shoulder-side
strands start *inside* them at rest. They shove the strands at load. They were left unconditional on purpose after
testing: excluding them (as the rest-overlap groups do for the game's own colliders) let the strands sink ~5 cm
*through* the shoulder line (worse). With the hair held to its few degrees of swing by the joint limits, the arm
capsule acts as a rest stop under the strand instead of an ejection. Treated as accepted, pre-existing behaviour.

## What was changed

### `src/export/PmxPhysics.cpp` — hair branch in the per-link physics build

New chain classification (`assemble::SceneChainIsHair`, shared, see below):

- `group == "Hair"` (the character's hair slots), or
- `attachNode` on a head socket (`a_n_head*`, `a_n_hair*` — the list row's attach node; imported custom-skeleton hair
  such as the FF2 Accessory Hair Front/Back items, which is what Mamako's look is made of).
- The body's own jiggle components (`characterOwn`) are never hair.
- Anything with no part row (synthetic test scenes) is not hair.

For hair chains only (garments, skirts and jiggle untouched):

| What | Before | After |
|---|---|---|
| Swing limit per joint (X and Z, twist 0.1×) | `bend` from component stiffness, up to ±34.4° | `min(bend, kHairMaxSwing = 0.090 rad ≈ 5.2°)` |
| Joint rotation spring | flat `5.0` (garment branch) / capped `own·225` | `kHairSpringMin = 25` … `clamp(max(inertia·ω², lever·98/droop), …, kHairSpringMax = 280)` |
| Damping (linear + angular) | garment-clamped at `0.75` | `kHairDamp = 0.90` |
| Locked (holds) | never locked (garment exemption) | never locked, but a small stable envelope instead |

The spring is sized from **gravity torque**, not the 15 rad/s inertia cap: the torque of every link hanging below
the joint held within 6 % of sag, bounded above by what the 120 Hz solver integrates cleanly. `kMmdWorldGravity = 98`
(saba's value) is used for this sizing; the rest of the export keeps its scale-derived `9.8·scale`.

The root body is still kinematic (mode 0, follows the bone), the rest are **mode 1** as before, the capsule shapes,
mass taper, collision masks and collider rest-overlap exclusions are unchanged, and hair bodies still meet every
collider (load-time strands keep their support; collisions are **not** disabled).

### `src/assemble/Scene.h`

`SceneChainIsHair(scene, chain)` — the shared predicate (head sockets or the Hair group; never the body's own jiggle).

### `src/assemble/Pose.cpp` — superseded

`BakeGarmentSettle` (the pass that baked MMD's world gravity into the cloth's rest pose) was removed in the
garment-physics redesign: the exported PMX's rest pose must stay the card's authored drape, and the joint springs carry
the authored pose in MikuMikuDance instead. See `docs/GARMENT_PHYSICS.md`.

### `src/assemble/Pose.cpp`, `src/assemble/PhysicsSim.{cpp,h}`, and the garment side of `src/export/PmxPhysics.cpp`

These held the parallel session's work-in-progress at the time of the hair fix (the garment rest-pose bake and the garment joint-spring sizing). Both were resolved in the garment-physics redesign described in `docs/GARMENT_PHYSICS.md`, superseding the interim state recorded here.

### `tests/cardbridge_tests.cpp` — regression tests (3 new)

- `hair: the chain predicate reads the part table` — hair/socket/garment classification, including the bust socket and the body's own jiggle.
- `physics: a hair strand chain keeps a small envelope with a strong centering spring and heavy damping` — exported limits ≤ 5.2°, twist ≤ 0.009 rad, spring within [25, 280], damping ≥ 0.9, the file still passes the repo's `pmxPhysicsFindings` read-back.
- `hair: the garment hang bake drops cloth under world gravity but never the hair` — the bake must not move hair bones while it does move cloth bones.

## Validation

Method: the exported PMX (never just the preview) was simulated offline with **Bullet** — the same engine MMD uses —
replicating saba's `MMDPhysics`: gravity `0, -98, 0`, fixed 1/120 s steps, `btGeneric6DofSpringConstraint` with the
PMX limits and springs (including its sign flip on the Z angular spring), kinematic bodies for mode 0,
`DISABLE_DEACTIVATION`. Measured, standing still, before → after:

- Hair chains behaving unstably: **66 of ~120 chains bad → 31, all of them garments; every hair chain now < 0.5
  units from authored and most < 0.1** (0.2–0.4 ≈ 2–5 cm micro-sag onto the 5° envelope, all of it settled within 0.25 s).
- Peak speed at rest: up to **11.5 units/s of random whip → ≤ 1.6** for hair (worst single initial impulse is the one
  touchdown onto the envelope at load, exactly as for stock Miku PMX joints).
- After 4–6 s standing: hair positions are **bit-identical from t = 1 s onward** (zero twitch).
- Head swept 3 units sideways in 0.6 s (≈ 0.4 m/s head turn): hair **follows with no overshoot and no visible ring**,
  and returns to its rest frame exactly.

Regression test in the game used for the human check: `KK_236802.png`, **outfit 1** (sailor uniform; "Oosuki Mamako"
shoulder-length style). The exported `Oosuki_Mamako_preview.png` is pixel-matched against the pre-fix export — the
hairstyle is unchanged, the resting silhouette preserved, including the ponytail. The exported PMX physics was also
read back through the independent PMX reader and through the repo's `pmxPhysicsFindings` validator: no new findings.

The full unit-test suite passes: **15622 checks, 0 failed** (was 15583 before the fix).

## Before / after physics parameters (one representative strand, Mamako, outfit 1, `outfit1.acc23/joint1`)

| | Before | After |
|---|---|---|
| `Body shape` | capsule + sphere, same | unchanged |
| Swing limit X/Z | ±34.4° | ±5.2° (twist ±0.52°) |
| Spring Rot | 5.0 (flat) | 280 / 280 / 153 (per joint) |
| Damping | 0.75 / 0.75 | 0.90 / 0.90 |
| Resting behaviour | fell onto the ±34° stops; 2.3 units of displacement; whip at load | micro-sag ≤ 4 cm, dead still at rest, follows the head |

## Validation performed vs what could not be validated here

- Validated in an offline replica of MMD's own physics (Bullet + the PMX's bytes, as described above).
- **Not performed:** opening the exported file in MikuMikuDance itself and watching it (this machine has MMD
  installed, but the project's verification pipeline is offline/scripted; final acceptance "opens correctly in MMD"
  stays a manual check for the user). The physics model, masses, inertia, constraints, groups, masks and the rest
  pose were all verified against an independent implementation; in-editor behaviour is the remaining step.

## Remaining limitations / notes

- The strands at the shoulder line (`outfit1.acc23 … acc45`, the face-framing lengths) start inside the generated
  upper-arm limb colliders in the authored pose. They are left meeting them deliberately: with the 5.2° envelope the
  capsule supports them instead of ejecting them. The rest-overlap exclusion for them let them sink ~5 cm *through*
  the shoulder line, which is worse. Deeper mesh-aware authoring is out of scope.
- The repo's `pmxPhysicsFindings` validator flags classes of pre-existing findings in this export that are **not**
  introduced by this change and not specific to hair: the jiggle anchor joints (zero spring by design) and
  garment/butt bodies that start inside their own resting colliders. The count went 52 → 56 across the re-export,
  all in the garment (cloth) class owned by the parallel session's rest-pose bake.
- A single small impulse at load (the touchdown onto the 5° envelope, ~0.1 s, tip speed up to ~8 units/s) exists at
  t ≈ 0 — the same thing every stock Miku joint does at load. It is settled by 0.25 s and never recurs.
- Hair still follows MikuMikuDance's world gravity; strands perfectly perpendicular to it can never be held at a
  large angle from gravity by any 120 Hz solver. The envelope keeps that motion tiny instead of absent or violent.
- **Co-work in the same file.** The garment (skirt / cape) physics in `src/export/PmxPhysics.cpp` and the cloth
  rest-pose bake in `src/assemble/Pose.cpp` / `PhysicsSim.*` are uncommitted work-in-progress from a parallel
  session in this working tree, included in every build used for validation. The hair fix is independent of them.
- Nothing was committed: the working tree is shared with that parallel session mid-task. The files that are entirely
  this fix are `src/assemble/Scene.h`, `tests/cardbridge_tests.cpp` (the three tests), and the hair sections of
  `src/export/PmxPhysics.cpp`; `src/assemble/Pose.cpp` carries a six-line hair exclusion on top of their WIP.
