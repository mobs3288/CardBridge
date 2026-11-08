# Stiletto Heel Compatibility — Remaining Limitations Report

CardBridge offline Koikatsu card → PMX, Stiletto v2.4.2 fidelity.

This report is the record section 8 of the follow-up task. It starts from commit
`d07313c` (pushed) plus the uncommitted shoe-warp work that was in the tree at
task start (`HeelConfig::ShoeWarpMatrix`, `Resolver::openModText` → `optional`,
one `cardbridge_tests` block), fixes that tree so it builds, and then closes
what can be closed with confidence. It does not claim full console parity:
anything that still differs from a live game is named in "Known deviations".

Baseline for this report: 692 checks (per the task). At task start the tree
claimed 712. Final: **1021 checks, 0 failed**.

## Original Stiletto behavior

Source: decompiled `Stiletto/HeelInfo.cs`, `HeelInfoAnimation.cs`,
`StilettoContext.cs`, `Configurations/DynamicFileProvider.cs`,
`DynamicProvider.cs`, `Models/CustomHeel.cs`, `Settings/FilePathSettings.cs`
(v2.4.2), cross-checked against the installed `BepInEx/Stiletto/*.txt` (50 files)
and the zipmods that ship one (`Augh.ApexKicks`, `Augh.HeavyBoots`, `Augh.Geta`).

- Config: `DynamicProvider<CustomHeel>` under
  `BepInEx/config/Stiletto/Heels/` (v2.4.2 `FilePathSettings.CUSTOM_HEEL_PATH`;
  older/legacy installs and every shipped zipmod surveyed here use
  `BepInEx/Stiletto/`). File stem = `ChaInfo.infoClothes[shoesType ? 8 : 7].Name`
  (trailing spaces preserved). `DynamicFileProvider.Read()`:
  missing file → `new T()` (all zeros); each `key=value` line is `Trim()`ed,
  keys exact/case-sensitive, values `TryParse`d (failure leaves the default),
  duplicates last-wins, unknown keys ignored.
- `HeelInfo.Setup` folds only ankle/toe/leg through `GetAngleInRange`
  (`AngleRange = 60`, epsilon `0.001`): `angle > 60.001 → -(360 - angle)`.
  `shoeAngle` and the two shear values stay raw. Zero scale → `1`.
- Pose (`HeelInfo.PostUpdate` → `HeelInfoAnimation.Update`):
  `_body.localPosition = height`, then `localRotation *= ankle` on
  `cf_j_foot_L/R`, `*= toe` on `cf_j_toes_L/R`, `*= leg` on `cf_j_leg03_L/R`
  (`_lowLeg`). Height/angles are gated by `flags.ACTIVE` and the animation
  flags; CardBridge works offline without an animator, so it bakes the same
  three rotations plus the lift as a rest pose. `HeelInfoAnimation` also has
  `CUSTOM_POSE` / `KNEE_BEND` / FullBodyBiped paths — CardBridge does not
  reproduce those (no animator state offline; see Known deviations).
- Shoe mesh warp (`HeelInfo.UpdateShoeWarp`, only when `_shoeDirty`):
  iterates the worn shoe's `SkinnedMeshRenderer`s, and for bones named
  `cf_j_foot_L/R` only:

  ```csharp
  val5 = TRS(0, Euler(-ankle - leg, 0, 0), 1)
  val6 = Translate(shoeTranslate)          // (0, y, z)
  val7 = Scale(shoeScale)
  identity.m12 = tan(pi/180 * ShoeShearY); identity.m21 = tan(pi/180 * ShoeShearZ)
  val8 = TRS(0, Euler(ankle + leg + shoeAngle, 0, 0), 1)
  bindposes[i] = val5 * identity * val6 * val7 * val8 * bindposes[i]
  ```

  Base binds are cached in `StilettoContext._baseShoeBindPoses` keyed by
  renderer+bone+heelName, so repeated calls do not accumulate. Heap of
  evidence: this is why CardBridge bakes the warp once into
  `SceneVertex::local` (bind space) at `addRenderer` time — the same
  non-accumulating property, without a process-wide cache.
