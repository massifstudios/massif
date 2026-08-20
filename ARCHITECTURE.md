# Massif — Architecture

Locked technical decisions for the project. Changes to this file are
deliberate decisions, not side effects of implementation work. If an
implementation need contradicts this document, stop and resolve the
contradiction here first.

## 1. Engine & Project

- **Engine:** Unreal Engine **5.8.1** (Linux binary build at
  `/mnt/storage/UnrealEngine/5.8.1`). Engine upgrades are deliberate,
  standalone commits.
- **Language:** C++ for all logic. Blueprints only as thin glue: wiring
  input, holding data assets, referencing meshes/materials. Anything that
  reads or writes sim state is C++.
- **World Partition** for the massif-scale map. One persistent level;
  generated content registers into World Partition streaming.
- **Single-player, offline.** No networking code. But all sim code follows
  the multiplayer-friendly rules below so replication can be added later.

## 2. Module Layout

Four runtime modules with one-way dependencies (arrows = "may depend on"):

```
MassifPresentation ──► MassifSim ──► MassifCore
        │                  ▲
        └──► MassifWorldGen ┘
```

| Module | Contents | Hard rules |
|---|---|---|
| `MassifCore` | Seeded RNG channels, fixed-tick driver, hashing, shared math/types. | No engine gameplay classes beyond basic types. |
| `MassifSim` | All gameplay state and rules: resort graph, lifts, slopes, snow grid, guest agents, economy, sim commands, save data structs. | No rendering, no UI, no actor spawning, no `GetWorld()`-dependent logic paths in state updates. Must run headless (`-nullrhi`). |
| `MassifWorldGen` | The terrain field (§5), hydrology graph, mask derivation, PCG input preparation. | Deterministic per seed. The terrain field is a pure function of world position with no global state, so any point at any resolution is evaluable without evaluating any other. |
| `MassifPresentation` (primary game module) | Terrain mesh building, snow rendering/deformation visuals, NPC visuals, camera, input, UI, PCG spawning. | Reads sim state; never writes it except by enqueuing sim commands. |

Each module owns a `Tests/` subfolder with UE Automation tests. `MassifSim`
and `MassifCore` tests must pass headless.

## 3. Sim / Presentation Separation

- The simulation lives in `UMassifSimSubsystem` (a `UGameInstanceSubsystem`)
  holding a single root state object of plain `USTRUCT` data
  (`FMassifSimState`). No sim state in actors, components, widgets, or
  materials.
- **Fixed sim tick:** the sim advances in fixed steps (10 Hz). The
  presentation frame tick feeds an accumulator that calls
  `AdvanceSimTick()` zero or more times per frame. Game speed (pause / 1x /
  4x) changes how many sim ticks run per real second — never the tick
  delta itself.
- **Command pattern:** every player-initiated mutation (place lift, draw
  slope, set cannon on, groom) is an `FSimCommand` struct enqueued by the
  presentation layer and applied at the start of a sim tick. Commands are
  plain serializable data. This is the future replication boundary:
  commands go client→server, state snapshots server→client.
- Presentation reads sim state freely (const access) and interpolates
  visuals between sim ticks. Nothing gameplay-relevant may originate in
  presentation.

## 4. Determinism

- One **master seed** (user-visible, shareable, entered at new game).
  Subsystem seeds derive as `HashCombine(MasterSeed, ChannelName)` —
  channels like `WorldGen.BaseNoise`, `WorldGen.Erosion`, `Sim.Guests`,
  `Sim.Weather`. Adding a channel never shifts existing ones.
- Sim code uses `FRandomStream` instances owned by the sim state
  (serialized with it). `FMath::Rand`, wall-clock time, frame delta, and
  actor-iteration order are forbidden in sim code. Iteration over sim
  collections uses stable ordering (arrays / sorted keys, never
  `TMap`/`TSet` iteration order).
- **Determinism test contract:** hash of `FMassifSimState` after N ticks
  from a given seed is identical across runs. An automation test enforces
  this for every sim system as it lands.
