# CardBridge — Facial Expression, Eye, Tears, Sweat and Blush Export to PMX

**Investigation and implementation roadmap**

- **Date:** 2026-11-25
- **Scope:** CardBridge 0.8.5 (`build\bin\cardbridge.exe`), Koikatsu BetterRepack RX21 and Koikatsu Sunshine EX BetterRepack R13
- **Method:** static source reading, Unity bundle probes, decompiled KKS runtime code, two real offline exports, PMX parsing, and independent PMX-format cross-checks
- **Evidence labels:** `[V]` verified from source/assets/export; `[S]` strongly supported by multiple independent signals but not visually confirmed; `[H]` hypothesis/design proposal; `[U]` unknown
- **Constraints honoured:** no game or MMD launch, no production source change, no game-file change, no commit. The two real cards are neutral (`eyesPtn=0`, `tearsLv=0`, `hohoAkaRate=0`), so the missing features are not visible in those two PMX files; the gaps are proven from the card schema, the game assets and the runtime code.

---

## A. Executive Summary

CardBridge already has a strong facial-expression export. Both real cards produce 601 PMX morphs, with the game's eye/brow/mouth patterns, 359 raw per-mesh shapes, 46 MikuMikuDance-standard morphs, six English aliases, semantic names, correct panels, and display frames that use exactly 255 of MMD's 255 listed-morph slots. The expressions are not the problem.

The gaps are the features that are not ordinary FaceBlendShape patterns:

1. **Tears are not exported at all.** `tearsLv` is 0–3, `tearsFlags` selects exactly one of `cf_O_namida_S/M/L`, and CardBridge deliberately skips every `namida` renderer (`Assembler.cpp:3904-3906`). The assets exist in both games and have 27 eye-expression shapes each. This is the largest missing facial feature.
2. **Blush is baked, not controllable.** `hohoAkaRate` is applied as `_overcolor2.a = 0.2 × rate` before texture baking (`Assembler.cpp:3986-3989`). There is no PMX morph, so a user cannot change blush after export.
3. **Independent eye control is only partly there.** The PMX has independent `左目`/`右目` bones, a `両目` parent, and left/right wink morphs. But the card's pupil direction/size and highlight placement are baked into static per-eye iris textures, `Eye Roll` is one combined UV morph for both eyes, and there are no per-eye pupil morphs.
4. **Gag/expression eyes are missing.** This is now fully explained, not guessed: the game's `cha_eyeset` list row selected by `eyesPtn` contains `EyeObjNo` (which of `cf_O_gag_eye_00/01/02`), `MainTex` (the gag-eye texture), `TileAnim`, `SizeSpeed`, `SizeWidth`, `AngleSpeed`, `Yurayura`, and `EpsTex` (the iris expression texture). Rows 21–32 cover spiral eyes, star pupils, heart pupils, heart eyes, flame eyes, deformed wink/closed/crying eyes and line eyes. CardBridge reads the eye pattern for the face shape but skips the gag-eye renderers and never applies `EpsTex`.
5. **Sweat is a CardBridge invention, not card data.** The game has no sweat geometry; sweat is runtime/plugin (SkinEffects). The current `Sweat`/`Sweat Heavy` droplets are a useful generic effect, but they are not a faithful export of a card field.
6. **The PMX material-morph encoding is correct.** The earlier concern that the sphere factor should be `float3` is unfounded: CardBridge writes/reads `float4` and `mmd_tools` reads `float4` for texture, sphere and toon factors.
7. **The viewer does not show the effects.** `BuildLiveMorphs` builds expressions, raw shapes, MMD standard morphs, winks and aliases, but not the four effects. `AccumulateLiveMorph` only applies vertex offsets, so UV and material morphs cannot be previewed. PMX import turns only type 1 vertex and type 0 group morphs into scene data; material/UV/bone/flip morphs are preserved but not editable, and the Explorer lists only type 1.
8. **KKS is close but not identical.** KKS adds four eye-expression morphs, uses `Koikano/*` shaders, and its create-material shader blobs are still unreadable by CardBridge, so face/eye texture fidelity needs visual confirmation. KKS `hlUpX/hlDownX` are parsed but ignored; the test card has 0.5/0.5, so the error is invisible there.

**Recommended direction:** keep the existing vertex-morph expression system. Add card-faithful tears as three switchable meshes with alpha-0 materials and material morphs; add per-eye UV morphs for independent iris control; add a cheek overlay + material morph for blush; and read the `cha_eyeset` row to export the exact gag/expression eye mesh, texture and `EpsTex` overlay. Then extend the viewer to preview UV/material/overlay effects and rebalance the 255-slot display-frame budget.

**Bottom line:** expressions are already exportable and strong. The missing card-faithful features are tears, independent per-eye iris morphs, blush control, and gag/expression eyes. The PMX format can express all of them with the mechanisms CardBridge already uses; the main risks are display-frame budget, KKS texture fidelity, and viewer preview support, not PMX capability.

---

## B. Existing Architecture

### B.1 Input pipeline

```
card PNG
  → CardFile            (PNG chunks, MessagePack, face/body/status/outfit fields)
  → Resolver / ModIndex (base library + zipmods, list rows, bundle selection)
  → UnityBundle / UnitySerialized / UnityScene
  → Assembler           (head/body/clothes/hair/accessories, materials, textures, bones, shapes)
  → Scene               (meshes, vertices, UVs, bones, weights, materials, textures, shapes, expressions)
  → Exporter            (PMX model, physics, morphs, names, textures)
  → PmxWriter           (PMX 2.0/2.1)
```

Key files:

| Area | File | Responsibility |
|---|---|---|
| Card | `src/card/CardFile.cpp` | Reads KK/KKS card chunks, face/status/outfit data, mods |
| Resolve | `src/resolve/Resolver.cpp`, `ModIndex.cpp`, `ListTable.cpp` | Finds bundles, list rows, mods |
| Unity | `src/unity/UnityBundle.cpp`, `UnitySerialized.cpp`, `UnityScene.cpp` | Reads Unity 5.6/2019 bundles, meshes, materials, textures, MonoBehaviours |
| Assemble | `src/assemble/Assembler.cpp`, `GarmentFix.cpp` | Builds the scene; applies card colours, eyes, blush, clothes, shapes |
| Scene | `src/assemble/Scene.h` | Mesh/shape/expression/material/texture/bone representation |
| Export | `src/export/Exporter.cpp`, `PmxPhysics.cpp`, `MmdNames.cpp`, `FaceMorphs.cpp`, `MorphCatalog.cpp` | Builds PMX, morphs, names, effects, viewer catalog |
| PMX | `src/pmx/PmxWriter.cpp`, `PmxReader.cpp`, `PmxToScene.cpp` | Writes/reads PMX; imports PMX into a scene |
| Viewer | `viewer/main.cpp`, `Morphs.inc`, `Explorer.inc`, `Preview.cpp` | Live preview, morph sliders, explorer, PMX import |

### B.2 Facial morph pipeline

1. **Source.** The head prefab carries `FaceBlendShape` MonoBehaviours with `EyebrowCtrl`, `EyesCtrl`, `MouthCtrl`. Each control has `OpenMin`, `OpenMax`, `FBSTarget` (target GameObject + `PtnSet`), and each pattern has `Close`/`Open` blend-shape indices. CardBridge applies the card's resting weights (`eyebrowPtn`, `eyesPtn`, `mouthPtn`, `*OpenMax`) to the vertices (`Assembler.cpp:3680-3714`). `[V]`
2. **Expressions.** `AddExpressionMorphs` (`PmxPhysics.cpp:717-757`) writes one PMX vertex morph per pattern state (brow/eye/mouth, open/closed) and one raw vertex morph per blend shape, named `<mesh>.<shape>`. Panels are 1 brow, 2 eye, 3 mouth, 4 other, assigned by mesh/shape name keywords. `[V]`
3. **MMD standard morphs.** `AddMmdMorphs` (`MmdNames.cpp`) builds `あ い う え お ん ワ ∧ ω ▲ ぺろっ 口角上げ 口角下げ にやり ムッ 怒った 口 大口 心配する えー まばたき 笑い じと目 ｷﾘｯ 喜び 慈愛 真面目 困る 怒り 悲しい 上 やっかい ウィンク ウィンク右 ウィンク２ ｳｨﾝｸ２右`, plus the iris-follow compensation. The one-eyed winks are cut at the face middle. `[V]`
4. **Aliases.** `AddFaceAliases` adds `Mouth Ah/Ee/Oo/Mm/Smile` and `Eye Happy` as group morphs pointing at the standard morphs. `[V]`
5. **Semantic names.** `ApplySemanticNames` renames expressions and raw shapes using `FaceStemLabels()`; unknown keys keep `Unknown …`, and the original identifier is kept in `nameEn`. `[V]`
6. **Effects.** `AddFaceEffects` (`FaceMorphs.cpp:520-721`) appends `Eye Roll`, `Heart Eyes`, `Sweat`, `Sweat Heavy` as overlay geometry + UV/material morphs. `[V]`
7. **Display frames.** `FacialDisplayFrames` (`FaceMorphs.cpp:723-764`) adds `Eye`, `Brow`, `Mouth`, `Other` frames after the special `表情` frame, sharing a 255-listed-morph budget. `[V]`

### B.3 Real export results

Both exports used the existing `build\bin\cardbridge.exe` and the real cards named below. No rebuild was needed.

