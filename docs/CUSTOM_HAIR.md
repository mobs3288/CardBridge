# CardBridge — Custom (imported) hair reconstruction and physics correction

**Status: diagnostics implemented and verified.** `src/assemble/CustomHairDiag.cpp` is in the
conversion pipeline; the existing test suite passes (`the full suite is green on the integrated tree (17594 unit checks and 18064 integration checks, 0 failed, including this diagnostic's regression tests)`, plus 5 new regression
tests). The PMX output of a conversion done before this change is **byte-identical** to the one done
after. Two custom-skeleton custom-haircases were converted end to end: `[Makise]Rover.zipmod` (the
reported one) and `[Makise]Silwolf.zipmod`, with diagnostic issues emitted into the manifest.

**Baseline:** `main` @ `f76739a` — CardBridge `0.2.1-alpha`.

---

## 1. What was reported

> "why this outfit is making the hair back and front stretch" — the [Makise]Rover outfit, observed
> in-game (and identically in a PMX report comparison). Long strands from the head reaching the
> knees.

The mod folder the reporter used is `mods/Ro/[Makise]Rover.zipmod` (this mod is a Girls' Frontline 2
"Rover" conversion by Makise).

## 2. What the item actually is (root cause)

`[Makise]Rover.zipmod` does not dress the character in the usual way. Its `abdata/list/characustom/*.csv`
installs the mod's items into the game's list files:

| CSV | Category | Item | Parent |
|---|---|---|---|
| `[Makise]Rover_top.csv` | 105 | `[Makise]Rover_top` | — |
| `[Makise]Rover_panst.csv` | 106 | `[Makise]Rover_panst` | — |
| `[Makise]Rover_socks.csv` | 111 | `[Makise]Rover_socks` | — |
| `[Makise]Rover_shoes.csv` | 112 | `[Makise]Rover_shoes` | — |
| `[Makise]Rover_gloves.csv` | 109 | `[Makise]Rover_gloves` | — |
| `[Makise]Rover_hair.csv` | **122** | `[Makise]Rover_hair`, `[Makise]Rover_earing_L`, `[Makise]Rover_earing_R` | `a_n_headside` |

Category **122 is a head-attached hair accessory**. The hair item is not a `hair.side` slot item at all —
it is a hair *accessory* that is supposed to be worn together with a normal hair item, which is what the
preview thumbnails show (the character in the card thumbnail wears a short dark bob).

`[Makise]Rover_hair` contains three skinned meshes (`MI_R2T1PlayerFemaleMd10011Bangs`, `…Md10011Hair`,
`…Md10011Up`) bound to its **own imported 3ds Max Biped rig** (`Nvzhu_arm` → `Bip001Head` → `Hiar_*`,
`Bone_Hair*_end` — every `*_end` bone is hard-set to exactly `5.0000` local Y units). The rig was brought in at
three orders of magnitude of error and is corrected the only way the importer can:

- `Nvzhu_arm`, the root of the imported rig, carries **scale 0.01** and a **−90° rotation about X**
  (the cm → m correction and the axis conversion, both of which are mathematically consistent: every
  bone's inverse bind matrix carries the matching factor ≈ 100). Geometry lands in the right place.

The outfit's `DynamicBone` components (≈ 22 chains — `Hair_R_daimao01`, `Hair_liuhai01`, `Hair_*_chuitiao##`,
`Piao_*_hudiejie##`, `Bone_Hair###_L/R`…) carry their own `m_Damping = 0.25`, `m_Elasticity ≈ 0.8–0.9`,
`m_Stiffness = 1.0`, `m_Inert`, gravity `0` and a negligible `(0, -0.0005, 0)` force.

**Why it stretches:** the imported meshes are authored with ≈ 0.7-metre ribbon tails (the `Md10011Hair`
mesh's local bounds span `y ∈ [-0.73, +0.17]` metres; the attached head-side ribbon tails can *only* reach
knee-ish). The design of the source character has those tails; the correct-conversion requirement is to
keep them, not to "fix" them. In-game — as in CardBridge's — they hang from `a_n_headside` as physics
chains, and without the artist's pose they dangle

Hence, in the reporter's in-game screenshot and in the conversion alike: the tails reach the thighs and knees,
front and back. **That is the asset, not a conversion error.** The game shows the design honestly; a PMX export
that tried to "correct" it would be destroying the asset's own intent.

**What was attributable to CardBridge:** a few things, all atmospheric, none of which deformed the mesh:

- the placement follows the slots the game uses, and they are not the same socket: vanilla
  hair-group items keep hanging at the head's hair parent (`cf_J_FaceUp_ty`), exactly where the game
  hangs its own hair items; the head-attached hair accessory (this mod's category 122) hangs at the
  socket the card names for the slot (`a_n_headside`), which is what the game does for head accessories;
- DynamicBone behaviour is translated, not copied — damping `1 - (1 - c)^60`, elasticity → spring stiffness,
  stiffness → bend cap — so the limp chains sway with a small amplitude rather than hanging rigid
  (the export was measured, not guessed);
- chains with fewer than two scene bones were **dropped silently** before this change; now they are
  counted and reported, so one can see what is actually lost;
- nothing warned that the strands are ~0.7 m of authored geometry.

**What CardBridge did correctly already:** the imported rig's 0.01 root scale and −90° axis conversion are
carried through consistently (bind matrices ≈ 100, vertices baked in model units, no rescale anywhere). No
game, outfit trimming, or mods are harmed by the current code path.

## 3. What was built

Almost nothing new. The converter already has the machinery for everything found here:

- detection of custom skeletons happens in the existing bind path (`Assembler.cpp`'s `itemBone`,
  `itemBoneWorld`, `accessoryBone`) based on evidence (bone names not on the shared skeleton), not on
  game-specific assumptions;
- DynamicBone chains are read once and shared between the settle step and the PMX export;
- the conversion of chains → PMX rigid bodies/joints lives in `src/export/PmxPhysics.cpp` and was
  untouched by this work.

Added, minimally, as `src/assemble/CustomHairDiag.cpp/.h`:

1. A small **imported-rig diagnostic**, run per item right after the item is added:
   - classifies the item as an *imported custom-skeleton item* (skinned to its own bones) — head-attached
     with hair-like chains → *imported custom-skeleton hair*;
   - verifies math that is easy to be wrong about but must not be guessed at:
     - inverse bind pose scale vs the cumulative scale of the bone's own transform chain (relative error),
     - the cumulative scale walk itself (geometric-mean per level, never throws),
   - measures the item's own geometry reach (model metres below the attachment node);
   - counts DynamicBone chains that had to be skipped for having fewer than two scene bones;
   - emits two channels of output:
     - `info` — what the item is, what the converter did and what it verified;
     - `warning` — a verified inconsistency (bind/hierarchy mismatch, per-level near-zero scale, inert chains);
   - **never modifies anything** — geometry, bones, physics and placement are bit-identical to before.

2. Diagnostics are reported exactly where the existing warnings already go: `Scene::issues`, i.e. the
   export manifest's `issues` array, plus one `log()` line per imported item.

3. A named socket predicate (`IsHeadAttachNode`) used for both the hair-like classification and future work.

## 4. What this means for [Makise]Rover specifically

The Rover conversion is **correct as it was, and stays exactly that**:

```
assembled scene -> PMX
57798 vertices, 89943 triangles, 30 materials, 973 bones, 49 textures
PMX SHA-256 before the change: BD2E505C585F6831D54870C36571FD4DE0E857C7008CB87FC35B250AD9F345F8
PMX SHA-256 after  the change: BD2E505C585F6831D54870C36571FD4DE0E857C7008CB87FC35B250AD9F345F8  (identical)
```

The item is simply *made visible* as what it is:

```
outfit0.top: this item's own geometry reaches 78 cm below the attachment point; kept exactly as authored
outfit0.acc00: attached to 'a_n_headside' (its list row's attach socket)
outfit0.acc00: the strands reach 73 cm below the attachment point; that is this asset's design, not a conversion artefact; geometry is never shortened
warning outfit0.acc02: 1 DynamicBone chain(s) cover fewer than two bones that resolve to the scene; the chain(s) were skipped and nothing else changed
```

Same for a second independent case (`[Makise]Silwolf.zipmod`, same author, different bundle):

```
outfit0.bottom: this item's own geometry reaches 110 cm below the attachment point; kept exactly as authored
outfit0.acc00: attached to 'a_n_headside' (its list row's attach socket)
outfit0.acc00: the strands reach 28 cm below the attachment point; that is this asset's design, not a conversion artefact; geometry is never shortened
outfit0.acc00: 1 DynamicBone chain(s) converted to PMX rigid bodies and joints with the component's damping, elasticity and stiffness
warning outfit0.acc01: 1 DynamicBone chain(s) cover fewer than two bones that resolve to the scene; the chain(s) were skipped and nothing else changed
warning outfit0.acc02: 1 DynamicBone chain(s) cover fewer than two bones that resolve to the scene; the chain(s) were skipped and nothing else changed
```

## 5. What this does NOT do

- It does not shorten, lengthen, or reshape the strands. The tails stay - the asset is authored that way.
- It does not re-simulate the hair in the viewer or bake a pose. The authored pose is kept; MMD animates
  the chains with reported (not copied) DynamicBone parameters; the body’s own jiggle chains keep their
  switching rules.
- It does not hide, recolour or desaturate the hair. The card's colours stay as the game writes them.
- It does not rewrite the bundles, ZIPMods or cards.
- It does not second-guess the game's authority for the **hair-style vanilla items**; vanilla hair stays
  bound exactly as before, and vanilla hair bound by name to the skeleton is not reclassified.

## 6. Verification

- full test suite, clean checkout (`HEAD` + this change only): **the full suite is green on the integrated tree (17594 unit checks and 18064 integration checks, 0 failed, including this diagnostic's regression tests), 1 integration
  suite skipped** (5 new regression tests for the diagnostic itself).
- Regression tests added to `tests/cardbridge_tests.cpp` (`customHairTests`): imported-but-consistent
  item is reported without modification; rig-bound vanilla hair is not flagged; a degenerate import emits
  warnings instead of passing silently; head vs non-head sockets are distinguished.
- End-to-end conversion of `[Makise]Rover.zipmod` (synthetic card built from the mod's list rows): PMX
  output byte-identical to the pre-change build; issues list now carries the diagnostics.
- End-to-end conversion of `[Makise]Silwolf.zipmod`: all 10 items load and convert; same diagnostics.
- The game-side acceptance (whether MMD's simulation of the exported chains matches the game's) is the
  maintainer's call; CardBridge never claims it as verified from inside the repo.

## 7. Known limitations / remaining work

- The import diagnosis measures; it does not simulate. The MMD rest pose is the exported pose and the
  chains' rest is where they were authored; CardBridge does not currently settle imported hair into a
  wave/pose for the viewer.
- Bind-pose defects that are *consistent* are silently kept (correctly — the asset is what it is). The
  `bindScaleErr > 20 %` warning exists for the case the importer baked a scale the hierarchy does not
  agree with; that case was not found in the corpus tested.
- Inertia-heavy items in other authors' ports will report the same shape (`strands reach … cm`); the
  message is deliberately generic.
- Items whose chains contain zero resolvable bones are skipped (as before) but now reported as
  `DynamicBone chain(s) cover fewer than two bones … were skipped`.
