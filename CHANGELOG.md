# Changelog

All notable changes to CardBridge are documented here, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). CardBridge did not keep a changelog before 0.3.0; entries for earlier releases were reconstructed from the git history and the release notes that shipped with those releases, and are not expanded beyond what those show.

## [0.4.8] - 2026-11-20

### Fixed

- **Hair washed out in MMD / exported PMX**: the shader bake feeds sRGB hair-colour properties (`_Color2`/`_Color3`) raw, so its output is already sRGB. A ToSrgb encode added with HAC support double-converted that output for materials with a non-sRGB main texture (the HAC back-hair gradient mask, the create-job body overlay), turning the game's dark hair into pale lavender in both the as-exported view and MikuMikuDance. Removed the erroneous encode so the bake is gamma in / gamma out.

## [0.4.7] - 2026-11-20

### Fixed

- **MMD ("as exported") view for hair**: hair materials are now baked with the game's own front light (per-material lit bake) so the MMD preview carries the game's hair colour/shade instead of a flat sheet re-shaded by MMD's light/ramp. Non-hair materials keep the flat + MMD-ramp treatment so the MMD look stays faithful.

## [0.4.6] - 2026-11-20

### Fixed

- **MMD ("as exported") view vs the game view**: hair materials now keep the character/accessory hair tint's alpha, and the plain (MMD-look) fallback shader gates cutout fragments, so the viewer's MMD-mode preview draws hair the same opaque way the game's own shaders do — the MMD viewer now matches the non-MMD viewer (back hair included).
- Submesh materials: the renderer material slot now follows Unity's `m_SubMeshes[].m_Material` when the asset serializes it (falling back to the submesh ordinal), so the material the right submesh draws with is the material the card meant.

## [0.4.4] - 2026-11-20

### Fixed

- **HAC vs MaterialEditor override order**: `ColorMatch=true` hair accessories now apply their colours after MaterialEditor edits (matching the game), fixing washed-out back hair on cards where MaterialEditor stored a different `Color2`.
- HAC regression fixture/test matrix updated with the real-card A/B evidence.

## [0.4.3] - 2026-11-20

### Added

- **Full HairAccessoryCustomizer (HAC) support**: dedicated MessagePack decoder, typed per-coordinate/per-slot settings (including `moreAccessories` slots), `ColorMatch` hair colours, stored outline/gloss/accessory colours, and `_GlossColor` in the material bake. Malformed or partial HAC records are ignored safely. `HairLength` is preserved in the scene's intermediate material properties but has no PMX equivalent.
- HAC regression tests with a payload fixture extracted from a real card (coordinate 6, slots 3/5/9/10/20).

## [0.4.2] - 2026-11-20

### Fixed

- **KK look regression from the KKS shader reader**: KK shaders place DXBC at offset 54, so the KKS tail parser was being used on them and corrupted KK create-job baking (white hair, dark skin, wrong textures). Program-format detection now uses the KKS preamble magic; verified against the pre-KKS build on `KK_236802`.
- Viewer: direct per-game KK/KKS library and mods pickers plus a "share this mods folder with both games" action, without needing to switch profiles first.

## [0.4.1] - 2026-11-20

### Added

- **Per-game viewer profiles**: independent KK and KKS library/mods folder pairs, automatic profile selection from the card marker, a mismatch warning when the active library belongs to the other game, and manual switching in the Setup tab. Existing single-library configs are migrated without losing paths; `--library` / `--mods` overrides remain authoritative for the session.

## [Unreleased] - 2026-11-20

### Added

