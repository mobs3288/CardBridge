# CardBridge

<p align="center">
  <img src="image/CB.png" alt="CardBridge logo: a character card and a wireframe 3D model joined by a bridge" width="500">
</p>

**CardBridge** converts Koikatsu character cards into rigged 3D models (PMX) with separately addressable outfit parts, entirely offline and **completely separate from the game**: it reads the card, a CardBridge asset library and the user's mod files, and needs no Koikatsu install to run. It never launches or controls the game.

It is a standalone C++ Windows application built in the same style as the Project K engine and Map Maker. Its core is a UI-free library so the Map Maker can later import its output (or call it) without a second implementation.

> **Version 0.5.0-alpha. It is usable today, but it is not finished.** It works **100% only for the base game**: cards made with the stock Koikatsu items convert and look right. **Several modded cards also work**, but not all of them, and a mod can still break a conversion or a material. It is **still in development**: expect rough edges, wrong colours on some mod items, and missing pieces (see Known limitations). Every export also writes a `_preview.png` (front, side, back) so a result can be judged without a PMX viewer, and the viewer shows exactly what the game's shaders draw. The checklist at the bottom of this file is the source of truth for what is done. The version is in the [`VERSION`](VERSION) file; `cardbridge --version` and the viewer's header show it.

## Get it and run it

The release is **one exe**: `CardBridge-<version>.exe` (the viewer, with the converter and exporter inside). No installer, no scripts, no extra files.

1. Run it. The first time it asks for **your own Koikatsu folder** (the one that contains `abdata`) and copies the base-game data it needs into `%LOCALAPPDATA%\CardBridge\library` (about 0.9 GB, about 20 seconds, once). The game is never needed again.
2. It asks for your Koikatsu **mods folder** (or Cancel for base-game items only). Mods are never shipped with CardBridge: each user points it at their own.
3. Drop a card `.png` on the window. Press **E** (or *Export PMX*) to write the PMX.

**Settings.** The **Setup** tab (last tab in the panel) changes everything chosen at first run, and each change is saved at once to `%LOCALAPPDATA%\CardBridge\viewer.json`: use another library folder or build a new one from a Koikatsu folder; choose the mods folder, rebuild the mod index after adding mods, or use no mods; one fixed export folder (or ask every time); an extra folder of shader layers; open the settings folder; forget all settings. Keys `M` (mods folder) and `L` (library) still work.

`CardBridge-CLI-<version>.exe` is the same converter as a command line tool (see Usage). Every exported PMX carries the model information (CardBridge, its version and its author, Ryan Oktaviandi Susilo Wibowo, which is fixed and cannot be changed by a setting; the character; the source card) in the header comment, which MikuMikuDance shows in its model info.

## What you need

| Thing | What it is | Where it comes from |
|---|---|---|
| Windows 10 or 11, 64-bit | CardBridge is a Windows program | A graphics card with Direct3D 11 draws the viewer with the game's shaders; without it the viewer falls back to a slow software renderer |
| **Koikatsu or Koikatsu Party install** | The game's own data files, read **once** by the first-run setup and never again | Your own copy of the game (this is the game folder that contains `abdata`) |
| **Your mods folder** (optional) | The `.zipmod` files (Sideloader mods: hair, clothes, accessories, textures) your cards use | Your Koikatsu `mods` folder (or any folder holding the `.zipmod` files). CardBridge reads it in place and never changes it |
| **A card** | A Koikatsu character card, `.png` | Made in the Koikatsu character maker |

**What the setup copies from the game** into the CardBridge library (`%LOCALAPPDATA%\CardBridge\library`, about 0.9 GB):

- `abdata/chara/oo_base.unity3d`: the shared skeleton and the body.
- `abdata/chara/etc.unity3d`: the dummy items some mods point their empty slots at.
- `abdata/chara/*.unity3d`: every base-game hair, clothing and accessory bundle, which also hold the game's shaders (the bulk of the 0.9 GB).
- `abdata/list/characustom/*.unity3d` and `abdata/list/customshape.unity3d`: the item tables (which item id is which file) and the body and face slider shapes.
- The colour-grading table (`Lut_Amount`) from the `abdata/map/scene` bundles, so the viewer and the export use the character maker's colours.

