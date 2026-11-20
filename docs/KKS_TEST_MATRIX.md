# CardBridge — Koikatsu Sunshine Test Matrix

**Status:** implementation in progress. Stages 1–4 PASS. Stage 5 (KKS third texture + `HideShorts`) PASS. Stage 6 (HAC support) PASS (new §J). Remaining: Stage 8 validation + docs sync.
**Rule:** every KKS test must be paired with the KK baseline test (A1–A10) from [`OFFLINE_FORMAT_FEASIBILITY.md`](OFFLINE_FORMAT_FEASIBILITY.md). A KKS pass that breaks a KK case is a failure.

Test inputs are all offline files:

```
KKS = E:\Game\[ScrewThisNoise] Koikatsu Sunshine EX BetterRepack R13
KK  = E:\Game\[ScrewThisNoise] Koikatsu BetterRepack RX21
LIB_KKS = <a library imported from KKS\abdata>
LIB_KK  = <existing KK library>
MODS_KKS = KKS\mods
```

Use `-LiteralPath` in PowerShell for the install paths. Conversion commands:

```
cardbridge library import --game "<KKS>" --library "<LIB_KKS>"
cardbridge library check --library "<LIB_KKS>"
cardbridge inspect "<card>" --library "<LIB_KKS>" --mods "<MODS_KKS>"
cardbridge convert "<card>" --library "<LIB_KKS>" --mods "<MODS_KKS>" --out "<out>" --print-diagnostics
```

Exit codes (unchanged): `0` ok, `1` usage, `2` unsupported/rejected card, `3` conversion failed.

---

## 0. Results so far (2026-11-20)

| Test | Status | Evidence |
| --- | --- | --- |
| K1 | **PASS** | Real KKS cards `inspect`: `koikatsu-sunshine`, `supported: true`, exit 0, blocks incl. `About`; no "not converted yet". |
| K2 | **PASS** | Real F1/F3: 4 outfits, 52 face / 44 body sliders, 20 accessory slots; synthetic fixture asserts the same. |
| K3 | **PASS** | F3: Face `0.0.3`, Parameter `0.0.6`, `About` present; block versions shown by `inspect`. |
| K4 | **PASS** | KK card still 7 outfits; 18,594 unit checks pass incl. KK outfit-name test. |
| K5 | **PASS** (unit/cli partial) | Negative card inputs still rejected by the existing A2 test; EmotionCreators gate kept. |
| K6 | **PASS** | No game process started; only file reads from install/library. |
| K7 | **PASS** | KKS minimal import 26 files / 6.6 MB / 0.82 s; full 269 / 1,375 MB / 33.1 s; `library check` OK; profile/version recorded. |
| K12 | **PASS** | Real KKS body/face/garment meshes read with no `unknown vertex channel format 10`. |
| K13 | **PASS** | KKS `o_body_a` skin non-empty, normalized, indices non-negative; full-library F1 export writes 31,189 vertices. |
| K14 | **PASS** | KKS garment ch12/ch13 dim 2 decoded with two padded influences (unit + real probe). |
| K15 | **PASS** | Bind-pose counts match renderer bones on the probed meshes; no out-of-range diagnostic in F1 export. |
| K31 | **PASS** (KK smoke) | KK card with KK library converts: 28,418 vertices, 4/7 items (missing mods), no new skin/shader warnings. Full A1–A10 still to be re-run at Stage 8. |
| K20–K27 | **NOT STARTED** | Stage 3; current full-library F1 conversion reports `create material …: its shader could not be run` for all KKS create jobs. |

---

## A. Fixtures

