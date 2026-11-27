# Changelog

All notable changes to CardBridge are documented here, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). CardBridge did not keep a changelog before 0.3.0; entries for earlier releases were reconstructed from the git history and the release notes that shipped with those releases, and are not expanded beyond what those show.

## [0.12.0] - 2026-11-27

### Added

- **Phase 7 foundation — card-free Koikatsu body extraction and append-only PMX body restoration.** `assemble::LoadBodyTemplate` reads the skinned body mesh, bone names and rest positions from `<library>/abdata/chara/oo_base.unity3d` through the existing `Resolver`/`UnityFile` primitives — no card and no running game. `pmx::RestoreBody` maps KK/KKS bone names onto an imported PMX's bones, fits translation and a bounded uniform scale from matched rest bones, and appends body vertices, indices and one material to the PMX document; original vertices, morph offsets, frames, physics and format metadata remain untouched. Unmatched skeletons are rejected with an explicit compatibility error instead of silently changing proportions. CLI: `cardbridge pmx restore-body <in.pmx> <out.pmx> --library <dir>`. Tests cover append-only preservation, incompatible-skeleton rejection and real-library body extraction (`CARDBRIDGE_TEST_LIBRARY`). See `docs/PMX_IMPORT_ROADMAP.md`.

## [0.11.1] - 2026-11-25

### Fixed

- **Selective clothing import no longer resets the viewer's part edits.** Moving, rotating or resizing a part with the gizmo (and deleting a part with the row's `x` chip) used to be wiped when the import reloaded the card: the reload cleared those edits unconditionally, even though they are keyed by stable part id and a same-card reload is meant to keep pose tweaks. They now survive the import and keep applying to every part that is still there; edits for garments or layers that are gone are dropped, as with a pose file. Verified with the scripted viewer: three part edits stayed at three across a selective import that was confirmed to reload and pin the imported outfit (the original code reset them to zero); 19,540 unit checks plus 100 format checks pass.

## [0.11.0] - 2026-11-25

### Added

- **Bra-Aware Breast Physics (on by default).** The exporter classifies the worn garments from card metadata, mesh geometry, skinning and spatial coverage before writing physics, and applies a firmer, better-damped, less-travelling breast profile only when a supportive bra is detected with high confidence. A normal shirt, pasties, a one-sided or translucent cover, an uncertain/unfamiliar garment, a missing mod item, and the new opt-out (`--no-bra-physics` / `CARDBRIDGE_NO_BRA_PHYSICS=1`, or the Look-tab switch) all keep the existing breast physics exactly as before. The supported profile is a derived copy of the tuned Subtle preset, scoped to the body's own breast chains, so hip/hair/garment physics, colliders, bones and geometry are untouched. KK and KKS share the path. 19,540 unit checks plus 100 format checks pass; real KK/KKS exports were compared body-by-body and joint-by-joint, and `pmx roundtrip` preserved structure.

## [0.10.0] - 2026-11-25

### Added

- **Delete and resize any part in the viewer.** Every part row now has an `x` chip: it removes that part from the live preview and from every export (PMX, glTF/GLB, OBJ), and the row disappears from the list. Deletes are part of the pose state, so `Ctrl+Z` restores them, "show every part" clears them, and pose files carry them (`deleted_parts`) for older files to stay valid. Each part with geometry also gains a third gizmo chip: drag a square X/Y/Z handle in the view to resize the part along that axis about its own bind-space centre. The scale rides with the bones like the existing move/rotate edits, is baked into the exported scene, survives save/load and undo, and right-clicking the chip resets just the scale (move and rotation are kept). Positions, normals and tangents use the inverse transpose under non-uniform scaling in both the Direct3D and software previews. 19,503 unit checks plus 100 format checks pass.

## [0.9.4] - 2026-11-25

### Changed

- **Non-PMX export targets are labelled as work in progress.** The viewer's format chip shows `WIP glb` / `WIP gltf` / `WIP obj` in amber (PMX stays a plain supported `Fmt: pmx`), the progress/log text says `(WIP)`, and the CLI help plus README say the same. No export behaviour changed.

## [0.9.3] - 2026-11-25

### Fixed

- **glTF files could not be imported in Blender ("Couldn't parse glTF").** Three spec violations in the generated document: the default scene's `nodes` array was empty (nothing to import), morph targets were wrapped in `{"attributes": {...}}` instead of being a flat attribute dictionary, and sparse morph POSITION accessors lacked the required `min`/`max`. Identity bone matrices are also omitted now. Verified with Blender 5.3's standard glTF importer in headless mode: GLB and glTF both import with geometry, armature and morph targets; OBJ imports with geometry. The Khronos glTF validator reports **0 errors**. 19,493 existing checks plus 100 format checks pass.

