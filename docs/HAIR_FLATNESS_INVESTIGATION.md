# Hair appearance loss on KK card import — investigation report

Reference card: `C:\Users\pepsi\Downloads\KK_101895.png` (Matoi Ryuko, `【KoiKatuChara】`).
Reference import: `Screenshot 2026-11-22 183030.png` (CardBridge 0.7.x viewer).
Investigated on 2026-11-22 with library `library_full` and the full BetterRepack RX21 mods folder
(44/44 card items resolved). Outputs used: `out/hair_mods` (baseline), `out/hair_restoremd`,
`out/hair_fix`, `out/hair_bakelit`, `out/hair_spherefix`.

## Verdict

The hair strands, gradient and gloss of the card are **not lost during extraction or geometry conversion**.
They are lost by two independent things in the **shader/material reconstruction** path:

1. **A concrete bug:** the PMX "hair ring highlight" sphere map was written by a branch guarded by
   `!mat.bakedLit`, but every successfully baked hair material is marked `bakedLit = true`
   (`Preview.cpp:1424` sets `thisLit = lit || IsHairShaderFamily(mat.shader)`, and `:1723` copies it to
   `mat.bakedLit`). The branch was therefore unreachable: **no hair material in any exported PMX ever got a
   sphere map.** MMD showed only the flat baked colour. — **FIXED.**
2. **A fundamental limitation:** KK's anisotropic hair highlight is a *view/normal-dependent* term. The
   UV-flat shader bake sets every vertex normal to +Z (`FlattenToUv`, `Preview.cpp:1117-1121`), so the
   highlight term is constant across the whole hair texture and every texel bakes to the same colour.
   The viewer then shows baked-lit hair without further shading (`Preview.cpp:1079`,
   `lit = mat.bakedLit ? 1.0f : …`). No geometry or UV data is involved.

A third, card-authored factor makes the loss worse for this particular card: its MaterialEditor payload
overrides `_DetailMask` on **every** hair material with texture id 13, a flat RGB `(255,93,235)` dummy
(`out/hair_mods/me_tex_13.png`), replacing the real strand masks (`cf_hair_*_md`, green vertical stripes —
`out/hair_mods/orig/cf_hair_f_106_00_md.png`). The card also stores the same value in
`baseColor`, `startColor` and `endColor` (`0.396, 0.439, 0.443`), so the `_ColorMask` root-to-tip gradient
texture has nothing to interpolate. These are properties of the card data, not conversion bugs; the game
reads the same values.

## Evidence

### Texture extraction — correct
From the original bundle `chara/deathweasel/acc_bo_hair_f_12.unity3d` (DeathWeasel Accessory Hair Front
v1.5.4) UnityPy shows the material `cf_m_hair_f_106_00` binds:

| Shader property | Texture | Role |
|---|---|---|
| `_ColorMask` | `cf_hair_f_106_00_mc` (1024²) | red field + yellow/magenta root/tip gradients |
| `_DetailMask` | `cf_hair_f_106_00_md` (1024²) | green strand stripes |
| `_HairGloss` | (library) `cf_hair_00_01_mh` | the band of dashes for the ring highlight |
| `_MainTex` | null | the shader's white default |

