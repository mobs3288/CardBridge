# Regression investigation — KKS rear hair & pantyhose/stockings (2026-11-22)

Reference card: `C:\Users\pepsi\Downloads\KK_176361.png` (Kojima Kana — a **KK** card, not KKS;
it is the card in the report, so it is the source of truth here).
Screenshots: `Screenshot 2026-11-22 213631.png` (pantyhose), `Screenshot 2026-11-22 213647.png` (rear hair).

All timestamps below are UTC+07:00 (the author's local time), the timezone used by the commits.

---

## A. Repository State

| Item | Value |
|---|---|
| Branch | `main` (tracking `origin/main`) |
| HEAD | `473b0ea7bd12b1583f4a7644520776c37f95d6ad` — *Revert alpha-cutout rule: it punched holes in capes without removing black bands* |
| Uncommitted | `M VERSION` (working tree `0.7.6`, HEAD `0.7.4`). Untracked: `graphify-out/` and ~40 `tools/_*.cpp` diagnostic probes from earlier sessions. **All preserved; nothing was reset, discarded or switched.** |
| Other branches | `release/v0.3.0`, `ui-redesign`, `feature/material-workbench`, `backup/option3-pre-collider-fix` — none contains later transparency work. Tags: `v0.2.1-alpha`, `v0.2.0-alpha`, `v0.1.0-alpha`, `v0.5.0-alpha`. |

### Critical timeline correlation

| Time (UTC+7) | Event |
|---|---|
| 2026-11-22 16:13 | `0808444` (0.6.7) *Fix MMD pantyhose transparency: binary cutout alpha for alpha-test shaders* |
| 2026-11-22 20:58 | `abdf32b` (0.7.4) *Fix black bands on alpha-test garments in exported PMX* — introduced the "bake-evidence cutout" (`shaderClipsAlpha`) rule |
| 2026-11-22 21:31–21:35 | `build/bin/cardbridge.exe` (21:31:58), `cardbridge_tests.exe` (21:33:36), `cardbridge_viewer.exe` (21:34:59) rebuilt — **source state = `abdf32b`** |
| **2026-11-22 21:36:31 / 21:36:47** | **the two screenshots were taken** — from that `abdf32b` build |
| 2026-11-22 21:42 | `473b0ea` (HEAD) reverted the `abdf32b` cutout rule |

**The screenshots predate HEAD by six minutes.** They show the `abdf32b` state, not the current source.
The working-tree sources are already at HEAD (`473b0ea`); only the stale binaries reproduce the screenshot.

---

## B. Pantyhose / Stockings Regression

### Confirmed historical status
Confirmed regression, with an important nuance: **two separate changes affected this material**, and the
one responsible for the large ragged holes in the screenshot was already reverted at HEAD.

### The material in the supplied card (structural data, `_matinspect`)

```
mat cf_m_panst_01  shader=Shader Forge/main_alpha  queue=3040  ZWrite=1
                   alpha=1  translucent=0  _Cutoff=0.50
                   tex=cf_m_clothesN_create_result 1024x1024
                   A0=470  A255=0  Amid=1,048,106  avgmid=181.3
```

Essentially every texel is alpha ≈ 181/255 (0.71): a **uniform tint**, not a cutout — only 470 of
1,048,576 texels are alpha 0. After the lit UV-flat bake, the covered texels are alpha 217–245
(mean 229.3); the other 561,133 alpha-0 texels are empty atlas space.

### Commits that changed the behaviour

1. **`0808444` (0.6.7) — the classification change.** In `MmdMaterialLook`
   (`src/export/Exporter.cpp:205`) it introduced:

   ```cpp
   const bool alphaTestSolid = !ignoresAlpha && mat.alpha && !mat.translucent && !alphaCutout;
   const bool transparent = !ignoresAlpha &&
       (mat.translucent || alphaCutout || (mat.alpha && !opaqueTexture && !alphaTestSolid));
   ```

   Before it the rule was `transparent = (mat.alpha && !opaqueTexture) || mat.translucent || alphaCutout`,
   i.e. this material was **transparent (diffuse.a = 0.99)**. After it, `alphaTestSolid` is true and the
   material **exports opaque (diffuse.a = 1.0)**. The same commit added `opaqueTexMap` / `cutoutTexMap`
   and solidifies the PNG alpha to 255 for every exported-opaque material (`Exporter.cpp:851-900`).
   Its own design doc (`docs/MMD_PANTYHOSE_ALPHA_FIX.md`) records that the visual acceptance test in
   MMD was **not performed** ("MMD is not installed in this environment").

2. **`abdf32b` (0.7.4) — the hole-punching change.** Added `SceneMaterial::shaderClipsAlpha`,
   `BakeHasAlphaCutout()` and `cardMainTex` (`src/assemble/Scene.h`, `src/assemble/Assembler.cpp`,
   `src/export/Preview.cpp`, `src/export/GpuView.cpp`). `BakeShaderTextures` marked any material for
   which ≥25% of covered texels were clipped as a cutout, overriding the opaque-name rule; viewers and
   the PMX writer then honoured that cutout. This is the change that punched large holes in
   alpha-test garments.

3. **`473b0ea` (HEAD) — the revert.** Removed `shaderClipsAlpha`, `BakeHasAlphaCutout`, `cardMainTex`
   and restored the name-based `IgnoresTextureAlpha` rule in both viewers. Commit message:
   *"The bake-evidence cutout flag made whole garments transparent (skirt/cape panels) while the black
   texels remained, so it was worse than the original artifact."*

### Last known good / first known bad
* **Known-good for the screenshot symptom:** `abdf32b^` = `63caef9` (0.7.0, 17:40).
* **First known-bad for the screenshot symptom:** `abdf32b` (20:58).
* **Already corrected at:** `473b0ea` (HEAD, 21:42).
* Independently, the *classification* of this material as opaque rather than tinted-translucent was
  introduced by `0808444` (16:13).

### Files / functions involved
`src/export/Exporter.cpp` — `MmdMaterialLook`, `HasAlphaCutout`, `ExportSceneImpl`;
`src/assemble/Scene.h` — `IgnoresTextureAlpha`, `TextureIsFullyOpaque`;
`src/export/GpuView.cpp` — bucket selection (`plainTransparent`) and `alphaGate`;
`src/export/Preview.cpp` — `DiscardBelow`, `BakeShaderTextures`.

### Reproduction / evidence
I built the same card headlessly (assemble → lit bake → plain/MMD view, WARP, 640×800) at four commits
in isolated git worktrees, using the identical library and mods folders:

| Commit | Result |
|---|---|
| `7fda8a9` (0.4.20) | smooth, sheer brown tights |
| `ccc155d` (0.6.5, before `0808444`) | smooth, sheer brown tights |
| `abdf32b` (0.7.4 — screenshot state) | fine black crosshatch over the tights |
| `473b0ea` (HEAD) | same crosshatch (the `abdf32b` hole rule is gone) |

Pixel measurement, leg region: mean absolute difference HEAD vs `ccc155d` = **9.52**;
HEAD vs `7fda8a9` = **9.52**. Hair region in the same images: 0.11 and 0.21 — i.e. only the legs changed.

Crucially, **the export path at HEAD is already correct**: the exported preview
`out/head_export_176361/Kojima_Kana_view.png` shows solid black tights matching the original card, and
the PMX material is `outfit0.pantyhose/o_panst_00_a#0`, diffuse alpha **1.000**, texture
`baked_cf_m_panst_01_163_opaque.png` (alpha solidified). So the mismatch you saw is in the **interactive
viewer's texture path**, not in the PMX writer.

### Root cause
* The large *holes* in your screenshots came from `abdf32b`'s bake-evidence cutout flag, which treated
  partially-clipped atlas texels as real holes — fixed at HEAD by `473b0ea`.
* `0808444` deliberately changed the material from tinted-translucent to opaque so MMD would not blend
  the baked 0.71 alpha. That is why the exported PMX and its preview now look right, while the viewer,
  which renders the *baked* (non-solidified) texture, still shows a dithered/crosshatched result.
* The baked texture itself is not damaged: covered texels are a smooth alpha 217–245, no mid-alpha
  tint was lost, and UVs are unchanged.

---

## C. Rear Hair Transparency

### Historical status: existing limitation (not a recent regression)

### Structural data (`_bakedump`, HEAD)

```
mat cf_m_hair_b_121_00  shader=Shader Forge/main_hair  queue=2000  ZWrite=1
                        alpha=0  translucent=0  bakedLit=1
                        baked tex A0=128,068  A255=134,076  -> look.diffuse.a = 0.990  (transparent)
   nine other hair submaterials: A0≈243k–260k  -> look.diffuse.a = 1.000 (opaque)
```

The PMX confirms it: `hair.back/cf_hair_b_121_00#0` (30,573 faces) has `a=0.990` and a **non-solidified**
cutout texture, while every other `hair.back/*` material has `a=1.000` and an `_opaque.png` texture.

### Root cause
`HasAlphaCutout()` (`src/export/Exporter.cpp:156-176`) samples each triangle's UV centroid against the
**baked** texture alpha and returns true on the first texel below `kItemCutout` (64). The baked hair
atlas contains both empty alpha-0 padding between strand islands and jagged alpha-0 strand tips
(visible in the dumped atlas). One centroid lands on such a texel, so the main rear-hair submesh is
classified as an alpha cutout → `diffuse.w = 0.99` → MMD's blended pass → **no depth write** → the
background and front-facing geometry show through the strand cutouts. The other nine submeshes' UV
islands happen to sample only opaque texels and stay opaque — hence the mixed, jagged appearance.

The material itself is not authored transparent: `mat.alpha = 0`, game queue 2000, ZWrite On.
`e21dc37` (0.4.6, 2026-11-20) already tried to keep back hair opaque by setting the hair tint's alpha
to 1.0 instead of 0.0, and noted that "the MMD viewer now matches the non-MMD viewer (back hair
included)". The later `HasAlphaCutout` classification overrides that intent for this card.

