# CardBridge — Offline Format Feasibility (Milestone 0)

Date: 2026-10-28. Scope: research only, no app code. Plan: `design/Archive/CardBridge_Standalone_Koikatsu_to_PMX_Plan.md` (sections 3, 11, 14–17).

**Verdict: GO for Milestone 1** (one card → PMX, no game process), with the conditions in section 9. **Update 2026-10-28: Milestone 1 is done and passes; results in section 12.**

Evidence tags used throughout:

- **[V]** verified in this study against a real card or file, or against the decompiled game assembly.
- **[S]** taken from a cited source, not re-verified here.
- **[U]** unverified or open. Treat as a risk, not a fact.

## 0. Decisions from the user that shape this study

- The finished product is a **C++ standalone exe, built like the Map Maker / engine**, in its **own dedicated folder** (`cardbridge/`), with scripts in `scripts/` (`cardbridge.ps1`, `cardbridge.bat`).
- **Windows only** for the first release.
- **Fully separate from the game at run time**: no game install is needed to run it. Base-game data it needs is copied once into a CardBridge **asset library** (section 12.3); nothing reads the game after that.
- The Python / UnityPy work below is a **research and reference tool only**. It never ships.

The repo's CMake already fetches glfw, glm, volk, Vulkan-Headers, imgui, nlohmann json, stb_image, miniaudio and stb_vorbis (`CMakeLists.txt`, `FetchContent_Declare` blocks) **[V]**. **Correction (Milestone 1):** the engine does have a PMX reader, `engine/src/assets/PmxFile.{h,cpp}` (the CMake comment on stb_image mentions PMX materials). An earlier version of this line said otherwise; that check was wrong. The engine has a PMX *reader* but no writer, so CardBridge writes its own, and uses the engine's reader as the independent validator (section 12).

## 1. What was inspected

