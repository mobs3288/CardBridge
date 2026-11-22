# Changelog

All notable changes to CardBridge are documented here, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). CardBridge did not keep a changelog before 0.3.0; entries for earlier releases were reconstructed from the git history and the release notes that shipped with those releases, and are not expanded beyond what those show.

## [0.6.1] - 2026-11-22

### Fixed

- **Nostril shadow discarded by the face-paint cutout (KK/KKS)**: a nose texture's alpha is authored as one continuous ramp - the dark nostril cores plus a soft shadow spanning both of them (alpha 4..28 in the supplied card). The `toon_nose` cutout kept only the cores, so the shadow never reached the viewer or the PMX. The sub-cutout fringe is now lifted from the source texture onto the face skin's create-job result at the shared UV layout, underneath any KoiSkinOverlayX face paint. Only pixels the card already authored are used; no new detail is drawn and the opaque cores stay on the decal mesh, so nothing is double-drawn. Added extraction/compositing tests; 19,046 unit checks pass. Verified on `KK_235599.png` in the GPU view.

## [0.6.0] - 2026-11-22

### Added

- **Per-card nose-ridge strength control (KK/KKS)**: some head mods bake a narrow shading ridge down the centre of the nose bridge into the face detail map (`cf_face_00_md`). It is authored in the card's data, but without a matching nose line it can read like one. The ridge is now detected per card and exposed as a presentation setting: `1` keeps the authored amount (default), `0` removes it, values in between are subtler and values above are stronger (gently capped so a normal map cannot clip to white). The Look tab has a **Nose ridge shading** slider, the CLI has `--nose-ridge F`, the effective value and the detected amount are written to the manifest's `facial_features` block, and the setting is persisted in the viewer. It never paints a stroke the source lacks: cards with no detected ridge are left byte-for-byte as authored, and the actual nose-line texture is still respected independently (`nose_line: false` on cards that have none). Added detection/preservation tests; 19,040 unit checks pass.

## [0.5.5] - 2026-11-22

### Fixed

- **Nostrils still missing in the live viewer (standard and MMD modes)**: the 0.5.3 fix corrected the texture and the software preview, but the viewer renders through Direct3D 11 (`GpuView::Render`), which had its own depth state. The separate `cf_O_noseline` decal mesh sits a fraction of a millimetre behind head mods' 3D nose surface, so the depth test discarded it even though the texture carried the nostrils. Face-paint decals (`toon_nose` shader family) now draw without depth testing in both the standard and MMD ("as exported") buckets, and their textures expose only mip level 0 so thin authored strokes are not averaged below the material's alpha cutoff. Verified with `cardbridge convert --gpu-view` on `KK_235599.png` in both the baked export view and the unbaked live-scene view: nostrils visible, no nose line introduced. 19,034 unit checks pass.

## [0.5.3] - 2026-11-22

### Fixed

- **Missing nostrils on cards whose nose texture has no nose line (KK/KKS)**: the `cf_O_noseline` texture carries two independent features — a tall centre nose line and short wide nostril arcs — but a single shared rule feathered every nose texture as if it were a line. On `KK_235599.png` the modded nose row has nostril arcs whose authored alpha peaks at 146/255; feathering dropped them to 84/255, below the material's alpha cutoff. The shader bake then produced fewer than 16 solid fragments, so CardBridge's "draws nothing" rule hid the whole noseline mesh in the viewer and the PMX. Nose features are now detected from the texture's own alpha shape: only line pixels are softened, nostril pixels keep their exact authored RGBA, and a card with nostrils but no nose line no longer loses them or gains a fabricated line. Face-paint decals additionally skip the depth test and sample level 0 (thin strokes were hidden behind head mods' 3D noses and lost to mip-averaging) and are exempt from the empty-bake hiding rule, which had also hidden genuine nose lines on other cards. Upper and lower lip-line strokes are detected separately and reported, with no feature gated on another. Presence/absence is exported in the manifest's `facial_features` block. Added unit tests for all four nostril/nose-line combinations, the stock base-game line shape and lip-line detection; verified on the supplied card plus two other cards. 19,034 unit checks pass. See `out/nostril_fix/before_after.png`.

## [0.5.2] - 2026-11-22

### Fixed

