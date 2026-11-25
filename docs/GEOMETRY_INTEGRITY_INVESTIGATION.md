# Universal Geometry Integrity Investigation — "necktie distortion" (2026-11-25)

## Question

Does CardBridge distort converted geometry (the school-uniform necktie being the reported example),
and if so, where? The requirement was to check the *whole* pipeline and *every* geometry category,
not just the tie.

## Verdict

**No geometry-conversion defect was found.** The source mesh, the assembled scene mesh, the live
preview and the exported PMX all carry the same vertices, the same topology and the same shape.
The necktie looks short in the CardBridge view because the game draws it hanging from a
DynamicBone chain (gravity) while CardBridge intentionally exports the card's *authored rest
pose*. That is a physiology/drape difference, not a conversion error, and it is a documented
design decision (`src/assemble/Pose.h:85-87`) — the world-gravity outfit hang bake was removed
because it made the authored silhouette unrecoverable.

No core code was changed. Changing it to "fix" the tie would either be a character-specific
visual patch (forbidden by the request) or a global re-enable of cloth physics that the code's
own notes record as wrong for 3 of 4 cards (`src/assemble/Assembler.cpp:3732-3734`).

---

## 1. Categories investigated

| Category | How it was checked |
|---|---|
| Upper-body clothing / sailor top (`top.sub_a`, `sub_b`, `sub_c`) | source-vs-assembled vertex arrays, connected components, PMX material bounds |
| Lower-body clothing (`bottom`, `shorts`) | PMX per-material bounds/centroids, vertex counts |
| Inner clothing / bra | PMX material 14 bounds, weight audit |
| Socks / shoes (stock + mod) | PMX materials 16-18, heel-warp path read |
| Accessories (11 slots, base + 6 mods) | PMX materials 19-30, `accessoryBone` / attach-node path read |
| Hair (base + mods) | PMX accessory materials, `accessoryMode` skinning path read |
| Body (`o_body_a`) and under-pieces | PMX materials 0, 35-37, source bundle read |
| Face, eyes, brows, mouth, tongue, teeth | PMX materials 1-12 |
| Facial effects / morph overlays | PMX materials 38-40 |
| Genital geometry (`kokan`, `body.under.kokan`) | PMX material 37 |
| Mesh categories with non-identity transforms / hierarchies | `addRenderer`: `bw = boneWorld`, `bp = inverseBindPose`, `boneMat = bw*bp`, `pos = Σ w·boneMat·p`; `Reshape` re-skin path |

The complete material inventory (names, counts, bounds, centroids) is in
`tools/_geom_audit.py`; `tools/_geom_components.py` and `tools/_render_mesh.py` were used to
separate and visualise individual components inside a material.

## 2. Which mesh "demonstrated the defect", and what it actually is

- Source card: `151506026_p6.png` (Koikatsu, outfit 0 "School Uniform").
- Exported PMX: `古都吹 寿子.pmx` (46,982 vertices / 67,401 triangles / 41 materials).
- The reported necktie is part of `outfit0.top.sub_c` — mesh `o_top_sailor01_03_a`, source
  bundle `chara/cpo_sailor_c_00.unity3d`, 500 vertices / 858 triangles.

`tools/_render_mesh.py` on PMX material 34 renders a **short tie** (the collar V plus a short
strip) — matching the viewer screenshot. But `tools/_geomcheck.cpp` reads the *source* mesh
from the bundle and the assembled scene mesh and shows they are the **same shape**:

| | source mesh (bundle) | assembled scene | PMX (×12.5) |
|---|---|---|---|
| vertex count | 500 | 500 | 500 (material 34) |
| x range | [-0.084, 0.084] | [-0.082, 0.082] | [-1.10, 1.09] |
| y range | [0.034, 0.307] | [1.025, 1.272] | [12.81, 15.87] |
| z range | [-0.001, 0.114] | [-0.000, 0.103] | [-1.29, 0.00] |
| height | 0.273 m | 0.247 m | 3.06 units |

`0.273 × 0.904 = 0.247`: the only change is the card's own uniform body-slider scale
(all four `cf_j_spinesk_*` bones that skin this mesh have world scale **0.904, 0.904, 0.904**).
An affine least-squares fit of scene vertices onto source vertices gives residuals of
~1.6 mm mean / 7.8 mm max, entirely explained by the remaining secondary bone weights.
`out/_source_o_top_sailor01_03_a.csv` and `out/_scene_o_top_sailor01_03_a.csv` contain the
raw arrays; `_render_csv.py` renders both as identical point clouds apart from translation.
Topology (858 triangles), vertex order, UVs and weights are unchanged. The exporter copies
each scene vertex verbatim (`src/export/Exporter.cpp:877-883`), applying only the uniform
`opt.scale = 12.5`.

The game's long tie comes from the `cf_j_spinesk_00 … _05` DynamicBone chain, which is
attached to this very outfit item (`_geomcheck.cpp` chain dump: `own=0,
part=outfit0.top.sub_c`). CardBridge never settles outfit chains: `PhysicsSim::Reset` skips
non-`characterOwn` chains when settling (`src/assemble/PhysicsSim.cpp:76`), and
`BakeSettledPhysics` only handles bust/hip jiggle. `CARDBRIDGE_DYNBONE` is off by default for
hair/accessories because the settle "hurt the others" (`Assembler.cpp:3732`). So the tie is
authored-short in the rest pose — that is the difference visible in the screenshot.

## 3. Stage where geometry first diverges

**None — there is no divergence in the conversion pipeline.** Checkpoints:

1. **Extraction** — `UnityScene.cpp::readMesh` decodes vertex channels by declared format;
   `tools/_geomcheck.cpp` confirms the source mesh bounds and first vertices.
