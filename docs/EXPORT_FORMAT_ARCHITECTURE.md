# CardBridge — industry-standard export formats: architecture, evaluation and plan

Status: **investigation complete; implementation in milestones** (see §7).
Companion to `docs/OFFLINE_FORMAT_FEASIBILITY.md` (card/Unity/PMX feasibility) and
`docs/ARCHITECTURE_DECISIONS.md` (ADR-011/012/013: PMX document model).

Nothing in this document changes the PMX pipeline. PMX stays a first-class target.

---

## 1. Architecture investigation (existing state)

### 1.1 The canonical assembled character

`cardbridge::assemble::Scene` (`src/assemble/Scene.h:15-17, 419-453`) is the single
canonical representation. It is **plain value data**: no PMX, Unity, Vulkan or
game types, no smart pointers, no exporter state.

> "The assembled character, in Unity units, Y up, left-handed (the same handedness
> as PMX - no mirroring needed, only scale). Plain data: the exporter and the
> future engine importer both read this, nothing in it knows about PMX or Vulkan."

| Data | Type / location |
|---|---|
| Scene root | `Scene` — `name`, `bones`, `textures`, `materials`, `shaders`, `meshes`, `parts`, `expressions`, `chains`, `colliders`, `pose`, `issues`, bakes (`Scene.h:419-453`) |
| Bone | `SceneBone` — `name`, `parent`, `pos` (model space), `source`, `world` (rest world matrix) (`Scene.h:19-25`) |
| Vertex | `SceneVertex` — `pos`, `normal`, `uv`, `uv1..uv3`, `bone[4]`, `weight[4]`, per-bone rest frames `local[4]/nlocal[4]` (`Scene.h:27-38`) |
| Triangle | flat `uint32_t` triples in `SceneSubmesh::indices`; submesh carries `material` index (`Scene.h:203-206`) |
| Mesh | `SceneMesh` — `name`, `part`, `vertices`, `submeshes`, `shapes`, `reshapable` (`Scene.h:219-226`) |
| Texture | `SceneTexture` — decoded **RGBA8, top row first**, `width/height`, `srgb`, `clamp`, `pixelHash`, provenance (`Scene.h:40-54`) |
| Material | `SceneMaterial` — `texture`, `diffuse`, `alpha`, `translucent`, `doubleSided`, `renderQueue`, `shadow`, `specular`, `specularPower`, `outline*`, `props`, `texProps` (ST), `shaderIndex`, `toon` (32 rows), `bakedLit`, cutout flags (`Scene.h:107-150`) |
| Morph | `SceneBlendShape` — `name`, `rest` (weight already baked into vertices, 0..100), sparse `{vertex, Vec3 offset}` deltas at weight 100, plus per-bone frames (`Scene.h:208-217`); `SceneExpression` — named recipe of `{mesh, shape, weight}` targets + MMD panel (`Scene.h:299-307`) |
| Physics | `SceneChain` (DynamicBone-ish), `SceneCollider` (`Scene.h:309-347`) |
| Animation | **none**. No clips, keyframes, tracks or frame times anywhere in `Scene` or in `src/`. `shape::AnimKeys` and `assemble::DbCurve` are assembly-time inputs collapsed to a static rest pose; `ScenePoseEntry` is a static pose edit. |

**Ownership/lifetime:** all by value in `Scene`; meshes own vertices; parts, submeshes,
materials and textures are referenced by integer index. Textures are decoded CPU
bytes, not GPU resources. GPU skinning exists only in the viewer, on a separate
upload (`src/export/GpuView.h:26-44,103-113`) and is never part of the scene.
LOD/simplification works on deep copies (`src/mesh/Lod.cpp:113-123`), never the
caller's scene. Bakes (`RunCreateJobs`, `BakeShaderTextures`, skin shade/AO) mutate
the scene deliberately before export; both CLI and viewer already do this.

### 1.2 The current PMX serialization pipeline

Public entry point: `exporter::ExportScene(const Scene&, const Card&, const ExportOptions&)`
(`src/export/Exporter.h:99`, `src/export/Exporter.cpp:1363-1474`). `card` is used for
manifest provenance only. `ExportOptions` (`Exporter.h:28-76`) carries `scale=12.5`,
`faceMinusZ`, `mmdNames`, physics/jiggle, simplification/LOD, bake/upscale settings and
`log`/`progress`/`cancelled` callbacks.