### Historical comparison
Rear-view renders (`_camerarender`, yaw 160°, pitch 8°, plain view) at `7fda8a9` (Nov 21),
`ccc155d` (Nov 22) and HEAD are visually identical in the hair region: mean absolute pixel difference
**0.11** (HEAD vs `ccc155d`) and **0.21** (HEAD vs `7fda8a9`). The change between those commits is
confined to the legs. `HasAlphaCutout` has used the centroid rule since `609d7c2` (Nov 13), whose
intent was to stop the *skin* atlas's coverage mask from turning the body transparent.

### Classification
**Existing limitation / long-standing defect** (alpha-cutout classification of baked hair atlases),
possibly made more visible by `cb783ef` (Nov 15)'s transparent-pass ZWrite rule, but **not** introduced
by the 2026-11-22 transparency commits. No evidence supports "confirmed regression" for this issue.

---

## D. Shared Root Cause Analysis

The two issues are **not** the same regression (Scenario C / D — mixed).

| | Pantyhose | Rear hair |
|---|---|---|
| First bad commit | `0808444` (classification), `abdf32b` (holes; reverted at HEAD) | none found in the recent range |
| Mechanism | alpha-TEST tint (181/255) read as translucency; baked atlas partially empty | baked atlas padding read as a strand cutout |
| Code path | `MmdMaterialLook` / export texture rewrite | `HasAlphaCutout` → transparent pass, no depth write |
| Status at HEAD | export correct, viewer still differs | unchanged since at least 0.4.20 |

