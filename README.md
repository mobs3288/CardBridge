# CardBridge

<p align="center">
  <img src="image/CB.png" alt="CardBridge logo: a character card and a wireframe 3D model joined by a bridge" width="500">
</p>

**CardBridge** converts Koikatsu character cards into rigged 3D models (PMX) with separately addressable outfit parts, entirely offline and **completely separate from the game**: it reads the card, a CardBridge asset library and the user's mod files, and needs no Koikatsu install to run. It never launches or controls the game.

It is a standalone C++ Windows application built in the same style as the Project K engine and Map Maker. Its core is a UI-free library so the Map Maker can later import its output (or call it) without a second implementation.

> **Status: Milestone 1 done.** Two real cards convert to PMX offline, the result is read back by the engine's own PMX loader, and the acceptance tests pass ([`docs/OFFLINE_FORMAT_FEASIBILITY.md`](../docs/OFFLINE_FORMAT_FEASIBILITY.md), section 12). The output is **recognisable but not yet a faithful likeness**: body proportions, garment colours, jacket sub-parts and the main face features follow the card, but skin textures, lips, face shape, hair colour for some cards, shading and animation do not (see Known limitations). Every export also writes a `_preview.png` (front, side, back) so a result can be judged without a PMX viewer. The checklist at the bottom of this file is the source of truth for what is done.

## Why this project exists

CardBridge exists to make character conversion more reliable, mod compatibility more extensible, and the entire workflow less painful.

Existing exporters run inside the running game, so a conversion needs a launched game, a working plugin set and a matching patch level, and it breaks whenever any of those drift. CardBridge reads the same data from disk instead.

<p align="center">
  <img src="image/meme.jpg" alt="Meme: &quot;All my homies hate that&quot;, captioned &quot;Fuck KKPMX&quot;" width="420">
</p>

*(Humour aside: KKPMX is MIT-licensed, documents useful PMX clean-up rules, and is a reference for this project. See **Prior art** below.)*

## What it does

1. Reads a Koikatsu character card (`.png` with the card payload after the image).
2. Identifies which item IDs the card uses and which are base-game versus mod items, using the card's own Sideloader resolve data.
3. Finds the item's assets in the asset library (base-game items) or in the matching `.zipmod` under the mods folder (mod items), read-only.
4. Assembles body, head, hair, clothing slots and accessories onto the shared skeleton.
5. Writes a self-contained folder: a PMX file, PNG textures, and a `.cardbridge.json` manifest carrying the part tree, stable part IDs, per-part export inclusion, material ranges and validation status.

Preview visibility and export inclusion are separate properties in the manifest, by design.

## What it deliberately does not do

- Launch Koikatsu, automate its UI, depend on a game process, or read a game install at run time.
- Modify user files. Mods are read in place; the output contains only meshes and textures generated for the card being converted.
- Guess. Unknown card families are reported as unsupported; unresolved items are reported by mod GUID rather than silently dropped.

## Asset library: running without the game

The converter needs base-game data (the shared skeleton and body, the item list tables, and the base-game hair, clothing and accessory bundles). Instead of reading a game install, CardBridge reads its own **asset library** folder, laid out like the game's `abdata` folder:

```text
<library>/library.json            what was copied, with sizes and hashes
<library>/abdata/chara/oo_base.unity3d
<library>/abdata/list/characustom/*.unity3d
<library>/abdata/chara/*.unity3d  (full profile)
```

`cardbridge library import --game <install> --library <dir>` copies those files once. Only this command ever reads a game install. `--minimal` copies just the skeleton, body and list tables (about 5 MB); the full profile is about 0.9 GB. `cardbridge library check` verifies a library. After that, `convert` and `inspect` take `--library` and never see the game. There is no game code to copy: what the game does in its C# assembly is reimplemented in C++ here from observation and decompiled reading.

**Important:** the library holds copies of the game's data files, which belong to their authors. It is kept out of this repository (`.gitignore`). Whether a library may be redistributed to people who do not own the game is a licensing question this project does not answer. The mechanism supports "build it from your own game copy" and "ship a prepared library"; choosing the second is a legal decision for whoever distributes it.

## Supported inputs