- Transformed/inactive case: when the shoe slot is hidden (`clothesState`
  non-zero) `UpdateShoeWarp` uses scale 1, angle 0, translate 0, but **still**
  computes the `R … R` wrap from the current ankle/leg values. CardBridge
  bakes the warp regardless of preview visibility (visibility gates only the
  pose, via `settleHeels`); see Known deviations.

## Reconstructed transformation mathematics

Math lives in `src/assemble/HeelConfig.cpp` (`ShoeWarpMatrix`,
`ShoeWarpIsIdentity`). CardBridge `Mat4` is row-major `p' = M·p`;
`operator*` applies the right operand first. The C# product
`R1 · Shear · T · S · R2 · BindOriginal` is therefore built as
`r1 * shear * trans * scale * r2` in that order — application order
`R2 → S → T → Shear → R1`, exactly as in the game.

- `R1 = Euler(-ankle - leg)`, `R2 = Euler(ankle + leg + shoeAngle)`, pure
  X-rotations. Built via `Mat4::TRS` with quaternion `(sin h,0,0,cos h)`,
  `h = deg·π/360` — identical to `shape::EulerToQuat` on a single X axis
  (`qY·qX·qZ` with `qY=qZ=id`), so the sign matches `settleHeels::rot`:
  positive points `+Z` (toes) down. When the warp fields are identity the
  two rotations cancel (`R(-a)·R(a)=I`), which is why shipped data
  (all-identity) reproduces the old output bit-for-bit.
- Shear: Unity `Matrix4x4` is column-major; `MultiplyVector` is
  `y = m10·x + m11·y + m12·z …`, `z = m20·x + m21·y + m22·z …`, so `m12`
  is the Y-from-Z coefficient and `m21` the Z-from-Y coefficient (verified
  against `UnityCsReference/Runtime/Export/Math/Matrix4x4.cs`). Mapping:
  `ShearY → y += tan(ShearY·π/180)·z` (CardBridge `m[6]`),
  `ShearZ → z += tan(ShearZ·π/180)·y` (CardBridge `m[9]`).
- Translation is `(0, shoeTranslateY, shoeTranslateZ)` metres in scene units,
  scale is `(shoeScaleX, shoeScaleY, shoeScaleZ)` with `0 → 1` at parse time.
- No PMX append/IK is added. `IKCorrect`/`FrameCorrect` do not drive
  `cf_s/cf_d/cf_j` leg bones (Assembly-CSharp verified in the earlier
  session); `cf_s_leg02/03` follow by hierarchy only, as in the game.

## Coordinate-space analysis

- **Sh shoe warp** operates in the skinning stage on the bind pose:
  `newBind = warp · oldBind`, `newSkinMatrix = boneWorld · newBind`,
  `position = Σ w · skinMatrix · bindSpaceVertex`. CardBridge applies
  `warpMat * bindPoses[i]` for `cf_j_foot_L/R` only, in both skinned
  branches of `addRenderer`, before `SceneVertex::local/nlocal` are
  captured. Normals go through the linear part (`boneMat.dir`), so
  non-uniform scale/shear affect normals exactly as positions do, then are
  renormalised.
- **Heel pose** operates after that, on the rest skeleton:
  leg about `cf_j_leg03`, ankle about the (already leg-moved) `cf_j_foot`,
  toe about the (already leg+ankle-moved) `cf_j_toes`, then a global lift
  `lowestVertex.y → 0`. Vertices use the same three pivots blended by
  `wl/wf/wt` (leg03 / foot-subtree / toes-subtree weights); colliders and
  the foot bone position follow the same chain. Because the warp is already
  inside `local`, `Reshape` (which rebuilds flat bones from the rig and
  vertices from `local`, then re-runs `settleHeels`) cannot double-apply it.
- PMX export is unchanged: warped bind data reaches PMX as baked rest
  positions + bone weights. No new PMX kinds are introduced.

## Implemented changes

### 1. Shoe warp (was: parsed, warned, ignored)