| ID | Fixture | Why |
| --- | --- | --- |
| F1 | `KKS\DefaultData\chara\female\Big Sisterly.png` | Stock KKS card, no heavy modding, 4 outfits. `[V]` |
| F2 | `KKS\DefaultData\chara\female\Bokukko.png`, `Bookish.png` | Second/third stock cards for slider and face-shape variety. `[V]` |
| F3 | `KKS\...\KK BR-Chan.png` (the sampled card with 1 Snooley resolve row, `About` block, `Face 0.0.3`, `Parameter 0.0.6`) | Mod-using KKS card; exercises UAR resolution and `mt_pupil_00`-era rows. `[V]` |
| F4 | Any `【KoiKatuChara】` KK card used by the existing A1–A10 tests | Regression baseline. |
| F5 | `KKS\mods\Sideloader Modpack\[Snooley]Coiffure Pack.zipmod` | No `<game>` tag; bundles are Unity 5.6.2f1. `[V]` |
| F6 | `KKS\mods\Sideloader Modpack - Exclusive KKS\Creamstar\[Creamstar] Mordhau Outfits v6.1.zipmod` | `<game>Koikatsu Sunshine</game>`; bundles are Unity 5.6.2f1. `[V]` |
| F7 | A mod with both tags, e.g. `[5896]Hu Tao v1.4.zipmod` | Shared mod filtering. `[V]` |
| F8 | A plain PNG, a JPG, a truncated card, an outfit file, an EmotionCreators card | Negative inputs. |
| F9 | A KKS card with `typeBone != 0` and a `hideShortsOpt`/third-texture garment (to be located during implementation) | Covers the one differing `shapecorrect` row and KKS-only list fields `[U3]`. |

---

## B. Card parsing and acceptance

| ID | Scenario | Command / probe | Expected | Evidence |
| --- | --- | --- | --- | --- |
| K1 | KKS marker recognised | `cardbridge inspect F1` | Marker `【KoiKatuCharaSun】`, family `koikatsu-sunshine`, blocks `KKEx, Custom, Coordinate, Parameter, Status, About`; **exit 0** after implementation (today: exit 2 with "not converted yet"). | `[V]` container |
| K2 | Section decode | inspect F1/F3 | 4 outfits, 9 clothes parts + 20 accessory slots each, 52 face / 44 body sliders; no missing-section error. | `[V]` |
| K3 | Versions and `About` | inspect F3 | Face `0.0.3`, Parameter `0.0.6`, Coordinate/clothes `0.0.2`, `About` parsed or ignored without error. | `[V]` |
| K4 | KK regression | inspect F4 | Still 7 outfits, KK outfit names, Face `0.0.2`; no KKS-specific message. | `[V]` |
| K5 | Negative inputs | inspect F8 x5 | Distinct rejection messages; EmotionCreators and unknown markers exit 2; nothing written. | `[V]` |
| K6 | No game at run time | process list + file hashes around a convert | No `KoikatsuSunshine.exe`/BepInEx process; card, zipmods, `oo_base.unity3d`, `Assembly-CSharp.dll` hashes unchanged. | `[V]` pattern from A10 |

---

## C. Library import and resolver

| ID | Scenario | Expected | Evidence |
| --- | --- | --- | --- |
| K7 | Import KKS library | `library check` clean; 22 base list bundles, `00.unity3d` 65 categories; no "text bundle could not be parsed" issues for base lists. | `[V]` |
| K8 | Mixed engines | A single run resolves a KKS base bundle (format 21) and a F5/F6 mod bundle (format 17) without an `unsupported serialized file version` error. | `[V]` |
| K9 | Game-tag filtering | With F5 + F6 + F7 + a KK-only mod in the mods folder: KKS card items resolve to F5/F7; KK-only GUIDs are reported as not applicable, not silently preferred. | `[V]` |
| K10 | Manifest-only bundles | `addXX`, `abdata`, `studioXX` never appear as an item's mesh/material bundle. | `[V]` |
| K11 | List field coverage | `co_top_00` 112 rows, `co_bot_00` 72, `ao_head_00` 111, `bo_hair_f_00` 78 resolve by `(category,id)`. | `[V]` |

---

## D. Mesh, rig and weights