| Item | Fact | Tag |
|---|---|---|
| Install | `E:\Game\[ScrewThisNoise] Koikatsu BetterRepack RX21`, `Koikatu.exe` + `Koikatu_Data`, BetterRepack RX21 (30.05.2024), `KK-HF_Patch-3.36.zip` in the root | [V] |
| Engine | Unity **5.6.2f1**, Mono (`Koikatu_Data\Managed\Assembly-CSharp.dll`, `Mono\`). Version read from `globalgamemanagers` and from the bundle headers | [V] |
| Asset bundles | `abdata\` = 2100 files; `abdata\chara\` = 299 files | [V] |
| Mods | `mods\` = 4224 `.zipmod` + 1 `.zip`, 4215 distinct GUIDs. Also 12 `.rar` + 1 `.7z` archives (not opened) | [V] |
| Sample card A | `UserData\chara\female\KK_138747.png` (character "Matou Sakura"), 509 KB, 125 mod-resolve rows, 24 distinct mods | [V] |
| Sample card B | `UserData\chara\female\Koikatu_F_20260718155357521_Rin.png`, 19 MB, 24 resolve rows, 10 distinct mods | [V] |
| Card census | 2071 `【KoiKatuChara】`, 110 `【KoiKatuCharaSun】`, 32 `【KoiKatuCharaSP】`, 8 `【KoiKatuClothes】`, 5 `【EroMakeChara】` (PNG files under `UserData\chara` and `mods`; 36 are plain PNGs) | [V] |
| Game launched? | **No.** Everything below came from files, the decompiled assembly and public sources | [V] |

## 2. Supported-format matrix

| | Koikatsu (JP / base) | Koikatsu Party | Koikatsu Sunshine |
|---|---|---|---|
| Card marker string | `【KoiKatuChara】` | `【KoiKatuCharaS】` and `【KoiKatuCharaSP】` (patched Party) | `【KoiKatuCharaSun】` |
| Marker source | Decompiled `ChaFile.cs` line 190 in this install **[V]**; KKManager `CardLoader.GetGameType` **[V]** | KKManager `CardLoader.GetGameType` **[V]** | KKManager `CardLoader.GetGameType` **[V]** |
| Local sample cards | 2071 **[V]** | 32 `SP` cards **[V]**; `S` (no P): 0 found | 110 **[V]** |
| Card block list (real card) | `KKEx, Custom, Coordinate, Parameter(0.0.5), Status` | `SP` card: same five blocks **[V]** | `KKEx, Custom, Coordinate, Parameter(0.0.6), Status, About` — one extra `About` block **[V]** |
| Outfits per card | 7 (array of 7 coordinate blobs) **[V]** | 7 **[V]** | **4** (array header `0x94`) **[V]** |
| Item resolve data | Sideloader `KKEx` block present **[V]** | present **[V]** | present, 67 rows in the sample **[V]** |
| Asset engine | Unity 5.6.2f1, UnityFS v6 **[V]** | same family, no Party install here **[U]** | may differ; **no Sunshine install here, so bundles are unverified** **[U]** |
| Offline status | **Proven end to end for card + mod + base assets** (sections 3–5) | Card layer proven; assets assumed same as Koikatsu **[U]** | Card layer proven; asset layer **not** tested **[U]** |

Reading of the table: the shared 5-block container is identical across all three, so **one container parser fits all**. The differences are small and visible (marker, block set, outfit count, parameter version). Asset readers differ only if the Unity version differs.

**Recommendation:** first release supports **Koikatsu (`【KoiKatuChara】`) only**, with Party `SP` accepted if its assets prove identical. Sunshine is detected and reported as "unsupported yet" until a Sunshine install is available. This follows plan section 2.3 (report unknown variants, never misparse).

## 3. What is in the card PNG and what is external

### 3.1 Container layout (all offsets verified on the sample cards)

Source of truth: decompiled `ChaFile.SaveFile / LoadFile` **[V]**.

```
PNG bytes (the card picture), ends at IEND
int32  loadProductNo        (100)
string marker               (.NET BinaryWriter string: 7-bit length prefix + UTF-8)
string version              ("0.0.0")
int32  faceLen ; byte[faceLen]   face thumbnail PNG
int32  headerLen ; byte[headerLen]   MessagePack BlockHeader { lstInfo: [ {name, version, pos, size} ] }
int64  dataSize
byte[dataSize]              blocks, each at data0 + pos, length size
```

Block names seen: `Custom`, `Coordinate`, `Parameter`, `Status`, `KKEx` (written by the ExtendedSave plugin, not by the game) and, on Sunshine, `About` **[V]**.

Payload encodings **[V]**:

- `Custom`: three int32-length-prefixed MessagePack maps, in order **face, body, hair**.
- `Coordinate`: one MessagePack array of N byte arrays (N = 7 on Koikatsu, 4 on Sunshine). Each byte array is `int32 len + clothes(msgpack)`, `int32 len + accessory(msgpack)`, `bool enableMakeup`, `int32 len + makeup(msgpack)` (decompiled `ChaFileCoordinate.SaveBytes`).
- `Parameter`, `Status`: single MessagePack maps (names, personality; clothes/accessory state, eye/mouth state).
- `KKEx`: one MessagePack map `plugin GUID → [version, payload]`; a value can be `nil`.
- MessagePack here uses **string keys** (field names), so decoding needs no schema.

### 3.2 Fields seen in the Custom and Coordinate maps (Rin card, exact key names)

- `face` (39 keys): `shapeValueFace` (52 floats), `headId`, `skinId`, `detailId`, `eyebrowId`, `noseId`, `pupil` (2), `hlUpId`, `hlDownId`, `whiteId`, `eyelineUpId`, `eyelineDownId`, `moleId`, `lipLineId`, plus colours and sliders.
- `body` (27 keys): `shapeValueBody` (44 floats), `bustSoftness`, `bustWeight`, `skinId`, `detailId`, `paintId`, `sunburnId`, `nipId`, `underhairId`, colours, `typeBone`.
- `hair`: `parts` (4: back, front, side, extension), each `id`, colours, `length`, `pos/rot/scl`.
- clothes: `parts` (9 slots) each `id`, `colorInfo`, `emblemeId`, `emblemeId2`, `hideOpt`, `sleevesType`; plus `subPartsId`, `hideBraOpt`, `hideShortsOpt`.
- accessory: `parts` (20 slots) each `type`, `id`, `parentKey` (attach bone, e.g. `a_n_headside`), `addMove`, `color`, `hideCategory`, `noShake`.
- `Status`: `clothesState` (bytes), `showAccessory` (20 flags), `shoesType`, body/face visibility flags. These are the **card's default toggle state** and map directly to the outfit tree defaults.

### 3.3 Data classification

| Data | In the card? | Where the real data lives |
|---|---|---|
| Name, personality, pose/state flags | Yes (`Parameter`, `Status`) | — |
| Face / body proportion sliders | Yes: 52 + 44 floats | Applied at runtime by game code (`ShapeHeadInfoFemale`, `ShapeBodyInfoFemale`) onto the skeleton **[V: classes exist in the assembly; behaviour not yet reimplemented]** |
| Colours, glossiness, slider values for every part | Yes | — |
| **Item IDs** (head, hair, each clothing slot, each accessory, eyes, makeup, skin detail…) | Yes, as integers | **No geometry.** IDs are keys into list tables |
| Mesh, skeleton, textures of an item | **No** | Unity AssetBundles in `abdata\chara\*.unity3d` (base game) or inside `.zipmod` files under `mods\` |
| Which mod an ID belongs to | Yes, in `KKEx` → `com.bepis.sideloader.universalautoresolver` | Rows `{ModID(GUID), Slot, LocalSlot, Property, CategoryNo}` **[V]** |
| Extra accessories beyond 20 | Yes, in `KKEx` → `moreAccessories` (XML text) | Items still external |
| Material overrides and **embedded textures** | **Yes, partly.** The MaterialEditor plugin stores a 16.0 MB `TextureDictionary` in the Rin card (`KKEx` total 18.8 MB) | Textures are inside the card PNG, not the game |
| Overlay textures (body/face overlays) | Yes: `KCOX` 26 KB, `KSOX` 2.7 MB in the Rin card | Inside the card |
| Bone-scale tweaks (ABMX), dynamic-bone edits, push-up data | Yes, as plugin payloads | Need plugin-specific decoding **[U]** |

So the card is **metadata plus references plus some embedded textures**. It is not a model. Plan section 2.2 is confirmed.

### 3.4 How an item ID turns into an asset (proven on the Rin card)

1. The card stores the mod author's **`Slot`** value, not the session-local ID. Proof: the card's `ClothesTop` id is `64428` and the resolve row for `outfit0.ChaFileClothes.ClothesTop` has `Slot: 64428, LocalSlot: 100003112` **[V]**. Same for `hlUpId 1919`, `underhairId 9001`, accessory ids `520358–520360`.
2. `ModID` is the mod's GUID. Find the `.zipmod` whose `manifest.xml` has that `<guid>`. All 10 GUIDs of the Rin card and all 24 of KK_138747 were found in `mods\`: **0 missing** **[V]**. Example: `[OoOoO]srTohsakaRin` → `mods\2026.06Rin\mods\srRin.zipmod`.
3. Inside the zipmod, `abdata\list\characustom\*.csv` map `Slot` to a bundle and an asset name. Example from `[OoOoO]srTohsakaRin_top.csv`: `64428 → MainAB chara/[OoOoO]srTohsakaRin_top.unity3d, MainData [OoOoO]srTohsakaRin_top` **[V]**. CSV format documented in the Sideloader zipmod wiki **[S]**: first four lines are category, distribution number, source path, header.
4. For IDs **without** a resolve row, the item is base game. The game loads `abdata\list\characustom\*.unity3d` (TextAssets, MessagePack `ChaListData`) into a `CategoryNo → id → ListInfoBase` dictionary (decompiled `ChaListControl.LoadListInfoAll`) **[V]**. Check: `bo_hair_f_00.unity3d` holds `id 1 → chara/bo_hair_f_00.unity3d, prefab p_cf_hair_f_01`; `id 0 → p_dummy` (no item) **[V]**.
5. The `CategoryNo` enum (decompiled `ChaListDefine`) names every slot: `bo_head 100`, `bo_hair_b/f/s/o 101–104`, `co_top 105` … `co_shoes 112`, `ao_* 120–130`, `cpo_* 200–212`, `mt_* 400–433`, `ex_* 500–504` **[V]**.

Rin card status: top, bottom, bra, gloves, pants, socks, shoes and three accessories all resolve to one mod (`srRin.zipmod`); head, eyes, skin detail, nipples, underhair, lips, back hair resolve to nine other mods, all present **[V]**.

## 4. Existing parsers and readers that work without the game

| Tool | Use for CardBridge | Runs without the game? | License | Tag |
|---|---|---|---|---|
| **Python `msgpack` + a 60-line script** (written here) | Full decode of container, Custom, Coordinate, Parameter, Status, KKEx. This study used it | Yes | msgpack: Apache-2.0 **[U]** | [V] |
| **UnityPy 1.23.0** | Reads UnityFS bundles: meshes, skin weights, bind poses, textures, materials, GameObject hierarchy. Proven on a mod bundle and on `oo_base.unity3d` | Yes | MIT **[S]** (LICENSE fetched) | [V] |
| **ILSpy / `ilspycmd` 8.2.0.7535** | Decompiled `Assembly-CSharp.dll` (1855 `.cs` files) and `KKManager.Core.dll`. **I installed `ilspycmd` as a global dotnet tool**; ILSpy GUI had no CLI | Yes | MIT **[U]** | [V] |
| **KKManager** (in the install) | Reference for card detection: marker → game table, card types. Mod/zipmod browsing | Yes (it is a separate exe) | **GPL-3.0** (`LICENSE` in install) | [V] |
| **KoiCatalog** (in the install) | Card browser; uses MessagePack for C# | Yes | MIT (`License.txt` in install) | [V] |
| **KoikatuCharaReader** (in the install) | Card reader exe | Yes | licence folder exists, not read **[U]** | [U] |
| **AssetRipper** | Whole-bundle export to Unity project; heavy, GPL | Yes | **GPL-3.0** **[S]** | [S] |
| **AssetStudio** (`D:\Project K\AssetStudio.net6.v0.16.47`) | GUI bundle browser, useful for eyeballing | Yes | not checked **[U]** | [U] |
| **SB3Utility** (in `KKManager\SB3UGS` and `[MODDING] Tools`) | The Sideloader wiki points authors to it for category numbers; reads bundles | Yes | not checked **[U]** | [S] |
| **KKBP Exporter** (`KKBP_Exporter_4.36.zip`: two DLLs, one for KK/Party, one for Sunshine) | Reference for what a full export contains (hidden meshes, outfits, physics). **It is a BepInEx plugin that runs inside the game**, so it cannot be a runtime dependency | **No** | not verified **[U]** | [V]/[S] |
| **KKBP wiki / importer** (`kkbpwiki.github.io`) | Documents the exporter options and the Blender side. States the exporter needs a BepInEx install and the HF Patch | Exporter: no | — | [S] |
| **KKPMX** (`github.com/CazzoPMX/KKPMX`) | Described by its README as a mod/plugin for KK/KKS to export characters to PMX plus a clean-up utility for MMD. Useful for known PMX clean-up rules (delete `Bonelyfans`, `c_m_shadowcast`, `Standard` materials; hide materials that were hidden in-game; remove `-1` display-frame entries) | Plugin part: no | **MIT** (LICENSE fetched) | [S] |
| **`PmxExport.dll`** (`Export_PMX\PmxExport.zip`) | Another in-game PMX exporter | No | unknown | [V] |

Takeaway: **no existing tool gives card → PMX without the game.** Every PMX exporter found runs inside the running game. The pieces that do run offline (card decode, list lookup, UnityFS read) are enough to build the pipeline ourselves, as section 5 shows.

GPL warning: KKManager and AssetRipper are GPL-3.0. Do not copy their code into CardBridge. Read them for format knowledge, then write our own implementation.

## 5. Feasibility of offline mesh, rig, material and texture extraction

All results below come from `[OoOoO]srTohsakaRin_top.unity3d` (extracted read-only to a scratch folder from `srRin.zipmod`) and `abdata\chara\oo_base.unity3d`.

### 5.1 Bundle format [V]

- UnityFS, format version 6, player `5.x.x`, engine `5.6.2f1`.
- Block-info table compression flag = 3 (LZ4HC) in both bundles. Per-block compression was not counted **[U]**.
- A C++ reader therefore needs: UnityFS header and block table, LZ4 (and probably LZMA for other bundles **[U]**), SerializedFile with TypeTree, plus the object types below.

### 5.2 Mesh, skin and skeleton [V]

Mod top bundle object counts: 4139 GameObject, 4139 Transform, 20 Mesh, 20 SkinnedMeshRenderer, 8 Shader, 5 Material, 7 Texture2D, 74 MonoBehaviour.

- Prefab root `[OoOoO]srTohsakaRin_top` → `cf_o_root` (the meshes, under `n_top`) and `cf_j_root` (the skeleton). 367 transforms under the prefab.
- 4 SkinnedMeshRenderers: `acc` (mesh `sh_acc`, 1942 verts), `n_top_a`, `n_top_b` (mesh `base`, 2268 verts), `topbase` (950 verts). Each has a material list.
- Mesh export to OBJ worked: `sh_acc` → 1942 `v`, 1942 `vt`, 1746 `f` **[V]**.
- Skin: 4 bone influences per vertex with weights (first vertex: bone indices 0,4,5,6, weights 0.023/0.299/0.455/0.222) and bind poses (8 for `sh_acc`) **[V]**.
- Bones are referenced **by name**. The 4 renderers reference 19 distinct bone names; **16 exist in the base rig** (`oo_base.unity3d`). The 3 others (`NecklaceL_JNT`, `NecklaceM_JNT`, `NecklaceR_JNT`) are mod-owned extra bones **[V]**. This is how clothing attaches to the body: by bone name onto a shared skeleton.
- Base rig/body `oo_base.unity3d`: roots `p_cf_body_bone`, `p_cf_body_00`, `p_cf_head_bone`, `p_cm_body_00` and low/hit variants; 3353 GameObjects, 930 of them named `cf_j_* / cf_J_* / cf_N_*`; body mesh `o_body_a` (several LODs); textures `cf_body_00_t` 2048², `cf_body_00_mc`, `gomu_t`, `cf_tang_*` **[V]**. Body meshes have **0 blend shapes**.

### 5.3 Textures [V]

7 Texture2D in the mod bundle decoded to PNG: formats 10 and 12 (DXT1 and DXT5) and one 4096×2048, one 2048×2048. A C++ port needs DXT1/DXT5 decoders at minimum; other formats (BC7, ETC, RGBA) were not seen in this sample **[U]**.

### 5.4 Materials [V for names, U for properties]

- Shaders are Illusion "Shader Forge" shaders by name: `main_opaque`, `main_alpha`, `main_item`, `main_emblem`, `main_opaque2`, `mnpb`, `toon_glasses_lod0`, `shadowcast`. They are not portable; PMX needs a fallback mapping (diffuse texture + alpha mode) **[V]**.
- Colour masks: the list CSV lists `MainTex`, `ColorMaskTex`, `MainTex02`, `ColorMask02Tex`. The game tints parts at runtime using the card's `colorInfo`. To match the in-game look the exporter must **bake the tint** (mask × card colours) into the diffuse texture **[V: columns exist; baking not tested]**.
- Material float/colour/texture property tables were not extracted yet: my probe failed on `m_SavedProperties.m_TexEnvs` because in this Unity version it is a list, not a dict. Trivial fix, not done **[U]**.

### 5.5 Outfit part structure (what independent toggles can be built on) [V]

The mod bundle carries a `ChaClothesComponent` MonoBehaviour with typed fields: `rendNormal01` (4 renderers here), `rendNormal02`, `rendAlpha01/02`, `rendAccessory`, `rendEmblem01/02`, `objOpt01/02`, `useColorN01–03`, `defMainColor01–03`. Accessory prefabs carry `ChaAccessoryComponent`. So the game itself groups renderers into named parts, which gives us part names for the tree.

### 5.6 Physics [V for presence]

- Mod garments contain `DynamicBone` and `BoneImplantProcess` scripts; the base rig contains `DynamicBone_Ver02`, `DynamicBoneCollider`, IK scripts. Settings are serialized fields. Converting to PMX rigid bodies and joints is a separate mapping task **[U]**.

### 5.7 The hard part: body and face shape [U]

Card sliders (`shapeValueFace` 52 floats, `shapeValueBody` 44 floats) are applied by game code (`ShapeBodyInfoFemale`, `ShapeHeadInfoFemale`, driven by data tables in `abdata`) onto the skeleton's bone scales and positions. To reproduce the character's proportions offline we must either reimplement that logic (decompiled source is available for study) or capture ground truth once with the game. **This is the highest-risk item for visual fidelity.** It does not block a first PoC (default proportions), but it blocks "the character looks like the card".

### 5.8 Feasibility summary

| Stage | Offline? | Evidence |
|---|---|---|
| Card container + all sections | **Yes** | Fully decoded on 2 Koikatsu cards; block headers and `KKEx` decoded on 1 Sunshine and 1 Party(SP) card |
| Item → asset resolution (base and mod) | **Yes** | 10/10 (Rin) and 24/24 (KK_138747) mod GUIDs found (31 distinct); Slot match proven on Rin |
| Mesh + skin weights + bind poses | **Yes** | OBJ export, weights read |
| Skeleton (shared rig by bone name) | **Yes** | 16/19 bones matched by name |
| Textures | **Yes** (DXT1/5) | Decoded to PNG |
| Materials | **Partly** | Names and shader identity yes; property tables and tint baking not yet |
| Proportion sliders | **Not yet** | Logic known to exist, not reproduced |
| Physics | **Not yet** | Scripts present, mapping undefined |

## 6. Recommendation: viewport and PMX generation

User decision: C++ standalone exe, Windows only. Combined with plan section 16 (Stage 2 folds into the engine):

1. **Viewport: use the engine's Vulkan renderer** (the exe is built on the same engine, like the Map Maker). No second renderer, no Blender in the loop, and Stage 2 needs no second preview. The plan listed this as the likely answer; Milestone 0 found nothing against it.
2. **Core library, no UI or renderer code** (plan 16.1): `card parser → asset resolver → Unity readers → scene assembler → outfit model → PMX writer → validator`. The exe is a thin shell.
3. **Readers written in C++, not a Python sidecar.** Needed pieces, all small and well defined: MessagePack subset (maps, arrays, bin, str, ints, floats), .NET string reader, ZIP read (zipmod), LZ4 (+ LZMA if needed), UnityFS + SerializedFile + TypeTree, Mesh/SkinnedMeshRenderer/Transform/GameObject/Material/Texture2D readers, DXT1/DXT5 decode, PNG (stb_image already present) and PNG write.
4. **PMX writer: our own.** PMX is a documented binary format; a writer is a few hundred lines. Do not depend on Blender to write it. Validation with an independent reader (Blender + mmd_tools, which the KKBP wiki also uses **[S]**, or PMX Editor) is a test step, not a runtime dependency.
5. **Order of work:** Milestone 1 in two parts. **1a**: a Python reference script (scratch, not shipped) that produces golden facts per card (counts, bone names, hashes). **1b**: the C++ headless command-line target in the new folder, which must reproduce 1a. This keeps C++ work testable from the first line.
6. **Dedicated folder + scripts:** create the folder in Milestone 1 (name is an open question, section 10), add it to CMake and to `scripts/build.ps1`, `scripts/package.ps1` (`package.ps1` already has uncommitted edits from other work; change it only in the Milestone 1 commit).
7. **Editor coverage** (project rule): the part tree and import must be reachable from the Map Maker in Stage 2, with an A-row in `map-maker.md` in the same change.

## 7. Limitations

- **Mod assets.** Present in this install for both samples (0 missing of 34 card-GUID pairs, 31 distinct). A card whose mod is missing must show "unresolved" rather than fail. Duplicates: the index showed exactly one zipmod per GUID for those GUIDs; duplicate handling is untested **[U]**. 12 `.rar` and 1 `.7z` in `mods\` were not indexed; whether the game's Sideloader loads them is not verified **[U]**.
- **Body masking.** List tables carry mask columns (`OverBodyMask`, `OverBraMask`, `OverInnerMask`, ...) and `abdata\chara` holds 19 `mt_mask_body_*` and 15 `mt_mask_bra_*` bundles **[V]**. The game hides skin under clothes with these. The mod top used none (columns `0`). Removing clothes can expose body areas the clothing mesh hid; the exporter must keep or apply the masks and warn **[U for behaviour]**.
- **Physics.** DynamicBone → PMX rigid bodies/joints is lossy and unmapped **[U]**.
- **Independent outfit toggles in PMX.** PMX has no per-object visibility switch. Candidate mechanisms, none tested here **[U]**: hide materials by opacity, and use **display frames / group and material morphs** to show or hide sets of materials in MMD-style viewers. Behaviour differs per viewer. Milestone 3 must test the chosen mechanism in the target viewer (plan section 8). KKPMX's own README says hidden-in-game materials are usually set to opacity 0 **[S]**. Keep the real toggles in the manifest/engine so Stage 2 does not depend on PMX viewer behaviour.
- **Shaders.** No parity with Illusion Shader Forge shaders. Export is a baked approximation.
- **Plugin data.** MaterialEditor, ABMX, KKAPI-style plugin payloads change the look and need their own decoders; ignoring them gives a visibly different result **[U]**.
- **Sunshine and Party.** Container proven; Sunshine/Party asset layout not tested (no install). Sunshine has 4 outfits, an `About` block and a higher `Parameter` version.
- **Licenses.** Game and mod assets stay out of the repo (plan 16.1). `manifest.xml` carries `author` and `website` per mod; show it as provenance in the tool.

## 8. Milestone 1 — proof of concept and acceptance tests

Goal: one real card → PMX, headless, game not running. Card for the test: `Koikatu_F_20260718155357521_Rin.png` (10 mods, all present) and `KK_138747.png` (24 mods) as the second case.

**Steps**

1. 1a (Python, scratch): decode card; resolve all rows; open top-clothing prefab and the base body; assemble one skinned mesh + skeleton; write a minimal PMX; reopen it with an independent reader.
2. 1b (C++ CLI in the new folder): do the same without Python.

**Acceptance tests (each reproducible by a script)**

| # | Test | Pass condition |
|---|---|---|
| A1 | Container parse | Rin card: marker `【KoiKatuChara】`, 5 blocks, sizes match header; KK_138747 too |
| A2 | Non-card inputs | Plain PNG, JPG, truncated PNG, `【KoiKatuClothes】` file are rejected with distinct errors |
| A3 | Unsupported family | A `Sun` card reports "recognized, unsupported", not a misparse |
| A4 | Section decode | Rin: 7 outfits; `outfit0` top id `64428`, 20 accessory slots; matches the numbers in this doc |
| A5 | Resolution | Rin: 10 of 10 GUIDs resolve to files; Slot `64428` resolves to bundle `chara/[OoOoO]srTohsakaRin_top.unity3d`, asset `[OoOoO]srTohsakaRin_top`; missing-mod case reports the GUID |
| A6 | Mesh | Top prefab: 4 SkinnedMeshRenderers; vertex/face counts equal the Python reference (`sh_acc` 1942 verts) |
| A7 | Rig | 16 of 19 referenced bones bind to the base rig by name; the 3 `*_JNT` bones are reported as mod-owned |
| A8 | Textures | `Lor` 4096×2048 and `Lor2` 2048×2048 decode to PNG with the same pixel hash as the Python reference |
| A9 | PMX out | File reopens in an independent PMX reader with vertex, material and bone counts equal to what was written; texture paths exist; weights sum to 1 per vertex |
| A10 | No game | Process list shows no `Koikatu.exe`, no BepInEx load; original files unchanged (hash before/after) |

**Exit criterion** (plan section 11): one real card completes the offline path end to end. If A5–A9 cannot be met, stop and revise the approach.

**Out of scope for Milestone 1:** proportion sliders, physics, UI, toggles in PMX, Sunshine.

## 9. Go / no-go

**GO for Milestone 1.** Reasons: the card is fully decodable offline; every sampled item resolves to a real file; mesh, weights, bind poses and textures were read offline with a general-purpose reader. Conditions and watch-items:

1. Treat proportion sliders (5.7) as the first risk to time-box after M1. If reproducing them needs ground truth, the plan's authorization allows one controlled game run; ask the user first, because the user may be playing.
2. The C++ port is real work (UnityFS + TypeTree). Mitigated by the 1a/1b split with golden outputs.
3. Do not copy GPL code (KKManager, AssetRipper).
4. Sunshine is out until a Sunshine install and sample bundles exist.

## 10. Open questions

1. Dedicated folder name and CMake target name (suggest `cardbridge/` with a `cardbridge_core` library and a `cardbridge` exe). The user said "dedicated folder"; name not given.
2. "In the scripts too updated new one": assumed to mean the repo build/package scripts get the new exe. Confirm.
3. Is a Sunshine install available for later? Needed to verify its bundle format.
4. May the game be launched once for ground truth on proportions (user must not be playing)?
5. Which independent PMX reader is the acceptance reference (Blender + mmd_tools, or PMX Editor)? Not installed or tested here.
6. Are the unextracted `.rar` / `.7z` in `mods\` meant to be indexed?
7. Licence of KKBP Exporter and KoikatuCharaReader (not verified).

## 11. Status of the open questions after Milestone 1

1. Folder name: **answered**, `cardbridge/` (library `cardbridge_core`, exe `cardbridge`, tests `cardbridge_tests`, `cardbridge_pmx_crosscheck`).
2. Scripts: **answered**, new `scripts/cardbridge.ps1` and `scripts/cardbridge.bat`; `scripts/build.ps1` and `scripts/package.ps1` were not touched (other work has uncommitted edits in `package.ps1`). Packaging the exe is Milestone 6.
3. Sunshine install: **still open**.
4. Game run for proportion ground truth: **still open**; not needed for Milestone 1.
5. Independent PMX reader: **answered**, the engine's own `PmxFile` loader. Blender with mmd_tools or a PMX editor is still untested and is the right check for viewer-level behaviour in Milestone 3.
6. `.rar` / `.7z` in `mods\`: **still open**.
7. KKBP and KoikatuCharaReader licences: **still open**.
8. New: may a prepared asset library be redistributed? **Open, the owner's legal decision** (12.3).

## 12. Milestone 1 results (2026-10-28)

**Status: Milestone 1 passes its exit criterion.** One real card completes the offline path end to end: card → asset resolution → mesh, weights, rig and textures → PMX, without the game running and without the game install being read at run time. A second real card (with mostly base-game items) also converts. Open items are listed in 12.4; none blocks the exit criterion, several block "looks like the card".

Code: `cardbridge/` (README there). Build: `scripts\cardbridge.ps1 -Test`. Tests: `cardbridge\tests\cardbridge_tests.cpp` (unit tests always; integration tests when `CARDBRIDGE_TEST_*` variables name the inputs), `cardbridge\tests\pmx_crosscheck.cpp` (independent read-back). Reference: `cardbridge\tools\reference_facts.py` produces `cardbridge\tests\golden\rin_reference.json` (counts, names and hashes only, no assets).

### 12.1 Acceptance tests

| # | Test | Result | Evidence |
|---|---|---|---|
| A1 | Container parse | **Pass** | Rin card: marker `【KoiKatuChara】`, 5 blocks, 7 outfits, 24 resolver rows; integration test compares each against the Python reference |
| A2 | Non-card inputs | **Pass** | CLI run on a JPG (`not-a-png`), a plain PNG (`png-without-card-data`), a truncated card (`truncated-png`), an outfit file (`outfit-file-not-a-character`): four distinct errors, exit 2. Also unit-tested with generated inputs, plus junk after a valid PNG (`unknown-card-marker`) |
| A3 | Unsupported family | **Pass** | Sunshine card: `recognised as koikatsu-sunshine, not converted yet`, exit 2, nothing written |
| A4 | Section decode | **Pass** | Rin: 7 outfits, outfit 0 top id `64428`, 20 accessory slots; same numbers as the reference |
| A5 | Resolution | **Pass** | Rin: all mod GUIDs found (24 rows / 10 mods); top slot `64428` → bundle `chara/[OoOoO]srTohsakaRin_top.unity3d`, prefab `[OoOoO]srTohsakaRin_top` (asserted against the reference). Sakura card: 24 mods, 25 of 25 items resolve. A missing item is reported with a status and reason (`missing-mod`, `missing-list-entry`, `missing-bundle`), not skipped |
| A6 | Mesh | **Pass** | Top prefab renderers `acc` 1942 v / 1746 t, `n_top_a` 2268 / 2644, `topbase` 950 / 1193 and body `o_body_a` 8302 / 14028 equal the Python reference exactly (asserted) |
| A7 | Rig | **Pass** | Body and head rigs from `oo_base.unity3d` (838 bones in the exported Rin model); clothing binds by bone name; mod-only bones (`NecklaceL_JNT` etc.) are added as extra bones; accessory skeletons get part-prefixed bones on their attach node. Weights sum to 1 on every vertex (asserted) |
| A8 | Textures | **Pass** | `Lor` 4096×2048, `Lor2` 2048×2048 and `Lor3` 50×50 decode to the same RGBA bytes as UnityPy (FNV-1a 64 compared in the integration test) |
| A9 | PMX out | **Pass** | The engine's own PMX loader (`engine/src/assets/PmxFile.cpp`, no shared code with our writer) reads both exported models: version 2.0, vertex/triangle/material/bone/texture counts match the manifest, material index counts sum to the index buffer, every weight sums to 1 and binds to an existing bone, no bone parent out of range, every texture file exists. Rin: 45,079 vertices, 64,107 triangles, 41 materials, 838 bones, 6 textures |
| A10 | No game | **Pass** | `Koikatu*` / `CharaStudio` / `InitSetting` process count 0 before and after; SHA-256, size and mtime of the card, two zipmods, `oo_base.unity3d` and `Assembly-CSharp.dll` unchanged after a run. `convert` takes only `--library` and `--mods`; only `library import` reads a game folder |

Also checked, not in the original list: a visual sanity check by plotting the exported Rin vertices by part (front and side): a T-pose body with the head on the neck, the twin-tail ribbons on the head, the long coat flaring down from the shoulders and shoes at the feet. Bounding boxes agree (body 0.18–19.06 on Y at scale 12.5, head 18.6–21.6, earrings at the ears). This is a plausibility check, not a fidelity test.

Unit tests: 29 checks (SHA-256, LZ4, inflate, MessagePack, CSV, UTF-16, card-loader errors, PMX writer validation). With the integration environment: 95 checks, 0 failed.

### 12.2 What was built (all verified by the tests above unless marked)

- Card container and section decoding (Koikatsu `【KoiKatuChara】`; Party `SP` accepted by the loader but only Koikatsu was run end to end; Sunshine recognised and refused).
- Mod index over the mods folder (manifest GUIDs, cached in JSON by path, size and mtime; 4,224 zipmods indexed once in about 15 minutes cold, then read from cache in seconds) and list tables (base `ChaListData` and zipmod `.csv`).
- UnityFS v6 reader (LZ4/LZ4HC), serialized-file reader with a type-tree interpreter, typed readers for GameObject, Transform, Mesh, Material, Texture2D, TextAsset, MonoBehaviour; DXT1/DXT5 and common uncompressed texture decode.
- Assembler: shared skeleton and body, head, hair, clothing slots and accessories skinned by bone name; default-state rules (see 12.4); manifest with stable part ids and per-part include/visible flags.
- PMX 2.0 writer (UTF-16LE, BDEF1/2/4 skinning, display frames) and exporter (`.pmx`, `textures/*.png`, `.cardbridge.json`).
- Asset library import and check (12.3).

### 12.3 The asset library: running without a game install

A direct instruction from the user: the tool must be literally separate from the game; anything needed from the game is copied, and users must not need a copy of the game to run it.

- `cardbridge library import --game <install> --library <dir>` copies, once, the files the converter reads: `abdata/chara/oo_base.unity3d` (skeleton and body), `abdata/chara/etc.unity3d` (the shared "none" prefabs mods point empty items at), `abdata/list/characustom/*.unity3d` (item tables), and, in the full profile, every `abdata/chara/*.unity3d` item bundle. Profiles: minimal 34 files / 4.4 MB; full 330 files / 926 MB. A `library.json` records size and hash per file; `cardbridge library check [--hashes]` verifies it. Both profiles were imported and checked (`OK`).
- After that, `convert` and `inspect` take `--library` and never see the game. The game's logic (what its C# assembly does with these files) is not copied: it is reimplemented in C++ from observation and from the decompiled assembly, which was read, not shipped.
- Rin's card needs only the minimal library (all its items are mods); Sakura's needs the full one.
- **Licensing, undecided:** the library is a copy of Illusion's data files. It is excluded from git (`cardbridge/.gitignore`). Whether a prepared library may be given to people who do not own the game is a legal question this study does not answer; the mechanism supports both "each user builds their own" and "ship one". Choosing the second is the project owner's decision.

### 12.4 Open items found by running real cards

| Item | Effect | Next step |
|---|---|---|
| **Head, face and eye textures are not exported.** The head mod's materials reference textures in other bundles (`archive:/cab-...`), and one head texture is BC7 (Unity format 25), which is not decoded. Head materials fall back to the card's skin colour | The face is untextured (flat skin colour) | External-bundle resolution through a CAB-name index over the library and mod zips; BC7 decoder |
| **Body skin is the card's base colour only.** The body material is in another bundle and the game builds the skin texture at run time | Flat-coloured body | Reimplement the skin texture build, or bake from the list's skin textures |
| **Base-game garments get only the list row's `MainTex`.** Colour-mask tinting (`ColorMaskTex`, `MainTex02`) from the card's colours is not baked | Garment colours differ from the card | Bake mask × card colours into the diffuse texture |
| **Default-state variants are chosen by name rules**: `n_top_b` / `n_bot_b` (the game's "nuge" pulled-off state, from `ChaReference` `S_CTOP_*_NUGE`) are left out, and the face's tears (`namida`), gag eyes and tongue are left out. The card's `Status.clothesState` and `showAccessory` are not applied yet | A card saved with half-off clothes would export the worn state | Read `clothesState` / `showAccessory` and set the per-part defaults from them |
| **Proportion sliders** (52 face + 44 body floats) are not applied | The model has default proportions, T-pose | As in 5.7 |
| **Materials**: all Illusion shaders become a diffuse texture plus flat colour; `m_Floats` and `m_Colors` are read but unused; double-sided only for alpha shaders | Look differs from the game | Per-shader mapping |
| **Physics, morphs, rigid bodies, joints**: none written | No cloth or hair motion, no expressions | Milestone 3+ |
| **Unknown item id `99999999`** appears as Rin's `shorts` id and is in no list. It is reported as `missing-list-entry`. It is not in the game assembly, so it is probably written by a plugin | One warning on that card | Find the plugin behind it |
| **Party `SP` and Sunshine** end to end are untested (no matching install) | Unsupported claims stay unsupported | As in section 2 |
| **PMX part toggles** (display frames, material opacity, group morphs) are not tested in any viewer | Section 7 still applies | Milestone 3 |
| **Only the first matching zipmod is used** per GUID; `[Misc] Amiya_ring_mod.zipmod` has no `<guid>` and is reported unreadable | Duplicates untested | Milestone 5 |
| **Output scale** defaults to 12.5 units per metre (MMD convention) and is recorded in the manifest | Check against the engine importer | Stage 2 |

### 12.5 Go / no-go for Milestone 2

**GO.** The offline path works on two real cards, validated by an independent reader. Milestone 2 (live preview and part tree on the engine renderer) can use the exported folder as its input today. Before the model looks like the card, the face textures, body skin, colour tinting and proportions in 12.4 need work; none requires the game to run.

### 12.6 Fidelity pass after the first look (same day)

The first exports were opened in MMD by the project owner and looked wrong: white hair, blank faces, white garments, default proportions. A headless preview renderer (`cardbridge/src/export/Preview.cpp`, written to PNG next to every export as `<name>_preview.png`) reproduced those screenshots, so it was used to check fixes. What changed, with the evidence in the code of the game:

- **Colour baking.** The game builds clothing textures at run time from the list row's `MainTex`/`ColorMaskTex` (and the `02` pair) plus the card's `colorInfo`, and assigns them to the first material of the renderers its `ChaClothesComponent` lists (`rendNormal01/02`, `rendAlpha01/02`). Mask channels R, G, B select colour 1, 2, 3. A list texture name is the **asset name in the bundle's container**, not the Texture2D's own name, and a list bundle of `"0"` means the item's own bundle. Implemented; the game builds nothing when the main texture or the mask is missing, and neither does CardBridge.
- **Jacket and sailor tops.** A top whose list `Kind` is 2 (jacket) or 1 (sailor) is an empty item (`p_dummy`); its garments are sub-parts A/B/C, categories 210+k / 200+k, with ids from the coordinate's `subPartsId`, coloured with the top's colours (part C's first colour is `colorInfo[3]`). Implemented. This is what put Sakura's shirt and vest back.
- **Accessories** attach to their named node and keep their own skeleton (part-prefixed bones), they are tinted by their card colours, and an accessory without a texture takes its first colour as its material colour.
- **Face parts.** Eyebrow, upper and lower eyeline, nose line, eye white and iris textures come from the list rows the card's face ids select (base or mod), tinted with the card's colours. Implemented; highlights, lips and makeup layers are not.
- **Default-state variants.** The pulled-off top and bottom (`n_top_b`, `n_bot_b`, from `ChaReference` `S_*_NUGE`) and the face's tears, gag eyes and tongue are left out.
- **Body proportions.** The 44 body sliders are applied. The game's `ShapeBodyInfoFemale.Update/UpdateAlways` formulas (about 700 lines of straight-line arithmetic) are **copied as generated C++** by `cardbridge/tools/gen_shape.py` from the decompiled assembly, with the key-frame table (`cf_anmShapeBody` in `oo_base.unity3d`) and the slider table (`cf_custombody` in `list/customshape.unity3d`) read from the library. Skeleton world matrices are rebuilt from the changed local transforms, so clothing and accessories follow. Verified visually on both cards (bust, waist and hips follow the card); not verified numerically against the game. The face formulas (`ShapeHeadInfoFemale`) are generated too (`ShapeHeadGenerated.inc`) but not wired in: the face key frames come from the head item's own list row (`ShapeAnime`).
- Tests still pass (99 checks); the engine's PMX loader reads both exports.

Still wrong or missing, in order of how visible it is: skin texture, lips, blush and makeup; face shape; BC7 and other-bundle textures on some face parts; hair colour for hair-type accessories and plugin-recoloured hair; the neck ribbon of Sakura's jacket (a renderer with no material); underwear showing through thin garments (the game masks it, CardBridge only hides a bra when the top's `NotBra` is set); no shading or outline, so the preview and MMD look flatter than the game.

### 12.7 Face pass and the bald spot (same day)

- **Bald spot on Sakura's forehead: fixed.** Cause: each accessory has a per-slot move/rotate/scale in the card (`ChaFileAccessory.PartsInfo.addMove`, position in centimetres) that the game applies to the prefab's `N_move` / `N_move2` nodes. Sakura's bangs are several copies of the same hair pieces (ids 568, 769, 578) placed differently by those values; ignored, they stacked in one spot and left the parting bare. Implemented in the assembler (`UnityFile::worldMatrix` takes local-matrix overrides). Hair materials are also marked double-sided, because hair cards are drawn from both sides in the game and a culled PMX loses half of them.
- **Face skin and body skin textures.** The head's own base texture and the body's `cf_body_00_t` are tinted through their colour masks with the card's two skin colours; cheek, lip line, lip and eye-shadow layers are blended on top. BC7 textures now decode (vendored `third_party/bc7decomp`, MIT or public domain, Richard Geldreich).
- **Close-up check.** Every export also writes `<name>_head.png`, a head close-up, so the face can be compared with the card's picture. Sakura's close-up now matches the card's layout (parting, bangs, ribbon, eyes); hair colour is paler and flatter than the card (the game's hair shader blends root, base and tip colours and adds toon shading and the colour conversion the KKBP notes describe).
- **Rin's face is still wrong**: a red mark on the nose (a mod nose texture, likely a mask in a channel CardBridge does not interpret), blank eyes (the "No Iris" mod removes the iris and the mod head supplies its own eye material), and grey hair. Not fixed.
- Not done: face shape sliders (formulas are generated but not wired), eyebrows over the forehead hair, eye highlights, mole, paint layers.

### 12.8 Card-embedded plugin data, colour conversion, shape fixes

- **Why the Rin sample looked wrong.** Its card carries a MaterialEditor payload (`com.deathweasel.bepinex.materialeditor`, 15 embedded images, 16 MB) that replaces the main texture of the outfit, hair and accessory materials, and a KoiSkinOverlayX payload (`KSOX`) with body, face and eye overlays. Without them the mod outfit shows its default blue textures, the hair is grey and the eyes are blank (the "No Iris" mod). `src/card/MaterialEdits.*` now decodes the main-texture replacements (keyed by coordinate, object type, slot and material name; object types follow MaterialEditor: clothing 1, accessory 2, hair 3, character 4; the slot is the clothing kind, accessory index or hair kind) and the overlays, which are alpha-composited over the baked skin and iris textures. Overlay types seen in the sample: 3 body, 4 face, 8 and 10 eyes. **[U]** the numbering is inferred from the images, not from KSOX's source; other types are ignored. Not decoded: colour, float, keyword and shader overrides of MaterialEditor, and `HairAccessoryCustomizer`.
- **Colour conversion.** The game shows card colours more saturated than stored: measured against the cards' own pictures, a vest colour of saturation 0.25 appears at 0.49 and hair 0.20 at 0.47, i.e. about the square root of the saturation at unchanged value. `kkColor` applies this to clothing, accessory, hair-type and face-part colours, not to skin.
- **Out-of-range sliders.** Cards may store a slider slightly outside 0..1 (the Rin sample has -0.04). The game clamps; without the clamp the key-frame index went negative and the spine bones collapsed to huge coordinates, which broke the whole body. `AnimKeys::get` clamps the rate.
- **Heeled shoes: left as modelled.** The Rin boots are modelled for a flat foot (level sole, heel stack hanging 4.4 cm below it), so on the T-pose foot the toes stand a heel-height above the floor. Three ways of posing the foot were tried and dropped at the owner's call: tilting the whole foot about the ankle (sloped, pointed toes), shearing the forefoot down over the arch (S-shaped boot) and pointing the ankle with the toes bent back (right silhouette, but 1,700 edges at the ankle bend stretched or squashed 10-80 % and the forefoot flare showed from behind). The boots are exported as the mod models them; posing belongs to the viewer or the engine. Each export writes `<name>_feet.png`, a front, side and back close-up of the feet.
- **Face shape, hair parent, hair colour (cards `KK_016673`, `KK_015975`).** The 52 face sliders are applied now: the generated `ShapeHeadInfoFemale` formulas run on the head rig with the key frames from the head item's `ShapeAnime` row and the `cf_customhead` table, including the parent-lossy-scale term that keeps the head's net scale uniform under the body's head-bone scale. Hair items (not accessories) hang under `cf_J_FaceUp_ty`, the game's `HairParent`; their own bones have no ancestor in the shared skeleton, and without the anchor a whole back-hair item landed at the origin below the feet. Hair items take the card's `baseColor` as diffuse (the root-to-tip gradient and shading are not modelled); a missing texture no longer leaves the hair white. Feet are exported exactly as the game skins them (foot posing was tried and removed, see above).
- **Shading and outline (F9, first pass).** Each material now carries the Shader Forge toon data: `_ShadowColor` becomes the PMX ambient (diffuse times shadow), `_SpecularColor` times `_SpecularPower` the specular, `_LineColor` / `_LineWidthS` the PMX edge colour and size with the edge draw flag. The preview renders two-tone toon shading with the same shadow colour. Not modelled: the shader's ramp textures, rim light, hair gloss. **Colour conversion corrected:** the saturation boost is now a plain doubling; the earlier square-root fit turned near-white colours (0.92, 0.95, 0.93) mint on `KK_222610`.
- **Outfit choice.** `KK_222610` stores `coordinateType` 1, but its card picture shows outfit 0 (the other outfits are different clothes). The tool follows the stored value; `--outfit N` overrides it.
- **Preview renderer.** Textures are mip-mapped and bilinearly filtered, so 4096 pixel atlases no longer alias into speckle. Remaining speckle on the Rin skirt and coat is z-fighting between overlapping layers of the same garment, which a depth-buffered preview cannot resolve; the game draws such layers as separate alpha passes.

## 13. Reproduction notes

- Scratch scripts and extracted data are outside the repo. Scripts copied to the shared notes folder `...\side-session-notes\local_bd91632b-1a9f-4e7f-a664-db96ab013587\m0-scripts\`: `decode_card.py`, `dump_chara.py`, `resolve_info.py`, `scan_mods.py`, `check_mods.py`, `scan_cards.py`, `read_bundle.py`, `probe_mod.py`, `inspect_base.py`, `bundle_facts.py`.
- Run Python with `python -I -X utf8` (the `-I` flag ignores `PYTHONIOENCODING`; the markers are non-ASCII).
- The install was only read. One zipmod bundle (`[OoOoO]srTohsakaRin_top.unity3d`) was extracted to scratch; `Assembly-CSharp.dll` and `KKManager.Core.dll` were copied to scratch before decompiling.
- Sources: [KKPMX](https://github.com/CazzoPMX/KKPMX), [KKBP wiki FAQ](https://kkbpwiki.github.io/faq), [KKBP export panel](https://kkbpwiki.github.io/export_panel), [Sideloader zipmod format](https://github.com/IllusionMods/BepisPlugins/wiki/1-Introduction-to-zipmod-format), [BepisPlugins](https://github.com/IllusionMods/BepisPlugins), [KKManager](https://github.com/IllusionMods/KKManager), [KoiCatalog](https://github.com/im-mi/KoiCatalog), [UnityPy](https://github.com/K0lb3/UnityPy), [AssetRipper](https://github.com/AssetRipper/AssetRipper).
