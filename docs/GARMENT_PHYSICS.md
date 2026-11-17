# Garment physics for the PMX export — authored drape, stability, and secondary motion

**Status:** implemented and validated offline (see "Validation"); visual check in MikuMikuDance is the remaining step.
**Stress-test cards:** `KK_400633.png` (Tokisaki Kurumi), plus `KK_015975` (Sword-Maiden), `KK_397110` (Kimono), `KK_233518` (Atago), `KK_160423` (Satou Sakie), `KK_400577` (simple control), outfit slot 0.
**Companion to:** `docs/HAIR_PMX_PHYSICS.md` (the hair-side fix).

## Update 7 — the swing envelope is flat and modest again

The per-link widening (1.15 rad at the yoke easing to 0.40 at the hem) let the skirt peel open in deep hip-motion frames — the back
panel flung up and showed the undies. The envelope is back to a **flat 0.40 rad per link** with the one-sided stop and the leg
collision unchanged: the authored drape is held at rest, the skirt sways with the body, never flings open, and collision keeps the
thighs out of the cloth. If a deeper fold still needs a bigger dodge the per-link taper can be re-introduced per link rather than
raising the whole budget.

## Update 6 — garment joints carry zero centring spring

The flat 50 spring on every garment joint was retained from the reference cloth convention. It is now **zero**
(`kGarmentClothSpring = 0.0f`, like the reference cloth models' garment joints): the authored drape is held by the
one-sided stop and gravity alone, so there is no spring fighting the stop to feed a spring-vs-stop buzz during motion
(the back skirt panel was the most visible case in MMD). The silhouette is still carried by the envelope stops and the
bone-aligned (mode-2) bodies; hair keeps its spring; the audit now accepts a zero (but not a negative or NaN) rotation
spring, and the test expectation moved from flat 50 to zero.

## Update 5 — the skirt collides with the legs, and can fold out of their path

Two defects left the exported skirt unable to follow a leg:

1. **No cloth-vs-leg collision at all.** The contact-level exclusion dropped every skirt body that starts touching a collider
   (any drape authored to rest on the thighs is dropped), so the thigh passed straight through the front and side panels. The
   exclusion now applies only to bodies *buried deeper* than a rest allowance (`kRestContactAllow` = 0.12 model units, in step
   with the test suite's `kRestContactAllow`), so a panel resting on a hit sphere keeps its collision. Garment-vs-garment and
   hair stays at the contact level, and the rest allowance is in Model units; see `src/export/PmxPhysics.cpp`.
2. **A 23-degree flat swing budget.** The flat `kGarmentMaxBend` (0.40 rad) per link could not fold a panel out of a swinging
   thigh's path. The swing budget now **decreases from the hip yoke to the hem** (1.15 / 0.93 / 0.71 / 0.49 / 0.40 rad, like the
   reference cloth models' per-ring envelope); the twist axis stays nearly rigid (`kGarmentTwist`, ±0.15 rad); the hang side is
   still a zero stop at the authored angle, so the rest drape in the PMX is exactly the drape the viewer shows — only the motion
   budget changed, never the rest pose.

Measured in the offline replica (`tools/_mmdsim.cpp`, rebuilt against bullet, mode-2 bodies held to the bone): at rest every
skirt chain sits at the authored pose with 0.000 displacement; the exported masks include every hip/thigh collider group the
card ships; MikuMikuDance is the visual acceptance test, with the model already loaded for the user's leg-spread check.

## Update 4 — the garment hang side is a zero stop

The PMX's garment joints used to allow a small asymmetric droop on their hang side (`pin` = 0.04 + 0.22·(1 − stiffness)
rad, clamped to 4–13°). In a mode-2 (bone-aligned) chain nothing but that joint stop stands between a panel and MMD's
gravity, so whatever range the stop allows becomes droop. The hang side is now **zero range**: gravity cannot rotate a
panel out of its authored orientation, so the rest drape in the PMX is exactly the drape the viewer shows. The other
axes are untouched — the swing axis keeps the full `kGarmentMaxBend` (±0.40 rad) budget and the twist axis its ±0.04 —
so a step still moves the panels, on the axis that does not narrow the silhouette. No card-specific numbers: every
garment chain gets the same treatment. The centring spring was later set to zero — see Update 6; the per-link swing
budget is described in Update 5 (it replaced the flat `kGarmentMaxBend`).

**Tool fix that came out of this:** `tools/_mmdsim.cpp` negated the Z-axis joint spring for every joint. That is saba's
**PMD** rule; its **PMX** path (`MMDJoint::CreateJoint(const PMXJoint&, …)`) writes every spring unmodified. Applied to a
PMX export the negation turns every +Z centring spring into a destabilising one and drives bodies to their Z limit —
which is why an earlier reading showed the skirt settling 18° off the rest pose. With the flip removed, both the old and
the new export hold the skirt within ~1° of rest (5 s, saba's model), so this change is a guarantee against droop in any
implementation that takes the range, not the explanation of the mismatch still being reported in MikuMikuDance.

## Update 3 — "Original garment" is the default: the PMX carries the card's own drape

The garment-vs-body clipping correction (`FixGarmentIntersections`) used to run on every export. It rewrites garment
vertices that sit inside the skin, which can visibly change a tight garment's authored drape — and the viewer never ran
it, so the exported PMX could differ from the preview. It is now **off by default**: the export keeps the card's own
garment vertices, exactly the drape the viewer shows.

- **CLI:** `--fix-garment-intersections` turns it on. `--no-fix-garment-intersections` (now the default) and
  `CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=0` keep it off; `CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=1` turns it on.
- **Viewer:** Look tab → **Garments → Original garment** is ON by default. Turning it off runs the correction on the
  preview scene (a reload) and on the export after the pose bake, so the preview and the PMX carry the same corrected
  garment. The switch is a pipeline setting, not a card tweak: it is not saved as a card edit.
- **Idempotent:** a second pass on a corrected scene moves nothing. The preview corrects at rest; the export copy from
  the preview is corrected again after the pose bake, which only catches vertices the pose newly buried.

## Update 2 — the leg-follow append removed (the "straight-tube skirt")

After the asymmetric envelope landed, MikuMikuDance still rendered the Kurumi skirt as a narrow, pinned column that did not
use the card's drape at all — even with no VMD or pose applied, at rest pose, with physics off. Diagnosis, confirmed on the
export's own numbers (`A-pose` toggled off, FK recomputed from PMX bytes): the exported garment chain roots carried an
append rotation into the thigh bones (`SkirtBoneL01 → cf_j_thigh00_L` at 75%, `SkirtBoneL07` at 17%, `SkirtBoneM01` at 35%,
right side mirrored) — MegaMMD applies append-rotation formulas at rest too, so the standing pose started by folding every
panel root 15–25° inward, and MikuMikuDance then pinned it that way. Cardbridge's own preview ignored the append, so the
wide flared silhouette rendered perfectly there while the MMD-side file quietly collapsed it. Fix: `AddLegFollow` and the
`kLegFollow` constant are gone entirely. Garment chains now carry **no append rotation and never promote a bone to deform
layer 1**. Validation: regression test updated to assert zero appends and zero layer promotions; MikuMikuDance must be
checked on the next import.

## Systemic findings (garments, hair and accessories together)

After the skirt fix, severe jitter remained in *accessories* as well. Root-caused across six cards:

- **Duplicate physics chains** — a card can resolve the *same* DynamicBone item into two items; the identical chain was
  exported twice on the same bones (Sword-Maiden: the whole skirt twice — 8 identical chain bodies on the same bones, in
  collision with each other). **Fix:** identical bone-index sequences are exported once.
- **4-bit collision group overflow shared a group between chains — and every chain set its bit in every other overflow
  chain's mask**, including its own. Same-chain bodies then collided with themselves (their own links, depth 0 up to
  0.59 on layered panels). **Fix:** colliders are capped at group 13 (14 reserved for the garment overflow, 15 limbs), and
  *same group* everywhere is an exclusion (bodies vs bodies, colliders vs bodies, limb colliders vs bodies).
- **Long hair/ornament strands cannot be held by any spring the 120 Hz solver can integrate.** Horn/mane/long-tail
  chains demand 1e3..5e4 vs the bangs ~9e2; the export capped hair springs at 280, so those strands fell to their stops and
  trembled. **Fix:** a demand-based rule — a strand keeps mode 1 ("free dynamics") only while its gravity hold demand is
  under the measured holdable ceiling (~2000 MMD-units); heavier strands are exported bone-aligned (mode 2): the bone holds
  the authored pose exactly, joints keep their envelope, strand swings, solver sees a stable system.
- **Offline replica fixed again:** mode-2 bodies now pinned to their own rest position plus the cumulative shift of their
  kinematic ancestors (MMD's bone-aligned semantics), not to the simulated parent chain (which made them fan out
  nonsensically).
- **Honorable mentions, already in f974745:** garment hold spring; `BakeGarmentSettle` removed; ×1.6 mass removed;
  garment groups per chain; rest-overlap exclusion everywhere.

Result on the six cards (offline Bullet replica at MMD's 120 Hz): **zero rest-buried-but-colliding pairs on all six**
(was up to 206), Sword-Maiden worst-case chain from 12.0 to 2.6 peak units/s, Kurumi's standing-still peak from 9.84 to
3.43 (last-second peak 0.03), Atago to 0.0001 — all skirt/long-hair/ornament chains now hold the authored silhouette.

Remaining known artifacts (small bangs swing settling within ~0.25 units): honest small-strand secondary motion, not
instability, and not a bug.

## Update 2 — the leg-follow append removed (the "straight-tube skirt")

After the asymmetric envelope landed, MikuMikuDance still rendered the Kurumi skirt as a narrow, pinned column that did not
use the card's drape at all — even with no VMD or pose applied, at rest pose, with physics off. Diagnosis, confirmed on the
export's own numbers (A-Pose toggled off, FK recomputed from PMX bytes): the exported garment chain roots carried an
append rotation into the thigh bones (SkirtBoneL01 → cf_j_thigh00_L at 75%, SkirtBoneL07 at 17%, SkirtBoneM01 at 35%; mirrored
on the right side) — MikuMikuDance applies append-rotation rules at rest too, so the standing pose started by folding every
panel root 15–25° inward, and MikuMikuDance then held it there. Cardbridge's own preview ignored the append, so the wide
flared silhouette rendered perfectly there while the MMD-side file quietly collapsed it. Fix: `AddLegFollow` and the
`kLegFollow` constant are gone entirely. Garment chains now carry **no append rotation and never promote a bone to deform
layer 1**. Validation: regression test asserts zero appends across garment chains; MikuMikuDance must be checked on the
next import.

## The two problems this answers

1. **The exported PMX did not preserve the card's authored skirt silhouette.** A wide, flared Koikatsu skirt became a
   narrow, hanging cone in MMD — the panels draped toward vertical under MMD's world gravity instead of holding the
   flare the artist drew.
2. **Large skirts were physically unstable.** Continuous vibration, random twitching, overlapping/self-intersecting
   panels, chains colliding with neighbours' physics, and occasional violent displacement, worst on the Kurumi card.

## Investigation method

The offline replica of MikuMikuDance's physics (`tools/_mmdsim.cpp`, built on Bullet — the same engine MMD uses) was
used to simulate the exported PMX byte-for-byte with MMD's world gravity (`0, -98, 0`) and a 120 Hz solver, and to
sweep parameters on the actual Kurumi export. Single-joint and multi-joint probes isolated every mechanism before any
exporter change was made.

## Root causes found, in order of discovery

1. **The joints between dynamic bodies could not hold against world gravity — they fell past their own rotation
   limits.** The exported 6-DOF spring constraint (MikuMikuDance's Bullet `btGeneric6DofSpringConstraint`) has wide
   enough ranges and a centring spring on the paper, but the solver could not keep the chains inside those limits under
   gravity: the Kurumi skirt rotated **~6°/joint past a ±2.9° joint limit**, settling on the limit and shivering. A
   kinematic-root chain (breast chains) holds fine in the same solver; long dynamic-dynamic chains hanging off a
   kinematic root do not.
2. **`BakeGarmentSettle` baked MMD's gravity hang into the rest pose** (`src/assemble/Pose.cpp`, called only by the
   CLI, never by the viewer preview). That destroyed the authored silhouette before the file was written and is
   fundamentally incompatible with preserving it.
3. **Springs sized for "hold the rest pose against gravity" were far too stiff for the 120 Hz solver to integrate.**
   Empirically, a body of ~2 kg held near its rest pose only below a spring of ~60,000; at 238,000+ (what any
   "sized to hold the flare" formula produces) the chain oscillated and collapsed regardless. The spring stiffness did
   not differentiate the chains' resting behaviour at all — gravity dominated completely at the limits.
4. **Per-link masses were scaled ×1.6** for no physical reason, increasing the inertial torque the chains had to
   fight.
5. **Garment chains had no chain-vs-chain collision at all** — panels of a layered skirt could interpenetrate freely,
   and bodies overlapping at rest launched each other at load.
6. **Every garment body had a full spring on the Y (twist) axis with a tiny inertia** (~0.02 kg·m² for a link):
   `sqrt(K/I) ≈ 150 rad/s`, far above what the 120 Hz solver can track — another shiver source.

## The redesign

Two structural changes, both toward what the game itself does:

### A. Garment chains are exported **bone-aligned** (`physicsMode = 2`, "physics with bone aligned"), not free dynamics

MikuMikuDance's mode 0 rigid body (kinematic anchor on the bone) is kept for the chain root, but the links below it are
no longer free dynamics. In mode 2 the **bone holds the body at its authored place**; the body's rotation is simulated.
The chain stays exactly at the authored silhouette by construction — no falling, no collapsing, no launching — while
the cloth still swings around its joints under gravity, follows the animation with real inertia, and responds to
colliders. This is how the reference MMD models build capes, necklaces, ties and skirts made for their character, and
it is what Koikatsu's DynamicBone does for the skirt in the game (the skirt's resting silhouette is held by its
position spring, and the card's skirt stiffness for this outfit is `1.0` — essentially rigid in the game).

The hair chains keep mode 1 (free dynamics) on purpose — the authored hairstyle is their rest pose and the hair fix
in `docs/HAIR_PMX_PHYSICS.md` validated them in that mode.

### B. The joint spring, envelope, mass and damping are sized from the card's own physics, not from a global constant

- **Envelope** stays `2·asin(1−stiffness)` per link for ordinary strands (the card's own stiffness), capped at
  `kGarmentMaxBend = 0.40 rad`; garment chains use the per-link envelope of Update 5 instead.
  The Kurumi outfit has stiffness ~0.975 → a ±2.9° envelope per link; that is the card's own answer for a rigid skirt.
- **Hold spring** (the centring spring that pulls back toward the authored pose) is sized from the subchain torque it
  must hold: `K = lever·g / droop`, evaluated per joint per link length and mass, with
  `kGarmentHoldDroop = 0.005 rad`, `kGarmentHoldSpringMin = 50`, `kGarmentHoldSpringMax = 500000`.
- **Y (twist) spring** from the same formula (the total envelope is the card's; the twist stays the narrow slot).
- **Mass** is `len/scale·10` like everywhere else (the ×1.6 multiplier is gone).
- **Damping** stays in [0.85, 0.95].
- **Garment bodies get a dedicated collision group per chain**; same-chain bodies never collide (they are jointed),
  and chains that overlap at rest never meet either (layered panels). Everyone else meets. This uses groups 2..14,
  skipping the collider groups — when garment chains outnumber the free groups the extras share one group
  conservatively, never touching the body.

## Result

Exported (CLI) with the new code, the Kurumi export simulates to:

- **0.000 units displacement for every skirt chain; peak speed 0.00** — the authored silhouette is held exactly, with
  no load-time scatter, no shiver, no droop.
- Under a sideways sweep the chain follows the root with ~0.5–1.5° of weighted lag per link — subtle secondary motion
  instead of either rigidity or collapse.
- Chains inherit the card's own stiffness envelope: a stiff card stays close to authored, a loose card swings wide.

## Files changed

- `src/export/PmxPhysics.cpp` — garment chains export mode 2; per-joint hold spring; per-chain collision groups;
  overlapping chains excluded; drop the ×1.6 mass mult and `kGarmentMaxBend`-only behaviour.
- `src/export/PmxPhysics.h` — mode 2 / mode semantics comments record garment behaviour; leg-follow append removed.
- `src/assemble/Pose.cpp` / `Pose.h` / `cli/main.cpp` — `BakeGarmentSettle` removed entirely.
- `tests/cardbridge_tests.cpp` — hang-bake test rewritten as the authored-drape regression
  ("the exported garment keeps the authored drape"); mode-2 regression ("garment chain bodies are bone-aligned");
  validator and crosscheck updated for the per-chain groups and mode semantics.
- `tests/pmx_crosscheck.cpp` — same group/mask semantics for the independent reader.
- `tools/_mmdsim.cpp` (offline replica) — mode 2 bodies simulated correctly: the bone holds the body's position, the
  body keeps its simulated rotation.
- `README.md`, `docs/HAIR_PMX_PHYSICS.md` — cross-references updated.

## Remaining limitations

- **A flared silhouette that is not a gravity equilibrium cannot be held by MMD's 6-DOF impulse solver.** Bullet/saba
  cannot replicate Unity DynamicBone for a dress that "wants" to fall. The bone-aligned mode is what actually holds
  the silhouette; it limits the cloth to rotation around its joints. A Skirt that wants to flare dramatically while
  moving freely will always need either mode 2 ("bone-aligned") or a much stiffer card's own physics — that is the
  format constraint, not an export defect.
- **Layered panels that overlap at rest cannot collide with each other** — Bullet groups are 4-bit per body. The
  current export excludes the overlapping pair (same rule the game's colliders already use), which lets those panels
  pass through each other; other pairs still collide.
- **Per-joint envelope is still derived from the card's stiffness,** not from garment dimensions. A very long skirt
  on a high-stiffness card clips like a rigid cone; a very short loose one behaves like cloth. That matches the game,
  not the preview.
- **Hair (mode 1) is unchanged** — the authored hairstyle there is the rest pose and the hair fix in
  `docs/HAIR_PMX_PHYSICS.md` stands.
- **The MMD load-time touchdown impulse** (a brief initial settle onto the mode-2 joints) is inherent to importing
  any PMX; it settles within ~0.25 s and does not jitter.

## How to verify

1. `cardbridge convert <card> --outfit <N> --library <library_full> --mods <mods> --out <dir>`
2. Open the PMX in MikuMikuDance, play a walking motion clip, and check: the skirt keeps its authored silhouette at
   rest, swings subtly and settles smoothly, does not launch at load, and does not self-intersect the legs.
3. `"0.000"` displacement on the offline simulation is the rest check; `--shift` on the offline sim is the motion check.