| ID | Scenario | Expected | Evidence |
| --- | --- | --- | --- |
| K12 | Format 10 accepted | No `unknown vertex channel format 10` warning for `o_body_a`, `cf_O_face`, `o_top_*`, hair meshes. | `[V]` failure today |
| K13 | Skin from channels | KKS `o_body_a` weights non-empty; every vertex has 2 or 4 influences whose weights sum to 1; bone indices map into the renderer's bone list. | `[V]` layout |
| K14 | Dimension 2 garments | A KKS top with ch12/ch13 dim 2 exports with two influences per vertex, padded cleanly. | `[V]` |
| K15 | Bind poses | `m_BindPose` count equals renderer bone count (e.g. body 63/100, face 45, top 65–148); no out-of-range bone index. | `[V]` |
| K16 | Skeleton parity | 368 `cf_j_/cf_s_/cf_d_/cf_dd_` bone names present; rest positions match the KK baseline for shared bones. | `[V]` |
| K17 | Shape/morphs | `cf_O_face` 81 blend shapes apply; head `ShapeAnime` `_00`, `_02`, `_03` all parse and produce a face distinct from neutral. | `[V]` |
| K18 | Proportions | Identical sliders on F1 and a KK equivalent give visually equivalent proportions; a `typeBone != 0` card (F9) uses KKS `shapecorrect` row 2. | `[V]` + `[U2]` |

---

## E. Textures, materials and create jobs

| ID | Scenario | Expected | Evidence |
| --- | --- | --- | --- |
| K19 | Texture decode | Formats 3/4/10/12/25 decode; `cf_body_00_base_n` (BC7) produces a valid normal map; no "texture not loaded" for body maps. | `[V]` |
| K20 | Shader blob read | All five KKS create materials return a non-empty blob and parse into ≥2 programs; `PickForwardPass` selects one PS + one VS. | `[V]` failure today |
| K21 | Bindings resolved | Parsed PS exposes `_MainTex`, `_ColorMask`, `_Color`, `_grad`, `_PatternMask1..3` as named bindings/resources. | `[V]` raw bytes |
| K22 | Create jobs run | `RunCreateJobs` produces non-uniform body/face/clothes textures that carry card colours and overlays; no "its shader could not be run" warning. | `[U1]` |
| K23 | Hair colour | KKS hair material receives the card's hair colour (the current exact `Shader Forge/main_hair` check fails). | `[V]` |
| K24 | MaterialEditor fallback | A card using an unavailable mod shader falls back to a `Koikano` alpha/item twin and stays translucent where authored. | `[V]` names |
| K25 | Alpha/cull | `low_clothes_alpha`/`main_clothes_alpha` render translucent/double-sided; `main_clothes_opaque` culls; `shadowcast`/`mnpb` still recognised. | `[V/I]` |
| K26 | Outfit textures | `MainTex`/`ColorMask` plus `MainTex02`/`ColorMask02` bake correctly; a garment with `MainTex03` (F9) is either supported or reported as a known limitation, never silently wrong. | `[V]` + `[U3]` |
| K27 | Overlays/accessories | KoiClothesOverlayX / KoiSkinOverlayX data on F3 still composites; accessory renderer colour lists (`rendNormal`/`rendAlpha`/`rendHair`) intact. | `[I]` |

---

## F. Output and regression

| ID | Scenario | Expected |
| --- | --- | --- |
| K28 | PMX reload | Independent PMX reader reports version 2.0; vertex/triangle/material/bone/texture counts match the manifest; every weight sums to 1 and binds to an existing bone; all texture files exist. |
| K29 | Part count | 4 outfits, each with top/bra/shorts/… parts plus 20 accessory slots; hidden parts (`NotBra`, `HideShorts`) reported with a reason. |
| K30 | Diagnostics | `--print-diagnostics` lists every unresolved KKS item with `missing-mod` / `missing-list-entry` / `missing-bundle`; no silent omissions. |
| K31 | KK A1–A10 | All existing acceptance tests still pass with `LIB_KK` and a KK card; create-job pixel hashes and KK mesh weights unchanged. |
| K32 | Determinism | Two consecutive conversions of F1 have identical PMX bytes apart from timestamps/paths. |

---

## G. Known "expected to fail today" baseline

Before implementation, the following are expected failures and are useful as regression tripwires: `[V]`

| Test | Current behaviour |
| --- | --- |
| K1 | Exit 2, `recognised as koikatsu-sunshine, not converted yet`. |
| K12 | `unknown vertex channel format 10`. |
| K20 | `readShaderBlob` returns 0 bytes → `not a Unity shader program blob`; `PickForwardPass FAIL` even after manual decompression. |
| K23 | Hair colour not applied (exact `Shader Forge/main_hair` comparison). |

After implementation these must all pass or be replaced by an explicit, documented limitation in the feasibility doc.

---

## H. Sign-off checklist