`cardbridge library import --game <game folder> --library <folder> --minimal` copies only the first, second, fourth and fifth items (about 5 MB): base-game clothes and hair are then missing and every item has to come from mods.

**What the mods folder is for.** A card stores a mod item as the mod author's slot id plus the mod's GUID. CardBridge finds the `.zipmod` with that GUID in your mods folder and reads its meshes, textures and item list from there. If the mod is not in the folder, that item is listed as missing (by mod GUID) in the issues and the manifest, and the rest of the character still converts. The first run over a mods folder builds an index (about 15 minutes for 4,000 mods; cached in `%LOCALAPPDATA%\CardBridge\modindex.json`, so later runs are fast). Mods packed as `.rar` or `.7z` must be unpacked first.

**Not needed:** the game running, BepInEx or any plugin, Blender, MikuMikuDance (only to open the PMX afterwards). Game files and mods are never uploaded anywhere and never stay inside CardBridge's exe.

## Why this project exists

CardBridge is a tool for making anime characters for my own game, Project K, using the established Koikatsu character engine instead of Blender. I am not good at 3D modelling or texturing, and Koikatsu's character maker already does that part very well; what was missing was a reliable way to get a finished character out of it, rigged and textured, into my own engine.

It also exists to make character conversion more reliable, mod compatibility more extensible, and the entire workflow less painful.

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

These are facts established against real cards and the decompiled game assembly; the full evidence, with the places where it is thin, is in [`docs/OFFLINE_FORMAT_FEASIBILITY.md`](docs/OFFLINE_FORMAT_FEASIBILITY.md).

**Card container.** PNG image, then `int32 productNo`, .NET string marker, .NET string version, `int32 faceLen` + face PNG, `int32 headerLen` + MessagePack block header, `int64 dataSize`, then the blocks `Custom`, `Coordinate`, `Parameter`, `Status`, plus `KKEx` (written by the ExtendedSave plugin) and, on Sunshine, `About`. MessagePack payloads use string keys, so they decode without a schema.

**The card is references, not a model.** It stores item IDs, colours, slider values, and some embedded textures (the MaterialEditor plugin payload alone was 16 MB on the sample card). Meshes, skeleton and most textures live in Unity AssetBundles under `abdata` or inside `.zipmod` files.

**Mod resolution.** The card stores the mod author's own *slot* ID, not the session-local ID; the `KKEx` block `com.bepis.sideloader.universalautoresolver` maps each slot to a mod GUID and a property path. The zipmod's `manifest.xml` gives the GUID, and its `abdata/list/characustom/*.csv` map slot to bundle and asset name. Items without a resolve row are base game and resolve through the `ChaListData` MessagePack tables in `abdata/list/characustom/*.unity3d`.

**Assets.** UnityFS version 6, Unity 5.6.2f1. All 297 bundles in `abdata/chara` use LZ4HC (no LZMA). Meshes were uncompressed in every sampled bundle, skin weights are stored per vertex (four influences), and clothing binds to the shared skeleton by bone name. Textures seen were DXT1 and DXT5.

**Outfit structure.** Mod and base garments carry a `ChaClothesComponent` listing their renderer groups, which is the source of the part tree.

## Known limitations

