# Koikatsu Ambient Occlusion — Investigation Report and Skin-Shade Bake Plan

Status: investigation complete (no implementation yet). Approved scope at the bottom.

Date: 2026-11-12. Relates to `--skin-shade` (the baked ray-cast crease shading in
`src/export/SkinShade.cpp`) and to the flat look of the exported PMX.

---

## 1. The premise, and why it fails

The task assumed Koikatsu ships **built-in ambient occlusion textures** that can be
extracted and multiplied into the skin texture to get crease definition — the AO
that, once baked into the PMX's referenced PNGs, would work in MikuMikuDance and any
other PMX viewer with no custom shaders.

**There are no such textures anywhere in the base game data.** This was verified,
not assumed, by decoding the bundles with CardBridge's own Unity 5.6 parser
(`UnityBundle`/`SerializedFile`/`UnityFile::readTexture`) and cross-checking against
the game's live runtime state and its own DLL/shaders:

- Every `ao_*` bundle in `abdata/chara` contains **accessory textures**
  (`acs_*`), not ambient occlusion:

  | Bundle | Contents (sample) |
  |---|---|
  | `ao_body_00` | `acs_back_LR_Bwing_00`, `acs_guitorCase`, `acs_schandbag`, `acs_nip_clip`, `Ramp_StateLine60` |
  | `ao_body_09` | `acs_armor_body_t/md/mc/n` (accessory body armour) |
  | `ao_face_00` | `acs_ear_*`, `acs_megane*`, `acs_3rdeye_n`, `acs_idolincam_mc` |
  | `ao_face_53` | `acs_ear_chaina_t/md/mc/n` |
  | `ao_hand_*` | `acs_hand_*` (pitchforks, rackets, cheering balls), `t_hs_ha_analvibe_00_t`, `t_hs_ha_den_00_n` |
  | `ao_arm_*` | `acs_watch01_*`, `acs_bangle_*`, `acs_armband_*`, `acs_hand_cuffs_n` |
  | `ao_hair_*` | `acs_pin_*`, `acs_ribbon_*`, `acs_flower_*` |
  | `ao_head_*` | `acs_headphone_01_*`, `acs_crown_mc`, `acs_hachimaki_mc` |

  These are the adult-only/extended accessory bundles; the prefix `ao` does not
  stand for "ambient occlusion". A scan of **all 297 bundles** in the extracted
  base library (`findtex` probe over every Texture2D name) found **no**
  AO/occlusion-named texture.

- The body skin base (`cf_body_00_t`, in `oo_base`) is a **flat white** 2048²
  texture; the skin colour is composited by the game at runtime into
  `cf_m_body_create` (two skin colours + colour mask + paint/sunburn/nipple/
  underhair overlays). Nothing crease-like is in the texture.

- The face base (`cf_face_00_t*`) is the same story.

- `_ambientshadowG` (and the Studio "ambient shadow" slider) is a **global Studio
  environment property** (`Manager.Character::UpdateGlobalShader` in the game's
  Assembly-CSharp.dll → `ChaShader.ChangeAmbientShaodwColor`), not a per-character
  baked texture and not a data file. It darkens the whole scene's toon shade for
  characters; it cannot be, and is not, a body-AO lookup.

- `cf_body_00_md`, `cf_face_00_md(2)` (`_DetailMask` / `_NormalMask`) are packed
  detail masks consumed by the toon shader for its effect parameters (mostly
  specular-line behaviour). They are not an occlusion map either.
- `cf_body_01_mp`, `cf_face_00_mp` are plain silhouette cut-out masks.
- `mt_body_paint_*` / `mt_body_detail_00` are tattoos, paints and **detail presets**
  (normal map + line mask), not AO.
- `mt_mask_body_*` are clothing-region masks.

## 2. Where the cleavage/under-bust definition actually comes from

Verified against a real live-game dump (BepInEx sidecar, `out/env_dump/KK_138747/renderers.json`):

