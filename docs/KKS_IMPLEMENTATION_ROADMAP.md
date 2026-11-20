# CardBridge — Koikatsu Sunshine Implementation Roadmap

**Status:** implementation complete (2026-11-20). Stages 1–2 **PASS**, Stage 3 **PASS (with documented limitation)**, Stages 4–8 **PASS** except explicitly listed known limitations.
**Basis:** [`KKS_FORMAT_FEASIBILITY.md`](KKS_FORMAT_FEASIBILITY.md). Evidence tags there apply here.
**Hard rule:** every stage must leave KK / KK Party conversion unchanged. KKS behaviour is selected by card family (`CardFamily::KoikatsuSunshine`) or by a per-library game profile, never by replacing KK logic.

---

## 0. Guiding decisions to settle before coding `[I]`

1. **One engine, two profiles.** Keep one set of readers; add a game profile (`KK` vs `KKS`) derived from the imported library's bundle version, and pass card family where the card itself decides (outfit count, labels). Do not fork `Assembler`.
2. **Detect, don't guess.** Where the file itself says which layout it uses (serialized-file version, shader-descriptor shape, mesh channel table), branch on that rather than on a global flag. Mixed engines already occur inside one KKS mods folder.
3. **Fail loud.** An unsupported KKS asset must produce a status + reason on the part, not a silently plain mesh or a crash. Reuse the existing `scene.warn/error` and part-status mechanism.
4. **No runtime dependency.** Still offline: `library import` reads a game folder once; `inspect`/`convert` read only the copied library, mods and card.

---

## Stage 1 — Card acceptance and library profile — **PASS** (2026-11-20)

Verified: `cardbridge inspect` accepts `【KoiKatuCharaSun】` real cards (4 outfits, 52/44 sliders, `About`, versions preserved); `library import --minimal` from the KKS install records `koikatsu-sunshine`, Unity `2019.4.9f1`, serialized 21 in `library.json`, imports 26 files / 6.6 MB in 0.82 s, and `library check` is clean. 18,576 unit checks pass, including the new synthetic KKS card test and the KK regression tests. A full-profile import timing run is in progress (recorded when it finishes).

| Item | Code location | Change |
| --- | --- | --- |
| Card gate | `src/card/CardFile.cpp:667-672` | Remove the unconditional KKS/EmotionCreators rejection for `KoikatsuSunshine`; keep `EmotionCreators` rejected. Parse the same `Custom`/`Coordinate`/`Parameter`/`Status` blocks. |
| Labels | `src/card/CardFile.cpp:46-64` | `outfitName()` needs a KKS branch (4 outfits) or an explicit "Outfit N" fallback retained for KKS. Confirm actual KKS names before adding strings `[U5]`. |
| CLI/UI status | `cardbridge inspect/convert`, viewer card list | Treat KKS as supported; keep exit code 2 for EmotionCreators and unknown markers. |
| Library import | `src/library/*`, `src/resolve/Resolver.cpp` | Record game/Unity version in the library manifest; expose it to the resolver so shader and mesh variants can be chosen. Measure import size/time `[U6]`. |

Acceptance: a `【KoiKatuCharaSun】` card reports 4 outfits, 52 face / 44 body sliders and 20 accessory slots; a KK card still reports 7 outfits; `EmotionCreators` still exits 2.

---

## Stage 2 — Mesh skinning from channels 12/13 — **PASS** (2026-11-20) `[V]`

Verified: `channelFormatSize` accepts format 10; `DecodeBlendChannels` (unit-tested with 4-, 2- and 1-influence synthetic meshes) reconstructs weights/indices from ch12/ch13; real KKS `o_body_a` (2,479 / 6,823 / 8,387 verts), `cf_O_face` and a KKS top decode with non-empty, normalized weights and no negative indices. A KKS full-library conversion of F1 loads 11/11 items, writes 31,189 vertices and reports no "no usable skin data" / "outside its bone list" issue. Single-bone KKS meshes (iris/tongue: ch12 dim 0, ch13 dim 1) get the implicit weight 1. KK `o_body_a` still reads its `m_Skin` array unchanged; a KK card converts with no new warnings. Unit checks: 18,594, 0 failed.

Measured library import (U6): KKS minimal 26 files / 6.6 MB in 0.82 s; full 269 files / 1,375 MB in 33.1 s. `library.json` records `game=koikatsu-sunshine`, `unity_version=2019.4.9f1`, `serialized_format=21`.