Flow (CLI `cmdConvert`, `cli/main.cpp:810-1284`; viewer `StartExport`,
`viewer/Export.inc:34-175`):

```
card -> Resolver -> Assemble -> Scene (metres, Y-up, left-handed, +Z facing)
     -> reshape / pose / bakes / part visibility -> BakeShaderTextures
     -> ExportScene (const Scene&) -> [optional simplify copies + LODs]
     -> ExportSceneImpl -> pmx::Model -> pmx::WritePmx -> <name>.pmx
                                      + textures/*.png + <name>.cardbridge.json
```

`ExportSceneImpl` (`Exporter.cpp:768`) writes **all** bones, iterates parts/meshes/
submeshes in scene order (no vertex welding), writes one PMX material per submesh,
encodes only referenced textures (parallel PNG encoding + dedup by hash), adds
expressions and raw shapes (`PmxPhysics.cpp:717-757`), MMD morph names/aliases/effects,
IK, rigid bodies/joints and display frames, then `pmx::TurnAboutY(model)` and the
file. `pmx::WritePmx` validates structure before emitting bytes
(`src/pmx/PmxWriter.cpp:105-186`).

MMD-only work (toon ramps, sphere maps, MMD names/panels, display-frame 255 budget,
outfit/nude material morphs, IK, rigid bodies/joints, `-Z` facing, 12.5 units/metre)
is isolated in `Exporter.cpp`, `MmdNames.cpp`, `FaceMorphs.cpp`, `PmxIk.cpp`,
`PmxPhysics.cpp`. The shared, reusable parts are the material-semantics helpers in
`Scene.h:56-604`, `ValidateScene`, `ApplyPartStateForExport`, `BakeShaderTextures`,
`SimplifyScene`, `fileio::*` and `encodePng`-style PNG encoding.

**Reliability gap:** PMX writes directly with `std::ios::trunc` and retries sharing
violations (`src/core/FileIo.cpp:98-116`); there is no temp-file/rename step, so a
failure can leave a partial file. New exporters will use atomic temp+rename; PMX is
left untouched to avoid regressions.

### 1.3 Coordinate and unit conventions

* Source and canonical scene: **left-handed, Y-up, +Z facing, metres**
  (`Scene.h:15-17`, `Exporter.h:44`).
* PMX target: same handedness; scale `12.5` model units/metre; optional 180° Y turn
  to face `-Z` (`Exporter.cpp:1222`, `PmxWriter.cpp:83-103`). Geometry, bones, morph
  offsets, IK and physics are all turned together.
* UV: scene uses Unity v-up; texture bytes are top-row-first. Every writer flips v
  (`Exporter.cpp:881`, `PmxToScene.cpp:127-132`).
* Winding: no global flip in the scene. Per-material interior linings may be reversed
  for MMD (`Exporter.cpp:981-988`) — that verdict is MMD-specific.
* ADR-003: "Lit mode always runs the game's shader" — no vendor-neutral PBR in the
  source or preview; any PBR in an interchange format is a documented approximation.

### 1.4 Existing deps and tests

Native C++20, standalone static core `cardbridge_core` + CLI + Win32 GUI + tests
(`CMakeLists.txt`). Only nlohmann/json 3.11.3, pinned stb_image/stb_image_write and
(for the optional crosscheck) glm are fetched; `third_party/` contains bc7decomp only.
No glTF/OBJ/FBX/USD writer or library exists in the tree. Tests are hand-rolled
(`tests/cardbridge_tests.cpp`, `CHECK`/`test` at `:126-148`), with an independent PMX
reader (`tests/PmxReader.h`) for round-trips and an optional crosscheck tool.

---

## 2. Existing data availability assessment

| Requirement | Available in `Scene`? | Notes |
|---|---|---|
| Positions, normals | Yes | `SceneVertex::pos`, `normal` |
| Tangents | **No** | Derive from positions + normals + UV0 in the exporter |
| UVs | Yes, 4 sets | `uv`, `uv1`, `uv2`, `uv3` |
| Vertex colours | **No** | There is no colour attribute; do not fabricate one |
| Triangles, materials | Yes | `SceneSubmesh::{indices,material}` |
| Textures | Yes | Decoded RGBA8 + `srgb`/`clamp`; PNG encode via stb |
| PBR metallic/roughness | **No** | Only toon/specular proxies — document conversion |
| Normal maps | Partial | Only if a `texProps` entry names a bump/normal property |
| Alpha / cutout / double-sided | Yes | `alpha`, `translucent`, `renderQueue`, cutout flags, `props["_Cutoff"]` |
| Transforms | Yes | `SceneBone::world`, `parent`; node TRS/matrices derivable |
| Skinning | Yes | 4 influences + weights; bind frames in `local[]/nlocal[]`, rest worlds in `SceneBone::world` |
| Morph deltas | Yes | Sparse per-vertex positions at weight 100 + `rest` weight |
| Morph normals | **No** | Unity source has `dNormal` (`UnityScene.h:58`) but it is dropped during assembly; do not invent it |
| Morph names / defaults | Yes | `SceneBlendShape::name`, `SceneExpression::name`, `rest` |
| Skeletal animation | **NO** | No clip/keyframe data exists anywhere — export none, and say so |
| Physics | Partial | `SceneChain`/`SceneCollider`; not an interchange-format concept |