They only share a theme — texture alpha being used as a proxy for "is this fabric see-through" — not a
commit, not a function, and not a fix. Do not force a common explanation.

---

## E. Implementation Changes

The screenshot-era correction (`473b0ea`) is already in HEAD; the permanent fixes for the two
remaining problems are described in **section H** below. Files modified:
`src/assemble/Scene.h`, `src/export/Exporter.cpp`, `src/export/Exporter.h`, `src/export/Preview.cpp`
and `tests/cardbridge_tests.cpp`.

One candidate was tried and discarded: changing the fallback alpha gate in `src/export/GpuView.cpp`
was pixel-identical (difference 0.0) because it did not address the texture the viewer binds. It was
reverted; `GpuView.cpp` is unchanged. No blind revert of `0808444` was performed — that commit
deliberately fixed a different, real problem (MMD blending the baked tint), and its PMX result is
correct.

---

## F. Testing Results

* **Build:** full `cmake --build build` (85 targets) succeeded with w64devkit GCC 16.2.0; the compiler
  needs `D:\Project K\tools\w64devkit\bin` on `PATH` for `cc1plus`.
* **Automated tests:** `cardbridge_tests.exe` — **19,169 checks, 0 failed**, 2 integration suites
  skipped (no `CARDBRIDGE_TEST_*` env vars). Same count before and after the trial change.
