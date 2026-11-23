# Phantom black bands on KK/KKS outfits — root cause and fix

## Reproduction

- `C:\Users\pepsi\Downloads\KK_388778.png` ("Granhiert Elsa"): black bands along the dress openings.
- `C:\Users\pepsi\Downloads\KK_237456.png` ("Yoko Littner", outfit 5): a solid black band above the shorts' waist in MMD; the CardBridge viewer shows skin.

## Root cause

A shader whose name contains `opaque` can still be an alpha test: `Shader Forge/main_opaque` has a
`discard`, and the material carries `_Cutoff = 0.5` with render queue 2450 (AlphaTest). The game cuts the
fabric wherever the texture's alpha is below the cutoff and shows the body or the layer underneath.
CardBridge's name-based `IgnoresTextureAlpha` rule solidified those discarded texels: their baked RGB is
black, so the PMX painter solid black geometry along the cut edges. On Yoko two materials are involved:
material 14 (`o_bot_pants01_a`, 33.4% of the covered UVs discarded) and material 45 (`o_top_jacket07_a`,
21.5%) — hiding either one in a software render of the affected export removes part of the band; a 25%
detection threshold caught only material 14, so the band came back.

## Fix

- `SceneMaterial::shaderClipsAlpha` records a clip proven by the shader bake; `IgnoresTextureAlpha`
  respects it, so the software viewer, the GPU fallback and the PMX writer all honour the holes.
- `BakeShaderTextures` sets it whenever the bake discards at least 5% of the texels the mesh covers —
  for any texture, card or game. Stray low-alpha atlas padding (a few texels) stays below the threshold,
  so intact garments are not turned into see-throughs. The evidence is the shader's own output, not a
  card, texture, shader or material name.
- Baked textures for proven-cutout materials get a `_cutout` filename suffix, so an identically named
  solid-alpha PNG left over in MMD's texture cache from an earlier export cannot bring the band back.
- `DiscardBelow` (software views) and the GPU fallback shader use the same `IgnoresTextureAlpha` rule.

## Verification

- Software renders of the exported PMX: both materials get alpha 0.99 and binary 0/255 PNG alpha, and
  the waist band is gone (skin down to the belt).
- `KK_388778.png`: the dress openings keep their cutouts; dark details remain.
- `KK_235599.png`: the stockings use `main_alpha` and are unaffected; the bottom's 10.4% clip is on
  hidden geometry, so its preview is pixel-identical with the cutout honoured.
- `cardbridge_tests.exe`: new coverage for the 5% bake proof and for honouring a proven clip on an
  opaque-named, game-textured material.
