# MMD dark face — root cause and fix

## Reproduction

Card `C:\Users\pepsi\Downloads\KK_235599.png` (Koikatsu, Hinata), exported with the
local library, viewed through the viewer's **as exported / MMD** (`X`) path:

- `out/_face_repro/gpu_view.png` — standard (game-shader) view: face and arms correct.
- `out/_face_repro/gpu_plain.png` — plain (MMD) view: **face greyish-brown, arms black**.

The baked face texture (`textures/baked_cf_m_face_00_42.png`) and the create-job body
texture (`textures/cf_body_00_t_tint_0.png`) are both correct light skin. The exported
PMX materials are also sane (face diffuse `(1,1,1,0.99)`, ambient `(1,1,1)`). So the
defect is in the **plain renderer**, not the bake, the material look or the PMX.

## Root cause

`GpuView::RenderImpl` draws a PMX material's *edge* with `ring()` **after** the surface
draw. `ring()` re-draws the whole mesh with its vertices pushed out in screen space and
a small depth push, in one flat colour (the PMX edge colour).

Two properties of the MMD path make that a full-surface tint instead of a rim:

1. The skin materials export with `diffuse.w = 0.99` (MMD's blended pass). The plain
   renderer puts them in the transparent bucket, which deliberately uses
   `I.depthOverlay` — **no depth write**, as MMD does for blended materials.
2. The edge is drawn *after* the surface, so there is no surface depth for it to be
   occluded by. Every edge fragment passes the depth test and paints the mesh.

Face material edge colour is a dark brown (`0.47, 0.41, 0.38`) → a dark brown face.
Body material edge colour is black → black arms. The painted face overlays (eyes,
brows, mouth) have no edge flag, so they render correctly — exactly the symptom.

Ruling out the other suspects, measured on this card:

- Toon ramps are near-white (`toon0..3.png`); `--no-toon-ramp` still gives a dark face.
- Baked diffuse/ambient and alpha are correct; disabling the edge entirely restores
  light skin immediately (`out/_face_noedge/gpu_plain.png`).

## Fix

Isolated to the plain (as exported / MMD) branch of `src/export/GpuView.cpp`:

- Draw the PMX edge **before** its surface (back faces only, expanded in screen space),
  then draw the surface over it. The surface covers the edge's interior; only the
  expanded rim outside the silhouette remains. This works whether or not the surface
  writes depth, so it is correct for opaque and blended materials alike.
- `ring()` takes an optional rasterizer state; the plain path passes
  `I.rasterCullFront` (MMD's back-face edge). The default remains `I.raster`, so the
  **standard viewer's outline path is byte-for-byte unchanged**.
- `ring()` swaps shaders/input layout/constant buffers; the surface's bindings are
  restored before its draw.

No material defaults, bake, colour grading, PMX writer, or standard-view code changed.
No card-, shader- or skin-colour-specific values are involved. The rule depends only
on the PMX edge flag and edge size, so it applies to KK, KKS and any other card.

## Verification

- `out/_face_edgebefore2/gpu_plain.png`: face, neck, arms and feet have the correct
  skin tone; a thin dark rim remains around the silhouette (legitimate PMX edge).
- `build/bin/cardbridge_tests.exe`: regression test renders a blended, edged skin
  material through WARP in plain mode and asserts the surface colour reaches the
  framebuffer rather than the edge colour. All checks pass.
- Standard view output is unchanged (shared `ring()` behaviour preserved).
