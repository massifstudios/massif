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
- **Sim never samples the terrain field directly.** The field is
  LOD-dependent by design, and sampling it at view LOD would make sim state
  depend on where the camera is. Instead worldgen bakes a fixed-resolution
  height/slope grid over the **playable region** once at new-game time and
  writes it into the save (§9, §12). Sim queries that grid and nothing
  else. Presentation samples the field freely at whatever LOD it needs; the
  two are expected to disagree below the sim grid spacing, and that
  disagreement is never gameplay-relevant.
- Baking the sim grid also keeps saves self-contained and immune to float
  drift between compilers and platforms. Shared seeds reproduce the same
  world on the same hardware class and a near-identical world elsewhere;
  a bit-exact cross-platform reference implementation is out of scope
  unless seed-sharing becomes a headline feature.

## 5. World Generation

Seed-driven, no hardcoded maps. Terrain is a **pure function of world
position**, never a stored grid:

```
Height(WorldX, WorldY, SampleSpacingMeters) → meters
```

`SampleSpacingMeters` selects how far down the octave stack to evaluate:
detail finer than roughly twice the spacing is skipped, and the finest
octave included is faded in smoothly rather than switched on, so a coarse
sample and a fine sample of the same point agree.

At the world extent in §12 no full-resolution grid can exist — 150 km at
1 cm is 10^15 samples — so on-demand evaluation is not an optimisation
here, it is the only representation that works.

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
and can be run as an offline pass over the baked sim grid, but it is off
by default and nothing depends on it.

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
  the playable region and wherever the camera goes.
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
- Playable-region actors register with World Partition. Far-field backdrop
  rings are camera-relative and not streamed through World Partition.
- Sim never queries the mesh, and never queries the field: all
  height/slope/mask queries go through `FWorldGenResult`, which is now the
  baked playable-region grid (§12).

### 5.4 Masks and PCG

Masks are derived over the playable region only, from the baked grid:
flow accumulation, deposition, slope, curvature, altitude bands →
semantic masks (forest, river/stream, scree, rock face, buildable).

PCG runs against the generated surface using the semantic masks as spatial
data. PCG output is presentation + collision only; anything
gameplay-relevant derived from vegetation (e.g. "forest blocks slope
placement") comes from the masks, not from spawned instances — this keeps
the door open for the later storm/treefall simulation to edit masks and
regenerate. The same simulation can later be run in accelerated form at
worldgen time to pre-age forests (fallen trees, clearings, wind-shaped
tree lines) instead of purely mask-driven placement.

Outside the playable region there are no masks. Far-field vegetation and
rock are driven directly by the terrain field (altitude, slope, aspect),
since nothing there is gameplay-relevant.

## 6. Snow System

Two layers with a strict boundary:

1. **Sim snow grid (gameplay truth).** `FSnowGrid` in `MassifSim`: coarse
   grid (≈4 m/cell) over the playable region only, of
   `{SnowDepth, Compaction}` per cell. Updated on sim
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

- Save = master seed + engine version tag + playable-region bounds +
  `FWorldGenResult` (the baked playable-region grid, 16-bit heights,
  compressed) + full `FMassifSimState` + save format version.
- The world outside the playable region is never saved. It is a pure
  function of the seed and is regenerated on load.
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

Two extents, deliberately different. Conflating them is what forces either
a tiny world or an unstorable one.

| | extent | resolution | where it lives |
|---|---|---|---|
| **World** | 150 × 150 km, finite | terrain field, any spacing on demand | nothing stored; pure function of the seed |
| **Playable region** | ~16 × 16 km around the resort massif | 4 m sim grid (4001², 16-bit ≈ 32 MB, compressed in save) | `FWorldGenResult`, masks, snow grid |

- The world is almost entirely backdrop and view. It is finite: rivers need
  real outlets, and globally consistent drainage is worth more than
  unbounded extent.
- The playable region is chosen at new-game time — the generator picks a
  massif with usable relief and aspect. It is the only part with
  sim-resolution data and the only part the player may build in.
- **Altitude range:** valley floors ~400–900 m, peaks to ~4500 m.
- **Presentation detail:** clipmap rings from ~10 cm at the camera,
  doubling outward to the world edge, ~20 rings and ~1.5 M vertices.
  Below the finest ring, detail is material displacement and scattered
  meshes, not geometry.
- `FWorldGenResult` keeps its existing sim-facing query API
  (`SampleHeightMeters`, `SampleSlopeDegrees`, `SampleNormal`). Only its
  extent and its producer change, so §6–§8 are unaffected.