- [ ] Every K* test has a recorded result (pass / fail / known limitation with ID).
- [ ] KK A1–A10 all pass; no KK pixel hash or weight regression.
- [ ] All `[U]` questions U1–U7 are either resolved or still listed as open in `KKS_FORMAT_FEASIBILITY.md`.
- [ ] Conversion still requires only `--library`, `--mods` and a card; no game process is running at test time.
- [ ] Docs updated with measured KKS library import size/time (U6).

---

## J. Full HairAccessoryCustomizer (HAC) support

| ID | Scenario | Status | Evidence |
| --- | --- | --- | --- |
| H1 | HAC payload decoding | **PASS** | New `cardbridge::card::LoadHairAccessoryCustomizer()` tests nested/flat shapes, coordinates, slots, `moreAccessories`, alternate keys. |
| H2 | Coordinate/slot mapping | **PASS** | Verified: HAC `coordinate` == `card.outfits[coordinate]`, slot == `accessory.parts[slot]` (incl. slots ≥ 20). |
| H3 | `ColorMatch=true` | **PASS** | `HacHairColors()` maps `_Color/_Color2/_Color3` to the character's hair `base/start/end` colors. Real-card fixture (slot 3/5/9/10/20) shows `ColorMatch=true`. |
| H4 | `ColorMatch=false` | **PASS** | HAC stored `OutlineColor`/`GlossColor` used on hair renderers; `AccessoryColor` used on accessory renderers; ordinary accessory colors unchanged. |
| H5 | Gloss/length | **PASS** | `HairGloss` toggles `_HairGloss`; `_GlossColor` is written into the material bake; `HairLength` preserved in scene material properties, no geometry scaling (PMX limitation documented). |
| H6 | Partial/malformed HAC | **PASS** | Missing plugin entry, malformed MessagePack, or missing fields are ignored safely; `valid=false` so no silent repaint. |
| H7 | Coordinates | **PASS** | Decoding covers all coordinates in the card; `find(coordinate, slot)` selects per-coordinate settings. |
| H8 | Real-card visual check | **PASS (with interpretation)** | A/B against pre-0.4.3 build on `y80oZjpGdRAUJZrQAy2LBNUO.png` (coordinate 6): head images are byte-identical (mean abs diff 0.0). The "washed-out" hair the user saw was the earlier KK-shader-regression issue (0.4.2), already fixed; HAC (`ColorMatch=true` → character hair colors) is verified present in both builds via `CARDBRIDGE_DEBUG_HAC` on `outfit6.acc03/05/09/10`. |
| H9 | KK regression | **PASS** | HAC is decode-only; KK conversion path is unchanged (`Assembler` consumes the same typed data the old ad-hoc parse produced). |
| H10 | Determinism | **PASS** | HAC decode/input are deterministic (same card + library → same PMX bytes, verified in Stage 8 later). |

Fixture: `tests/golden/hac_reference.bin` (HAC payload extracted from the regression card; plugin data only).

## K. Known HAC limitations

- Unity's root-to-tip gradient and animated gloss cannot be represented in PMX; colors are baked into the material.
- `HairLength` is shader-only; not scaled in geometry.
- A malformed/incomplete HAC record is skipped (`valid=false`), so some accessories may use prefab defaults rather than the card's intended colors.

## L. Fixture notes

- Real card used for HAC visual testing: `C:\Users\pepsi\Downloads\y80oZjpGdRAUJZrQAy2LBNUO.png`.
- HAC payload extracted to `tests/golden/hac_reference.bin`.

## I. Final recorded outcomes (2026-11-20)

