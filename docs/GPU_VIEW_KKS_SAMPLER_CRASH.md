# KKS `--gpu-view` access violation — root cause and fix (0.4.15)

## Reproduction

```
cardbridge.exe convert "C:\Users\pepsi\Downloads\Yx3EcDn2ddoGekvOHwvI1jyx.png" \
  --library "D:\opencode-kks-library-full" \
  --mods "E:\Game\[ScrewThisNoise] Koikatsu Sunshine EX BetterRepack R13\mods" \
  --out <out> --gpu-view --overwrite
```

Release and Debug both crash (`0xC0000005`) immediately after "gpu view: … (GPU, skin on CPU)" — i.e. in the first
`gpu.Render`, not during device creation or shader upload. The crash reproduces with the original colour code as
well as the corrected one, so it is independent of the colour pipeline.

A Debug build under gdb (with symbols) gave:

```
Thread 1 received signal SIGSEGV
0x...d3d11.dll
#0  d3d11.dll (invalid sampler slot indexing, mov 0x6c10(%r8,%rdi,1),%rax)
#1  cardbridge::exporter::GpuView::RenderImpl (...)  src/export/GpuView.cpp:1819
#2  cardbridge::exporter::GpuView::Render (...)       src/export/GpuView.cpp:1437
#3  cmdConvert (...)                                 cli/main.cpp:1084
#4  main (...)                                       cli/main.cpp:1259
```

## Root cause

`CARDBRIDGE_DEBUG_GPU=1` (added with this fix) showed the last operation before the fault:

```
[gpu] bind tex 'unity_SpecCube0' ... t=15 s=0 dim=8
[gpu] bind tex 'unity_SpecCube1' material='cf_m_body' shader='Koikano/main_skin' t=15 s=4294967295 dim=8
```

KKS (Koikano, Unity 2019) shader resource records encode "this texture has no sampler state of its own — reuse the
previously bound sampler" as the word `0xFFFFFFFF`. `ParseKksTail` stored that sentinel verbatim in
`TextureBinding::sampler`. `GpuView::RenderImpl` passed it straight to `PSSetSamplers`; the D3D11 driver uses the slot
number to index a fixed sampler array, so `0xFFFFFFFF` is an out-of-bounds read/write inside the driver — the access
violation. The same card with the KK library has ordinary sampler indices (0..15), which is why KK never crashed.

This is not a driver bug: the application supplied an invalid API argument. It is not a texture-upload, shader-compile
or resource-lifetime problem either; all of those had completed successfully before the first draw.

## Fix

1. `src/shader/UnityShader.h` — `kNoSampler` sentinel and `TextureBinding::hasSampler`; KK records keep the default
   `true`. `ValidTextureBinding` / `ValidTextureRegister` / `ValidSamplerRegister` are shared, header-only validators.
2. `src/shader/UnityShader.cpp` — `ParseKksTail` converts the `0xFFFFFFFF` sampler word into `hasSampler = false`
   instead of a register. The texture is still bound at its `t` register; the already-bound sampler (from the previous
   record, typically `unity_SpecCube0`) is intentionally left in place, which is what "no sampler" means.
3. `src/export/GpuView.cpp` — validates every shader-derived `t`/`s` register against the D3D11 slot limits before
   calling `PSSetShaderResources` / `PSSetSamplers`, skips invalid binds with a diagnostic, and emits a one-time
   load warning when a shader's binding table is malformed. Added `CARDBRIDGE_DEBUG_GPU=1` stage/resource logging:
   game-side resource counts, shader upload counts, texture upload counts, geometry counts, each draw's part/material/
   shader, and each texture binding's name/registers/dimension.
4. `cli/main.cpp` — logs the card family and scene counts before GPU init, and stages around init/load/render.

The fix is shared (KK goes through the same validation), never disables GPU preview or a material feature, and does
not touch the colour/grading code. `cardbridge_tests` now checks the sentinel parsing and both invalid-register cases.

## Verification

- Original reproduction with the fix: exit 0, `gpu_view.png` and `gpu_plain.png` written, full render passes
  (first render → second render → plain/MMD render → PMX export → manifest).
- KK library + `--gpu-view`: no crash. Two unrelated KKS cards + `--gpu-view`: no crash.
- Repeated rendering within one run (three renders) and repeated initialization across runs: no intermittent faults.
- The MMD (`gpu_plain.png`) image shows the expected light-blue shirt, navy vest, black skirt and natural skin —
  textures and colours are intact. The game-shader (`gpu_view.png`) image renders some KKS skin areas dark; that is
  pre-existing KKS shader-emulation behaviour and is unchanged with `CARDBRIDGE_NO_GRADE=1`, i.e. unrelated to this
  crash fix and to the colour-grading work.
