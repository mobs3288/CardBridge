# CardBridge — Performance Review

Scope: the character workflow — card load, asset resolution, assembly, clothing
switches, texture pipeline, baking/preview/export — and the tooling to measure it.
Every optimization keeps the exported output byte-for-byte identical to before
(PMX, textures, manifest), verified per stage; unit tests and the new regression
tests pass.

Machine used for numbers: Ryzen 5 3500X (6c/6t), 16 GB RAM, warm OS file cache,
the machine under active concurrent development load during the session — treat
small deltas as noise (+/- ~15 %), use the ratios as the story.

## How to measure

```powershell
# one full convert with a per-stage breakdown on stderr
$env:CARDBRIDGE_TIMING = 1
cardbridge convert card.png --library library_full --out out\probe --overwrite
# timing JSON (stages with ms, count)
$env:CARDBRIDGE_TIMING_JSON = "probe.json"
cardbridge convert card.png --library library_full --out out\probe --overwrite
# the representative workflows (cold / warm / outfit switches / no-bake / no-mod viewer reload)
powershell -ExecutionPolicy Bypass -File tools\bench.ps1 -Repeats 3
```

`tools\bench.ps1` reports median wall-clock; `out/bench.exe` is the in-process
harness (two back-to-back loads in one process, like the viewer's outfit switch).
`CARDBRIDGE_MEMPROBE=1` prints working set / private bytes per stage (unused in
the numbers below).

## What the pipeline spends its time on (before)

`image/demo_card1.png` → full PMX export, base-game items only (7/29 resolve),
measurements on this machine; before = after the tree was made to compile, with
the upstream/source-tree state at the start of this session.

| Stage | before | after | note |
|---|---|---|---|
| total wall clock | ~105–126 s | ~19–30 s | cold convert; lower bound measured on a quiet machine, upper under full developer load (your viewer/tools running) |
| shader bake (game shaders over UV charts) | 76.5 s | ~10.4 s | hot path was a `getenv` per sample |
| game create materials (mm_base) | 37.5 s | ~2.9 s | same root cause + per-pixel allocations |
| item assembly (parse + textures + mesh) | ~3–4 s | ~1.2–2 s | LZ4 parallel + decoded-texture cache |
| previews | 5.9 s | ~2.2–3.0 s | five pictures now render in parallel |
| export (textures+meshes) | ~1.0 s | ~1.0 s | PNG encode now parallel (included above) |

Measured end-to-end on this machine, quiet-ish: **~19–21 s** (repeat runs ~18.5–24 s);
under full concurrent editing load: **~26–30 s**. Per-stage timings above are the
reliable reference. Final `out\bench\results.json` from this session (all under full
concurrent load): cold 29.6 s, warm 28.2 s, outfit switches ~26.7 s, no-bake 9.0 s,
second card 8.8 s, viewer warm reload 2.18 s.

Wrap-up: the single biggest root cause was one line of code: `getenv` inside
`MatTextures::SampleImpl` was called **per sampled texel** (`CARDBRIDGE_CLAMP_ALL`),
which walks the environment block on Windows. Removing it (and per-sample linear
scans for the register→texture resolution) took the bake from 76.5 → 22.6 s on its
own; the rest followed from allocation-reuse and cache-work below.

## What changed

### Elimination of per-sample / per-texel waste

1. **`CARDBRIDGE_CLAMP_ALL` was read via `std::getenv` on every texture sample.**
   Now a function-local `static` read once. (Preview.cpp `MatTextures::SampleImpl`)
2. **Texture lookup per sample did linear scans** of the program's texture table,
   the material's texProps and the scene's global textures (string compares) per
   texture register. Now resolved once per `MatTextures` and cached (128 slots,
   plus a rare out-of-range fallback).
3. **`Sm4Io` (1 KB of zeroed arrays) was allocated/member-initialised per covered
   pixel**, plus a heap vector of constant-buffer pointers. Now one `Sm4Io` per
   worker is reset per pixel (`memset` of the two arrays), no allocations.
4. **`ComputeTangents` was recomputed per render call** (once per flat per
   light angle incl. the toon ramp's rows). Tangents depend only on the flat's
   positions/UVs — now computed once per material and reused everywhere.
5. **`temps.assign(...)` in the SM4 interpreter** is one `memset` on the
   thread-local buffer per invocation — kept, it's cheap; no change.

### Parallelism

6. **LZ4 block decompression** in `UnityBundle::Open` decompresses every block
   independently into its destination range — now spread over `hardware_concurrency`
   threads when the bundle is > 1 block and ≥ 8 MB uncompressed. Also the LZ4
   literal and match loops now use `memcpy` where provably non-overlapping
   (previously byte-by-byte everywhere).
7. **Export textures (PNG encode) run in parallel** — `stbi_write_png_to_func`
   is independent per texture; the textures are encoded in a thread pool and
   written in deterministic index order; output bytes unchanged.
8. **The five preview pictures** (front/side/back, head, 3/4 view, shaded,
   feet) render in parallel (one view state each). `CARDBRIDGE_SERIAL_PREVIEWS=1`
   falls back to the old sequential behaviour for A/B.
9. The toon-ramp/cover passes and create-job pixel rows were already
   parallelised upstream — unchanged in behaviour, kept as-is.

### Caching (correctness-preserving)

10. **Decoded-texture cache** (`Resolver::cachedDecodedTexture`): process-lifetime,
    byte-bounded (`CARDBRIDGE_TEXTURE_CACHE_MB`, default 384 MB, 0 disables),
    LRU-evicted, keyed by `source identity | size | last-write ticks | pathId`,
    where source identity is the absolute library path (base) or
    `modPath|rel` (mod bundles — never just the rel path, so two mods shipping the
    same bundle path can't alias). Re-reading a card (an outfit switch in the
    viewer) no longer re-decodes textures at all: measured warm assembly
    **~4.9 s → ~1.8 s**.
11. **Create-job result cache** (`RunCreateJobs`): the game's "CustomTextureCreate"
    pixel jobs are deterministic functions of shader blob hash + properties +
    input texture content-hashes + globals + overlay hash; cached per process
    (512 MB cap). A warm reload drops the create-job stage **~2.98 s → ~76 ms**.
12. Existing bounded LRU caches (bundles, in-assembly decodes) are retained
    unchanged.

### Incremental rebuilding (what is, and is not)

What exists today: in-process caches that make **repeated loads** of the same
character O(meshes + packing) instead of O(everything) — the decoded-texture and
create-job caches above are effective the moment a card is loaded twice in one
process (the viewer's outfit switch). `Reshape()` already re-poses shape edits
without reloading bundles; slider edits are the fast path they were.

What is deliberately **not** claimed: there is no partial "rebuild only the
changed garment" engine for clothing swaps. `Scene` is a flat pool (parts,
meshes, materials, textures all cross-indexed), so cutting one part out isn't
free: bone/index renumbering, under-piece masks, physics chains and material
indices all move. Doing it right means re-indexing the pool, which is an
architectural change with real regression risk for near-zero gain here (a full
assembly is now ~2 s warm) — deferred, and documented in "Remaining bottlenecks".

### GPU usage

None added, on purpose. The bake is the SM4 interpreter — the reference
implementation that defines every exported pixel. Running it on D3D11/WARP would
(a) change every exported texture (float rounding, fused multiply-add), failing
the byte-identical guarantee this review was asked to hold, and (b) add a hard
GPU requirement for the export path, which requirement 10 of the task forbids
without necessity. GPU **skinning** already exists and is unchanged (its own
algorithms already match the MMD path; `skin-bench` measures it).

## Correctness / compatibility verification

* Byte-identical output: PMX, all exported textures and the manifest hashed and
  compared against pre-change output, for the demo card (default outfit +
  `--outfit 2`) and for identical outputs in a fresh process multiple times.
* Unit tests: pass (9234 checks, 0 failed at the time of writing), integration
  suite remains conditional on `CARDBRIDGE_TEST_*` and untouched.
* New `cardbridge_perf_tests` binary (pure core, no game files): LZ4 round-trip +
  overlap-match encoding, the decoded-texture LRU (hit, capacity accounting,
  invalidation on source change), timing-scope no-op when disabled.
* CLI behaviour is untouched; all commands work as before; no output format,
  flag, exit code or env var was changed (new env vars only *add* knobs).

## Bugs found and fixed while profiling

1. **Crash / out-of-bounds read in the normal-map preview path**
   (`RenderView` / `RenderPreviewPng`): the material normal-map lookup matched
   `_NormalMask` (a mask, not a tangent-space normal map) as "normal", so a mip
   index could exceed the shorter mask chain, and `SampleBilinear` read past an
   empty chain — intermittent heap OOB, seen as an access violation in outfit-2
   runs. Fixed two ways: the normal-map sample now clamps the lod to the mask's
   chain length (the LOD was computed against the *main* texture, which may have
   more levels than the normal map), and `SampleBilinear` fails soft on empty or
   short chains instead of reading out of bounds.
2. **Non-deterministic `_view.png` (the eye/face region changed between runs of
   the same export)**: `RenderView`'s toon-ramp sample computed the row index as
   `(int)((1-nz)*0.5*kToonRows)` and cast straight to `size_t`. For nz > 1 (found
   on the face, nz ≈ 1.0683 on those texels) the result was negative, wrapped to
   SIZE_MAX, and read whatever bytes sat past the `mat.toon` vector — different
   per process, stable within a process. The exported PMX was always
   byte-identical; only the preview picture flapped. Fixed by clamping the row
   to `[0, kToonRows-1]`. Verified: `_view.png` now identical across runs.
3. The repository's own in-flight work did not compile as found (Preview.cpp
   forward declarations, `Paint.inc`'s include order, a header ODR duplicate);
   it was repaired in the smallest possible way to make the tree build at all.
   Nothing of yours was reverted or reordered.

## Remaining bottlenecks (after)

* **The SM4 interpreter** (~10 s of the ~20 s convert). It is the reference
  for "exactly what the game's shader does" and changing it is how output drifts.
  The remaining win is micro-optimisation (better instruction dispatch, SIMD-free
  register fetch) or a compliant JIT — both risky, both deferred.
* **Previews** (~2–3 s) of which the shaded picture is one-shader-row bound;
  already at the parallel ceiling of 6 cores.
* **Item parsing/serialization** (~1 s) — Array/descriptor reads for meshes and
  transforms; incremental, not structural.
* **No partial asset rebuild** (see "Incremental rebuilding" above).
* **The `_view.png` is now stable**, but the face shading in this environment
  renders the card's eye-mask-heavy look because the card's mod hair/iris items
  are not installed here; not a rendering bug — the data set isn't available.
