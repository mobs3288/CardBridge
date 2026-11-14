# Koikatsu Coordinate PNGs as an outfit source (CardBridge)

**Status: implemented and verified in-repo.** Unit suite covers detection, extraction, the clothing-merge import, the wholesale coordinate import and cross-character transfer; the CLI `convert --clothes` path was run end-to-end against real cards from the maintainer's own game folder (base-game and modded sources). The game-side acceptance is the maintainer's call as usual (CardBridge never launches the game).

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

A proper coordinate import: the **target** outfit is replaced **wholesale** with the source coordinate, keeping the coordinate's own settings as completely as the format allows:

- clothes, accessories and makeup map copies; accessory **parts are copied verbatim** — positioning, rotation, scale, parenting (`parentKey`), colors, `hideCategory`/`noShake` — including slots the source leaves empty (which therefore **clear** the target's, hair-type accessories included; that is the documented difference from Import as Clothing);
- `enableMakeup`/`makeup` follow the source;
- accessory visibility flags (`Status.showAccessory`, 20 flags) are copied when both are 20-slot bool arrays (they are the maker's per-slot show/hide state for the current coordinate);
- resolver rows: every row scoped to the target outfit is dropped (the coordinate is gone wholesale), the source's coordinate rows are rebased `N → M`, 1:1 on accessory indices;
- coordinate-scoped KKEx is rebased the same way: MaterialEditor rows for coordinate N become M (texture ids merged collision-safely as above), KCOX overlays are stored under M (the target's old paint for M is dropped; other coordinates keep theirs), ABMX bones are unioned with the source winning on shared bones (the clothing import's own rule);
- `moreAccessories` XML of the **target** coordinate's `<accessorySet type="M">` block is removed (the extras travel inside the copied coordinate payload; keeping the stale XML block would double-append them on the next card load); other outfits' blocks are kept; flat XML is untouched (unattributed, never read by the loader);
- hair-type accessories replace too (wholesale); the character's hair slots (`Custom/hair`) are never touched, same as any import;
- `clothesState` (per-coordinate "half-off clothes" bytes) is **not** ported: its meaning is unverified in this codebase, and silently copying it would corrupt the target's toggle state. It stays a documented limitation, like all other clothing code here (see `docs/CLOTHING_EXPORT.md` M7).
- `Additional_Card_Info` (`CardInfo`/`CoordinateInfo`) is not ported either way: its schema is not verified here and it can carry character-level identity; neither mode needs it. `showAccessory` is the one visibility flag whose schema is verified.
- **Identity never travels:** `Custom`, `Parameter`, `Status` (except the 20 visibility flags), the source's other coordinates, `ABMX` bone data onto non-shared bones stays (shared-bones union is the clothing import rule), plugin payloads outside ABMX/MaterialEditor/KCOX/MoreAccessories (`hairaccessorycustomizer`, `DynamicBoneEditor`, `PluginListTool`, …) are never copied.

### 3.3 Cross-character transfer

Both modes transfer onto any supported target character:

- clothes/accessory ids are **author slot ids on both sides of a card → card transfer**, so a transfer needs no re-localisation (only the local→slot map for file-born mods moves); patterns and sub-parts follow the same rules as the clothing import;
- accessory parenting (`parentKey`) references the shared maker skeleton, which is the same for every Koikatsu character of a family;
- if the source and target characters have different `Parameter.sex`, a warning note is reported (clothes ids are typically sex-specific; the game would mis-resolve the same items) — the import proceeds, as nothing in the data proves the items incompatible;
- missing mods / unresolved items / incompatible configurations degrade exactly as they do for regular cards (they are reported by the resolver and the issues list; nothing is silently dropped, and the import itself never aborts mid-merge);
- the target's own hair, body, face, name, personality and other six outfits are untouched in both modes (verified in tests).

## 4. Where the modes are used

- **Viewer (drop).** A regular clothing file → the existing `ApplyClothingFile` (unchanged, no dialog). A Coordinate PNG dropped **with a character on screen** → a task dialog naming the source (`<name> — <outfit>`), with two primary buttons: **Import as Clothing** (default) and **Import as Coordinate**, plus **Open as character** (the previous drop behavior) and Cancel. With nothing loaded, a Coordinate PNG opens as a character, as today; likewise a Sunshine/EmotionCreators card opens with its usual note — never a Coordinate source, never clothing import. `CARDBRIDGE_COORDINATE_MODE=clothing|coordinate|open|cancel` answers the dialog without UI (same convention as `CARDBRIDGE_CLOTHES_PATH` / `CARDBRIDGE_CLOTHES_WITH_HAIR`).
- **Pinned imports.** Both modes pin to the outfit slot on screen through the same state as a clothing file (`g.clothingPath`, `g.clothingOutfit`), with `g.clothingMode` (0 = clothing, 1 = coordinate) and `g.clothingSrcOutfit`. The outfit slot chips, `<`/`>` (move the pinned import to another slot) and **Reset clothing** work unchanged for both modes; a reload re-applies the pinned import from the source, exactly like a clothing file.
- **CLI.** `convert <card> --clothes <file>` detects a Coordinate PNG and applies the outfit contents (as-clothing semantics) with a log line saying so (`coordinate (as clothing): <name> (coordinate N) -> outfit M …`). A regular outfit file merges exactly as before. There is no interactive choice on the CLI and no path that falls back to opening the card as a character; a failed parse is a hard error. Import-as-Coordinate is viewer-side only for now (it is an interactive, wholesale operation).

## 5. What is deliberately not implemented

- **Export / viewer paths**: untouched; the import happens in memory and behaves like any applied outfit on the exported PMX;
- **Writing cards**: cards stay read-only; imports are in-memory, exactly like the clothing import.
- **Importing the source's ABMX onto a vanilla target with no `KKEx` block**: the ABMX merge (shared with the clothing import) needs a destination KKEx map to write the union into; when the target card has no KKEx at all, the source's bone mods cannot be stored yet (the clothing import has always behaved the same). Not a new limitation — flagged here for completeness.
- **`Parameter.sex` mismatch**: warning only (§3.3); ids of the wrong sex resolve to nothing and are reported missing as usual.
- **`clothesState` / `Additional_Card_Info` / `coordinateType` of the target**: never copied (§3.2).
- **Impure round-tripping of `KKEx` plugins other than ABMX / MaterialEditor / KCOX**: not copied; consistent with the clothing import.

## 6. Tests

`tests/cardbridge_tests.cpp` (all in-memory except where noted):

- **source detection** — temp-file fixtures for plain PNG, `【KoiKatuClothes】` (and `Sun` variant), the three character markers, Sunshine/EC markers, junk; detection by bytes, never name; a Coordinate PNG is rejected by `LoadClothingFile` (never silently absorbed as clothing);
- **end-to-end** — a minimal `【KoiKatuChara】` PNG encoded by the test fixture (PNG + productNo + marker + Custom/Coordinate/Parameter/Status/KKEx blocks), re-opened by `LoadCard`; `ApplyOutfitSource` applied to a second, in-memory character; asserts clothes/accessories transfer, hair accessory preserved, identity untouched, resolver rows rebased and accessory slot re-indexed, provenance ids set;
- **import as clothing** — extraction → `ApplyClothing` (merge semantics, relocated accessory slot, `clothingPartIds`, makeup flag, outfits not on screen untouched);
- **import as coordinate** — wholesale replacement incl. hair accessory; makeup; resolver rows dropped/rebased 1:1; ABMX untouched semantics; KCOX: other coordinate kept, replaced coordinate's paint dropped, source's stored under the target index; MaterialEditor: target coordinate's rows dropped, others kept, source rows rebased `CoordinateIndex` N→M (collision-safe texture dictionary path covered by the shared helper); MoreAccessories XML: target's `<accessorySet>` block removed, others kept; `showAccessory` copied; identity/hair/other outfits untouched;
- **error paths** — empty source, unsupported source, out-of-range indices; coordinate index resolution via `Status.coordinateType`.

Plus real-data runs: CLI `convert … --clothes <a Coordinate PNG>` on vanilla and modded sources from the maintainer's own install produced the expected merged outfit (exit 0, `coordinate (as clothing): …` log line, `added_outfit` in the manifest); Sunshine/EC cards remain on the plain load path.
