# Bust physics for the PMX export — one connected structure, two deformable sides

**Status:** fixed in this change. Both demo cards (Matou_Sakura, Rurikawa_Tsubaki — different bust sizes and shapes)
were exported before and after; the exported PMXs were simulated with an offline replica of MikuMikuDance's physics
(Bullet — the same engine MMD uses, 120 Hz, world gravity `0,-98,0`, mode-2 bodies position-pinned to their bones
exactly like saba does it) and compared. MikuMikuDance itself is the visual acceptance test — the viewer previews
the same physics numbers (see WYSIWYG parity below), but the PMX must be checked in MMD itself before release.

## The symptom

The exported character's bust moved as two unrelated objects: left and right breasts oscillated out of phase
(one flopped while the other lagged), ended up wedged tens of degrees apart after any big motion, and looked
disconnected from each other and from the chest.

## Root causes (in order of discovery)

1. **The two chains shared nothing.** Each side was an independent solve island hanging off the chest: its own
   kinematic anchor on the parent bone (`cf_s_bust00_L/R`, both at the chest centre), its own mode-2 bodies
   (bone-aligned, rotation simulated), its own joints. Nothing exchanged force between the sides at all.
2. **The two sides were deliberately de-phased.** The export hard-coded a 2% mass skew (0.98/1.02) "so they never
   move as one rigid pair", and the viewer sim skewed stiffness (×1.03), damping (×0.98) and amplitude (×1.02) per
   side the same way. A 2% skew is a guaranteed slowly growing phase difference — under repeated forcing (walking,
   jumping) the two sides drift into opposite-phase oscillation. This is what made the pair read as disconnected
   rather than as one soft, symmetric structure.
3. **Independent phase accumulates without bound.** Even with identical parameters, the MMD-class 6-DOF solver is
   not exactly mirror-symmetric (cross-product parity), so the two sides de-phase under any violent transient and
   there was nothing to pull them back to coherence.

## What was ruled out

- **Collision groups / masks** — the bust bodies meet only the generated limb colliders; nothing was pushing them apart.
- **Spring/envelope magnitudes** — the joint springs (3.3 N·m/rad) and the ±34.4°/±30.9°/±6.9° envelope were not the
  problem; stiffening them made the solver noisier without improving coherence (verified by parameter sweeps).
- **Mass distribution** — the 1/2/4/8 taper is correct; only the inter-side skew was wrong.
- **A shared kinematic chest anchor** — merging the two anchors into one body on `cf_d_bust00` is possible but
  pointless: both anchors already sit at the chest centre and the rigid anchor joints carry the root; replacing
  two kinematic bodies with one would touch the body tree for no measurable physics change. Kept as two.

## The fix

### PMX export (`src/export/PmxPhysics.cpp`)

- **Identical physical parameters for both sides of a bust/hip pair.** The artificial inter-side mass skew is gone.
  Symmetric forcing now produces mirror-symmetric response to floating-point precision (test: "jiggle: symmetric
  forcing moves the two sides alike").
- **A soft coordination joint per corresponding pair of rotating links** of the character's own bust/hip chains
  (one per link depth, e.g. `bust coordination 1..3` for a 4-bone chain). It is a plain 6-DOF spring joint, no new
  rigid bodies, and it is the fascia of the chest wall made explicit — the thing that stops one breast flopping
  away from the other:
  - a gentle centring spring on the relative rotation, a share (`kCoordSpringShare` = 0.25) of the link's own hold
    spring, tapering toward the tip (`kCoordTaper` = 0.75, per extra link depth) — the base coordinates, the tip
    stays freest;
  - a hard envelope at `kCoordRangeShare` = 0.75 of the link's own envelope (bust: ±25.8° flop / ±23.2° sway /
    ±5.2° twist), wide enough for real independent play, tight enough to bound opposite-direction bouncing;
  - **common motion costs nothing**: both bodies turning together keeps the relative rotation at zero, so walking,
    jumping and stopping are answered coherently exactly as before — the coupling only ever damps the *differential*
    mode;
  - authored on the left link at the rest frame (like every other joint in the export), locked on translation
    (the mode-2 bodies are pinned to their bones anyway; the rest error is zero in every frame);
  - the chest-locked root is deliberately not coupled (it cannot rotate relative to the chest at all).
- The coordination joints are named `<chain> coordination <k>` (ASCII class prefix, `bust`/`hip`), are appended
  after all chain joints, and are validated by the scene exporter's own physics checks and the joint-count mirror
  in the integration test.

### Viewer preview (`src/assemble/PhysicsSim.cpp`) — WYSIWYG parity

The same mechanism in the live preview, so the Look-tab jiggle preview shows what MMD will play:

- The side skews (×1.03 stiffness / ×0.98 damping / ×1.02 amplitude) are removed; both sides share the exact
  settings, as in the PMX.
- Corresponding links of the left/right pair are coupled by a soft spring pulling their lags
  (`pos − target`) together — common motion is invisible to it, divergence is resisted — with the same share
  (0.25) and the same per-link taper (0.75). A non-negligible pull wakes a sleeping sibling (a one-sided bone
  edit in the editor drags the other side along, exactly as the fascia does).

### What was deliberately not done

- No new bones, no morphs-as-physics, no forced synchronization, no change to rest pose, hierarchy, colliders,
  masks, groups or the hold springs.
- The butt chains get the same treatment (they are the same code path and can benefit); a card with a one-sided
  butt chain simply gets no partner, no joint.

## Measurements (before → after, both demo cards)

Offline replica, MMD's world gravity, 120 Hz, 4 s window, per-link left/right divergence (the angle between the
two sides' corresponding breast bodies, the quantity that reads as "disconnected"):

