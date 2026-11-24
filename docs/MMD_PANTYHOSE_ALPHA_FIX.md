# MMD pantyhose transparency — root cause and fix (0.6.7)

> **Update (0.7.11):** binarising at `_Cutoff` is correct only for an alpha-TEST shader. A material
> in a transparent queue (Unity 3000+, e.g. the fishnet `cf_m_panst_02` at queue 3040) *can blend* its
> authored alpha over the skin — a ~200/255 dark tint draws a fishnet, not a solid sheet. Thresholding
> that alpha made it fully opaque black in MMD. Transparent-queue materials were therefore exempted
> from binarisation, while alpha-test materials kept the binary cutout described below.
>
> **Regression fix (0.7.33):** the queue exemption was too broad. The Roy12 knee socks
> (`cf_m_socks_knee01`, Yoko Littner outfit 5/6) are a modded `main_alpha` alpha test at material queue
> 3050 / shader queue 3040 — but with **ZWrite On**. Its ~190/255 is a tint that passed `_Cutoff`, not
> translucency; the export kept it and MMD drew the stage through the socks again. The exporter now
> treats a depth-writing material with mesh-sampled cutouts as an alpha test and binarises it at
> `_Cutoff`, regardless of queue. A transparent material with ZWrite Off (the fishnet) keeps its
> fractional alpha. Both cases have export regression tests.

## Symptom

Card `C:\Users\pepsi\Downloads\KK_237456.png` (Yoko Littner, KK + RX21 mods). The internal
CardBridge viewer shows solid pink pantyhose. The exported PMX opened in MikuMikuDance shows the
stage grid through the tights.

## Root cause

`outfit5.socks/o_socks_knee01` uses `Shader Forge/main_alpha` (KKS twin:
`Koikano/main_clothes_alpha` / `low_clothes_alpha`). This is an **alpha-test** shader: it discards
fragments whose alpha is below `_Cutoff` (0.5) and writes depth for everything it keeps. The game
therefore draws the garment solid where the alpha is 190/255 — that value only has to pass the test,
it is not a translucency request.

The UV-flat shader bake writes the shader's **output** alpha into the PMX texture, so the exported
atlas had alpha 190 in the garment, 0 in the empty atlas space, and 255 at the waistband.
`MmdMaterialLook` marked the material transparent (diffuse alpha 0.99) because the texture was not
fully opaque. MMD has no alpha test: it multiplies the texture's alpha into the material alpha, so
the tights rendered at roughly `0.99 × 190/255 ≈ 0.74` in MMD's blended pass (no depth write) and
the stage showed through.

Measured on the reproduction: 2,998 socks triangles; 890 sampled alpha-0 texels (real holes),
1,962 sampled alpha 190 (visible garment), 110 sampled alpha 255 (waistband). The old export left
all 190 texels as partial alpha in the PNG.

## Fix

Two conversion rules in `src/export/Exporter.cpp`, both structural (shader semantics + mesh-sampled
alpha), no card/material names or hardcodes:

1. **Binary cutout alpha** — for an alpha-shader material that is not authored translucent and whose
   mesh actually samples discarded texels (`HasAlphaCutout`), write the PMX PNG alpha as 0 below
   `_Cutoff` and 255 above it. MMD then draws every texel the game kept at full opacity and discards
   only the real holes. The material stays in the 0.99 pass so the binary mask works as a cutout.
2. **Solid alpha for alpha-test without holes** — an alpha-shader material whose mesh never samples
   a discarded texel is solid in the game; export it with diffuse alpha 1.0 and a fully opaque PNG.
   This covers the stock KK/KKS pantyhose atlases whose sub-255 alpha is a uniform tint.

Materials with `translucent` set (the card's IBL_Shader_alpha and friends), genuine cutouts,
face-paint overlays, hair cards, the opaque shader family and the skin coverage mask keep their
existing paths. The existing `main_item` 0.25 cutout and LOD texture reuse are unchanged apart from
the extra dedupe maps.

## Verification

- Fresh export of `KK_237456.png` with the RX21 mods: socks material alpha 0.99, texture alpha now
  only 0 (617,024 texels) and 255 (431,552 texels), zero intermediate values (was 190).
- Stock KK card `132708366_p1.png`: socks material alpha 1.0, PNG alpha all 255; its shorts cutout
  stays 0.99.
- `cardbridge_tests.exe`: 19,061 checks, 0 failed. New/updated coverage: coverage-mask skin alpha is
  solidified, tint-only alpha shaders become opaque, real cutouts keep holes and gain fully opaque
  kept texels, no mid-alpha tint survives.
- Final acceptance — the exported PMX in MMD — must be confirmed by the user; MMD is not installed
  in this environment.
