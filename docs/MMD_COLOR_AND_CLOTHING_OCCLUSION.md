# MMD colour fidelity and clothing occlusion — investigation and fix (0.4.14)

## 1. Root-cause analysis

### 1.1 Why MMD colours differed from the game view

The game (character maker and in-game) runs every finished frame through AmplifyColor's 32³ `Lut_Amount` table. CardBridge loads that game asset into `library/colorgrade/lut_amount.png` (`ColourGrade.cpp::LoadColourGrade`) and applies it with a trilinear lookup (`ApplyColourGrade`).

There are exactly two ways that lookup reaches a picture:

1. **Frame grading — the game view.** The software rasterizer grades the finished frame (`Preview.cpp:1088`) and the GPU path grades it too (`GpuView.cpp:1924`). Both are gated on `shaded` / `!plain`; this is the "Non-MMD" look and it is correct.
2. **Texture grading — the MMD/PMX look.** A PMX has no post-process, so the grade has to live in the texture bytes. `87e18b9` established this for textures the shader bake writes (`Preview.cpp::BakeShaderTextures`) and added an export-time fallback for colour textures the bake did *not* write (`Exporter.cpp`, guarded by `SceneTexture::bakedInShaderBake`).

The divergence was in the second path. The fallback lived **only in the PMX writer**. The viewer's "as exported" (plain/MMD) renderer deliberately does not frame-grade (`GpuView.cpp:1924`), because it assumes the textures it samples were graded by the bake — but a material whose shader bake is skipped never gets graded in the in-memory scene:

- `574620d` (KKS support) skips every `Koikano/` material in `BakeShaderTextures` (`Preview.cpp:1389-1392`) because the UV-flat bake cannot reproduce their screen-space lighting.
- Create-job results and materials with no shader in the bundle are also never written by the bake.

So the *same texture* was graded once in the PMX PNG and zero times in the viewer's MMD preview. KKS cards, create-job textures and missing-shader materials therefore looked different in MMD mode, while the Lit view stayed correct. A second gap existed in `Exporter.cpp`: the `main_item` cutout branch (`solid`) was an `else-if` sibling of the grading branch, so an unbaked cutout texture escaped grading entirely.

The grade is strongly non-linear (e.g. the shipped table maps grey 128 → ~90 and 64 → 0), which is why the symptom covered the whole colour range — greys, off-whites and shadow detail shifted, not just one colour.

There was no double grading in the normal flows; `bakedInShaderBake` correctly suppressed the exporter fallback for baked textures. The bug was a **skipped** processing stage in one consumer, plus a flag that described provenance ("written by the bake") rather than the fact consumers actually needed ("already graded").

### 1.2 Why the inner garment was not occluded

List rows in both KK and KKS carry three visibility mask columns: `OverBodyMask`, `OverBraMask` and `OverInnerMask`. `Assembler.cpp` resolved and applied only the first two. The third is how an outer top sub-part (e.g. a jacket, category 211) tells the game which triangles of the inner sub-part (the shirt, category 210) to discard — authored in the inner garment's own UV space (the `*_inner_mab` masks in `chara/mt_mask_inner_00.unity3d`).

Because it was never read, CardBridge exported the shirt's full geometry, including the sleeves the jacket hides. The existing body-mask and occlusion machinery only ever targets `body` (`BodySuppression.cpp`), and `SeparateCoincidentLayers` only handles duplicates of the *same* item, so nothing else could compensate. The recent skinning fix (`3609674`) made the sleeves follow the pose correctly, which made the long-standing overlap visible.

This is a shared KK/KKS format field — the columns exist identically in both base list tables — so one implementation serves both games with no game-specific branch.

## 2. Git history findings