| Metric | KK (`Big Sisterly.png` → `Teranishi Sumire`) | KKS (`KKS BR-Chan.png` → `Kitagawa Nozomu`) |
|---|---:|---:|
| Vertices | 39,101 | 38,816 |
| Triangles | 52,159 | 55,806 |
| Bones | 719 | 724 |
| Materials | 30 | 26 |
| Textures | 43 | 22 |
| PMX size | 7,549,276 B | 7,383,336 B |
| Morphs | 601 | 601 |
| Vertex morphs | 575 | 579 |
| Group morphs | 10 | 9 |
| UV morphs | 1 | 1 |
| Material morphs | 15 | 12 |
| Semantic renames | 529 | 533 |
| Raw shapes | 359 | 359 |
| `Unknown` names | 99 | 99 |
| Effect morphs | 597–600 | 597–600 |
| Overlay vertices | 1,114 | 1,114 |

Display frames:

| Frame | KK | KKS |
|---|---:|---:|
| `表情` (special) | 72 | 68 |
| `Eye` | 71 | 74 |
| `Brow` | 23 | 23 |
| `Mouth` | 89 | 90 |
| **Total listed** | **255** | **255** |

The exact-255 total is important: MMD 9.32 stores the number of morphs listed by all display frames in one byte and corrupts its heap at 256. There is **no free display slot** for a new morph. Adding tears/blush/gag/per-eye morphs requires removing some existing listed morphs (raw shapes are the obvious candidates) or leaving the new morphs unlisted. `[V]`

### B.4 PMX morph support

`PmxWriter`/`PmxReader` support PMX morph types 0 (group), 1 (vertex), 2 (bone), 3 (UV), 4–7 (extra UV), 8 (material), 9 (flip), 10 (impulse). CardBridge currently writes 0, 1, 2, 3, 8, 9; the effects use 3 and 8. `[V]`

Material morph layout (`PmxWriter.cpp:353-380`, `PmxReader.cpp:292-305`):

- diffuse `float4`
- specular `float3`
- specular power `float`
- ambient `float3`
- edge colour `float4`
- edge size `float`
- texture factor `float4`
- sphere factor `float4`
- toon factor `float4`

This matches `mmd_tools`' `MaterialMorphOffset.load()`, which reads `float4` for texture, sphere and toon. The earlier concern about sphere being `float3` was unfounded. `[V]`

### B.5 Viewer architecture

- `BuildLiveMorphs(scene)` (`MorphCatalog.cpp`) builds expressions, raw shapes, MMD standard morphs, winks and aliases as recipes over the scene's blend shapes. It deliberately excludes outfit/nude material morphs (the Parts tab handles those) and **does not call `AddFaceEffects`**. `[V]`
- `AccumulateLiveMorph` (`MorphCatalog.cpp:165-217`) accumulates vertex offsets only, with the iris-follow compensation. `[V]`
- `Morphs.inc` applies `g.morphDelta` to the copied mesh before skinning. It has no UV or material channel. `[V]`
- `PmxToScene::BuildScene` (`PmxToScene.cpp:153-200`) converts type 1 vertex morphs to scene blend shapes and type 0 group morphs to expressions; material/bone/UV/flip/impulse morphs are preserved in the document but not editable. `[V]`
- `Explorer.inc:61-78` counts material/other morphs but lists only type 1. `[V]`
- `Loaded::pmxDoc` keeps the full PMX document, so the viewer can be extended to read the preserved material/UV morphs without re-importing. `[V]`

---

## C. KK vs KKS

### C.1 Verified differences

| Aspect | KK RX21 | KKS R13 | Impact |
|---|---|---|---|
| Card marker | `【KoiKatuChara】` | `【KoiKatuCharaSun】` | Both supported; family detection works. `[V]` |
| Unity | 5.6.2f1, serialized 17 | 2019.4.9f1, serialized 21 | Both readers work. `[V]` |
| Face mesh/shapes | `cf_O_face` 2,695 verts, 81 shapes; `cf_O_mayuge` 34; `cf_O_eyeline` 27; `cf_Ohitomi_L/R` 107 verts, 27 shapes; irises 179 verts, 0 shapes | Same core counts; KKS has extra eye patterns 28/29 and a mod head with 36 brow shapes | KKS export has 4 extra eye expression morphs: `Eye Smile Closed 4/5`, `Eye Wink Left/Right Open 2`. `[V]` |
| Tear meshes | `cf_O_namida_S/M/L` 304/364/494 verts, 27 shapes each; material `cf_m_namida_00` shader `Shader Forge/toon_glasses_lod0`; texture `cf_namida_00_t` 512² BC7 | Same meshes/shapes; shader `Koikano/main_clothes_item_glasses`; same texture | Same geometry; material bake differs by shader. `[V]` |
| Gag-eye meshes | `cf_O_gag_eye_00/01/02` 240 verts, 0 shapes | Same | Same geometry. `[V]` |
| Gag-eye materials | `cf_m_gageye_00/01/02`, shader `Shader Forge/toon_textureanimation` | Same names, shader `Koikano/sub_texture_animation` | KKS needs the `Koikano` material path. `[V]` |
| Gag/expression textures | `cf_t_gageye_00..09`, `cf_t_expression_00/01` in `mt_eye_00.unity3d` | Same names; adds `_low` variants | Same asset set; KKS adds low-res variants. `[V]` |
| Iris material | `Shader Forge/toon_eye_lod0`; 4 textures | `Koikano/main_eye`; 8 textures incl. `_expression`, `_overtex3`, `_texcoord*` | KKS has more shader channels; CardBridge bakes the visible ones. `[V]` |
| Face/eye textures | Source textures baked to `baked_cf_m_*` files (43 textures) | Create-result textures `cf_m_face_create_result_*`, `cf_m_eye_create_result_*`, `cf_m_eyewhite_create_result_*` (22 textures) | KKS create-material shader blobs are unreadable; visual fidelity needs confirmation. `[V]` |
| Morphs | 601 (575 vertex / 10 group / 1 UV / 15 material) | 601 (579 vertex / 9 group / 1 UV / 12 material) | Same total; KKS has 4 more eye vertex morphs and fewer outfit material morphs. `[V]` |
| Outfit morphs | `outfit0` set | `outfit2` set | KKS card uses a different outfit slot. `[V]` |
| Blush | `_overcolor2.a = Lerp(0, 0.2, hohoAkaRate)` | Same | Same formula. `[V]` |
| Tears | `tearsLv` 0–3, `tearsFlags` diagonal, `S_TEARS_01/02/03` → S/M/L | Same | Same semantics. `[V]` |
| Eye highlight X | Not in the card face | `hlUpX`, `hlDownX` | CardBridge parses but ignores them; test card is 0.5/0.5, so no visible error there. `[V]` |
| Makeup | `baseMakeup` 10 | `baseMakeup` 11 | Extra KKS field; not facial-morph critical. `[V]` |
| Eyeset rows | 39 rows | 41 rows | KKS adds 2 rows; rows 21–32 are identical. `[V]` |

### C.2 What is identical

- The head mesh names, tear mesh names, gag-eye mesh names, eye white/iris names, and the 27-shape eye/tear shape sets. `[V]`
- The `cha_eyeset` rows 21–32: same names, same `EyeObjNo`, same textures, same tile/size/angle fields. `[V]`
- The `tearsFlags` table and `ChaReference` mapping. `[V]`
- The `ChangeHohoAkaRate` formula. `[V]`
- The PMX morph/display-frame behaviour. `[V]`

### C.3 KKS-specific risks

1. **Create-material shader blobs.** `docs/KKS_FORMAT_FEASIBILITY.md:158-168` documents that KKS `Shader` objects have nested per-subshader descriptor arrays; `UnityFile::readShaderBlob()` reads them flat and returns an empty blob. The software shader runner behind `RunCreateJobs()` cannot run KKS create materials. Body, face, eye and clothing textures are mostly built by those jobs, so without a new reader the KKS export keeps flat input textures and loses baked colours/overlays. The real KKS export did produce `cf_m_face_create_result_*`, `cf_m_eye_create_result_*` and `cf_m_eyewhite_create_result_*` textures, so the pipeline is not empty, but visual confirmation in MMD is still required. `[V]`
2. **KKS `hlUpX/hlDownX`.** The card has both; CardBridge uses only `hlUpY/hlDownY` and the shared `pupilX`. Non-0.5 horizontal highlight offsets will be wrong. `[V]`
3. **KKS eye shader channels.** `_expression`, `_overtex3`, `_texcoord*` are not baked by the KK-oriented iris path. The `EpsTex` overlay is exactly the `_expression` channel, so KKS gag/expression-eye export must handle it explicitly. `[V]`
4. **KKS gag material shader.** `Koikano/sub_texture_animation` instead of `Shader Forge/toon_textureanimation`; the material bake must accept both. `[V]`

---

## D. Facial Expressions

### D.1 What the card carries

The card's `FaceBlendShape` data is a set of pattern states per control:

- `EyebrowCtrl` / `EyesCtrl` / `MouthCtrl`
- `OpenMin`, `OpenMax`
- `FBSTarget` entries: target GameObject path ID + `PtnSet`
- Each pattern has `Close` and `Open` blend-shape indices

CardBridge reads these from the head prefab's MonoBehaviours, applies the card's resting pattern/open-max to the vertices, and keeps every shape for morphs. `[V]`