| Family | Card marker | Status |
|---|---|---|
| Koikatsu | `【KoiKatuChara】` | Target of the first release; card and asset layers verified on real files |
| Koikatsu Party (patched) | `【KoiKatuCharaSP】` | Card container verified; asset layout assumed identical, not yet tested |
| Koikatsu Sunshine | `【KoiKatuCharaSun】` | Recognised and reported as unsupported: the container differs (4 outfits, an extra `About` block) and no Sunshine install was available to verify its asset bundles |
| Outfit files, other games | `【KoiKatuClothes】`, others | Rejected with a specific error |

**Platform:** Windows only for now.

## Reverse-engineering findings

These are facts established against real cards and the decompiled game assembly; the full evidence, with the places where it is thin, is in [`docs/OFFLINE_FORMAT_FEASIBILITY.md`](../docs/OFFLINE_FORMAT_FEASIBILITY.md).

**Card container.** PNG image, then `int32 productNo`, .NET string marker, .NET string version, `int32 faceLen` + face PNG, `int32 headerLen` + MessagePack block header, `int64 dataSize`, then the blocks `Custom`, `Coordinate`, `Parameter`, `Status`, plus `KKEx` (written by the ExtendedSave plugin) and, on Sunshine, `About`. MessagePack payloads use string keys, so they decode without a schema.

**The card is references, not a model.** It stores item IDs, colours, slider values, and some embedded textures (the MaterialEditor plugin payload alone was 16 MB on the sample card). Meshes, skeleton and most textures live in Unity AssetBundles under `abdata` or inside `.zipmod` files.

**Mod resolution.** The card stores the mod author's own *slot* ID, not the session-local ID; the `KKEx` block `com.bepis.sideloader.universalautoresolver` maps each slot to a mod GUID and a property path. The zipmod's `manifest.xml` gives the GUID, and its `abdata/list/characustom/*.csv` map slot to bundle and asset name. Items without a resolve row are base game and resolve through the `ChaListData` MessagePack tables in `abdata/list/characustom/*.unity3d`.

**Assets.** UnityFS version 6, Unity 5.6.2f1. All 297 bundles in `abdata/chara` use LZ4HC (no LZMA). Meshes were uncompressed in every sampled bundle, skin weights are stored per vertex (four influences), and clothing binds to the shared skeleton by bone name. Textures seen were DXT1 and DXT5.

**Outfit structure.** Mod and base garments carry a `ChaClothesComponent` listing their renderer groups, which is the source of the part tree.

## Known limitations

- **Body proportions: applied. Face shape: not yet.** The card's 44 body slider values are turned into bone scales and offsets by the game's own formulas, which are copied into this project as generated C++ (`tools/gen_shape.py`, `src/shape/`) and applied to the skeleton, so height, bust, waist, hips and limbs follow the card. The 52 face sliders (eye size and position, jaw, nose ...) are not applied yet: the head keeps its default shape. The body shape ignores the alternative bone type (`typeBone` other than 0), which has a correction table that is not loaded.
- **Body masking.** Clothing hides body areas through mask textures; removing a garment can expose geometry that was never modelled. The tool warns; it does not invent body geometry.
- **Materials and colours.** The game's Shader Forge shaders have no PMX equivalent, so output is a diffuse-texture approximation. Done: clothing textures are baked from the list row's main texture and colour mask with the card's colours (including jacket-type tops, whose real garments are sub-parts), accessories are tinted by their colours, and eyes, eyebrows, eyelines and the nose line get their textures from the card's face ids. Not done: skin texture (body and face are the card's flat skin colour), lips, blush and makeup layers, eye highlights, pattern textures, hair colour (hair-type accessories are recoloured to the character's hair by a plugin whose data is not decoded), shading and rim light, BC7 textures (a few face parts) and textures stored in other bundles. Items a mod ships with mismatched texture names (as in the Rin sample) keep their prefab look.
- **Physics.** DynamicBone settings are not mapped to PMX rigid bodies and joints.
- **Independent outfit toggles in PMX.** PMX has no per-object visibility switch. Whether material opacity, display frames or group morphs give reliable toggles in a given viewer is untested; the manifest keeps the real toggles so the engine does not depend on viewer behaviour.
- **Plugin data.** Done: MaterialEditor main-texture replacements and KoiSkinOverlayX body, face and eye overlays, whose images are embedded in the card (they decide the look of many mod cards: the Rin sample is a blue outfit without them and the red-and-black one the card shows with them). Not done: MaterialEditor colour, float and shader overrides, ABMX, DynamicBoneEditor, HairAccessoryCustomizer and similar payloads, which need their own decoders.
- **Mods.** Only mods present on disk resolve. Duplicate GUIDs and mods shipped as unextracted `.rar`/`.7z` are not handled yet.

