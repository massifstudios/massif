# Massif — Roadmap

Current phase: **Phase 1 — Playable Winter Core**. Everything not listed
under Phase 1 is out of scope until this file says otherwise.

Phase 1 goal, end to end: imported massif with snow → place one lift and one
slope → guest NPCs arrive, queue, ride up, ski down, repeat.

Each milestone must be runnable/testable in isolation, compile clean
(Development Editor), pass the automation suite, and end in a pushed
commit.

## Phase 1 Milestones

### M0 — Project Skeleton
- `.uproject`, four modules (`MassifCore`, `MassifSim`, `MassifWorldGen`,
  `MassifPresentation`) with correct dependency directions, git + LFS
  setup, `.gitignore`.
- One trivial automation test per module proving the test harness runs
  headless.
- **Verify:** project compiles; `Automation RunTests Massif` passes under
  `-nullrhi`.

### M1 — Base Heightfield & Terrain View
- `MassifCore`: master seed, `HashCombine` channel derivation, state
  hashing utilities (needed for worldgen determinism tests).
- Ridged/uplift fractal noise heightfield (CPU, deterministic, into
  `FWorldGenResult`).
- Chunked terrain meshes via Geometry Script (`UDynamicMeshComponent`),
  distance LOD, World Partition registration. Free-fly management camera
  (pan/zoom/rotate).
- Height/slope query API against `FWorldGenResult` (sim consumes this
  later without touching meshes).
- **Verify:** test — same seed → identical heightfield hash, different
  seed → different hash, height/slope queries sane; manual — fly over a
  plausible massif, seed change gives a different mountain.

### M2 — Deterministic Sim Core — **still open**
Skipped at the time and never returned to: `MassifSim` is an empty module, one
`IMPLEMENT_MODULE` line and a smoke test. Everything from W1 on is blocked on it,
as are M7–M11. Easy to miss given how far the worldgen side has run ahead.

- `MassifSim`: `UMassifSimSubsystem`, `FMassifSimState`, fixed 10 Hz tick
  with accumulator, game-speed control (pause/1x/4x), `FSimCommand` queue
  with one dummy command.
- **Verify:** tests — same seed → identical state hash after N ticks;
  command applied on tick boundary, not mid-frame; speed change alters
  tick count, not tick delta.

### M3 — Erosion (GPU) — done, partly superseded
- Hydraulic erosion (virtual pipes model) + thermal erosion as compute
  shader passes over the generated heightfield, results read back into
  `FWorldGenResult`. Skipped gracefully under `-nullrhi`.
- Erosion parameters in `FWorldGenConfig` (iteration counts, rain,
  sediment capacity, talus angle).
- **Verify:** GPU reproducibility test (two runs, same seed, identical
  hash; auto-skips headless); CPU-stage tests unchanged; manual —
  drainage gullies, smoothed flanks, sediment-flattened valley floors.
- **Superseded by M4 and M4b.** A grid-and-neighbours erosion pass over
  the play area cannot be part of a terrain field that must be evaluable
  point-by-point at any resolution. Macro relief now comes from the
  coarse fluvial bake (M4b), incising below it is analytic (ARCHITECTURE
  §5.1). The compute passes are deleted.

### M4 — Terrain Field & World Scale
Replaces the stored 8 x 8 km heightfield with a terrain field evaluable at
any point and any resolution, and opens the world to 150 × 150 km,
playable throughout. See ARCHITECTURE §5 and §12.

- `FMassifTerrainField`: `Height(FVector2d, spacing)` and slope, CPU-only,
  deterministic, no global grid. Octave count keyed to sample spacing;
  finest octave faded in, never switched, so LOD levels agree.
- Coordinates widened to double throughout worldgen. Float resolves only
  ~8 mm at 150 km, which would spend the detail budget on coordinate error.
- Hydrology graph reworked: hierarchical, lazily refined from stable node
  ids, uniform spatial index over segments so point queries are local
  instead of looping every node. Eager to ~1.25 km spacing world-wide.
- Uplift/range layer (20–80 km) so a 150 km world has ranges and basins
  rather than one massif tiled.
- Erosion filter retuned for the new scale, and its octave stack keyed to
  sample spacing rather than a fixed count.
- `FWorldGenResult` retired as a monolithic heightfield; its query API
  moves onto the field, so M7–M11 are unaffected.
