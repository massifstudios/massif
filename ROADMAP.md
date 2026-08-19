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

### M2 — Deterministic Sim Core
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

### M5f — Geological Refinement (next)
Erosion over the imported surface: smooth scanner artefacts, cut detail below
the 2 m source resolution, age the ground. Runs once at import over a finite
grid and emits another `.massifdem` — a preprocessing step on measured data,
never a solve in the query path (ARCHITECTURE §5.1.1).

Diffusion-generated terrain (Infinite Diffusion) enters the same pipeline at
the same point, through the same format.

### M6 — Masks, Textured Terrain & PCG Forests
- Derived masks from eroded terrain: flow accumulation, deposition,
  slope → semantic masks (river, scree, forest, rock face, buildable);
  debug visualization mode per mask.
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