## Prior art and licensing

| Tool | Role | License |
|---|---|---|
| KKPMX | Reference for PMX clean-up rules; it runs in the game, so it cannot be a dependency | MIT |
| KKBP exporter/importer | Reference for what a full export contains; in-game plugin | Not verified |
| KKManager | Reference for card-marker detection | GPL-3.0: read for format knowledge only, no code is copied |
| UnityPy | Used for research and golden reference data only; not shipped | MIT |
| AssetRipper | Not used; noted as a reference | GPL-3.0 |

Format knowledge is re-implemented from observation and the decompiled assembly; no third-party code is copied. Game and mod assets are never committed to this repository.

## Layout

```text
cardbridge/
  CMakeLists.txt      standalone project; also add_subdirectory()-able from the engine
  src/
    core/             file IO (UTF-8 paths), LZ4, inflate, MessagePack, zip, math, SHA-256
    card/             card container and section decoding
    unity/            UnityFS bundles, serialized files, meshes, textures
    resolve/          mod index, list tables, card-to-asset resolution
    assemble/         skeleton and part assembly
    pmx/              PMX writer
    export/           output folder and manifest
    library/          asset library import and check
  cli/                command-line shell
  tools/              Python reference (research only, not shipped)
  tests/              unit and golden tests
  image/              README artwork
```

## Building

Windows, using the repository's portable toolchain (`tools/`):

```powershell
scripts\cardbridge.ps1
```

`scripts\cardbridge.ps1 -Test` also runs the unit tests. The build runs at idle priority. Output goes to `build-cardbridge\bin\cardbridge.exe`.

## Usage

```powershell
# once: copy the needed base-game data into a library (the only command that reads a game install)
cardbridge library import --game "<game folder>" --library "<library folder>"          # add --minimal for ~5 MB
cardbridge library check  --library "<library folder>" --hashes

# look at a card: family, outfits, which items resolve to which mod
cardbridge inspect "<card.png>" --library "<library folder>" --mods "<mods folder>"

# convert (writes <out>\<name>.pmx, <out>\textures\*.png, <out>\<name>.cardbridge.json)
cardbridge convert "<card.png>" --library "<library folder>" --mods "<mods folder>" --out "<out folder>"

# look at a card in a window (Windows): orbit with the left mouse button, pan with the right, zoom with the wheel.
# Hover a part row to light that part up in the model, click to hide/show it, right-click to solo it, click a group
# header to fold it. The log panel shows every load/bake/export stage as it happens; E (or Export PMX) exports the
# scene currently on screen to a new folder, with the same live log. The panel shows the current outfit's number and
# name over a row of numbered slots: click a slot, press 1-7, or step with Left/Right. Other keys: T textures, H game
# shaders, F/S/B front/side/back, R reset, I issues, A show all, C clear log, O open, M mods folder, L library.
# Drop a card .png on the window to open it.
cardbridge_viewer "<card.png>" --library "<library folder>" --mods "<mods folder>"      # the folders are remembered
```

Exit codes: 0 success, 1 usage, 2 not a supported card (with the reason: not a PNG, no card data, truncated, an outfit file, unknown or unsupported family), 3 conversion failed. Items that cannot be loaded are listed with a status and reason on screen and in the manifest's `issues`; the first run over a mods folder builds an index (about 15 minutes for 4,000 mods, then cached).

Tests: `scripts\cardbridge.ps1 -Test` runs the unit tests (no game files needed). Set `CARDBRIDGE_TEST_LIBRARY`, `_MODS`, `_CARD` and `_GOLDEN` (`tests/golden/rin_reference.json`) to also run the card-to-PMX integration test, and run `cardbridge_pmx_crosscheck <model.pmx> <manifest.json>` to read a result back with the engine's PMX loader.

## FAQ

**Q: Is this vibecoding?**

