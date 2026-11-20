# CardBridge — Koikatsu Sunshine (KKS) Offline Format Feasibility

**Status:** research completed 2026-11-20; implementation complete and validated the same day. See [`KKS_IMPLEMENTATION_ROADMAP.md`](KKS_IMPLEMENTATION_ROADMAP.md) and [`KKS_TEST_MATRIX.md`](KKS_TEST_MATRIX.md) for results and limitations.
**Scope:** can the existing offline CardBridge conversion pipeline read a KKS card plus a KKS asset library and produce the same kind of PMX it produces for Koikatsu (KK)? The findings below are the evidence base for that implementation.

Evidence tags used throughout:

| Tag | Meaning |
| --- | --- |
| `[V]` | Directly verified in this study: decompiled code, parsed file bytes, or a scratch probe linked against `build/libcardbridge_core.a`. |
| `[I]` | Inferred from verified facts plus the current CardBridge code; highly likely but not end-to-end proven. |
| `[U]` | Unknown / not proven. Needs an implementation and a side-by-side check before relying on it. |

Scratch probes, dumps and decompilations live outside the repository in
`C:\Users\pepsi\AppData\Local\Temp\opencode\kks-research\`. They are not part of CardBridge.

---

## 1. Installs inspected

| Role | Path | Verified facts |
| --- | --- | --- |
| KKS (target) | `E:\Game\[ScrewThisNoise] Koikatsu Sunshine EX BetterRepack R13` | Unity **2019.4.9f1**; `abdata` ~17.46 GB / 1,449 files; `mods` 4,413 `.zipmod` in 10 folders; BepInEx 5.4.23.5; KKSAPI 1.48; ExtensibleSaveFormat 21.1.4. `[V]` |
| KK (baseline) | `E:\Game\[ScrewThisNoise] Koikatsu BetterRepack RX21` | Unity **5.6.2f1**; used only as comparison. `[V]` |

KKS bundles are UnityFS v6 / engine 2019.4.9f1 and their `SerializedFile` is **format 21**.
`src/unity/UnitySerialized.cpp:66-88` accepts versions 13–21 and already handles the v19 reference-type hash and v21 trailing data, so KKS bundles open and their type trees read correctly. `[V]`

Path gotcha: PowerShell needs `-LiteralPath` for these paths because of the `[...]` in the folder name.

---

## 2. Card PNG: container and sections

Card census over 2,326 PNGs in the KKS install: 2,105 `【KoiKatuChara】`, 182 `【KoiKatuCharaSun】`, 32 `【KoiKatuCharaSP】`; no Sunshine-flavoured clothes (`【KoiKatuClothes…】`) or EmotionCreators cards found. `[V]`

A KKS card container is byte-layout compatible with KK: `[V]`

```
KKEx(3), Custom(0.0.0), Coordinate(0.0.0), Parameter(0.0.6), Status(0.0.0), About(0.0.0)
```

- `src/card/CardFile.cpp:91` already recognises `【KoiKatuCharaSun】` as `CardFamily::KoikatsuSunshine`.
- `src/card/CardFile.cpp:667-672` then sets `supported = false` and returns the note *"recognised as koikatsu-sunshine, not converted yet"*. That is the only deliberate KKS gate in the card reader. `[V]`
- The gate happens **after** the block header is parsed, so the container is already decoded; the existing section readers can be used once the gate is lifted. `[I]`

Field comparison (400 KK cards vs 70 KKS cards, `card_diff.txt`): `[V]`

- **No key-level differences** in `Face`, `Body`, `Hair`, Clothes, Accessory, `Parameter`, `Status` or `About`.
- Differences are values/versions: `Face` `0.0.2`→`0.0.3`, `Parameter` `0.0.5`→`0.0.6`, `Coordinate/clothes` `0.0.1`→`0.0.2`, 7 outfits→**4 outfits**, and a KKS-only `About` block (also present on 8 KK cards).
- `Interest`, `Attribute`, `Denial` etc. are present in both families, so the KKS `Parameter` upgrade (`ilspycmd ChaFileParameter`: adds `0.0.6` interest init) does not introduce a new key for CardBridge to read.
- The KKS `ChaFileFace` adds `hlUpX`/`hlDownX` and version `0.0.3` (`ComplementWithVersion`); those keys are already present on the KK cards in this install. `[V]`

`Card::outfitName()` (`src/card/CardFile.cpp:46-64`) has seven hard-coded KK names and only uses them for the KK/KK Party families; a KKS card would fall back to `Outfit 1..4`, which is safe but not localised. `[V]`

**Conclusion — card layer:** low risk. The container, block names and section keys are shared. The work is the rejection gate, a 4-outfit-aware UI/label path, and a regression check that KK cards still take the 7-outfit path. `[V/I]`

---

## 3. Bundles, meshes and textures

### 3.1 Serialized files and prefabs `[V]`

`chara/oo_base.unity3d` (KKS): 11,674 objects, 5,544 transforms, format 21; existing `UnityFile` reads prefab roots, renderers, materials and textures. `p_cf_body_00`, `p_cf_head_00`, the hair/clothes prefabs and their `cf_j_root`/`cf_o_root` hierarchies are present with the expected non-`_low` root names.

- Skeleton: 368 unique `cf_j_/cf_s_/cf_d_/cf_dd_` bone names in both KK and KKS `oo_base`; all 366 shared rest-pose local positions are numerically identical.
- KKS adds low-LOD bodies (`p_cf_body_00_low`), hit meshes (`o_hit_*`), default underwear and more transforms; they are extra roots, not renames.
- Head: `p_cf_head_00/02/03`; face meshes keep names and vertex counts; 81 blend shapes on `cf_O_face`; 52 face / 44 body slider counts unchanged; blend-shape names are empty in both games (index-driven), which the existing reader already handles.
- Clothing prefab `p_o_top_tsyatu02` has the same root children and `ChaClothesComponent` on the prefab root.

### 3.2 Mesh skinning is the hard incompatibility `[V]`

KK mesh (8 channels) stores weights/bone indices in the serialised `m_Skin` array. KKS mesh (14 channels) stores them in vertex streams:

```
ch0  stream0 off0  fmt0 dim3   position
ch1  stream0 off12 fmt0 dim3   normal
ch2  stream0 off24 fmt0 dim4   tangent/colour
ch3  stream1 off0  fmt2 dim4   colour (often dim0 on clothes)
ch4..6 stream1 ... fmt0 dim2   UV0..UV2
ch12 stream2 off0  fmt0 dim4   blend weights (float4) on body/face, dim2 on many clothes
ch13 stream2 off16 fmt10 dim4  blend indices (UInt32; dim2 on many clothes)
m_Skin = empty, m_BindPose = present
```

Concrete consequences in `src/unity/UnityScene.cpp`:

- `channelFormatSize()` line 39 throws `unknown vertex channel format 10` as soon as a KKS mesh is read (`readMesh` line 294 computes stream strides over every channel). `[V]`
- Even with format 10 accepted, `readMesh` line 389 only fills `m.skin` from `m_Skin`, which is empty on KKS, so every vertex would be unweighted. `[V]`
- The number of influences is the **channel dimension**, not a fixed 4: body/face use dim 4, many KKS garments use dim 2. A fallback reader must pad to four influences and read `dim` entries. `[V]`
- KK and KKS body vertex counts differ slightly (KK `o_body_a`: 2,479 / 6,823 / 8,302; KKS: 2,501 / 6,823 / 8,387), so vertex-count equality is not a usable KKS acceptance test. `[V]`

This is a required engine change, not a data-mapping change. Without it no KKS mesh can be exported with weights.

### 3.3 Textures `[V]`

KKS texture formats decoded successfully by the existing readers: 3 (RGB24), 4 (RGBA32), 10 (DXT1), 12 (DXT5), 25 (BC7). The body map names CardBridge depends on exist in KKS:

| Texture | KKS | KK |
| --- | --- | --- |
| `cf_body_00_t` | format 4, 2048² | same name |
| `cf_body_00_mc` | format 3, 1024² | same name |
| `cf_body_00_base_n` | **BC7 (25)**, 1024² | DXT5 (12) |
| `cf_body_00_line` | DXT1 (10), 1024² | same |

`SkinAoBake`/`cf_body_00_base_n` should still work because BC7 decoding is verified. `[V/I]`

### 3.4 Shape/proportion data `[V]`

| Asset | Result |
| --- | --- |
| `list/customshape.unity3d` → `cf_customhead`, `cf_custombody` | Byte-identical between KK and KKS (FNV `3dea508b…`, `f8c67171…`). Slider→bone semantics unchanged. |
| `chara/oo_base.unity3d` → `cf_anmShapeBody` | Byte-identical (121,513 B, FNV `ed71a489…`). The generated formulas in `src/shape/Shape.cpp` remain applicable. |
| `chara/bo_head_00.unity3d` → `cf_anmShapeHead_00` | Byte-identical (90,698 B). KKS adds `_02` (95,771 B) and `_03` (96,790 B); all three parse with CardBridge's `AnimKeys::Parse` (25 keys each). Head list rows name their own `ShapeAnime`, which `src/assemble/Assembler.cpp:323-327` already reads by name. |
| `list/shapecorrect/shapecorrect.unity3d` | Same size (1,156 B) and 32 rows, but one row differs: row 2 scale KK `(0.25, 0.15, 0.22)` vs KKS `(0.15, 0.15, 0.03)`. Only relevant when `typeBone != 0` (`Shape.h:76-78`, `Assembler.cpp:362-370`). |
| `ShapeBodyInfoFemale` (decompiled) | KKS adds `deformedHeight`, `deformedHeightAid`, `deformedHeadSize`, `deformedNeckSize`, `deformedArmLength`, `deformedLegLength`, `deformedFootSize`, `deformedStandPos`. The four used multipliers default to `1`, and no assignment to them exists in KKS `Assembly-CSharp.dll`, so they are behaviourally neutral for a stock card. A plugin could set them, which is `[U]`. |

### 3.5 List tables `[V]`

`abdata/list/characustom/*.unity3d` uses the same MessagePack `categoryNo`/`lstKey`/`dictList` schema. `00.unity3d` holds 65 TextAssets in KKS vs 64 in KK; the category numbers CardBridge hard-codes (401, 406, 407/408, 410, 411, 412, 414, 430, etc.) are unchanged, so `rowTexture`/`listTex` category lookups remain valid.

Changes to handle: `[V]`

- KKS adds categories 416 (`mt_pupil_00`) and 417 (`mt_overhl_00`); KK has 130 (`ao_kokan_00`), KKS does not.
- Row counts grow (e.g. `co_top_00` 44→112, `co_bot_00` 32→72, `ao_head_00` 32→111, `bo_hair_f_00` 38→78).
- New row keys: `co_top_00` `MainTex03*`, `co_bot_00` `HideShorts`/`MainTex03*`, `co_bra_00` `MabUV`. CardBridge currently reads only `MainTex`/`MainTex02`, `ColorMask`/`ColorMask02`, `OverBodyMask`, `OverBraMask`, `NotBra` (`Assembler.cpp:3079-3108, 3460-3461, 3914`), so a third texture slot or `HideShorts` behaviour would be ignored. `[V]`
- `MainManifest`/`MainTexAB`/`ColorMaskAB`/`OverBodyMaskAB`/`OverBraMaskAB`/`ThumbAB`/`ThumbTex` already exist in KK-era tables — these are **not** KKS additions.

`src/resolve/ListTable.cpp:35-64` parses every TextAsset in every list bundle as MessagePack inside one try/catch; on KKS the list bundles parse, but an unrelated non-MessagePack TextAsset would silently drop that whole bundle. Not currently triggered, but worth a targeted issue message. `[V/I]`

---

## 4. Materials and shaders — second hard incompatibility

### 4.1 Shader families `[V]`

KK uses `Shader Forge/*`; KKS character assets mostly use `Koikano/*`:

| KK | KKS |
| --- | --- |
| `Shader Forge/main_opaque` | `Koikano/main_clothes_opaque`, `Koikano/low_clothes_opaque` |
| `Shader Forge/main_alpha` | `Koikano/main_clothes_alpha`, `Koikano/low_clothes_alpha` |
| `Shader Forge/main_item` | `Koikano/main_clothes_item`, `Koikano/main_clothes_item_glasses` |
| `toon_eye_lod0` / `toon_eyew_lod0` | `Koikano/main_eye` / `Koikano/main_eyew` / `Koikano/main_eyeb` |
| `Shader Forge/main_skin` | `Koikano/main_skin` |
| `Shader Forge/main_hair[_front]` | `Koikano/hair_main_sun_front`, `Koikano/hair_main_low_sun` |

KKS base bundles still contain legacy shaders: `oo_base` has `Shader Forge/shadowcast`, `Shader Forge/mnpb`, `Standard`, and some `Shader Forge/*`; `mm_base` has a legacy `cf_m_eye_create 1` with `Shader Forge/create_eye` and `cf_m_hair_create` with `Shader Forge/create_hair`. KKS `cf_m_eye_create` container entry resolves to the **Koikano** `create_eye` material, because `UnityFile::findAsset` returns the first matching container key (`src/unity/UnityScene.cpp:211-227`). `[V]`

Name-based logic that will misbehave on KKS: `[V]`

- `Assembler.cpp:976` tints hair only for exact `Shader Forge/main_hair`/`main_hair_front`. KKS hair shaders contain `hair` but not those strings, so the card's hair colour would not be applied.
- `Assembler.cpp:1114-1115` falls back to `Shader Forge/main_alpha`/`main_item` for missing MaterialEditor shaders; those names do not exist in a stock KKS library.
- `Assembler.cpp:711-718` `twoSided()` uses `"alpha"`/`"hair"` substrings plus shader render state; the KKS names mostly satisfy this, and `readShaderRenderState()` works on KKS `main_skin` (`cull=1`, `zWrite=1`, `queue=2450`). `[V]`
- `Assembler.cpp:1730` recognises `shadowcast` by substring and `Shader Forge/mnpb` exactly; both still work on the KKS bundles inspected. `[V]`
- KKS create materials exist under the expected names (`cf_m_body_create`, `cf_m_face_create`, `cf_m_clothesN_create`, `cf_m_eye_create`, `cf_m_eyewhite_create`) but use `Koikano/create_*` shaders with extra properties (`_AnimationMask`, `_texcoord*`, `_Dither`, `_Surface*`, `_UPanner`, etc.). The properties CardBridge writes for clothing (`_Color`, `_Color1_2`, `_Color2_2`, `_Color3_2`, `_PatternMask1..3`, `_PatternScale*`) are present in both games' `create_topN`, so the *inputs* are compatible. `[V]`

### 4.2 Shader program blobs cannot currently be read `[V]`

The KKS `Shader` object still has `platforms`, `offsets`, `compressedLengths`, `decompressedLengths`, `compressedBlob`, but the three descriptor arrays are **nested per subshader** (one inner array per platform), unlike the flat KK arrays. `UnityFile::readShaderBlob()` (`src/unity/UnityScene.cpp:507-527`) indexes them flat, gets zero lengths and returns an empty blob. Verified for all five KKS create materials: `blob bytes=0`, `ParseUnityShaderBlob` → `not a Unity shader program blob`.

A scratch probe that manually decompressed the nested arrays recovered valid bytes (e.g. `Koikano/create_eye`: 1,461→3,320 bytes, two programs) and exposed further differences:

- Each program has a ~42-byte Unity 2019 header before the DXBC container; CardBridge's `ParseHeader()` assumes the KK-era 24-byte header plus keyword list.
- The DXBC chunks have no `RDEF`, so `ParseDxbc` cannot derive pixel vs vertex type and reports both programs as vertex; `PickForwardPass()` fails. The DXBC shader code chunk itself starts with SM4 version tokens (`0x00010040` VS, `0x00000040` PS), so a reader that derives type from the code token may work.
- The binding tail is a new table containing `UnityPerDraw`, `UnityPerFrame`, `$Globals`, `_MainTex`, `_Color`, … rather than KK's cbuffer/texture record stream; `ParseTail()` will not map `_MainTex`/`_Color` correctly.

**Consequence:** the software shader runner behind `RunCreateJobs()` (`src/export/Preview.cpp:1750-1863`) cannot run KKS create materials. Body, face, eye and clothing textures are mostly built by those jobs, so without a new shader-blob reader a KKS export would keep flat input textures and lose the card's baked colours/overlays. This is a required fidelity change, independent of the mesh fix. `[V]`

`ShaderReadsColor()` (`src/material/MaterialWorkbench.cpp:80-97`) returns `-1` (unknown) for KKS shaders, which is handled conservatively by the accessory-tint path but means the interactive material workbench cannot prove KKS shaders ignore `_Color`. `[V]`

---

## 5. Mods and resolver

- KKS Sideloader `manifest.xml` uses `<game>` tags: KKS-exclusive mods carry `<game>Koikatsu Sunshine</game>` (e.g. `[Creamstar] Mordhau Outfits v6.1`), shared mods carry both `<game>Koikatsu</game>` and `<game>Koikatsu Sunshine</game>` (e.g. `[5896]Hu Tao v1.4`), and older packs carry no `<game>` tag (e.g. `[Snooley]Coiffure Pack`). `[V]`
- `src/resolve/ModIndex.cpp:32-88` reads guid/name/version/author, `<MaterialEditor>` shaders and list CSV ids; it does **not** read `<game>`. If a single mods folder mixes KK-only and KKS mods, compatibility filtering must be added or wrong assets may win. `[V]`
- Engine mixing is real: `[Snooley]Coiffure Pack` bundles and the KKS-exclusive `[Creamstar] Mordhau Outfits` bundles both read as Unity **5.6.2f1 / serialized 17**, while KKS base bundles are 2019.4.9f1 / 21. The resolver and readers must continue to accept both — which they already do for KK. `[V]`
- Zipmod content layout matches KK: `abdata/chara/*.unity3d` plus `abdata/list/characustom/<author>/*.csv` with three header lines followed by CSV rows. `[V]`
- `abdata/addXX`, `abdata/abdata`, `studioXX` are tiny UnityFS manifest bundles (classes 142/290), not content; they should stay out of asset matching. `[V]`

---

## 6. Verdict

**Feasible, but this is not a data-only enablement.** KKS is close enough to KK that the card reader, list tables, skeleton, shape tables, textures and prefab structure can be reused. Two engine-level gaps must be closed first:

1. **Mesh skinning** — KKS puts blend weights/indices in channels 12/13 and leaves `m_Skin` empty; format 10 must be accepted and a channel-based skin fallback added (§3.2).
2. **Shader program blobs** — KKS stores them in a newer container and the create-material shaders are `Koikano/*`, so `readShaderBlob`/`ParseUnityShaderBlob`/`PickForwardPass` and the create-job texture baking do not work today (§4.2).

Then a small set of mapping fixes: hair shader identity, MaterialEditor fallback names, third clothing texture / `HideShorts`, `<game>` mod filtering, and 4-outfit presentation. None of these require changing KK behaviour if they are gated on card family or on detected bundle/engine version. `[V/I]`

The standalone property is preserved: everything above is file-format work against a copied library plus `.zipmod` files. No BepInEx plugin, game process or runtime API is needed at conversion time. `[V]`

---

## 7. Open questions / unknowns

| # | Question | Why it matters |
| --- | --- | --- |
| U1 | Can KKS DXBC programs actually execute on CardBridge's SM4 VM after the blob reader is adapted? | Program type and binding tables are now readable in principle, but no end-to-end create-job run has been attempted. |
| U2 | Are the KKS `_02`/`_03` head `ShapeAnime` variants behaviourally identical to `_00` for the sliders present on KK-era cards? | They parse, but the generated head formulas may need a KKS variant. |
| U3 | How do `MainTex03`, `HideShorts` and `MabUV` change the authored look? | Cards using those rows may export with missing layers or wrong coverage. |
| U4 | Can `deformed*` body-shape fields be set by a plugin (ABMX-like)? | No assignment exists in `Assembly-CSharp.dll`; if a plugin sets them, KK-generated shape formulas would diverge. |
| U5 | What are the exact KKS outfit names? | Generic `Outfit 1..4` is safe but not faithful. |
| U6 | Library import size/time for 17.46 GB `abdata` + 4,413 zipmods; how much is actually copied? | Needs a measurement before promising an import experience. |
| U7 | KKS-exclusive accessories/hair with new component fields (`rendNormal03`, `objSleeves*`, `exRendEmblem*`) — how often are they used? | CardBridge only reads `rendNormal01/02` and `rendAlpha01/02` (`Assembler.cpp:3161-3176`). |

---

## 7b. What implementation changed in the findings above

Corrections and confirmations from actually running the code (2026-11-20):

- **§4.2 descriptor table:** the "both programs are vertex" probe result was itself caused by reading the 3-word KKS descriptors as 2-word KK pairs. With correct descriptor normalisation, the two create-shader programs parse as VS + PS; `Dxbc.cpp` already derives the stage from the SM4 version token, so no RDEF fallback was needed. The real remaining work was the nested arrays, the 42-byte program preamble and a new binding-table parser.
- **KK regression found and fixed:** KK program preambles place DXBC at offset 54, so an early `at >= 32` discriminator sent KK shaders through the KKS tail parser and broke 41 KK material bakes (white hair / dark skin / wrong textures on `KK_236802`). The discriminator now checks the KKS preamble magic `0x0C07513C`; A/B against the pre-KKS build shows byte-identical-looking previews again.
- **U1 (resolved):** create-job shaders DO run on CardBridge's SM4 VM. All five KKS create materials parse, select a forward pass, and produce correct coloured textures (verified visually: iris, clothes, face, body). The first black outputs were a cbuffer-slot mistake in the new tail parser (KKS uses cb0; the trailing word is not a register slot).
- **U3 (partial):** third clothing texture and `HideShorts` are implemented; `MabUV` is still unmodelled (no known visual effect in the tested cards).
- **U6 (resolved):** KKS minimal library = 26 files / 6.6 MB / 0.82 s; full = 269 files / 1,375 MB / 33.1 s.
- **U7 (partial):** `rendNormal03` is now collected; `objSleeves*` / `exRendEmblem*` are still ignored but diagnosed when present is not implemented.
- **Known limitation:** KKS `Koikano/*` materials are NOT re-baked through the UV-flat material bake (`BakeShaderTextures`) because that pass produced transparent sheets and hid all parts. Their verified create-job textures are kept and the skip is recorded in diagnostics. KK `Shader Forge/*` material baking is unchanged.

## 8. Reproduction notes

All `[V]` findings above came from read-only probes in the scratch directory, linked against the existing `build/libcardbridge_core.a` (no repository file edited):

```
D:\Project K\tools\w64devkit\bin\c++.exe -std=c++20 -O2 \
  -I"D:\Project K\cardbridge\src" \
  -I"D:\Project K\cardbridge\third_party\bc7decomp" \
  -I"D:\Project K\cardbridge\build\_deps\json-src\include" \
  -I"D:\Project K\cardbridge\build\_deps\stb_image-src" \
  -I"D:\Project K\cardbridge\build\_deps\stb_image_write-src" \
  <probe>.cpp "D:\Project K\cardbridge\build\libcardbridge_core.a" \
  -o <probe>.exe -ld3d11 -ldxgi -lole32 -luuid -luser32 -lgdi32 -lshell32
```

Probes used: `kks_probe` (card sections), `mesh_probe`, `prefab_probe`, `chara_probe`, `list_probe`, `textasset_probe`, `mat_probe`, `state_probe`, `shader_probe`, `shader_fields_probe`, `kks_shader_try`, `anm_probe`, `correct_probe`, `tex_probe`; Python `card_diff.py`, `list_diff.py`, `bone_diff.py`, `zipmod_survey.py`, `cat_diff.py`. `ilspycmd 8.2.0.7535` was used on both games' `Assembly-CSharp.dll`.

Related documents: [`OFFLINE_FORMAT_FEASIBILITY.md`](OFFLINE_FORMAT_FEASIBILITY.md) (KK baseline and acceptance tests A1–A10), [`KKS_IMPLEMENTATION_ROADMAP.md`](KKS_IMPLEMENTATION_ROADMAP.md), [`KKS_TEST_MATRIX.md`](KKS_TEST_MATRIX.md).