2. **Transform handling** — `Assembler.cpp::addRenderer` uses standard skinning
   `bw * inverseBindPose` with per-bone weights; the tie's bind poses are rigid (unit scale).
   `tools/_geomcheck.cpp` prints the bone scales used by the mesh.
3. **Assembly / merging** — no vertex dedup or remapping in `Assembler`; submeshes keep their
   authored indices. `Reshape()` (`Assembler.cpp:4655`) is only entered for a shape edit and
   re-derives locals from the same bind frames.
4. **Pose bake** — `Pose.cpp::BoneWorlds(scene, Pose{})` is a pure-translation skeleton with
   **correct bone positions** (0 translation mismatches over 849 real bones) and
   `SkinMatrices(scene, Pose{})` is identity on all 849 bones (max deviation 1.5e-8). The
   synthetic unit tests at `tests/cardbridge_tests.cpp:4136` therefore hold for real scenes.
   `BakePose` with a real A-pose rotates limbs about each bone's own position and does not
   rescale geometry.
5. **Export** — `Exporter.cpp:877-989` copies vertices (`pos * scale`) and indices verbatim,
   with no dedup, no remap, no per-mesh scaling.
6. **Preview vs PMX** — the viewer (`GpuView.cpp` / `Preview.cpp`) consumes the same scene;
   the independent PMX renderer (`tools/_render_mesh.py`) reproduces the same short tie, so
   preview and export agree.

## 4. Shared components reviewed (no defect found in any)

- `src/unity/UnityScene.cpp::readMesh` — channel decoding, blend shapes, bind poses.
- `src/assemble/Assembler.cpp::addRenderer` — skinning, accessory attach, static fallback,
  heel warp, blend-shape deltas.
- `src/assemble/Assembler.cpp::Reshape` — re-skin on shape edits; only `reshapable` meshes.
- `src/assemble/GarmentFix.cpp::FixGarmentIntersections` — **off by default**, moves only
  penetrating vertices by their penetration depth + clearance.
- `src/assemble/GarmentLayers.cpp::SeparateCoincidentLayers` — defined but not called by
  `Assemble` (the coincident cape separation was reverted in `d9a1adc`).
- `src/assemble/Pose.cpp` — `BoneWorlds`, `SkinMatrices`, `BakeSkin`, `BakePose`,
  `BakeSettledPhysics`; verified identity at rest on the real scene.
- `src/export/Exporter.cpp` — vertex/triangle copy, PMX serialization.
- `src/export/Preview.cpp`, `src/export/GpuView.cpp` — preview rendering.

## 5. Root cause classification

| Symptom | Class | Root cause |
|---|---|---|
| Short necktie vs card image | **Not a conversion defect** | Rest pose vs the game's DynamicBone hang on `cf_j_spinesk_00…05`; outfit-chain settle is intentionally disabled |
| Body 9.5 % smaller than card's default proportions | Expected | The card's own body sliders give all spine-skin bones scale 0.904; uniformly applied |

## 6. Why no character- or item-specific patch was applied

A universal correction would require either (a) re-enabling outfit DynamicBone settling, which
the code explicitly records as harmful for most cards and which changes the authored drape, or
(b) scaling/positioning one mesh to match one card image — precisely the "clothing-specific
scaling / character-specific correction" the request forbids. Neither preserves source
geometry; both *change* it.

## 7. Before/after measurements

| Quantity | Source | Assembled | PMX |
|---|---|---|---|
| vertices (`o_top_sailor01_03_a`) | 500 | 500 | 500 |
| triangles | 858 | 858 | 858 |
| x extent (m) | 0.168 | 0.164 | 0.165 (÷12.5) |
| y extent (m) | 0.273 | 0.247 | 0.245 (÷12.5) |
| z extent (m) | 0.115 | 0.103 | 0.103 (÷12.5) |
| dominant bone scale | — | 0.904 uniform | — |

Because no root-cause defect was found, there is no "after" build; the existing test suite is
the regression evidence.

## 8. Visual verification

- `out/tie_only.png` — independent render of PMX material 34; matches the viewer's short tie.
- `out/tie_source_points.png` / `out/tie_scene_points.png` — source and assembled vertex
  clouds; identical shape, only translated.
- `C:\Users\pepsi\OneDrive\Desktop\古都吹 寿子\古都吹 寿子_preview.png` and `_view.png` —
  both show the same tie as the PMX, i.e. preview and export are consistent.

## 9. Automated regression results

`build\bin\cardbridge_tests.exe`: **19,473 checks, 0 failed** (2 integration suites skipped —
they need `CARDBRIDGE_TEST_LIBRARY/_MODS/_CARD`). No core source files were modified, so
`VERSION` and all pre-existing uncommitted files remain untouched.

## New diagnostic artifacts (untracked, no runtime effect)

- `tools/_geom_audit.py` — per-material PMX vertex/triangle/bounds/centroid dump.
- `tools/_geom_components.py` — connected-component analysis of a PMX material.
- `tools/_pmx_verts.py` — PMX bone and per-vertex weight dump.
- `tools/_tie_probe.py`, `tools/_front_profile.py`, `tools/_render_mesh.py`,
  `tools/_render_csv.py` — targeted shape/occlusion checks.
- `tools/_geomcheck.cpp` — assembles a card and prints source vs scene mesh geometry, bone
  scales, bind poses and DynamicBone chains.
- `tools/_poseworld.cpp` — checks rest-pose skeleton/skin matrices on a real card.

## Open question for the owner

If the desired output is the *hanging* tie seen in the card thumbnail rather than the authored
rest drape, that is a deliberate behaviour change (enable cloth-chain settling for outfits),
not a geometry bug. It needs a separate decision because the existing notes say it damaged
most other cards. The geometry itself is already faithful.