* **New regression harness (headless, per-commit):** `tools/_camerarender.cpp`, `tools/_bakedump.cpp`,
  `tools/_matinspect.cpp`, plus worktrees at `7fda8a9`, `ccc155d`, `abdf32b`, `555c716`. Outputs in
  `out/repro_*`, `out/rear_*`, `out/bakedump_head/`.
* **Viewer:** plain/MMD view reproduced at all four commits; the leg region changes only at `0808444`,
  the hair region does not change at all.
* **PMX export:** exported at HEAD to `out/head_export_176361`; material alpha and solidified texture
  names verified by parsing the PMX (`outfit0.pantyhose` a=1.000; `hair.back/*#0` a=0.990).
* **External renderer:** MMD is not installed in this environment, so MMD acceptance is still
  unverified. The in-app "as exported" preview images were used as the closest available proxy and
  show correct tights.

---

## G. Remaining Limitations

1. **MMD is not installed**, so final acceptance in an independent MMD-compatible renderer is still
   unverified. The PMX material values and the in-app exported preview were verified instead.
2. The headless renderer uses WARP and the runtime fallback shader, so it cannot reproduce the
   Direct3D game-shader path of the live viewer. Pixel deltas here are evidence of *change*; please
   confirm the final look in the rebuilt interactive viewer.
3. **Original strand-level alpha cutouts on hair are not reproduced per-fragment in PMX.** MMD has no
   alpha test; the fixed classification prioritises correct depth occlusion and silhouette (solid
   rear hair), with the baked strand detail preserved in RGB. Genuinely alpha-tested materials still
   use the binary-cutout path where the mesh actually spans the holes (verified on `KK_235599.png`).

---

## H. Implementation (permanent fixes)

### Pantyhose / stockings — viewer and export consistency

**Root cause of the inconsistency.** `MmdMaterialLook` correctly classifies `cf_m_panst_01` as
`alphaTestSolid` and the PMX writer solidifies its texture alpha to 255 (`opaqueTexMap`). The
interactive viewer, however, bound the **baked** texture directly: its alpha (217–245) was read as
translucency by both the game-shader and fallback paths, so the underlying body — itself dithered by
the AO bake — showed through as a crosshatch.

**Shared interpretation.** Added to `src/export/Exporter.cpp` / declared in `src/export/Exporter.h`:

* `IsAlphaTestSolid(scene, mat)` — one structural decision (alpha shader, no authored cutout, no
  translucency, texture not already opaque), used by `MmdMaterialLook` *and* by the scene preparation.
* `SolidifyAlphaTestSolidTextures(scene)` — copies the affected material's texture, forces **only the
  alpha channel** to 255, appends it to `scene.textures` and repoints `mat.texture` at it. The baked
  RGB (the sheen, shading and fabric detail) and all UVs are byte-for-byte unchanged. It is idempotent,
  so calling it twice is safe.