- **Body proportions: applied. Face shape: not yet.** The card's 44 body slider values are turned into bone scales and offsets by the game's own formulas, which are copied into this project as generated C++ (`tools/gen_shape.py`, `src/shape/`) and applied to the skeleton, so height, bust, waist, hips and limbs follow the card. The 52 face sliders (eye size and position, jaw, nose ...) are not applied yet: the head keeps its default shape. The body shape ignores the alternative bone type (`typeBone` other than 0), which has a correction table that is not loaded.
- **Body masking.** Clothing hides body areas through mask textures; removing a garment can expose geometry that was never modelled. The tool warns; it does not invent body geometry.
- **Materials and colours.** The game's Shader Forge shaders have no PMX equivalent, so output is a diffuse-texture approximation. Done: clothing textures are baked from the list row's main texture and colour mask with the card's colours (including jacket-type tops, whose real garments are sub-parts), accessories are tinted by their colours, and eyes, eyebrows, eyelines and the nose line get their textures from the card's face ids. Not done: skin texture (body and face are the card's flat skin colour), lips, blush and makeup layers, eye highlights, pattern textures, hair colour (hair-type accessories are recoloured to the character's hair by a plugin whose data is not decoded), shading and rim light, BC7 textures (a few face parts) and textures stored in other bundles. Items a mod ships with mismatched texture names (as in the Rin sample) keep their prefab look.
- **Export vs the preview.** The viewer runs the game's own shaders with the maker camera's colour grading and one headlight; a PMX has neither. What the export does about it: each material's shader is run over its UV layout once (colour only, lit straight from the front) and the result, passed through the same grading look-up table as the preview (`G`), is the PMX texture; the material is plain white (diffuse 1, so an engine that tints the texture by diffuse, like Project K's, shows it unchanged). Its shade comes with it as a toon ramp: the shader is also run with the light swept from straight on to behind and the result, relative to the baked colour, becomes the material's own PMX toon texture (`textures/toonN.png`, bright at the top, MikuMikuDance's and Project K's convention; the material's ambient is then white). So lighting still comes from the viewer that opens the file, but through the game's own shade curve (specular, rim and outline stay out). `--no-toon-ramp` writes the shared toon01 and the old shadow tint. Press `X` in the viewer to see exactly that ("as exported": baked textures on plain materials, no shader, no second grading); on the sample cards its mean colour is within about 3/255 of the preview. Still different: the game's rim and outline shading, cast shadows and the face shadow mask are not in a PMX, and each viewer lights materials its own way. CLI: the baked-and-graded default, `--no-grade` to leave the grading out, `--no-toon-ramp` for the shared toon, `--bake-lit` for the old texture with the front-view shading baked in (the material is then unlit, which an engine that lights it itself shows black), `--no-bake` for the raw textures; `--gpu-view` also writes `gpu_plain.png`, the "as exported" picture.
- **Bones, expressions and physics in the PMX.** Every transform of the body and head rigs is a bone (677 on the sample cards; eyes, eyelids, brows, mouth, cheeks, chin, ears, nose included), plus the joints and chain nodes of hair, clothes and accessories. The game's bone name is the PMX name (the identity the sliders and mods use); the English field is a readable guess, driven / shape / attach nodes are written hidden, the tail points at the first child, and the bone panel is grouped (Face: Eyes, Arms, Hands, Hair, ...). The face's blend shapes become vertex morphs: one per brow / eye / mouth pattern of the game's FaceBlendShape (closed and open, in the brow / eye / mouth panels) and one per raw shape. DynamicBone chains (hair, skirts, ribbons, the body's jiggle chains) become a kinematic anchor, capsules and spring joints, and the body colliders become kinematic bodies. The mapping follows the component's particle update: every chain bone (the root too) has a capsule and a spring joint at its head, damping becomes the same decay a second, elasticity the swing rate, stiffness the rotation limits; the collision mask is the Bullet filter (bit n = collides with group n), so chains meet colliders and nothing else. A test-side PMX reader and a small planar Bullet-like reference (`tests/PmxReader.h`, `tests/SpringRef.h`) check the written bodies, joints, morphs and frames and show the exported numbers settle in the time the DynamicBone sim does; gravity and force are not carried, and none of it has been opened in a real PMX editor. IK: one IK bone per arm and leg (`cb_ik_hand_L` ...) is added after the rig's bones where the limb bones (`cf_j_arm00` / `forearm01` / `hand`, `thigh00` / `leg01` / `leg03`) form a parent chain; the elbow and knee may only flex further the way the rest pose already bends. There are no append (twist) bones: nothing read from the game says the driven `cf_d_*` bones copy a parent's rotation.
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
  viewer/             the viewer window (Win32 + Direct3D 11): main.cpp, Ui.* (off-screen panel, theme), res/ (icon, logo)
  tools/              Python reference (research only, not shipped), viewer_smoke.ps1
  tests/              unit and golden tests
  image/              README artwork (CB.png is the logo)
  VERSION             the one place the version number is written