The card status fields that select the rest state are `eyebrowPtn`, `eyesPtn`, `mouthPtn`, `eyebrowOpenMax`, `eyesOpenMax`, `mouthOpenMax`. Both test cards are `0/0/0` with open max 1/1/0. `[V]`

### D.2 What the exporter writes

- **One vertex morph per expression pattern state**, e.g. `Eye Angry Open`, `Eye Neutral Closed`, `Brow Serious Open`, `Mouth A (Large) Open`.
- **One raw vertex morph per blend shape**, e.g. `Eye Shape Neutral Closed` (`nameEn` = `cf_O_face.eye_face.f00_def_cl`).
- **46 MMD-standard morphs** and **6 English aliases**.
- **Panels:** 1 brow, 2 eye, 3 mouth, 4 other.
- **Display frames:** `表情` plus `Eye`, `Brow`, `Mouth`; `Other` is omitted because the 255 budget is exhausted. `[V]`

Counts by panel (vertex morphs):

| Panel | KK | KKS |
|---|---:|---:|
| Brow (1) | 72 | 72 |
| Eye (2) | 217 | 221 |
| Mouth (3) | 286 | 286 |
| Other (4) | 0 | 0 |
| **Total vertex** | **575** | **579** |

The 99 `Unknown` names per export are keys whose meaning is not documented (`gimo` etc.); they are deliberately not invented. `[V]`

### D.3 Coverage and gaps

**Covered:**

- All eye/brow/mouth pattern states in the sampled base heads.
- Raw per-mesh shapes, including teeth, tongue, eyeline, nose line, eye whites.
- MMD-standard morphs and one-eyed winks.
- Semantic English names with source identifiers preserved.
- Display frames and the 255-morph safety limit.

**Not covered:**

- FV/L visemes: the Koikatsu head data has no FV or L patterns; CardBridge does not invent them. `[V]`
- Combined "Face Angry/Sad/…" presets: the cards carry no brow+eye+mouth combo list. `[V]`
- Unknown keys: 99 per export keep `Unknown …`; no fabricated meanings. `[V]`
- The new effects (tears, blush, gag/expression eyes) are outside the pattern system; see F. `[V]`

### D.4 PMX mapping

| Source | PMX mechanism | Panel | Status |
|---|---|---|---|
| Expression patterns | Vertex morph (type 1) | 1/2/3 | Done |
| Raw blend shapes | Vertex morph (type 1) | 1/2/3 | Done |
| MMD standard morphs | Vertex morphs / group morphs | 1/2/3 | Done |
| English aliases | Group morphs (type 0) | 1/2/3 | Done |
| Winks | Vertex morphs split at face middle | 2 | Done |
| Display frames | `表情` + `Eye`/`Brow`/`Mouth` | — | Done; 255 used |

### D.5 Evidence

- `PmxPhysics.cpp:717-757` — expression/raw morph generation.
- `MmdNames.cpp:279-378` — eye bones, winks, standard morphs, iris follow.
- `FaceMorphs.cpp` — semantic registry, aliases, effects, display frames.
- `kk_face_export/morphs.txt`, `kks_face_export/morphs.txt` — 601 morphs, types, panels, names.
- `kk_face_export/blendshapes.txt`, `kks_face_export/blendshapes.txt` — real shape inventories.
- `docs/FACE_MORPHS.md` — implemented design and honest limitations.

---

## E. Eye Movement

### E.1 Card fields and what they mean

The card face carries:

- `pupil[2]`, `pupilX`, `pupilY`, `pupilWidth`, `pupilHeight`
- `hlUpId`, `hlDownId`, `hlUpColor`, `hlDownColor`, `hlUpY`, `hlDownY`
- KKS only: `hlUpX`, `hlDownX`
- `shapeValueFace[33]` — iris rotation/tilt
- status: `eyesPtn`, `eyesOpenMax`, `eyesBlink`, `eyesYure`, `eyesLookPtn`, target type/angle/range/rate

The game's `EyeLookMaterialControll` computes per-eye texture scale/offset from the pupil sliders and highlight offsets, with the gaze straight ahead. CardBridge replicates that math in `Assembler.cpp:3945-3977`:

- `_MainTex` ST = `(1 + sx, 1 + sy, vx - sx*0.5, vy - sy*0.5)`
- `_overtex1` ST = same + `hlUpY`
- `_overtex2` ST = same + `hlDownY`
- `_rotation` from `shapeValueFace[33]`
- Left/right X is mirrored by `(left ? 1 : -1)`.

This is baked into the iris texture; the PMX material samples the baked texture with static UVs. `[V]`

The card's `pupilX/Y` is a shared pupil direction, mirrored for the right eye; it is not an independent left/right pupil slider. The game's `eyesLookPtn` is a runtime tracking pattern (`EyeLookCalc`), not a static morph. `[V]`

### E.2 What the PMX has

- **Bones:** `左目` and `右目` (independent), `両目` (both, append parent). The eye pivot is moved back into a sphere fitted to the eye white, with append ratio 0.4; the iris follows the white's mean offset per morph. `[V]`
- **Winks:** `ウィンク`, `ウィンク右`, `ウィンク２`, `ｳｨﾝｸ２右`, cut at the face middle. `[V]`
- **`Eye Roll`:** one UV morph, 358 offsets (both irises), `(0, -0.16, 0, 0)` per vertex, panel 2. Both eyes roll together. `[V]`
- **`Heart Eyes`:** one material morph over two iris-clone overlays, panel 2. Both eyes share one material. `[V]`
- **No per-eye pupil morphs.** No gaze morphs. No `eye_posx/negx/posy/negy` shapes in the sampled base heads; ExpressionLink is not installed. `[V]`

### E.3 Independent eye control design

The PMX already supports independent gaze through the `左目`/`右目` bones. For stylized pupil control, add:

| Morph | Type | Effect | Panel |
|---|---|---|---|
| `Eye Roll L` | UV (3) | Roll the left iris only | 2 |
| `Eye Roll R` | UV (3) | Roll the right iris only | 2 |
| `Eye Roll` | Group (0) | Both at 1.0, preserving the existing name | 2 |
| `Pupil Size L/R` | UV (3) | Scale the iris UV around its own UV centre | 2 |
| `Pupil X L/R` | UV (3) | Shift the iris UV horizontally | 2 |
| `Pupil Y L/R` | UV (3) | Shift the iris UV vertically | 2 |
| `Heart Eyes L/R` | Material (8) | Separate materials per eye, if per-eye hearts are wanted | 2 |

The pupil-size UV offset per vertex is `(uv - centre) * (scale - 1)`, where `centre` is the iris UV bounding-box centre. The position morph is `(du, dv)`. These are additive relative to the baked rest ST, so the card's pupil direction/size is preserved. `[H]`

**Limits:** PMX UV morphs are additive and MMD applies the first two components; large shifts can sample outside the iris texture's transparent ground. The iris texture must keep a transparent border. Independent highlight movement is not possible with the baked iris texture; it would need separate `_overtex1/2` overlay meshes with their own textures and UV morphs. `[H]`

### E.4 KKS highlight X

KKS adds `hlUpX`, `hlDownX`; CardBridge parses them but does not use them. The test card has 0.5/0.5 (centre), so the current export is correct for that card. For non-0.5 cards, the highlights will be horizontally misaligned. The fix is to apply the same `lerp` used for `hlUpY/hlDownY` to the X component of the `_overtex1/_overtex2` ST. `[V]`

### E.5 Evidence

- `card_fields.txt` (KK/KKS) — real pupil/highlight values.
- `Assembler.cpp:3942-3989` — eye ST, highlight textures/colours, rotation, blush.
- `MmdNames.cpp:279-360` — eye bones, `両目`, iris follow, winks.
- `FaceMorphs.cpp:535-558` — `Eye Roll`; `560-623` — `Heart Eyes`.
- `PmxReader`/`PmxWriter` — UV morph type 3.
- `docs/FACE_MORPHS.md` — current effect design and limitations.

---

## F. Tears, Sweat and Effects

### F.1 Tears

**Game semantics `[V]`**

- `status.tearsLv` is 0–3.
- `ChaFileDefine.tearsFlags` is a 4×3 diagonal:
  - 0: none
  - 1: `cf_O_namida_S`
  - 2: `cf_O_namida_M`
  - 3: `cf_O_namida_L`
- `ChaReference` maps `S_TEARS_01/02/03` to those three objects.
- `ChaControl` activates exactly one of them from `tearsFlags[tearsLv, 0..2]`.

**Assets `[V]`**

| Mesh | Vertices | Shapes | Material | Texture |
|---|---:|---:|---|---|
| `cf_O_namida_S` | 304 | 27 | `cf_m_namida_00` | `cf_namida_00_t` 512² BC7 |
| `cf_O_namida_M` | 364 | 27 | `cf_m_namida_00` | same |
| `cf_O_namida_L` | 494 | 27 | `cf_m_namida_00` | same |

The 27 shapes are eye-expression shapes (`naS00_def_cl/op`, `egao_cl/op`, `winkl_op`, `winkr_op`, `setunai_op`, `tere_op`, `ikari_op`, `sinken_op`, `tumara_op`, `kurusi_op`, `keno_op`, `sian_op`, `kanasi_op`, `naki_op`, `aseri_op`, `rakutan_op`, `komaru_op`, `doya_op`, `gyul_op`, `gyur_op`, `gyu_cl`, `gyul02_op`, `gyur02_op`, `gyu02_cl`). They are the tear mesh's own shapes, not copies of the eye-white shapes. `[V]`

