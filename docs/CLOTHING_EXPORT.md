# CardBridge — Exporting a clothing (outfit) file from a character card

**Status: implemented and verified.** Phases 0–3 of this plan are built and verified (Phase 0 findings are recorded in §3 and §8; the writer, the CLI and the viewer are in, `tests/cardbridge_tests.cpp` carries the round-trip suite). See "Implementation record" at the end. Remaining open items (game-side acceptance, hair/garment exclusions, `hideOpt` semantics) are listed there — the design did not change.
**Baseline:** `master` @ `0d26d18` — CardBridge `0.2.1-alpha`, Windows, MSVC.

This document answers the question "can I export a clothing from a character
card, in the viewer and the CLI — is it possible?" and records the design for
doing it. Short answer: **yes.** Almost everything the feature needs already
exists in the tree; what is missing is (1) a clothing-file *writer* (the reader
exists), (2) a CLI subcommand, (3) a viewer button, (4) tests. The one thing
that cannot be verified from inside the repo is whether the game itself
accepts a file written this way; everything else is round-trippable and
testable.

---

## 1. What this feature is — and what it is not

A Koikatsu **outfit file** (also called a *clothing file*) **is** a `.png`
file — that is Koikatsu's own container format for coordinates, not a CardBridge
invention. Opening one shows a plain image; the real payload (the coordinate
data) is appended after the PNG's IEND chunk, and the game and CardBridge read
it from there. A character card is the same trick with more blocks.

```text
outfit.png:  [ PNG image  ..........................  IEND ][ payload after IEND ]
                                                            └─ marker, name,
                                                               clothes/accessory/makeup
                                                               (MessagePack) + KKEx
```

So "export a clothing from the card" can only produce a file of the family
Koikatsu already has for that: a `.png`. There is no Koikatsu "clothing
model" format to export into — the mesh, skeleton and materials all live in
the game's AssetBundles and the zipmods, not in the card or the outfit file.
The image part is cosmetic (an icon); the data part is what makes it outfit
data. CardBridge writes it the same way Koikatsu and `--clothes` already read
it: a rendered, bald, grey-faced mannequin in the outfit as the icon (or the
face thumbnail, or any PNG the user chooses), then the coordinate payload
appended after IEND.

CardBridge today can **apply** an outfit file to a character (`--clothes` on
the CLI, drag-and-drop in the viewer). It cannot **produce** one.

This feature adds the missing direction: **export one outfit slot of a
character card as an outfit file.** Concretely:

- The user opens a card (or nothing else) and asks for the coordinate on
  screen (`Shift+E` / a new button in the viewer, or
  `cardbridge clothes <card.png> --outfit N --out <file.png>`).