**Body material (`cf_m_body`, shader `Shader Forge/main_skin`) — what the game actually binds at runtime:**

| Slot | Texture (source bundle) | What it is |
|---|---|---|
| `_MainTex` | composed skin (runtime RenderTexture, 2048²) | flat tint — no crease |
| `_NormalMap` | **`cf_body_00_base_n`** (`mm_base`, 1024², BC5) | soft body normal map: under-bust valley, rib/abs bumps, navel bump, collarbone — *the strongest anatomy cue in the data* |
| `_NormalMapDetail` | card's body **detailId** row → `NormallMapDetail` (here a mod: `cf_body_06_n`) | detail normal (very soft; 8×8 blank by default) |
| `_LineMask` | card's body **detailId** row → `LineMask` (here a mod: `cf_body_06_line`; base-game default `cf_body_00_line`) | ink-line + faint shadow mask; the mod's B channel carves the under-bust shadow and its R channel paints the sternum cleavage line |
| `_DetailMask` | `cf_body_00_md` (`mm_base`, 512²) | packed detail mask |
| `_NormalMask` | `null` | — |
| `_overtex1` | `mt_nipple03` | nipple overlay |
| `_Texture2/3`, `_liquidmask` | `liquid01_*`, `cf_body_01_ml` | wet-skin gloss layer |

The male body (`cm_m_body`) is bound the same way with `cm_body_00_n`.

The face (`cf_m_face_00`) binds `_DetailMask = cf_face_00_md` (bo_head_00),
`_NormalMask = cf_face_00_md2`, no `_NormalMap`, no `_LineMask`; face detail
(category 400) exists and is selected the same way.

**How the game resolves the detail textures** (decompiled from Assembly-CSharp.dll,
`ChaControl.ChangeSettingBodyDetail` / `ChangeSettingFaceDetail`):

1. `ChaControl.ChangeTexture(material, categoryNo, detailId, **30**, keyType, ...)`
2. body → category **420** (face → 400), the card's `detailId`
3. list row keys: `NormallMapDetail` → material slot `_NormalMapDetail` (KeyType 30, 44),
   `LineMask` → slot `_LineMask` (KeyType 30, 27)
4. get the texture from the list row's bundle (`MainAB`), load `cf_body_06_n` / `cf_body_06_line`
5. `_linetexon` defaults to 1 and `_linewidth` ≈ 0.5, so the line mask is *always* live on the body

Base-game detail rows (category 420, from the base library list tables):

| detailId | Name | NormalMapDetail | LineMask |
|---|---|---|---|
| 0 | なし (none) | `cf_body_00_n` (8×8 blank) | `cf_body_00_line` (clavicle, knees, fingernail lines only; nothing in the breast region) |
| 1 | ムキムキ (muscular) | `cf_body_01_n` | `cf_body_01_line` (pecs/abs ink lines) |
| 2 | ポッチャリ (plump) | `cf_body_02_n` | `cf_body_02_line` (weak; navel marker only) |
| 3 | スレンダー (slender) | `cf_body_03_n` | `cf_body_03_line` (ribs, sternum line, under-breast shading) |

Modded rows (e.g. the BetterRepack RX21 body kit on the dumped card) add ids ≥ 4,
e.g. the woman the card was dumped with uses `cf_body_06_line`, whose **R** channel
is a faint sternum cleavage line, **G** is the standard ink lines, **B** is a dark
under-bust/body-silhouette shadow carve — the visible cleavage shading on that
character comes from exactly that texture, scaled by `_linewidth` (0.5) and mixed by
the toon ramp.

**Bottom line:** the cleavage/crease look is not a single AO texture. It is the
sum of (a) the always-bound base body normal map`cf_body_00_base_n`, shading under the
fold via the toon lighting, (b) the optional detailId line mask, and (c) the vertex
normals. CardBridge bakes **none** of these today (it composes the skin colour and
leaves `_NormalMap` / `_LineMask` unbound), which is why its exports read flat —
most visibly at the under-bust fold.

## 3. What the task can therefore *not* do, and what it can

