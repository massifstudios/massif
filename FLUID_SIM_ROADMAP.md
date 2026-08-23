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

## What the solve produces, and where it lives

Not a raster. `FMassifTerrainField::SampleHeightMeters` is a point query at
whatever spacing the caller is about to mesh at, down to 14 cm at the finest
quadtree depth, and it reads the channel through `FMassifRiverField` — which is
an analytic function of the polyline, not a lookup. A stored grid could not
answer it at any one resolution and be right.

So the solve's output is **per-station cross-sections**: at every station along a
reach centreline, a profile across the channel — bed altitude at a fixed set of
offsets, water surface altitude, mean velocity, Froude number. Roughly 300 km of
channel at 4 m stations and 33 offsets is about 30 MB, and it reconstructs at any
resolution, which is what the terrain field needs. It is also the natural output
of a curvilinear `(s, n)` solve, so nothing is resampled on the way out.

That makes it an extension of `FMassifRiverReach`, which already carries
per-node attributes that `FMassifRiverField` interpolates and indexes. No new
subsystem, and no channel storage that a 2 m world grid would need 400 MB per
channel for.

**Per reach.** Reaches are independent given upstream inflow, so they solve in
parallel, and each one's domain is bounded. Determinism is by fixed iteration
count per reach, never a convergence test (ARCHITECTURE §4). Sediment scatter
into shared cells uses the fixed-point integer atomics already established by
`Soil.Deposition`.

Alongside it, coarse `Water.*` channels at the hydrology's own 8 m, so the
result is visible in the debug channel views and readable by habitat and masks
like anything else. Those are ordinary channels and need no framework change.

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

`Hydrology.SeasonalFlow` is not the channel this needs, despite the name. It is
an effective *area* in m², and it normalises the precipitation by the year's own
total (`ShareOfYear`), so it carries when the water moves and not how much of it
there is. Two catchments of one size on the wet and dry flanks of a range read
identically through it.

River extraction stays a free function — a process writes channels and a river
network is a set of polylines, which `MassifEnvironmentGraph.h:116` already
records. What it reads changes.

- `Hydrology.Discharge`, m³/s, Float32 at the hydrology's own 8 m: the year's
  runoff routed downhill. `MassifBakeCpu::RouteInflow` is `RouteFlow` over a
  per-cell contribution instead of a per-cell area; the catchment channel is
  untouched, because a catchment is ground and does not become something else
  because the rain over it is uneven.
- Runoff per cell is `Precip.Annual − Climate.Evapotranspiration`, floored at
  zero, both optional so the water solve still stands alone in a graph with no
  weather in it. Infiltration is deliberately not subtracted: over a year what
  soaks in returns at a spring, so it moves timing rather than volume, and
  `SeasonalFlow` is where that already lives.
- `Off` sheds `FallbackRunoffMillimetersPerYear` evenly, which reproduces the
  deleted `SpecificRunoffMetersPerSecond` exactly. So `Off` is today's rivers,
  and the fidelity switch is a real before-and-after.
- Hydraulic geometry stays as it is — this milestone changes how big rivers are,
  not what shape they are.
- **Verify:** tests — everything a plane yields crosses its downhill border,
  `Off` is the catchment times one depth at every probe, and the windward flank
  of a ridge carries more than the lee flank it mirrors while draining the same
  catchment. Manual — the trunk and its tributaries are in a plausible ratio.

### F1 — Cross-sections and normal depth

The smallest thing that changes the look, and the hydraulic engine F2 and F3 are
both built on. It replaces the constant-width ribbon with a channel that fits the
ground it is in.

- `FMassifChannelProfile` on each river node: bed altitude at a fixed set of
  offsets across the channel, taken from the elevation source, plus the solved
  water surface, wetted width, mean velocity and Froude number.
- Normal depth by Manning against the reach's local slope and the node's
  discharge, solved on the measured cross-section rather than on a power law. A
  confined reach runs deep and narrow, an open one shallow and wide, at the same
  discharge — which the hydraulic geometry cannot express, because it knows only
  the discharge.
- Roughness from `Geology.RockHardness` and the reach's own slope.
- `FMassifRiverField::Sample` reports depth and surface from the profile.
- **Verify:** tests — a prismatic trapezoid of known slope and roughness
  reproduces the closed-form normal depth; halving the width at fixed discharge
  raises the surface; the wetted edge sits where the profile crosses the solved
  surface; determinism hash over the profiles.

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

- **Where the solve is registered.** It is a pure function over the elevation
  source and the network, like `MassifExtractRivers`, and its main output is not
  a channel. The coarse `Water.*` channels are, and a process that wrote them
  would have to run the solve a second time or read a result no channel carries.
  Resolve before F3, not before F1 — F1 needs no answer.
- **Station spacing and offset count.** 6 m stations and 33 offsets are a guess at
  what reads as a river.
- **`MaxWettedWidthFactor` on the Ötztal.** F1 puts the widest channel in the
  world at 45 m against a hydraulic-geometry width of 17 m for the same 12.8 m³/s.
  A braided gravel flat does that, and so does a solve spilling across a flat
  valley floor, and the two are not distinguishable from the number. Worth a look
  before the guard is loosened or tightened.
- **Confluences.** Two reaches meeting have overlapping corridors, and the water
  surface has to be single-valued there or it draws as two sheets in the same
  place. `FMassifRiverField` currently resolves this by taking the channel whose
  bank is furthest away. Whether that survives a real solve is unknown.
- **Does F3 `Cheap` carry the look on its own?** If Manning normal depth over an
  F2 bed already reads as a river, `Standard` becomes a fidelity option rather
  than the target, and the milestone gets much cheaper.