Conclusion: all M1/M2 geometry, hierarchy, skinning, morph and texture features are
available from one representation. Nothing needs a second Koikatsu parser. Animation
is genuinely absent and will not be fabricated.

---

## 3. Proposed exporter architecture

```
Koikatsu / KKS card
      |  (existing unchanged import pipeline)
      v
assemble::Scene  <-- canonical representation
      |
      +-- exporter::ExportScene(...)          PMX (unchanged, first-class)
      |
      +-- exporter::ExportModel(...)  --> format dispatch
             +-- gltf::WriteGlb / WriteGltf   (M1)
             +-- obj::WriteObj                (M2)
             +-- [fbx/usd future]             (M3/M4)
```

New shared surface:

* `src/export/ModelExport.h/.cpp` — `ExportFormat {Pmx, Glb, Gltf, Obj}`,
  `ModelExportOptions` (output dir/stem/overwrite, textures, armature, skinning,
  morphs, animation, separate objects, scale, format-specific toggles, progress log
  and cancellation), `ModelExportResult` (paths, counts, warnings) and
  `ExportModel(scene, card, options)`. PMX simply forwards to the existing
  `ExportScene` with its established defaults — no PMX logic is duplicated.
* `src/gltf/GltfWriter.h/.cpp` — builds the glTF document directly from the scene.
  Owns a single binary buffer, accessor/bufferView allocation, sparse morph
  accessors, node/skin construction, material conversion and PNG embedding.
* `src/obj/ObjWriter.h/.cpp` — streaming OBJ/MTL/texture writer from the same scene.
* `src/export/TexturePng.h` — shared `EncodePng(const SceneTexture&)` used by the new
  writers, so PNG encoding is not reimplemented per format. PMX keeps its own helper
  for now (zero regression risk; the duplication is recorded, not expanded).
* `src/export/AtomicWrite.h` — write to `.tmp`, then rename; no partially written
  output is ever treated as success. Shared by the new writers.

Design rules: one coordinate-conversion helper used by both new writers; one texture
dedup/encode path; one material-classification path; format-specific hacks stay in
their own writer. No new dependency for M1/M2.

### 3.1 Coordinate policy (explicit, applied to geometry + skeleton together)

glTF and OBJ are **right-handed, Y-up**. The scene is left-handed Y-up. The
conversion is a mirror of X, applied uniformly to:

| Item | Conversion |
|---|---|
| Position / normal / morph delta | `(x,y,z) -> (-x*s, y*s, z*s)` |
| Tangent | `(x,y,z,w) -> (-x, y, z, -w)` (handedness flips) |
| Bone world matrix | `M * W * M`, then uniform scale `s` (row-major math) |
| Bone local matrix | `inverse(parentWorld') * world'`, written as `node.matrix` (column-major JSON) |
| Inverse bind matrix | `inverse(world')`, column-major JSON |
| Triangle winding | reverse indices 1 and 2 (mirror flips orientation) |
| UV | `(u, v) -> (u, 1-v)` (glTF/OBJ top-left origin; same as PMX) |
| Forward | +Z in the interchange file; Blender's importer presents it facing -Y |

Scale `s` defaults to **1.0 (metres)** for glTF/OBJ — correct Blender size without a
user setting. PMX keeps 12.5. No independent half-turn to -Z for these formats: glTF
defines +Z as forward and Blender's standard importer handles the rest. Applying the
mirror to bones *and* geometry in one helper prevents the classic "mesh flipped but
rig not" bug.

### 3.2 Material conversion policy (documented, deterministic)