- **Terrain and determinism.** The terrain field (§5) is CPU-only and reads
  a fixed imported elevation model, so it is deterministic by construction —
  the same file gives the same ground on every machine, with none of the
  float-reproducibility exposure a computed surface carries. No
  gameplay-relevant value comes from a GPU pass. Compute shaders may mirror
  the field for rendering, but their results are never read back into sim.
- **The environment bake (§5.6) runs on the GPU and is not sim state.** Its
  channels — drainage, standing water, wetness, soil depth, insolation — feed
  worldgen and presentation: what to plant, what the ground is made of, how the
  terrain is shaded. Nothing in `MassifSim` reads them. The bake carries a CPU
  reference implementation that is the algorithm's definition and the contract
  the tests hold; the GPU path exists to make it fast. When a mask that sim
  does read (buildable, forest) later needs a bake channel, it comes from the
  CPU path, so the rule above stands unweakened.
- **Sim samples the terrain field only at fixed, named spacings.** The
  field is LOD-dependent by design, so sampling it at view LOD would make
  sim state depend on where the camera is. Sim therefore uses named
  constants — `SimContactSpacing` (0.5 m) for agent-ground contact and
  placement validation, `SimSurveySpacing` (4 m) for wide-area scans — and
  never a spacing derived from camera distance. Presentation samples freely
  at whatever LOD it needs.
- The whole world is playable, and the elevation model covers all of it.
- **Consequence, accepted:** the elevation model is a build asset, not save
  data. A save records which model it was built on (§9); loading it against a
  different or missing one is a hard error, not a silent regeneration onto
  different ground.

## 5. World Generation

**Terrain macro shape is imported, not generated.** Procedural macro relief was
tried three ways — GPU erosion (M3), a fluvial stream-power bake (M4b), and the
analytic erosion filter alone (M5d) — and none produced valleys that read as a
real massif or that a ski resort could be built in. Real mountains are the
output of processes no per-point function reproduces, and public elevation data
for the Alps is free, accurate and already exactly what the game wants.

The world is therefore a real 20 × 20 km window of Tirol, imported once from
the Land Tirol airborne-laserscan terrain model (CC BY 4.0) by
`Tools/fetch_tirol_dem.py`, and refined from there:

```
1. Imported elevation model — macro and meso shape. Measured ground.
2. Geological refinement — erosion passes over the imported surface, to
   smooth scanner artefacts and cut detail below the source resolution.
   Open; not yet built.
3. Sub-meter — material displacement, normal maps, scattered rock, scree
   and vegetation, snow deformation. Not heightfield.
```

Diffusion-generated terrain (Infinite Diffusion) is the other intended source
for the same pipeline: it produces the same kind of artefact — a heightmap with
real macro structure — and enters at layer 1 through the same `.massifdem`
format, so nothing downstream distinguishes a generated massif from a measured
one. Seeds no longer select a world shape; they select scattered content.

The query API is unchanged, and stays the single way anything reads the ground:

```
Height(FVector2d WorldMeters, double SampleSpacingMeters) → double meters
```

**Positions are double precision, not float.** `float32` resolves about
1 mm at 10 km from the origin, which would spend a real share of the
centimeter detail budget on coordinate error before any terrain is
evaluated — and doubles keep the field API stable if the extent grows
again. Seeds and noise lattices stay integer; only positions widen.

`SampleSpacingMeters` selects which mip of the elevation model answers, and
the two levels around it are blended, so a coarse sample and a fine sample of
the same point agree instead of stepping where clipmap rings meet. Distant
tiles read a filtered surface rather than aliasing the finest one.

The whole world is playable. Nothing is scoped to a sub-region: what is
scoped instead is *residency*, since anything needing neighbours or
persistence lives in tiles that come and go around player activity
(§5.5).

### 5.1 The elevation model

`FMassifHeightmap` holds the imported surface. One `.massifdem` file per
world: a 192-byte header, then uint16 samples quantised over the window's own
altitude band, row-major, row 0 north. 20 km at 2 m is 10 000² samples,
200 MB, and a mip chain built at load takes that to about 270 MB resident —
small enough to hold outright, so no streaming or memory mapping is needed at
this resolution.

