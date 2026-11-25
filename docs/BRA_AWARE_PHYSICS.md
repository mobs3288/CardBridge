# Bra-Aware Breast Physics

**Status:** implemented; baseline regression and synthetic classification tests added. Real-card PMX
validation and MMD playback validation are recorded at the end of this document.

## What it does

One export-time switch, **Bra-Aware Breast Physics**, is ON by default. When it is on and a *supportive
bra* is detected with high confidence, the exported PMX gets a firmer, better-damped, less-travelling
breast profile. In every other case — no bra, an ordinary shirt or undershirt, pasties, a one-sided
garment, unknown/modded gear, missing geometry, the switch off — the existing breast physics is used
exactly as before. There is no new "no bra" profile, and the original settings object is never mutated.

## Where it sits

```
card -> assembler -> Scene (parts / meshes / bones / jiggle chains)
                     |
                     +-- ExportScene() -- ClassifyGarmentSupport(scene)   [once, before LODs]
                     |                         |
                     |                    GarmentSupportResult (pure data, reused by every LOD)
                     |                         v
                     +-- ExportSceneImpl() -- AddPhysics(..., bustOverride)
                                                  |
                                     only characterOwn && bust chains use the override
```

* `src/assemble/GarmentSupport.h/.cpp` — region derivation from the character's own breast bones,
  multi-evidence classification, diagnostics. No PMX types; no scene mutation; no asset IO.
* `src/assemble/BraPhysicsProfile.h/.cpp` — `SupportedBraJiggle(base)` returns a NEW sanitized
  `JiggleSettings`. This is the only place the supported numbers are defined.
* `src/export/PmxPhysics.h/.cpp` — `AddPhysics` gained one optional `const JiggleSettings*
  bustOverride`. With `nullptr` (all existing callers) the function follows the exact old path. When
  set, only the body's own bust chains use it; hip chains, hair/garment chains, colliders, groups and
  masks are untouched, and no rigid bodies or joints are added or removed.
* `src/export/Exporter.h/.cpp` — `ExportOptions::braAwarePhysics` (default `true`) and the computed
  `braClassification`. The result is computed once on the un-simplified scene and copied into LOD
  exports. The manifest records the verdict, confidence, evidence and diagnostics under
  `jiggle.bra_aware`.
* CLI: `--no-bra-physics` or `CARDBRIDGE_NO_BRA_PHYSICS=1`. Viewer: Look → Jiggle → "Bra-Aware Breast
  Physics", persisted in the existing settings file; configurations without the key default to ON.

## Classification methodology

The classifier never decides from a name, slot, item id, mod identifier or texture name alone. Those
are, at most, a small positive weight; the mandatory evidence is geometry.

1. **Breast region** — `BuildBreastRegion` reads the current `cf_s_bust00_*`, `cf_d_bust01..03_*`,
   `cf_hit_bust02_*` and `cf_s_bnip*` bone positions. Both sides must exist. The two volumes follow
   ABMX and shape edits because they are measured from the posed rig, not from fixed coordinates.
   The waist bone gives the lower torso reference.
2. **Candidates** — exported, non-under-piece Outfit/Accessory parts whose geometry actually overlaps
   either breast volume. This includes modded garments in unfamiliar slots, and it excludes skirts,
   shoes, headwear and hidden/under-piece geometry.
3. **Evidence** — fraction of the garment inside the volumes, left/right balance, distance from its
   vertices to the body's own surface, vertical extent against the breast band and waist, skinning to
   bust bones, and ray coverage of the body's breast surface (using the project's existing
   `CoverageGrid`/`RayHitsTriangle`).
4. **Vetoes** — fully translucent/cut-out materials; one-sided or tiny coverage; less than 40% of
   each breast covered (pasties); torso-length garments extending below the waist; garments too loose
   against the bust. Each veto yields `NonSupportive` regardless of metadata.
5. **Verdict** — `Supportive` only when the weighted score is ≥ 0.72 **and** a geometry quorum holds
   (both sides covered, ≥45% of each breast, close fit, compact band). Otherwise `Unknown` or
   `NonSupportive`, both of which keep the baseline. `CARDBRIDGE_DEBUG_BRA=1` prints every evidence
   line to stderr.

## Supported profile

Derived from the project's own tuned "Subtle" preset, whose documented intent is a firm, well-damped,
sports-top-like fit — not from arbitrary per-parameter multipliers:

* mass and simulation rate unchanged;
* stiffness raised at least to the Subtle reference, never below the user's value;
* damping recomputed as `c = 2ζ√(km)` from the better of the user's and the reference damping ratios,
  capped below 0.70 so the result is settled but not rigid;
* vertical and depth amplitudes reduced more than lateral amplitude; rotation follow and master
  strength may only decrease; gravity may only decrease (MMD supplies its own world gravity, so this
  mainly affects the viewer preview and the manifest's derived numbers).

In PMX this becomes: higher rotational springs on the bust joints, higher body damping, tighter joint
envelopes (especially the vertical/depth "flop" axis), and correspondingly slightly tighter fascia
coordination joints. Bone hierarchy, names, rest pose, geometry, materials and unrelated physics are
unchanged.

## Platform coverage

KK and KKS share the assembly, Ver02 chain extraction and PMX physics path; the classifier reads
bone names, geometry and part metadata, none of which differ between the two for this feature. KKS's
4-outfit layout and extra list-row keys need no special case. Modded garments are handled through
the geometry evidence path; a mod that cannot be classified confidently is left on the baseline.

## Garment-aware collision — limitation

Garment geometry is **not** turned into extra rigid bodies. A bra is a skin-tight shell whose collider
would start buried in the bust at rest; MMD ejects buried bodies on frame 0, and per-garment
collision bodies would add size and a real risk of explosive instability. The supported profile is
delivered independently of this. Existing body/limb colliders and their masks are unchanged.

## Tests and validation

| Category | Status |
|---|---|
| Profile derivation / no mutation | unit test, passing |
| Override affects only bust chain | unit test, passing |
| Synthetic bra / shirt / pasties / missing-geometry classification | unit test, passing |
| Full unit suite | 19,540 checks, 0 failed |
| Format tests | 100 checks, 0 failed |
| Feature OFF / no bra / unknown garment: identical physics | real-card PMX dumps compared, identical |
| Real base-game bras (KK) | detected with confidence 0.94–0.99, supported profile applied |
| Ordinary tops / pasties / modded-but-missing bras | rejected, baseline kept |
| KKS card | detected (confidence 0.959), supported profile applied |
| PMX structure | `pmx roundtrip` byte-identical, reimport preserved, for both supported exports |
| MMD playback | **not performed** — MMD and the Bullet-based `mmdsim` tool are not available in this environment |

A real KK card wearing a base-game bra was exported with the switch off and on. The full rigid-body
and joint dumps differ in exactly 40 lines: the two breast root bodies' damping, the six breast link
joints' springs and envelopes, and the three left/right coordination joints' springs and envelopes.
All other bodies and joints — hips, hair, garments, colliders, limb colliders — are byte-identical.
A card whose bra is a missing mod (no geometry) produced identical physics dumps with the switch on
and off.

**Not claimed:** visual or physical correctness inside MikuMikuDance. The exported files are
structurally valid and the numbers are derived from the project's existing, tuned physics model, but
the final MMD acceptance test still belongs to the user.