- `42d8a42` introduced frame grading; `d069b1f` linear-light rendering; `eaeab4f` introduced "grade once in the texture" for the as-exported view and the plain renderer's skipped frame grade. All three are still current.
- `87e18b9` (the KK colour fix) removed the translucent-material skip in the bake, added `bakedInShaderBake`, and added the exporter's grade fallback for unbaked sRGB textures. It did **not** touch the viewer's plain path; the invariant was already "grade once in the texture", but the flag was only consulted at export.
- `574620d` (KKS) added the `Koikano/` bake skip. MMD rendering was not part of that fix, and the skip created the ungraded-texture case in the viewer.
- `6e3664a` tried a global lit bake for the as-exported view and was reverted by `cd4451a`; `deab7e1` kept the lit bake only for hair. `1757772` removed a double sRGB encode in the bake. Both are preserved — this change does not alter them.
- `265b65b` (coincident duplicate layers) and `3609674` (skinning bind table) are orthogonal and untouched.

**Reused, not duplicated:** the previous implementation's `ApplyColourGrade`, the bake grade, and `bakedInShaderBake` are retained. The new `GradeTexture` is the *same* lookup applied through one helper; `bakedInShaderBake` stays as provenance and a new `colourGraded` flag is the single source of truth for the fact both consumers need.

## 3. Implementation

### Colour (`src/export/*`, `src/assemble/Scene.h`)

- `SceneTexture::colourGraded` records that the LUT was applied to a texture's RGB.
- `ColourGrade.cpp::GradeTexture(SceneTexture&)` grades one sRGB colour texture exactly once (alpha preserved, `srgb == false` data textures skipped, idempotent via the flag); `ApplyOutstandingColourGrade(Scene&)` applies it to every texture in a scene.
- `BakeShaderTextures` no longer grades inline. At the end it calls `ApplyOutstandingColourGrade(scene)`, so the as-exported viewer's texture bytes and the PMX writer's PNG bytes come from the same rule — including KKS `Koikano/`, create-job and missing-shader materials.
- `Exporter.cpp` now uses `GradeTexture` on the copy it is about to encode, and does so for both the normal and the `main_item` cutout cases (closing the gap where cutouts escaped grading). A texture already graded by the bake is never graded again.
- `LoadColourGradePixels` exposes the LUT loader to tests without a game library.

### Clothing occlusion (`src/assemble/*`)

- `ScenePart::subGroup` records the card-defined `subPartsId` group of a top's sub-parts.
- `Assembler.cpp` resolves `OverInnerMaskAB` / `OverInnerMask` exactly like the body and bra masks.
- `IsInnerGarmentOf(cover, candidate)` (in `Assembler.h`) pairs an outer sub-part with the inner sub-part of the *same group* and lower list category — no slot numbers, item ids, outfit indices or "jacket"/"shirt" names.
- The existing `applyMasks` routine is reused with a target selector: the inner mask is sampled in the inner garment's UV space, and only covered triangles move into a hidden under piece (`visibleInPreview=false`, `startHidden=true`, `coveredBy=<cover>`), restored by the nude switch / PMX morph. Translucent covers, hidden-in-game covers and missing-mask warnings follow the existing rules.

## 4. Verification

- `cardbridge_tests.exe`: all checks pass; new tests cover exactly-once grading, exclusion of data textures, alpha preservation, idempotence, and the structural inner-garment pairing for both jacket (210/211) and sailor (200/201) sub-part groups.
- KKS occlusion card (`120324154_p2.png`, Outfit 4), KKS library/mods: `inner garment mask: 1737 of 4692 triangles hidden under clothing`; the remainder (collar/hem/visible sleeve) stays; the PMX carries `outfit3.top.sub_a.under.top.sub_b` with an `Under ...` morph.
- KK-shader colour card (`Yx3EcDn2ddoGekvOHwvI1jyx.png`): the PMX and every generated preview PNG are byte-identical to the pre-fix export (only the embedded version string changed), confirming no regression to the established KK bake and the Lit view.
- KK regression card (`139306275_p3.png`) converted with the KK library/mods to confirm the shared code path still assembles and exports.
