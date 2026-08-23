# Massif — UI Design System

Visual and structural decisions for everything drawn on top of the world.
Same standing as `ARCHITECTURE.md`: changes here are deliberate decisions,
not side effects of implementation work. Where this file and a widget
disagree, the widget is wrong.

Scope note: the only UI that exists today is developer tooling — the
environment settings screen (`Source/MassifPresentation/Private/UI/`) and
the debug HUD and channel views (`Private/Debug/`). Player-facing UI is not
in the current `ROADMAP.md` phase. This document defines the foundation
layer and the components the tooling already needs; it does not design a
player HUD ahead of scope.

## 1. Direction

Massif renders real Tirol laserscan terrain and simulates thirty channels
over it. The honest visual ancestor is not game UI — it is **Swiss
topographic cartography** (swisstopo / IGN map sheets) crossed with **alpine
resort signage**. Both are already inside the data: contour lines, hillshade,
ramp legends, piste-difficulty colours, the avalanche danger scale.

The target impression is a scientific instrument panel for a mountain, not a
menu. Muted, cold, high signal-to-noise, generous negative space.

Ruled out, explicitly: gradients, drop shadows, bevels, glow, glass
refraction, sci-fi HUD framing, rounded pill buttons, decorative iconography.
None of them survive being drawn on top of terrain that the player is
supposed to be reading.

## 2. Two colour systems that never touch

| System | Owns | Rule |
|---|---|---|
| **Chrome** | Surfaces, text, borders, selection, staged change, warning, disabled. | Cool neutral greys plus exactly one accent. |
| **Data** | Channel ramps, classified palettes, the difference view's signed map. | Perceptual ramps only. Never used for UI state. |

The systems are separate because a legend that shares a hue with the
selection colour is a legend that lies. Today's selection blue
(`SMassifEnvironmentSettings.cpp:17`, `0.16, 0.44, 0.72`) collides with the
water and wetness ramps; the accent moves, not the ramps.

**Domain palettes are canon and reserved.** Piste difficulty (green, blue,
red, black) and the five-step avalanche danger scale (green, yellow, orange,
red, black) are scales players already know. Neither may be spent on chrome.

### 2.1 Chrome tokens

Six cool neutrals from near-black to bone white, one accent in cold glacial
cyan-blue, one amber for staged-but-unapplied change (the settings screen
already needs this distinction and encodes it inline at
`SMassifEnvironmentSettings.cpp:18`), one red for error.

Panels are dark and translucent over terrain. The existing panel background
(`0.02, 0.02, 0.03, 0.94`) is correct and becomes the token rather than a
literal.

### 2.2 Data ramps

Sequential (viridis, magma), classified, and a diverging blue–white–brown
ramp for the E5 difference view. These live with the channel registry, not
with the widget styles — a channel carries its own colour map per
`ARCHITECTURE.md` §5.6.2, and the design system's job is only to render the
legend around it consistently.

## 3. Scales

Pick once, never interpolate between steps.

- **Spacing:** 4 px base. 4 / 8 / 12 / 16 / 24 / 32 / 48. Nothing else.
- **Type:** one neutral grotesque (Inter or IBM Plex Sans), five sizes, two
  weights. **Tabular lining figures are mandatory** — this UI is mostly live
  numbers, and proportional figures make them jitter as they update.
  Uppercase letterspaced for labels and section headers only.
- **Radius:** 2 px. Sharp reads cartographic; rounded reads mobile app.
- **Borders:** 1 px hairline, single colour token.
- **Motion:** state changes are instant or ≤120 ms. Nothing eases in on a
  panel the user opened with a hotkey.

## 4. Implementation

Tokens and styles live in C++ in the presentation module:

```
Source/MassifPresentation/Private/UI/Style/
  MassifDesignTokens.h    colour, spacing, type, radius, motion — no widget deps
  MassifStyle.h/.cpp      FSlateStyleSet, registered from the module startup
  MassifWidgetStyles.h    FButtonStyle / FTextBlockStyle / FCheckBoxStyle per role
```

The style set is created in `FMassifPresentationModule::StartupModule` and
unregistered on shutdown, so hot reload does not leak brushes.

**Hard rule: no colour literal in any `S*` widget.** A widget names a token
or a widget style; it never constructs an `FLinearColor`. This is
grep-checkable, so it is enforced by an automation test in the same spirit as
the E0 check that no environment source includes `MassifHeightmap.h`. Without
that test this document is a coat of paint.

Debug channel colour maps are the one exemption — they are data, they come
from the registry, and they are not chrome.

## 5. Components

The inventory is driven by what the environment tooling already needs (E4,
E5, and the open items in `NEXT.md`), not by a generic UI kit.

