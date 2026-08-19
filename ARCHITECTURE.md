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
- **Terrain and determinism.** The terrain field (§5) is CPU-only and
  seed-driven. No gameplay-relevant value comes from a GPU pass. Compute
  shaders may mirror the field for rendering, but their results are never
  read back into sim.
- **Sim samples the terrain field only at fixed, named spacings.** The
  field is LOD-dependent by design, so sampling it at view LOD would make
  sim state depend on where the camera is. Sim therefore uses named
  constants — `SimContactSpacing` (0.5 m) for agent-ground contact and
  placement validation, `SimSurveySpacing` (4 m) for wide-area scans — and
  never a spacing derived from camera distance. Presentation samples freely
  at whatever LOD it needs.
- The whole world is playable, so nothing terrain-shaped is baked: 150 km
  at 4 m would be 1.4 × 10^9 cells. The field is cheap and deterministic
  enough to query directly.
- **Consequence, accepted:** terrain is no longer frozen into the save, so
  a save carried to a machine whose float math differs would sit on
  near-identical rather than identical ground. Single-player and offline,
  so this only matters for shared saves, which is not a feature. Shared
  *seeds* reproduce the same world on the same hardware class and a
  near-identical world elsewhere. A bit-exact cross-platform
  implementation stays out of scope.

## 5. World Generation

Seed-driven, no hardcoded maps. Terrain is a **pure function of world
position**, never a stored grid:

```
Height(FVector2d WorldMeters, double SampleSpacingMeters) → double meters
```

**Positions are double precision, not float.** `float32` resolves about
4 mm at 75 km from the origin and 8 mm at 150 km, which would spend most
of the centimeter detail budget on coordinate error before any terrain is
evaluated. Seeds and noise lattices stay integer; only positions widen.

`SampleSpacingMeters` selects how far down the octave stack to evaluate:
detail finer than roughly twice the spacing is skipped, and the finest
octave included is faded in smoothly rather than switched on, so a coarse
sample and a fine sample of the same point agree.

At the world extent in §12 no full-resolution grid can exist — 150 km at
1 cm is 10^15 samples — so on-demand evaluation is not an optimisation
here, it is the only representation that works.

The whole world is playable. Nothing is scoped to a sub-region: what is
scoped instead is *residency*, since anything needing neighbours or
persistence lives in tiles that come and go around player activity
(§5.5).

### 5.1 Layers

Each is a pure function of position and the master seed:

```
MasterSeed
  └► 0. Uplift field — where ranges and basins sit. Warped ridged noise
        at 20–80 km. Sets the continental shape.
     1. Hydrology graph — outlets on the world border, trunk rivers
        inland, recursively subdivided into tributaries (§5.2).
        A graph, not a grid.
     2. Macro surface — valley floors and cross-sections from the graph,
        crest height from the uplift field. Analytic per point.
     3. Erosion filter — analytic gully field: cosine stripes laid across
        the local flow direction, each octave dragging the next finer one
        sideways so gullies branch. O(1) per point.
     4. Sub-meter — material displacement, normal maps, scattered rock and
        scree meshes, snow deformation. Not heightfield.
```

Layer 3 is what makes the scheme work: it needs no neighbours and no
iteration, so any point at any resolution is evaluable in isolation.
**Grid-based erosion cannot meet that contract.** The hydraulic pipe model
and thermal erosion from M3 need the whole grid and its neighbours, so
they are no longer part of the terrain definition. The code stays in tree
and can be run as an offline pass over a resident tile, but it is off by
default and nothing depends on it.

Layer 4 is where centimeter-scale *appearance* comes from. Centimeter
geometry is not a target: a 1 cm mesh over 50 × 50 m is 50 M triangles,
and for a ski game the skiable surface is snow, which drapes over
everything below roughly half a meter anyway.

### 5.2 Hydrology graph

The graph is the terrain's skeleton and stays **global** — a valley
floor's elevation depends on the whole path to its outlet, so it cannot be
made chunk-local without giving up consistency. It is affordable because
it is a graph, not a grid.

- Built hierarchically: outlets on the world border, then trunks, then
  tributaries, each level roughly halving segment length down to ~200 m
  headwater streams.
- Each node's children derive from a hash of that node's stable id, so
  refinement is deterministic and order-independent.
- Levels down to ~1.25 km spacing are built eagerly for the whole world
  (~15 k nodes, milliseconds). Finer levels are refined on demand around
  the camera and wherever the player has built.
- Segments are held in a uniform spatial index. Point queries touch only
  nearby segments, which is what lets the macro surface be evaluated
  analytically at any point instead of via a precomputed lattice.

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
arrives from far outside the tile. The hydrology graph already carries
that: it supplies discharge entering the tile, and the tile only has to
resolve the last few hundred meters locally.

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

- Save = master seed + engine version tag + full `FMassifSimState` +
  modified resident tiles (§5.4, compressed) + save format version.
- No heightfield is saved. Terrain is a pure function of the seed and is
  regenerated on demand, so save size scales with what the player has
  built and touched, not with world area.
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
- **Terrain field contract tests**, enforced as it lands:
  - *Chunk independence:* a tile sampled on its own is bit-identical to
    the same tile sampled as part of a larger block. This is the property
    the whole streaming scheme rests on.
  - *LOD agreement:* sampling a point at coarse and fine spacing differs
    by less than the finest skipped octave's amplitude, with no step at a
    ring boundary.
  - *Surface sanity:* over a sampled region, no step between adjacent
    samples exceeds a slope ceiling, and local minima stay below a
    threshold — these are what catch tearing and pitting, which visual
    inspection catches late and unreliably.

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

- **Extent:** 150 × 150 km, finite, **playable throughout**. Finite because
  rivers need real outlets: globally consistent drainage is worth more than
  unbounded extent, and it is what lets large landforms come from the
  hydrology graph rather than from noise.
- **Nothing terrain-shaped is stored.** No heightfield, no world-sized
  arrays. What is scoped is residency, not area (§5.4).
- **Coordinates:** double precision throughout the field API. See §5.
- **Altitude range:** valley floors ~400–900 m, peaks to ~4500 m.
- **Sim sampling:** fixed spacings only — 0.5 m for agent contact and
  placement validation, 4 m for wide-area scans. Never view-derived.
- **Resident tiles:** 512 m square, 4 m cells (128² per tile) for masks and
  snow. Generated on demand, evicted when far, saved only if modified.
- **Presentation detail:** clipmap rings from ~10 cm at the camera,
  doubling outward to the world edge, ~20 rings and ~1.5 M vertices.
  Below the finest ring, detail is material displacement and scattered
  meshes, not geometry. Centimeter *geometry* is not a target: 1 cm over
  50 × 50 m is 50 M triangles, and snow drapes over everything under
  roughly half a meter regardless.
- **Hydrology graph:** eager to ~1.25 km node spacing world-wide
  (~15 k nodes), refined on demand to ~200 m headwaters near activity.
- `FWorldGenResult` as a monolithic heightfield is retired. Its sim-facing
  query API (`SampleHeightMeters`, `SampleSlopeDegrees`, `SampleNormal`)
  survives on the terrain field, so §6–§8 are unaffected.
