# Massif — Fluid Simulation Roadmap

Rivers, from where the water goes to what it looks like a metre away.

`ROADMAP.md` holds the project milestones; this holds the water work, which
spans the environment bake, the terrain field, the water mesh and the surface
material, and does not fit under any one of them. Anything here that lands moves
into `ROADMAP.md` as a milestone and comes out of this file.

Read `ARCHITECTURE.md` §4 (determinism), §5.6 (the environment framework) and
the invariants in `CLAUDE.md` first. Everything below is constrained by them.

---

## Why

The rivers read as flat ribbons with no shape. That is not a shading problem —
`Shaders/MassifRiverSurface.ush` already does ripples, foam and absorption
through the column. It is a geometry problem, and there are five of them:

- **Position.** The channel is traced from an 8 m flow-accumulation grid and
  D8-routed, so it is a staircase turning in 45° steps. Simplifying and splining
  it (`MassifRiverNetwork.h:60-80`) replaces the staircase with a smooth curve
  that does not sit where a river would sit.
- **Width.** Purely a power law of discharge (`MassifRiverNetwork.cpp:73-80`),
  so a reach at constant discharge is a ribbon of constant width. Real channels
  narrow into confinement and widen over their own deposits.
- **Shoreline.** The wetted edge is a fixed offset from the centreline, so it is
  a perfect parallel curve. Nothing in the world has an edge like that. This is
  the single biggest tell.
- **Cross-section.** One parabola everywhere
  (`MassifTerrainField.cpp:30-35`). No bars, no undercut outer bends, no
  asymmetry.
- **Water surface.** The measured altitude, clamped monotone downstream and
  spline-smoothed (`MassifRiverNetwork.cpp:580,621`) — a smooth ramp. No pools,
  no riffles, no backwater above a constriction, no drops.

And one gap that is not about looks: discharge is catchment area times a **flat
constant**, `SpecificRunoffMetersPerSecond = 3.2e-8` (`MassifRiverNetwork.h:26`).
The whole precipitation, melt and infiltration solve — `Hydrology.SeasonalFlow`,
24 half-monthly slices — is not read by anything that draws a river.

## The constraint that decides the design

The Ötztal DEM is a 2 m laser scan and it already contains real channels: a
16 m flat water plane between 2.7 m banks in the wide reaches, an incised notch
in the narrow ones, meandering across the valley floor. It is tempting to detect
that and use it directly.

**Do not.** Two reasons, and the second is the binding one:

1. It is laser-scan-specific knowledge — flat returns over water — which is
   exactly what E0 forbids the environment layer from having. Terrain enters
   through `IMassifElevationSource` and nothing else.
2. Diffusion-generated terrain is coming in through the same `.massifdem`
   pipeline (`ROADMAP.md` M5f) and **has no channels in it at all**. A design
   that reads channels out of the elevation model produces nothing on those maps,
   and the maps cannot be hand-authored — they could be anything.

So the solve must **create** the channel, on any surface, knowing nothing about
where it came from.

The pleasant consequence: it fixes the Ötztal for free and with no special case.
The solve initialises from whatever the elevation source gives. On the Ötztal it
starts with a notch already partly there and refines it; on a diffusion map it
starts flat-floored and cuts one. Same code path. The measured channel gets used
without anything having been told it is measured.

## What the solve has to be

**Fluid alone will not carve.** Water over an un-incised valley floor spreads
into a wide shallow sheet and stays there — that is the correct answer to the
question a fluid solver asks. What makes a channel is sediment transport coupled
to the flow: shear stress above a threshold entrains, falling transport capacity
deposits, and the bed evolves by the Exner equation. Incision, banks, meander
migration, point bars and braiding are all consequences of that coupling, and
none of them come out of the flow on its own.

So: **2D shallow water + Exner over the river corridor, run to quasi-equilibrium
at world-gen time.** Grid-based, not particles. Particles were considered and
rejected: they give a free surface and not a bed, the bed is most of what reads
as shape, sub-metre coverage of a 20 km world is ~10⁹ particles, and SPH
neighbour sums are float-order-dependent, which fights the determinism contract
rather than fitting it.