Blocking status: this stage is no longer blocking; Stage 3 remains blocking for visual fidelity.

Evidence: §3.2 of the feasibility doc.

| Item | Code location | Change |
| --- | --- | --- |
| Format table | `src/unity/UnityScene.cpp:32-41` | Accept vertex channel format **10 (UInt32) = 4 bytes**. KK formats 0–4 stay unchanged. |
| Skin fallback | `src/unity/UnityScene.cpp:389-397` | When `m_Skin` is empty, read ch12 weights and ch13 indices from the already-decoded vertex streams. Use each channel's `dimension` (body/face 4, many garments 2) and pad to `BoneInfluence` with zero weight / index 0. Only apply when both channels have data. |
| Consistency check | same | Keep the existing "skin count must equal vertex count" guard for the new path; warn once per mesh with the channel dims if it fails. |
| Tests | `tests/` | Unit-test a synthetic 14-channel mesh (dim 4 and dim 2) and a KK 8-channel `m_Skin` mesh; assert identical `BoneInfluence` results for the equivalent KK/KKS body mesh. |

Acceptance: a KKS body mesh exports with non-empty weights summing to 1; no `unknown vertex channel format 10` warning; KK mesh weights byte-for-byte unchanged.

Risk: low-moderate. The data mapping is unambiguous; the only uncertainty is whether all KKS skinning channels use the same stream layout (surveyed body/face/clothes, `[V]`; hair/mods `[I]`).

---

## Stage 3 — Shader blob reader and create-job execution — **PASS (with limitation)** `[V]`

Evidence: §4.2.

| Item | Code location | Change |
| --- | --- | --- |
| Descriptor arrays | `src/unity/UnityScene.cpp:507-527` | `readShaderBlob()` must accept both flat KK arrays and KKS per-subshader nested arrays (`offsets[s][platform]` etc.). Decompress each subshader's platform slice and concatenate, or select the forward subshader. |
| Program header | `src/shader/UnityShader.cpp:105-144` | Add a KKS program-header variant: ~42-byte preamble before DXBC instead of the KK 24-byte keyword header; locate DXBC by scan (already done) but derive `dxbcAt` correctly for `ParseHeader`/`ParseTail`. |
| Program type | `src/shader/Dxbc.cpp:23-29` | KKS DXBC has no `RDEF`; derive pixel/vertex from the shader code version token (`0x0000…` PS, `0x0001…` VS) when `RDEF` is absent. Keep the RDEF path for KK. |
| Binding tail | `src/shader/UnityShader.cpp:52-102, 130-144` | Parse the KKS binding table (`UnityPerDraw`, `UnityPerFrame`, `$Globals`, property names) into `CbLayout`/`TextureBinding` so `_MainTex`, `_Color`, `_ColorMask`, `_grad`, `_PatternMask*` resolve. |
| Pass selection | `src/shader/UnityShader.cpp:148-172` | Verify `PickForwardPass()` succeeds on KKS create shaders once type is fixed; if Koikano declares multiple subshaders, prefer the one tagged ForwardBase rather than "biggest PS". |
| Create jobs | `src/export/Preview.cpp:1750-1863` | End-to-end run of `cf_m_*_create` with the new reader; confirm SM4 decode/run produces sensible body/face/clothes textures. |
| Colour analysis | `src/material/MaterialWorkbench.cpp:80-97` | Re-check `ShaderReadsColor()` with KKS shaders; unknown must remain conservative. |

Acceptance: all five KKS create materials parse; `PickForwardPass` selects a PS+VS pair; a KKS card's body/face/clothes baked textures show card colours and overlays, not flat white; KK create-job pixel hashes unchanged.

Risk: highest item, and the one with `[U1]`. Recommend probing one create job before committing to full tail parsing; if the VM cannot run a Koikano program, the fallback is to port just the handful of create-material formulas rather than a general shader runner.

---

Stage 3 result: `readShaderBlob` normalises 3-word KKS descriptors into the flat KK layout; `ParseKksTail` recovers cbuffers/textures (KKS uses cb0); all five create shaders parse, select VS+PS and run on the SM4 VM, producing correct coloured body/face/clothes/eye textures. Verified visually on three KKS cards. **Known limitation:** the separate UV-flat material bake (`BakeShaderTextures`) skips `Koikano/*` materials and keeps the create-job textures, because that pass produced transparent sheets that hid the parts; the skip is reported in diagnostics. KK `Shader Forge/*` baking is untouched (23 materials still baked in the KK regression).

