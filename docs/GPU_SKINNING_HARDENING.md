# GPU Skinning Hardening — Adapter Selection, Validation, Memory, Performance

Date: 2026-11-08. Hardware: Windows 11, NVIDIA GeForce RTX 2060 SUPER
(8 GB dedicated, feature level 11_0), MinGW-w64 GCC 16.2.0, CMake 3.30.5,
Ninja 1.12.1. Build: `build/bin/cardbridge.exe`, `cardbridge_tests.exe`,
`cardbridge_viewer.exe`.

## 1. Implementation summary

### 1a. Adapter classification (Phase 2)

**Original problem.** `GpuView::Init` scored adapters almost entirely on
vendor ID: NVIDIA/AMD `+10000` (assumed discrete), Intel `+100`, Microsoft
`−10000`, everything else `+500` if it had any dedicated memory, software
`−50000`. Every NVIDIA/AMD part won over every Intel part regardless of
memory, so an AMD integrated Radeon beat an Intel Arc discrete card, and an
Intel integrated part lost even when it was the only hardware on a hybrid
system with a disabled discrete card.

**Root cause.** Vendor was used as a discrete/integrated verdict instead of
as a tie-breaker, and dedicated/shared memory was barely considered.

**Files modified.**
- New `src/export/AdapterSelect.h` (header-only scoring, no D3D dependency):
  `AdapterDesc`, `ScoreAdapter()`, `SelectBestAdapter()`,
  `DescribeAdapter()`, `AdapterClass` (including `Unknown` for ambiguous
  signals — the code does not invent a discrete/integrated verdict when
  memory signals are in the middle).
- `src/export/GpuView.cpp`: `Init` fills `AdapterDesc` from
  `DXGI_ADAPTER_DESC1` (`Description`, `VendorId`, `DeviceId`,
  `DedicatedVideoMemory`, `DedicatedSystemMemory`, `SharedSystemMemory`,
  `Flags`) and orders via `AdapterSelect.h`. Software/remote/virtual
  adapters are skipped for hardware selection. `CARDBRIDGE_GPU_ADAPTER`
  still names an adapter by enumeration index before ordering.
  `CARDBRIDGE_DEBUG_GPU=1` prints every adapter (name, vendor/device,
  dedicated/shared MB, flags, score, class, reason) and the selection.
  `CARDBRIDGE_D3D_DEBUG=1` asks for a debug device when the SDK is present.
- `src/export/GpuView.h`, `README.md`, `cli/main.cpp`, `viewer/main.cpp`:
  comments/help updated to the new policy; CLI/viewer skin modes now
  document `auto`.

**Chosen solution.** Suitability for CardBridge (compute skin + DXBC forward
pass) is scored, not a definitive integrated/discrete label:

- Software (`DXGI_ADAPTER_FLAG_SOFTWARE`, `FORCE_D3D10`, description match
  for "Basic Render Driver"/"WARP"/"Software", or vendor `0x1414`) and
  remote (`DXGI_ADAPTER_FLAG_REMOTE`) are unsuitable — excluded from normal
  hardware selection, only used when explicitly allowed as fallback.