## Where it runs, and what it costs

Corridor only. A 20 × 20 km world above the 1 km² channel-initiation threshold
carries roughly 150–300 km of channel; a 40 m corridor at 2 m cells is about
3 M cells, one to three percent of a world grid at the same resolution. At 1 m
it is about 12 M. Both are affordable; neither is affordable as a dense world
grid, which is why the storage below exists.

**Per reach, in the reach's own frame.** The domain is curvilinear — `s` along
the centreline, `n` across it — which bounds cost per reach, parallelises
naturally (reaches are independent given upstream inflow), makes the along/across
UVs and the flow map fall out of the coordinates, and packs into a rectangular
buffer per reach for an atlas.

Determinism is by fixed iteration count per reach, never a convergence test
(ARCHITECTURE §4). Sediment scatter into shared cells uses the fixed-point
integer atomics already established by `Soil.Deposition`.

## Ordering: the carve must not feed back into the bake

`MassifTerrainField.h:53-58` records the existing rule: the river field is a
layer over the elevation model, never under it, so the carve cannot feed back
into the solve that traced it. Keep that. The corridor solve reads the elevation
source and the finished hydrology channels; nothing upstream of it reads the bed
it produces. This is not a relaxation group and must not become one — putting
the whole hydrology solve inside a fixed-point iteration would multiply the
bake's cost for a second-order correction.

---

## Milestones

### F0 — Discharge from the water balance

Extraction is currently a free function, `MassifExtractRivers`
(`MassifEnvironmentGraph.cpp:1208`), called by the game mode against a finished
state. E3 recorded it as registered against the framework; it is not. Making it a
process is what lets it declare `Hydrology.SeasonalFlow` as an input rather than
guessing at runoff.

- `Hydrology.RiverExtraction` as a registered process. Declares `Altitude`,
  `FilledAltitude`, `FlowAccumulation`, `SeasonalFlow`.
- Discharge becomes the accumulated water balance rather than area × constant.
  `SpecificRunoffMetersPerSecond` is deleted.
- Hydraulic geometry stays as it is — this milestone changes how big rivers are,
  not what shape they are.
- **Verify:** tests — a wet catchment and a dry catchment of equal area produce
  different discharge; total discharge at the world border matches the water
  balance's own inflow minus evapotranspiration within tolerance; determinism
  hash. Manual — the trunk and its tributaries are in a plausible ratio.

### F1 — Corridor channels

The one framework change the whole thing needs. `FMassifEnvironmentState`
stores channels as world grids at a per-channel cell size (E1); a 2 m world grid
is 400 MB per channel and most of it is dry ground.

- A corridor storage kind: per-reach strips in the reach's `(s, n)` frame,
  registered and sampled through the same `FMassifChannelHandle` API so nothing
  downstream learns a second way to read a channel.
- World-position sampling maps into the nearest reach's frame, which is the
  spatial-index problem `FMassifRiverField` already solves.
- Debug channel views, probe and transect (E5) work on corridor channels
  unchanged — that is the test of whether the abstraction held.
- **Verify:** tests — round-trip through the encoding, sampling agrees with
  `ReadCell` at cell centres, a corridor channel and a world channel sampled at
  the same position agree where they overlap, the registry walk covers corridor
  channels without being taught about them.

### F2 — Channel carve

`Hydrology.ChannelCarve`. Outputs `Water.BedAltitude`, `Water.BankHeight`.

- Sediment transport against the flow: shear stress from depth and slope,
  entrainment above a threshold, deposition as capacity falls, bed updated by
  Exner. Bank erosion on the outer bend, deposition on the inner.
- `Geology.RockHardness` gates incision rate, so the lithology work already in
  the graph reaches the channel.
- **Off** is the current analytic hydraulic-geometry trench, which works on any
  surface — so `Off` is a usable level on a diffusion map rather than a sheet of
  water on flat ground. `MinCarveDepthMeters`, the hand-hack that puts a bed
  under the Ötztal's flat laser ribbons, is deleted: the carve removes that
  material because the physics says to, not because the format was known.