> **A:** Yes.
>
> The vibes are immaculate. The vertices are under investigation.

**Q: Can you code?**

> **A:** Yes. I use AI coding tools extensively because they can implement things faster than I can write everything manually. I still make architectural decisions, review code, debug issues, and validate the results.

**Q: Then why not do it manually?**

> **A:** Because I'm too lazy to do everything manually, and Claude Opus 5.5 can often implement things faster than I can.
>
> Why spend hours writing code when I can spend those hours reviewing AI-generated code, debugging it, reverse-engineering Unity AssetBundles, and questioning every life decision that led me here?
>
> **Work smarter, not harder.** Or at least make the computer do the hard part.

**Q: So the AI writes everything?**

> **A:** A substantial amount of the code is AI-generated. I'm not going to pretend otherwise.
>
> I direct the implementation, make architectural decisions, investigate problems, review changes, test the results, and decide what needs fixing. AI-generated code can be wrong, inefficient, or unnecessarily complicated.
>
> If something breaks, it's still my problem.
>
> **AI-assisted development. Human-operated suffering.**

**Q: Why build this instead of just using KKPMX?**

> **A:** KKPMX already exists and is useful. CardBridge is my attempt to build an alternative with a standalone workflow, extensible mod support, better diagnostics, and eventually an integrated editing experience.
>
> These are goals, not claims that CardBridge is already better. The conversion pipeline still needs substantial validation.
>
> The objective isn't to reinvent everything just for the sake of it. It's to build something that better fits my needs.
>
> *(The meme stays, though.)*

**Q: Why Koikatsu, though?**

> **A:** Because creating anime-style characters in Koikatsu is much more accessible than building them from scratch in general-purpose 3D software.
>
> Its character creator makes it easy to customise faces, hairstyles, clothing, body proportions, and other details without needing to model everything manually. Combined with its modding ecosystem, it offers a convenient way to create and customise anime characters.
>
> The problem is getting those characters out of the game and into an editable PMX model without spending hours fixing materials, hair, accessories, and other conversion issues.
>
> That's where CardBridge comes in: bridging the gap between Koikatsu's character creation workflow and the broader MMD/PMX ecosystem.
>
> And honestly, creating anime characters is fun. Reverse-engineering Unity AssetBundles to get them out is where the suffering begins. 💀

**Q: Is CardBridge production-ready?**

> **A:** No.
>
> Milestone 1, the headless proof of concept, is complete. However, successfully generating a PMX file doesn't mean the resulting model faithfully reproduces the original character.
>
> Colours can be wrong, hair can disappear or move, materials can break, and geometry or deformation can require further work.
>
> The feasibility study suggests the approach is viable. The complete conversion pipeline still needs to prove itself through testing.

**Q: Will it support every mod?**

> **A:** That's the goal, not a promise.
>
> Mods can introduce different assets, dependencies, material configurations, and structures. CardBridge aims to handle these through extensible asset resolution, clear diagnostics, and compatibility testing.
>
> If a mod breaks the converter, I'd rather know exactly why than silently export a broken model.

**Q: Why is the project called CardBridge?**

> **A:** It bridges the gap between a character card and an editable 3D model.
>
> Also, `CardToPMXButHopefullyItActuallyWorksAndEasyToUseWithoutAnyManualModification` was too long.

**Q: Why is the logo AI-generated?**

> **A:** Because my entire budget went to Claude, and I suck at design.
>
> I'm trying to reverse-engineer Unity AssetBundles and build a standalone PMX exporter, not become a graphic designer.
>
> **Priorities, people.** 💀

## Milestones

- [x] **0.** Offline feasibility study (verdict: go)
- [x] **1.** Headless proof of concept: one real card to PMX, no game process (verified on two cards; open items in the feasibility doc, 12.4)
- [ ] **2.** Live preview and part tree on the engine renderer
- [ ] **3.** PMX part organisation, toggle testing, validation
- [ ] **4.** Standalone UI, project save/load
- [ ] **5.** Slot replacement and mod robustness
- [ ] **6.** Packaging

Milestone 1 acceptance tests (A1 to A10) are listed in the feasibility document.

---

<p align="center">
  <sub>Built with C++, reverse engineering, AI assistance, and a questionable amount of spite.</sub>
</p>
