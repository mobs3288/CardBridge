# Texture quality and bake resolution (0.7.12)

Audit, design, measurements and remaining limitations for the configurable shader-bake
texture quality added in 0.7.12. This is an incremental change: the SM4 shader bake, the
PMX material model and the colour/alpha rules are unchanged.

## 1. What was actually wrong

The audit (`docs/HAIR_FLATNESS_INVESTIGATION.md`, `docs/MMD_COLOR_AND_CLOTHING_OCCLUSION.md`,
`docs/PERFORMANCE_REVIEW.md` and the current source) found these real limits — not a
fundamentally broken renderer:

| Cause | Where | Effect |
|---|---|---|
| Hardcoded 1024 cap halves any larger main texture | `cli/main.cpp` bake call, `Preview.cpp::BakeShaderTextures` | A 2048/4096 source loses detail before MMD ever sees it |
| Shader maps (masks, ramps, gloss) box-shrunk to ≤1024 **at assembly** | `Assembler.cpp::shaderTexture` | Raising the bake cap alone cannot recover this detail |
| One shade sample at the exact texel centre, no AA | coverage + `RunSm4` passes in `Preview.cpp` | Jagged thin strands, lashes and island edges |
| Fixed 6-texel nearest-neighbour dilation, no island ownership | `Preview.cpp` padding loop | Thick fringes; two UV islands that touch can bleed into each other |
| Bilinear LOD-0 sampling with wrap, no island mask | `Preview.cpp::SampleBilinear` | Neighbouring atlas texels can contaminate an edge |
| Mips built but unused by the bake (`mt.lod = 0`) | `Preview.cpp` | No prefiltering of high-frequency source detail |

Not causes: double gamma conversion, double LUT grading, or material diffuse double-multiply —
all three are already guarded (the bake is a gamma-space pipeline; `SceneTexture::colourGraded`
makes the LUT exactly-once; baked materials get `diffuse = white`). Geometry, UVs and rigging
are untouched.

## 2. Changes

- **`--bake-quality standard|high|ultra`** (default `standard`) and **`--bake-size N`**
  (64..8192, overrides only the resolution) are resolved before assembly. Unknown presets
  are rejected with a clear usage error; existing flags and defaults are unchanged.
- The viewer exposes the same three presets in the **Look** tab ("Texture quality"). A chip
  click applies the cap, drops the cached "as exported" bake and reloads the card so shader
  maps are decoded at the new cap. The setting is per-session (the CLI flag is the durable
  one for scripted exports). Verified with `tools/viewer_smoke.ps1 -Only look`, which now
  clicks High and checks the state dump, plus a private off-screen click test.
- `BakeShaderTextures` gained two trailing defaulted parameters (`supersample`,
  `islandPadding`), so the viewer and the diagnostic tools keep calling it exactly as before.
- **Supersampling** renders at up to 2x per output texel and reduces **once** with an
  area-weighted filter (`core/ImageResample.h`). Internal rendering is capped at 4096 per
  side, so an already-large source gets no extra work. This improves coverage of thin UV
  islands and edge colour; **it cannot invent detail that is not in the source**.
- **Island-aware padding** uses a union-find over triangles sharing a vertex within a
  flattened mesh; a texel only inherits from a neighbour in its own island. Padding happens
  at the internal resolution before the reduction, so the fringe stays ~6 output texels.
- **Shader maps** use one area-weighted reduction at the preset cap instead of a power-of-two
  box average. Small sources are never enlarged.
- Reusable primitive: `cardbridge::image::areaResample<T>()` in `src/core/ImageResample.h`
  (float and byte paths, optional validity mask). The standard paths keep their original
  code and stay byte-identical.

### Presets

| Preset | Bake cap | Internal samples | Shader-map cap | Internal render cap |
|---|---:|---:|---:|---:|
| `standard` | 1024 | 1 | 1024 | 1024 |
| `high` | 2048 | 2 | 2048 | 4096 |
| `ultra` | 4096 | 2 | 4096 | 4096 |