- **Verify:** tests — a channel cut into an analytic tilted plane is deeper at
  higher discharge and steeper slope; mass is conserved between eroded and
  deposited volume; an outer bend erodes and the matching inner bend deposits;
  determinism hash; `Off` reproduces today's trench exactly.

### F3 — Channel flow

`Hydrology.ChannelFlow`. Outputs `Water.Depth`, `Water.SurfaceAltitude`,
`Water.Velocity` (vector2), `Water.Froude`.

- 2D shallow water over the carved bed, integrated to steady state at a fixed
  step count.
- Fidelity ladder: **Off** fills to the hydraulic depth (today's behaviour);
  **Cheap** is Manning normal depth per cross-section, no time integration, which
  already gives width and depth that vary with local slope and a wetted edge that
  follows the real cross-section; **Standard** is the 2D solve, which adds
  backwater above constrictions, pool-riffle sequences, steepening into drops and
  wetted extent that responds to season; **Reference** runs the seasonal slices
  rather than one representative discharge.
- **Verify:** tests — a prismatic channel of known slope and roughness reproduces
  the closed-form normal depth; discharge is conserved through a constriction;
  water surface is monotone non-increasing downstream; the Froude number crosses
  1 at a known drop; CPU determinism hash across runs.

### F4 — Consumers rewired

The point of the whole thing. Four consumers stop guessing and read the solve.

| consumer | reads | replaces |
| --- | --- | --- |
| `FMassifTerrainField::CarveChannel` | `Water.BedAltitude` | the fixed parabola |
| `MassifWaterTileMesh` boundary | `Water.Depth > 0` | fixed offset from the spline |
| water mesh vertex Z | `Water.SurfaceAltitude` | the monotone-clamped ramp |
| `MassifRiverSurface.ush` flow | `Water.Velocity` as a flow map | per-node `FlowDirection` |
| `MassifFoamAmount` | `Water.Froude` | a speed-fraction heuristic |

`FMassifRiverField` survives as the thing that answers "what is the river doing
here", which is what its header already promises — it stops computing the answer
from segment attributes and starts reading it from the corridor channels. The
four-consumers-agree-by-construction property is preserved.

- **Verify:** tests — the water mesh's wetted boundary matches `Water.Depth`
  crossing zero, the mesh surface matches `Water.SurfaceAltitude` vertex for
  vertex, the terrain under a river matches `Water.BedAltitude`, tile boundaries
  still agree. Manual — you look at a river.

### F5 — Live solve near the camera

Presentation-only, and the last thing. Nothing in `MassifSim` may depend on it or
the sim/presentation invariant breaks.

- A fine solve over the nearest quadtree chunks, boundary-fed by the baked
  `Water.Depth` and `Water.Velocity` at the domain edge so it agrees with the
  bake where it meets it.
- Objects and the player displace water; the displacement never leaves
  presentation.
- Particles are reasonable *here* — a few thousand over a hundred metres, for
  spray and foam advection — for the same reason they were wrong for the bake:
  the question is what the surface is doing this second, not what the bed settled
  into over centuries.
- **Verify:** the residency boundary is invisible; the sim's state hash is
  unchanged by anything this does, tested by running with it on and off.

---

## Out of scope

Waterfalls as their own geometry, lakes beyond `Hydrology.StandingWaterDepth`,
glacial melt as a distinct source, ice and freeze-over, floods as weather events,
and swimmable or rideable water. Some of these become reasonable once F3 exists;
none of them are this work.

## Open questions

- **Corridor resolution.** 2 m matches the source data and costs ~3 M cells;
  1 m resolves a brook properly and costs ~12 M. Decide against a measured F3
  solve time, not in advance.
- **Confluences.** Two reaches meeting have overlapping corridors, and the water
  surface has to be single-valued there or it draws as two sheets in the same
  place. `FMassifRiverField` currently resolves this by taking the channel whose
  bank is furthest away. Whether that survives a real solve is unknown.
- **Does F3 `Cheap` carry the look on its own?** If Manning normal depth over an
  F2 bed already reads as a river, `Standard` becomes a fidelity option rather
  than the target, and the milestone gets much cheaper.