## [0.9.2] - 2026-11-25

### Added

- **`--simplify N` now applies to glTF/GLB/OBJ exports too** (it used to be PMX-only), and the viewer's Optimize tab applies it to whichever format is selected. It reuses the same copy-on-write simplifier (UV/normal/material seams, 4-influence weights and blend shapes are preserved), so a lighter model can be written for viewers with triangle budgets without touching the scene being edited. 19,493 existing checks plus 96 format checks pass.

## [0.9.1] - 2026-11-25

### Fixed

- **Viewer toolbar cropped on the right after adding the format chip.** The new chip widened the header row past the 380 px panel (`asExported` ended at x=450), so the rightmost buttons were cut off. The toolbar is compacted to fit within x=370 (10 px margin) and the chip shows just `Fmt: pmx|glb|gltf|obj`.

## [0.9.0] - 2026-11-25

### Added

- **Industry-standard export formats: glTF 2.0 / GLB and OBJ / MTL.** CardBridge is no longer exclusively a Koikatsu-to-MMD converter. The new `--format glb|gltf|obj` option (and the viewer's **Format** chip) writes from the *same canonical assembled `Scene`* the PMX exporter consumes — no second card parser, no duplicated mesh/material/texture/skinning logic. glTF carries geometry, normals, derived tangents, UV0–3, metallic-roughness materials with PNG textures (embedded in GLB, external for `.gltf`), alpha modes, texture transforms, mesh nodes, a joint hierarchy with inverse bind matrices, 4-influence skinning and named **sparse** morph targets; no animation array is written because the canonical scene has no animation source data. OBJ/MTL writes static geometry, groups, per-mesh objects, material assignments and external textures, and reports what a static format cannot carry. Coordinates use one shared left-to-right-handed conversion (mirror X, flip winding and V, Y-up, metres) applied to geometry, bones and morph deltas together. Output is atomic (temp file + rename) with cancellation and named limitation warnings in `cardbridge-export.json`. PMX is completely unchanged and remains the default. Architecture, conversion policy, library evaluation and compatibility matrix: [`docs/EXPORT_FORMAT_ARCHITECTURE.md`](docs/EXPORT_FORMAT_ARCHITECTURE.md). FBX and USD were evaluated and deliberately deferred (no licence-compatible native writer fits the standalone architecture). 19,493 existing checks plus 83 new format checks pass.

## [0.8.7] - 2026-11-25

### Fixed

- **Anatomy drew as a flat white plate in the viewers ("where must I go?" UI).** The game binds its runtime skin texture to `cf_m_mnpb` / `cf_m_mnpa`; the assets carry no texture binding and the SM4 preview cannot evaluate the procedural shader, so the mat showed its authored diffuse (0.98, 0.90, 0.87) and MMD's toon lighting saturated that to white. The extraction now reuses the body's own baked skin texture with a lower-abdomen UV sampled from the same card's body mesh; the panel's White-mode props and the material look still match the body. (`src/assemble/Assembler.cpp`)

## [0.8.6] - 2026-11-25

### Fixed

- **Kk/KKS genital anatomy (`o_mnpa`/`o_mnpb`, `o_dankon`/`o_gomu`) was dropped during conversion.** The body assembly loop accepted only the renderer named `o_body_a`, so every other skinned renderer under `p_cf_body_00` silently disappeared; with it went the game's own genital geometry for both sexes (KK ships both sets in one prefab and shows the one matching `Parameter.sex`). The body skin pass is unchanged; the anatomy pieces now go through the same renderer/mesh/bone pipeline as a new `body.anatomy` part, keeping their own materials and textures (`body.anatomy/o_mnpa` 82 tris, `body.anatomy/o_mnpb` 114 tris; bones `cf_s_ana`, `cf_j_ana`, `cf_j_kokan`, `cf_s_waist02`, …). Male cards now export `o_dankon`/`o_gomu` (257+361 verts, 476+658 tris). Verified on the KK female card `151506026_p6.png` and the KKS card `142609856_p20.png`.
- **Anatomy previously rendered black in the shaded/GPU views.** The game's anatomy material `cf_m_mnpb` has no texture; the SM4 emulator evaluates its procedural shader to black, so the browser of `BakeLostColour`-style "lost colour" produced a black bake and the Preview/GpuView rendering drew nothing. Anatomy meshes now take a plain constant-colour path (structural detection by shader family, no per-character names or character paths), and the "bake came back all black with no texture to fall back to" case keeps the material's own diffuse colour. Regression test added for the classification rule.

## [0.8.5] - 2026-11-25

### Added

- **Part gizmo for every scene part.** The move/rotate gizmo is no longer limited to accessories: any part row can now move or turn its own vertices as a rigid bind-space edit, with the pivot at the part's bind-space centre. The edit stays glued to the character through bone poses, is applied in both GPU and software previews and baked into the scene copy on export. Pose files carry an optional `parts` object (older files stay valid) and undo sees each gesture as one step.

### Fixed

- **Black band on the swimsuit pareo's hem in the MMD / "as exported" view (Yoko Littner, `KK_237456.png`, outfit 4).** The 0.8.0 alpha-shader classification correctly stopped treating every depth-writing `main_alpha` material as a blend, but the pareo's hem samples discarded texels in 3.99% of its covered UVs — just below the exporter's 5% cutout proof. It was therefore treated as "alpha test solid": the black atlas padding at the hem was solidified into an opaque black band. A depth-writing alpha shader really discards sub-`_Cutoff` texels, so for those materials the proof threshold is now 1% (5% is kept for every other material). The 0.8.0 alpha-shader, lining and `_AlphaMask` fixes are unchanged. Verified in the PMX preview and the live viewer's MMD mode: hem black pixels 24.95% → 0%, matching the 0.7.11 reference; 19,473 unit checks pass.
- **Viewer no longer shows Windows "Not Responding" while cards/outfits load.** The card, ZIPMOD indexing, parsing, mesh building and texture decoding already ran on worker threads, but the UI thread still painted a full software-shaded viewport frame right after a load — measured at 2.1 s on one card — and then uploaded the baked export view's shaders/textures synchronously. The viewport now skips software shading and scene rasterisation while a load owns the progress bar, and the export preview is uploaded into a private `GpuView` on the bake worker and adopted on the UI thread by a move (generation-guarded so stale results cannot overwrite a newer character). The scene snapshot for the bake is also taken on the worker under the existing scene lock. Measured longest UI-thread frame during loading dropped from 2.1 s to 13-66 ms, with zero unanswered `WM_NULL` polls during loading (the existing startup device init still takes ~0.5 s). Added per-stage `max` timing (JSON `max` field), a `tools/measure_ui_responsiveness.ps1` harness, and tests for the recorder's max aggregation and the movable GPU view. 19,473 unit checks pass.

## [0.8.0] - 2026-11-24

### Added

- **Phase 3 — card-independent Koikatsu Asset Library.** A new `library::AssetCatalog` indexes every base-game list row and every installed KK/KKS zipmod into a searchable catalog (clothing, hair, accessories, body, textures, other) with display names, source package/GUID, bundle/asset references, attachment node, texture dependencies and authored flags. The cache is keyed on the library list files and the mods-folder fingerprint, so repeat opens cost no game-file reads. `cardbridge assets build|list --library ... --mods ... [--category ...] [--search ...] [--json]` exposes it from the CLI, and the viewer has an **Assets** tab with search, category chips, a result list, a metadata/preview card and decoded `ThumbTex` images when the source asset has one. The catalog needs no running game and no loaded character. See `docs/PMX_IMPORT_ROADMAP.md`.

## [0.7.35] - 2026-11-24

### Fixed

- **Black cape restored**: the coincident-layer separation pass (0.4.11) shifted the outer cape a few millimetres off its lining on the Megumin Outfit card, which hid the black shell in the GPU viewer and left only the yellow lining visible. The pass is no longer called during assembly, so both copies are placed exactly as the card authors them. Both capes and their original colours are still in the PMX and the software preview; the trade-off is that the original z-fighting between the near-coincident copies returns.

## [0.7.33] - 2026-11-24

### Added

- **Body Proportion Transfer**: a dedicated **Transfer** tab measures a reference card from its assembled skin and bones and transfers any selection of 14 measurements (height, head size, head-to-body ratio, bust/waist/hip, shoulders, neck, arm/upper-arm/lower-arm, leg length, thigh, calf) into a separate, non-destructive layer. The target keeps its face, hair, clothes, textures, materials, accessories and bone names; Apply re-assembles so clothing masks and attachments follow the new body, and the exported PMX plus manifest record what was applied. Includes before/after preview, reset, absolute (non-accumulating) values, measured refinement with honest residuals, and `cardbridge measure` / `cardbridge transfer` / `convert --transfer` for batch use. [`docs/BODY_PROPORTION_TRANSFER.md`](docs/BODY_PROPORTION_TRANSFER.md). 19,395 unit checks pass.

### Fixed

- **Transparent socks / pantyhose came back in the exported PMX (Yoko Littner, `KK_237456.png`)**: the 0.7.11 fishnet fix stopped binarising every `main_alpha` material in Unity's transparent queue (>= 3000). The Roy12 knee socks (`cf_m_socks_knee01`) are also in that queue (material override 3050, shader 3040) but are an alpha TEST: ZWrite On, a uniform ~190/255 tint over the skin and real alpha-0 cutouts. MMD read the 190 as opacity and blended the stockings with the body through them. The exporter now uses the shader's depth write to tell an alpha test from a genuine blend — a depth-writing material with mesh-sampled cutouts gets its binary 0/255 mask back, while ZWrite-Off transparent materials (the fishnet) keep their authored fractional alpha. Verified on a fresh export of `KK_237456.png`: the socks texture has only alpha 0/255 and no mid-alpha tint. See `docs/MMD_PANTYHOSE_ALPHA_FIX.md`.

## [0.7.32] - 2026-11-24

### Fixed

- **Switching between KK and KKS cards no longer rebuilds the mod index.** Indexing was a side effect of resolver creation, keyed on the library+mods pair; changing the card's game swapped the library path and was mistaken for an indexing dependency. The mod database now has its own cache keyed only on the mod source (with a cheap path/size/mtime fingerprint for added, removed or edited zipmods), while the card's game is only a lookup filter. A concurrent card load or profile switch cannot start a second indexing job. Explicit reindex, different mod folders and KK/KKS compatibility resolution are unchanged. Eight regression tests cover both switch directions, repeated switching, source/file changes, manual reindex and concurrent loads; 19,341 unit checks pass.

## [0.7.31] - 2026-11-24

### Fixed

- The UV1-atlas glove sleeve is drawn from both sides: its material is now double-sided in the viewers and the PMX (MMD look flags include `0x01`), so looking along/inside the arm shows a solid sleeve instead of a see-through sheet. 19,296 unit checks pass.

## [0.7.30] - 2026-11-24

### Fixed

- The UV1-atlas sleeve is complete again: dropped the per-triangle removal introduced in 0.7.28, which punched holes in the inside and outside of the glove sleeve. The atlas padding is still cut per fragment by the alpha test, so all views and the PMX keep agreeing without changing the authored garment geometry. 19,296 unit checks pass.

## [0.7.29] - 2026-11-24

### Fixed

- **The body's red vein / body-paint texture is now mapped onto the skin.** KK/KKS bodies leave UV0 at (0,0) for ~80% of their vertices and author the real texture layout in UV1; we sampled UV0, so the create-job texture shown in the Material Workbench never reached the rendered body. The body mesh now uses its UV1 layout as the primary UV too, and body-paint textures are sampled at LOD 0 in the viewers so the thin strokes survive mip reduction. Verified on KK_351156: the red veins appear on the legs/arms in the as-exported view and the PMX carries the same UVs. 19,296 unit checks pass.

## [0.7.28] - 2026-11-24

### Fixed

- **The card's long skirt is back in the viewer.** A normal card load again keeps its coordinate-specific ABMX (e.g. `cf_d_sk_top` Y=2.0), matching the card picture and the CLI export. The assembler now clamps only the inward X pinch of skirt bones (`cf_d_sk_*` X<1) that used to pull the apron/skirt into the body, so the 0.7.24 clipping fix is preserved: verified on Oosuki Mamako's outfit 7 as well as the supplied card.
- **The long, pinstriped hand sleeve is back without the black bell cuffs.** Garments that author their real layout in UV1 (the base game's maid gloves have a degenerate UV0) now use UV1 as their main UV, so the create-job atlas is sampled where the garment actually is. The atlas padding is proven as a real alpha cutout and applied in the Lit view, plain/MMD view and PMX; triangles not fully covered by the authored texels are dropped, leaving the smooth sleeve to the wrist instead of black ruffles and slivers.
- Regression coverage: a partially cut UV1-atlas garment keeps its sleeve rather than being removed wholesale. 19,296 unit checks pass.

## [0.7.27] - 2026-11-24

### Fixed

- The export's shaded preview now recognises every texture written by the shader bake, including the CLI's colour-only (`lit=false`) bake, so the dress no longer falls back to a flat blue plate. Hands and feet still use the KKS skin shader, which is deliberately not baked; see the remaining limitations in the investigation notes. All 19,295 unit checks pass.

## [0.7.26] - 2026-11-24

### Fixed

- **KKS game-shader views no longer draw body, hands and clothes as flat black/blue plates.** KKS 2019 shaders declare several constant buffers that the SM4 programs address as `cb0..cbN` in declaration order, but the shader parser assigned them all to `cb0`. Unrelated builtin globals then overwrote the material's colours and texture transforms at the same byte offsets. The parser now keeps each buffer's own slot, the constant-buffer filler clears a slot only once, and vertex-stage registers are cleared between vertices. Verified on card KK_351156 (Matou Sakura alter): the Lit viewer and the headless shaded preview again show the card's dark-red dress instead of a flat blue sheet.
- **The exported PMX no longer draws fully alpha-clipped garments as black plates.** A create-job clothing atlas can pack a garment into a transparent cell; a mesh whose UVs land entirely in discarded texels is now detected once at assembly and drawn nowhere (Lit, plain/MMD, shaded preview or PMX), instead of being alpha-solidified into black cuffs and ruffles. The "opaque" shader-family name no longer overrides that proof. Verified on KK_351156: the unwanted maid-glove cuffs disappeared from the Lit view, the MMD view and the exported PMX, with the dress, skin and genuine cutouts preserved.
- The export's shaded preview no longer re-runs a game shader on an already-baked (lit) texture, which used to crush the outfit to a flat blue plate.
- Regression tests: fully-cut opaque-named garments keep their cutout in MmdMaterialLook, separate constant buffers keep their values at colliding offsets, and the existing 19,295 unit checks pass.

## [0.7.24] - 2026-11-24

### Fixed

- **Garments no longer clip into the body on card load (the shared clothing fitting regression).** A normal viewer load again clears the shown outfit's coordinate-specific ABMX slot before assembly. Those mods (e.g. `cf_d_sk_top` X=0.755 on Oosuki Mamako's outfit 7) are authored for the game's own coordinate switch, where the cloth is re-fitted against that coordinate; applying them to CardBridge's static rig pulls skirt/apron vertices inside the body. All outfit categories benefit from the shared clear; imports still merge or replace the slot through the normal ABMX path. The nipple/areola coverage fix and every texture/render improvement from 0.7.12-0.7.22 are preserved. Verified on the supplied card: the apron silhouette is 159 px wide at the hip after the fix vs 135 px before, matching the 0.7.11 reference; 19,285 unit checks pass.

## [0.7.22] - 2026-11-24

### Fixed

- **The MMD / "as exported" view no longer draws an extra, differently-shaded white bang.** KK/KKS hair draws its strand pattern through `_ColorMask` and leaves `_MainTex` unset, but `BakeShaderTextures` skipped every `Koikano/*` shader, so hair reached the plain GPU renderer with no texture at all and came out as a flat white cap over the head. Hair shader families are now baked like the rest, giving both the view and the PMX the card's lavender strand colour and detail. Verified on card KK_351156; 19,282 checks pass.
- **Opaque-shader-family garments keep their depth writes in the MMD view.** The exported-alpha bucket now keeps `main_opaque` materials in the opaque pass instead of blending them over the body, so skirts no longer show the underwear through them.

## [0.7.17] - 2026-11-23

### Fixed

- **Viewer keeps the card's authored outfit fit (long skirts no longer render knee-length).** A normal card load no longer clears the outfit's coordinate-specific ABMX bone mods; the authored skirt/fit scale (e.g. `cf_d_sk_top` Y=2.0) is preserved so the viewer matches the original card and the CLI export. Clothing/coordinate imports still clear or merge the target slot through the existing ABMX merge path.
- **Opaque outfits no longer read see-through in the baked / "as exported" view and PMX.** The body's nipple/areola overlay is now masked when the shown outfit wears a top (new structural `card::OutfitWearsTop`), reproducing the game's coverage behaviour; the pale discs that bled through opaque cloth are gone. Nude outfits keep the card's own nipple shape. Genuine cutouts and transparent-queue (fishnet) materials keep their holes / fractional alpha.
- **Cutout garments no longer read as see-through panels in the MMD / "as exported" view.** Binary-alpha (0/255) cutout atlases are now sampled at LOD 0 (no mip averaging) in both the fallback/MMD and Lit GPU paths, so the black island padding is not blended into partial alpha. The long skirt renders solid like the card, keeping only its authored thin slits; fractional-alpha (fishnet) materials keep mip mapping.
- Regression tests: `OutfitWearsTop` (worn top vs nude) and default-load preserving coordinate ABMX while imports still clear only the target slot. 19,282 unit checks pass.

## [0.7.16] - 2026-11-23

### Fixed

- The viewer's upscaler controls are now labelled **WIP** (badge on the switch, note in the panel and docs). Upscaling is export-only, needs external tools/models installed, and is not part of the MMD preview. No functional change.

## [0.7.15] - 2026-11-23

### Added

- **Selectable upscaler model**: `--upscale-model waifu2x|realesrgan` and the viewer's Look tab "Model: Waifu2x / Real-ESRGAN" chips. Real-ESRGAN uses the x4plus-anime ncnn model and is usually sharper on line art. Before the model runs, fully transparent texels have their RGB filled from the nearest covered texel (the cutout atlas stores black there), preventing black bleed at edges. 19,274 unit checks pass; verified on the Yoko outfit-7 bra atlas in MMD-bound exports.

## [0.7.14] - 2026-11-23

### Added

- **Optional open-source texture upscaling**: `--upscale N` (2..8) and the viewer's Look-tab "Upscale PMX textures 2x" switch post-process exported colour PNGs with waifu2x-ncnn-vulkan (MIT). Auto-detects the tool via `--upscaler`, `CARDBRIDGE_UPSCALER`, `tools/upscaler/` or PATH; without it the export is unchanged. Toon and sphere maps are excluded. The anime model interpolates alpha, which antialiases hard cutout edges (0.52% edge texels on the Yoko outfit-7 bra atlas; 17 textures, 2.3 → 7.6 MB). Upscaling cannot restore source detail; see `docs/TEXTURE_QUALITY_AND_RESOLUTION.md`. 19,271 unit checks and the viewer smoke test pass.

## [0.7.13] - 2026-11-23

### Added

- **Viewer bake-quality control**: the Look tab now has a "Texture quality" preset row (Standard / High / Ultra) at the top, driving both the PMX export and the "as exported" preview through the same settings as `--bake-quality`. Switching presets reloads the card so shader maps use the new cap and discards the cached export preview. `tools/viewer_smoke.ps1` now clicks High and verifies the state; a private off-screen click test confirmed `0 → 1`. See `docs/TEXTURE_QUALITY_AND_RESOLUTION.md`.

## [0.7.12] - 2026-11-23

### Added

- **Configurable shader-bake texture quality**: `--bake-quality standard|high|ultra` and `--bake-size N` (64..8192). `standard` (1024, one sample per texel) is byte-identical to earlier exports; `high` (2048) and `ultra` (4096) add 2x internal supersampling with a single area-weighted downsample, UV-island-aware padding that cannot bleed between touching islands, and area-resampled shader maps at the preset cap. Nothing is upscaled beyond its source, and internal rendering is capped at 4096 per side so Ultra stays practical. 19,266 unit checks pass, including new resolution/resample/preset regression tests; a base-game card exported with `standard` is byte-identical to the pre-change build apart from the version string. See `docs/TEXTURE_QUALITY_AND_RESOLUTION.md`.

## [0.7.11] - 2026-11-23

### Fixed

- **Fishnet stockings exported as flat black (Yoko Littner, `KK_237456.png`, outfit 7)**: `cf_m_panst_02` is a `main_alpha` material in Unity's **transparent queue (3040)** — it blends its dark ~200/255 alpha over the skin, which the viewer draws as fishnet. The exporter was binarising that alpha at `_Cutoff` for every `main_alpha` material, turning the mesh into an opaque black sheet in MMD. Binarisation (and alpha-solidification) now apply only to alpha-TEST queues (<3000); transparent-queue materials keep their authored fractional alpha and MMD's 0.99 blended pass. Verified on a fresh export of outfit 6 (viewer "outfit 7") and 19,219 unit checks, including a fishnet-alpha regression test. See `docs/MMD_PANTYHOSE_ALPHA_FIX.md`.

## [0.7.10] - 2026-11-23

### Fixed

- **Black band above the shorts' waist in exported PMX (Yoko Littner, `KK_237456.png`)**: two `main_opaque` materials are really alpha tests (`_Cutoff` 0.5, queue 2450) — the bloomers (33.4% of covered UVs discarded) and the jacket (21.5%). A 25% detection threshold caught only the first, leaving the jacket's black texels to paint the band. The shader bake now proves the clip from its own output at 5% of the covered texels for any material, card or game texture, so both are cut out; the viewers and PMX writer respect the proof, and clipped texels no longer draw black over the body. Cutout bakes get a `_cutout` texture filename so a solid-alpha PNG cached by MMD from an earlier export cannot bring the band back. Verified with a software render of the PMX (skin down to the belt, no holes) and 19,213 unit checks. See `docs/BLACK_OUTFIT_CUTOUT_FIX.md`.

## [0.7.9] - 2026-11-23

### Fixed

- **Non-ASCII character names became underscores in the exported PMX file name (KK/KKS)**: the exporter sanitized the file stem byte by byte and replaced every byte outside ASCII alphanumerics with `_`, so `프린츠 오이겐` exported as `___________________.pmx` while the output folder kept the correct Korean name. Naming now goes through `fileio::SafeFileName`, which decodes UTF-8 scalars and replaces only characters Windows forbids in a file name (`< > : " / \ | ? *` and control codes). Korean, Japanese, Simplified/Traditional Chinese, spaces and punctuation are kept byte for byte - no transliteration or normalization - and the 80-character cap now counts characters rather than bytes so it cannot cut a multibyte scalar in half. Verified with `KK_238563.png`: it now writes `프린츠 오이겐.pmx` inside `프린츠 오이겐`; 19,207 unit checks pass, including new regression tests for Korean, Japanese, Chinese, English and mixed names and a real Windows file-creation check.

## [0.7.8] - 2026-11-22

### Fixed

- **Transient `cannot create ...textures/*.png` failures during export**: on Windows an antivirus or the search indexer can hold a just-created or just-deleted PNG briefly, so the `ofstream` open in `fileio::WriteFile` failed with a sharing violation. A re-export deletes and rewrites the same texture names, which made this intermittently fail unit tests (and could fail a real conversion). `WriteFile` now retries the open/write for up to ~140 ms before giving up; a successful write is unchanged.

## [0.7.7] - 2026-11-22

### Fixed

- **Pantyhose / stockings dithered in the interactive viewer while the PMX exported correctly (KK/KKS)**: `MmdMaterialLook` classifies an alpha-test material whose mesh samples no discarded texel as solid, and the PMX writer solidifies its texture alpha — but the viewer bound the baked texture directly, reading its 217–245 alpha as translucency and showing the body through the tights as a crosshatch. The material decision now lives in one shared helper (`IsAlphaTestSolid`) used by both paths, and `SolidifyAlphaTestSolidTextures` gives the viewer's scene the same opaque texture the PMX gets. The baked RGB (the subtle sheen) and all UVs are preserved byte for byte; genuine cutouts and authored translucent materials are untouched.
- **Rear hair see-through from a false alpha-cutout classification (KK/KKS)**: `HasAlphaCutout` declared a material a cutout on the first low-alpha texel a triangle centroid landed on. A baked hair atlas has alpha-0 padding between strand islands and jagged strand tips, so the main rear-hair submesh was exported with diffuse alpha 0.99, lost depth writes and showed the background through it. A structural rule now treats materials with no alpha flag, no translucency and an opaque render queue as opaque (the game ignores their texture alpha), and cutout detection needs at least 5% of the mesh's own samples to be discarded. The whole rear hair now exports opaque with solidified textures, while genuine alpha-test hair and lace still keep their masks. Verified on `KK_176361.png`, `KK_235599.png` and `KK_101895.png`; 19,183 unit checks pass. See `docs/REGRESSION_INVESTIGATION_2026-11-22.md`.

## [0.7.4] - 2026-11-22

### Fixed

- **Black bands on alpha-test garments in exported PMX (KK/KKS)**: a shader whose name contains `opaque` can still be an alpha test (`_Cutoff`, queue 2450) and discard texels of a modded garment's texture (a card-supplied MainTex or a game texture). The name-based rule solidified those discarded texels' black RGB, painting a band above the shorts on `KK_237456.png` (Yoko Littner) and along dress openings on `KK_388778.png`, neither of which the game or viewer shows. The shader bake now proves the clip from its own output — for any material the name rule would otherwise solidify — when at least 25% of the covered texels are discarded, so stray low-alpha atlas texels still leave intact clothes solid. The PMX keeps a binary cutout mask and MMD's 0.99 pass, and the software/GPU viewers discard the same texels. Verified on `KK_237456.png`, `KK_388778.png` and `KK_235599.png`; 19,181 checks pass. See `docs/BLACK_OUTFIT_CUTOUT_FIX.md`.
- **Holes under clipped garments in the exported body**: the assembly's `always` / `kokan` under-pieces carry body triangles moved out of the body mesh, so hiding them while covered opened holes wherever a garment clipped alpha. They now stay drawn; only under-pieces covered by a named garment are hidden.

## [0.7.0] - 2026-11-22

### Added

- **Phase 2 — native PMX models open directly in the CardBridge viewer.** `Open` (and drag-and-drop, and the command-line path) now accepts `.pmx` alongside Koikatsu cards. A native `.pmx` goes through a new `pmx::BuildScene` bridge into the same `assemble::Scene` the rest of the editor uses, so the Bones tab and posing, Morphs, Material Workbench, viewport renderer and gizmos work on it unchanged. The Parts tab becomes a **Model Explorer** for these models (meshes, materials, bone hierarchy, morphs, physics, textures, display frames), with card-only panels clearly marked as unavailable. Export writes from the original PMX document with the viewer's material edits applied — it is never rebuilt through the card pipeline — and copies textures into a self-contained `textures/` folder. Everything source-specific (additional UVs, SDEF/QDEF, local axes, morph kinds, IK, rigid bodies, joints, display frames, soft-body bytes, format/encoding) is still preserved. Five new tests cover the bridge, material-edit round-trip and self-contained export; the supplied real PMX is checked end to end (`CARDBRIDGE_TEST_PMX`). See `docs/PMX_IMPORT_ROADMAP.md`.

## [0.6.7] - 2026-11-22

### Fixed

- **Pantyhose / stockings translucent in exported PMX: MMD's stage grid visible through the tights (KK/KKS)**: KK's `main_alpha` and KKS's `main_clothes_alpha` are alpha-TEST shaders — they discard texels under `_Cutoff` and draw everything above it solid. Their baked textures carry the shader's output alpha (about 190/255 on the pantyhose), which MMD reads as *opacity*: with the material's 0.99 blended pass the tights rendered at roughly 0.74 and showed the stage through them. The exporter now writes the PMX texture alpha as a binary 0/255 cutout at the shader's `_Cutoff` for those materials (keeping MMD's 0.99 pass so real holes vanish), and exports alpha-test materials that never sample a discarded texel as fully opaque with solid alpha. Genuine translucency (`translucent` materials), face overlays, hair cards, opaque shaders and the skin's coverage mask keep their existing handling. Alpha-test cutout detection remains mesh-sampled, so stray low-alpha atlas texels do not turn a garment see-through. Verified on `KK_237456.png` (the supplied Yoko Littner card, with the RX21 mods): the socks texture went from 190 mid-alpha texels to a clean 0/255 mask with no intermediate values; all 19,061 unit checks pass, including new KK/KKS-aware export tests for the cutout, solid-tint and preserve-transparency paths.

## [0.6.5] - 2026-11-22

### Fixed

- **Nose-ridge slider never freezes the viewer**: dragging now only rewrites one small texture for live feedback. The full skin re-bake is deferred until you release the slider and confirm it in a Yes/No popup; declining restores the previously applied value and live texture. Build and 19,056 unit checks pass.

## [0.6.4] - 2026-11-22

### Added / Fixed

- **Two independent nose-ridge controls**: the original **Nose ridge shading** slider is back on its full re-apply path (1 = as authored, 0 = off, 2 = stronger), and a separate **Nose ridge opacity** layer was added beside it (0 = invisible, 1 = fully visible) that applies live to a single small texture. `--nose-ridge-opacity F` is the matching CLI flag and both values are recorded in the manifest. The two controls multiply, so opacity 0 hides the ridge regardless of the shading setting. Viewer settings persist both.
- **Stray dark smear under the mouth on cards that have a nose line (KK/KKS)**: the 0.6.1 shadow restore lifted every sub-cutout pixel of the nose texture onto the face skin. On cards whose nose texture also authors a nose line, the line's low-alpha fringes were baked into the face atlas near the mouth as a dark vertical smudge. The fringe restore is now limited to nostril-only nose textures - the cards that actually lose their authored shadow to the cutout. Verified on `KK_236802 (1).png`: smudge gone, authored nose line unchanged. Build and 19,046 unit checks pass.

## [0.6.3] - 2026-11-22

### Fixed

- **Nose-ridge slider: live, stays in the current view, and reads as opacity**: the slider now writes through its own value setter, so the ridge updates on each drag step without a re-bake (no loading bar needed). The change is applied to both the live scene and the baked "as exported" scene, so dragging it no longer kicks you out of MMD mode. The control is labelled **Nose ridge opacity**: `0` invisible, `1` as authored, `2` stronger. The spreading falloff of the ridge is preserved. Build and 19,046 unit checks pass.

## [0.6.2] - 2026-11-22

### Fixed

- **Nose-ridge slider now responds live while dragging**: it had been routed through the full skin-look re-apply, so every drag event re-ran the automatic body shading and AO bakes and the slider felt stuck. The drag now changes only the head mod's detail-map texture and re-uploads that single image; the exported copy is invalidated once on release. Build and 19,046 unit checks pass.

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