- **Verify:** tests — chunk independence (a tile sampled alone is
  bit-identical to the same tile sampled inside a larger block), LOD
  agreement across spacings, surface sanity (max step between adjacent
  samples, local-minimum fraction), determinism hash. Manual — fly from
  ground level out to the 150 km horizon.
- **Hydrology bullets superseded by M4b:** the hierarchical river graph
  is replaced by river polylines extracted from the fluvial bake.

### M4b — Fluvial Macro Rework
Replaces the synthetic river-graph growth with a coarse whole-world
stream-power erosion bake plus analytic amplification. See ARCHITECTURE
§5.1–§5.2.

- `FMassifFluvialBake`: priority-flood pit removal, D8 receivers,
  drainage accumulation, implicit stream-power solve against the uplift
  field, world border as base level. ~128 m cells, once per seed,
  CPU-only, deterministic, never serialized.
- River polylines extracted from the receiver graph above a discharge
  threshold, water elevation clamped monotone downstream, uniform
  spatial index.
- Field query becomes bake bicubic + discharge-scaled channel carve +
  gully erosion filter; the orientation lattice is replaced by bake
  gradients.
- River-graph growth (`RiverNetwork`) and the M3 GPU erosion passes are
  deleted, along with their config parameters.
- **Verify:** tests — bake determinism hash, every cell drains to the
  border, water elevation monotone downstream, existing field contract
  tests unchanged. Manual — debug-export heightfield/hillshade show
  dendritic drainage; fly-through reads as a real massif.

### M5 — LOD Streaming & Terrain Rendering
- Quadtree of mesh tiles sampled from the field: the root tile covers the
  whole world, tiles near the camera split in four down to ~14 cm vertex
  spacing. One component per tile, so engine frustum culling applies.
  **Done** — splits and merges swap only once replacement meshes are
  built, skirts hide depth boundaries; startup no longer materialises a
  preview window.
- Still open: vertex morphing across depth boundaries (skirt-only now, so
  fine detail pops in as tiles split), and collision meshes for gameplay.
- Resident tile manager (§5.4): 512 m tiles generated on demand around
  camera and player activity, evicted when far, keyed by tile coordinate.
  Not needed until something persists per-tile state.
- **Verify:** tests — a tile mesh matches the field exactly, its skirt
  closes the boundary. Manual — fly from ground level to the horizon with
  no seams or holes.

### M5c — Small World Rescale & Live Erosion Tuning
World shrinks to 20 × 20 km (ARCHITECTURE §12) so the fluvial bake grid
resolves valley-floor shape (~16 m cells) instead of faking it below cell
size — the 150 km world's valleys were not buildable.

- Rescale worldgen defaults: extent, altitude band, uplift wavelengths,
  bake cell size, river threshold, sample warp.
- Terrain field can adopt an existing bake, so a rebuild that only touches
  the analytic erosion-filter layer skips the stream-power solve.
- Live tuning: `massif.Gully.*` console variables for the erosion-filter
  knobs (depth, wavelength, branch strength, ridge/crease rounding, slope
  reference); changing one rebuilds the field with the shared bake and
  resets the terrain quadtree. `massif.Regenerate` forces a full rebuild
  including the bake, picking up `massif.Seed` without a level restart.
- **Verify:** tests — shared-bake field bit-identical to a freshly built
  field, gully-knob change leaves the macro layer identical; suite green.
  Manual — tweak a gully cvar in the console, terrain regenerates within
  ~1 s plus tile rebuild; hillshade export shows broad connected valleys.

### M5d — Filter-Only Terrain
The fluvial bake (M4b) and its channel carve never produced buildable,
natural valleys at any tuning; deleted (git carries it). Terrain is now
uplift envelope + fractal base relief + the erosion filter alone
(ARCHITECTURE §5.1). Rivers and global drainage are open again — future
milestone, per-tile from the sampled surface.

- **Verify:** tests — field contract suite still green without the bake;
  knob changes reach the ground. Manual — fly-through; filter fidelity
  against the reference article is the active tuning question.
- **Superseded by M5e.** The filter tuned convincingly at gully scale but
  never at valley scale, and no setting made ground a resort could be built
  in. Procedural macro relief is abandoned; terrain is imported.