- **Koikatsu Sunshine (KKS) as a first-class source format** in the same offline engine: card acceptance for `【KoiKatuCharaSun】` (4 outfits, Face 0.0.3, Parameter 0.0.6), source-game profile in `library.json`, KKS mesh skinning from vertex channels 12/13 (UInt32 format 10), a KKS shader-blob reader (nested descriptors, 2019 program header, KKS binding table) so all five create materials run on the SM4 VM, family-aware hair/alpha shader mappings, third clothing texture and `HideShorts`, `<game>` tag filtering for mods, 1,968 KKS list rows, `rendNormal03`, KKS shape corrections, and online-independent conversion unchanged.
- Research and validation documents: `docs/KKS_FORMAT_FEASIBILITY.md`, `docs/KKS_IMPLEMENTATION_ROADMAP.md`, `docs/KKS_TEST_MATRIX.md`.

### Known limitations

- KKS `Koikano/*` materials are not re-baked through the UV-flat material bake; their verified create-job textures are kept and the skip is reported. KK `Shader Forge/*` baking is unchanged.
- `MabUV` and KKS-exclusive accessory component fields (`objSleeves*`, `exRendEmblem*`) are not modelled.

## [0.3.0] - 2026-11-19

The first installer release. From here on, CardBridge-Setup is the recommended download; the portable ZIP stays available for the same binaries without an installer.

### Added

- **Windows installer** (Inno Setup 6): installs into `Program Files\CardBridge` (changeable), Start Menu shortcuts (`CardBridge`, `CardBridge Command Line`, `Uninstall CardBridge`), an optional desktop shortcut, Add/Remove Programs integration with a working uninstaller, and in-place upgrades that remove obsolete files and keep your settings, library and exports untouched
- **Portable ZIP**: `CardBridge-Portable-0.3.0.zip` — the identical build without the installer
- **Reproducible packaging**: `installer/build_installer.ps1` builds the Release binaries, verifies staging (files, version metadata, that only Windows system DLLs are required), compiles the installer, writes the ZIP and SHA-256 sums
- **Material Workbench**: live material overrides for mod items, a texture inspector and diagnostic viewport modes (Lit/Unlit/BaseColor/Wireframe/Normal/Roughness/Metallic/Opacity/UVChecker/Transmission/MaterialID)
- **Selective Import**: applies outfit/coordinate imports one component at a time (Add / Replace / Keep / Skip / Add Layer) with three live views and a per-component picker; nothing touches the card until you press Apply
- **Coordinate import**: Koikatsu Coordinate PNGs (a character card used as an outfit source) and `【KoiKatuClothes】` outfit files import as clothing (merged over the shown outfit) or as a whole coordinate (wholesale replace). The card on disk is never saved by either mode; [`docs/COORDINATE_IMPORT.md`](docs/COORDINATE_IMPORT.md)
- **Clothing export**: `cardbridge clothes` and the viewer's **Export clothes** button (`Shift+E`) write one outfit back out as a `【KoiKatuClothes】` file from a character card; [`docs/CLOTHING_EXPORT.md`](docs/CLOTHING_EXPORT.md)
- **Mesh simplification and LODs**: quadric-error-metrics reduction with per-part budgets (face/eyes/hair/body/clothes/accessories) and stacked `name_lod1..4.pmx` exports; Optimize tab and `--simplify` / `--lods` / `--lod-targets`
- **Body measurements (BWH)**: Bust / Waist / Hip measured in centimetres from the actual skin (a card whose skin is not a closed surface falls back to slider percent); type an exact value and the shape solves to it — with Band and Cup presets for bust size
- **Proportion correction** (opt-in, non-destructive): flags slider and ABMX values the maker allows but most cards do not use, offers half-way-to-neutral fixes you accept or reject individually, never modifies the card; [`docs/PROPORTION_CORRECTION.md`](docs/PROPORTION_CORRECTION.md)
- **Skin shading and AO bake**: automatic cleavage shading, an underboob fold control, nipple colour tint, and Koikatsu's own cavity/ink-line look baked into the body skin (`--skin-ao`, Look tab); [`docs/KOIKATSU_AO_INVESTIGATION.md`](docs/KOIKATSU_AO_INVESTIGATION.md)
- **Translucency brush**: hand-paint see-through regions on garments (live strokes, mask exported as alpha); painted areas render through correctly in the viewer, the "as exported" view and in MMD
- **Bust soft-physics chains** in the PMX (default on, both sides, coordinated); `docs/BUST_PMX_PHYSICS.md`
- **Viewer physics preview** (`Y` toggles it) for DynamicBone chains and jiggle; `N` applies a shove
- **Face morphs with MMD-style names** and embedded facial effects with no runtime plugin (Eye Roll, Heart Eyes, Sweat / Sweat Heavy). Implemented and unit-tested; real-card in-game verification pending; [`docs/FACE_MORPHS.md`](docs/FACE_MORPHS.md)
- **Viewer rotate gizmo** for accessories (X/Y/Z rings next to the move gizmo; `Ctrl+Z` undo; bakes into exports)
- **UI redesign**: two-row tab grid, flat widget kit, modern folder picker, single-accent charcoal theme; `docs/UI_DESIGN_SYSTEM.md`
- **Performance**: ~5× faster conversion (cached decode, LZ4 fast paths, pre-built mip chains), ~20% lower peak memory; [`docs/PERFORMANCE_REVIEW.md`](docs/PERFORMANCE_REVIEW.md)
- New `cardbridge` CLI verbs and hardware tooling: `clothes`, `skin-bench`, `simplify-bench`, `--gpu-view`, `--gpu-skin`
- GPU skinning for the viewer (Direct3D 11 compute): adapter selection, per-device skin path, Setup tab GPU rows, `CARDBRIDGE_GPU*`/`CARDBRIDGE_SKIN*` overrides; [`docs/GPU_SKINNING_HARDENING.md`](docs/GPU_SKINNING_HARDENING.md)