- CardBridge writes a valid `【KoiKatuClothes】` `.png` that:
  - re-applies to any character through the existing merge path (and through
    the CLI's `--clothes`),
  - contains for that coordinate: the clothes (with jacket sub-parts and
    pattern ids), the makeup, the Sideloader resolver rows, the garment paint
    (`KCOX`) and the MaterialEditor main-texture edits — rebased to
    coordinate 0;
  - the accessories that are **not hidden** in the card's own per-slot
    toggle state (`Status.showAccessory`), and none of the **hair-type**
    ones — both omitted by default, rows included, with report lines saying
    so (§5.9);
  - is byte-reproducible (same input → same SHA-256, because the thumbnail and
    the packed sections are copied/encoded deterministically).

It is **not**:

- a PMX export. A *clothing-only PMX* is already possible today: untick every
  non-Outfit part in the Parts list (or pass `--hide body,head,hair,a,...`
  and `--hide` the accessories on the CLI) and export; excluded parts are
  absent from the PMX entirely (`include_in_export: false`). That route is
  lossy (baked textures, the game's shaders approximated, no round-trip
  through `--clothes`) and is **not** what this feature does — but see
  §11 (Alternatives).
- an in-place edit of the card. Cards are never written. The outfit file is a
  new sibling file; the card on disk is untouched.
- a clothing *library* or browser. No asset management, no catalog, no batch
  mode (see §12 for a possible later step).
- a way to export *face* or *hair* items, or a body. Outfit files carry
  clothing and accessories only; hair is the character's, never the file's.

### The three gaps

| Gap | What exists today | What the feature adds |
|---|---|---|
| Clothing-file **writer** | Full reader (`LoadClothingFile`, `src/card/CardFile.cpp:522`); the reverse (encoder) does not exist | `BuildClothingFileBytes()` + `ExportClothingToFile()` re-encoding one coordinate |
| CLI surface | `convert --clothes <file.png>`, `inspect <outfit.png>` (works on outfit files already, `cli/main.cpp:308-345`) | new `cardbridge clothes <card.png>` subcommand |
| Viewer surface | `Export PMX` button (`viewer/Panel.inc:20`), save-dialog pattern (`viewer/Pose.inc:62-76`) | new **Export clothing…** button + save dialog + log lines |
| Round-trip proof | Applying an outfit file is unit-tested | write→read equality tests + CLI smoke + optional in-game check |

No library and no mods folder is needed: everything is re-packed from the card
itself. The size of the output is the size of the source card plus a few
percent (embedded textures dominate, and they are copied, not re-encoded).

---

## 2. What CardBridge does today (verified against source)

All references were read, not inferred. Line numbers are from `master` @
`0d26d18`.

### 2.1 Reading an outfit file

`LoadClothingFile` (`src/card/CardFile.cpp:522-622`) establishes the exact
on-disk layout:

```
<PNG image>                              (any valid PNG; the bytes after IEND are the payload)
int32 productNo                          (little-endian; reader ignores the value)
dotnetString marker                      (UTF-8, 7-bit length prefix: "【KoiKatuClothes】")
dotnetString formatVersion               (free string; reader keeps it, interprets nothing)
dotnetString name                        (the outfit's own display name)
int32 coordLen                           (bytes of the one coordinate)
coordBytes:
  int32 n + MsgPack  →  clothes          (map: parts[], subPartsId[], per-part colourInfo patterns)
  int32 n + MsgPack  →  accessory        (map: version, parts[] — 20 + MoreAccessories-appended)
  uint8                enableMakeup
  int32 n + MsgPack  →  makeup           (map; never empty even when disabled)
[optional] dotnetString "KKEx"
  int32 version + int32 size             (or int32 size alone — the reader accepts both shapes)
  extBytes: MsgPack map  pluginId → [version, payload]
```

`decodeCoordinate` (`CardFile.cpp:96-113`) is shared with the character-card
`Coordinate` block — a coordinate inside a card is exactly one `coordBytes`.
The outfit file is therefore a *slice* of a character card's Coordinate
array, wrapped in its own marker/name pair.

`dotnetString` is .NET `BinaryReader.ReadString`: a 7-bit-encoded varint
length, then UTF-8 (`src/core/Bytes.h:83-94`). Writing it is a few lines.

`pngEnd(file)` (`CardFile.cpp:68-83`) finds the first byte after IEND; the
reader validates PNG signature and complete chunks on the way out — the
writer embeds the mannequin icon verbatim (it is already a valid PNG).

### 2.2 Outfit files are self-contained about ids

From `CardFile.h:36-38` and `ApplyClothing` (`CardFile.cpp:642+`):

- a character card stores **author Slot ids** for mod items;
- an outfit file stores **session-local ids** (`LocalSlot` on the Sideloader
  rows);
- the merge translates id-by-id through the file's own resolver rows
  (`localToSlot`, `CardFile.cpp:665-671`) — and, as a special case, pattern
  ids inside `colorInfo` (`CardFile.cpp:688-703`) and `subPartsId`
  (`CardFile.cpp:708-711`) with the same map.

Export is the same translation in reverse (§5.3).

### 2.3 What the outfit import merges (and what it does not)

`ApplyClothing` consumes, per applied outfit file:

| Payload in the file | CardBridge consumes it and… | See |
|---|---|---|
| Clothes + sub-parts + patterns + accessories | ids re-localised Slot↔LocalSlot, overwritten in the target coordinate; slots ≥ 20 arrive via the file's MoreAccessories XML, appended as parts 20+ | `CardFile.cpp:675-717`, `:313-362` |
| Makeup, when the file enables it | overwrites the coordinate's makeup | `CardFile.cpp:715-719` |
| Sideloader rows (`KKEx com.bepis.sideloader.universalautoresolver`) | merged into the character, property paths rewritten to `outfitN.` | `CardFile.cpp:779-815` |
| KKABMX bone mods | union-merged (file wins on a shared bone) | `CardFile.cpp:817-843` |
| MaterialEditor edits (texture property lists + TextureDictionary) | merged, `CoordinateIndex` remapped to the target, tex ids remapped on collision | `CardFile.cpp:844-924` |
| KCOX `Overlays` (garment paint) | added under coordinate N, char's other coordinates kept | `CardFile.cpp:925-1010` |

Deliberately **not** consumed by the import on a clothing file: HairAccessory-
Customizer, DynamicBoneEditor, KSOX (skin paint — an outfit carries no skin),
and anything else in `KKEx`. The exporter uses the same allowlist (§5.5).

### 2.4 Test fixture to extend, not to invent

`tests/cardbridge_tests.cpp` already builds synthetic cards/clothing files by
hand (`mkClothes`, `mkAcc`, `accPart`, around line 1960-2110) and asserts
merge semantics — including pattern id translation
(`outfit0.ChaFileClothes.ClothesTopPattern0`, `tests/cardbridge_tests.cpp:2171-2185`).
The writer's round-trip tests plug into exactly this fixture style; no new
test harness is needed.

---

## 3. Format reference (as written, not just as read)

The writer emits this byte sequence. Everything between the PNG and the KKEx
block is deterministic; nothing is re-encoded on the thumbnail path (§5.2).

```
+-----------------------------+-----------------------------------------------------------+
| section                     | content                                                    |
+-----------------------------+-----------------------------------------------------------+
| PNG (IEND included)          | thumbnail image, see §5.2                                 |
| i32 productNo               | source card's `productNo` (real game files carry 100);    |
|                             | fallback 100. The reader ignores the value; matching the  |
|                             | game exactly costs nothing.                               |
| dotnetString marker         | "【KoiKatuClothes】" (UTF-8 U+3010/U+3011) — always this,  |
|                             | not the Party/Sunshine variants: see §6, marker policy    |
| dotnetString formatVersion  | source card's `formatVersion` (real game files carry      |
|                             | "0.0.0"); fallback "0.0.0". Reader ignores it; cosmetic.   |
| dotnetString name           | the outfit's display name, §5.6                           |
| i32 coordLen                | byte size of coordBytes, little-endian                     |
| coordBytes                  | int32 n + MsgPack clothes   (re-encoded)                  |
|                             | int32 n + MsgPack accessory (re-encoded)                  |
|                             | uint8 enableMakeup                                          |
|                             | int32 n + MsgPack makeup      (re-encoded)                |
| [optional] dotnetString      | "KKEx" — omitted entirely when there is nothing to carry    |
| i32 version + i32 size       | the shape the game writes; the reader also tolerates a      |
|                             | bare size, but we always write version+size                |
| extBytes                     | MsgPack map: pluginId → [version, payload]                |
+-----------------------------+-----------------------------------------------------------+
```

Endianness: little-endian throughout. `dotnetString` = 7-bit varint length +
UTF-8 bytes. `int32` = 4 bytes. No alignment, no padding. `coordBytes` must
end exactly at `coordLen` or the file is invalid; `extBytes` likewise at
`size` — the writer computes both lengths, never guesses.

**Coordinate semantics** (what differencing happens when going card → file):

- Clothes: the source map is re-emitted with ids re-localised (§5.3).
- Accessory: the coordinate chunk carries **at most 20 parts**, with ids
  re-localised (§5.3); outfits with more accessories store slots 20+ in the
  file's MoreAccessories XML, copied verbatim (§5.8). Hidden and hair-type
  accessories are omitted from the file (§5.9).
- Makeup: `Outfit::makeup` re-encoded unchanged (makeup carries no ids),
  `enableMakeup` copied as the 1-byte flag.
- `version` fields inside the clothes/accessory maps are kept byte-for-byte
  (packed from the parsed Value, so they survive automatically).

---

## 4. API and file layout

New files (kept out of `CardFile.cpp`, which is already 1050 lines):

- `src/card/ClothingExport.h` / `.cpp` — the writer.
- Declared in `card/CardFile.h` beside `ClothingFile` so the header group is
  coherent (or included standalone; final call at implementation).
- Added to `CMakeLists.txt` under the `cardbridge_core` source list.

```cpp
namespace cardbridge::card {

struct ClothingExportOptions {
    int outfit = -1;               // -1: the card's current (Status.coordinateType), like convert
    std::string name;              // empty: default derived from character + outfit, §5.6
    std::string thumbnailPngPath;  // empty: render or use `thumbnailBytes`; empty there too: reuse the card's faceThumbnail
    Bytes thumbnailBytes;           // empty: render via the mannequin path (viewer/--render-thumbnail) or fall back to the face thumbnail
    bool withAbmx = false;         // KKABMPlugin.ABMData verbatim; default off, §5.5
    bool includeHidden = false;    // keep accessories the card hides (showAccessory==0); default off (§5.9)
    bool withHairAccessories = false;  // keep hair-type accessories (type 122/123); default off (§5.9)
    std::set<int> hiddenSlots;          // extra accessory slots to omit (viewer untick / CLI --hide), §5.9
};

struct ClothingExportReport {
    std::string outfitName;
    int outfit = -1;
    int clothesIdsLocalized = 0;   // author Slot → LocalSlot rewrites (clothes + subs + patterns)
    int accessoryIdsLocalized = 0;
    int subPartsLocalized = 0;
    int uarRowsKept = 0;
    int meListRows = 0;            // MaterialEditor rows rebased to coordinate 0
    int meTexturesKept = 0;        // TextureDictionary entries that survived the subset filter
    int kcoxGarments = 0;          // overlays carried over
    int extraAccessoriesKept = 0;  // accessories beyond slot 20 carried in MoreAccessories XML (§5.8)
    int hiddenAccessoriesDropped = 0;  // slots the card hides (or the user unticked), emptied in the file (§5.9)
    int hairAccessoriesDropped = 0;    // hair-type accessories omitted by default (§5.9)
    std::vector<std::string> droppedPlugins;   // KKEx keys omitted, by name — nothing is silent
    std::vector<std::string> warnings;         // unresolved/unknown ids written raw, etc.
};

// Packs one coordinate into a standalone outfit file. Throws FormatError /
// CardError (ClothesFile/family checks) like the reader, and FormatError when
// the coordinate claims extra accessories with no backing MoreAccessories XML
// (corrupt in-memory card; never a silent drop, see §5.8).
Bytes BuildClothingFileBytes(const Card& card, const ClothingExportOptions& opt,
                             ClothingExportReport* report = nullptr);

// Writes to disk. Refuses to overwrite the source card (out == card.path)
// and refuses to overwrite an existing file unless `overwrite` is true.
ClothingExportReport ExportClothingToFile(const Card& card, const std::string& outPath,
                                          const ClothingExportOptions& opt, bool overwrite = false);
```

The pure `BuildClothingFileBytes` split from the file writer is what makes the
round-trip tests trivial (assert on bytes, then `LoadClothingFile` the same
bytes via a memory-loaded path or a temp file — tests already round-trip
through temp files where needed).

---

## 5. Design

### 5.1 Which coordinate, and which state of it

- **Slot selection:** `--outfit N` / the on-screen slot; default is
  `Status.coordinateType` (the same default `convert` uses, `cli/main.cpp:375-376`).
  Out of range → `FormatError("the card has N outfits")`, like `ApplyClothing`.
- **Viewer session with an applied outfit file** (`--clothes` / drag-and-drop):
  the export writes the *merged* outfit — exactly what the user sees, mirroring
  `StartExport()` which exports the on-screen scene (`viewer/Export.inc:10-83`).
  The log and the report name the provenance (`from <clothingName>`), the same
  way the manifest records `added_outfit`.
- **Clothes state:** the card's `Status.clothesState` (half-off clothes) is
  *not* applied — exporting writes the stored ids, consistent with how the
  rest of CardBridge treats the state as unapplied. Documented, not silently
  resolved.
- **Pinning:** if an import is pinned to slot K, exporting slot K writes the
  merged outfit; exporting another slot writes that slot's own, untouched
  clothes (same pinned semantics as the viewer shows).

### 5.2 The PNG the outfit file carries

Reader doesn't care about the image; the game shows it as the outfit's icon.
Policy (current, shipped):

1. **The icon is a faceless, bald mannequin wearing exactly what the file
   carries** — a render through the same software path the preview uses
   (`exporter::RenderOutfitIcon`). Bald, because an outfit file never carries
   the character's hair; grey-flat, because skin is not the outfit. This is the
   same convention Koikatsu itself uses for its own coordinate .png files
   (verified on real game-produced files: a bald grey mannequin in the outfit,
   no face, on the preview-coloured background). Deterministic: same card +
   same outfit → same bytes.
2. `--thumbnail <file.png>` CLI / an option in the viewer's save dialog: use
   the provided image (validated: PNG signature + `pngEnd` succeeds, so a JPEG
   with a renamed extension is rejected with a clear error rather than
   silently embedded).
3. `--render-thumbnail` CLI: assemble the scene like `convert` does (the same
   `--library` / `--mods` requirement) and render the mannequin icon (1).
   The viewer's **Export clothes** button always does (1).
4. Face thumbnail (`Card::faceThumbnail`) remains the fallback path only when
   neither thumbnail option is set and rendering is not available — kept for
   `BuildClothingFileBytes` callers with no scene at all.
5. `--with-hair-accessories` keeps the hair on too: hair-type accessories
   (122/123) and the character's own hair show on the mannequin (and enter the
   file), off by default.