### M5e — Imported Terrain
Procedural macro relief is abandoned. Three attempts (M3, M4b, M5d) never
produced valleys that read as a real massif or that a resort could be built
in; the world is now a real 20 × 20 km window of Tirol, refined from there.
See ARCHITECTURE §5.

- `Tools/fetch_tirol_dem.py`: fetches the Land Tirol laserscan terrain model
  (CC BY 4.0) over a lat/lon window from its WCS, mosaics the request tiles,
  and writes a `.massifdem` plus a hillshade preview. Pure stdlib + numpy, no
  GDAL. Checks the window against the state boundary with one coarse request
  before spending a full download on ground that is not there.
- `FMassifHeightmap`: `.massifdem` loading, mip chain, Catmull-Rom sampling
  with mip blending. `FMassifTerrainField` keeps its API and becomes the layer
  stack over it.
- Deleted: uplift field, fractal base relief, the analytic erosion filter,
  `FWorldGenResult`/`FWorldGenPipeline`/`FWorldGenConfig`, the debug
  heightfield export, and the F1 gully-tuning panel with its `massif.Gully.*`
  cvars — the knobs tuned a surface that no longer exists.
- **Verify:** tests — sample fidelity against the source altitudes, chunk
  independence, LOD agreement and continuity across mip boundaries, border
  behaviour, `.massifdem` format round-trip; all against an in-memory
  heightmap, so a bare checkout stays green. Manual — 20 km of the Ötztal at
  Sölden, 1168–3757 m, loads in 0.3 s and renders. **Done.**

### M5g — Environment Bake: Water — done
Habitat currently guesses water from local slope and a noise field, so valley
floors are no wetter than the shoulders above them and ground type is a slope
threshold. Replace the guesses with a world-wide solve at world gen — where
water goes, where it stands, and where the sediment it carried settled. See
ARCHITECTURE §5.6.

- `FMassifEnvironmentBake`: 8 m cells over the world, built from the heightmap
  and the master seed. Depression fill (Planchon–Darboux relaxation) →
  `StandingWaterDepth`; multiple-flow-direction routing over the filled surface
  → `FlowAccumulation`; seeded droplets eroding and depositing against a
  sediment capacity → `SoilDepthMeters`; topographic wetness index → `Wetness`.
- CPU reference implementation and a GPU compute path behind one interface.
  Fixed iteration counts, integer atomics on fixed-point for droplet scatter,
  so both paths are reproducible run to run and machine to machine.
- `FMassifHabitatField` rewired: humidity from `Wetness` instead of the
  four-neighbour convergence probe and weather noise; ground type from
  `SoilDepthMeters` and standing water instead of slope thresholds alone.
- Debug channel views (ARCHITECTURE §5.7): number keys paint a bake channel
  onto the terrain, legend top-right, 0 returns to the normal material.
- **Verify:** tests — bake determinism hash across runs, every cell drains to
  the border or to a filled depression, filled surface never below the measured
  one, flow accumulation sums to the cell count, CPU and GPU paths agree within
  tolerance (GPU test auto-skips headless), habitat contract tests updated to
  the bake. Manual — number keys show dendritic drainage in the real Ötztal
  valleys, standing water only in hollows, forest following wetness and soil
  rather than a contour.

### M5h — Environment Bake: Sunlight — folded into E6
Superseded as a standalone milestone. Insolation is one process in the
environment framework below, and lands as E6 with the rest of it.

## Environment Simulation (E-series) — complete

The whole environment is simulated and verified before any work on visual
fidelity or game content. M5g proved the approach on four channels wired
together by hand; that arrangement does not survive thirty channels, runtime
fidelity switching, or the feedback loops between vegetation, soil, wind and
snow. E0–E3 replace the hand-wiring with a channel graph and change no
behaviour; E4–E5 build the switching and the verification tools; E6 onward add
one system per milestone, each a single registration against the framework.

Two hard requirements run through all of it:

1. **Map independence.** Nothing in the environment layer may know what terrain
   it is running over. Terrain enters through `IMassifElevationSource` and a
   georeference, and through nothing else, so another elevation model can be
   slotted in at any point. The two exemptions are the two quantities that are
   genuinely properties of a place on Earth rather than of a shape: air
   temperature reduced to the datum, and the sun's position and strength.