| Component | Notes |
|---|---|
| `Panel` | Translucent surface, hairline border, optional header. |
| `SectionHeader` | Uppercase letterspaced label, rule beneath. |
| `SegmentedControl` | Fidelity Off / Cheap / Standard / Reference, and the global presets of `NEXT.md` item 8. Carries the staged-change state. |
| `Row` | Label, optional description line, right-aligned control, right-aligned readout. The description line is where `NEXT.md` item 11 lands. |
| `NumericReadout` | Tabular figures, unit suffix, fixed decimal places. |
| `BusyState` | Named, non-blocking-looking indicator for the 10.6 s solve (`NEXT.md` item 6). A frozen panel with no indicator currently reads as a crash. |
| `ChannelLegend` | Ramp, channel name, unit, min/max. Built from the registry. |
| `Histogram` | Thin bars, range readout. |
| `TransectGraph` | Multiple thin series over a distance axis. |
| `TimeScrubber` | 24 half-monthly ticks, one handle, slice label. |
| `ProbeCard` | Channel, value, unit, writing process — one line each. |
| `GraphNode` | Process graph node with fidelity, dirty state, last execution time. |
| `Toast` | Transient result of an apply or a regenerate. |
| `HotkeyHint` | The screen is opened with F1 and closed with Escape; say so. |

## 6. Sequencing

1. Tokens, style set, and the grep test. No visual change yet.
2. `SegmentedControl` and `Row`.
3. Reskin `SMassifEnvironmentSettings` onto them. It is 274 lines and already
   carries selected / staged / plain states, which makes it the right single
   screen to prove the system on.
4. Only then, the debug HUD and the channel legends.

Keyboard traversal and Escape-to-close (`NEXT.md` item 10) come with step 2,
not after it — focus visuals are a token-level concern and retrofitting them
across finished widgets costs more than building them in.

## Appendix A — Image generator prompt

For visualising the direction before any of it is built. Not a spec; the
sections above are the spec.

```
UI design system sheet for a mountain ski-resort building game, rendered as a
single flat-lay style guide on a very dark charcoal background (#0A0B0D).

Aesthetic: Swiss International Typographic Style crossed with alpine
topographic cartography. Precise, technical, cartographic. Hairline 1px
borders, 2px corner radius, no gradients, no drop shadows, no glow, no
skeuomorphism, no glass refraction, no sci-fi HUD tropes.

Layout, arranged on a strict 8px grid across three columns:

LEFT COLUMN — foundations. A color token row: six cool neutral greys from
near-black to bone white, each a small square with a hex label beneath. Below
it, a single accent swatch in cold glacial cyan-blue. Below that, a labelled
"piste difficulty" row of four swatches: green, blue, red, black. Below that,
a five-step avalanche danger scale: green, yellow, orange, red, black. Below
that, three horizontal perceptual color ramps (viridis-like purple-to-yellow,
magma-like black-to-cream, and a blue-white-brown diverging ramp), each with
min/mid/max numeric tick labels.

MIDDLE COLUMN — a dark translucent settings panel floating over a faint
grey hillshaded mountain terrain background. Panel header reads
"ENVIRONMENT SIMULATION" in small uppercase letterspaced type. Below it, six
rows, each row a left-aligned process name ("Sun Insolation", "Wind Field",
"Snow Depth", "Soil Deposition", "Vegetation Cover", "Precipitation") and a
right-aligned four-segment control with the segments labelled OFF, CHEAP,
STANDARD, REFERENCE — one segment highlighted in the accent color, one shown
in an amber "staged change" state. A right-aligned monospaced tabular number
in each row showing elapsed seconds like "10.62 s". Footer with two small
buttons: "RESET" and "APPLY".

RIGHT COLUMN — data components. A small legend card with a color ramp,
channel name "Snow.Depth", unit "m", and range 0.00–4.20. Below it a compact
histogram with thin bars. Below it a line-graph transect card with two thin
plotted series over a distance axis. Below it a half-monthly time scrubber:
a horizontal track with 24 tick marks and one highlighted handle labelled
"MAR I". Below it a node-graph fragment: four small rectangular nodes joined
by thin orthogonal connector lines, one node outlined in amber to indicate
dirty state.

Typography: a single neutral grotesque sans (Inter / IBM Plex Sans feel).
Uppercase letterspaced labels, tabular lining figures for all numbers.
Text is legible and correctly spelled.

Overall impression: a scientific instrument panel for a mountain, not a
video game menu. Muted, cold, high signal-to-noise, generous negative space.
Flat vector UI mockup, orthographic, front-on, no perspective, no mockup
device frame, no hands, no photo texture.
```

Negative prompt, where the tool takes one:

```
photorealistic, 3d render, bevel, glossy, neon, purple gradient, sci-fi hud,
cluttered, lens flare, blurry text, gibberish text, watermark, drop shadow,
rounded pill buttons, mobile app
```