- `HeelConfig.{h,cpp}`: `ShoeWarpMatrix` / `ShoeWarpIsIdentity`; `shoeAngle`
  and both shears no longer range-folded (raw per `Setup`); non-finite values
  rejected at parse; BOM/`\r`/`\n`/duplicate/trim handling (duplicates
  last-wins, as in the original).
- `Assembler.cpp` `addRenderer`: warp baked into foot binds (both
  `accessoryMode` and retarget branches), guarded by `isfinite` on the
  matrix so NaN/inf never reaches PMX. `settleHeels` now reports warp as
  baked (was "parsed but not baked").
- Tests: identity/uniform/non-uniform scale, rotation, translation, both
  shears, combined S+R2, ankle-cancels, skinned-deformation identity,
  double-application differs (proves single-bake matters).

### 2. Geometry toe fallback (unchanged math, better evidence)

`toe = -ankle` is kept. Investigation did not find a reliable geometry-only
way to distinguish high heel / platform / wedge / pointed / asymmetric
footwear (platform lift without pitch is invisible to the stack-lean
estimator; pointed vs conventional share the same rear stack). Rather than
invent a heuristic, CardBridge keeps the flat-forefoot assumption, adds
confidence/diagnostics (shoe-vert count + lean-vs-wedge path in the pose
warning), and documents the categories it cannot separate. Flat shoes and
barefoot are covered by tests (see below).

### 3. Extreme-angle stability

- `HeelAngleInRange` boundaries exercised at 0/60/60.001/61/90/180/270/360
  and negative angles (negative never folds — original behaviour).
- Warp finiteness exercised at 0/60/180/270/360/720; shear pole (90°) is
  huge-but-finite. Parser rejects non-finite input; `addRenderer` drops a
  non-finite warp; `settleHeels` skips non-finite candidates; a config with
  `350°` (folds to `-10°`) stays finite end to end and passes
  `ValidateScene`. No arbitrary clamping was added.

### 4. Provenance and diagnostics

- `Resolver::openModText` now returns `optional<Bytes>`: missing → `nullopt`,
  present-but-empty/present-with-no-heel-keys → explicit flat (suppresses
  known table and geometry, matching `new T()`), present-and-parsable →
  normal config. Lookup tries the v2.4.2 path
  `BepInEx/config/Stiletto/Heels/<Name>.txt` first, then the legacy
  `BepInEx/Stiletto/<Name>.txt` (every surveyed zipmod uses legacy).
- Lookup is case-insensitive ASCII, preserves trailing spaces and unicode
  byte-for-byte, duplicates resolve to the first entry. One warning per
  present-but-unparsed non-empty file names the path, shoe slot and zipmod
  GUID; the common cases (missing → known/geometry, empty → flat) need no
  warning and stay quiet.
- `ScenePart::heelConfigSource` now records e.g.
  `zipmod <guid> (Stiletto)`, `zipmod <guid> (config/Stiletto/Heels)`,
  `zipmod <guid> (Stiletto flat: …)`, `known <Name>`; the manifest adds a
  structured `heel` block per shoe part (list name, source, angles, height,
  full warp tuple). Pose warnings include config source/part id, and for
  geometry the vert count and lean-vs-wedge path.

### Reshape idempotency

`Reshape` remains idempotent because it restores flat bones/vertices from
the rig + stored `local` before `settleHeels`. Direct repeated
`SettleHeels` without `Reshape` is *not* idempotent for config-driven pose
(the second call re-reads fixed angles); geometry-driven pose is
self-cancelling after the first call (aligned sole re-reads as height 0).
The viewer and CLI always go through `Assemble`/`Reshape`, so this is not a
user-visible path; it is documented, not "fixed" by caching.

## Remaining limitations

- **No runtime animation/CUSTOM_POSE/KNEE_BEND/FBBIK.** Offline rest-pose
  baking cannot reproduce per-animation foot roll or IK body-length
  compensation. Only the worn-slot static heel (ankle/leg/toe/height) is
  approximated.
- **Geometry fallback is prior, not evidence.** Without config, platforms,
  wedges and unusual/pointed footwear cannot be reliably separated from
  geometry alone (see §2). `toe = -ankle` (forefoot flat) is the documented
  assumption; Stiletto's config default is `toe = 0`.