Cannot:

- "Clone `ao_body_00` → multiply into the skin" — the file does not contain an AO map.

Can (and the user approved this scope on 2026-11-12):

1. **Bake the base body normal map** — sample `cf_body_00_base_n` in UV space and
   derive a local-curvature (cavity) factor from the neighbourhood contrast —
   creases, joints and contact folds darken, flat skin is untouched, and no
   view/light direction is baked in (see §4.4 for why the directional variant was
   rejected on validation).
2. **Bake the game's default line mask** — `cf_body_00_line` (always on in game),
   multiplied in with a strength knob.
3. **Bake the card's detailId masks** — resolve the body `detailId` via the list
   tables (category 420, `NormallMapDetail` / `LineMask` keys), exactly as the game's
   `ChangeSettingBodyDetail` does, and bake them into the skin texture.

Hard requirement (user's): the bake must live in the exported **PMX's referenced
PNGs** — i.e. applied to `scene.textures` in the assembler like `ApplySkinShade`
and `RunCreateJobs`, so both the viewer and `cardbridge convert` produce it, and
MMD / Project K / any PMX viewer shows it with no CardBridge-specific rendering.

## 4. Implementation plan (not implemented yet)

### 4.1 Where the bake plugs in

`src/assemble/Assembler.cpp` → after `RunCreateJobs(scene)` and
`ApplyNippleTint`, before `report(0.96, "finishing")` — same place the existing
`ApplySkinShade` call lives. The body skin texture (`scene.skinShade.texture`, the
same index `ApplySkinShade` bakes into) is the target.

Reused infrastructure (no new texture plumbing):

- `scene.skinShade.meshes` — the body skin mesh(es) for UV coverage rasterization
- `RasterizeToUv` / a variant — per-texel coverage + tangent-space normal lookup
- `FeatherMap` / `BlurMap` — edge feathering of the baked map
- `ss.base` snapshot — keep a copy of the texture before any bake multiplies it
  (extend `SkinShadeScratch` or hold a local `std::vector<uint8_t>`)
- alpha channel untouched (write RGB only)

### 4.2 New module

`src/export/SkinAoBake.h/.cpp` (name TBD) with e.g.:

```cpp
struct SkinAoBakeOptions {
    float baseNormalStrength = 0.8f;   // cf_body_00_base_n influence (0 = off)
    float lineMaskStrength   = 0.65f;  // cf_body_00_line ink lines (0 = off)
    float detailStrength     = 1.0f;   // the card's detailId masks (0 = off)
    float lineKeep           = 0.0f;   // ink-line floor; 0 = multiply to black OK? (see 4.4)
    const char* detailBundleHint;      // from the list row MainAB
};
bool ApplySkinAoBake(assemble::Scene& scene, const SkinAoBakeOptions& opt);
```

The baking runs **after** `ApplySkinShade` multiplies its crease shade into the
texture (or fold both into one pass — see 4.5), so the two multiply correctly
and the "off" path stays bit-exact with today's output. They must never
**stack** into double darkening — see 4.4.

### 4.3 Getting the game's textures

- `mm_base.unity3d` (resolver base bundle, already opened for shaders; the
  resolver caches it) → `findTexture("cf_body_00_base_n")`,
  `findTexture("cf_body_00_line")`, `findTexture("cf_body_01_ml")` (not needed),
  the male `cm_body_00_n` if the card is male (check `card.header` / `sex`
  — cf vs cm). `unityFormat=12` is BC5/DXTnm — `TextureDecode` already decodes
  BC5 (used by `_NormalMap*` today).
- The detail row: `resolve::ListTable` row at `(category=420, detailId)` with keys
  `NormallMapDetail` / `LineMask` / `MainAB`. Details resolved through the same
  `rowTextureFor(...)` path used for paint/sunburn/nipple overlays, but without
  requiring a card colour — the line texture is used as a raw multiplier
  (`_lineMask.G`), never tinted.
- `detailId` is in `card.body` (`detailId`, default 0). The 4 base rows are in the
  base library; modded rows work the same once the mod's list is installed, which
  matches the game.

### 4.4 Roles, blending contract and baking model

**Division of labour (fixed, not configurable):**

- `SkinShade` keeps owning **geometry-driven cleavage and under-bust shading**
  (ray-cast cavity + midline band + underboob fold). Its controls
  (`--skin-shade`, `--skin-underboob`, `--skin-top`, `--skin-width`, the Look/Setup
  rows) are preserved untouched — same semantics, same defaults.
- The new bake adds **anatomical depth** from Koikatsu's own data
  (a local-curvature cavity factor off the base body normal map — no light direction
  is baked in, so it cannot fight MMD's lighting — and the card's detailId detail
  normal folded into the same local vector, exactly as `_DetailNormalMapScale`
  does) and **Koikatsu's original body details** (the ink-line channel `G` of the
  default line mask `cf_body_00_line`, and of the card's `detailId` `LineMask`).
  **Collarbone damping:** the line mask used with this bake finds its own
  clavicle band (the top-third row with the widest G\>60 run, no fixed atlas
  coordinates) and damps the ink-line multiplier inside it to \~0.35 so the
  collarbone strokes stay subtle without muting any other ink.
  **Limb dampening:** vertices bound mostly to the arm or leg chains (any bone
  whose name contains `leg`, `shin`, `thigh`, `knee`, `arm`, `forearm`, `elbow`,
  `wrist`) take ~25% of the bake's effect so the elbow and knee joints stay
  faint; torso, head and hands keep full strength (the crease accents worth
  seeing live in those regions).

**Blending contract (mandatory — no stacking):**

- The new bake must not double-darken texels already shaded by `SkinShade`. It
  reads the same `ss.base` snapshot and the `ss.shade`/`ss.shadeU`/`ss.covered`
  maps, and **attenuates its own contribution where `SkinShade` already darkened**:
  - cavity term: `occ = clamp((dev - 0.075f) / 0.25f, 0, 1)`, where
    `dev = length(nLocal(center) - nLocal(neighbourhood average))` is how much the
    texel's normal curves away from the covered texels around it (radius 8,
    covered-neighbour-only); `shade = 1 - 0.28 * occ^1.6`, floored at strength
    application to 0.30 and attenuated by `att = max(0, 1 - 0.7 * existingDark)`
    with `existingDark = 1 - splitMul` as before. Flat atlas areas see almost no
    darkening (measured mean 0.995 over the body, ~2-3% of texels marked).
  - line mask term: only `G`, and **scaled × 0.55** so the navel, clavicle and
    knee strokes read as faint accents; multiplied in with the cleavage/fold
    band's attenuation factor (same `shade` band structure) so lines never land
    on the already-dark crease and produce an artificial black stripe.
  - line mask term: multiplied in with the cleavage/fold band's attenuation factor
    (same `shade` band structure), so the ink line never lands on the already-dark
    crease and produces an artificial black stripe — it only draws where the skin
    is (near-)lit or weakly shaded. Only the ink-line channel (G) is used; the R/B
    passes are faint rim/carve data for the game's lighting and multiply wrong
    into a diffuse texture.
  - the detailId `_NormalMapDetail` inherits the same cavity attenuation.