`src/export/Preview.cpp` (`BakeShaderTextures`, after grading) now calls
`SolidifyAlphaTestSolidTextures`, so the viewer's scene and the exported scene contain the same
pixels. The PMX writer's existing `opaqueTexMap` still handles every exported-opaque material, so
exported diffuse alpha stays 1.0 and the written texture stays solid. The Lit view's `texProps` still
reference the original texture, so the game-shader look is unchanged.

Genuine cutouts are excluded by `IsAlphaTestSolid` (they go through the binary-cutout path) and
authored-translucent materials are excluded by `mat.translucent`, so nothing is forced opaque based on
the shader name alone.

### Rear hair — false alpha-cutout classification

**False-positive mechanism.** `HasAlphaCutout` sampled one UV centroid per triangle and declared a
material a cutout on the first texel below `kItemCutout` (64). A baked hair atlas contains alpha-0
padding between strand islands and jagged strand tips, so a single sample was enough to send the main
rear-hair submesh (30,573 faces) to MMD's blended, no-depth-write pass.

**Fix.** Two complementary changes:

1. `src/assemble/Scene.h`: added `EffectiveRenderQueue(scene, mat)` and extended
   `IgnoresTextureAlpha` with a structural rule — a material with **no alpha flag, no translucency and
   an opaque render queue (< 2450)** is drawn by the game without reading texture alpha, whatever the
   baked atlas contains. The rear hair (`main_hair`, queue 2000, ZWrite On, alpha 0) is opaque by this
   rule; genuinely transparent hair (`main_hair_front`, queue 2850) and alpha-test hair remain eligible
   for cutout detection.
2. `src/export/Exporter.cpp`: `HasAlphaCutout` now takes four barycentric samples per triangle and
   requires at least **5%** of the material's own samples to be discarded (with a minimum sample
   count). One stray texel or a small patch of padding can no longer classify an entire material as a
   cutout; a real lace boundary or strand cutout still does.

Because `MmdMaterialLook` and the export texture rewrite both use these helpers, the rear hair now
exports with diffuse alpha 1.0 and a solidified texture, and is drawn with depth writes in the viewer.
No card-specific or material-name exception was added.

### Verification of the implementation

* **Build:** full project rebuilt successfully (w64devkit GCC 16.2.0).
* **Automated tests:** **19,183 checks, 0 failed**, 2 integration suites skipped (baseline 19,169).
  Added tests cover: alpha solidification preserving RGB, idempotence, genuine cutouts keeping their
  mask, a single stray padding texel not classifying a material as a cutout, opaque-queue hair staying
  opaque, and genuine alpha-test hair staying transparent.
* **Plain / "as exported" viewer (headless WARP, same card):** the leg region changed by **11.61**
  mean absolute RGB; the crosshatch disappeared and the tights now render as smooth dark fabric with
  the baked sheen visible. The rear-hair region changed by **2.47** and the jagged see-through patches
  are gone; the hair occludes correctly.
* **Scene inspection (`_bakedump`, same card):** main rear-hair material `look.diffuse.a` changed
  **0.990 → 1.000**; all 9 sibling submaterials remain 1.000.
* **PMX export (`out/fixed_export_176361`):**
  `hair.back/cf_hair_b_121_00#0` is `a=1.000` with `baked_..._opaque.png` (was 0.990 with a cutout
  texture); all nine other `hair.back/*` materials remain 1.000.
  `outfit0.pantyhose/o_panst_00_a#0` stays `a=1.000` with a solidified opaque texture.
  The exported preview image is unchanged and still shows correct black tights.
* **Other cards:** `KK_235599.png` — the knee-high stocking `cf_m_socks_knee01`, which has a genuine
  lace cutout, still exports `a=0.990` with its alpha mask: no over-correction. `KK_101895.png` —
  all rear-hair submaterials are opaque (1.000), including modded hair, with no crash or regression.
  `KK_237456.png` was read without errors (its pantyhose material name differs, so the filtered dump
  produced no lines).
* **External MMD:** still unavailable; MMD acceptance remains unverified. The in-app exported preview
  and the PMX material values are the available proxies and both are correct.