Koikatsu uses Illusion `Shader Forge/*` (KK) and `Koikano/*` (KKS) toon shaders;
they are not PBR. CardBridge's normal path bakes the game shader into the texture
(`BakeShaderTextures`), so the **texture already contains the toon lighting**.

| Scene data | glTF metallic-roughness | OBJ/MTL | Rule |
|---|---|---|---|
| main texture | `baseColorTexture` | `map_Kd` | Dedup by `pixelHash`, PNG bytes from RGBA8 |
| `diffuse` | `baseColorFactor` | `Kd` | Used verbatim; after bake it is near-white by design — do not tint twice |
| alpha flags, `_Cutoff`, cutout classification | `alphaMode` OPAQUE/MASK/BLEND (+ `alphaCutoff`) | `d`/`Tr` only (MTL has no alpha test) | Opaque-shaded + fully-opaque texture -> OPAQUE; shader clips -> MASK; `translucent` or queue>=3000 -> BLEND; MTL reports the limitation in a warning |
| `_ShadowColor`, `toon[]`, outlines, rim/hair gloss | **not representable** | **not representable** | Already baked into the texture; named warning in the manifest and result |
| `specularPower`, `specular` | `metallicFactor=0`, `roughnessFactor` from a fixed, documented curve | `Ns`/`Ks` | Deterministic: `roughness = clamp(1 - 0.01 * specularPower, 0.3, 1)`; not a claim of parity |
| `_EmissionColor` (if present) | `emissiveFactor` | `Ke` | Only when the property exists |
| tex binding `st` | `KHR_texture_transform` (declared in `extensionsUsed`) | unsupported | v offset adjusted for the flipped V |
| UV1-as-main (`mainUvFromUv1`) | `baseColorTexture.texCoord = 1` | unsupported (use UV0) | Second UV set is exported as TEXCOORD_1 |
| Mesh vertex colour | none | none | Not in the source; never fabricated |

The goal is a **visually faithful baked result**, not a claim that toon parameters
map to PBR. Limitations are recorded per export in `ModelExportResult::warnings` and
the manifest.

---

## 4. Library and dependency evaluation

No new runtime dependency is needed for M1/M2. This matters because CardBridge must
stay a standalone native Windows app with no Python/Blender/Unity/game runtime.

| Library | License / maintenance | Decision |
|---|---|---|
| nlohmann/json 3.11.3 (already vendored via FetchContent) | MIT, active | **Use** for glTF JSON; no new dependency |
| stb_image_write (already fetched) | public domain / MIT, pinned | **Use** for PNG bytes |
| cgltf | MIT, maintained, **read-only** | Not useful for writing; could be used by a future independent validator |
| tinygltf | MIT, maintained, C++ header-only, read+write | Viable but pulls in its own JSON and adds a large header; hand-written writer is ~500 lines and fully under our control. **Defer.** |
| fx-gltf | MIT, C++17 header-only, read+write | Viable alternative to tinygltf; same reasoning. **Defer.** |
| Khronos glTF-Validator | MIT (Node.js tool) | **Test-time only, optional** — never a runtime dep |
| tinyobjloader | MIT, read-only | Not useful for writing; OBJ/MTL writer is trivial |
| Autodesk FBX SDK | proprietary EULA, redistribution restrictions | **Reject** — conflicts with MIT distribution |
| OpenFBX / ufbx | permissive but **read-oriented**; no complete, maintained FBX writer with skinning | **Reject for now** — no reliable writer |
| Blender Python FBX exporter | GPL application, Python runtime | **Reject** — would violate "no Blender/Python at export" |
| Pixar USD (OpenUSD) | Apache-2.0, active, but very large C++/build footprint; USDZ adds packaging | **Defer** (M4); no implementation required |

FBX (M3) and USD (M4) are therefore **evaluated and deferred**, not half-implemented.
A future FBX implementation would need a licensed SDK or a maintained permissive
writer that supports skinning; neither exists in a form that fits this project today.

---

## 5. Feature compatibility matrix

