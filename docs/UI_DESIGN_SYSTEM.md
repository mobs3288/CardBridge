# CardBridge UI Design System

Introduced with the UI/UX redesign (branch `ui-redesign`). One accent, quiet neutrals, three sizes of corner
radius, eight-pixel spacing. The theme lives in `Ui.h`; everything else builds on it.

## Colour tokens (`ui::`, `Ui.h`)

| Token | Value | Use |
|---|---|---|
| `kBg` | `RGB(17,18,22)` | app background behind the panel |
| `kCard` | `RGB(24,25,31)` | raised cards (list, log, banners) |
| `kCardHi` | `RGB(31,32,39)` | hover fill on raised cards |
| `kSelect` | `RGB(37,45,56)` | selected rows / "on" fills |
| `kLine` | `RGB(42,43,51)` | 1 px borders (low contrast) |
| `kLineHi` | `RGB(58,59,70)` | hover borders, scrollbar thumb |
| `kText` | `RGB(233,233,236)` | primary text (soft white) |
| `kSub` | `RGB(160,161,170)` | secondary text |
| `kDim` | `RGB(126,127,138)` | tertiary / captions |
| `kFaint` | `RGB(88,89,100)` | disabled |
| **`kAccent`** | `RGB(96,156,214)` | **the only accent** — primary actions, selection, focus |
| `kAccentHi` | `RGB(124,176,224)` | accent hover / on-selected text |
| `kOnAccent` | `RGB(16,26,34)` | dark ink on accent fills |
| `kAmber` | `RGB(224,173,88)` | **values that changed from the card** (sliders, reset affordances, edit marks) |
| `kGood` / `kBad` | `RGB(106,190,130)` / `RGB(222,106,106)` | success / error |

`kAccent2` and every gradient were removed. No competing hues; amber means exactly one thing ("dirty vs card").

## Radius scale

`kRadSm` = 4 (small controls), `kRad` = 6 (controls, buttons, pills, rows), `kRadLg` = 8 (cards). Nothing else.

## Typography (Segoe UI / Consolas)

- `fontTitle` — Segoe UI Semibold 20 px: the wordmark only.
- `fontSemi` — Segoe UI Semibold 11 px: section captions, tab labels, card headings, quiet bold.
- `fontUi` — Segoe UI 14 px; `fontBold` — Segoe UI Bold 13 px; `fontMono` — Consolas 13 px; `fontLog` — Consolas 12 px; `fontSmall` — Segoe UI 11 px.
- Captions that used to be `fontBold` + accent are now `fontSemi` + `kSub` (quiet), uppercase.

## Widget kit (`Widgets.inc`)

- `Button(..., Btn kind)` — `Btn::Normal` (secondary, raised, hover `kSelect`), `Btn::Primary` (flat accent, dark ink, the *one* work action), `Btn::Ghost` (no chrome until hover), `Btn::Danger` (kBad outline).
- `CheckBox` — accent fill + `kOnAccent` tick; `kBad` when the thing failed.
- `DrawSliderRow` — unchanged behaviour (fill from card default to current, amber when changed), tokens only.
- `ScrollBar` — faint `kCard` track + `kLineHi` thumb.
- `DrawMenu` — one dropdown implementation (`kCard` body, `kLineHi` border, `kSelect` hover, accent-tinted current item); the material class filter, class picker and texture-slot dropdowns all use it.
- `DrawPresetChips`, `Expander`, `ViewOf`/`ScrollPx` — unchanged behaviour, new tokens.

## Layout (`LayoutFor`, `State.inc`)

Header band 0..64 (logo 50, wordmark, card title, stats, errors right) → toolbar y=72 h=30 (Export PMX is the only
Primary) → outfit rows → tab strip **2 rows × 5 pill tabs** (all tabs visible, no scrolling) → list
(parts/issues/sliders/bones/materials/optimize) → log (with scrollbar) → footer (keys + status).

## Rules for new UI

1. No raw `RGB()` outside token derivation/HSV conversion; no radius literals outside 4/6/8.
2. Amber only for "changed from card". Accent only for primary/selection/focus.
3. One `Button` style hierarchy per screen; at most one `Primary` visible.
4. Section captions quiet (`fontSemi`, `kSub`, caps); spacing over borders.
5. New popups are `DrawMenu`; new cards are `kRadLg` + 1 px `kLine`.