- Hard floor: the final per-texel multiply never goes below **0.30** combined
  (both bakes together), so no fold can crush skin tones to black; SkinShade's
  existing 0.04 floor is respected by keeping its factor authoritative inside its
  own band (the new bake yields to it, never the reverse).
- No new crease/line drawing into SkinShade's midline band at all — the sternum
  line from a mod's `_LineMask.R` (if any) is attenuated by the same band factor.
- Alpha channel always untouched (write RGB only), skin colour customization
  preserved (the bake multiplies the composed colour; it never writes the base
  colour).

**Per covered body texel** (UV space — same coverage as `ApplySkinShade`):

1. Sample `cf_body_00_base_n` at UV (x,y like the other bakes: row 0 = v=1),
   decode DXT5nm (X in alpha, Y in green, Z reconstructed), and keep the tangent-
   space vector. If the card's detailId row has a `NormallMapDetail` (not the
   8×8 blank at the default), blend it into the same local vector exactly as the
   game's `_DetailNormalMapScale` does.
2. Cavity shading (no fixed light — see "Why not a directional bake" below):
   radius-8 neighbourhood average over **covered texels only**,
   `dev = |nLocal - avg|`, `occ = clamp((dev - 0.075) / 0.25, 0, 1)`,
   `shade = 1 - 0.28 * occ^1.6`. Strong creases and joints (knees, shins, groin,
   feet, elbows) sit at 0.72–0.90; flat atlas regions read 1.0 and are untouched.