### Changed

- **Version scheme**: 0.3.0 is the first non-alpha release; the internal `0.2.x-alpha` bumps never shipped as releases. `VERSION` at the repository root is the single source (the Win32 manifest derives from it, and the CLI exe now carries the same metadata)
- **Outfit/coordinate imports** apply the file's own verbatim plugin payloads (MaterialEditor/KCOX/ABMX/ACI/`clothesState`/accessory visibility) rebased to the target outfit — the same semantics the game itself uses
- **Viewer defaults to the "as exported" MMD view** on load rather than the Lit view
- **Deterministic bakes**: texture baking is order-independent; exported textures no longer vary between runs
- **`%LOCALAPPDATA%\CardBridge` is the user-data root** (settings, library, mod index); exports go where the user points, never into the install folder
- **Simplifier** never removes visible clothing: only whole faces fully occluded by an opaque outer layer are dropped (LOD 2+), and no face may rotate more than 40° from its authored orientation at any level

### Fixed

- MikuMikuDance crashed a few seconds after opening a model with facial morphs: it keeps the number of listed morphs for the whole file in one byte (256+ corrupts its heap). The `Exp` frame listed 255 and the facial tabs (Eye/Brow/Mouth/Other) listed every facial morph on top of that. The 255-slot listed-morph budget is now shared: `Exp` lists the priority morphs (outfit toggles, the MMD-standard morphs, the four face effects), the facial tabs split the remainder proportionally, nothing is listed twice, and whatever does not fit stays in the file unlisted (the audit passes: Exp 69 + Eye 78 + Brow 27 + Mouth 81 = 255)
- Crash (`0xC0000005`) in the software shader bake when a translucency stroke invalidated a mip chain being read by worker threads
- Crash and "exploding head" when an ABMX bone had zero scale (singular inverse); scales are clamped at 1e-6
- Double-layered "studio-alpha" garments (an opaque shell and its translucent twin in one mesh) drew the lining over the outside in the viewer, in MMD, and in the PMX; the lining is now detected, exported single-sided and fenced to the interior only
- Garments painted with the translucency brush disappeared (the opaque vertex program discarded the alpha); they now use an alpha-cloth twin
- Alpha-cutout test sampled the whole atlas and could mark the body and face as cut-out (flat plaster skin in MMD); it now samples only texels the mesh covers
- Brows, lashes and ink lines baked white regardless of the card's colours; they now take the card's colours
- Outfit imports no longer inherit the destination outfit's coordinate-specific ABMX or stale paint (clipped skirts); stale ACI/plugin data resolved like the game
- Selective import: additive applies could doubly draw the outfit, offset it ~8 cm, or silently drop stacked applies; pinned imports compose correctly
- Hair no longer meets colliders at all — the neck/shoulder vibration at load and in motion is gone, and hair no longer grows a black MMD edge
- Jiggle rebuilt as a physical mass-spring-damper system: resting pose is the authored pose, presets retuned, butt jiggle off by default
- Memory use of large cards reduced (bounded caches), avoiding the ~430 MB peak of earlier builds
- LOD 1 never deletes geometry anywhere; LOD 2+ drops only fully-occluded hidden faces, reported in the manifest
- Face morphs are discovered per head by the game's own blend-shape keys, so they export on any card (with the usual caveat: verified on synthetic scenes, not yet on every real card)