### 5.3 Re-localising ids (author Slot → LocalSlot)

Build `slotToLocal` from `card.rows`, mirroring the importer's `localToSlot`
(`CardFile.cpp:665-671`), but keyed by `slot`:

```text
for rr in card.rows
    if rr.property starts with  "outfitN."  and rr.localSlot != 0
        slotToLocal[rr.slot] = rr.localSlot   (first wins; rows are unique per property anyway)
```

Apply it to every id stored in the coordinate:

| Carried id | Where it lives | Re-localised |
|---|---|---|
| clothes ids | `clothes.parts[i].id` (i < 9) | yes |
| jacket sub-parts | `clothes.subPartsId[*]` | yes |
| colourInfo patterns | `clothes.parts[i].colorInfo[j].pattern` | yes (their rows are `…PatternP`, same filter) |
| accessory ids | `accessory.parts[i].id` for i < 20 | yes |
| accessory ids (beyond 20) | MoreAccessories XML (`additionalAccessories`), §5.8 | yes (attribute `id`, same slot→local map) |
| accessory pattern ids | accessories have none (patterns are clothes-only) | n/a |
| makeup ids | none | n/a |

An id with no matching row is **base game** (or unresolved): written as-is.
If it *looks* like a mod id (large, and another row references it) with no
resolver row, that means the card was never resolved on this machine — the
report adds a warning (`"id <n> has no resolver row; written raw"`) so the
output is never silent about a suspicious id. The id is still written raw —
guessing a localSlot is worse.

Hair accessories (type 122/123, `IsHairAccessoryPart`) are **excluded by
default** (§5.9) — an outfit file is the outfit, not the hair; the file never
carries the character's hair slots, and by extension not the hair worn as
accessories, unless the user opts in with `--with-hair-accessories`.

### 5.4 Resolver rows (`KKEx` Sideloader block)

- Source: `card.extended["com.bepis.sideloader.universalautoresolver"]`, shape
  `[version, {"info": [bin, bin, ...], ...}]`.
- Filter: keep rows whose property starts with `outfitN.`.
- Rebased: strip the `outfitN.` prefix (`outfit2.ChaFileClothes.ClothesTop` →
  `ChaFileClothes.ClothesTop`; `outfit2.accessory8...` → `accessory8...`),
  exactly the inverse of `withOutfitPrefix` (`CardFile.cpp:423-426`).
- **Accessory rows of any index are kept** — except those for slots omitted by
  §5.9 (they addressed a slot the file does not provide), and those ≥ 20 are
  re-numbered to match the compacted XML extras (§5.8/§5.9).
- `ModID`, `Slot`, `LocalSlot`, `CategoryNo` are copied verbatim — the ids the
  coordinate itself now stores are written straight from `slotToLocal`, so the
  pair (id in parts, LocalSlot in the row) is always consistent.
- The entry's version (index 0) and any sibling keys in the UAR map are kept
  verbatim — only `info` is rewritten. If nothing survives the filter, the UAR
  key is omitted from the output KKEx entirely.
- If the card has no UAR block at all, one is created as `[0, {"info": [...]}]`
  only when there is at least one row to carry; otherwise no KKEx block is
  written. The version value is copied from the existing UAR entry when one
  is present; otherwise 0 — which is what the game writes for outfit files
  (verified, Phase 0).

### 5.5 Other plugin payloads — explicit allowlist, nothing silent

| Plugin key | Handling on export |
|---|---|
| `...universalautoresolver` (Sideloader) | rebased, §5.4 |
| `com.deathweasel.bepinex.materialeditor` | subset + rebase, §5.5.1 |
| `KCOX` (KoiClothesOverlayX) | subset + re-key, §5.5.2 |
| `KKABMPlugin.ABMData` (ABMX) | **excluded by default**; `--with-abmx` copies verbatim, with a warning that applying the outfit then re-applies the whole character bone-mod table (which is not outfit-specific) |
| `Additional_Card_Info` (CoordinateInfo `_acckeep`/`_hairkeep`) | copied verbatim if present (small, optional, coordinate-relative) |
| `KSOX` (skin paint) | **dropped** — an outfit file carries no skin; carrying it would repaint the wearer's body |
| HairAccessoryCustomizer, DynamicBoneEditor and anything else | **dropped**, same reason the import does not consume them; the report lists each dropped key by name |

Nothing is dropped silently: `ClothingExportReport::droppedPlugins` names every
key omitted, and the CLI prints it (`dropped plugin blocks: KSOX, ...`).

#### 5.5.1 MaterialEditor

Source lists live in the ME payload map (`TextureDictionary`,
`MaterialTexturePropertyList`, `MaterialFloatPropertyList`,
`MaterialColorPropertyList`, `MaterialShaderList`, `MaterialNamePropertyList`,
`MaterialKeywordPropertyList`, `MaterialNamePropertyList`,
`ProjectorPropertyList`, `RendererPropertyList`) — the same names
`ApplyClothing` rebases (`CardFile.cpp:891-910`).

Algorithm:

1. For each list: parse the bin; keep rows with `CoordinateIndex == N` and
   `ObjectType ∈ {1 (clothing), 2 (accessory)}` (hair/character/studio rows are
   not garment-scoped — dropping them is the point; a hair row repainted the
   wearer's hair). Drop `ObjectType == 2` rows whose `Slot` is an omitted
   accessory (§5.9) — an empty or removed slot in the file must not repaint a
   real accessory on the target character. Accessory rows with `Slot ≥ 20`
   address the MoreAccessories extras (§5.8) and are re-numbered on compaction
   (§5.9) when an extra is omitted.
   Rewrite kept rows' `CoordinateIndex` to
   `0`. Keep the rest of the row verbatim
   (`MaterialName`, `Property`, `Scale`/`Offset`, `Slot`, …).
2. `TextureDictionary`: keep only entries whose id appears as `TexID` in the
   kept texture rows (`-1` allowed = tiling-only, consumes no id). Ids are
   **not** remapped — kept ids are unique by construction in a subset of one
   dictionary, so no collision machinery is needed (unlike the merge path,
   which collides two dictionaries).
3. Re-encode: each list stays a bin of an array of maps; `TextureDictionary`
   stays a bin of a map. If everything was dropped, the ME key is omitted
   from the output KKEx entirely (no empty payload).
4. `TexID` references that survive are guaranteed present — the subset filter
   is driven by the rows, so no dangling `TexID` can leak into the file.

#### 5.5.2 KoiClothesOverlayX

Source shape (character): `{coordinate → {garment → [png, replace]}}` (a bare
`{garment → …}` also occurs in the wild and is read the same way,
`MaterialEdits.cpp:92-102`). Export:

1. Take the sub-map for `CoordinateIndex == N` (or the whole bare form if the
   card uses it).
2. Write the **bare** form `{garment → [png, replace]}` as `Overlays` —
   this is the clothing-file form the importer handles directly
   (`CardFile.cpp:944-955`), with no coordinate level to interpret.
3. Keep the source `KCOX` entry's version (default 1) and payload shape
   `[version, {"Overlays": bin}]`. If empty, omit the key.
4. Skin-sounding keys inside KCOX (there are none in practice — garment object
   names are `ct_*`) are not special-cased; nothing is guessed.

### 5.6 Name, and file name for the default output path

- Outfit file's `name` string: `--name` wins; otherwise
  `"<Character displayName> - <outfitName(N)>"` using `Card::displayName()` and
  the game's slot names (`Card::outfitName`, `CardFile.cpp:45-63`). ASCII-safe
  in practice; non-ASCII passes through as UTF-8 regardless.
- CLI default `--out`: `<card stem> - <N+1>.png` next to the card's folder —
  or, like `convert`, simply *derive from the character* and print the path.
  Exact spelling is an implementation detail; `--out` stays explicit and
  recommended in docs.
- Viewer: the save dialog pre-fills `<Character> - <OutfitName>` in the
  viewer's `exportDir` when set (same convention as `PickExportParent`,
  `viewer/Export.inc:4-8`), else asks; filter `*.png`, `OFN_OVERWRITEPROMPT`.
- `Shift+E` mirrors `E` ("Export PMX"); registered next to it in
  `viewer/Window.inc` (~line 1084); `C` stays "clear log".

### 5.7 Determinism and safety

- **Deterministic:** pure function of the loaded `Card` *and* of the scene the
  icon renderer draws (grey mannequin + the outfit, fixed cameras/lights/
  background — the same renderer that draws `out/preview*.png`). `MsgPackPack`
  is a pure function of the `Value`, map order is preserved — two runs on the
  same card produce the same SHA-256. Tested (§8, test T7).
- **Never overwrites the source card:** `ExportClothingToFile` compares
  `_fullpath`s and throws when they match. With `--overwrite` absent, an
  existing target is an error (`exit 3`), not a silent clobber.
- **Never writes a bad file partway:** pack to memory first, then
  `fileio::WriteFile` once; on any earlier failure nothing is created.

### 5.8 Accessories beyond slot 20 (MoreAccessories)

A coordinate stores 20 accessory slots in the outfit block itself; anything
past that lives in the **MoreAccessories** plugin's XML, stored in
`KKEx.moreAccessories.additionalAccessories`. Phase 0 established what the
**game** actually writes, in `KKCoordeF_20251115143454517.png` (a real modded
outfit file) and `KKCoordeF_20250430234238627.png`:

```xml
<additionalAccessories version="2.0.21.1"><accessory type="120" /><accessory type="120" /></additionalAccessories>
<additionalAccessories version="1.1.0.5" />
```

Flat `<accessory …>` elements directly inside `<additionalAccessories>`,
positional: **slot 20 + n is the n-th element**; an unused extra slot is
written as a bare `<accessory type="120" />`; there is **no `<accessorySet>`
wrapper** in a coordinate file (the `<accessorySet type="N">` form exists only
inside a character card, where 7 coordinates share one XML). Element
attributes carry `type`, `id`, `parentKey`, `hideCategory`, `noShake`,
`addMove*`, `color*` (empty type-120 placeholders carry no attributes at
all).

Character cards, per CardBridge's `appendMoreAccessories` (`CardFile.cpp:133-180`),
group per-coordinate with `<accessorySet type="N"> … </accessorySet>` (cards
share one XML between 7 outfits; the wrapper says which outfit owns which
extra). Both shapes carry the same `<accessory>` elements.

**Decision: copy the source XML verbatim, never re-serialise it.**
Re-generating the XML from the parsed parts would require guessing the game's
number formatting (culture/precision) and attribute layout — exactly the kind
of invented-byte-format risk this project avoids. The verbatim copy has no
such risk, and works for both observed card shapes:

1. From `card.extended["moreAccessories"].at(1)["additionalAccessories"]`
   (a string), select the extras of the coordinate being exported:
   - **Shape A** (`<accessorySet type="N">` present): take the elements of
     the block whose `type="N"`.
   - **Shape B** (flat elements carrying a coordinate-ish attribute, e.g.
     `<accessory coordinate="N" …>`): take the elements whose coordinate
     attribute equals N. (Unverified in the wild; kept defensive.)
   - **Neither** but elements exist → `FormatError` naming the shape —
     never a silent drop of extras the coordinate claims to own.
2. Rewrite, in exactly those element bytes, only attribute values:
   - each `<accessory>` element's `id="…"` attribute through the same
     slot→local map as §5.3 (ids are author Slots in the card, must become
     LocalSlots in the file — the importer applies the same local→slot
     translation to them, `CardFile.cpp:728-729`). Unknown ids are kept raw
     with a warning, same policy as parts;
   - when the card's extras include elements whose id re-localises (any
     modded extra), that is the only rewrite needed. Everything else —
     `parentKey`, `addMove*`, `color*`, `hideCategory`, `noShake`,
     whitespace, ordering, attribute order — is byte-for-byte the card's own
     elements.
3. The coordinate chunk's accessory parts are truncated to the first 20
   (ids re-localised, §5.3). The extra parts (indices 20+, present in memory
   because the card already baked them in) go **only** into the XML copy.
4. Write the file's XML from the selected elements, as
   `<additionalAccessories version="<version attr from the source XML when
   present, else omitted>">` + elements + `</additionalAccessories>`
   (self-closing when the source had none / no version) — the exact shape the
   game writes and the importer consumes, so applying the written file
   reconstructs parts 20+ in memory through the same code path.
   - Empty extras (`<accessory type="120" />`) are preserved as empty — the
     game itself writes them and expects positional slots.
5. Resolver rows with `accessory<index>` for index ≥ 20 are kept (§5.4), and
   MaterialEditor accessory rows with `Slot ≥ 20` are kept (§5.5.1) — their
   `Slot` already matches the XML element's slot index (20 + position), so no
   remap is needed.

**Required companion fix (same PR, tests in the same build):**
`appendMoreAccessoriesToOutfit` (`CardFile.cpp:315-362`) currently ignores
flat outfit files — no `<accessorySet>` means `coordinate` stays at −1 and
every `<accessory>` element is skipped (silently dropping the extras
`KKCoordeF_….png` carries). This is wrong for game-produced files, not just
for ours. Fix: if no `<accessorySet>` wrapper is seen, treat all
`<additionalAccessories>` children as the single coordinate (coordinate 0).
One `if`, plus a unit test fed with the real XML shape above.

**Defensive failure mode (never a silent drop):** if the in-memory coordinate
holds parts with index ≥ 20 but the card carries no usable `additionalAccessories`
XML (or the shape is unrecognised), the export throws `FormatError` naming
counts and the offending slot indices — a corrupt or hand-built card is
reported, not guessed around. On a normally-saved card this path is
unreachable: parts 20+ exist only because they were parsed out of that XML.