2. **Every channel is visualisable the moment it exists.** A simulated quantity
   whose only evidence is a second-order effect on where trees ended up cannot
   be verified, and will be wrong for months without anyone noticing.

### E0 — Elevation Source Interface — done
Break the environment layer's dependency on `FMassifHeightmap`.

- `IMassifElevationSource`: altitude by position and sample spacing, world
  extent. Pure, thread-safe, the only terrain the environment layer sees.
- `FMassifHeightmapElevationSource` adapter; `FMassifGeoreference` (latitude,
  longitude, EPSG, datum mean temperature) carried beside it.
- `FMassifAnalyticElevationSource`: cone, ridge, tilted plane, single pit,
  saddle. Terrain for tests that needs no DEM on disk and solves in
  milliseconds.
- `FMassifEnvironmentBake::Build` takes the interface instead of the heightmap.
- **Verify:** tests — analytic sources return their closed-form altitudes, the
  heightmap adapter agrees with `FMassifHeightmap::Height` sample for sample,
  a bake over an analytic cone drains radially outward. Grep-level check that
  no environment source includes `MassifHeightmap.h`.

### E1 — Channel Registry & State — done
Channels become registered and self-describing rather than an enum.

- `FMassifChannelDescriptor`: name, type (scalar/vector2/classified), encoding,
  own cell size, time-slice count, unit, description, colour map, display range.
- `FMassifEnvironmentState`: storage keyed by name, resolved to
  `FMassifChannelHandle` once so inner loops index rather than hash. Bilinear
  for scalars and vectors, nearest for classified. Per-channel resolution, so a
  64 m climate channel costs a sixty-fourth of an 8 m hydrology channel.
- Determinism hash over channels in name order.
- **Verify:** tests — round-trip through each encoding within its quantisation
  step, sampling agrees with `ReadCell` at cell centres, channels at different
  resolutions sample consistently at the same world position, classified
  channels never interpolate, hash stable across registration order.

### E2 — Process Graph & Scheduler — done
- `IMassifEnvironmentProcess`: declares inputs, outputs, supported fidelities;
  `Execute(Context, Fidelity)` may touch nothing but the context. Optional GPU
  path, CPU otherwise and always headless.
- Scheduler: topological sort from the declared dependencies, Tarjan SCC to
  find feedback groups, fixed iteration count per group — never a convergence
  test (ARCHITECTURE §4).
- Dirty propagation: a changed parameter or fidelity re-runs that process and
  its transitive dependents, nothing else.
- Seeded streams derived per process name, so adding a process cannot shift
  another's random sequence.
- **Verify:** tests — declared order independent of registration order, a
  cycle lands in a relaxation group instead of failing, a process reading an
  undeclared channel is rejected, dirty marking re-runs exactly the dependent
  set, two schedules of the same graph produce identical hashes.

### E3 — Port the Existing Bake — done
The six M5g stages become six registered processes, bodies unchanged.

- Depression fill, standing water, flow routing, droplets, wetness, river
  extraction registered against the framework; `FMassifBakeSurfaces` and
  `EMassifBakeChannel` deleted; `FMassifEnvironmentBakeParams` split so each
  process owns its own parameters.
- **Verify:** tests — the bake's content hash is bit-identical to the
  pre-port hash on the same seed and elevation model. This is the whole point
  of the milestone; a port that changes a value is a failed port.

### E4 — Fidelity Levels & Graph Inspector — done
- `EMassifFidelity` (Off / Cheap / Standard / Reference) per process. `Off`
  still writes its channels, with a constant or trivial analytic value, so no
  downstream code ever branches on whether a system is enabled.
- Runtime UI: process list with a fidelity dropdown each, global presets, and
  the dependency graph drawn with per-node fidelity, dirty state and last
  execution time.
- Re-solve on change, off the game thread, previous result held until the new
  one lands.
- **Verify:** tests — every process produces its declared channels at every
  fidelity it claims including `Off`, switching fidelity and switching back
  reproduces the original hash, `Off` output is independent of the seed.

### E5 — Verification Tools — done
Built before the systems that need verifying.

- **Probe:** click a point, panel lists every channel's value there with units
  and the process that wrote it.
- **Vector overlay:** wind and flow direction as arrows or line-integral
  convolution. A scalar heatmap cannot show a direction field being wrong.