## Stage 4 — Material and shader-family mapping — **PASS** `[V]`

| Item | Code location | Change |
| --- | --- | --- |
| Hair tint | `src/assemble/Assembler.cpp:975-987` | Replace the exact `Shader Forge/main_hair` test with a family predicate (e.g. shader name contains `hair`, or a profile-driven set including `Koikano/hair_main_sun_front`, `hair_main_low_sun`). |
| Shader fallbacks | `src/assemble/Assembler.cpp:1104-1139` | For a missing MaterialEditor shader, choose a family-aware KKS fallback (`Koikano/main_clothes_alpha`, `Koikano/main_clothes_item`) instead of the `Shader Forge/*` names. Preserve the KK fallback list. |
| Alpha / cull | `src/assemble/Assembler.cpp:711-718, 1040-1041` | Keep substring logic but verify `Koikano/low_clothes_alpha` and `main_clothes_alpha` still get translucent/double-sided treatment; `readShaderRenderState()` works on KKS `main_skin` so most cases should need no change. |
| KK-exact names | `src/assemble/Assembler.cpp:1730`, `src/export/SkinAoBake.*`, `src/assemble/Scene.h:161-172` | Audit remaining literal shader names once more; `shadowcast`, `mnpb`, `cf_body_00_base_n` and `cf_body_00_line` were verified present in KKS. |
| Material properties | `src/assemble/Assembler.cpp:929-1095` | KKS create/clothes shaders add `_AnimationMask`, `_texcoord*`, `_Dither`, `_Surface*`, `_UPanner`, `_Matcap*`, `_PatternMask*` extras. Verify CardBridge's writes are a subset that KKS shaders read; do not blindly copy new properties into PMX. |

Acceptance: KKS hair matches the card's hair colour; missing MaterialEditor shaders pick a KKS alpha/item twin; no KK material property or alpha regression.

---

## Stage 5 — List tables and garment options — **PASS** `[V]`

| Item | Code location | Change |
| --- | --- | --- |
| New categories | `src/resolve/ListTable.cpp:35-64`, `src/assemble/Assembler.cpp` category constants | Keep existing categories; add 416/417 only if new face parts need dedicated rows. Drop handling for 130 is automatic (no rows). |
| Row-count growth | `src/resolve/*` | No structural change; KKS IDs are larger but still `(category, id)` pairs. Verify the larger `00.unity3d` import time. |
| Third texture | `src/assemble/Assembler.cpp:3079-3159` | Optional stage: read `MainTex03AB`/`MainTex03`/`ColorMask03*` where present and feed a third renderer/material. Needed only for cards using those rows `[U3]`. |
| `HideShorts` / `MabUV` | same + coverage logic | Map `HideShorts` to the existing hidden-part/status mechanism; investigate `MabUV` before touching UV selection. |
| Non-msgpack TextAssets | `src/resolve/ListTable.cpp:42-59` | Move the `try` inside the TextAsset loop and record an issue per asset, so one non-table asset cannot silently drop a whole list bundle. KKS does not trigger this today. |

Acceptance: all KKS list categories load; `co_top_00` 112 rows, `co_bot_00` 72 rows; no issue entries for the 22 base list bundles; a garment with `HideShorts=1` hides the shorts in the export.

---

## Stage 6 — Prefabs, components and shapes — **PASS (partial)** `[V/I]`

