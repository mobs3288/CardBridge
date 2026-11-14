# Koikatsu Coordinate PNGs as an outfit source (CardBridge)

**Status: implemented and verified in-repo.** Unit suite covers detection, extraction, the clothing-merge import, the wholesale coordinate import, cross-character transfer, the coordinate-scoped plugin payloads (ABMX/KKEx creation, Accessory_States, DynamicBoneEditor, HairAccessoryCustomizer, pushup, Additional_Card_Info v0/v1), the sex-mismatch warning and the export/re-import round trip; the CLI `convert --clothes` path was run end-to-end against real cards from the maintainer's own game folder (base-game and modded sources) and the game's export→convert round trip was verified. The game-side acceptance is the maintainer's call as usual (CardBridge never launches the game).

Format findings in §6 were verified against the installed game data (Koikatsu BetterRepack, `UserData/chara` female+male, 2249 readable cards) by decoding the cards' own bytes and by reading the managed assemblies (`ClothesKind` enum order, `ClothesStateMenuX`'s state names) and the plugins' source (Accessory_States, Additional_Card_Info). Where a value could only be observed, it says so.

---

## 1. What a "Coordinate PNG" is, in Koikatsu's own formats

Koikatsu appends binary data to PNGs after the `IEND` chunk. Three families exist, told apart strictly by the marker string after the image (never by the file name):

| Marker prefix | Family | What it is | CardBridge path |
|---|---|---|---|
| `【KoiKatuClothes…` | Outfit / "coordinate" file (`UserData/coordinate`) | One coordinate: clothes + accessories + makeup + KKEx. This is what every Koikatsu user means by a "coordinate card" and what CardBridge has always called an outfit file | regular clothing import (unchanged) |
| `【KoiKatuChara】` / `【KoiKatuCharaS】` / `【KoiKatuCharaSP】` | Character card | Blocks: `Custom` (face/body/hair), **`Coordinate`** (7 outfits; each holds one coordinate payload), `Parameter`, `Status`, `KKEx` | character load; **as a Coordinate PNG** when used as an outfit source (this feature) |
| `【KoiKatuCharaSun】` / `【EroMakeChara】` | Sunshine / EmotionCreators | Not converted yet | character load, unchanged; never a Coordinate source |

An important, verified fact from the game's own files and the reference deserializer (kkloader): **a Koikatsu coordinate file (`【KoiKatuClothes】`) and one element of a character card's `Coordinate` block carry exactly the same payload.** There is no separate "coordinate file format" for Koikatsu; the coordinate is a payload shape, not a file family. The `Coordinate` block inside a card is a MessagePack array of bins, one bin per outfit; each bin is `clothes (i32 + msgpack) + accessories (i32 + msgpack) + enableMakeup (u8) + makeup (i32 + msgpack)` — precisely what a clothing file wraps after its own header (`decodeCoordinate` reads both).

**So, in this feature, a "Coordinate PNG" is a Koikatsu character card used as an outfit source** — "take a Coordinate PNG from Character A and put its outfit on Character B". That reading is the only one in which the two operations differ (see §3) and the only one where "do not transfer the source character's identity, face, body, personality" names real data. A plain clothing file remains what it always was (`Regular clothing`) and goes through the existing import with no dialog (§1.2 of the brief: regular clothing imports are unchanged).

What detection does (`card::DetectImageSource`, `src/card/CardFile.cpp`): reads the PNG signature and walks to `IEND`, skips the 4-byte product number, reads the marker with the same 7-bit length prefix every card parser here uses, and classifies: outfit-file marker → `RegularClothing`; Koikatsu character-card marker → `Coordinate`; everything else → `Invalid`/`PlainPng` (callers fall back to the previous behavior, never to clothing import). It never throws and never looks at the file name.

## 2. How the existing pipeline works (and what "reuse" means here)

`ApplyClothing` (`src/card/CardFile.cpp`) merges an **outfit file** into one outfit of a character:

