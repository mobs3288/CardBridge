# Black bands on alpha-test garments in the MMD view — scoped fix (0.7.9)

Card: `C:\Users\pepsi\Downloads\KK_388778.png` (China dress / `cf_m_top_tennis_d`).
Symptom: in the viewer's **as exported (MMD)** mode the dress had solid black bands over the
forearms, under the bust and across the waist — "a black shadow outfit". The game and the
card thumbnail show skin / the layer underneath in those regions.

## Root cause

The garment's shader is `Shader Forge/main_opaque`, so CardBridge classifies it with the
name-based `IsOpaqueShaded` rule: texture alpha ignored, texture solidified when written to
PMX. But this material's `_MainTex` was **replaced by an image embedded in the card**
(MaterialEditor) that has real alpha holes (about 83% of the 2048² card image is alpha 0),
and the game shader is an alpha **test** (`_Cutoff` 0.50, queue 2450). The game clips those
texels; CardBridge drew (and solidified) them.

Evidence from `tools/_bakedump.cpp` on the assembled, baked scene:

```
mat cf_m_top_tennis_d shader=Shader Forge/main_opaque alpha=0 trans=0 lit=1
    lookdiff.a=1.000 tex=baked_cf_m_top_tennis_d 1024x1024
    A0=947960 A255=100616 Amid=0        (material 204)
    A0=615483 A255=433093 Amid=0        (material 205)
```

The bake contains the holes — the shader **did** clip them. The discarded texels carry black
RGB (the shader writes zero where it clips), so treating the material as opaque painted that
black over the dress: the bands.

## Why the 0.7.4 fix was reverted

Commit `abdf32b` added `shaderClipsAlpha`, set from the bake's own output whenever at least
25% of the covered texels were clipped — for **card and game textures alike**. That was too
broad: a game texture's low-alpha atlas texels (skirt/cape panels, Yoko Littner's bloomers)
were declared real cutouts, which put whole garments into MMD's blended pass and punched
holes in them, while the black texels were not removed. It was reverted in `473b0ea`.

## The scoped fix

Keep the bake as the evidence, but require the structural fact that distinguishes this case:
**the card supplied `_MainTex`**. `CardMainTexClipsAlpha(mat, img, cov)` in `Scene.h` is true
only when all of:

1. `mat.cardMainTex` — the card's MaterialEditor replaced `_MainTex` (set in `Assembler.cpp`);
2. `!mat.translucent` — a genuine blend is never converted to a cutout;
3. `IsOpaqueShaded(mat)` — the opaque-name family the name rule would otherwise solidify;
4. `BakeHasAlphaCutout(img, cov)` — at least 25% of the texels the mesh actually covers were
   clipped by the shader's own bake (stray atlas texels do not count).

When it fires, `BakeShaderTextures` sets `shaderClipsAlpha = true`, `alpha = true` and
`translucent = false`. `IgnoresTextureAlpha` then returns false for the material, so the same
decision reaches all three consumers:

* the software viewer (`DiscardBelow` now uses `IgnoresTextureAlpha` in both modes),
* the GPU viewer (the fallback shader's `alphaGate` uses `IgnoresTextureAlpha` in both modes),
* the PMX writer (`HasAlphaCutout` → binary alpha texture, material alpha 0.99 so MMD tests it).

Game-texture materials keep the name-based opaque rule no matter what their atlas alpha does,
so the cape / stockings / bloomers regressions cannot come back through this path.

Verified after the fix on `KK_388778.png`:

```
mat cf_m_top_tennis_d shader=Shader Forge/main_opaque alpha=1 trans=0 lit=1
    lookdiff.a=0.990 tex=baked_cf_m_top_tennis_d 1024x1024
    A0=947960 A255=100616 Amid=0        (material 204)
    A0=615483 A255=433093 Amid=0        (material 205)
```

The alpha holes that were previously solidified now reach MMD as a cutout, so the discarded
black texels no longer paint the bands.

## Files

* `src/assemble/Scene.h` — `SceneMaterial::shaderClipsAlpha`, `cardMainTex`,
  `BakeHasAlphaCutout`, `CardMainTexClipsAlpha`, `IgnoresTextureAlpha` override.
* `src/assemble/Assembler.cpp` — set `cardMainTex` when a card image replaces `_MainTex`.
* `src/export/Preview.cpp` — scoped bake decision; `DiscardBelow` honours `IgnoresTextureAlpha`.
* `src/export/GpuView.cpp` — fallback `alphaGate` honours `IgnoresTextureAlpha` in both modes.
* `tests/cardbridge_tests.cpp` — regression test: the card-texture case becomes a cutout, a
  game texture with identical pixels stays opaque, translucent and non-opaque shaders are
  excluded, and stray alpha is not a cutout.

## Caveats

* MMD itself is not installed here; the acceptance proxy is the PMX material alpha (0.990),
  the binary alpha texture, and the in-app "as exported" preview. Please confirm in MMD and
  in the rebuilt viewer.
* The rule is deliberately narrow. Other cards that hide a game-texture cutout behind an
  `opaque`-named shader still need their own evidence; they are not changed by this fix.