- **Transect:** drag a line, graph any set of channels along it.
- **Difference view:** one channel at two fidelities, signed colour map.
- **Histogram and range readout** per channel.
- Debug channel selection moves from number keys to a list; the hand-written
  switches in `MassifDebugChannels.cpp` are replaced by a registry walk, so a
  new channel needs no presentation change at all.
- **Verify:** tests — the registry walk covers every registered channel, probe
  values match a direct `Sample` call. Manual — you read the overlays.

### E6 — Sun — done
- `Sun.Position` from georeference, day of year and hour. `Sun.Horizon`:
  horizon angle per azimuth over the elevation source. `Sun.SkyViewFactor`,
  `Sun.DirectInsolation`, `Sun.DiffuseInsolation`.
- Fidelity: `cos` incidence → + horizon shadowing → + sky view and diffuse
  split over a sampled sun path.
- **Verify:** tests — a flat world takes full insolation and unit sky view, a
  pit takes neither, horizon angles monotone with obstruction height, north and
  south flanks of an analytic ridge differ in the expected direction and by a
  plausible amount.

### E7 — Temperature — done
- `Climate.MeanTemperature` and per-slice `Climate.Temperature`: datum
  temperature, lapse rate, insolation anomaly, and cold-air pooling read from
  the basin depth the depression fill already computes.
- Habitat stops computing temperature and reads the channel.
- **Verify:** tests — lapse rate recovered from a tilted plane, sunny and shaded
  flanks of a ridge separate, a closed basin is colder than its rim under
  inversion and equal to it without, habitat contract tests updated.

### E8 — Wind — done
- `Wind.Synoptic` (seeded bearing and strength) → `Wind.Exposure` (Sx
  topographic exposure index) → `Wind.Field` (vector2: channeling along valley
  axes, speedup over crests, lee separation and recirculation) →
  `Wind.Roughness` fed back from canopy at E11.
- Fidelity: uniform → exposure index → + channeling and lee → + roughness and
  diurnal slope winds.
- **Verify:** tests — a crest is windier than the flat it rises from, a lee
  pocket slower and reversed, a valley aligned with the synoptic bearing
  accelerates and one across it does not, exposure invariant under rotating
  both terrain and wind together.

### E9 — Precipitation — done
- `Precip.Annual`, per-slice `Precip.Rate`, `Precip.SnowFraction`: base rate,
  orographic gain windward and loss leeward, altitude gradient, phase split by
  temperature, moisture depleted along the advection path.
- Habitat humidity reads the water balance rather than the windwardness term.
- **Verify:** tests — windward and lee flanks of an analytic ridge separate in
  the expected direction, total precipitation conserved against the inflow
  moisture budget, snow fraction goes to 0 and 1 either side of the freezing
  level.

### E10 — Time Slices & Snowpack — done
- Time-sliced channels (24 half-monthly) and the process kinds that come with
  them: static, per-slice, sequential, reducing.
- `Snow.Depth` as the first sequential process: accumulation from
  `Precip.SnowFraction`, redistribution by `Wind.Field` and `Wind.Exposure`,
  melt from `Sun.DirectInsolation` and `Climate.Temperature`, sublimation.
  Reduced to `Snow.CoverDuration` and `Snow.PeakDepth`.
- Time scrubber in the debug UI.
- **Verify:** tests — mass conserved over a year on a closed domain minus melt
  and sublimation, snow survives longer on shaded and lee ground, a warm world
  ends the year bare, sequential processes reproduce under re-solve.

### E11 — Vegetation & Feedback Loops — done
- `Vegetation.Cover`, `Vegetation.Height`, `Vegetation.Canopy` as processes
  reading temperature, water balance, soil, insolation and wind exposure.
- Close the loops: vegetation → soil stability → soil depth; canopy →
  roughness → wind → snow redistribution → vegetation. First real relaxation
  groups.
- **Verify:** tests — the SCC groups are exactly the expected ones, a fixed
  iteration count gives an identical hash every run, forested cells retain more
  soil and more snow than the same cells with vegetation forced `Off`.

### E12 — Habitat as a Read — done
- `FMassifHabitatField` stops computing climate and becomes a lookup over
  channels plus ground-type classification. Masks (§5.5) derive from the same
  channels, so mask and habitat cannot disagree.