3. Line mask: `line = cf_body_00_line.G x 0.55` (at the default; the card's detailId row's
   `_LineMask.G` when present, same scale). Multiply in with `bandAttenuation` — the ink lines
   the game draws with `_linewidth`.

Order: `SkinShade` (existing, unchanged) → attenuated cavity AO → attenuated
line multiply. All write into the same texture the PMX exporter references;
recompute `pixelHash` afterwards (like `ApplySkinShade`).

**Why not a directional bake (measured, 2026-11-12).** An earlier iteration lit the
base body normal with a fixed front/top key light (wrap curve + smoothstep) and a
normal-based diffuse multiplier applied over the torso. Validation on the real card
(`KK_138747`, export inspected directly) showed it crushed every region not facing
the light — half the organism (up/down-facing normals) went to the 0.70 floor — and
produced a heavy sepia wash over the whole body, far beyond any "form shading" MMD
could tolerate; a second iteration (cavity over all local curvature) darkened ~15%
of texels including convex breast mounds. Both were rejected. The shipped cavity
factor restricts darkening to high-curvature creases and cannot stack on MMD's own
scene lighting by construction (no light, view or axis in the math at all).

### 4.5 Configuration

Consolidate under the existing `--skin-shade` family rather than adding a new
surface:

- `SkinShade` knobs stay exactly as they are (control and behaviour preserved).
- shipped: `--skin-ao F` — the **master strength of the new bake** (cavity AO +
  the card's `detailId` masks), default 0.85; 0 = off → output byte-identical to
  today's export. It scales the *new* contribution only; the overlap attenuation
  (4.4) is always on and is not a separate knob.
- the bake runs only when the skin is being composed anyway (after
  `RunCreateJobs`/`ApplySkinShade`, GUI and CLI alike); `--skin-shade 0` with
  `--skin-ao F > 0` still produces the new-only bake (useful for A/B comparison).
- viewer Look tab: one new row "Koikatsu AO (base body normal + detail lines)"
  wired to the same `--skin-ao` value; Setup persists it in settings like the
  `skinShade*` knobs; both persist across sessions (settings json).
- no `--skin-ao-normal`/`--skin-ao-line` split — one knob, per the plan; the
  channel weights are fixed in code.

### 4.6 Validation (measure, don't assume)

All three runs share the identical card (`KK_138747`, as the dumped live-game dump
used) and the identical CLI invocation `cardbridge convert … --skin-ao 0 (combined
off) / default 1.5 1.0 / 0.85 combined`, exported into
`out/ao_bake_test_off`, `out/ao_bake_test_shade`, `out/ao_bake_test_full5`.
The metric is the **exported PMX's referenced body skin PNG** (`baked_cf_m_body_111.png`),
visualised directly (`out\ao_bake_test_*\textures\baked_cf_m_body_111.png`), never the viewer.

Measured (2026-11-12):

| Check | Result |
|---|---|
| off export (`--skin-ao 0`) vs pre-feature output | unchanged code paths; the bake is skipped entirely (no texture writes) |
| alpha off == full | byte-identical (`off==full5`, all three runs) |
| full vs shade-only | mean |Δ| 0.81, p95 2, p99 24, max 158 (158 on the toes/groin ink lines); 3.8% of pixels Δ>5 |
| full vs off | mean |Δ| 1.43, p95 8, p99 35, max 158; 7.8% pixels Δ>5 |
| covered-only modification | corners max Δ = 0.0 |
| skinShade tests | unchanged, green |
| unit tests for the bake | cavity marks the normal-map stripes and spares flat regions; line mask darkens; strength 0 restores byte-exact; attenuation yields to the full crease |
| shins/groin/toes/clavicle ink | visible (clavicle stroke + navel dot, toe definition, faint knee arcs), no wash at any region |
| rejected: directional front/top light | abandoned — half the organism crushed to the 0.70 floor, sepia wash |
| rejected: cavity at the low 0.03 threshold | abandoned — convex breast mounds marked |

Also verified by the runs: the modded-detail path (the `detailId` row resolved
`cf_body_06_line` through `rowTextureFor` and its ink-line channel baked;
attenuated lines), the base-normal cavity only, the base-library line-mask
fallback path (default row ids), and the `--skin-ao 0` no-op path.

Still open for the owner to confirm when they open the PMX: the "does it feel
right in MMD" call — the bake is orientation-independent but static; it never
chases the MMD scene light (by construction) — by design it shows the same
creases in every viewer.

### 4.7 Definition of Done (revised)

A character exported through CardBridge produces a PMX whose **referenced body
skin PNG** shows the base body normal-map shading, the game's default (and, when
the card has one, its `detailId`) line mask baked in — visible in standalone MMD
with no CardBridge support. The spec's literal "extract the AO textures" step is
struck: they do not exist; the above is the functional equivalent, implemented
from the game's real data path.

