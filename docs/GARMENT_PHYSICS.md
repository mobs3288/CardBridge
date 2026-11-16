# Garment physics for the PMX export — authored drape, stability, and secondary motion

**Status:** implemented and validated offline (see "Validation"); visual check in MikuMikuDance is the remaining step.
**Stress-test cards:** `KK_400633.png` (Tokisaki Kurumi), plus `KK_015975` (Sword-Maiden), `KK_397110` (Kimono), `KK_233518` (Atago), `KK_160423` (Satou Sakie), `KK_400577` (simple control), outfit slot 0.
**Companion to:** `docs/HAIR_PMX_PHYSICS.md` (the hair-side fix).

## Update — the systemic pass (garments, hair and accessories together)

After the skirt fix, severe jitter remained in *accessories* as well. Root-caused across six cards; see "Systemic findings" below.

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

- **Envelope** stays `2·asin(1−stiffness)` per link (the card's own stiffness), capped at `kGarmentMaxBend = 0.40 rad`.
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
- `src/export/PmxPhysics.h` — `AddLegFollow` comment corrected (garments are not "locked"); modes documented.
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