### Removed

- The short-lived garment-physics experiment (replaced by, and reverted from) the v0.2.1-alpha garment physics model; the collider burial-depth fix was kept

### Notes

- **Development pace**: starting 1 December 2026, development slows down, but CardBridge is still in active development. If you run into any issue, open an issue on GitHub — it helps more than email does.
- This release ships a **first-time installer**. Existing users who ran the plain `CardBridge-<version>.exe` should simply run the new installer; your `%LOCALAPPDATA%\CardBridge` folder is untouched by both install and uninstall.

## [0.2.1-alpha] - 2026-11-04

### Added

- About 45 standard facial morphs with the names and panels of hand-made MMD models, each backed by the game's own pattern of the same meaning, so lip sync and facial keys in dance motions reach the face

### Fixed

- The vowel morphs (あ い う え お ん) now drive the game's own lip-sync shapes instead of scaled "happy"/"mouth-stuffed" patterns (which left one vowel a black hole and the teeth floating)
- The tongue is in the mouth and moves with every mouth shape; an open mouth is no longer an empty black cavity

## [0.2.0-alpha] - 2026-11-04

### Added

- Ground shadow (0x02), self-shadow (0x04) and edge-colour requests on the materials MMD draws (face layers and lenses excepted), like hand-made models
- Sphere maps (highlight rings) chosen per material; MMD's broad specular sheen is off
- New **MMD** button (or `X`): the export's look — baked textures, toon ramps, edges, self- and ground shadows — as MikuMikuDance shows it
- A-pose (arms 40° down) as the default rest pose, matching dance-motion keying; `--tpose` keeps the T-pose
- Semi-standard rig bones of hand-made models (twist bones, shoulder/hands P/C/D, leg IK, tips)

### Fixed

- Items cut by a MaterialEditor texture are cut in the PMX too (no more black cut parts)
- Which faces are drawn follows each shader's own setting (the game's main shaders draw both sides)
- Garment option pieces the card switches off are left out
- Eyes: the iris and eye white match the game's colours, shape and place, and turn correctly in MMD
- Underhair no longer covers the whole body; triangles with no UV area are baked too
- Physics: skirts and coats turn with the legs, skirt end stubs no longer flip, the front no longer trembles in MMD

## [0.1.0-alpha] - 2026-11-04

### Added

- Initial public alpha: converts one real Koikatsu card to PMX fully offline (no game process). Details of this release were not recorded in a changelog at the time; the only surviving artifacts are the tag, the feasibility document (`docs/OFFLINE_FORMAT_FEASIBILITY.md`) and the `v0.1.0-alpha`/`v0.2.0-alpha`/`v0.2.1-alpha` release files.