- clothes replace wholesale; the makeup follows the file's own enable flag;
- accessories **merge**: empty file slots never clear the character; the character's own slots win when occupied, and **hair worn as accessories (type 122/123) is never deleted** (it is kept or moved to a free slot);
- the file's Sideloader rows are re-prefixed to the target outfit (`outfitN.…`), with accessory indices re-mapped to the merged slots; the file's ids are **session locals** translated to the card's author slot ids on the way in;
- coordinate-scoped KKEx (ABMX bone mods, MaterialEditor rows, KCOX garment paint) is merged for the file's one coordinate, rebased to the target outfit; textures are merged collision-safely (fresh ids on collision);
- MoreAccessories extras past slot 20 travel through the card's XML (the file's flat shape or the card's `<accessorySet type="N">` blocks), appended to the chunk — never re-serialised by re-encoding parts.

That is the pipeline both import modes feed, once an in-memory `ClothingFile` exists. Regular clothing imports, their replacement and hair behavior are byte-identical after the change (the shared helpers parameterise the source coordinate; defaults are the file's single coordinate 0).

## 3. The two modes

### 3.1 Import as Clothing — `ExtractClothingFromCard` → `ApplyClothing`

The source card's coordinate N (default: the source's own current outfit, `Status.coordinateType`) is turned into an **in-memory `ClothingFile`**:

- `outfit` = the coordinate payload copied verbatim (clothes, accessories, makeup, enableMakeup — positioning, rotation, scale, parenting, colors, hide flags all travel).
- `rows` = the source's resolver rows for **that coordinate only**, rebased to the prefix-free shape a file stores (`outfit1.ChaFileClothes.…` → `ChaFileClothes.…`), with **`LocalSlot` cleared**: a card already stores author slot ids, so the merge's local→slot translation becomes a no-op and ids pass through unchanged.
- `extended` = the source's full KKEx map; the readers that consume it are parameterised by `ClothingFile::srcCoordinate` (default 0), so ABMX / MaterialEditor / KCOX / MoreAccessories aim at coordinate N, not 0.
- `name` = `"<Character> (<outfit name>)"` for logs and the Added-outfit chip; `path`/`sha256` from the source (provenance only).