CardBridge resolves all of these into the scene (`_matdumpprops.exe`): `_ColorMask -> cf_hair_f_106_00_mc`,
`_DetailMask -> card_tex_13` (the card's ME override), `_HairGloss -> cf_hair_00_01_mh`.
So extraction itself works; the masks are present.

### Shader bake — flat by construction
`main_hair_front` PS uses 8 texture registers; the disassembly shows the specular path:
`dp3` of (normal, half-vector) → `log/exp` power → `_AnotherRamp` sample (instr 57-69), then `_HairGloss`
sampled twice with UVs derived from that dot product (instr 88-93), then `_DetailMask` (instr 117-118)
and an additive term (instr 126, `mad_sat`). With `FlattenToUv`'s normals all `(0,0,1)` and one fixed
headlight, the dot product is identical for every texel, so the whole term collapses to one constant.

Measured result: `out/hair_mods/textures/baked_cf_m_hair_f_106_00_170.png` is a single solid navy
rectangle. Restoring the original strand masks through a Material Workbench override
(`out/hair_mods/restore_md.json`, `out/hair_restoremd/`) adds faint stripes to the bake and more clump
definition in the head preview, confirming the masks are decoded and sampled correctly — but the bright
white highlights still cannot be baked, because they are normal-view dependent.

Experiments that did **not** work (reverted, not committed):
- Keeping the authored normals in `FlattenToUv` for hair: the diffuse `N·L` term turns the hair almost
  black (`out/hair_fix`), because the bake's light is defined for the +Z flat sheet. Not a fix.
- Baking in `--bake-lit` mode: same dark result (`out/hair_bakelit`).

### PMX material conversion — bug and fix
`Exporter.cpp:314` had:
```cpp
else if (!mat.bakedLit && !overlay && !lens && hair) { look.sphere = 3; look.sphereStrength = 0.15f; }
```
`mat.bakedLit` is true for every hair material after the shader bake, and `Exporter.cpp:848` writes the
sphere texture only when `look.sphere > 0`. Result: hair sphere never exported
(baseline: 70 textures, no `sphere_hair_*`).

Changed to:
```cpp
else if (!overlay && !lens && hair) { look.sphere = 3; look.sphereStrength = 0.15f; }
```
with an explanatory comment. The generic gloss sphere (`look.sphere = 2`) intentionally keeps its
`!mat.bakedLit` guard, so lit bakes do not get a second broad sheen.

After the fix: `out/hair_spherefix` exports 71 textures including `textures/sphere_hair_15.png`, a
add-mode ring band for hair. `git diff` of PMX info before/after is identical in vertices, indices,
deform types, bones, morphs, materials and rigid bodies — only the extra sphere texture.

### Geometry / UV — verified unchanged
- Modded conversion: 131 159 vertices, 187 123 triangles, 973 bones, 623 morphs (identical before/after).
- No geometry code was touched. `FlattenToUv` experiment was reverted.
- UVs are read from the mesh and used directly by the bake (`Preview.cpp:1496-1507`); exported UVs are
  the same V-flipped values as the input (`Exporter.cpp:824`). No UV transform or assignment bug found.

## What was changed

1. `src/export/Exporter.cpp` — `MmdMaterialLook`: removed the `!mat.bakedLit` guard from the hair sphere
   branch so baked hair gets its ring highlight in the PMX and in the viewer's "as exported" (MMD) mode;
   documented why the generic gloss branch keeps the guard.
2. `tests/cardbridge_tests.cpp` — extended the existing baked-lit hair/accessory test: baked hair must
   get `sphere == 3` with non-zero strength, while a baked-lit skin material must not get a sphere.

No other source file was changed by this investigation. `src/export/Preview.cpp` was temporarily
patched and then fully reverted (see `git diff` — only the pre-existing user changes remain).

## Validation

- Rebuilt `cardbridge_hairfix.exe` and `cardbridge_tests_hairfix.exe` locally (the user's running
  `build/bin/cardbridge.exe` was deliberately left untouched — it had the file locked).
- Converted the reference card with the full mods folder: `out/hair_spherefix`,
  `sphere_hair_15.png` present, 71 textures (was 70), same geometry counts.
- Unit tests: **19 176 checks, 0 failed** (2 integration suites skipped, as usual without the env vars).
- `cardbridge pmx info` on both PMX files confirms geometry parity.

## Remaining limits (not fixed, by design)

- The fine white strand lines are still absent from the **lit viewer** and from the baked texture in the
  PMX: reproducing a view-dependent anisotropic highlight in a static UV bake would require baking a
  separate normal/gloss response or a viewer-side hair highlight shader — neither exists today, and
  adding one would change every hairstyle. The PMX ring sphere restores a subdued highlight in MMD.
- This card's flat magenta MaterialEditor `_DetailMask` override and its identical three hair colours are
  card data and are honoured as authored; overriding them automatically would be a card-specific hack.
- The MaterialWorkbench `anisotropic` field is currently preview-only metadata and is not read by any
  renderer; a future viewer-side hair highlight could use it.

## Reproduce

```powershell
$mods = 'E:\Game\[ScrewThisNoise] Koikatsu BetterRepack RX21\mods'
.\build\bin\cardbridge.exe convert 'C:\Users\pepsi\Downloads\KK_101895.png' `
  --library 'D:\Project K\cardbridge\library_full' --mods $mods --out out\hair_spherefix
# inspect out\hair_spherefix\textures\sphere_hair_15.png
```