| Feature | PMX | GLB / glTF (M1) | OBJ / MTL (M2) |
|---|---|---|---|
| Vertex positions / normals / UVs | Yes | Yes (UV0..3) | Yes (UV0) |
| Tangents | No | Derived from UVs | n/a |
| Vertex colours | No | No source data | No source data |
| Triangles / primitives | Yes | Yes (one primitive per submesh) | Yes |
| Material separation | Yes | Yes | Yes (`usemtl`) |
| Metallic-roughness PBR | No (toon) | Yes (approximation; documented) | n/a |
| Textures embedded | toon/sphere generated; external PNGs | GLB embedded; glTF external PNGs | external PNGs |
| Alpha modes | MMD flags | OPAQUE / MASK / BLEND | `d` only |
| Texture transform | n/a | KHR_texture_transform | No |
| Object / group separation | parts + materials | nodes per mesh | `o`/`g`/`usemtl` |
| Node hierarchy | bones only | mesh nodes + skeleton root + joint nodes | n/a |
| Skeleton + bone transforms | Yes (MMD names) | Yes (`node.matrix`) | No (static) |
| Skinning (4 influences) | BDEF2/4 | JOINTS_0/WEIGHTS_0 + IBM | No (static) |
| Morph targets | Yes (MMD panels, 255 listed) | Yes (sparse, named via `extras.targetNames`) | No |
| Morph default weights | Not stored | Mesh `weights` from `rest` | No |
| Animation | No | **No source data — never fabricated** | No |
| Physics chains / colliders | rigid bodies + joints | No | No |
| Toon ramp / outline / rim / hair gloss | Yes | Lost (baked into texture); warned | Lost; warned |
| Atomic output | No (legacy) | Yes (temp + rename) | Yes (temp + rename) |

---

## 6. Identified technical limitations

1. **No skeletal animation exists** in CardBridge. GLB will contain no `animations`
   array unless source data is added later. This is deliberate.
2. **No vertex colours and no morph normals** in the canonical scene; neither can be
   exported without inventing data.
3. **Toon shading is not PBR.** The bake makes the texture visually close, but the
   glTF material is a deterministic approximation; outline/rim/gloss/toon ramp are lost.
4. **4 influences** (glTF core JOINTS_0/WEIGHTS_0 limit matches the scene exactly).
5. **OBJ/MTL cannot express skinning, morphs, alpha test or hierarchy.** It is a static
   interchange file; no nonstandard extensions will be used.
6. **PMX writes non-atomically** today. New writers are atomic; PMX is unchanged in
   this milestone (recorded as a known debt).
7. **Bone `world` matrices can carry shear/negative scale**; using `node.matrix`
   instead of TRS avoids decomposition errors, at the cost of not being animator-
   friendly for manual editing (no animation is exported anyway).
8. **Large morph sets:** dense morph accessors would be wasteful, so sparse accessors
   are used; Blender supports them, but third-party tools with weaker sparse support
   may need a fallback (recorded and testable).
9. **Blender validation is a user/test step**, not a runtime dependency. Automated
   checks parse the generated files independently; visual/aesthetic confirmation in
   Blender remains a human check.

---

## 7. Implementation plan (milestones)

* **M0 — investigation (this document).** Done.
* **M1 — glTF 2.0 / GLB.** Shared `ModelExport` dispatch + `GltfWriter`; geometry,
  UVs, derived tangents, materials/textures (embedded GLB / external glTF), alpha
  modes, node hierarchy, skinning, sparse named morphs, no animation; atomic writes;
  CLI `--format glb|gltf`; independent parser tests. PMX unchanged.
* **M2 — OBJ / MTL.** `ObjWriter` on the same scene + shared conversion/texture helpers;
  object/group separation, MTL, external textures; CLI `--format obj`; parser tests.
* **M3 — viewer UI.** Format dropdown using the existing `DrawMenu`, format-specific
  options only; wire into the existing `StartExport()`; native-imported PMX documents
  stay PMX-only.
* **M4 — validation hardening.** Optional glTF-Validator test hook, documented Blender
  import checklist, real-card integration tests behind the existing env-gated
  integration suite, performance/memory notes.
* **M5/M6 — FBX and USD: evaluate only** (§4). No implementation unless a reliable,
  licence-compatible native writer becomes available.

Each milestone builds cleanly, passes tests, and leaves PMX byte-for-byte unchanged.

---

## 8. Deliverables recorded here

1. Architecture investigation — §1.
2. Data availability assessment — §2.
3. Proposed exporter architecture + coordinate policy — §3.
4. Library/dependency evaluation — §4.
5. Feature compatibility matrix — §5.
6. Technical limitations — §6.
7. Milestone plan — §7.

Post-implementation results are recorded in `CHANGELOG.md` (0.9.0/0.9.1) and below.

---

## 9. Implementation results (M1 + M2)

### 9.1 Files

**New**