The header carries the source georeference (EPSG code, easting/northing of the
window's north-west corner) so imports stay traceable and later overlays —
orthophotos, cadastral data, real piste and lift lines — can be registered
against the same ground.

Sampling is Catmull-Rom within a level and linear between levels. Both are
pure functions of position, so the chunk-independence property the streaming
scheme rests on is preserved by construction: no cache, no order dependence,
no per-tile generation of terrain shape.

Quantisation: uint16 over a ~2600 m band is 4 cm, well under the source
model's own vertical accuracy, and it halves the resident footprint against
float32.

Grid erosion solves (the M3 GPU passes, the M4b–M5c fluvial bake) and the
procedural stack that replaced them (uplift field, fractal base relief, the
analytic erosion filter) are all deleted — git carries them.

Below the source resolution, appearance comes from material displacement and
scattered meshes. Centimeter *geometry* is not a target: a 1 cm mesh over
50 × 50 m is 50 M triangles, and for a ski game the skiable surface is snow,
which drapes over everything below roughly half a meter anyway.

### 5.1.1 Geological refinement (open)

Erosion over the imported surface — smoothing scanner artefacts, cutting
detail below the source resolution, aging the ground. Unlike the deleted
worldgen solves this runs **once at import**, over a finite imported grid, and
its output is another `.massifdem`. It is a preprocessing step on measured
data, not part of the terrain definition, so it reintroduces no world-wide
solve into a query path that must stay per-point.

### 5.2 Hydrology

Drainage, standing water and wetness come from the environment bake (§5.6):
one world-wide solve over the finite imported grid, run once at world gen.

This is a world-wide solve, and the earlier ones (M3, M4b) were deleted for
being exactly that — the distinction is which side of the query API they sit
on. Those computed the *terrain surface*, so every point of `Height` depended
on a global grid, and the field could no longer answer a point at any
resolution without evaluating the world. The bake computes quantities *derived
from* a surface that is already fixed and measured. `Height(Position,
Spacing)` is untouched by it and stays a pure per-point function of the
elevation model; the bake is a second field beside it, not a layer under it.

A world-wide solve is affordable here for the same reason it was not before:
the world is finite, imported, and 20 km across, so the whole drainage problem
is 2500² cells.

### 5.3 Terrain representation

Runtime landscape creation is not properly supported by `ALandscape`, so
terrain is **generated meshes sampled from the field**.

- **Clipmap rings**, not uniform chunks. Fixed vertex count per ring,
  spacing doubling outward: ~10 cm at the camera out to the world edge in
  roughly 20 rings, ~1.5 M vertices total. Uniform tiles cannot work —
  near the camera a 100 × 100 m tile at 1 cm would be 100 M samples.
- **Tiles are sampled, not generated and stitched.** Independently
  generated chunks always seam. Because every tile samples one continuous
  function, neighbours agree exactly at shared edges by construction and
  there is no stitching step.
- The only seam risk is between LOD levels, handled by vertex morphing
  plus the octave fade described above.
- Player-built actors register with World Partition. Terrain rings are
  camera-relative and not streamed through World Partition.
- Sim never queries the mesh. Height and slope come from the field at
  fixed spacings (§4); masks and snow come from resident tiles (§5.4).

### 5.4 Resident tiles

The field answers point queries, but masks, snow and scattered content
need neighbours or need to persist. Those live in **resident tiles**:
512 m square, generated on demand around player activity and the camera,
evicted when far, keyed by tile coordinate so generation is deterministic
and order-independent.

```
TileSeed = HashCombine(MasterSeed, TileX, TileY, LayerName)
```

Three kinds of content, with different seam rules. Getting these
backwards is the classic way to reintroduce the seams the field exists to
avoid:

| kind | examples | seeding | seams |
|---|---|---|---|
| **Continuous fields** | height, slope | **never per-tile.** Always the global field | none by construction |
| **Derived grids** | flow accumulation, masks, snow | per tile, computed *from* the field with an apron of neighbouring cells that is discarded after | none, provided the apron is at least the widest kernel used |
| **Discrete scatter** | trees, rocks, scree | per-tile seed, each tile also sampling its 8 neighbours so objects near a border appear in both | none; a seam is not a concept for point sets |

Terrain shape must never be generated per tile from a tile seed. Adjacent
tiles would not agree at their shared edge, which is exactly the stitching
problem the field is there to remove.

**Flow accumulation** is the one genuinely global derived quantity — water
arrives from far outside the tile, so no apron is wide enough. It is therefore
not a per-tile derived grid at all: it comes from the environment bake (§5.6),
world-wide and coarser than a tile, and resident tiles read it rather than
computing it.

Only tiles the player has modified are saved (§9). Everything else is
regenerated from the seed on demand.

### 5.5 Masks and PCG

Semantic masks (forest, river/stream, scree, rock face, buildable) are
derived per resident tile from flow accumulation, deposition, slope,
curvature and altitude bands.

PCG runs against the generated surface using the semantic masks as spatial
data. PCG output is presentation + collision only; anything
gameplay-relevant derived from vegetation (e.g. "forest blocks slope
placement") comes from the masks, not from spawned instances — this keeps
the door open for the later storm/treefall simulation to edit masks and
regenerate. The same simulation can later be run in accelerated form at
worldgen time to pre-age forests (fallen trees, clearings, wind-shaped
tree lines) instead of purely mask-driven placement.

Distant terrain that has no resident tiles gets vegetation and rock driven
directly by the field (altitude, slope, aspect). It becomes mask-driven as
soon as tiles become resident, so the two must agree closely enough that
the transition is not visible.

### 5.6 Environment bake

Where a plant grows is decided by water, light and what the ground is made of,
and none of the three can be answered from a single point of the surface: water
arrives from the whole catchment above, shade is cast by ridges kilometres
away, and soil is where sediment came to rest. `FMassifHabitatField` first
approximated all three from local slope and altitude, and it reads as
approximation — trees stop at a contour line rather than at a treeline, and
valley floors are no wetter than the shoulders above them.

The **environment bake** replaces the approximations with simulation over the
whole world, run once at world gen against the imported grid. Channels:

| channel | from | drives |
|---|---|---|
| `StandingWaterDepth` | depression fill: filled surface minus measured | tarns, ponds, wetland |
| `FlowAccumulation` | multiple-flow-direction routing over the filled surface | streams, rivers, catchment size |
| `Wetness` | topographic wetness index, blended with standing water and climate | humidity, species density |
| `SoilDepthMeters` | droplet erosion/deposition against a sediment capacity | rock vs scree vs earth |
| `SkyViewFactor`, `DirectInsolation` | horizon sweep and a day's sun path (later milestone) | temperature, treeline, snow retention |

Resolution is 8 m — 2500² cells over the 20 km world, about 50 MB resident as
quantised uint16. Finer would resolve stream channels better, but habitat is
read at species scale and the DEM's own detail is already available through the
terrain field for anything that needs it.

**Determinism.** Iteration counts are fixed, never driven by a convergence test
against a readback — a solve that stops when it has converged stops at a
different iteration on a different machine. Droplets start on a jittered
lattice from a seeded stream, and scatter their sediment through **integer
atomics on a fixed-point encoding**: float atomic adds commit in whatever order
the hardware schedules, so a float accumulator would make the same bake differ
run to run on one machine, never mind across two.

**Two implementations, one interface.** The CPU path is the definition of each
algorithm and what the automation suite runs; the GPU compute path is the same
algorithm arranged for the hardware, and is what actually runs at world gen.
Headless (`-nullrhi`) falls back to the CPU path rather than skipping, so the
bake is covered by tests on every machine. The two are held to agreeing within
a tolerance, which is what catches a GPU port that has drifted from its
reference.

The bake is not saved. It is a pure function of the elevation model and the
master seed, so it is rebuilt at load like the mip chain (§9).

### 5.7 Debug channel views

Any bake channel can be painted onto the terrain in place of its material,
selected by the number keys, with a legend naming the channel and its range in
the top-right corner. The channel is uploaded as one world-sized texture and
sampled by world XY in the terrain material, so nothing about the mesh, the
quadtree or the field changes when a view is switched on.

This is presentation-only debug, not a game mode: it reads the bake and draws
it. It exists because every one of these channels is a simulation whose output
is otherwise only visible as a second-order effect on where trees ended up.

## 6. Snow System

Two layers with a strict boundary:

1. **Sim snow grid (gameplay truth).** `FSnowGrid` in `MassifSim`: coarse
   grid (≈4 m/cell) held as resident tiles (§5.4) around resorts and
   player activity, not as one world-sized array, of
   `{SnowDepth, Compaction}` per cell. Untouched terrain derives its snow
   from altitude and aspect analytically, and only becomes a resident tile
   once something writes to it. Updated on sim
   tick by weather (snowfall, melt by altitude/aspect/temperature), snow
   cannons, and grooming — all deterministic. Slope quality, cannon
   coverage, and guest satisfaction read this grid. Serialized in saves.
2. **Visual deformation (presentation only).** High-frequency render-target
   deformation for ski tracks and displacement detail, applied through the
   snow material via **Nanite displacement** (decided; if cost or
   stability disappoints in practice, fall back to material-only
   parallax/normal). Written by NPC and grooming visuals.
   **Never read by the sim.** Grooming resets the deformation RT locally
   and applies its gameplay effect through a sim command — the two layers
   stay causally separate.

The snow material blends depth (from a GPU copy of the sim grid, updated
when cells change) with the deformation RT for close-range detail.

## 7. Resort Network & Buildables

- The resort is a **graph**: nodes (lift stations, slope tops/bottoms,
  building entrances, spawn points) and edges (lift lines, slope segments,
  walkable paths). Guests plan routes on this graph — no navmesh planning
  for the core ski loop.
- **Lift:** sim object with two station nodes, a tower list, chair
  positions advanced by sim tick, queue with `LiftQueueCapacity`, boarding
  logic. Presentation spawns station/tower/chair meshes from sim data.
- **Slope:** spline over the terrain (control points snapped to the sim
  height grid) with a width, rasterized into the affected snow-grid cells.
  Placement validation (slope angle range, buildable mask, connectivity)
  is a sim-side check.
- Placement flow: presentation runs a preview (ghost meshes, validity
  coloring) using the same sim-side validation functions, then enqueues a
  `PlaceLift`/`PlaceSlope` command.

## 8. Guests (NPC Agents)

- Custom lightweight agent model in `MassifSim`: `TArray<FGuestAgent>`
  (struct-of-data, no actors). Agent state: id, position (2D on the sim
  height grid + cached height), current graph edge + progress, activity
  state machine (Arrive → QueueLift → RideLift → SkiSlope → repeat →
  Leave), needs/satisfaction scalars.
- Agent update is a deterministic fixed-tick pass over the array. Skiing
  follows the slope spline with speed from slope angle + snow condition +
  per-agent skill (seeded).
- Presentation representation: pooled skeletal or instanced meshes with a
  small blended cycle set (ski stance, turn L/R, sit-on-lift, walk),
  positioned by interpolating sim state. Management-camera fidelity.
- Custom agents, not MassEntity (decided): simpler, deterministic,
  trivially serializable, and optimization stays fully in our hands.

## 9. Save Games

- Save = master seed + elevation model name and content hash + engine version
  tag + full `FMassifSimState` + modified resident tiles (§5.4, compressed) +
  save format version.
- No heightfield is saved. The elevation model is a build asset that ships
  with the game; the save names it and records its hash, so a mismatch is
  refused rather than silently loading a resort onto different ground. Save
  size scales with what the player has built and touched, not with world area.
- Custom version GUID (`FMassifCustomVersion`) from the first save-touching
  commit; every format change bumps it. During development, incompatible
  saves are invalidated (dev reset) unless migration is explicitly
  requested.

## 10. Testing Strategy

- UE Automation tests per module in `Tests/` subfolders.
- Standing test categories: determinism hashes (worldgen CPU stages, sim
  state after N ticks), sim unit tests (lift boarding, agent state
  machine, snow accumulation math), command round-trip (apply command →
  expected state delta), save/load round-trip (state hash equality).
- Everything in `MassifSim`/`MassifCore` runs under `-nullrhi`. The terrain
  field is CPU-only and fully testable headless.
- **Terrain field contract tests**, enforced as it lands. They run against a
  heightmap built in memory, never against a fetched elevation model, so the
  suite passes on a bare checkout:
  - *Chunk independence:* a tile sampled on its own is bit-identical to
    the same tile sampled as part of a larger block. This is the property
    the whole streaming scheme rests on.
  - *LOD agreement:* height is continuous in sample spacing, with no step at
    a mip boundary, and a coarse sampling stays within the relief the levels
    it drops could carry.
  - *Sample fidelity:* at a sample centre, at the finest spacing, a query
    returns the imported altitude to within the quantisation step. This is
    what pins the field to the source data rather than to a resampling of it.
  - *File format:* the `.massifdem` byte layout is a contract with the Python
    import tool, so a hand-built file is parsed and malformed ones rejected.

## 11. Assets & Naming

- Placeholders now, AI-generated (Meshy/Tripo) later. Standard prefixes
  (`SM_`, `SK_`, `M_`, `MI_`, `T_`, `ABP_`), stable descriptive names
  (`SM_LiftChair`, `SM_LiftTowerStandard`, `M_SnowSurface`) so replacements
  drop in without reference fixups.
- Content referenced from C++ via soft paths collected in a single
  `UMassifAssetCatalog` data asset — one place to retarget when assets are
  swapped.
- Git LFS for binary content. `Binaries/`, `Intermediate/`, `Saved/`,
  `DerivedDataCache/` never committed.

## 12. World Scale (decided)

- **Extent:** 20 × 20 km, finite, **playable throughout**. Finite because the
  world is an imported window of real ground. Kept small — buildable valleys
  are the game, and 400 km² is already larger than the largest real ski
  domains.
- **Source resolution:** 2 m. The Tirol model is published at 50 cm, but
  20 km at 50 cm is 3.2 GB of samples, and detail under 2 m is what the
  refinement and material layers put back anyway. The import tool takes the
  resolution as an argument, so this is a data decision, not a code one.
- **Nothing terrain-shaped is saved.** The elevation model is a build asset
  (§9). What is scoped is residency, not area (§5.4).
- **Coordinates:** double precision throughout the field API. See §5.
- **Altitude range:** set by the imported window. The current one (Ötztal,
  Sölden) runs 1168–3757 m.
- **Sim sampling:** fixed spacings only — 0.5 m for agent contact and
  placement validation, 4 m for wide-area scans. Never view-derived.
- **Resident tiles:** 512 m square, 4 m cells (128² per tile) for masks and
  snow. Generated on demand, evicted when far, saved only if modified.
- **Environment bake:** 8 m cells, 2500² over the world, solved once at world
  gen and never saved (§5.6).
- **Presentation detail:** clipmap rings from ~10 cm at the camera,
  doubling outward to the world edge, ~20 rings and ~1.5 M vertices.
  Below the finest ring, detail is material displacement and scattered
  meshes, not geometry. Centimeter *geometry* is not a target: 1 cm over
  50 × 50 m is 50 M triangles, and snow drapes over everything under
  roughly half a meter regardless.
- **World selection:** `massif.Dem` / `-MassifDem=` names a model under
  `Data/Terrain`. Fetched models are gitignored and re-fetchable, so no
  multi-hundred-megabyte binary enters the repository.
- The sim-facing query API (`SampleHeightMeters`, `SampleSlopeDegrees`,
  `SampleNormal`) is unchanged by the switch to imported terrain, so §6–§8
  are unaffected.
