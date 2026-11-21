# MMD face transparency / back-of-head bleeding — root cause and fix

## Reproduction

Card `C:\Users\pepsi\Downloads\KK_235599.png` (Koikatsu, Hinata). The **as exported / MMD**
view (plain mode) showed a translucent face: patches of lighter geometry showing through, and
dark triangular artifacts around the mouth (screenshot `Screenshot 2026-11-21 222830.png`).
The standard viewer was correct. Close-up: `out/_trans_face3/gpu_plain.png` (before) vs
`out/_trans_fixed2/gpu_plain.png` (after).

## Root cause — material classification, not a rendering-state bug

The character's skin is built by a game "create" job. Its alpha channel is a **coverage mask**
for the clothes, not transparency; the game draws skin opaque and ignores that alpha.

`MmdMaterialLook` (the shared source for the PMX material and the plain viewer) classified a
material as an alpha cutout whenever a triangle's UV centroid landed on a low-alpha texel,
for every shader except the "opaque" family. The baked face texture has alpha holes (the
mask), so after baking the face was classified transparent and exported with
`diffuse.w = 0.99`. In both the PMX and the viewer that means MMD's blended pass:

- no depth write → geometry behind the face can show through;
- the fallback shader alpha-gated those texels → the dark triangles around the mouth.

Disabling the edge pass did not help; forcing the face lookup opaque did. Confirmed by
instrumenting the classifier on the reproduction and on KKS cards:

| material | shader | alpha flag | result before |
|---|---|---|---|
| KK face `cf_m_face_00` | `Shader Forge/main_skin` | 0 | cutout → 0.99 |
| KK/KKS body `material` | *(empty, create job)* | 1 | blended → 0.99 |
| KKS face `Koikano/main_skin` | skin family | 0 | cutout → 0.99 |

## Fix

- `src/assemble/Scene.h`: new `IgnoresTextureAlpha(scene, mat)` — the game ignores texture
  alpha for the opaque shader family **and for the character's own skin**, recognised
  structurally: a skin-family shader, or a shader-less create-job material used by the
  character's `body`/`head` part. No card, texture, material index or hardcoded colour.
- `src/export/Exporter.cpp::MmdMaterialLook`: skin can no longer be marked a cutout or made
  transparent by its alpha flag / texture mask. Face and body now export with PMX alpha 1.0;
  under-pieces, face FX overlays, hair cards and genuinely translucent materials are untouched.
- `src/export/GpuView.cpp`: in plain mode the fallback shader no longer alpha-clips skin.
  The standard viewer's alpha test is deliberately left as it was (the new rule applies only
  when `look` is present, i.e. in MMD/plain mode).

The software renderer and the game-shader path are unchanged; this is an MMD/material-look
fix, not a global opacity override.

## Verification

- `out/_trans_final/gpu_plain.png`: face fully opaque, no dark mouth triangles, texture and
  shading intact, eyes/brows/mouth intact; other materials unchanged.
- PMX: `head/cf_O_face#0` and `body/o_body_a#0` diffuse alpha 1.000 (was 0.990); under pieces
  and FX overlays keep their alpha 0.000 / 0.990.
- KK (`KK_235599.png`, `139306275_p3.png`) and KKS (`120285586_p25.png`, `120324154_p2.png`)
  cards: skin exports opaque, legitimate cutouts/blends unaffected.
- `cardbridge_tests.exe`: 19,006 checks, 0 failed. New coverage:
  (1) structural classification of KK/KKS face+body skin vs hair/alpha/opaque materials;
  (2) WARP render — an opaque skin quad occludes a dark panel behind it in MMD mode;
  (3) the existing edge regression test now uses a genuinely blended overlay.