Additionally, the combined result must be **complementary to `SkinShade`, never
stacked on top of it**: `SkinShade` remains the owner of the geometry-driven
cleavage/underboob shading and its controls are untouched; the new bake yields to
it inside its band (4.4), the combined multiply has a hard floor (0.30), no
texel double-darkens versus the SkinShade-only output in the overlap region, and
the three-way validation (off / `SkinShade`-only / combined) shows the new
contribution *adds* anatomical definition without degrading any existing result.
Both feature sets are baked into the exported PMX's referenced skin PNG, for both
GUI and CLI exports.

## 5. Driver notes (hard evidence, for whoever implements)

- `ao_*` inventory: see §1 (probe: `ao_probe` / `listprobe` / `findtex` against the
  extracted library; scratch files were left in the session temp dir, not the repo).
- Live runtime state: `out/env_dump/KK_138747/renderers.json` + `tex_43..52.png` —
  the ground truth for what the body material binds.
- Game code path: Assembly-CSharp.dll → `ChaControl.ChangeSettingBodyDetail`
  (IL dumped with Mono.Cecil; keys `NormallMapDetail`/`LineMask`, category 420/400).
- Shader path: `ShaderForge_main_skin` forward pixel shader (disassembled in
  `out/shaders4/*.asm`): `_NormalMap`/`_NormalMapDetail` blended by
  `_DetailNormalMapScale`, `_LineMask` with `_linetexon`-style gating, ramp via `_RampG`,
  fresnel/rim via `_rimpower`/`_rimV`, outline separate.
- Body detail base rows: list tables (MsgPack) — category 420, ids 0–3; face
  detail category 400, ids 1–5. Base library includes all; mod rows resolve the
  same way.
- Card fields: body `detailId` (0 = なし); the composed create job runs
  `cf_m_body_create`; the bake target is the same texture (`scene.skinShade.texture`).
- `cf_body_00_base_n`/`cf_body_00_line`/`cf_body_00_md`/`cm_body_00_n` live in
  `chara/mm_base.unity3d` (decoded: BC5 normal 1024², BC1 lines 1024², BC7 mask 512²);
  face masks in `bo_head_00.unity3d`.

## 6. Explicitly out of scope (for now)