Baked dimensions follow the source: `W,H = source`, integer-halved only while the longer side
exceeds the cap, then floored at 64. A 1024² source stays 1024² at every preset; a 4096²
source exports at 1024² / 2048² / 4096² for standard / high / ultra.

## 3. Measurements

Card: `image/demo_card1.png`, library `library_full`, no mods, one machine under normal
development load. Times are the shader-bake stage only.

| Run | Bake time | Textures | Output texture sizes |
|---|---:|---:|---|
| baseline build (HEAD, hardcoded 1024) | — | 36 | 14x 32², 9x 512², 7x 1024², 3x 64², 2x 256² |
| new build, `standard` | 10.2 s | 36 | identical dimensions |
| new build, `high` | 41.1 s | 36 | identical dimensions (no source above 1024) |
| new build, `ultra` | 33.3 s | 36 | identical dimensions (no source above 1024) |

High and Ultra produced identical textures for this card because every source map is ≤1024,
so neither the resolution cap nor the shader-map cap had anything extra to preserve; the 2x
internal supersampling is identical in both. Run-to-run time varies with machine load — the
ratios, not the absolute numbers, are the story.

**Byte identity:** all 41 texture and preview PNGs and the PMX/manifest were compared against
the pre-change build. Every PNG is byte-identical; the PMX and manifest differ only in the
embedded version string (`0.7.11` → `0.7.12`, a pre-existing uncommitted bump). A second
`standard` run was byte-identical to the first (43/43 files).

**Changed pixels at high/ultra:** 19 of 36 textures differ from `standard` (all 17 non-baked
textures — toon/sphere/data maps — are unchanged). Mean RGB and alpha distributions are
stable; a naive mean-gradient metric is very slightly *lower* at high (e.g. face 0.841 →
0.838), which is expected from area reduction: edges are smoother, not sharper. Whether that
reads as "less pixelated" or "more blurred" is a judgement for MMD at real viewing distance,
not for a numeric proxy.

## 4. MMD visual validation (to be completed by the user)

Cards MMD is authoritative; the internal `X` ("as exported") view is only a proxy.

Exports ready for A/B:

- `out/qa_baseline` — pre-change build, equivalent to the new `standard` export
- `out/qa_high` — `--bake-quality high`
- `out/qa_ultra` — `--bake-quality ultra`

Method:

1. Launch standard MMD (verified here: `E:\MikuMikuDanceE_v932x64`) with **no MME/effect**
   assigned, so the comparison tests stock PMX materials only.
2. Load each PMX in turn into the same project: identical camera, light direction/colour,
   pose, background and model scale. Do not change edges or ground shadow between runs.
3. Compare at 100%, 200% and normal viewing distance against a capture of the original
   Koikatsu character. Repeat for a base-game card, a modded card and a KKS card.
4. Fill in the table: face detail/makeup, skin tone, eyes, hair, clothing, transparency,
   toon transitions, overall similarity. Record whether thin hair strands and eyelashes
   improve without artificial highlights or fringes.

No MMD process was running when this work was done, so no visual sign-off is claimed here.

## 5. Remaining limitations

- Higher presets cannot recover detail that was never in the source. Upscaling small textures
  would only interpolate; it is deliberately not done.
- The hair's normal/view-dependent anisotropic highlight still cannot be baked into a UV
  texture. The PMX ring sphere remains a simple add-mode band (`Exporter.cpp::MmdSphereImage`);
  re-deriving it from the material's hair-gloss mask is deferred until MMD A/B shows a reason.
- KKS `Koikano/*` materials still skip the UV-flat bake; their create-job textures keep the
  export-time grade fallback. No screen-space effect is forced into an inaccurate bake.
- PMX toon, sphere, edge, ambient and specular settings are unchanged: the existing unlit
  trick for baked-lit materials already prevents double lighting, and no change was justified.
- No new dependency, no MME requirement, no shader/geometry/UV/rigging change.