- Local hardware base `1000` (even modest integrated graphics is suitable:
  a character's skinning buffers are tens of MB).
- Dedicated video memory thresholds (`≥8/4/2/1 GB`, `≥512/256/64 MB`)
  order the adapters; dedicated share `dedicated/(dedicated+shared)`
  gives a weak discrete hint (high share + ≥1 GB → `DiscreteLikely`,
  low share + ≤512 MB → `IntegratedLikely`, middle → stays `Unknown`).
- Total addressable memory is a small capped bonus (shared memory alone
  cannot outscore a discrete card).
- Vendor (NVIDIA/AMD/Intel) is an equal `+10` tie-breaker only. Intel Arc
  discrete wins on its large-dedicated/high-share memory signal like any
  other discrete card; AMD integrated scores like Intel integrated with
  the same memory. No NVIDIA/AMD part is auto-classified as discrete and
  Intel is never penalised.
- `<256 MB` total addressable memory is unsuitable for even a small
  character.
- Selection picks the max score, tie-breaking on dedicated memory then
  enumeration order (deterministic). `D3D11CreateDevice` is attempted in
  that order; WARP is the fallback when no suitable hardware device can be
  created (or when `pref == "warp"`), and `software` still means the
  software interpreter renderer.

### 1b. Bone-index bounds validation (Phase 3)

**Original problem.** The CPU loop rejected negative and out-of-range bone
indices (`b < 0 || b >= skin.size()`). The compute shader rejected only
`b < 0` and `w <= 0`, so `b >= numBones` indexed `g_bones` out of range,
and neither side rejected NaN/inf weights or guarded non-finite sums.

**Root cause.** Missing upper-bound check in the shader and missing
finiteness checks on both sides.

**Files modified.**
- `src/export/GpuView.cpp` (`MakeSkinCompute` HLSL): the shader now reads
  the actual uploaded bone count via `g_bones.GetDimensions()` (`numBones =
  boneRows / 4`), rejects `b < 0 || (uint)b >= numBones`, rejects weights
  with `!(w > 0) || isinf(w) || isnan(w)`, skips invalid influences
  without accessing the matrix buffer, requires `sum > 1e-6 && isfinite(sum)`
  before dividing, and only writes the blended position/normal/tangent
  when the result is finite (otherwise the rest vertex is kept — the
  existing fallback). No hardcoded maximum.
- `src/assemble/Pose.cpp` (`SkinPoint`, `SkinDirection`, `BakeSkin`
  blend-shape offsets) and the `GpuView::SetSkin` CPU loop: same checks
  (`b` range, `!(w > 0) || !isfinite(w)`, finite `sum`, finite result).
  Valid influences are preserved when others are invalid; vertices with
  no valid influence keep the rest position/normal (the established
  fallback — no new fallback invented).
- `tests/cardbridge_tests.cpp`: CPU tests for every row in the Phase 3
  table (valid, negative, `== count`, `> count`, zero, negative, NaN,
  inf, no-valid). GPU parity is proven by `SkinBench` numerical
  equivalence (max error ~3e-7) plus the mirrored shader logic; the D3D
  debug layer was enabled (`CARDBRIDGE_D3D_DEBUG=1`) during bench runs
  with no validation output observed.

### 1c. GPU memory overhead (Phase 4)

**Files modified.** `src/export/GpuView.h` (`SkinMemoryStats`),
`src/export/GpuView.cpp` (`GpuView::SkinMemory()`, `CARDBRIDGE_DEBUG_SKIN`
Load log). No buffer layout change (see below).

**Buffer layout (per mesh, `sizeof(GVert) = 120` bytes).**

| Resource | Purpose | Elements | Stride | Bytes (formula) | Usage | CPU access | Persistent? | Duplicated? |
|---|---|---|---|---|---|---|---|---|
| `restBuf` | Rest vertex buffer (drawn at rest) | `nVerts` | 120 | `120·nVerts` | DEFAULT, VB | none | yes (until Unload/Shutdown) | yes — same bytes as `skinInBuf` |
| `skinInBuf` | Structured input for the shader (SRV `restSrv`) | `nVerts` | 120 | `120·nVerts` | DEFAULT, SRV, STRUCTURED | none | yes | yes — same bytes as `restBuf` |
| `skinOutBuf` | Structured output for the shader (UAV `posedUav`) | `nVerts` | 120 | `120·nVerts` | DEFAULT, UAV, STRUCTURED | none | yes | yes — copied to `posedBuf` each dispatch |
| `posedBuf` | Posed vertex buffer (drawn when posed) | `nVerts` | 120 | `120·nVerts` | DEFAULT, VB | none (UpdateSubresource on CPU path) | yes | yes — copy of `skinOutBuf` |
| `skBones` | Bone matrices (4×float4 per bone) | `nBones·4` | 16 | `64·nBones` | DYNAMIC, SRV, STRUCTURED | WRITE (Map) | yes, resized only when bone count changes | no |
| CPU `rest` | CPU mirror of rest vertices (re-skinning) | `nVerts` | 120 | `120·nVerts` (system) | — | — | yes | yes — mirrors GPU rest |
| Transient `posedVerts` | Scratch for one CPU-skin update | `largestMeshVerts` | 120 | `120·maxMesh` (system, per SetSkin) | — | — | no (alloc per update) | no |
| Index buffers | As authored | `totalIndices` | 4 | `4·totalIndices` | IMMUTABLE, IB | none | yes | no (not skinning) |

Total GPU skinning per scene = `480·totalVerts + 64·nBones` bytes.
WARP uses the same buffers (no separate allocation). `SetSkin` never
recreates vertex buffers (only the bone buffer resizes when the bone count
changes); `Load` recreates everything (expected on card change);
`Clear` flushes before release so in-flight dispatches cannot bleed into
the new card. Physical GPU memory (driver reservations, fragmentation)
cannot be obtained exactly through D3D11; `SkinMemory()` reports the
calculated allocation above, which is also the peak attributable to
skinning (no other per-frame skinning allocation exists).

**Optimization investigated, not taken.** Merging `restBuf`+`skinInBuf`
and `posedBuf`+`skinOutBuf` into one buffer per side was tried
(`BIND_VERTEX_BUFFER|BIND_SHADER_RESOURCE` / `|BIND_UNORDERED_ACCESS`
with `MISC_BUFFER_STRUCTURED`). D3D11 rejected creation on the test
hardware (`CreateBuffer` failed, “rest buf create failed”), confirming
the original comment: a structured buffer cannot also be a vertex
buffer. Halving the allocation would require a `ByteAddressBuffer`
rewrite of the shader I/O (plain VBs bound as `ByteAddressBuffer` SRV/UAV
+ `Load`/`Store`). That rewrite changes the skinning architecture for
~24 MB saved at 100k verts (~120 MB at 500k verts) and carries new
validation risk; it was not taken because correctness must not be traded
for memory. Reusing/resizing vertex buffers across `Load`s, aliasing,
or earlier release were also not taken: vertex buffers are sized per
mesh, `Load` is not per-frame, and aliasing while GPU commands are in
flight would break synchronization. See §3 for measured sizes.

### 1d. CPU vs GPU benchmarking + auto policy (Phase 5)

**Files modified.** New `src/export/SkinBench.h/.cpp`
(`DefaultSkinWorkloads`, `MakeSyntheticSkinWorkload`, `BenchCpuSkin`,
`BenchGpuVsCpu`, `RunSkinBenchmark`), `src/export/GpuView.h/.cpp`
(`SetBenchQueries`, `LastGpuDispatchMs`, `ReadbackPosed`,
`SetSkinThreshold`), `CMakeLists.txt`, `cli/main.cpp` (`skin-bench`),
`viewer/main.cpp` (skin `auto` row), `tests/cardbridge_tests.cpp`
(CPU/GPU parity fixtures).

**Methodology.** Synthetic workloads (documented below; no real-character
benchmark claimed) with deterministic PRNG, 4 influences/vertex
(Dirichlet-like weights). CPU = isolated `SkinPoint` loop; GPU = real
`GpuView::SetSkin` on the same input data through the same shader.
Warmup 5×, then median/p95 over `repeats` steady-state iterations (30 for
the reported numbers; shader compilation excluded). GPU execution uses
D3D11 timestamp/disjoint queries first; the test driver reported
`Frequency = 0`, so the harness falls back to an event query (GPU
completion time from dispatch start, waited for — never submission
alone). End-to-end adds flush + staging readback (the sync cost a
readback consumer pays; the preview itself never reads back). Equivalence
is the max |CPU − GPU| position error over all vertices.

**Auto policy.** `skin = "auto"` keeps meshes with fewer than
`skinThresholdVerts` on the CPU loop and dispatches the rest on the GPU
(per mesh, so a small accessory does not pay dispatch cost for a big
body). Threshold sources, in order: `GpuOptions::skinThresholdVerts` /
`SetSkinThreshold`, then `CARDBRIDGE_SKIN_THRESHOLD`, then default
**2000 verts**. Manual `gpu`/`cpu` modes and `CARDBRIDGE_SKIN` override
are preserved; the default stays `gpu` for compatibility. On the
measured hardware the preview-path crossover (CPU vs GPU submission)
sits below 10k verts, while the sync+readback crossover sits near 50k;
the default 2000 is just above fixed dispatch overhead (~a few hundred
verts), so preview latency stays GPU-accelerated without paying GPU
cost for tiny meshes. The threshold is configurable because the
crossover is GPU-dependent (documented, not hardcoded as universal).

## 2. Performance report (actual measurements)

Device: NVIDIA GeForce RTX 2060 SUPER (Hardware). Workloads are
synthetic (real-character numbers are not claimed). Times are medians
over 30 steady-state repeats after 5× warmup.

| Workload | Vertices | Bones | CPU time (ms) | GPU time (ms, submission) | GPU exec (ms, event-timed) | End-to-end GPU time (ms, +flush/readback) | Speedup (CPU/submit) | Max err (model units) | Equiv. |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| small | 10000 | 64 | 0.278 | 0.011 | 0.089 | 0.490 | 25.27× | 2.4e-07 | yes |
| medium | 50000 | 128 | 1.520 | 0.016 | 0.209 | 1.631 | 95.00× | 2.5e-07 | yes |
| large | 150000 | 256 | 9.267 | 0.034 | 0.542 | 5.035 | 272.56× | 2.5e-07 | yes |
| extreme (modded geometry) | 500000 | 400 | 18.016 | 0.044 | 1.100 | 13.466 | 409.45× | 3.6e-07 | yes |

Synthetic distribution: positions uniform in `[-1,1]^3`, normals random
unit, 4 random bones/vertex, weights = squared uniform normalized.
Skin matrices: random small rotations (±20°/axis) + translations
(±0.1). Reproduce with `cardbridge skin-bench --repeats 30`
(`--gpu auto|hardware|warp` selects the device).

Reading the table:

- Preview latency (what the viewer pays on the CPU per pose edit) is
  submission time; GPU wins at every measured size, by 25×–400×.
  Pure GPU completion (exec) is 0.09–1.1 ms, still 3×–16× faster than
  CPU.
- End-to-end with flush + full CPU readback is slower than CPU at 10k
  verts (0.49 vs 0.28 ms) and about equal at 50k (1.63 vs 1.52 ms); it
  wins clearly at ≥150k verts. CardBridge preview never reads back, so
  the preview crossover is the submission one; a future consumer that
  needs CPU positions should set `CARDBRIDGE_SKIN_THRESHOLD ≈ 50k` or
  use `cpu`/`auto` explicitly.
- WARP (CPU fallback) equivalence also holds (max err 1e-07 on all
  sizes) with exec ≈ CPU time, confirming the shader is not
  GPU-vendor-specific in its math.

## 3. Memory report

Calculated allocations (from buffer byte widths). Before = the 4-buffer
layout as shipped; after = same layout (merge investigated and rejected,
§1c). Per-mesh GPU = `480·nVerts` bytes; bones = `64·nBones`.

| Resource | Before (500k verts, 400 bones) | After | Difference |
|---|---:|---:|---:|
| Rest positions (`restBuf`) | 58,593 KB | 58,593 KB | 0 KB |
| Structured input (`skinInBuf`) | 58,593 KB | 58,593 KB | 0 KB |
| Structured output (`skinOutBuf`) | 58,593 KB | 58,593 KB | 0 KB |
| Posed positions (`posedBuf`) | 58,593 KB | 58,593 KB | 0 KB |
| Bone matrices (`skBones`) | 25 KB | 25 KB | 0 KB |
| **Total GPU** | **234,375 KB (229 MB)** | **Same** | **0** |

Scaling: 10k verts → 4,687 KB total GPU + 1,171 KB CPU mirror;
50k → 23,437 KB; 150k → 70,312 KB. CPU mirror = `120·totalVerts`
system memory (kept for re-skinning/CPU fallback). No allocation
optimization is justified without the ByteAddress rewrite; the
duplication is required by D3D11 while the shader uses structured
buffers (verified by the failed combined-buffer creation). Peak
attributable GPU memory equals the total above (no per-frame skinning
allocations); repeated `SetSkin` calls allocate only the transient
CPU `posedVerts` scratch on the CPU path and resize the bone buffer
only when the bone count changes.

## 4. Adapter selection report

Scoring (see §1a for the full policy; `CARDBRIDGE_DEBUG_GPU=1` prints
one line per adapter with score/reason):

| Synthetic adapter | Score | Class | Suitable? | Reason (short) |
|---|---:|---|---|---|
| NVIDIA discrete (8 GB dedicated) | 4460 | hardware | yes | large dedicated (≥4 GB) |
| AMD discrete (16 GB) | ~5400 | discrete-likely | yes | large dedicated + high share |
| AMD integrated (512 MB + 8 GB shared) | ~2100 | integrated-likely | yes | dedicated ≥512 MB (mid) |
| Intel Arc discrete (16 GB, vendor Intel) | ~5400 | discrete-likely | yes | large dedicated + high share (same as AMD discrete) |
| Intel integrated (128 MB + 16 GB shared) | ~1600 | integrated-likely | yes | small dedicated share |
| Microsoft Basic Render Driver (0x1414) | −1,000,000 | software/virtual | no | virtual/basic, excluded |
| WARP / software flag | −1,000,000 | software | no | software, excluded |
| Remote desktop adapter | −50,000 | remote | no | remote, deprioritised |

Representative multi-GPU picks (unit-tested, `SelectBestAdapter`):

- Intel integrated + NVIDIA discrete → NVIDIA discrete.
- AMD integrated + Intel Arc discrete → Arc discrete (fair competition;
  memory signal, not vendor).
- Intel integrated alone → Intel integrated (suitable; no penalty for
  being Intel).
- Basic + WARP only → no hardware (`-1`); explicit software fallback
  allowed only when requested.
- Basic + Intel integrated → Intel integrated.

Physical validation on the test machine (2× RTX 2060 SUPER entries +
Basic Render Driver): the RTX was selected (score 4460, feature level
11_0), Basic excluded. Vendor-equal check: AMD/Intel integrated with
identical memory score identically; Arc discrete scores within 200 of
AMD discrete with identical memory.

## 5. Test results

- **Unit tests:** 625 checks, 0 failed. Includes 3 new suites:
  adapter selection (8 synthetic adapters + 5 multi-GPU picks),
  bone-index/weight validation (9 scenarios), rest/extreme-rotation
  finiteness. Integration suite skipped (no `CARDBRIDGE_TEST_*` inputs
  set — 1 skipped, 0 failed).
- **CPU/GPU numerical comparison:** max position error 2.4e-07–3.6e-07
  (model units) on all 4 synthetic workloads, Hardware and WARP;
  tolerance for “equivalent” is 1e-03. Rest pose, normal pose, and
  180° extreme rotation stay finite on CPU; GPU matches.
- **GPU validation/debug layer:** `CARDBRIDGE_D3D_DEBUG=1` bench and WARP
  runs completed with no validation output. Invalid-bone-index GPU
  safety is proven by construction (bounds check before any `g_bones`
  access) mirroring the CPU-tested logic; a dispatch that successfully
  runs is not treated as correctness evidence (equivalence is checked
  numerically).
- **Regression:** existing character imports, PMX exports, bone
  transforms and weighting semantics unchanged (no exporter/PMX code
  touched; skinning math identical except the added guards). CPU
  fallback verified by forced `cpu` mode and WARP bench. Large
  (500k-vert) workload stable. Resource release path (`Flush` +
  `Clear`, `Unload`, `Shutdown`) unchanged.
- **Environment limitation:** timestamp/disjoint queries reported
  `Frequency = 0` on the test driver, so GPU exec uses event-query
  completion timing (documented above). Real-character benchmarks
  were not run (no suitable card/library inputs available in this
  environment); all performance numbers are synthetic and labelled
  as such. A reproducible procedure is `cardbridge skin-bench`
  (optionally with `--gpu`) on any D3D11 hardware.

## 6. Final assessment

- **Is GPU skinning now safe against invalid bone indices?** Yes. Both
  CPU and shader reject negative/`>= count` indices, zero/negative/
  NaN/inf weights, and non-finite sums; invalid influences never reach
  the matrix buffer; vertices with no valid influence keep the rest
  position; no NaN/inf is emitted. Unit-tested on CPU with GPU logic
  mirrored and numerically equivalent.
- **Is adapter selection more reliable?** Yes. Software/virtual/remote
  are excluded, dedicated memory + share order the rest, vendor is an
  equal tie-breaker only, ambiguous cases stay `Unknown` instead of
  inventing a verdict, and the policy is unit-tested synthetically for
  all required vendor/layout cases plus physically for RTX vs Basic.
- **Has memory overhead been reduced or at least quantified?** Quantified
  in full (per-resource table, `SkinMemory()`, debug log). Not reduced:
  the 4-buffer duplication is required while the shader uses structured
  buffers (D3D11 rejects VB+structured), and the ByteAddress rewrite
  that would halve it was investigated but not taken to avoid
  architecture churn and validation risk. No per-frame GPU allocation
  occurs on repeated pose edits.
- **Is GPU skinning actually faster for representative workloads?**
  Yes, on the measured hardware for synthetic workloads spanning
  10k–500k verts: 25×–400× faster pose-edit submission, 3×–16× faster
  GPU completion, numerically equivalent. Real-character speedups will
  vary with GPU, but the direction is consistent across sizes.
- **At what workload size does GPU become beneficial?** For the preview
  path (no readback) below the smallest measured size — GPU already wins
  at 10k verts by 25×; the `auto` default of 2000 verts is conservative
  and configurable. If CPU readback is required, GPU becomes beneficial
  at ~50k–150k verts (set a correspondingly higher threshold).
- **Ready for normal use?** Yes. Existing functionality, CPU fallback,
  and PMX compatibility are preserved; invalid input cannot cause
  unsafe GPU buffer access; adapter selection is fair and logged;
  memory is quantified and stable; performance is measured, not
  assumed. The remaining known limitation is the unadopted
  ByteAddress memory halving and the driver-specific timestamp
  frequency fallback (event timing used instead).