Size is proportional to the number of extra accessories (a KB-ish each);
there is no upper limit. Determinism (§5.7) holds: the substring copy is a
pure function of the source string.

**Round-trip check:** importing the written file applies the extras through
the *same* `appendMoreAccessoriesToOutfit` path this design relies on, so
`card → export → apply-to-another-card` yields the same extra parts as the
source card — verified by test T4 (§8). In-game apply (MoreAccessories plugin
reading the XML) inherits the same shape the game itself writes.

### 5.9 Hidden accessories — and hair-type accessories — are not exported

Two kinds of accessories are excluded from the written file by default. Both
options are on the CLI (`--include-hidden`, `--with-hair-accessories`) and off
unless explicitly asked for; the viewer follows the defaults, plus its own
unticked-items rule (below). Nothing about either exclusion is silent: every
exclusion is counted in `ClothingExportReport` (`hiddenAccessoriesDropped`,
`hairAccessoriesDropped`) and printed.

**Rule 1 — hidden accessories stay out.** The card stores the user's per-slot
show/hide state in `Status.showAccessory` (20 flags, one per accessory slot —
`docs/OFFLINE_FORMAT_FEASIBILITY.md:86`). This is the same "default toggle
state" the PMX export's untick list mirrors. The export treats
`showAccessory[slot] == 0` as **hidden**. Scope note: this applies to
**accessories only — and, by the same mechanism, the viewer's session
untick in the Parts list for accessory slots**. Garments are not dropped on
this flag's reading (it says nothing about garments; `hideOpt`/`hideBraOpt`
/`hideShortsOpt` are unverified, Rule 3), and the viewer's untick of a
*garment* does not silently remove it from the file either — see Open
question 5. The export:

- slots 0–19: the part is written as an **empty part** (id 0, in the same
  slot — no re-numbering of the fixed 20 slots, and the file semantically says
  "this slot is provided empty", so applying the file never paints or forces
  anything into that slot on the target character; the empty-slot merge rule
  applies);
- slots ≥ 20: the corresponding `<accessory>` element is **removed from the
  XML copy** and the remaining extras are compacted (slot 20 + n indexes walk
  in XML order); the re-numbering is propagated to every consumer of accessory
  indices:
  - UAR rows `outfit0.accessory<i>.ChaFileAccessory.PartsInfo.id` → `accessory<i>` with the compacted index;
  - MaterialEditor rows with `ObjectType == 2, Slot ≥ 20` → compacted `Slot`;
  - the extras that were before the removed one keep their slots;
- rows for omitted slots are removed: their UAR rows (`accessory<i>…`) and
  their MaterialEditor rows (`ObjectType == 2, Slot == i`) are dropped, not
  kept inert;
- the card's own `showAccessory` is read defensively: a missing/short/odd
  array hides **nothing** (what the card does not say is not dropped), a `0`
  hides, any other value keeps. If the viewer's session unticked an accessory
  in the Parts list, that slot is omitted too — same rule, same empty-part /
  compaction machinery, via an option-set of hidden slots passed by the
  viewer (and `--hide 3,7,17` on the CLI);

**Rule 2 — hair-type accessories are not exported.** Koikatsu can wear hair
as accessories (front/back/side/extension as type 122/123, and/or the
resolver category 122/123, and/or ids whose mod name matches `hair` —
`IsHairAccessoryPart`, `CardFile.cpp:624-640`). An outfit file never carries
the character's hair slots, and the hair worn as accessories is the same kind
of thing — the wearer's hair, not the outfit. By default those accessories
are omitted, with the same empty-part/compaction mechanics as Rule 1. The
character's own target character keeps applying its own hair after the outfit
lands — exactly the point: an outfit is interchangeable, hair is not. Opt in
with `--with-hair-accessories` (and `--include-hidden` for Rule 1) if the user
really wants the full coordinate.

> Interpretation note: "hair" here means **hair-as-accessories** (the only
> hair an outfit file could carry). The character's actual hair sections
> (`Custom` face/body/hair, incl. `HairBack`/`HairFront`/`HairSide`/
> `HairExtension`) are never in an outfit file at all, by format.

**Rule 3 — nothing else is "hidden" data without semantics we can verify.**
The card also stores `clothes.hideOpt` (per garment), `hideBraOpt` /
`hideShortsOpt` (clothes map), per-accessory `hideCategory` (meaning uninterpreted
anywhere in this codebase) and `Status.clothesState` (half-off clothes). Their
semantics are **not established from the offline format survey** — the
feasibility doc only notes they exist. Dropping a garment on a guessed meaning
of `hideOpt` would be silent data loss by another name; the export therefore
leaves them untouched (they carry over verbatim in the parts, or stay
unread). If/when their meaning is verified (the same phase-2-style evidence
gathering the Stiletto report used), dropping becomes a two-line change and a
doc line, not a redesign — tracked in Open Questions. `clothesState` remains
M7 (never applied, consistent with the rest of CardBridge).

Report/log lines (CLI and viewer):

```
clothing: 2 accessory slot(s) hidden by the card, left out (slots 4, 9)
clothing: 1 hair accessory left out (--with-hair-accessories keeps it)
```

---

## 6. Scope, marker policy and family handling

- **Supported input:** `CardFamily::Koikatsu`, `KoikatsuParty`,
  `KoikatsuPartySP` — the same set `convert` accepts
  (`CardFile.cpp:229-234` gates only Sunshine/EmotionCreators as unsupported).
- **Sunshine / EmotionCreators cards:** rejected with the family-specific
  message and `exit 2` (same as `convert`). Sunshine *outfit files* are also
  rejected (the reader already reports them as unsupported).
- **An outfit file as input:** rejected, `exit 2`, with the existing
  `ClothesFile` error kind.
- **Marker:** always `【KoiKatuClothes】`. No known Party/SP clothing variants
  exist in the reader or in the wild (`familyOf` matches only on the
  `【KoiKatuClothes…` prefix and `isSunshineClothesMarker` on "Sun";
  `CardFile.cpp:539-541`, `:307-311`). Inventing `【KoiKatuClothesSP】` would
  be guessing; the one safe, tested marker for a coordinate file is the
  plain one. If a real Party coordinate file is later found to use another
  marker, this is a one-line change plus a doc update.
- **productNo:** `0`. The reader ignores it and nothing else in the tree
  consumes it; the value has no known meaning we can claim (unlike, e.g., a
  cache-format stamp).
- **formatVersion:** copied from the source card's `formatVersion` (it is
  whatever the game/saver that wrote the card wrote — the most consistent
  value available); if empty, `"1.0"`. Cosmetic either way.

## 7. Known limitations and non-goals (v1)