The material shader is `Shader Forge/toon_glasses_lod0` in KK and `Koikano/main_clothes_item_glasses` in KKS; both are glasses/toon-style transparent materials. `[V]`

**Current behaviour `[V]`**

`Assembler.cpp:3904-3906` marks any Head renderer whose name contains `namida` as hidden and skips it. The tear meshes, material and texture never reach the PMX. `tearsLv` is never read by the exporter.

**Proposed PMX representation `[H]`**

1. Export all three tear meshes (or one mesh with three material slots), with their existing skinning and their 27 shapes.
2. Set exactly one material alpha to 1 according to the card's `tearsLv` at rest; the others 0. This preserves the card's appearance.
3. Add material morphs `Tears Off`, `Tears S`, `Tears M`, `Tears L`. Each morph adjusts all three material alphas to the target state (`Off` = 0/0/0; `S` = 1/0/0; etc.), relative to the rest state. Material morphs are additive, so a morph can subtract the rest value and add the target.
4. Merge the tear shapes into the existing eye expression morphs by core key (`naS00_*` → the same semantic eye morph). This keeps the tear mesh deforming with blinks/winks/expressions without adding 81 new morphs. If a tear shape has no matching expression, add a raw morph or skip it.
5. Set the tear materials double-sided, no edge, alpha blend, and draw after the face. The tear texture is BC7; CardBridge already decodes BC7. Verify the glasses/toon bake and transparency in MMD.
6. Add the tear morphs to the display-frame priority list, or leave them unlisted if the 255 budget is full. See H.5.

**Why not a single `Tears` morph?** A PMX morph is scalar 0–1; one morph cannot select three discrete levels. Three level morphs plus `Off` is the smallest correct set. `[H]`

**Why not geometry morphs?** Moving the tears inside the head would hide them, but it would also move them relative to the face and lose the game's exact placement. Material alpha is reversible, cheap and preserves the geometry. `[H]`

**Main unknowns:** the tear material's exact transparency/draw order in MMD; whether the tear mesh skinning is complete in the bundle; whether merging shapes by key covers all 27. `[U]`

### F.2 Blush

**Game semantics `[V]`**

`ChaControl.ChangeHohoAkaRate` sets `_overcolor2.a = Mathf.Lerp(0f, 0.2f, rate)` on the face material. CardBridge applies exactly `0.2 × clamp(hohoAkaRate)` to `_overcolor2` before baking the face texture (`Assembler.cpp:3986-3989`). The base face material's `_overcolor2` RGB is `(1, 0.686, 0.686)`, so the blush is a fixed pink at 0–0.2 alpha. `[V]`

The game config `EtcData.hohoAka` can disable blush at runtime; CardBridge applies the card's rate regardless. That is a game setting, not card data. `[V]`

**Current behaviour `[V]`**

The blush is baked into the face texture. The PMX has no `Blush` morph; the face material's diffuse alpha cannot be used because it would make the whole face transparent.

**Proposed PMX representation `[H]`**

1. Generate a cheek overlay patch from the face mesh's cheek region (or duplicate the cheek vertices), with the same skin weights, offset slightly along the normal.
2. Use a soft radial blush texture in the game's pink (`_overcolor2` RGB), or a procedural mask. The `idol_khohoaka` texture exists in the head bundle but is not referenced by the base `cf_m_face_00` material or the `cha_face` list row; it is not a verified blush source, so do not use it without evidence.
3. Add `face/fx_blush#0`, alpha 0 at rest, double-sided, no edge.
4. Add a `Blush` material morph that adds +1 alpha. Optionally `Blush Light`/`Blush Heavy`.
5. Set the rest alpha from the card's `hohoAkaRate` if the export should look like the card; the morph then adds the remainder. Or set rest 0 and use `Blush` to turn it on. The former is faithful; the latter is more controllable.
6. Panel 4 (Other) or 2 (Eye); `Other` is the natural place for a face effect. Add to the display frames only if the budget allows.

**Why an overlay?** PMX material morphs can offset the PMX material's diffuse alpha, but the blush is a shader-property overlay (`_overcolor2`), not the material's diffuse alpha. A separate overlay is the only way to make blush independently controllable without tinting the whole face. `[H]`

**Main unknowns:** the overlay's exact shape/softness; whether a cheek patch follows smiles without clipping; the best rest/morph convention. `[U]`

### F.3 Sweat

**Current implementation `[V]`**

- `Sweat` and `Sweat Heavy` are material morphs (panel 4).
- Two droplet sets: A = forehead + two cheeks; B = larger forehead + two lower cheeks.
- Droplets are 8×14 UV spheres, 0.42 z-squash, bound to the head bone.
- Textures `fx_sweat.png` (128²) and `fx_hearteyes.png` (512²) are generated beside the model.
- Overlay materials start at `diffuse.a = 0`; the morph adds +1 alpha.
- Total overlay vertices: 1,114 (heart 358 + sweat 756).

**Game source `[V]`**

The base game has no sweat geometry. Sweat is runtime/plugin (SkinEffects) and is not a card field. CardBridge's sweat is a generic effect, not a card-faithful export.

**Proposed PMX representation `[H]`**

Keep the current implementation as a generic extra. Optionally add `Sweat Light`, `Sweat Forehead`, `Sweat Cheeks` variants. Ensure the droplets draw after the face and test against translucent blush/face layers. The material morph alpha-add is standard PMX; visual verification in MMD is still pending.

### F.4 Gag / expression eyes

This is the most important new finding. The mapping is **not unknown**: it is a readable list row.

**Runtime mechanism `[V]`**

`ChaControl.ChangeEyesPtn` reads the `cha_eyeset` row selected by `eyesPtn`:

- `EyeObjNo`: 0 = no gag mesh; 1/2/3 = activate `cf_O_gag_eye_00/01/02`.
- `MainTexAB` + `MainTex`: the gag-eye texture, assigned to the active mesh's material.
- `TileAnim`: tile animation vector (`x/y/z`, e.g. `1/6/1`).
- `SizeSpeed`, `SizeWidth`, `AngleSpeed`, `Yurayura`: runtime animation parameters.
- `EpsTexAB` + `EpsTex`: an iris expression texture (`_expression`) with `_exppower = 1` on both eye renderers.

The gag materials `cf_m_gageye_00/01/02` have `_MainTex` null in the prefab and `_TileAnimation = (1,6,1,0)` on `_02`; the runtime assigns the texture. `[V]`

**The exact rows `[V]`**

