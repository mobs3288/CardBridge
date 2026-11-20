# HairAccessoryCustomizer (HAC) support

Status: **implemented** (2026-11-20). Version 0.4.8.

## Verified data mapping

Decompiled `KKS_HairAccessoryCustomizer.dll` and decoded real card payloads:

1. KKEx key: `com.deathweasel.bepinex.hairaccessorycustomizer`.
2. Entry shape: `[version, { "HairAccessories": <MessagePack blob> }]`. Some cards use `CoordinateHairAccessories`; both keys are accepted.
3. Blob shape (cards): `coordinate index -> accessory slot -> settings`.
   - Coordinate index is the same index as `card.outfits[coordinate]`.
   - Slot index is the same zero-based index as `card.outfits[coordinate].accessory.parts[slot]` and `resolve::ItemRef::accIndex`.
   - `moreAccessories` slots are appended to that same array at indices 20+, so their HAC keys are also 20+ (verified on slot 20 of the regression card).
   - Outfit files can contain a flat `slot -> settings` map; those entries are stored under wildcard coordinate `-1` and only used when no exact coordinate entry exists.
4. Settings fields: `HairGloss` (bool), `ColorMatch` (bool), `OutlineColor`, `AccessoryColor`, `GlossColor` (RGBA float arrays), `HairLength` (float). A record is only used when **all** fields are present and well-formed; partial/malformed records are ignored rather than silently defaulted.

## Colour resolution

When `ColorMatch = true`, the game overrides the hair accessory's `rendHair` materials with the character's hair configuration:

| Shader property | Source |
| --- | --- |
| `_Color` | `card.hair.parts[0].baseColor` |
| `_Color2` | `card.hair.parts[0].startColor` |
| `_Color3` | `card.hair.parts[0].endColor` |
| `_LineColor` | `card.hair.parts[0].outlineColor` |
| `_GlossColor` | `card.hair.parts[0].glossColor` |

When `ColorMatch = false`, the stored `OutlineColor` / `GlossColor` are used for hair renderers and `AccessoryColor` for `rendAccessory`, matching the plugin's run-time order. Ordinary accessory colors are unchanged. HAC data is decoded once per assembly from the current KKEx map, so an in-memory coordinate import is reflected immediately.

**Override order (fixed in 0.4.4):** in the game HAC applies its colours at run time, *after* MaterialEditor's stored material edits. CardBridge previously applied MaterialEditor edits after the HAC colours, so a card with a MaterialEditor `Color2` override on a back-hair material looked washed out. HAC colours now win for `ColorMatch=true` hair accessory renderers; other materials keep MaterialEditor's edits.

## Gloss and hair length

- `HairGloss` keeps/removes the `_HairGloss` texture via the existing `glossForThisRenderer` / `stripHairGloss` path.
- `_GlossColor` is now written into the material property list before the shader texture bake; without it the hair bake used the shader default (black), which made ColorMatch hair look washed out.
- `HairLength` is shader-only in the game. PMX has no equivalent and no geometry is ever scaled from it. The value is preserved as `_HairLength` in the intermediate scene material property table (and therefore in diagnostics) but has no visible PMX effect.

## Files

- `src/card/HairAccessoryCustomizer.{h,cpp}` — decoder, typed model, colour helpers.
- `src/assemble/Assembler.cpp` — consumes decoded settings instead of parsing MessagePack inline; writes `_GlossColor` / `_HairLength`.
- `CMakeLists.txt` — new source.
- `tests/golden/hac_reference.bin` — HAC payload extracted from the regression card (plugin data only, no card image).
- `tests/cardbridge_tests.cpp` — decoder, colour, and real-fixture tests.

## Limitations

- Unity's root-to-tip hair gradient and animated gloss cannot be reproduced exactly in PMX; the bake approximates them with the three card colors.
- `HairLength` cannot be represented visually.
- A record missing any HAC field is ignored so a malformed payload cannot silently repaint an accessory.