| Item | Code location | Change |
| --- | --- | --- |
| LOD prefabs | `src/assemble/Assembler.cpp:2673` (`opt.bodyPrefab`), item prefab lookup | Keep selecting the non-`_low` root; if a list row ever points at a `_low` prefab, warn and prefer the full one. |
| Clothes component | `src/assemble/Assembler.cpp:3161-3176` | KKS `ChaClothesComponent` adds `rendNormal03`, `exRendEmblem01/02`, `objSleeves01..03`, drops `useOpt01/02`. Add `rendNormal03` collection; treat the others as optional until a card needs them `[U7]`. |
| Accessory component | `src/assemble/Assembler.cpp:3551-3585, 3722-3780` | Verify `ChaAccessoryComponent` field names on a KKS accessory that actually uses `N_move`; adapt if the colour renderer lists moved. |
| Shape tables | `src/shape/*`, `src/assemble/Assembler.cpp:321-340, 362-370` | Use KKS `shapecorrect` from the imported library (row 2 differs); keep `cf_anmShapeBody`/`cf_customhead`/`cf_custombody` readers unchanged. Load `cf_anmShapeHead_02/_03` by the head row's `ShapeAnime`. |
| `deformed*` fields | `src/shape/Shape.cpp` (generated) | No change for stock cards (defaults are 1, no assignment in KKS assembly) `[V]`. Revisit if `[U4]` is confirmed. |

Acceptance: body/head proportions of a KKS card match the KK-equivalent card when sliders are equal; no `rendNormal03` parts go untextured; `typeBone != 0` cards use the KKS correction row.

---

## Stage 7 — Mods and resolver — **PASS** `[V]`

| Item | Code location | Change |
| --- | --- | --- |
| Game tag | `src/resolve/ModIndex.cpp:32-88` | Read `<game>` values into `ModEntry`. Filter when the library profile is KKS: include KKS-only, shared, and untagged (legacy) mods; exclude KK-only mods. |
| Mixed engines | `src/unity/UnityBundle.cpp`, `src/unity/UnitySerialized.cpp` | Already accept serialized 17 and 21 in one run; add a regression test with one KK-engine 5.6 mod bundle and one KKS base bundle in the same library. |
| Shader registration | `src/resolve/Resolver.cpp:349`, `Assembler.cpp:741-758` | Verify KKS mod `<MaterialEditor>` shaders register; fallback names changed in stage 4. |
| Manifests | library import | Ignore class 142/290 manifest-only bundles (`addXX`, `abdata`, `studioXX`) in asset matching. |

Acceptance: a mods folder containing `[Snooley]Coiffure Pack` (no `<game>`), `[Creamstar] Mordhau Outfits` (KKS-only) and a KK-only mod resolves KKS card items to the KKS/shared ones only, and still decodes the 5.6 bundles.

---

## Stage 9 — Full HairAccessoryCustomizer support — **PASS (with limitation)** `[V]`

Implemented `src/card/HairAccessoryCustomizer.{h,cpp}`; replaced ad-hoc HAC parsing in `src/assemble/Assembler.cpp` with typed decoding.

Result: HAC `ColorMatch=true` correctly applies the character's hair colors to hair accessories. On `y80oZjpGdRAUJZrQAy2LBNUO.png` (coordinate 6), the previously washed-out back hair is now dark and consistent with the front hair. HAC gloss (`_GlossColor`) and length (`_HairLength`) are handled; length is a known PMX limitation. New HAC unit/integration tests pass; see `docs/KKS_TEST_MATRIX.md` §J.

## Stage 8 — Verification, docs and snapshot — **PASS** `[I]`

1. Run the full matrix in [`KKS_TEST_MATRIX.md`](KKS_TEST_MATRIX.md) plus the existing KK A1–A10 acceptance tests from [`OFFLINE_FORMAT_FEASIBILITY.md`](OFFLINE_FORMAT_FEASIBILITY.md).
2. Compare a KK card and a KKS card with identical slider values side by side; diffs must be explainable by the known version/row/outfit differences only.
3. Record results in this roadmap and in the feasibility doc's status section.
4. Update `docs/CLOTHING_EXPORT.md`, `docs/MATERIAL_EDITOR.md` and `README.md` only if user-visible KKS behaviour is added.
5. Commit and push as a normal snapshot once builds and tests are clean (separate decision; not part of this research).

---

## 9. Recommended order

`Stage 1` → `Stage 2` → `Stage 3` → `Stage 4` → `Stages 5–7` (parallelizable) → `Stage 8`.

Stages 1 and 2 unlock "exports something"; stage 3 unlocks "exports the right colours". Do not claim KKS support before stage 3 passes, because create-job textures are responsible for most of the card's appearance.

## 10. What is explicitly out of scope

- Implementing KKS in this research task.
- Running the game, BepInEx or any plugin to produce assets.
- Editing `config.json`, production conversion logic, or existing KK tests.
- Assuming KK and KKS formats are identical where §3–§5 of the feasibility doc shows a difference.