- **Verify:** tests — habitat contract suite green against channels, habitat
  and mask classification agree cell for cell on the same input.

### E13 — Environment Gaps (done)
The systems the E-series left absent, and one bug it introduced. Each is a
registration against the framework or an extension of one process; none of them
needed the framework itself to change, which is the thing worth noticing.

- **Seasonal sun.** Insolation was one representative day and the snowpack read
  it for the whole year. Direct and diffuse now hold 24 half-monthly slices; the
  horizon sweep stays outside the slice loop. `Sun.AnnualInsolation` carries the
  growing-season mean for the things that want a climatology.
- **Seasonal precipitation and the water balance.** `Precip.Seasonal`,
  half-monthly `Precip.SnowFraction`, and `Climate.Evapotranspiration` — the only
  term that removes water from the massif.
- **Lithology.** `Geology.RockHardness`, `SoilYield`, `ScreeProduction`. Scree is
  produced by frost shattering rather than inferred from slope.
- **Infiltration, springs, seasonal flow.** Ground that soaks water up, water
  that comes back out at a permeability contact, and the spring melt pulse.
- **Slope winds and foehn.** `Wind.KatabaticStrength` reads both the catchment
  above a point and the hollow it cannot leave; `Wind.FoehnWarming` the descent
  from upwind.
- **Snow density, albedo and avalanches.** The albedo coupling closes the last of
  the three feedback loops. Avalanche release and path give the forest-free
  stripes nothing else produced.
- **Vegetation interception, class and disturbance.** The vegetation→water half
  of the loop, what is growing rather than only how much, and what knocks it down.
- **Performance.** Droplet erosion moved out of the relaxation group: it depends
  only on the filled surface and was being recomputed once per round. The bake
  over the 20 km world went 23.9 s → 15.1 s before any of the above was added.
- **Verify:** the whole suite, and a full-world solve reported for cost and
  resident size.

### E14 — GPU Paths (open)
The bake is about 24 s over the 20 km world across 40 channels, and two processes
are most of it: `Sun.Insolation` at 10.6 s and `Soil.Deposition` at 3.1 s.

- **Sun first.** Per-cell, read-only terrain, no accumulation across cells, so it
  is deterministic without any special handling. The horizon sweep is a ray march
  per azimuth.
- **Droplets second.** Parallel over droplets scattering into shared cells, which
  needs the integer atomics on a fixed-point encoding the CPU path already uses —
  the determinism contract was designed for this port before it existed.
- **The two ordered hydrology solves last, if ever.** A priority flood and an
  altitude-sorted routing pass would have to become fixed-count relaxations,
  which is what `GpuDepressionFillIterations` and `GpuFlowRoutingIterations` are
  reserved for. Together they are 2.5 s.
- **Not the rest.** Every other process already solves in under half a second.
- **Wind and precipitation become candidates once W2 lands.** Binning their
  response over 16 bearings multiplies their upwind sweeps, which is the same
  per-cell read-only shape that makes the sun portable. Cheap now, not after.
- **Verify:** CPU and GPU paths agree within a tolerance, headless still runs the
  CPU path, and the determinism hashes are unchanged.

Nothing here is blocked by the sim any more: the response table (W1) is pinned to
the CPU path, so what the GPU produces is never gameplay truth.

### E15 — Live Re-solving (open)
Most of the graph is already fast enough to re-solve while the game runs, and
what blocks it is not speed.

- Double-buffer the state so a re-solve can leave the game thread: consumers
  share the object the solve writes into, which is why it is synchronous now.
- Refresh the painted debug view when a re-solve lands, rather than on the next
  view change.
- Regenerate foliage and masks when the channels driving them change.

## Weather (W-series)

The bake holds the average year. Weather is this year, and it is sim state —
fixed tick, serialized, hashed (ARCHITECTURE §6.1). It factors into a global
synoptic state times a static terrain response, so nothing here is a runtime grid
solve.

W0 stands alone against the bake. Everything from W1 on is blocked on M2.

### W0 — Sun.Horizon and Time of Day (open)
The bake already sweeps the horizon per cell and throws it away, reducing it to
a sky view factor. Keeping it is what lets anything ask whether the sun is up
*here*, now — the question `Sun.DirectInsolation` cannot answer, because it is a
half-monthly *daily total*. Morning refreeze, afternoon shade on a north slope
and the spring corn cycle are all downstream of it.