```

## Building

Windows, using the repository's portable toolchain (`tools/`):

```powershell
scripts\cardbridge.ps1
```

`scripts\cardbridge.ps1 -Test` also runs the unit tests. The build runs at idle priority. Output goes to `build-cardbridge\bin\cardbridge.exe`.

On its own (this folder as its own repository): install CMake, Ninja and a Windows C++20 compiler (MinGW-w64 or MSVC), then `build.ps1` (`-Test` runs the unit tests); output goes to `build\bin`. `package.ps1` makes the release files (`CardBridge-<version>.exe`, `CardBridge-CLI-<version>.exe`, their SHA-256) in `dist\`; the version comes from the `VERSION` file.

To check a PMX for what MikuMikuDance chokes on (nested group morphs, IK limits, NaN, bad indices): `python tools/audit_pmx_for_mmd.py model.pmx`.

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
# shaders, G colour grading, X as exported (the PMX's baked textures on plain materials), F/S/B front/side/back, R reset, I issues, A show all, C clear log, O open, M mods folder, L library.
# Posing: K shows the skeleton over the model (click a bone to select it), the Bones tab lists the bones as a tree (helper nodes behind a switch)
# and edits the selected bone's move / rotate / scale (sliders, wheel for a fine step, right-click resets a field); the mesh follows at once, no re-assembly.
# P (or Look tab > Pose) is the automatic A-pose: clavicle and upper arm turn the arms down from the T-pose, 40 degrees by default. Export PMX writes the
# pose as the model's rest pose and lists it in the manifest's "pose". CLI: --apose [--apose-degrees N]; --gpu-view --gpu-bones draws the skeleton on the picture.
# With the skeleton on, the bone under the pointer is ringed and a click-drag on a bone turns it (Ctrl+drag moves it): the tip follows the pointer, the bone and
# everything below it (drawn in the same warm colour) swing. The body and face sliders and the nipple controls are real time too (about 6 ms: the skeleton is rebuilt
# with the game's formulas and every vertex follows through its per-bone frames, no loading), the blend shape offsets and the areola size follow too (a character whose DynamicBone chains were settled with CARDBRIDGE_DYNBONE is assembled again instead).
# Ctrl+Z / Ctrl+Y undo and redo bone edits and the A-pose (one drag or click is one step), Ctrl+S / Ctrl+O (or the buttons under the bone editor) save and load a pose as
# JSON; `--pose <file>` on the viewer loads one at start, `cardbridge convert --pose <file>` bakes it into the PMX. The software view (no Direct3D 11) shows the pose too.
# The panel is drawn off screen and only where it changed, the mouse never queues work faster than a picture can be made, and the lists scroll
# smoothly. Timing: set CARDBRIDGE_BENCH=<file.json> and open a card; the viewer plays a scripted session (orbit, hover, tabs, a slider drag, a flood of
# mouse moves, log lines, wheel), writes how long each step took from input to finished paint, and closes.
# tools/viewer_smoke.ps1 runs a private viewer on a card and takes screenshots of every tab and interaction (see its header).
# Y switches the physics preview on: the DynamicBone chains (hair, skirts, ribbons, the body's jiggle) are simulated live at 60 updates a second against the body
# colliders, so they swing as bones, sliders or the A-pose move what they hang from; N gives them a shove. The camera never pauses it.
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

**MikuMikuDance.** The PMX faces -Z, MikuMikuDance's front (`--unity-facing` on the CLI keeps the +Z the character is built facing). MMD keeps the number of morphs a display frame lists in one byte and corrupts its heap from 256 on, so the Exp frame lists the first 255 morphs (outfit toggles, then the game's brow / eye / mouth patterns, then the raw per-mesh shapes); morphs beyond that stay in the file, unlisted. A physics body meets only the colliders it does not already overlap in the rest pose (a body that starts inside one is thrown out of it), no link turns more than 35 degrees, and the body's own jiggle chains (breast, hip) are left rigid (a light link carrying body skin shivers in MMD). A chain's root follows its bone and the links below it swing (a moving root shook the whole model in MMD), and a link's spring is never stiffer than 15 rad/s for its own weight, a joint that gravity would push onto its stop is locked, `--no-physics` writes no physics at all, which MMD's solver turns into a permanent shiver otherwise.