- **Translucent face / back of the head visible through the face in MMD mode (KK/KKS)**: the game builds the character's skin at run time and ignores the alpha channel of that texture — it is a coverage mask, not transparency. `MmdMaterialLook` classified any material whose covered texels included low alpha as a cutout (unless its shader name contained "opaque"), so the baked face was exported with alpha 0.99 and drawn in MMD's blended pass: no depth writes, geometry behind the face showing through, and dark triangular fragments around the mouth. A new structural `IgnoresTextureAlpha` helper recognises skin by shader family (KK/KKS `main_skin`) or as a shader-less create-job material on the character's body/head part, and keeps it opaque in the PMX and the plain viewer; genuinely translucent materials, hair cards, under-pieces and face FX are unaffected. The standard viewer's alpha test is unchanged. Face and body now export with alpha 1.0. Added classification and WARP-render regression tests; 19,006 unit checks pass. See `docs/MMD_FACE_OPACITY_FIX.md`.

## [0.5.1] - 2026-11-21

### Fixed

- **Dark face and black exposed skin in the MMD ("as exported") view (KK/KKS)**: the PMX edge was drawn after the surface. Skin materials export with alpha 0.99 (MMD's blended pass), so the viewer deliberately does not write depth for them — the edge then passed the depth test over the whole mesh and painted the face with its dark brown edge colour and the body with its black edge colour. The painted facial overlays have no edge flag, so they were correct, which isolated the defect to the plain renderer rather than the card, the bake or the PMX. The edge is now drawn before the surface as a back-face silhouette rim that the surface covers; the shared ring helper takes an optional rasterizer state so the standard viewer's outline path is byte-for-byte unchanged. Added a WARP-rendered regression test (blended, edged skin quad must show its surface colour, not the edge colour); 18,990 unit checks pass. No material defaults, grading, baking or PMX-writer code changed.

## [0.5.0] - 2026-11-21

### Added

- **Native PMX import with lossless round-trip (Phase 1 of the unified PMX editor).** `cardbridge pmx info <model.pmx>` reports a file's format and feature counts; `cardbridge pmx roundtrip <in.pmx> <out.pmx>` imports, re-exports and verifies structural preservation. The new `cardbridge::pmx::ReadPmx` reads PMX 2.0/2.1 in either text encoding with all index sizes, all five deform types (BDEF1/2/4, SDEF, QDEF), all eleven morph types, IK and bone flags, additional UV channels, display frames, rigid bodies and joints. `pmx::Model` now carries the source format and raw trailing sections so import → export preserves data CardBridge does not edit. Verified byte-for-byte on a real 3.78 MB PMX (31,942 vertices, 2 additional UV channels, 41 local-axis bones, 83 morphs, 14 display frames, 500 joints) and cross-checked with the engine's independent loader. New regression tests cover 2.0/2.1, both encodings, every deform and morph type and real-model round-trip (`CARDBRIDGE_TEST_PMX`). See `docs/PMX_IMPORT_ROADMAP.md`.

## [0.4.20] - 2026-11-21

### Fixed

- **Opaque KK/KKS garments rendered translucent or see-through in MMD view mode**: two pipeline defects turned intact clothes into look-throughs. (1) Garments whose game shader is the opaque family (`main_clothes_opaque`, `main_opaque`) were marked transparent when their textures carried stray low-alpha texels; the exporter now leaves these cutouts opaque and both viewers no longer discard their fragments. (2) A mod's `Shader Forge/main_alpha` outfit over a fully opaque atlas (e.g. the 时崎狂三 school uniform) was run through the UV-flat shader bake, which wrote alpha holes where the game's screen-space alpha test discards; MMD blended those holes over the interior lining and produced dark patches. The bake now keeps the authored opaque texture for such cutouts and the export writes them opaque. The GPU viewer's plain (MMD) mode now buckets materials by the exported material alpha, matching the PMX instead of the scene's alpha flag, so the interior lining can no longer sort through the garment. Verified on KK and KKS cards with the uniform mod: `cloth`, `skirt`, `sleeve` and `shoes_outer` PMX alpha 0.990 → 1.000 with raw opaque atlases, and the GPU-rendered as-exported preview is solid. Intentionally translucent materials (authored alpha holes, face overlays, sheer hair) keep their appearance. Added regression tests for both defects.

- **Black hair accessories in the as-exported view and PMX (KK/KKS)**: a third-party toon shader on a hair-accessory item had a binding table the parser could not read, so every constant resolved to zero and the shader bake multiplied its source by nothing, replacing the card's red/dark accessory texture with a solid black plate. The bake now detects a degenerate black result over covered texels while the authored texture carries colour and keeps that authored texture instead; the Unity built-in ambient constants (`unity_AmbientSky`/`Equator`/`Ground`) are synthesised like the existing SH ambient, and a shader whose forward pass has no parsed constant buffers stays on the simple viewer shader so the live view does not draw the accessory black either. The earlier fix that preserves bake-marked accessory material diffuse in the PMX is retained. No card name, shader name or accessory slot is hardcoded in the detection. Added a regression test for the black-bake guard.

## [0.4.15] - 2026-11-21

### Fixed

- **KKS `--gpu-view` access violation after shader upload**: Koikano shaders describe a texture that has no sampler state of its own with the sentinel `0xFFFFFFFF` in its resource record. `ParseKksTail` stored that sentinel as a real sampler register, and the GPU view passed it straight to `PSSetSamplers`; the D3D11 driver indexes its sampler array with that value and faulted with `0xC0000005`. The sentinel is now recorded as "no sampler" (the already-bound sampler is reused, which is what the shader intends), and all shader-derived texture/sampler registers are validated against the D3D11 slot limits before any bind, with a load-time warning for a malformed table. Both KK and KKS use the same validation; KK shaders carry ordinary sampler indices and are unaffected. Added stage/resource diagnostics behind `CARDBRIDGE_DEBUG_GPU` (game family, shader/texture/material/mesh counts, per-draw shader and binding registers) so a future driver fault can be traced to the last successful operation without a debugger. No material, texture, shader feature or colour-processing path was disabled.

## [0.4.14] - 2026-11-21

### Fixed

- **MMD material colours differed from the game view (KK and KKS)**: the game's AmplifyColor LUT is a *frame* post-process, so a PMX has to carry it in its colour textures. Commit `87e18b9` established that rule, but the fallback that grades an unbaked texture ran only in the PMX writer — the viewer's "as exported" (MMD) path grades the frame only in Lit mode and sampled those same unbaked textures raw. KKS `Koikano/` materials (which deliberately skip the UV-flat shader bake) and create-job textures therefore appeared ungraded in MMD mode while the exported PMX was graded, and a `main_item` cutout escaped grading entirely. A single `SceneTexture::colourGraded` flag and one `GradeTexture` helper now govern both paths: the export bake grades every still-ungraded sRGB texture in the scene copy, and the PMX writer grades only what is still ungraded. Masks, normal maps and toon/sphere maps (`srgb == false`) are never graded, alpha is preserved, and the grade is applied exactly once. Non-MMD rendering and the existing KK bake are unchanged.
- **Inner garments not hidden by the outer garment (KK and KKS)**: list rows carry three visibility masks — `OverBodyMask`, `OverBraMask` and `OverInnerMask`. CardBridge resolved and applied only the first two, so an outer top sub-part (a jacket) never cut away the inner shirt beneath it and the shirt's full sleeves were exported. The inner mask is now resolved like the others from the same list column, and applied to the inner garment of the same card-defined `subPartsId` group in that garment's own UV space. Only the covered triangles move into a hidden under piece (restored by the nude switch); the visible collar, hem and sleeves stay, and no slot numbers, item ids or outfit combinations are hardcoded.

## [0.4.12] - 2026-11-21

### Fixed

- **Clothing stuck in T-pose while the body is posed (KK and KKS)**: some garments ship a `SkinnedMeshRenderer` whose bone list is longer than the mesh's inverse bind-pose table (the reproduction KKS sailor top has 115 bones but only 100 bind poses, with no vertex actually using the extra slots). The shared skinning gate rejected the whole mesh whenever the table was shorter, so the garment was placed rigidly on the prefab root and never followed the pose — the sleeves stayed horizontal while the arms moved down, and the garment could sit misaligned. The gate now accepts any mesh with weights, bones and a non-empty bind table; influences naming a bone past the table are dropped (and reported), while all other vertices keep their correct bind and follow the same evaluated skeleton as the body. This is one shared correction for every clothing slot, garment-as-accessory and both game formats — no per-card or per-slot workaround. Regression tests cover the mismatch, the rigid fallbacks and pose-following; the PMX export carries the restored arm-bone weights.

## [0.4.11] - 2026-11-20

### Fixed

- **Overlapping layered clothing / capes**: two copies of the same garment - a shell and its coloured lining, or one cape worn in two accessory slots - are authored a hair apart, so CardBridge drew them as one flickering, intersecting mesh. A new shared assembly pass (`SeparateCoincidentLayers`) detects such pairs from geometry and card metadata alone (same source item id, matching topology and silhouette, >=85% surface coverage within 2 cm) and shifts the outer copy a few millimetres off the inner one as one rigid piece. Both garments, their colours, weights, UVs and authored drape are preserved; normal layered clothing, footwear and parts from different items are untouched. The correction runs before both the preview and the PMX export, so they stay identical, and per-bone frames are re-derived so re-shaping agrees. `CARDBRIDGE_NO_LAYER_SEPARATION=1` restores the old behaviour for A/B comparison.

## [0.4.10] - 2026-11-20

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