- `Sun.Horizon`: horizon elevation per azimuth, 16 azimuths on the slice axis,
  `Uint16`, 32 m cells. No new sweep — the existing one stops being discarded.
- A sun-position function shared by the bake and the runtime clock.
- **Verify:** tests — a cone shades its own polar flank and a tilted plane never
  shades itself; the swept angles bracket the sky view factor the same cell
  already reports; the channel is unchanged by fidelity where the sweep is.

### W1 — The Response Table (blocked on M2)
`FMassifSimEnvironmentTable` in `MassifCore` (ARCHITECTURE §5.6.6): the pinned,
CPU-only, content-hashed snapshot the sim reads instead of bake channels.

- Built by `MassifWorldGen` at world load, injected into the sim subsystem by
  presentation, so no `MassifSim` → `MassifWorldGen` edge appears.
- Solved at one fidelity named in code, never the machine's setting.
- **Verify:** tests — the table is byte-identical across bake fidelity settings;
  its hash changes when the elevation model does; `MassifSim.Build.cs` still does
  not reference `MassifWorldGen`.

### W2 — Direction-Binned Terrain Response (blocked on M2)
Unit-forcing responses per bearing, 16 bearings, as a separate process outside
the wind/vegetation relaxation group so the iteration count does not multiply it.

- `Wind.Response.<bearing>`: exposure, channeling weight, lee separation
  strength, foehn descent — scalars only. The vector is rebuilt at runtime from
  the actual flow, because the valley axis carries a sign that flips across the
  perpendicular and blending finished vectors would collapse and reverse a valley
  wind.
- `Precip.LiftResponse.<bearing>`: the unit-speed lift integral, so the runtime
  can exponentiate the shadow at the actual wind speed. A baked finished field is
  correct at one speed only, and the shadow is exponential in speed.
- Bin nothing direction-free: katabatic strength and snow fraction are not.
- **Verify:** tests — response × speed reproduces the direct solve at that speed
  to tolerance, for several speeds and bearings; bearing interpolation is
  continuous across a valley axis; added bake time is measured and recorded.

### W3 — Weather State and Generator (blocked on M2)
- `FMassifWeatherState` in `FMassifSimState`: air-mass temperature at datum,
  freezing level, wind bearing and speed aloft, cloud cover and base, precip
  intensity, humidity, front bearing and position, gust factor — plus the
  integrated ones a formula cannot express: inversion strength, season snowfall
  anomaly.
- Seeded Markov generator over weather types, conditioned on the bake's 24
  half-monthly slices, on the `Sim.Weather` channel.
- `MassifGameMinutesPerSimTick`, and weather advancing every fixed *N* ticks.
- Point sampling: temperature, wind, precipitation and phase at a position.
- Do not build runtime temperature on `Climate.SeasonalTemperature` — it already
  has climatological snow-albedo cooling inside it, which would double-count
  against a runtime snowpack that disagrees with it.
- **Verify:** tests — same seed gives an identical weather hash after N ticks;
  the generator's long-run means match the bake climatology within tolerance;
  cadence is invariant to game speed; wet-bulb derivation for snowmaking.

### W4 — Weather into Snow (folds into M7)
- `FSnowGrid` driven by W3's point functions, `ParallelFor` over disjoint
  resident tiles inside the tick.
- Non-resident ground reads baked depth × season anomaly, so the residency
  boundary stays invisible, with the 64 m → 4 m reconciliation explicit.
- Save format bump (`FMassifCustomVersion`): **dev reset, not migration**.
- **Verify:** tests — snow hash stable across runs; a grooming command inside the
  same tick as a snow pass survives it; resident and non-resident depth agree
  across the boundary; bake and sim snowpack agree on a season within tolerance.

### M5f — Geological Refinement
Erosion over the imported surface: smooth scanner artefacts, cut detail below
the 2 m source resolution, age the ground. Runs once at import over a finite
grid and emits another `.massifdem` — a preprocessing step on measured data,
never a solve in the query path (ARCHITECTURE §5.1.1).

Diffusion-generated terrain (Infinite Diffusion) enters the same pipeline at
the same point, through the same format.