| # | Limitation / non-goal | Handling |
|---|---|---|
| M1 | **Accessories beyond slot 20** (MoreAccessories) *are* carried: game-flag **flat** `<additionalAccessories><accessory …/>…</additionalAccessories>` elements selected from the card's XML (per-`<accessorySet>` blocks when present, else coordinate-tagged, else a hard error), copied verbatim, per-element `id` through slot→local | Supported with no re-serialisation risk (§5.8), and the required importer fix for the flat shape is in scope. A corrupt in-memory card with parts ≥ 20 and no backing XML fails — `FormatError` naming counts, CLI exit 2; never a silent drop |
| M2 | **ABMX** (bone mods) excluded by default; a skirt-fit that lives in the character's ABMX does not travel with the outfit | `--with-abmx` opt-in (copies the whole table verbatim, with a warning). Deciding which bones are "outfit-scoped" is not possible from the card — guessing is worse than opt-in |
| M3 | **HairAccessoryCustomizer / DynamicBoneEditor / KSOX** payload entries are dropped | Listed by name in `droppedPlugins`; matches the import's behaviour exactly |
| M4 | **Id re-localisation can be incomplete** when a mod item has no resolver row (card saved without universalautoresolver data, or the row was hand-edited) | Raw slot id written, warning in the report; the file loads but that item will fall back to base item N on import |
| M5 | **Jacket sub-parts/patterns when the garment is a base-game item but the pattern is modded** — both are translated by the same slot→local map; if a *base-game* pattern id collides numerically with a mod row's slot | Current behaviour of the importer too (`TranslatePattern` applies the same map). Documented, not special-cased |
| M6 | **Game-side acceptance is unverified** — no Koikatsu install is used by CardBridge, ever. Only structural correctness and CardBridge round-trip can be proven in-repo | §8 Phase 0: byte-compare against one real outfit file; final gate is the maintainer loading the result in-game (manual, §8). MMD/PMX is not involved |
| M7 | **`clothesState` (half-off clothes) is not applied** | Documented; export is the stored coordinate |
| M9 | **`clothes.hideOpt` / `hideBraOpt` / `hideShortsOpt` / accessory `hideCategory`** — the remaining hide flags (per-garment, bra/shorts, per-accessory) — carry their semantics unverified in the offline survey; dropping a garment on a guessed meaning would be silent data loss | Kept verbatim / not applied (§5.9 Rule 3). If semantics are verified later, exclusion becomes a two-line change + doc line. Only `Status.showAccessory` is treated as a hide flag today |
| M8 | **Re-encoding is not byte-identical to a sliced original**: `MsgPackPack` may pick shorter int encodings for values that were stored wide | Semantics are identical (asserted by round-trip tests); byte equality between two exports of the *same* input still holds |

## 8. Phases and verification

### Phase 0 — Spike (done — against the user's own game install)

Reference files used (read-only): `UserData\coordinate\Cute Nurse.png`
(151,262 bytes, no KKEx — base-game outfit), `Casual Clothing.png` (same),
`KKCoordeF_20251115143454517.png` (190,919 bytes, modded coordinate
"Mafuyu", KKEx 42,046 bytes) and `KKCoordeF_20250430234238627.png`
(15,299,379 bytes, MaterialEditor-bearing modded coordinate).

Findings (each changed a detail of the plan, all folded back in):

- **productNo 100** and **formatVersion `0.0.0`** on real files — writer
  copies the source card's values, falling back to these (§3).
- **Marker** exactly `【KoiKatuClothes】` ✓; the payload is the flat
  byte sequence in §3, and the existing reader handles the real files
  (`cardbridge inspect` parses all of them).
- **KKEx** written as `dotnetString "KKEx"` + `int32 version` + `int32
  size`; UAR entry index 0 is `0`; rows carry extra keys (`Author`,
  `Website`, `Name`) — verbatim copy preserves them (§5.4).
- **Resolver rows are unprefixed** in outfit files
  (`ChaFileClothes.ClothesShoesInner`, `accessory5.…`) — exactly what the
  exporter writes after stripping `outfitN.` (§5.4).
- **KCOX**: `[1, {"Overlays": bin}]`, bin unpacks to the **bare** shape
  `{garment → [png, replace]}` (e.g. `ct_clothesTop` → 1,024×1,024 RGBA
  PNG) — the §5.5.2 choice is game-correct.
- **moreAccessories** in coordinate files is **flat** —
  `<additionalAccessories version="…"><accessory type="120" />…</additionalAccessories>`
  — no `<accessorySet>` wrapper; empty extras appear as bare
  `<accessory type="120" />`; two real files carry them. Cards may keep
  the `<accessorySet type="N">` wrapper (shape A). Both shapes are handled
  in §5.8, plus a required importer fix for the flat shape.
- **Additional_Card_Info** (`CoordinateInfo`) is a bin of msgpack with
  `_acckeep`/`_hairkeep` arrays — empty in the game file; copied verbatim.
- Modded outfit files also carry `PluginListTool`, `MigrationHelper`,
  `com.deathweasel.bepinex.pushup`, `com.deathweasel.bepinex.hairaccessorycustomizer`
  and `org.njaecha.plugins.dbde`; none are consumed by CardBridge's import
  either, so the allowlist drops them with report lines (§5.5) — in-game
  apply will lose those (like ABMX, this is symmetric; see §12 for a future
  `--allow-plugin` escape hatch).
- **`Status.showAccessory` remains unverified** (no card with a hidden
  accessory was dumped) — §5.9 Rule 1 stays defensive.

### Phase 1 — Writer core + round-trip tests

- `ClothingExport.{h,cpp}` (§4), `CMakeLists.txt`.
- Tests in `tests/cardbridge_tests.cpp` (extending the existing fixture
  helpers `mkClothes`/`mkAcc`/`accPart`, ~line 1960):
  - **T1** round trip: build card → export → `LoadClothingFile` → assert
    name, clothes ids, pattern ids, subPartsId, accessory ids, makeup bytes,
    enableMakeup, UAR rows (property-stripped), KCOX garments, ME rows.
  - **T2** id translation both directions: author-slot ids become
    `LocalSlot`s on write; `LocalSlot`s become slots on apply.
  - **T3** material editor rebase: `CoordinateIndex` 3→0, `ObjectType 1/2`
    kept, hair row dropped, texture dictionary subsetted to referenced ids,
    no dangling `TexID`.
  - **T4** more than 20 accessories round-trip: build a card whose coordinate
     carries extras via a `moreAccessories` XML fixture (the same string shape
     `appendMoreAccessories` consumes), export, re-apply with `ApplyClothing`,
     and assert the extras land at the same indices with ids re-localised;
     plus the defensive failure: a corrupt in-memory card with parts ≥ 20 and
     no backing XML → `FormatError` naming the counts; nothing is written.
  - **T5** guards: outfit file as input → `ClothesFile`; Sunshine card →
    unsupported; bad outfit index; `--out` == source path rejected.
  - **T9** hiding and hair exclusion: a card with `Status.showAccessory`
    hiding slots 2 and 21 (one below 20, one in the XML extras) → the
    slot-2 part is an empty part in the file, the slot-20 element is gone
    from the XML, later extras shift to 20, 21…, rows/ME re-numbered the same
    way; rows for the omitted slots are dropped; a hair accessory (type 123)
    is excluded by default, kept with `--with-hair-accessories`; a missing or
    short `showAccessory` array hides nothing; a viewer-provided
    `hiddenSlots` set empties its slots.
  - **T6** KKEx omitted cleanly when everything was dropped; UAR version/sibling
    keys survive verbatim when some rows remain.
  - **T7** determinism: two exports → identical bytes (SHA-256 equal).
  - **T8** byte layout: PNG signature, `pngEnd` finds the payload, marker is
    `【KoiKatuClothes】`, `coordLen` covers exactly the coordinate, trailing
    KKEx parses.
- `cardbridge_tests` must stay at 0 failures (CI/baseline: 1021 checks per the
  Stiletto report; this feature adds, never removes).

### Phase 2 — CLI

```
cardbridge clothes <card.png> [--outfit N] [--name <name>]
                              [--out <file.png>] [--thumbnail <file.png>] [--render-thumbnail]
                              [--with-abmx] [--json] [--overwrite]
                              [--hide 3,7,17] [--include-hidden] [--with-hair-accessories]
```

- `--hide a,b,c` omits the named accessory slots (additive to the card's own
  hidden state); `--include-hidden` keeps what the card hides;
  `--with-hair-accessories` keeps hair-type accessories **in the file and on
  the mannequin icon** (default: out, §5.9 / §5.2).
- The icon is the faceless mannequin by default (§5.2/1); `--render-thumbnail`
  assembles the scene like `convert` does (`--library` required, `--mods` or
  `--no-mods`) and renders it; without it the card's face thumbnail is used.
  The viewer's button always renders the mannequin icon.
- Prints (to stdout, and as JSON with `--json`): the written path, size,
  outfit index/name, counts from `ClothingExportReport` (including hidden and
  hair-accessory omissions), warnings, dropped plugin keys — the same shape
  `applyClothing` reports (`cli/main.cpp:299-304`).
- Usage/help text block next to `convert`'s (`cli/main.cpp:103-130`), plus a
  line in the header comment (`cli/main.cpp:1-9`) and `boolFlags` additions
  (`"overwrite"`, `"with-abmx"`, `"include-hidden"`, `"with-hair-accessories"`).