- Face `_DetailMask`/`_NormalMask` baking, eye AO
- Studio `_ambientshadowG` (global env, per-scene; not bakeable per character — would darken cloth too)
- The `_liquidmask` wet-skin specular
- `_AnotherRamp`/rim/specular re-bake of create materials (existing F9/ramp work covers toon shading)
- MMD-edge emulation of ink lines (the lines are already multiply-baked; no extra pass needed)

---

**Record:** user chose (2026-11-12): implement base-normal + default line mask bake
and card `detailId` detail masks, baked permanently into the exported PMX;
documentation first; `SkinShade` integration; proof in MMD by opening the PMX.
Amendment (same day): the new bake must *complement* `SkinShade` — it owns
nothing `SkinShade` owns (§4.4 division of labour), attenuates itself inside
`SkinShade`'s band, respects a 0.30 combined floor, keeps every `SkinShade`
knob and default untouched, and is verified by a three-way off / SkinShade-only /
combined comparison on the exported PMX PNGs (§4.6).

## 7. Implementation status (2026-11-12, landed)

Implemented and verified (build green, 5979 checks passing including the three new
AO unit tests; see §4 for the plan this executed):

- `src/export/SkinAoBake.h` / `.cpp` — `ApplySkinAoBake(scene, strength, rebuild)`:
  UV rasterization of per-texel tangent-space normals from the base body normal map
  (X in alpha, Y in green, Z reconstructed; the existing DXT5/BC5 decoder),
  consumed through a **local-curvature cavity factor** — how far the texel's
  normal curves away from its covered neighbours' average — never through any
  fixed view or light direction; plus the ink-line channel (G) of the card's
  `detailId` `LineMask` (`cf_body_00_line` at the default; the attenuation keeps
  them off the crease band). Alpha untouched, skin colour preserved.
- Two committed iterations shipped look wrong and were reverted before this one
  (see §4.4 "Why not a directional bake").
- `scene.aoBake` (`SceneSkinAoBake`) holds the decoded sources (`cf_body_00_base_n`,
  the `detailId` row's `NormallMapDetail`/`LineMask`, fallback `cf_body_00_line`)
  and the baked maps so the viewer can re-apply at a new strength without the
  bundles. The decode happens once, at assembly, after the clothing masks.
- Split reconstruction `tex = base x SkinShadeMul x cavity AO x ink lines` per
  call — strength drags never compound.
- CLI: `--skin-ao F` (0.85 default, 0 = off, byte-exact with pre-feature output);
  viewer: Look-tab row + Setup default row, settings `skinAo`, all wiring
  (`g.tw`, Look/Setup tabs, `ApplySkinLook`, `ReshapeNow`, assembler).
- `SkinShade` untouched: its strengths/controls/defaults are unchanged; its tests
  pass unchanged.
- The bake runs inside `Assemble` (GUI + CLI), after `ApplySkinShade`, into the
  same texture the PMX exporter references — MikuMikuDance and every other PMX
  viewer sees it with no CardBridge support.

Validation snapshot (`KK_138747` — the card the live-game dump was taken from —
`out/ao_bake_test_off`, `out/ao_bake_test_shade`, `out/ao_bake_test_full5`, exported with
`--skin-ao 0 --skin-shade 0`, `--skin-ao 0` (skinShade default 1.5/1.0/1.2/0.6) and
`--skin-ao 0.85`):
off is unchanged; the combined export's body skin PNG differs from the shade-only PNG
by mean |Δ| 0.81 (p99 24, max 158 on the ink lines), commits the clavicle stroke, the
navel dot and the toe definition; alpha byte-identical; corners untouched. The earlier
directional-light attempt (mean |Δ| 71, 47% pixels > 5, a sun-lit cast over the body) and
the low-threshold cavity attempt are both in git history as `ao_bake_test_full*` outputs
and were superseded by the shipped design.

The final check — opening `Matou_Sakura.pmx` in MikuMikuDance and judging the look — is
owned by the user; the offline comparison harness (this doc plus the three export
folders) is what proves no regression on the pipeline side.