Both installs have the same rows 21–32 (the probe read category 2 through CardBridge's `ListTable`; 39 rows in KK, 41 in KKS):

| eyesPtn | Name | EyeObjNo | Mesh | MainTex | TileAnim | Extra | PMX target |
|---:|---|---:|---|---|---|---|---|
| 21 | ぐるぐる目１ (Spiral 1) | 1 | `cf_O_gag_eye_00` | `cf_t_gageye_00` | 1/1/1 | — | static overlay / `Gag Spiral 1` |
| 22 | ぐるぐる目２ (Spiral 2) | 1 | `cf_O_gag_eye_00` | `cf_t_gageye_02` | 1/1/1 | — | static overlay / `Gag Spiral 2` |
| 23 | ぐるぐる目３ (Spiral 3) | 2 | `cf_O_gag_eye_01` | `cf_t_gageye_01` | 1/1/1 | `AngleSpeed=-0.5` | static overlay / `Gag Spiral 3` |
| 24 | 瞳に星 (Star pupil) | 0 | — | — | — | `EpsTex=cf_t_expression_01` | iris overlay `Star Eyes` |
| 25 | 瞳にハート (Heart pupil) | 0 | — | — | — | `EpsTex=cf_t_expression_00` | iris overlay `Heart Eyes` |
| 26 | 目がハート (Heart eyes) | 2 | `cf_O_gag_eye_01` | `cf_t_gageye_03` | 1/1/1 | `SizeSpeed=0.5`, `SizeWidth=0.03` | overlay / `Heart Eyes` |
| 27 | 目に炎 (Flame eyes) | 3 | `cf_O_gag_eye_02` | `cf_t_gageye_08` | 1/6/1 | — | tile strip; static frame or UV scroll |
| 28 | デフォルメウィンク (Deformed wink) | 3 | `cf_O_gag_eye_02` | `cf_t_gageye_07` | 1/1/1 | — | static overlay |
| 29 | 縦一文字 (Vertical line) | 1 | `cf_O_gag_eye_00` | `cf_t_gageye_05` | 1/1/1 | — | static overlay |
| 30 | ディフォルメつむり (Deformed closed) | 1 | `cf_O_gag_eye_00` | `cf_t_gageye_04` | 1/1/1 | — | static overlay |
| 31 | 横一文字 (Horizontal line) | 1 | `cf_O_gag_eye_00` | `cf_t_gageye_06` | 1/1/1 | — | static overlay |
| 32 | デフォルメ泣き (Deformed crying) | 3 | `cf_O_gag_eye_02` | `cf_t_gageye_09` | 1/6/1 | — | tile strip; static frame or UV scroll |

`EyeObjNo` 1/2/3 maps to `cf_O_gag_eye_00/01/02`. The textures `cf_t_gageye_00..09` and `cf_t_expression_00/01` live in `mt_eye_00.unity3d` in both installs; KKS adds `_low` variants. The materials live in `mm_base.unity3d`; KK shader `Shader Forge/toon_textureanimation`, KKS `Koikano/sub_texture_animation`. `[V]`

**Current behaviour `[V]`**

- CardBridge applies the `eyesPtn` FaceBlendShape pattern to the face/eye whites.
- It skips every `gag_eye` renderer (`Assembler.cpp:3905`).
- It never applies `EpsTex` to the iris material; the prefab's `_expression` is null.
- The test cards use `eyesPtn=0`, so this gap is not visible in the two real PMX files. A card with `eyesPtn=21..32` loses the stylized eye overlay.

**Proposed PMX representation `[H]`**

1. Read the `cha_eyeset` category 2 row for the card's `eyesPtn` via `ListTable` (base + mod rows).
2. If `EyeObjNo != 0`, export the selected `cf_O_gag_eye_0{ EyeObjNo-1 }` mesh (240 verts) with the row's `MainTex` and the matching `cf_m_gageye_*` material. Set it visible at rest if the card uses that eyeset; add a `Gag Eyes` material morph to toggle it.
3. If `TileAnim.y > 1` (e.g. `1/6/1`), either crop the texture to the first frame for a static rest, or add a `Gag Eyes Scroll` UV morph that scrolls the strip. PMX cannot reproduce the game's `SizeSpeed`/`AngleSpeed`/`Yurayura` animations without morph animation; document the static approximation.
4. If `EpsTex != 0`, export a clone of the iris caps sampling `cf_t_expression_00` (heart) or `_01` (star), alpha 0, and add `Heart Eyes`/`Star Eyes` material morphs. This is the card-faithful version of the current generated `Heart Eyes`.
5. Keep the generated `Heart Eyes`/`Eye Roll` as generic extras, or replace them with the card-faithful eyeset versions when the card uses rows 24–26.
6. Test cards with `eyesPtn=21..32`; the neutral cards cannot exercise this path.

**Main unknowns:** the gag mesh's skinning (eye bones vs head); the exact material bake for `toon_textureanimation`/`sub_texture_animation`; whether a static first frame is acceptable for tile strips; whether `EpsTex` should be per-eye or shared. `[U]`

### F.5 Effect status matrix

| Effect | Source | Current PMX | Card-faithful? | Missing work |
|---|---|---|---|---|
| Expressions | FaceBlendShape | 575/579 vertex morphs | Yes | — |
| MMD standard | Derived | 46 morphs + 6 aliases | Yes | — |
| Winks | Eye shapes | 4 one-eyed morphs | Yes | — |
| Gaze | Eye bones | `左目`/`右目`/`両目` | Yes | — |
| Eye Roll | Generic/plugin | 1 combined UV morph | Generic | Split L/R; visual check |
| Heart Eyes | Generic/plugin | 1 material morph + generated overlay | Generic | Use `cf_t_expression_00`/`cf_t_gageye_03` when the card uses eyeset 25/26 |
| Star Eyes | eyeset 24 | Not exported | No | `EpsTex` overlay |
| Tears | `tearsLv` | Not exported | No | 3 meshes + material morphs + shape merge |
| Blush | `hohoAkaRate` | Baked only | Static only | Cheek overlay + material morph |
| Sweat | Runtime/plugin | 2 material morphs + droplets | Generic | Optional variants; visual check |
| Gag eyes 21–23, 26–32 | `cha_eyeset` | Not exported | No | Mesh + texture + material morph |
| EpsTex 24/25 | `cha_eyeset` | Not exported | No | Iris overlay + material morph |

### F.6 Evidence

- `ChaFileDefine.cs:387` — `tearsFlags` diagonal.
- `ChaReference.cs:242-244` — `S_TEARS_01/02/03` → `cf_O_namida_S/M/L`.
- `ChaControl.cs:1267-1278` — tear activation.
- `ChaControl.cs:7821-7832` — `ChangeHohoAkaRate` → `_overcolor2.a = Lerp(0, 0.2, rate)`.
- `ChaControl.cs:7339-7398` — `EyeObjNo`, `MainTex`, `TileAnim`, `SizeSpeed`, `SizeWidth`, `AngleSpeed`, `Yurayura`, `EpsTex`, `ChangeGagEyesMaterial`.
- `ChaReference.cs:247-249` — `N_Gag00/01/02` → `cf_O_gag_eye_00/01/02`.
- `Assembler.cpp:3904-3906` — tear/gag skip.
- `Assembler.cpp:3986-3989` — blush bake.
- `bo_head_00.unity3d` — tear meshes/materials/texture; gag meshes.
- `mm_base.unity3d` — gag materials.
- `mt_eye_00.unity3d` — gag/expression textures.
- `eyeset_kk.txt`, `eyeset_kks.txt` — list rows 21–32.

---

## G. Reference Projects

No local source for the community exporters was found; this section is based on online documentation and repository browsing. Use the ideas, not the code.

| Project | What it does | What it proves for CardBridge | Licence / caveat |
|---|---|---|---|
| **KKBP** (community Koikatsu→Blender/PMX exporter) | Reads live Unity `SkinnedMeshRenderer` blendshapes from a running game | The full runtime shape set is available and is the same data CardBridge reads from bundles; a live-runtime dependency is not acceptable for CardBridge's offline requirement | No clear licence found; do not copy code. `[S]` |
| **KKPMX** | Post-processes PMX files | PMX morph/name cleanup is a separate concern; CardBridge should emit correct PMX directly | MIT. `[S]` |
| **ExpressionLink** | Runtime BepInEx plugin using `eye_motion.f00_eye_posx/negx/posy/negy/blink` | Per-eye motion can be expressed as blendshapes; not installed in either game; CardBridge's base heads have no such shapes | No clear licence; not installed. `[S]` |
| **IllusionMods/Ahegao** | Runtime plugin: eye texture offset, `_expression` EpsTex, expression pattern switches, tears/blush | Confirms the runtime techniques and their PMX equivalents (UV morph, overlay + material morph, pattern morphs) | GPL-3.0; do not copy code. `[V]` for the technique, `[S]` for the mapping |
| **mmd_tools** | Independent PMX reader/writer | Material morph factors are `float4` for texture/sphere/toon; CardBridge's encoding is compatible | Open source; used as a format cross-check. `[V]` |
| **PMX Editor / MMD 9.32/9.50** | Target tools | The real acceptance test: morph list, display frames, UV/material morphs, transparency | Freeware; visual test pending. `[S]` |

**Why CardBridge cannot simply reuse KKBP/ExpressionLink:** both depend on a running Unity game and installed plugins. CardBridge is a standalone offline Windows C++ app; it must read the card and the game's bundles directly, and write a self-contained PMX. The reference projects are useful for technique validation, not as runtime dependencies.

---

## H. PMX Export Strategy

### H.1 Principles

1. **Preserve the existing export.** Do not change topology, skinning, textures, materials, accessories, CLI or viewers for features that are not requested.
2. **Use PMX-native mechanisms.** Vertex morphs for shape changes; UV morphs for iris/pupil effects; material morphs for alpha/colour; group morphs for combinations; bones for gaze.
3. **Rest state = card state.** If the card has tears/blush/gag eyes, the PMX should show them at rest. Morphs then switch them.
4. **No runtime dependency.** No Koikatsu, BepInEx, Unity, Blender, Python or plugin at MMD time. Textures and geometry are embedded.
5. **No texture swaps.** PMX material morphs cannot swap textures. Use overlays with alpha, or UV scroll/atlas.
6. **Respect MMD's 255 listed-morph limit.** Total display-frame items must stay ≤255; test it.
7. **Data-driven.** Read `tearsLv`, `hohoAkaRate`, `eyesPtn` and the `cha_eyeset` row. Do not hardcode per-card names beyond the verified mappings.
8. **Viewer/editor parity.** Every effect should be previewable and adjustable, not just written to the file.
9. **Backward compatibility.** Decide whether new effects are always emitted (consistent with the current generic Eye Roll/Heart Eyes/Sweat) or only when the card uses them (preserves the neutral baseline). See M.

### H.2 Feature-to-PMX mapping

| Feature | Source | PMX representation | Rest | Morph(s) | Panel |
|---|---|---|---|---|---|
| Expressions | FaceBlendShape | Vertex (1) | Card pattern weights | Existing semantic names | 1/2/3 |
| Raw shapes | Blend shapes | Vertex (1) | 0 | Existing `Shape` names | 1/2/3 |
| MMD standard | Derived | Vertex/group | 0 | `あ`…`ｳｨﾝｸ２右` | 1/2/3 |
| Winks | Eye shapes | Vertex, split | 0 | `ウィンク` etc. | 2 |
| Gaze | Eye bones | Bones | Card ST | `左目`/`右目`/`両目` | — |
| Eye Roll | Generic | UV (3) | 0 | `Eye Roll L/R` + group | 2 |
| Pupil size/position | Card pupil fields (baked) | UV (3), optional | Baked | `Pupil Size/X/Y L/R` | 2 |
| Star/Heart pupil | eyeset 24/25 | Iris overlay + material (8) | Card eyesPtn | `Star Eyes`/`Heart Eyes` | 2 |
| Heart eyes | eyeset 26 | Gag mesh + material (8) | Card eyesPtn | `Heart Eyes` | 2 |
| Spiral/flame/line/cry | eyeset 21–23, 27–32 | Gag mesh + material (8) | Card eyesPtn | `Gag Eyes` | 2 |
| Tears | `tearsLv` 1–3 | 3 meshes + material (8) | Card tearsLv | `Tears Off/S/M/L` | 2/4 |
| Blush | `hohoAkaRate` | Cheek overlay + material (8) | Card rate | `Blush` | 4 |
| Sweat | Generic | Droplet overlay + material (8) | 0 | `Sweat`/`Sweat Heavy` | 4 |
| Outfit/nude | Card | Material/group (8/0) | Card | Existing | 4 |

### H.3 Tears: concrete design

- **Geometry:** export `cf_O_namida_S/M/L` (304/364/494 verts) with their skin weights. If all three are present for switching, 1,162 verts total. If only the card's level is exported, 304–494 verts, but then no switching.
- **Materials:** three PMX materials, one per tear mesh, sharing `cf_namida_00_t`. Set double-sided, no edge, alpha blend. Bake the glasses/toon shader to a simple transparent material if the full shader bake is not available.
- **Rest:** set the active level's material alpha to 1; others 0.
- **Morphs:** `Tears Off` (all 0), `Tears S` (1/0/0), `Tears M` (0/1/0), `Tears L` (0/0/1). Each is a material morph (type 8) adjusting all three alphas by `target - rest`.
- **Shapes:** merge the 27 tear shapes into the existing eye expression morphs by key. This keeps the tear mesh deforming with blinks/winks. If merging is not possible, export the tear shapes as raw morphs (adds 81 morphs) or leave the tears static (lower fidelity).
- **Display:** add the four tear morphs to `表情` or the Eye frame only if the budget allows; otherwise leave them unlisted (still usable in PMX Editor/engine).
- **Acceptance:** a card with `tearsLv=1/2/3` shows exactly the S/M/L mesh at rest; each morph switches cleanly; `Tears Off` hides all; no z-fighting; the tears follow blinks.

### H.4 Independent eyes: concrete design

- Split the existing `Eye Roll` UV morph by iris mesh:
  - `Eye Roll L`: 179 offsets on `cf_Ohitomi_L02`.
  - `Eye Roll R`: 179 offsets on `cf_Ohitomi_R02`.
  - `Eye Roll`: group morph (type 0) referencing both at 1.0, so existing motions that key `Eye Roll` still work.
- Add optional pupil morphs:
  - `Pupil Size L/R`: per-vertex UV offset `(uv - centre) * (scale - 1)`.
  - `Pupil X/Y L/R`: per-vertex UV offset `(du, dv)`.
  - The rest is the baked card ST; the morph is a delta.
- Keep the eye bones as the primary independent gaze control. Document that bone rotation is the natural MMD way; the UV morphs are for stylized pupil motion.
- For KKS, fix `hlUpX/hlDownX` before baking; no morph needed.
- For independent highlights, export `_overtex1/_overtex2` as separate overlay meshes with their own textures; this is optional and larger.

### H.5 Display-frame budget

Current: `表情` + `Eye` + `Brow` + `Mouth` = 255 exactly. The `Other` frame is absent.

Options:

| Option | Effect | Recommendation |
|---|---|---|
| A. Drop raw per-mesh shapes from display frames | Frees many slots; semantic expressions remain; raw shapes stay in the file | **Recommended** |
| B. Reduce `表情` priority list | Keeps raw shapes but hides some effects/standard morphs | Not recommended |
| C. Leave new effects unlisted | No UI change; users need PMX Editor/motion data | Fallback |
| D. Use group morphs | Combines effects but cannot select discrete tears levels | Limited |

Recommended policy:

- List semantic expressions, MMD-standard morphs, winks, aliases, and the new card-faithful effects.
- Do **not** list raw per-mesh shapes by default; they remain in the PMX and can be listed with an opt-in flag.
- Keep total listed ≤255; add a unit test that sums every frame's `morphs.size()`.
- If the user wants raw shapes listed, expose `--list-raw-shapes` and accept that some effects will be unlisted.

### H.6 Material and morph encoding

- Use type 3 UV morphs for iris/pupil/gag scroll.
- Use type 8 material morphs for alpha/colour.
- Use type 0 group morphs for combined effects and aliases.
- Keep the verified material-morph layout: diffuse `float4`, specular `float3`, specularPower `float`, ambient `float3`, edgeColor `float4`, edgeSize `float`, texture `float4`, sphere `float4`, toon `float4`.
- Use `add = true` for alpha additions; use `add = false` for absolute replacements (e.g. hide outfit).
- Material morphs can add negative values; use that for `Tears Off`/switching.

### H.7 What not to do

- **Do not swap textures via material morphs.** PMX cannot; use an overlay or a UV-scrolled atlas.
- **Do not animate shader properties** (`_expression`, `_exppower`, `_TileAnimation`, `_SizeSpeed`, `_AngleSpeed`, `_Yurayura`) directly. PMX has no shader-property animation; bake a static look or approximate with UV morphs.
- **Do not rely on a plugin** to enable tears/blush/gag eyes. The PMX must be self-contained.
- **Do not invent card fields or texture names.** Use `tearsLv`, `hohoAkaRate`, `eyesPtn`, and the `cha_eyeset` row.
- **Do not exceed 255 listed morphs.** MMD 9.32 can crash.

---

## I. Viewer Integration

### I.1 Current state

| Component | Current behaviour | Gap |
|---|---|---|
| `BuildLiveMorphs` | Expressions, raw shapes, MMD standard, winks, aliases | No Eye Roll/Heart Eyes/Sweat, no tears/blush/gag |
| `AccumulateLiveMorph` | Vertex offsets + iris follow | No UV/material channel |
| `Morphs.inc` | Applies `morphDelta` before skinning | Cannot preview UV/material morphs |
| `PmxToScene` | Type 1 → blend shapes, type 0 → expressions | Type 3/8 preserved but not editable |
| `Explorer.inc` | Counts material/other; lists only type 1 | Material/UV morphs not browsable |
| `Loaded::pmxDoc` | Full PMX kept | Available for a richer importer |

### I.2 Proposed changes

1. **Shared effect builder.** Refactor `AddFaceEffects` into a reusable effect set that produces scene-space overlay meshes, UV recipes and material recipes. The exporter converts it to PMX; the viewer renders it directly. This avoids two divergent implementations.
2. **Live morph channels.** Extend `LiveMorph` with:
   - `std::vector<std::pair<int, Vec2>> uv;` (mesh-local vertex → UV offset)
   - `std::vector<MaterialDelta> materials;` (material index → alpha/colour delta)
   - `std::vector<Overlay> overlays;` (mesh, material, alpha)
   - Or a separate `LiveEffect` catalog for non-vertex effects.
3. **Preview.** Apply UV offsets to the copied mesh's UVs; apply material alpha/colour overrides at draw time; render overlays with their alpha. The software and GPU views should both support this.
4. **PMX import.** `PmxToScene` should keep type 3/8 morphs in the scene (or a side table) and the Morphs tab should list them. The `pmxDoc` already holds them; the importer can expose them without changing the scene's vertex representation.
5. **Explorer.** List all morph types with type/panel/count; make material/UV morphs selectable and previewable. Update the "vertex morphs are the editable ones this phase" comment.
6. **UI.** Add an `Effects` section/tab: `Eye Roll L/R`, `Heart/Star Eyes`, `Tears Off/S/M/L`, `Blush`, `Sweat/Sweat Heavy`, `Gag Eyes`. Group by panel; add reset.
7. **Display-frame parity.** The viewer should show which morphs are in the PMX's display frames and warn when the 255 limit is exceeded.
8. **Tests.** Build a synthetic scene with all effect types; assert the live catalog matches the PMX morph list; assert UV/material preview changes pixels.

### I.3 Effort and risk

This is a medium-to-large viewer change because the renderer currently has only a vertex-delta path. The `pmxDoc` availability lowers the import risk; the shared effect builder lowers the duplication risk. The main risk is GPU/software preview parity and material override plumbing.

---

## J. Performance and Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Display-frame 255 overflow | High | High (MMD crash) | Rebalance frames; drop raw shapes from listings; test the sum |
| KKS create-material shader blobs | High | Medium (flat/wrong face/eye textures) | Visual MMD check; improve the KKS shader-blob reader; fallback textures |
| Tear material bake/transparency | Medium | High (invisible or opaque tears) | Bake the glasses/toon shader; test alpha/draw order; fallback simple transparent material |
| Gag tile animation approximation | Medium | Medium (static vs animated eyes) | Static first frame by default; optional UV-scroll morph; document |
| Blush overlay fit | Medium | Medium (cheeks clip/slide) | Derive from face cheek vertices with skin weights; test smiles |
| Eye Roll direction | Low | Medium (rolls down instead of up) | Visual test; flip the `-0.16` sign if needed |
| Material morph alpha in MMD | Low | Medium (effects don't show) | Test 9.32/9.50; use standard type 8 alpha-add |
| PMX size/morph count | Low | Low | 601+ morphs and ~7.5 MB are fine; new assets add little |
| Viewer performance | Medium | Medium | Only evaluate active effects; cache overlay meshes |
| Mod eyeset rows | Low | Medium | Use `ListTable` base+mod rows; test a mod eyeset |
| Licensing | Low | Low | Ideas only; no code copy from unclear licences |
| Visual verification gap | High | High | MMD/PMX Editor test matrix; the two neutral cards cannot prove the new features |

**Performance notes:**

- PMX size is dominated by existing geometry/textures; tears (+1,162 verts, one 512² texture), gag eyes (+240 verts, 256² textures), blush (a few hundred verts) and per-eye UV morphs (179 offsets each) are small additions.
- Morph count is not the MMD limit; the display-frame listing is. 601 morphs are already fine.
- The viewer's per-frame cost is dominated by active morph evaluation; only active effects should be evaluated. The current `AccumulateLiveMorph` is cheap for a few active vertex morphs; UV/material effects should be similarly lazy.
- Export time is not a concern for these additions; no new heavy texture bake is required beyond the tear/gag textures, which are small.

---

## K. Roadmap

Estimates are rough (solo developer, existing codebase); they are for sequencing, not commitments.

### M0 — Baseline and investigation (done)

- Real KK/KKS exports, morph/asset inventories, runtime semantics, PMX encoding check, this report.
- **Acceptance:** evidence labels, no production change, no game/MMD launch.

### M1 — Low-risk correctness and viewer preview (S/M, ~3–5 days)

**Scope:**

- Apply KKS `hlUpX/hlDownX` to the iris highlight ST.
- Split `Eye Roll` into `Eye Roll L/R` + group `Eye Roll`.
- Extend the viewer's live catalog/preview to show the existing `Eye Roll`, `Heart Eyes`, `Sweat`, `Sweat Heavy` (UV/material channels).
- Update the Explorer to list material/UV morphs (at least for native PMX).
- Add unit tests for the split and the viewer channels.

**Acceptance:**

- Existing PMX morphs/names/frames unchanged for neutral cards (except the Eye Roll split, which is a new export).
- Viewer can preview Eye Roll/Heart Eyes/Sweat on a card scene.
- KKS highlight X matches the card for non-0.5 values.

### M2 — Tears (M, ~5–8 days)

**Scope:**

- Read `tearsLv`; export `cf_O_namida_S/M/L` with skinning and shapes.
- Bake `cf_m_namida_00`/`cf_namida_00_t` to transparent PMX materials.
- Add `Tears Off/S/M/L` material morphs; rest state from the card.
- Merge tear shapes into the existing eye expression morphs.
- Add display-frame entries only if the budget allows.
- Tests: `tearsLv` 0–3, shape merge, alpha rest, round-trip, MMD visual.

**Acceptance:**

- A card with `tearsLv=1/2/3` shows exactly S/M/L at rest.
- Each tear morph switches cleanly; `Tears Off` hides all.
- Tears follow blinks/winks; no z-fighting.

### M3 — Gag / expression eyes (M, ~5–8 days)

**Scope:**

- Read `cha_eyeset` category 2 row by `eyesPtn` (base + mod).
- Export the selected `cf_O_gag_eye_*` mesh with `MainTex` and `cf_m_gageye_*`.
- Handle `TileAnim` (static first frame or UV scroll).
- Export `EpsTex` overlays for rows 24/25 (star/heart) with `cf_t_expression_00/01`.
- Add `Gag Eyes`, `Star Eyes`, `Heart Eyes` material morphs; rest state from the card.
- Tests: rows 21–32, mod row, tile crop, MMD visual.

**Acceptance:**

- A card with `eyesPtn=21..32` reproduces the stylized eyes at rest.
- The morphs toggle the overlays; no texture swap; tile strips are readable.
- Neutral cards are unchanged (unless effects are always emitted; see M).

### M4 — Blush (S/M, ~3–5 days)

**Scope:**

- Generate a cheek overlay from the face mesh with skin weights.
- Add `face/fx_blush#0`, alpha 0, and a `Blush` material morph.
- Rest alpha from `hohoAkaRate` (or 0 + morph; decide).
- Tests: rate 0/0.5/1, smile deformation, MMD visual.

**Acceptance:**

- Blush is independently controllable; no whole-face tint; follows smiles; no clipping.

### M5 — Viewer/editor integration and display-frame rebalance (M/L, ~5–10 days)

**Scope:**

- Shared effect builder used by export and viewer.
- UV/material preview in software and GPU views.
- PMX import type 3/8; Explorer lists all types; Morphs/Effects tab.
- Display-frame policy: drop raw shapes from listings by default; add opt-in flag; 255 test.
- Tests: catalog parity, preview pixels, frame sum.

**Acceptance:**

- Every exported effect is previewable in the viewer.
- Native PMX material/UV morphs are browsable.
- No export exceeds 255 listed morphs.

### M6 — Verification, docs, release (M, ~3–5 days)

**Scope:**

- MMD 9.32/9.50 visual matrix; PMX Editor; mmd_tools import.
- KK/KKS neutral + feature cards; mod eyeset.
- Docs: update `docs/FACE_MORPHS.md`, `docs/KKS_FORMAT_FEASIBILITY.md`, add this report/implementation notes.
- Regression: golden PMX counts/names; no unintended changes.

**Acceptance:**

- All test-plan items pass or are documented as limitations.
- Release notes state exactly what is card-faithful vs generic.

### Optional M7 — Per-eye pupil/highlight morphs and more effects (M)

- `Pupil Size/X/Y L/R` UV morphs.
- Separate `_overtex1/2` highlight overlays.
- More eyesets/effects (angry veins, blush variants, tear intensities).
- Only after M1–M6 are stable.

### Sequencing

`M1 → M2 → M3 → M4 → M5 → M6`. M2 and M3 are independent and can be swapped; M5 depends on the effect shapes from M2–M4. M1's viewer work can run in parallel with M2/M3 if the shared effect builder is designed first.

---

## L. Test Plan

### L.1 Unit tests

| Test | Input | Expected |
|---|---|---|
| `tearsFlags` mapping | `tearsLv` 0–3 | 0 none; 1 S; 2 M; 3 L |
| Tear export | `tearsLv=1..3` | Correct mesh/material; others alpha 0 |
| Tear morphs | `Tears Off/S/M/L` | Alpha targets exact; additive relative to rest |
| Tear shape merge | `naS00_*` keys | Offsets added to the matching eye morphs; no duplicate names |
| Blush rate | `hohoAkaRate` 0/0.5/1 | Overlay alpha 0/0.5/1 (or rest convention); no face tint |
| Blush morph | `Blush` 0/1 | Alpha 0→1; reset returns to rest |
| Eye Roll split | `Eye Roll L/R` | Only the correct iris vertices; group `Eye Roll` = both |
| Pupil UV math | `Pupil Size L/R` | Offset = `(uv - centre) * (scale - 1)`; no NaN |
| KKS highlight X | `hlUpX/hlDownX` 0/0.5/1 | ST X matches the game's `lerp` |
| Eyeset lookup | `eyesPtn` 21–32 | Correct `EyeObjNo`, `MainTex`, `TileAnim`, `EpsTex` |
| Eyeset mod row | Mod category 2 row | Mod row wins/merges per resolver policy |
| Gag tile crop | `TileAnim=1/6/1` | UVs cover one frame; no strip smear |
| Gag/EpsTex morphs | `Gag Eyes`/`Star Eyes`/`Heart Eyes` | Correct material alpha; rest state |
| PMX round-trip | Type 3/8 morphs | Reader/writer preserve offsets and factors |
| Display frames | All frames | Sum ≤255; no duplicate morphs |
| Viewer catalog | Synthetic scene | Live catalog matches PMX morph list |
| Viewer preview | UV/material morph | Pixels change; reset restores |

### L.2 Integration / golden

| Scenario | Card | Fields | Expected |
|---|---|---|---|
| Neutral KK | `Big Sisterly.png` | `eyesPtn=0`, `tearsLv=0`, `hohoAkaRate=0` | Baseline 601 morphs; no tears/blush/gag |
| Neutral KKS | `KKS BR-Chan.png` | same | Baseline 601 morphs; KKS-specific eye morphs |
| KK tears S/M/L | feature card | `tearsLv=1/2/3` | One tear mesh visible; morphs switch |
| KK blush | feature card | `hohoAkaRate=1` | Blush overlay visible; morph controls |
| KK spiral | feature card | `eyesPtn=21/22/23` | Correct gag mesh/texture |
| KK star/heart | feature card | `eyesPtn=24/25/26` | `EpsTex`/gag heart overlay |
| KK flame/wink/lines/cry | feature card | `eyesPtn=27..32` | Correct overlay/tile |
| KKS features | feature card | same | Same visuals; KKS shaders |
| Mod eyeset | mod card | mod row | Mod texture/mesh |
| Missing face/iris | malformed card | — | Graceful log; rest of export continues |

The two neutral cards cannot prove the new features; feature cards are required. If none are available, create test cards offline (card JSON) outside the production tree, or use cards from the user's collection. Do not modify game files.

### L.3 Visual MMD / PMX Editor / mmd_tools

1. Load the PMX in MMD 9.32 and 9.50.
2. Confirm the morph panel count is ≤255; no crash after a few seconds.
3. Play every expression, standard morph, wink, effect; confirm reset.
4. Tears: each level; `Tears Off`; blinks; draw order; no z-fighting.
5. Blush: rate 0/1; smiles; no clipping.
6. Gag eyes: rows 21–32; tile strips; `EpsTex` star/heart; no texture smear.
7. Eye Roll: direction; L/R independence; group; pupil size/position.
8. Sweat: light/heavy; z-order.
9. PMX Editor: morph list, types, panels, display frames, material morph factors, UV morphs.
10. mmd_tools: import; check material morph values, UV morphs, no errors.
11. Save/reload; confirm no corruption.

### L.4 Performance

- Export time and peak memory for neutral and feature cards.
- PMX size and texture count.
- Viewer FPS with all effects active; morph slider latency.
- MMD load time; morph panel responsiveness.

### L.5 Regression

- Neutral KK/KKS PMX morph counts, names, panels, frames, materials, textures unchanged (if new effects are conditional).
- If new effects are always emitted, update the golden baseline and document the change.
- Existing CLI flags and both viewers still work.
- No change to body/hair/clothes/accessories.

### L.6 Acceptance criteria

- A card with tears/blush/gag eyes exports those features faithfully at rest.
- Every effect is independently controllable; reset returns to the card's rest state.
- PMX loads in MMD 9.32/9.50 without the 255 crash.
- No texture swaps or runtime plugins.
- The viewer previews every effect.
- Tests cover the new mappings and the 255 limit.

---

## M. Unresolved Questions

1. **Always emit or only when used?** The current Eye Roll/Heart Eyes/Sweat are always emitted. Should tears/blush/gag eyes also always be emitted (so any card can use them) or only when the card's fields are non-zero (preserves the neutral baseline)? This affects golden tests and PMX size. **Recommendation:** always emit the generic effects; emit card-faithful tears/blush/gag only when the card uses them, with a flag to force all. Decide before M2.
2. **Display-frame policy.** Drop raw shapes from listings by default? Add `--list-raw-shapes`? Which effects get priority? The 255 limit is hard. **Recommendation:** list semantic expressions + standard + effects; leave raw shapes unlisted by default.
3. **Tear shape merge.** Can all 27 `naS00_*` shapes be mapped to existing eye morphs without conflicts? If not, add raw tear morphs (81) or leave tears static. Needs a probe/test.
4. **Tear material bake.** Can the glasses/toon shader be baked to a transparent PMX material with correct alpha/draw order? If not, use a simple transparent material and accept a look difference.
5. **Blush rest convention.** Rest = card rate (faithful) vs rest 0 + `Blush` morph (controllable). Which is better for MMD users? **Recommendation:** rest = card rate, morph adds the remainder; provide `Blush Off`.
6. **Blush overlay fit.** A cheek patch may slide/clip during smiles. Derive from face vertices with skin weights; test. Is a full-face overlay with a blush mask better?
7. **Gag tile animation.** Static first frame vs UV scroll vs multiple materials. The game's `SizeSpeed`/`AngleSpeed`/`Yurayura` cannot be reproduced exactly. Which approximation is acceptable? **Recommendation:** static first frame at rest; optional UV scroll morph.
8. **Gag mesh skinning.** Are the 240-vert gag meshes skinned to the eye bones or the head? If not, bind manually. Needs a probe.
9. **EpsTex mapping.** Is `cf_t_expression_00` heart and `_01` star? The list rows 24/25 imply that; verify visually. Should the overlay be per-eye or shared? The game applies it to both eye renderers; shared is faithful.
10. **KKS create-material shader blobs.** Will the KKS face/eye textures be correct in MMD? The real export produced create-result textures, but visual confirmation is pending. If not, fix `readShaderBlob` for nested arrays and the 2019 header/DXBC binding tail, or provide a fallback.
11. **KKS `hlUpX/hlDownX`.** Implement the X offset; are there other KKS-only eye fields? Check `ChaFileFace` 0.0.3 vs 0.0.2.
12. **MMD version behaviour.** Does MMD 9.32/9.50 apply type 3 UV morphs on the iris and type 8 alpha-add material morphs identically? Test both.
13. **Mod eyeset rows.** How does the resolver choose a mod category 2 row vs the base row? The card's mod GUID must select the right row. Test with a mod card.
14. **Viewer architecture.** Shared effect builder vs separate export/viewer implementations. The shared builder is cleaner but a larger refactor; the separate path is faster but risks divergence. **Recommendation:** shared builder.
15. **Independent eye UX.** Are per-eye UV morphs useful, or is bone rotation enough? The PMX already has `左目`/`右目`; per-eye morphs are for stylized pupil motion. Decide whether to ship them in M1 or M7.
16. **Test cards.** The two real cards are neutral; feature cards are needed. Can the user provide cards with `eyesPtn=21..32`, `tearsLv=1..3`, `hohoAkaRate=1`, or should we create offline test cards? Do not modify game files.
17. **Licensing.** KKBP/ExpressionLink have no clear licence; Ahegao is GPL-3.0; KKPMX is MIT. Use ideas only; do not copy code. If any code is reused, re-check the licence.
18. **PMX material morph alpha in MMD.** The current Heart Eyes/Sweat effects rely on alpha-add; they are not visually verified. Test before building more effects on the same mechanism.

---

## Appendix 1 — Evidence index

### Real exports

- `C:\Users\pepsi\AppData\Local\Temp\opencode\kk_face_export\`
  - `Teranishi Sumire.pmx` (7,549,276 B)
  - `morphs.txt` (601 morphs; effects 597–600; frames)
  - `blendshapes.txt` (real head shapes)
  - `renderers.txt` (head renderer/material inventory)
  - `card_fields.txt` (real face/status values)
  - `mt_eye.txt` (gag/expression/iris textures)
  - `textures\baked_cf_m_hitomi_00_123.png`, `_124.png`
- `C:\Users\pepsi\AppData\Local\Temp\opencode\kks_face_export\`
  - `Kitagawa Nozomu.pmx` (7,383,336 B)
  - `morphs.txt`, `blendshapes.txt`, `renderers.txt`, `card_fields.txt`, `mt_eye.txt`
  - `textures\cf_m_face_create_result_*`, `cf_m_eye_create_result_*`, `cf_m_eyewhite_create_result_*`

### Runtime/source evidence

- `C:\Users\pepsi\AppData\Local\Temp\opencode\kks-research\ilspy_all\ChaFileDefine.cs:387` — `tearsFlags`.
- `...\ChaReference.cs:242-244` — tears mapping; `:247-249` — gag objects.
- `...\ChaControl.cs:1267-1278` — tear activation; `:7339-7398` — eyeset/gag/EpsTex; `:7821-7832` — blush.
- `...\EyeLookMaterialControll.cs` — per-eye iris/highlight ST.
- `...\EyeLookCalc.cs` — gaze tracking.
- `...\ChaListDefine.cs` — category 2 `cha_eyeset`, keys.

### List probe

- `probe_eyeset.exe` (temp) — dumps `ListTable` category 2.
- `eyeset_kk.txt`, `eyeset_kks.txt` — rows 21–32 and fields.

### Code references

- `src/assemble/Assembler.cpp:3904-3906` — tear/gag skip.
- `src/assemble/Assembler.cpp:3942-3989` — eye ST, highlight colours, rotation, blush.
- `src/export/PmxPhysics.cpp:717-757` — expression/raw morphs.
- `src/export/MmdNames.cpp:279-378` — eye bones, winks, standard morphs.
- `src/export/FaceMorphs.cpp:520-764` — effects, display frames.
- `src/export/MorphCatalog.cpp` — live morph catalog.
- `src/pmx/PmxWriter.cpp:336-380` — morph serialization.
- `src/pmx/PmxReader.cpp:285-310` — morph reading.
- `src/pmx/PmxToScene.cpp:153-200` — PMX import limits.
- `viewer/Explorer.inc:61-78` — morph listing.
- `viewer/Morphs.inc` — live morph UI/preview.
- `docs/FACE_MORPHS.md` — current effect design.
- `docs/KKS_FORMAT_FEASIBILITY.md:158-168` — KKS shader-blob limitation.

### Format cross-check

- `mmd_tools` `MaterialMorphOffset.load()` reads `float4` for texture, sphere and toon; CardBridge's `PmxWriter`/`PmxReader` match.
- PMX morph types: 0 group, 1 vertex, 2 bone, 3 UV, 4–7 extra UV, 8 material, 9 flip, 10 impulse.

---

## Appendix 2 — Glossary

- **FaceBlendShape**: the game's head MonoBehaviour holding eyebrow/eye/mouth controls and pattern targets.
- **Raw shape**: one per-mesh Unity blend shape, e.g. `cf_O_face.eye_face.f00_def_cl`.
- **Expression pattern**: a game `PtnSet` state combining close/open shapes, e.g. `Eyes 0 closed`.
- **`tearsLv`**: 0–3 status field selecting none/S/M/L tears.
- **`hohoAkaRate`**: 0–1 status field for cheek blush; `_overcolor2.a = 0.2 × rate`.
- **`eyesPtn`**: index into the `cha_eyeset` list; selects eye shape plus optional gag mesh/texture/EpsTex.
- **`EyeObjNo`**: 0 = no gag mesh; 1/2/3 = `cf_O_gag_eye_00/01/02`.
- **`EpsTex`**: iris expression texture (`_expression`), e.g. heart/star pupil.
- **`TileAnim`**: texture tile animation vector, e.g. `1/6/1` = six frames horizontally.
- **PMX type 3**: UV morph; **type 8**: material morph; **type 0**: group morph.
- **Display frame**: MMD's morph panel group; all frames together may list ≤255 morphs in MMD 9.32.

---

*End of report.*