- Exit codes: `0` written; `1` usage; `2` supportedness/family/corrupt-card;
  `3` write failure (target exists, source unreadable, pack failure).
- **Free win:** `cardbridge inspect <written file.png>` prints its name,
  ids and resolver rows (`cmdInspect`, `cli/main.cpp:308-345`) — the smoke
  test is one existing command.
- **End-to-end smoke (manual, scripted in `tools/`):**
  `convert card.png --clothes <written.png> --out <dir>` on the
  `outfit2` test fixture card and compare the resulting manifest's part tree
  against the baseline (the merge path is already unit-tested; this catches
  writer bugs the unit tests miss).

### Phase 3 — Viewer

- New **Export clothing…** button: placed next to `Export PMX` in the toolbar
  (`viewer/Panel.inc:18-25`), `L != nullptr && !g.busy && !g.exporting` to enable;
  `Shift+E` next to `E` in `viewer/Window.inc` (free today — only
  Ctrl+Shift+Z/Y are bound, for redo).
- Save dialog modelled on `PosePathFromUser` (`viewer/Pose.inc:62-76`):
  `CARDBRIDGE_CLOTHES_PATH` env override for scripted runs (pose/material file
  convention), `OFN_OVERWRITEPROMPT`, filter `*.png`, default path from the
  viewer's `exportDir` when set (`g.settings.exportDir`,
  `viewer/Util.inc:56`), default file name `<Character> - <OutfitName>.png`.
- The handler is `StartExportClothing()` in `viewer/Export.inc` (next to
  `StartExport`): takes a copy of the loaded card, applies the session's
  unticked slots as `hiddenSlots` (the same untick source the PMX export
  reads, `ApplyPartStateForExport`), runs `ExportClothingToFile`
  synchronously (it is a memory pack + one write; the file dialog is the slow
  part), and logs the path + counts like `SavePoseFile` does — including the
  hidden-accessory and hair-accessory omission lines. When the merged-outfit
  case applies (`hasClothing`), the log says so and names the source file —
  mirroring how `ApplyClothing` logs.
- The viewer follows the §5.9 defaults (hidden omitted, hair omitted) with no
  extra checkboxes in this plan; `--with-hair-accessories` /
  `--include-hidden` exist on the CLI for users who need them.
- The outfit row keeps its current pinned semantics; **no new list UI** is
  added (the export targets the on-screen slot; that is stated in the log
  line and the help text).
- Statusbar/header untouched; `ALL`/`Issues`/`MMD` indices not renumbered
  (only appended in `Lay`).

### Phase 4 — Close-out

- README: new paragraph under “Clothing files (outfit files)” (§ Clothing
  files, line ~148) — inverse of the import, with the one-liner
  `cardbridge clothes …` and the viewer button; the
  `supported inputs` table row gains “also writable”; the CLI example block
  (`README.md:252-260`) gains one line; the “Known limitations” section loses
  any wording implying outfits are read-only; the milestone checklist (the
  source of truth at the bottom of the README) gains the entry.
- `docs/` this file's status → `implemented`, with verification results
  (checks added, Rin/Nurse round trip, the maintainer's in-game load result
  if performed).
- `VERSION` + `Open CardBridge Viewer.bat` unaffected; bump `VERSION` per
  release practice (proposal: `0.2.2-alpha`) in a separate commit.