| Scenario | card 1 before | card 1 after | card 2 before | card 2 after |
|---|---|---|---|---|
| rest (3 s) | 0.50° | 1.2° | 1.55° | 1.65° |
| sudden stop, 12 cm sideways | 62.4° | 44.7° | 63.4° | 34.6° |
| sudden stop, 12 cm forward | 49.7° | 17.7° | 65.9° | 23.7° |
| hop, 2.5 units up | 7.7° (chaotic outlier) | 26.4° | 74.1° | 21.2° |
| torso twist, 18° yaw | 67.8° → stuck | 44.3° | 59.1° → stuck | 40.3° |
| torso lean, 10° roll | 23.6° | 10.7° | 16.7° | 9.8° |
| mean, 5 motion scenarios | 42.2° | 29.2° | 55.8° | 25.9° |

Two honest caveats on the numbers:

- **Peak values during violent transients are chaotic solver noise** — the hop case swings either way depending on
  the run order; the pre-fix 7.7° was luck, not coherence. The meaningful, repeatable effects are: divergence is
  now hard-bounded by the fascia envelope, the pair settles back to within ~2° of each other in well under a second
  (was tens of degrees wedged forever after a twist), and the mean divergence across scenarios is 31–54% lower.
- **Rest bias 0.5° → ~1.7–2.2°** on card 1. The two breasts rest ~2° apart in their flop axis — the authored rest
  orientations differ by 0.4° and the 6-DOF solver amplifies that into a static 2° mismatch (the pre-fix skew
  masked it by accident). It is sub-degree at the skin, stable, and not a functional problem.

In MikuMikuDance physics semantics (`mmdsim`, the project's offline replica of the shipped solver, used by the
author for every physics decision): the bust chains' displacement after load dropped (card 1: 0.076 → 0.026 and
0.076 → 0.095, peak 1.5 → 0.8 and 1.4). The remaining ">0.15 displacement" chains are the pre-existing garment
chains, unchanged by this fix.

## The known, pre-existing limitation this fix does not solve

**Mode-2 bodies can wedge on the twist axis of their 6-DOF joints and never come back.** After a large torso
rotation the two breasts rest tens of degrees apart (card 1 yaw: ~39° after this fix, ~60-68° before), with the
twist envelope at ±6.9° per the authored anti-spin guard. The 6-DOF solver does not enforce (or does not correctly
report) that limit once the body is past it, and the spring cannot win against a hard stop wedge — permanently.
This is inherent to "bone-aligned (mode 2) bodies + small twist envelope in MMD's 6-DOF", not to the coordination
design, and it affects the shipped export identically. This fix halves the wedge and bounds it between the two
sides; eliminating it entirely would mean widening the twist envelope (deliberately tuned by the author) or moving
the attachment model — a separate decision, not a hidden one. **Validate in real MMD before release** — that is
the project's acceptance test for physics.

## Validation

- Build: `cmake --build build --target cardbridge cardbridge_tests cardbridge_viewer` (clean).
- Unit tests: `cardbridge_tests.exe` — 17,622 checks, 0 failed (incl. new tests: coordination joints are emitted
  per corresponding link; left links are coupled to right links; the chest-locked root is not; an unpaired chain
  exports none; identical parameters on both sides; mirror-symmetric forcing moves the sides alike; the unforced
  side is dragged only subtly by the fascia during a one-sided edit; the joint-count integration mirror updated).
- Integration test (`CARDBRIDGE_TEST_*` set, demo_card1 against library_full): export + independent read-back pass;
  physics validator clean (no more "joint far from its bodies" findings from the coordination joints);
  all remaining failures are the missing zipmod library in this machine's empty mods folder (118× `mi.find` for the
  installed-game resources), unrelated and pre-existing.
- `pmx_crosscheck` (the engine's own PMX loader, no shared code with our writer): identical output before and after
  (the 2 findings — zero-spring rigid anchor joints — are the pre-existing intentional design).
- `mmdsim` on both cards: no new instability; bust chains strictly better.
- `tools/_mmdsim.cpp`-style replica (`breastcmp`, built on the same Bullet): per-link left/right divergence before
  and after — see the table above.
- Diff of the two exports: only the bust mass/damping (identical on both sides) and the +3 coordination joints
  changed — no bones, morphs, materials, colliders, masks or other joints.

## Files

- `src/export/PmxPhysics.cpp` — mass skew removed; coordination-joint collection during chain export; the
  left/right pairing and emission pass; the fascia-envelope and spring sizing (`kCoordSpringShare` 0.25,
  `kCoordRangeShare` 0.75, `kCoordTaper` 0.75).
- `src/export/PmxPhysics.h` — same notes on `AddPhysics`.
- `src/assemble/PhysicsSim.cpp` / `PhysicsSim.h` — symmetric parameters (side skews removed); `jigglePairs_`
  built in Reset; the paired lag spring (substep-snapshot, symmetric, wakes a sleeping sibling).
- `tests/cardbridge_tests.cpp` — side-asymmetry test rewritten to "symmetric physics"; new coordination tests;
  integration-test joint count mirrored; the two-chain "one side doesn't jump in a single update" test updated to
  the honest fascia behavior; "symmetric forcing, mirror-symmetric response".

## Remaining obligations for the author

1. Play a motion in real MikuMikuDance (idle / walk / run / jump / sharp stop / torso twist / lean) and confirm the
   breasts read as one connected structure with independent soft-body motion — the MMD import is the acceptance test.
2. Watch for the twist wedge after big torso turns (see the limitation above); if it bothers, the twist envelope is
   the knob — but that is an author's tuning decision, not something to change silently.
3. The viewer's live jiggle preview now shows the same coordination; eyeball it on Look tab with a jump and a stop.