| File | Purpose |
|---|---|
| `src/export/ModelExport.h/.cpp` | format enum/names, unified options and result, dispatch (PMX forwards to the untouched `ExportScene`) |
| `src/export/FormatCommon.h/.cpp` | shared coordinate transform, tangent derivation, alpha classification, texture dedup/PNG encode, atomic temp+rename writes, cancellation/progress helpers |
| `src/gltf/GltfWriter.h/.cpp` | GLB and glTF document construction, accessors (sparse morphs), nodes/skins/materials/images, GLB container and glTF sibling files |
| `src/obj/ObjWriter.h/.cpp` | streaming OBJ/MTL writer from the same scene |
| `tests/format_tests.cpp` | independent parser checks for GLB, glTF, OBJ/MTL, error handling, overwrite guard and cancellation |

**Modified:** `CMakeLists.txt` (four core sources + format-test target), `build.ps1`
(runs both test binaries), `cli/main.cpp` (`--format` and format-specific flags),
`viewer/{main.cpp,State.inc,Util.inc,Parts.inc,Panel.inc,Window.inc,Export.inc}`
(persisted format chip and routing), `README.md`, `CHANGELOG.md`, `VERSION`.

**Untouched:** `src/export/Exporter.cpp`, `src/pmx/*`, all PMX morph/physics/name
code. PMX output is byte-for-byte the same pipeline.

### 9.2 Implemented capabilities

* **GLB / glTF 2.0:** positions, normals, derived tangents, UV0–3, JOINTS_0/WEIGHTS_0
  (4 influences), triangle primitives per submesh, metallic-roughness materials,
  alpha OPAQUE/MASK/BLEND, `KHR_texture_transform`, embedded PNGs in GLB and external
  PNGs + `.bin` for glTF, node hierarchy with `node.matrix` bones, one skin with all
  joints and inverse bind matrices, expression + raw shape morphs as sparse named
  targets with zero default weights, manifest with warnings. No `animations` array.
* **OBJ / MTL:** shared-vertex geometry with mirrored coordinates and flipped V,
  one object/group per mesh, `usemtl` per submesh, MTL with Ka/Kd/Ks/Ns/d/illum and
  `map_Kd`/`map_Bump`, external textures, named warnings for static-format losses.
* **UI/CLI:** `--format pmx|glb|gltf|obj` with `--no-textures`, `--no-armature`,
  `--no-skinning`, `--no-morphs`, `--no-mtl`, `--combine-objects`; viewer Format chip
  cycles the four formats and persists the choice.

### 9.3 Known limitations

See §6. In particular: no animation, no vertex colours or morph normals, toon→PBR is
a documented approximation, OBJ/MTL cannot represent skinning/morphs/alpha test,
and PMX remains non-atomic (unchanged by this milestone).

### 9.4 Automated test results

* `cardbridge_tests.exe`: **19,493 checks, 0 failed** (2 integration suites skipped —
  they need `CARDBRIDGE_TEST_LIBRARY/_MODS/_CARD`; unchanged from before this work).
* `cardbridge_format_tests.exe`: **83 checks, 0 failed** — GLB container/JSON/accessor
  read-back including the X-mirror check, skin joint + IBM counts, embedded PNG,
  morph target names/weights, glTF external `.bin`/PNG round-trip with decoded image
  dimensions, OBJ/MTL index/material/texture validation, malformed-texture failure
  with no partial file, overwrite guard, and cancellation.
* Full Release build with w64devkit GCC 16.2 + Ninja: clean (no new warnings).

### 9.5 Blender import validation

Structural validation is automated above and does not depend on Blender. A visual
import check in Blender (geometry, UVs, materials, transparency, armature deformation
and facial morphs) is a human confirmation step — the exporter deliberately has no
Blender runtime dependency. The user reported the bone/armature result as OK.

### 9.6 Performance and memory

No large-card benchmark was run in this milestone. The writers avoid a second asset
pipeline: one pass over `Scene` per format, textures encoded once per unique pixel
hash, binary data appended straight into one buffer (no intermediate per-accessor
copies), and morph deltas stored as **sparse** accessors so a character with many
expressions does not allocate a full vertex-count array per shape. Atomic writes
hold the final file in memory before committing it; that is one extra buffer for the
model and is intentional (no partial output). PMX's existing upscale/bake paths are
unchanged and still run before either writer.

### 9.7 PMX confirmation

No PMX source was modified and the PMX path still runs first, with its full option
set and 12.5 units/metre defaults. The existing PMX round-trip and crosscheck tests
all pass. `--format` defaults to `pmx`.
