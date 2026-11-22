# Phantom black bands on KK/KKS outfits — root cause and fix

## Reproduction

Card `C:\Users\pepsi\Downloads\KK_388778.png` ("Granhiert Elsa", a modded KK card using
`[6607ASDG] WSJ` / `[Quokka] China Dress`), converted with the full library and RX21 mods.
The viewer drew solid black bands along the openings of the dress (hem, arm holes, navel tab,
collar and sleeve edges). The original card has no such bands.

## Evidence

- The top's `_MainTex` is a card-embedded image (`card_tex_4`) supplied by a MaterialEditor
  `MainTex` edit. It is a sparse grey/purple overlay: 83% of its texels have alpha 0.
  (`_dumptex` on `cf_m_top_tennis_d`; `CARDBRIDGE_DEBUG_ME=1` shows the edit replacing
  `cf_m_clothesN_create_result`.)
- `Shader Forge/main_opaque` (in `chara/co_top_00.unity3d`) does contain a `discard`
  instruction. The game clips the low-alpha texels, so those areas show the body or the
  garment underneath — exactly what the original card shows.
- CardBridge's shader bake ran that clip faithfully: of 321,922 covered texels only 96,846
  had alpha >= 0.5. But because the shader name belongs to the "opaque" family,
  `IsOpaqueShaded` / `IgnoresTextureAlpha` told the viewers and the exporter to ignore alpha.
  The bake's clipped texels keep a black RGB, and `Exporter.cpp` solidifies their alpha to
  255 for an opaque PMX material. The result was opaque black geometry along every cut edge —
  not a texture defect in the card, not duplicate geometry, and not the outline pass
  (`CARDBRIDGE_NO_OUTLINE=1` changed nothing).

## Root cause

The blanket rule "a shader whose name contains `opaque` ignores texture alpha" is wrong when
the card replaces `_MainTex` with an alpha-masked image. The game's shader clips those texels;
CardBridge treated them as solid black and painted them. The same happens with a game texture whose material is an alpha test: the shader's own discard, not the texture's owner, is the evidence. A few stray low-alpha atlas texels still do not trip the bake's 25% test, so intact clothes stay solid.

## Fix

- `src/assemble/Scene.h`: `SceneMaterial::cardMainTex` records a card-supplied MainTex (as
  opposed to a game texture); `shaderClipsAlpha` records a clip proven by the bake;
  `BakeHasAlphaCutout` detects a real alpha cutout in the bake's own output.
  `IgnoresTextureAlpha` respects `shaderClipsAlpha`.
- `src/assemble/Assembler.cpp`: records `cardMainTex` where a MaterialEditor `MainTex` image
  is bound and marks card-authored alpha masks as cutouts for the unbaked view. Game textures
  are not inferred from their pixels here — a bake is required to prove the shader clips.
- `src/export/Preview.cpp`: after baking, any material the name-based rule would solidify is
  marked as a cutout when the shader's own output shows a substantial clipped area — card or
  game texture alike. `DiscardBelow` uses `IgnoresTextureAlpha`, so viewers discard those
  texels instead of drawing their black RGB.
- `src/export/GpuView.cpp`: the fallback shader's alpha gate uses the same rule in both the
  standard and plain view.
- `src/export/Exporter.cpp` already reads `IgnoresTextureAlpha`, so the PMX keeps a binary
  cutout mask and MMD's 0.99 pass for these materials. Skin, accessories and alpha shaders
  keep their existing rules.

## Verification

- `cardbridge_tests.exe`: 19,181 checks, 0 failed. New coverage: alpha-mask detection, the
  card-MainTex cutout decision, and the bake proof being honoured for a game texture too.
- `KK_388778.png`: the black bands are gone, the grey/purple panels and all legitimate dark
  details (gloves, tights, shoes, hair) remain.
- `KK_235599.png`: the stockings use `main_alpha` and are unaffected. The bottom's 10.4%
  clipped UVs are a real `_Cutoff` discard; the software preview is pixel-identical with the
  cutout honoured (that region is on hidden geometry), so there is no visible regression.
- `KK_237456.png` ("Yoko Littner"): the bloomers' `main_opaque` shader discards 33.4% of the
  covered texels. With the card-texture-only restriction removed, material 14 is alpha 0.99
  with a binary 0/255 PNG, the band above the waist is gone and the render matches the viewer.