- **Warp on hidden shoes.** The game still warps binds when inactive
  (with identity scale/angle/translate). CardBridge bakes the worn warp
  unconditionally and gates only the pose by visibility.
- **Single pitch per foot.** If both shoe slots are heeled, the taller wins
  (the worn outer is taller in practice). The game poses one worn slot
  (`shoesType ? 8 : 7`); CardBridge's visibility-aware per-part choice is
  the offline equivalent, not a literal port.
- **50° cap and thresholds.** Geometry lean is capped at 50°; wedge path
  takes over above 35°; <2° lean and <4mm drop are treated as flat. These
  remain heuristics, disclosed in the code comments.
- **Zip-entry duplicates / case folding.** Duplicate zip entries resolve to
  the first match; case folding is ASCII-only. Non-UTF-8 zip name encodings
  are not transcoded (bytes compared as stored).

## Test results

Full unit suite (`cardbridge_tests.exe`): **1021 checks, 0 failed**,
1 integration suite skipped (no `CARDBRIDGE_TEST_*` environment — same as
baseline).

New/strengthened coverage (section 7 mapping):

| Area | Tests |
|---|---|
| Warp transforms | identity, uniform, non-uniform, rotation, translation, shear Y/Z, combined, skinned-deformation identity, double ≠ single |
| Pose fixtures | flat geometry (existing), heeled geometry (existing), barefoot, height-only platform, flat-config suppression, hidden-shoe ignored, inner-flat + outer-heeled, extreme-config |
| Angles | all Range boundaries, negative/U-turn angles, warp finiteness, shear pole |
| Parser | BOM, trim, duplicates, malformed ignored, non-finite rejected, `shoeAngle`/`shoeShear` unfolder |
| Known table | 50 entries round-trip (existing) |

## Golden fixtures

| Card | Path | Result |
|---|---|---|
| Rin (`srTohsakaRin`) | `Koikatu_F_20260718155357521_Rin.png` | geometry, 4.3 cm, 8.1° (453 verts, stack lean). PMX SHA256 `53d5d8ef79c30694…` — **unchanged vs pre-warp build** (identity warp proof). Feet/preview PNGs in `out/stiletto_rin_new`. |
| c395 | `KK_395305.png` | outer geometry 2.3 cm at 50° cap; inner `上履き` known flat ignored. MMD audit no ERROR. |
| Sakura | `KK_138747.png` | inner+outer `ローファー` known flat, no pose. MMD audit no ERROR. |
| Geta | `KK_287959.png` outfit 2 | `zipmod Augh.Geta`, 3.0 cm height-only, 0° (from `out/test_geta287`; re-export timed out, kept as golden). |

MMD audit (`tools/audit_pmx_for_mmd.py`) on Rin/c395/Sakura: no ERROR
(only the pre-existing Shift-JIS bone-name WARN). Before/after feet images:
`out/rin_flat/Rin_feet.png` (flat) vs `out/stiletto_rin_new/Rin_feet.png`
(posed) — numeric assertions above are the primary evidence; screenshots
alone are not claimed as proof.

Clothes-card workflow still targets the shown outfit
(`viewer/main.cpp:1177` `opt.outfit = shown`); unaffected by this work.

## Known deviations from original runtime behavior

1. **Missing config → geometry (CardBridge-only).** In game, a shoe with no
   Stiletto file gets all-zero (flat). CardBridge falls through to geometry
   estimation unless a file is present-but-flat or the known table knows
   the shoe. This is intentional offline behaviour; known-table flat shoes
   behave identically to the game.
2. **Rest-pose baking vs per-frame animator multiplication.** CardBridge
   bakes once; the game re-applies per frame after the animator.
3. **No `CUSTOM_POSE`/`KNEE_BEND`/FullBodyBipedIK**, no per-animation flags.
4. **Warp visibility** (see Remaining limitations).
5. **Non-finite config values rejected** (game's `TryParse` would accept
   e.g. `inf`); CardBridge prefers flat/skip over NaN in PMX.