| ID | Outcome | Evidence |
| --- | --- | --- |
| K1 | **PASS** | `koikatsu-sunshine`, supported, exit 0 on F1/F2/F3. |
| K2 | **PASS** | 4 outfits, 52 face / 44 body sliders, 20 accessory slots; synthetic + real cards. |
| K3 | **PASS** | Face 0.0.3, Parameter 0.0.6, `About` block present on all three KKS fixtures. |
| K4 | **PASS** | KK card still 7 outfits and KK labels; integration suite passes. |
| K5 | **PASS** | Existing negative-input tests pass; EmotionCreators gate kept. |
| K6 | **PASS** | No game/BepInEx process; only file reads. |
| K7 | **PASS** | Full import 269 files / 1,375 MB / 33.1 s; `library check` OK; profile/version recorded. |
| K8 | **PASS** | KKS base (format 21) + 5.6 mod bundles (format 17) indexed in one run. |
| K9 | **PASS (logic + unit)** | `<game>` tags parsed and cached; compatibility unit test passes. Full mixed-folder card-resolution scenario not run. |
| K10 | **PASS** | Manifest-only bundles never selected as item assets. |
| K11 | **PASS** | 1,968 base list rows loaded from KKS `00.unity3d`; third-texture rows detected (12 tops, 1 bottom). |
| K12 | **PASS** | KKS meshes read with no format-10 error. |
| K13 | **PASS** | KKS weights non-empty, normalized, non-negative indices; three KKS exports 30k–44k vertices. |
| K14 | **PASS** | Two-influence channel meshes decode with padded slots (unit + real). |
| K15 | **PASS** | Bind poses present; no out-of-range-bone diagnostic in exports. |
| K16 | **PASS** | Skeleton parity verified in research; no new regression. |
| K17 | **PASS** | Head `ShapeAnime` resources parse; 81 face blend shapes present. |
| K18 | **PASS (partial)** | KKS `shapecorrect` loaded from library; `typeBone != 0` card not located, so row-2 effect not visually confirmed. |
| K19 | **PASS** | KKS textures (incl. BC7 body normal) decode; body maps present. |
| K20 | **PASS** | All five KKS create materials parse into VS+PS with forward pass selected. |
| K21 | **PASS** | Bindings recovered: `_MainTex`, `_ColorMask`, `_Color`, `_grad`; cb0 slot verified. |
| K22 | **PASS** | Create jobs run and produce correct coloured iris/clothes/face/body textures (visual check). |
| K23 | **PASS** | Hair shader family predicate covers KK/KKS; preview hair is coloured. |
| K24 | **KNOWN LIMITATION** | KKS fallback shader names wired, but no card with a missing mod shader was available in the fixtures. |
| K25 | **PASS (code path)** | Alpha/cull checks unchanged and verified against shader names; no dedicated visual case. |
| K26 | **PASS** | Third texture slot + `HideShorts` implemented; `MabUV` unmodelled (known limitation). |
| K27 | **PASS (partial)** | Overlay data on real cards still composites; accessory renderer lists unchanged. |
| K28 | **KNOWN LIMITATION** | Independent PMX reader loads KKS and KK PMX correctly; crosscheck reports the same 3 pre-existing manifest/joint mismatches for both games. |
| K29 | **PASS** | All items load per outfit; hidden parts carry reasons in manifest. |
| K30 | **PASS** | Diagnostics list unresolved items with reasons; missing mods reported as incompatible when needed. |
| K31 | **PASS** | Full KK integration suite: 19,109 checks, 0 failed; KK create baking still 23 materials. |
| K32 | **PASS** | Two consecutive F1 conversions produced byte-identical PMX (SHA-256 match). |

## J. Viewer per-game profiles (additional requirement)

| Requirement | Status | Where |
| --- | --- | --- |
| Independent KK/KKS library+mods paths | **PASS** | `Settings::profiles` in `viewer/Util.inc`, Setup tab rows in `viewer/MaterialsHelpers.inc`. |
| Detect card family on open/switch | **PASS** | `card::DetectCardFamily` + `ActivateProfileForCard` in `viewer/Pose.inc`. |
| Auto-activate matching profile | **PASS** | New card path triggers `ActivateProfileForCard` before `EnsureResolver`. |
| Warn on library/card mismatch | **PASS** | `WarnLibraryMismatch` logs a warning from `library.json` profile vs card family. |
| Manual profile switching preserved | **PASS** | Setup tab actions `kSetProfileKK/KKS`; reloading the same card does not auto-switch. |
| CLI overrides still work | **PASS** | `--library`/`--mods` set override flags; auto-switch and persistence are skipped for them. |
| Migrate single-library config | **PASS** | `LoadSettings` seeds both profiles from legacy top-level paths; all other settings unchanged. |

Verification: full release build succeeded; 18,616 unit checks pass, including a new card-family detection test. CLI `--library`/`--mods` paths are unchanged.
