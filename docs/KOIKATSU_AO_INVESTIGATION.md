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

1. **Bake base-normal skin lighting** — sample `cf_body_00_base_n` in UV space with
   the mesh's tangents (CardBridge computes these), shade with a fixed light
   direction, multiply the result into the composed skin texture.
2. **Bake the game's default line mask** — `cf_body_00_line` (always on in game),
   multiplied in with a strength knob.
3. **Bake the card's detailId masks** — resolve the body `detailId` via the list
   tables (category 420, `NormallMapDetail` / `LineMask` keys), exactly as the game's
   `ChangeSettingBodyDetail` does, and bake both into the skin texture.

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
and the "off" path stays bit-exact with today's output.

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

### 4.4 Baking model

Per covered body texel (UV space — same rasterization order and padding behaviour
as `ApplySkinShade`):

1. Sample `cf_body_00_base_n` at UV (x,y like the other bakes: row 0 = v=1).
2. Convert tangent-space normal via the mesh tangent frame at the nearest vertex
   or per texel's interpolated tangent — CardBridge rasterizes per triangle, so
   interpolate `T`, `B`, `N` from vertex attributes (they are already needed by the
   GPU viewer; the CPU path computes `ComputeMeshTangents` when absent).
3. Light: one fixed key light from the front, slightly above, *soft* wrap lighting
   (like the game's ramp, not a hard cosine): `d = saturate(dot(N, L)*0.5+0.5)`,
   then `shade = lerp(1.0, pow(d, rampContrast), baseNormalStrength * occlusion)`.
   No need to reproduce the exact Shader Forge curve — this is a static bake and
   MMD/Project K light the model themselves; the aim is "under-bust valley and
   ribs read as form", which a wrapped diffuse does.
4. Line mask: `line = cf_body_00_line.G` (+ the detail row's `LineMask.G` /
   `LineMask.R` with separate weights). Multiply `rgb *= (1 - line * lineMaskStrength)`
   — the ink lines the game draws with `_linewidth`. Keep a floor of e.g.
   `max(result, 0.35)` so thin lines don't collapse to black on dark skin tones.
5. Detail `_NormalMapDetail` (if present and not the 8×8 blank): blend its normal
   toward the base normal by `_DetailNormalMapScale` (default 1.0) before step 3,
   exactly like the shader does — cheap and reuses step 3.

Order: AO/normal bake → line multiply. Both write into the same texture the PMX
exporter references; recompute `pixelHash` afterwards (like `ApplySkinShade`).

### 4.5 Configuration

Consolidate under the existing `--skin-shade` family rather than adding a new
surface:

- no new mandatory switch; master switch `--skin-shade 0` keeps everything off
  (the bake runs only when `opt.skinShade > 0 || opt.skinShadeUnder > 0 || skinAoBake > 0`)
- proposed: `--skin-ao F` (base-normal + default line mask strength, default 1; 0 = off)
  and fold the detailId bake into it (off only when the whole `--skin-ao` is 0)
- viewer Look tab: one new row "Game AO bake (base normal + lines)" wired to the
  same value; Setup persists it in settings like `skinShade*`
- if the user prefers separate knobs, split `--skin-ao-normal` and `--skin-ao-line`;
  decision deferred until the PR

### 4.6 Validation

Before/after PNGs on the same cards (Sakura + Rin + the modded-detail card):

- inspect the *exported PMX's referenced* body PNG directly (not the viewer) —
  the fold under each breast darkens, sternum valley deepens, collarbone/knee
  lines appear, no dark patches elsewhere, skin colour customization intact
- alpha of the body texture unchanged (body skin is opaque; verify by hashing the
  alpha channel before/after across covered texels)
- covered-only modification (rasterized coverage) — untouched texels bit-identical
- seams: `FeatherMap`/edge handling identical to the existing bake; check the UV
  island borders in the PNG
- disable flag → output bit-identical to current export (test in the suite)
- male body (`cm_body_00_n`) path on a male card
- regression tests: `check` alpha preservation, additive-multiply math, coverage
  of the multiply against a synthetic flat normal map, detail-row resolution from
  a fake list table entry, and the "off = byte-identical" property

### 4.7 Definition of Done (revised)

A character exported through CardBridge produces a PMX whose **referenced body
skin PNG** shows the base body normal-map shading, the game's default (and, when
the card has one, its `detailId`) line mask baked in — visible in standalone MMD
with no CardBridge support. The spec's literal "extract the AO textures" step is
struck: they do not exist; the above is the functional equivalent, implemented
from the game's real data path.

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
