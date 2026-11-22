# Bra Artifact Investigation & Fixes — Onabuta Ikuko (ztokki Underwear Pasties)

Investigation card: `C:\Users\pepsi\Downloads\139306275_p3.png` (Koikatsu card, "Onabuta Ikuko", outfit 0),
library `library_full`, mods `E:\Game\[ScrewThisNoise] Koikatsu BetterRepack RX21\mods`.

## Status: the scene geometry is clean — there are no duplicated triangles at the breast covers

`tools/_cbbra_probe15.exe` runs an exhaustive pairwise near-duplicate analysis of every triangle of the
assembled bra mesh (o_bra_a): **exact dups: 0, near dups (<5e-4 distance): 0**. The 34 pastie triangles form two
diamonds (left, right). Sources:
- Unity mesh (probe 3): source mesh of the `ztoki_01b_bra_nippless` bundle: 1960 unique triangles, 0 dups.
- Scene (probe 4): o_bra_a 34/34, o_bra_a.under 1926/1926, cross-mesh 0.

So the "extra triangles" the user sees at the pastie tips are not duplicated geometry — I checked to be sure
(and per the constraints I will not delete geometry to mask symptoms).

## What the "repeated pattern at the tips" actually is

The pastie texture is a white diamond with a purple/grey interior and a ~65%-transparent diamond outline
(`card_tex_38` → `baked_cf_m_bra_npl_00`: white diamond, transparent corners). The bra's shader
(`Shader Forge/main_opaque`, DXBC at cb_shader_opaque.txt, the main pixel program 2, line 106) **discards
fragments where the main texture alpha < 0.5** (`discard r0.xxxx` from `sample … t1` → `min - 0.5 < 0`).
So at the diamond tips — where the diamond is thin and the texture alpha is mid/low — the DXBC clips the
fragment, and the leftover micro-geometry around the tips (the thin, clipped triangles) can read as a
"repeated pattern" of tiny triangles. That is a **drawing artifact of the game's own DXBC clip**, not a
converted-geometry bug.

Relatedly, the same DXBC writes `mov o0.---w imm0.(0,0,0,0)` (line 173) — the bra DXBC also outputs alpha 0.

## Root cause of the grey polygons (the separate symptom)

The grey polygons over the upper breasts are the **body's chest triangles (o_body_a / cf_m_body)** drawn flat
grey by the game's `main_skin` DXBC — present in both the CPU renderer and the GPU view (probe 11: all grey
chest pixels map to o_body_a). The skin texture is skin-coloured; the DXBC shades the chest flat-grey.

## Files and functions modified

| File | Function(s) | Change |
|---|---|---|
| `src/export/GpuView.cpp` | `MakeFallback` (HLSL), `RenderImpl` | `alphaGate` cb field; fallback `ps()`: `if (alphaGate.x > 0.5) clip(c.a - 0.5)` on the opaque path. |
| `src/unity/UnityScene.h` | `MeshData` | New `subMeshMaterials` field; new free function `SubmeshMaterialSlot`. |
| `src/unity/UnityScene.cpp` | `SubmeshMaterialSlot`, `readMesh` | Capture `m_SubMeshes[].m_Material`; implement the slot helper. |
| `src/assemble/Assembler.cpp` | `addRenderer` | Use `SubmeshMaterialSlot` + warn on ordinal/asset disagreement. |
| `tests/cardbridge_tests.cpp` | `unitTests()` | New `SubmeshMaterialSlot` unit test (+10); `fixturePath()` helper + fixed HAC fixture test (18,644 → 18,654, script-CWD-independent). |

## Regression test results

- `cardbridge_tests.exe`: **18,654 checks, 0 failed** (from the cardbridge repo root AND from `D:\Project K` via
  `scripts\cardbridge.ps1 -Test`).
- `cardbridge convert` of the card: exit 0; no new issues.
- Scene dump diff (pre/post fixes): 0 lines.

## Honest conclusion (for the user)

1. **Duplicates**: none — the scene is clean; the "repeated pattern at the tips" is the DXBC's alpha-clip at
   the diamond tips (`cb_shader_opaque.txt:106`). If they can capture the problem image again and point at the
   triangles, I can re-verify.
2. **Grey polygons**: the body draws flat grey by the DXBC — possibly "correct" game behaviour (the DXBC is
   the game's). The open question (§2 in the report) is whether the game renders the chest grey too; if it
   does, there's nothing to fix on the CardBridge side.