### M6 — Masks, Textured Terrain & PCG Forests
- Semantic masks (river, scree, forest, rock face, buildable) from the
  environment bake channels (M5g, M5h) plus slope and altitude; debug
  visualization per mask, through the channel views M5g builds.
- Terrain material with real PBR texture sets (Polyhaven, CC0: rock,
  snow, grass, scree) — triplanar, slope/altitude/mask blended.
- PCG forests from the forest mask with realistic conifer models
  (Polyhaven/CC0 or AI-generated), density by altitude to treeline,
  deterministic per seed.
- **Verify:** manual — masks match geology, forests and textures read
  naturally; headless suite still green.

### M7 — Snow: Sim Grid + Visual Prototype
- `FSnowGrid` in sim: snowfall and altitude/temperature-driven melt on
  sim tick, deterministic.
- Snow material on terrain: coverage/depth from GPU copy of sim grid;
  deformation render target for tracks (presentation-only); Nanite
  displacement prototype.
- Debug: snow depth overlay, manual "dump snow here" debug command.
- **Verify:** tests — snow accumulation/melt determinism, sim grid hash
  stable across runs; manual — snow visibly accumulates, a dragged debug
  brush leaves tracks that do not affect sim hash.

### M8 — Building Tools: Lift & Slope Placement
- Placement mode UI shell (toolbar, ghost preview, validity coloring).
- Lift placement: pick two station points → towers auto-spaced along the
  line; validation (length, slope under stations, buildable mask) in sim
  code; `PlaceLift` command.
- Slope drawing: spline control points snapped to terrain, width, raster
  into snow-grid cells; validation (angle range, connectivity to lift
  nodes, buildable mask); `PlaceSlope` command.
- Resort graph assembled from placed objects.
- **Verify:** tests — validation accepts/rejects known cases, command →
  expected graph delta, graph connectivity query; manual — place a lift
  and a connected slope on the generated terrain.

### M9 — Lift Simulation
- Chairs advance along the lift line each sim tick; guest queue with
  `LiftQueueCapacity`; board/alight at stations.
- Presentation: station/tower/chair placeholder meshes from sim data,
  chairs interpolate between sim ticks.
- **Verify:** tests — chair spacing/period math, queue FIFO under load,
  boarding conservation (nobody duplicated/lost), determinism hash;
  manual — chairs visibly cycle.

### M10 — Guest Agents
- `FGuestAgent` array in sim: spawn at resort entry, state machine
  Arrive → QueueLift → RideLift → SkiSlope → loop → Leave; ski speed from
  slope angle + snow condition + seeded skill.
- Presentation: pooled placeholder guest meshes, small animation cycle
  set (ski, turn, sit, walk), interpolated from sim state.
- Guests' ski runs write the visual deformation RT (tracks).
- **Verify:** tests — full agent loop headless over N ticks (guests cycle
  and never wedge in a state), determinism hash with 200 agents;
  manual — guests queue, ride up, ski down, repeat.

### M11 — Playable Loop & Save/Load
- New-game flow: seed entry → generate → play. Minimal HUD (sim speed,
  guest count, seed display).
- Save/load: seed + worldgen result + full sim state; versioned per
  ARCHITECTURE §9.
- Perf pass: acceptable frame rate with full massif + 200 guests
  (target: 60 fps on dev machine, recorded in the milestone commit).
- **Verify:** tests — save/load round-trip state-hash equality; manual —
  full loop from seed entry to watching guests, quit, load, identical
  resort continues running.

## Explicitly Out of Scope (Phase 1)

Non-winter seasons; hikers/climbers/bikers/parachuters; economy beyond
placeholder counters; buildings beyond lift stations; snow cannons and
grooming vehicles as player systems (sim hooks exist, tools do not);
storm/treefall simulation (mask design anticipates it); multiplayer;
non-placeholder art; audio; localization.

## Later Phases (parking lot, not planned)

Economy & guest needs depth · snow cannons + grooming as gameplay ·
realistic buildings (restaurants, hotels, rentals) · seasons & season
transition (winter/spring/summer/autumn mountain looks) · summer/all-year
activities (hiking, climbing, mountain biking, parachuting) · snowboarding
as distinct guest type · storm/treefall forest simulation (live and as
worldgen pre-aging) · ramps & fun parks for biking/skiing · multiplayer
evaluation.