Then the result goes through **`ApplyClothing` unchanged** — the same merge a regular outfit file takes. Everything the existing import does (replacement semantics, hair preservation, accessory placement around hair, resolver-row rebasing with slot re-indexing, ABMX/MaterialEditor/KCOX merging, `clothingPartIds` provenance for the exporter's "Added outfit" split) behaves identically to dropping a clothing file of that outfit.

**Nothing of the source's identity travels:** `Custom` (face/body/hair), `Parameter`, `Status`, the source's other six coordinates and any coordinate-agnostic plugins are simply not copied into the extraction. Hair *slots* (the character's own back/front/side/extension hair, which lives in `Custom/hair`) are untouched, like any clothing file.

### 3.2 Import as Coordinate — `ApplyCoordinate`

This path serves two source families, both applying the *whole coordinate* to the target outfit slot. A character card (【KoiKatuChara】, the marker Koikatsu itself uses for a coordinate source) carries `Status`, so
`clothesState` and `showAccessory` follow the coordinate (§6.1, §6.3); an outfit file (【KoiKatuClothes】, Koikatsu's own "coordinate" save — what the `UserData/coordinate` folder holds) carries no `Status`, so those two do not apply; the file's resolver-row ids are the session locals the file was written with and are translated to the card's author ids on the way in — the same id path the clothing import uses.

A proper coordinate import: the **target** outfit is replaced **wholesale** with the source coordinate, keeping the coordinate's own settings as completely as the format allows:

- clothes, accessories and makeup map copies; accessory **parts are copied verbatim** — positioning, rotation, scale, parenting (`parentKey`), colors, `hideCategory`/`noShake` — including slots the source leaves empty (which therefore **clear** the target's, hair-type accessories included; that is the documented difference from Import as Clothing);
- `enableMakeup`/`makeup` follow the source;
- accessory visibility travels in full, in both modes: the 20-flag `Status.showAccessory` (current-coordinate show/hide) copied when both sides carry 20 bools, **plus** `Accessory_States.CoordinateData` (the jalil49 Accessory_States plugin's per-coordinate, per-slot binding/state machine) copied or merged with slot re-mapping — see §6.3;
- clothes toggle state (`Status.clothesState`, a `byte[9]` indexed by `ClothesKind`, 0=on / 1=half-off / 2=half-off#2 / 3=off) is copied in Coordinate mode (see §6.1); clothing mode never touches `Status`;
- resolver rows: every row scoped to the target outfit is dropped (the coordinate is gone wholesale), the source's coordinate rows are rebased `N → M`, 1:1 on accessory indices;
- coordinate-scoped KKEx is rebased the same way: MaterialEditor rows for coordinate N become M (texture ids merged collision-safely as above), KCOX overlays are stored under M (the target's old paint for M is dropped; other coordinates keep theirs), ABMX bones are **rebased per coordinate** (the source coordinate's effective entry replaces only the target outfit's slot in the shared list; blanket length-1 lists expand to the target's outfit count; a target with no `KKEx` at all gets one created) — see §6.4;
- further coordinate-scoped plugin payloads travel with the coordinate in both modes (remapped in clothing mode, wholesale in coordinate mode): `Accessory_States`, DynamicBoneEditor `AccessoryDynamicBoneData`, HairAccessoryCustomizer `HairAccessories`, pushup `Pushup_TopData`/`Pushup_BraData` — see §6.3;
- `Additional_Card_Info`: the source coordinate's own ACI entry travels (Coordinate mode: the whole per-coordinate entry; clothing mode: the accessory keep flags only). The character-scoped keys never travel — see §6.2;
- resolver rows and the merge are kept consistent with the card's **extended** `universalautoresolver` payload, which is rewritten to match the merged rows (untouched rows keep their raw bins verbatim) — this fixes the clothing export → re-import round trip, which previously lost all rows on re-export;
- `moreAccessories` XML of the **target** coordinate's `<accessorySet type="M">` block is removed (the extras travel inside the copied coordinate payload; keeping the stale XML block would double-append them on the next card load); other outfits' blocks are kept; flat XML is untouched (unattributed, never read by the loader);
- hair-type accessories replace too (wholesale); the character's hair slots (`Custom/hair`) are never touched, same as any import;
- **Identity never travels:** `Custom`, `Parameter`, `Status` (except `showAccessory` and — Coordinate mode only — `clothesState`), the source's other coordinates, `Pushup_BodyData` (the wearer's own body, not the garment), ACI `CardInfo`/`Personal_Clothing_Save`/`MakeUpKeep`/`Cosplay_Academy_Ready`/`CoordinateNames` (MoreOutfits), `BraHigherPriority`, and plugins outside the ported set are never copied.

### 3.3 Cross-character transfer

Both modes transfer onto any supported target character:

- clothes/accessory ids are **author slot ids on both sides of a card → card transfer**, so a transfer needs no re-localisation (only the local→slot map for file-born mods moves); patterns and sub-parts follow the same rules as the clothing import;
- accessory parenting (`parentKey`) references the shared maker skeleton, which is the same for every Koikatsu character of a family;
- if the source and target characters have different `Parameter.sex` (female = 1, male = 0 -- verified on the install), both modes note it accurately: mod items resolve by GUID regardless of sex (the note counts the source's mod rows), base items resolve by id (shared tables; there is no per-sex table in the library), and garments authored for one sex may clash with the target's body shape. If the source coordinate carries its own ACI gender-restriction flag (`_restrictioninfo._gender` / `GenderType`), the note names it too -- it is imported with the coordinate, never enforced and never discarded; the import proceeds.
- missing mods / unresolved items / incompatible configurations degrade exactly as they do for regular cards (they are reported by the resolver and the issues list; nothing is silently dropped, and the import itself never aborts mid-merge);
- the target's own hair, body, face, name, personality and other six outfits are untouched in both modes (verified in tests).

## 4. Where the modes are used

- **Viewer (drop).** An outfit file (【KoiKatuClothes】, what Koikatsu itself saves as a “coordinate” file, e.g. `UserData/coordinate/*.png`) dropped on the viewer **always opens a task dialog**: with a loaded, idle character it asks how to import, with two primary buttons **Import as Clothing** (default) and **Import as Coordinate**, plus Cancel; with no character on screen (or while the current load is still running) the dialog explains there is nothing to import onto yet, and offers only Cancel. Dropping again once a character is loaded offers the import choices. A dropped character card (【KoiKatuChara】, a Koikatsu card) opens as a character exactly as before — no dialog, no coordinate import (the feature's coordinate-from-card source remains available programmatically and through the CLI `--clothes` path, which imports as clothing); a Sunshine/EmotionCreators card likewise opens with its usual note. `CARDBRIDGE_COORDINATE_MODE=clothing|coordinate|cancel` answers the dialog without UI (same convention as `CARDBRIDGE_CLOTHES_PATH` / `CARDBRIDGE_CLOTHES_WITH_HAIR`).
- **Pinned imports.** Both modes pin to the outfit slot on screen through the same state as a clothing file (`g.clothingPath`, `g.clothingOutfit`), with `g.clothingMode` (0 = clothing, 1 = coordinate) and `g.clothingSrcOutfit`. The outfit slot chips, `<`/`>` (move the pinned import to another slot) and **Reset clothing** work unchanged for both modes; a reload re-applies the pinned import from the source, exactly like a clothing file.
- **CLI.** `convert <card> --clothes <file>` detects a Coordinate PNG and applies the outfit contents (as-clothing semantics) with a log line saying so (`coordinate (as clothing): <name> (coordinate N) -> outfit M …`). A regular outfit file merges exactly as before. There is no interactive choice on the CLI and no path that falls back to opening the card as a character; a failed parse is a hard error. Import-as-Coordinate is viewer-side only for now (it is an interactive, wholesale operation).

## 5. Remaining unavoidable limitations

- **Export / viewer paths**: untouched; the import happens in memory and behaves like any applied outfit on the exported PMX;
- **Writing cards**: cards stay read-only; imports are in-memory, exactly like the clothing import;
- **`clothesState` is a screen state, not a per-coordinate store.** The game keeps exactly one `Status.clothesState` (a `byte[9]`, one byte per `ClothesKind`); the coordinate block itself carries only `hideBraOpt`/`hideShortsOpt` (the two special bra/panties hide cases). A Coordinate PNG therefore carries only the toggle state that was on screen when the source was saved. Copying it is faithful to what the author had visible, but: (a) it is the save-time screen state, not a look; (b) `2` (the underwear "half#2" state) is only offered by the maker where a slot has that state — the bytes are still copied verbatim as stored; (c) it cannot tell apart "outfit saved indoors" from "outfit saved half-naked", so the outfit is imported with its toggles exactly as the author left them. Importing it is opt-in Coordinate mode only; the clothing import never touches it;
- **`Additional_Card_Info` character-scoped keys never travel** (in either mode): `CardInfo` (per-part keep flags + Cosplay/Advanced folder directories), `Personal_Clothing_Save`, `MakeUpKeep`, `Cosplay_Academy_Ready`. These describe the wearer, not the coordinate;
- **v1 ACI arrays must span the source's outfit count exactly.** A v1 payload whose per-coordinate arrays have another length (an older or third-party generator) is not ported rather than guessed at. v1 payloads in real outfit files (single coordinate) are likewise not ported (real outfit files carry no ACI — verified on the install);
- **`Pushup_BodyData`** (the wearer's own breast/body pushup tuning) is tied to the character's body, not to her garments, and is not ported. Top/bra (`Pushup_TopData`, `Pushup_BraData`) are tuned to the garments and travel; `BraHigherPriority` (a character-global preference) is not ported either;
- **MoreOutfits `CoordinateNames` are not ported.** They name the source card's outfits; the target picks its own outfit count and meaning, and synthesizing or renumbering outfits on the target is not this feature's job;
- **Sex mismatch: warning only** (§3.3); the import proceeds and nothing is dropped;
- **Sunshine / EmotionCreators** cards are never Coordinate sources; Koikatsu's own "coordinate file" family (`【KoiKatuClothes】` markers) is the regular clothing import, never Coordinate mode;
- **MoreOutfits cards**: a coordinate import applies exactly the asked outfit slot; the source's other 9/11/29/… outfits are not synthesized on the target and nothing is deleted there;
- **ABMX rule difference**: Coordinate mode rebases per coordinate (§6.4); the clothing-file path keeps the historical whole-bone-union rule (the source's outfit wins each shared bone wholesale). The two are deliberately different and documented rather than unified;
- **Unknown KKEx plugins are never copied** other than the merged set (ABMX / MaterialEditor / KCOX / MoreAccessories / Accessory_States / DynamicBoneEditor / HairAccessoryCustomizer / pushup / Additional_Card_Info); hidden behaviours of plugins not yet understood (e.g. `UncensorSelector` per-coordinate settings, `KSOX`) are left alone;
- **`ExtendedSaveData`** (in `Coordinate` maps and `Status`) is the literal `null` in every card observed (726/2249 clothes maps carry the key); it is unmanaged and untouched.

## 6. Verified format findings

Everything in this section was verified against the installed game data (Koikatsu BetterRepack, `UserData/chara` female+male — 2256 PNGs found, 2249 readable) by decoding the cards' own bytes, disassembling the managed assemblies (`ChaFileStatus`, `ClothesKind`) and reading the plugins' source and assemblies.

### 6.1 `clothesState` (`Status.clothesState`)

- **Shape:** `byte[9]`, one byte per `ClothesKind`. The enum order was read from the assembly: top=0, bot=1, bra=2, shorts=3, gloves=4, panst=5, socks=6, shoes_inner=7, shoes_outer=8. All 2249 readable cards carry exactly 9 bytes.
- **Values:** `0` = on, `1` = ½ (half-off), `2` = ½#2 (underwear state), `3` = off. Verified against `ClothingStateMenuX` (keelhauled), which presents exactly those four states per slot, and against real cards (2111 all-zero, 100 all-`3`, 38 mixed). `ChaControl::SetClothesState(int clothesKind, byte state, bool next)` and `ChaFileStatus::get_clothesState`/`set_clothesState` treat the array indexed by kind (disassembled during the investigation; `GetNowClothesType` reads it the same way).
- **Semantics:** the current screen state of one character — **not** a per-coordinate record. The per-coordinate hide flags `hideBraOpt`/`hideShortsOpt` live in the coordinate payload and are carried by the payload itself.
- **Import behaviour:** Coordinate mode copies the source's `clothesState` into the target (creating the key when absent) after validating byte values (≤ 3) and — when the target already has an array — its length; a source with unexpected values leaves the target untouched with a note. Clothing mode never touches `Status`. Note: the source's bytes are the state of the coordinate that was on screen at save time; the coordinate provenance is `Status.coordinateType`.

### 6.2 `Additional_Card_Info` (KKEx `Additional_Card_Info`)

- **Two generations in the wild** (verified on the 235 scanned cards that carry it: 233 v0, 2 v1):
  - **v0** — `[1, { CoordinateInfo: bin({coordIdx: entry}), CardInfo: bin({...}) }]`. Per-coordinate entries: `_makeup` (bool), `_clothnot` (bool[3]), `_coordsavebool` (bool[9]), `_acckeep` / `_hairkeep` (slot indices the maker keeps when loading outfits), `_creatornames`, `_set`, `_subset`, `_restrictioninfo` = `{_personality, _trait, _interest, _height[3], _breast[3], _htype, _club, _gender, _coordtype, _coordsubtype}`. `CardInfo`: `{_personalsavebool[9], _cosplayready}` (+ Advanced folder settings in newer versions).
  - **v1** — `[0, { <key>: bin(msgpack) }]`, flat bins; per-coordinate values are arrays spanning the card's outfit count (7 for vanilla): `HairAcc`, `AccKeep`, `CoordinateSaveBools` ([7] × [9] bools), `PersonalityType_Restriction`, `TraitType_Restriction`, `HstateType_Restriction`, `ClubType_Restriction`, `Height_Restriction` ([7] × [3]), `Breastsize_Restriction`, `Interest_Restriction`, `CoordinateType`, `CoordinateSubType`, `Creator`, `Set_Name`, `SubSetNames`, `ClothNot`, `GenderType`. Character-scoped flat keys: `Personal_Clothing_Save`, `MakeUpKeep` (both [9] bools), `Cosplay_Academy_Ready` (bare bool). The two v1 cards in the wild also carry `CoordinateSaveBools` as bins over the same outfit span.
- **Coordinate mode:** the source coordinate's own entry is ported to the target's outfit slot (v0: the entry as stored; v1: the `n`-th element of every per-coordinate array). `CardInfo` (character-scoped) never travels; the target's other outfits' entries and its own ACI character keys stay.
- **Clothing mode:** only the accessory keep flags travel (`_acckeep`/`_hairkeep` in v0; `AccKeep`/`HairAcc` in v1) — what a clothing file semantically carries. Restrictions, authors, set names and makeup-keep never follow the clothes.
- **Restriction flags:** `_restrictioninfo._gender` (v0) / `GenderType` (v1) is reported (never enforced) when the source coordinate is applied across sexes; every `_gender` observed in the scanned cards is `0`.
- **Cosplay Academy:** ACI is the data store of the "Additional Card Info" / cosplay-maker tooling (jalil49 Maker_Tools family). The coordinate restrictions are honoured by the maker's coordinate browser, not by imports.
- **ABI fragility:** v1 detection keys on the absence of `CoordinateInfo`-as-bin; both generations coexist per-game and the merge handles either.

### 6.3 Accessory visibility — `showAccessory` + `Accessory_States` + friends

- `Status.showAccessory` (bool[20]) — the maker's per-slot show/hide for base accessories, current outfit only. Coordinate mode copies it; clothing mode never touches it. 35 distinct flag patterns exist on the install (2240/2249 cards have at least one `true`; 2195 all-`true`; 9 all-`false`).
- `Accessory_States` (KKEx `Accessory_States`, entry version 2; present in 927 cards): the full per-coordinate, per-slot visibility/state machine, covering MoreAccessories slots beyond 20 (observed `_slotinfo` indices 0..106+). `CoordinateData` = `bin({coordIdx: {...}})`:
  - `_slotinfo`: `{slotIdx: {_binding, _state, _shoetype, _parented}}`. `_binding` = a `ClothesKind` index (0–8) — hide/show this accessory while that clothing part is in the given state; **−1 = unbound** (the accessory is not tied to a clothing part).
  - `_state`: array of `[lo, hi]` ranges over `clothesState` values in which the slot is visible.
  - `_names`: `{slotIdx: {_name, _statenames{0: "Normal", ...}}}`; `_clothnotdata`: `bool[3]` (top/bot/underwear don't-fit); `_forceclothnotupdate`: `bool`.
  - Coordinate mode ports the coordinate's `CoordinateData` entry wholesale into the target outfit slot; clothing mode merges entries with the hair-aware slot re-map.
- `DynamicBoneEditor` (KKEx `com.deathweasel.bepinex.dynamicboneeditor`, 356 cards): `AccessoryDynamicBoneData` = bin(array of rows keyed by `CoordinateIndex` + `Slot`). Coordinate mode: the target outfit's rows are dropped, the source coordinate's rows are rebased N→M (slots re-indexed 1:1 — same mapping as accessories); clothing mode: rows for slots that travelled only. Other coordinates' rows are never touched.
- `HairAccessoryCustomizer` (KKEx `com.deathweasel.bepinex.hairaccessorycustomizer`, 1813 cards): `HairAccessories` = `bin({coordIdx: {slotIdx: settings}})`; same rebase rules. Per slot, the source's settings replace the target's for the outfit.
- `pushup` (KKEx `com.deathweasel.bepinex.pushup`, 1128 cards): `Pushup_TopData` / `Pushup_BraData` = `bin({coordIdx: {...}})` — tuned to the garments, travel with the outfit in both modes. `Pushup_BodyData` (the wearer's own breasts/body) is not ported. `BraHigherPriority` (character-global) is not ported.

### 6.4 ABMX and KKEx creation

- KKABMX v3 stores bones as `{bone: [AbmxMod, ...]}`. A list longer than 1 is **coordinate-indexed** (entry n = coordinate n); a length-1 list is a **blanket** shared by every coordinate.
- **Coordinate mode (fixed limitation):** the source coordinate's effective entry (its own slot entry if the list is long enough, else the blanket) replaces only the target outfit's slot:
  - target had no KKEx at all → one is created (previously the source's bone mods were silently unstorable);
  - target bone list was length-1 → expanded to the target outfit count, blanket preserved everywhere except outfit m, which takes the source's own effective entry;
  - target bone list was multi-entry → padded to the outfit count if needed; only index m is replaced; other outfits keep theirs;
  - source bone effective entry empty → nothing written for the bone;
  - unknown plugins in the target's KKEx are preserved verbatim.
- **Clothing-file path (unchanged):** the source wins each shared bone wholesale (whole-list union) — the historical rule, kept deliberately.
- **KCOX**: overlays are keyed by coordinate (int keys in cards, string keys in files — verified both); coordinate mode drops the target outfit's paint, stores the source's under the target's index; clothing mode applies the same rebase. Other coordinates keep theirs. `ExtendedSaveData` never interferes.

### 6.5 Resolver rows and the extended universalautoresolver (UAR) payload

- `LoadCard` seeds `card.rows` from KKEx but never writes it back; after a clothing export, the re-exported outfit file lost all rows (round-trip bug found by this phase). Fixed: both merge paths now rewrite the extended `com.bepis.sideloader.universalautoresolver` bin to match the merged rows — rows for the outfit are dropped wholesale (Coordinate import) or for the replaced parts only (clothing path), surviving rows keep their raw bins verbatim (extra keys survive), the rebased rows the merge added are appended. A target with no UAR gets one seeded from the merged rows (the five keys CardBridge's row model round-trips). All other extended keys are untouched.

### 6.6 Sex

- `Parameter.sex`: **female = 1, male = 0** (verified on the install: 2102 female / 147 male readable cards; `ill_male_*` samples are `0`).
- No per-sex list tables exist in the library: base items resolve by id (shared `clothes` tables for both sexes, as shipped), mod items by GUID. The sex note therefore counts the source's mod rows and never claims an item is unusable.
- `_gender`/`GenderType`: the ACI gender-restriction flag of the source coordinate, imported with the coordinate (the maker honours it in its coordinate browser); reported verbatim (`raw value N`), never enforced. All scanned cards observe `0` (no restriction set) in practice.

### 6.7 Coordinate arrays and outfit counts

- Vanilla cards carry a `Coordinate` array of 7; MoreOutfits cards up to 30 (observed sizes: 4, 5, 8–22, 30). Coordinate imports apply the asked outfit slot only; other outfits are never synthesized or deleted. `moreoutfits` stores `CoordinateNames` (`bin({coordIdx: name})`) which are **not** ported (the target names its own outfits).

## 7. Tests

`tests/cardbridge_tests.cpp` (all in-memory except where noted):

- **source detection** — temp-file fixtures for plain PNG, the outfit marker (and `Sun` variant), the three character markers, Sunshine/EC markers, junk; detection by bytes, never name; a Coordinate PNG is rejected by `LoadClothingFile` (never silently absorbed as clothing);
- **end-to-end** — a minimal character PNG encoded by the test fixture (PNG + productNo + marker + Custom/Coordinate/Parameter/Status/KKEx blocks), re-opened by `LoadCard`; `ApplyOutfitSource` applied to a second, in-memory character; asserts clothes/accessories transfer, hair accessory preserved, identity untouched, resolver rows rebased and accessory slot re-indexed, provenance ids set;
- **import as clothing** — extraction → `ApplyClothing` (merge semantics, relocated accessory slot, `clothingPartIds`, makeup flag, outfits not on screen untouched);
- **import as coordinate** — wholesale replacement incl. hair accessory; makeup; resolver rows dropped/rebased 1:1; KCOX (other coordinate kept, replaced coordinate's paint dropped, source's stored under the target index); MaterialEditor rows rebased N→M (texture ids merged collision-safely); MoreAccessories XML block removed for the target, others kept; `showAccessory` copied; identity/hair/other outfits untouched;
- **clothesState** — Coordinate mode copies the source's byte[9] (creating the key when the target lacks it), keeps the target's own when the source's values are out of range (with a note), and clothing mode never touches `Status`;
- **Additional_Card_Info** — v0 and v1 fixtures with distinct outfit markers: Coordinate mode replaces the source coordinate's entry (and nothing else — other outfits, `CardInfo`, character-scoped keys and the target's own restrictions stay), clothing mode ports only the accessory keep flags (`_acckeep`/`_hairkeep` / `AccKeep`/`HairAcc`), a target without ACI gains one, and the clothing-merge lands keep flags in the outfit the file was applied to;
- **ABMX** — a vanilla target with no `KKEx` gets one created; the source's shared (length-1) bone expands to the target's outfit count with the blanket preserved at other outfits; the source's multi-entry (coordinate-indexed) bone replaces only the target outfit's index; unknown plugins (`KSOX`) in the target survive verbatim; the clothing path keeps the historical union rule;
- **per-coordinate plugins** — DynamicBoneEditor, HairAccessoryCustomizer, Accessory_States and pushup payloads rebased `n → m` (coordinate mode wholesale; clothing mode slot-remapped around kept hair, unmapped slots dropped); outfit 3 (and the character's body pushup) untouched;
- **sex mismatch** — a male source onto a female target reports the mismatch (with the mod-row count) and the source coordinate's gender restriction (raw value); nothing is dropped;
- **survival of unrelated data** — unknown plugins in the target's KKEx (`KSOX`, `custom.plugin`) and the target's other outfits survive the import byte-for-byte;
- **round trip** — Coordinate import → outfit-file export → clothing import preserves clothes ids, resolver rows (extended UAR bin kept in sync by the merge), and accessory placement (the kept hair accessory survives, the source's travels);
- **error paths** — empty source, unsupported source, out-of-range indices; coordinate index resolution via `Status.coordinateType`.

Plus real-data runs: CLI `convert --clothes <a Coordinate PNG>` on vanilla and modded sources from the maintainer's own install produced the expected merged outfit (exit 0, `coordinate (as clothing): ...` log line, `added_outfit` in the manifest), and the outfit export → convert round trip passed against those real cards; Sunshine/EC cards remain on the plain load path.