- Snapshot push (per the repo's push-snapshots rule) once the tree builds
  clean and `cardbridge_tests` is green.

## 9. Rejected alternatives

- **Write the coordinate back into the source card.** Violates the project's
  never-write-user-files rule (`README`, `CardFile` — the card is only ever
  read). Outfit files exist precisely as the sanctioned side channel.
- **Export a clothing-only PMX as “the” answer.** Possible today via `--hide`
  (confirmed: excluded parts are absent from the PMX and the manifest);
  lossy, one-way, useless for re-applying to another character. Useful as a
  separate feature; explicitly out of scope here, noted in §11.
- **FBX/glTF export of a garment.** No writers exist in the tree; new
  dependency; needs a rigging story; not what was asked; revisit after PMX
  clothing export ships (§12).
- **Passing unknown plugin blocks through verbatim by default.** Real outfit
  files carry `PluginListTool`, `MigrationHelper`, `pushup`,
  `hairaccessorycustomizer` and `DynamicBoneEditor`; CardBridge's import does
  not consume them either, and `hairaccessorycustomizer`/DBDE are keyed to
  accessory slots we compact/omit — passing them verbatim would apply stale
  settings to wrong slots. Dropped with report lines; a future
  `--allow-plugin <key>` escape hatch is the follow-up (§12).
- **Re-serialising the MoreAccessories XML from parsed parts.** Requires
  guessing the game's number formatting and attribute layout — invented bytes
  where the card itself already holds a verbatim, loadable XML string. The
  verbatim-copy design (§5.8) is why accessories ≥ 20 are *in* this plan
  rather than an error.
- **Keeping `<slot20+>` rows while dropping the parts.** Dangling Sideloader
  rows that re-localise to slots that do not exist: rejected; rows and parts
  travel together (§5.4/§5.8).

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Game rejects the file despite correct layout | low–medium | the headline feature does nothing in-game | Phase 0 reference file; maintainer in-game smoke (manual); everything else (round-trip, PMX re-apply) still works |
| `KCOX` bare form rejected by the game | low | garment paint lost on in-game apply | importer already accepts both forms; write bare (§5.5.2); if Phase 0 shows the game expects `{0: {…}}`, emit that instead — one line |
| UAR version index 0 looks wrong | low | apply failure or extended-save complaint | Phase 0 verified: the game writes 0 and versions vary by plugin; the writer copies the source card's version verbatim (0 fallback) |
| Texture id collisions on some card | very low (subset of one dictionary) | paint applied to the wrong material | T3 covers; ids kept verbatim, never renumbered |
| Viewer layout regression (toolbar crowding) | medium | ugly/broken panel | button appended in `Lay` with its own hit rect; visual check per `viewer_smoke.ps1` |

## 11. The other reading, for the record

If “export a clothing” turns out to mean **"get the garments out as a 3D model"**
rather than "as an outfit file", the answer is already partly in the tree:
`--hide`/`include_in_export` removes everything but the outfit from the PMX,
and the manifest keeps the toggles. A proper “clothing-only PMX” (a standalone,
re-bindable garment model, or a Project K `.kmap`-importable outfit asset) is
a different feature with its own open questions (skeleton sharing across
characters, garment-only weight cleanup, texture bundling for Project K's
content pipeline) and should be planned separately if wanted — it does not
gates, conflict with, or depend on this plan.

## 12. Possible follow-ups (not this plan)

- **`--allow-plugin <key>`** escape hatch: carry an unknown KKEx block
  verbatim for users who want e.g. pushup/DynamicBoneEditor settings in the
  round trip (slots unchanged by omissions only; rejected when slots were
  renumbered).
- **Batch export**: every outfit of a card → one file each (`--all-outfits`),
  after the single-outfit path is proven.
- **Batch export**: every outfit of a card → one file each (`--all-outfits`),
  after the single-outfit path is proven.
- **Clothing import→export round-trip fuzz**: apply many real outfit files,
  export, re-apply, diff — as a regression tool (`tools/`), not a test gate.
- **Manifest siblings**: none planned; the outfit file is its own truth.

---

## Open questions (for the implementer / maintainer)

1. **Phase 0 file:** which real outfit file should be the reference dump —
   the Nurse outfit (per the tests), or another? If there is one with
   MoreAccessories (slots past 20), use it as well to confirm the §5.8 shape,
   and a card with a maker-hidden accessory to pin down `showAccessory`
   (20 flags, slot order, 0 = hidden — `OFFLINE_FORMAT_FEASIBILITY.md:86`) —
   §5.9 Rule 1 reads it defensively until that's confirmed.
   (Only needed for §6/§5.4/§5.8/§5.9 fine print, not for the architecture.)
2. **`hideOpt` / `hideBraOpt` / `hideShortsOpt` / `hideCategory` semantics:**
   do you know what these mean (per-garment hide, bra/shorts hidden state,
   accessory hide category)? The offline survey never decoded them; the plan
   keeps them verbatim and does not drop anything on a guessed meaning (§5.9
   Rule 3, M9). If you can point at a card saved with the bra hidden or an
   accessory hidden through another mechanism, semantics can be pinned in a
   Phase 0-style pass.
3. **`clothesState` (half-off clothes):** should an outfit saved in the
   pulled-off state export the pulled-off garments, or the stored ids (today's
   answer, M7)? The outfit container has no way to carry the state, so this
   would mean dropping those garments from the file — destructive-looking but
   perhaps correct. Your call.
4. **Hair exclusion scope:** §5.9 Rule 2 drops hair-type *accessories* by
   default. If you meant something else (e.g. keep them, drop only hidden
   ones, or drop all accessories the maker considers "hair" including hair-
   coloured hair accessories), say which. `--with-hair-accessories` exists
   as an opt-in either way.
5. **Garments unticked in the viewer (and garment hidden flags):** §5.9 only
   omits *accessories*. If the maker/viewer should also remove a *garment*
   that is unticked on screen (or hidden via some flag, once `hideOpt` is
   decoded), that is a one-paragraph extension of §5.9 — but it is dropping
   authored content, so it needs your explicit call rather than a default
   I chose.
6. **Default `--out` spelling:** `<card stem> - <N+1>.png` next to the card
   (proposed) vs. mandatory `--out` (convert-style, stricter). Either is one
   line; the proposed default just needs confirmation it won't surprise.
7. **`Shift+E`** binding: free in `viewer/Window.inc` today; confirm vs. an
   on-screen affordance alone.

---

## Implementation record

Written after the work landed; the plan above is the design, and this records
how it actually went.

**Phases 0–3 are done.**

- **Phase 0 (real files).** `Cute Nurse.png`, `Casual Clothing.png`,
  `KKCoordeF_20251115143454517.png` ("Mafuyu") and
  `KKCoordeF_20250430234238627.png` (a MaterialEditor-bearing modded coordinate,
  15 MB) from the user's own `UserData\coordinate` were used as reference dumps.
  Findings: outfit files carry `productNo = 100`, `formatVersion = "0.0.0"`,
  **no** KKEx at all when there is nothing to carry (base-game outfits),
  `KKEx` with `int32 version + int32 size` (version 3 in practice, any version
  round-trips), UAR entry version `0` (rows carry extra keys like `Author` /
  `Website` / `Name` verbatim), rows are **unprefixed** (`ChaFileClothes.ClothesTop`,
  `accessory8.ChaFileAccessory.PartsInfo.id`), KCOX `Overlays` is the **bare**
  `{garment → [png, replace]}` shape, and `moreAccessories` in a coordinate
  file is the **flat** form
  `<additionalAccessories version="…" ><accessory …/>…</additionalAccessories>`
  — not the `<accessorySet>` wrapper cards use. One more real card (the
  Himekawa Akeno sample) exposed that the game **double-writes** extras past
  slot 20 in the current version (the coordinate chunk carries 24 parts *and*
  the XML carries the same 4 — identical content).
- **The duplicate-extras loader bug**, unrelated to this feature but found by
  it and fixed with it: `appendMoreAccessories` (character path) appended the
  XML's elements to a parts array the game had already extended — every extra
  accessory appeared **twice** (the resolver listed each twice; conversions of
  such cards carried each extra twice). The loader now appends only as many
  elements as needed to bring a coordinate to `20 + its XML count`. Same
  correction applied to the outfit-apply path. This is recorded here so the
  snapshot carries the context: pre-existing defect, not part of the plan,
  fixed because both this feature and "items/cards with MoreAccessories" need
  it.
- **CardBridge's outfit importer silently dropped game-written extras** —
  `appendMoreAccessoriesToOutfit` required `<accessorySet type="0">` and the
  game writes the flat form — fixed to accept the flat shape (all elements are
  the single coordinate), plus the same double-write guard.
- **Writer** (`src/card/ClothingExport.{h,cpp}`): `ClothingExportOptions`
  (`outfit`, `name`, `thumbnailPngPath`, `thumbnailBytes`, `withAbmx`,
  `includeHidden`, `withHairAccessories`, `hiddenSlots`), `BuildClothingFileBytes`,
  `ExportClothingToFile`. The icon is the faceless grey mannequin
  (`exporter::RenderOutfitIcon`, the same software renderer as the preview);
  the `KeepThumbnail` flag was dropped — the thumbnail comes from the caller
  (`thumbnailBytes` "rendered mannequin" or `thumbnailPngPath` from the user),
  else from `Card::faceThumbnail`.
- **CLI**: `cardbridge clothes <card.png> [--outfit N] [--name X] [--out <f.png>] [--thumbnail <f.png>] [--render-thumbnail] [--json] [--overwrite] [--hide a,b,c] [--include-hidden] [--with-hair-accessories] [--with-abmx]`.
  Exit codes: 0 written, 1 usage, 2 unsupported / out-of-range / corrupt, 3 write failure.
  Default output: `<card stem> - <outfit name>.png` next to the card; `--overwrite` refuses to clobber without it; refusing to overwrite the source card itself.
- **Viewer**: a second button in the outfit row — **Export clothes** (Shift+E),
  writes `<Character> - <slot name>.png` via a save dialog (or
  `CARDBRIDGE_CLOTHES_PATH` for scripted runs); unticked accessory slots in the
  Parts list become empty parts; hidden (card `showAccessory`) and hair-type
  accessories are omitted; the log names every omission and every dropped
  plugin block. The icon is always the mannequin render (§5.2).
- **Verification**
  - `cardbridge_tests`: 6 065 checks, 0 failed. The clothing-export suite
    covers: round trip with `LoadClothingFile` + `ApplyClothing` re-apply
    (byte/ids/KCOX/ME/CoordinateIndex-fixups), the hidden-accessory rules
    (slots below and above 20, re-numbering of Sideloader rows and
    MaterialEditor rows, hair excluded), the determinism check, the byte
    layout, and the guards (outfit index, overwrite, Sunshine).
  - Real-card run (private test instance, off-screen): the viewer's
    **Export clothes** button wrote the file; byte-identical to the CLI run on
    the same card; `inspect` round trip reads it back correctly. The icon is
    the mannequin render (§5.2); with `--with-hair-accessories` the hair and the
    hair accessories show on it and in the exported file (verified on the
    Akeno card: 24 accessories carried, twin-tails visible).
  - Determinism: two exporter runs on the same card produce the same SHA-256;
    the viewer and the CLI write identical bytes.
  - Determinism: exported file re-applies to the card it came from via
    `--clothes` (`ApplyClothing` counted "0 slots replaced, 3 accessories added"
    — an idempotent round trip, as expected).
- **Not verified / not done**
  - In-game load of a written file (the game was never run). The container is
    byte-verified against the game's own outfit files; that is the strongest
    thing CardBridge can say without launching Koikatsu.
  - Game acceptance of the renumbered MoreAccessories XML path (the writer
    mirrors the game's shape, but the slot past 20 renumbering has only been
    round-tripped through CardBridge's own reader).
  - `Status.showAccessory` stays unverified against a real card that hides an
    accessory (the reader tolerates the layout either way); the tests use a
    synthetic flag array.
  - `hideOpt` / `hideBraOpt` / `hideShortsOpt` / `hideCategory` /
    `clothesState` semantics remain unverified; they are kept verbatim, never
    dropped on guessed meanings.
  - The `Shift+E` binding and the default `--out` spelling are as proposed
    (unused elsewhere in the codebase; may be confirmed or rebound later).
  - **VERSION was not bumped**; the release cadence is the maintainer's call.

