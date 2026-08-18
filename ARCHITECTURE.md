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
| `MassifWorldGen` | Heightfield generation, erosion compute shader dispatch, mask derivation, PCG input preparation. | Deterministic per seed (see §4 caveat on GPU). Outputs plain data (heightfield + masks) consumed by both sim and presentation. |
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
- **GPU caveat:** erosion runs as compute shaders. GPU float results are
  deterministic on one machine/driver but not guaranteed identical across
  GPUs. Resolution: worldgen runs **once at new-game time** and its outputs
  (heightfield + masks) are written into the save file. Gameplay
  determinism therefore never depends on regenerating terrain. Shared seeds
  reproduce the same world on the same hardware class and a near-identical
  world elsewhere. This variance is accepted; a bit-exact CPU reference
  implementation is out of scope unless seed-sharing becomes a headline
  feature.

## 5. World Generation Pipeline

Runs at new-game time, seed-driven, no hardcoded maps.

```
MasterSeed
  └► Base heightfield: ridged/uplift fractal noise (CPU, ParallelFor —
     bit-deterministic on every platform and testable under -nullrhi)
       └► Hydraulic erosion passes (compute, pipe/virtual-pipes model)
            └► Thermal erosion passes (compute)
                 └► Derived masks (compute + CPU readback):
                      flow accumulation, deposition, slope,
                      curvature, altitude bands
                        └► Semantic masks: forest, river/stream, scree,
                           rock face, buildable
                             └► PCG graphs consume masks → forests,
                                rivers, scree fields, rock scatter
```

- **Scale (initial values, tunable):** 8 × 8 km massif, heightfield
  4096 × 4096 (≈2 m/px), altitude range ≈1200–3400 m.
- All passes are compute shaders operating on persistent textures; final
  readback produces `FWorldGenResult` (CPU-side heightfield + masks) which
  is the single source of truth for both terrain building and sim queries
  (height/slope lookups, buildability checks).
- PCG runs against the generated surface using the semantic masks as
  spatial data. PCG output is presentation + collision only; anything
  gameplay-relevant derived from vegetation (e.g. "forest blocks slope
  placement") comes from the masks, not from spawned instances — this
  keeps the door open for the later storm/treefall simulation to edit
  masks and regenerate. The same simulation can later be run in
  accelerated form at worldgen time to pre-age forests (fallen trees,
  clearings, wind-shaped tree lines) instead of purely mask-driven
  placement.

### Terrain representation

Runtime landscape creation is not properly supported by `ALandscape`, so
the terrain is **generated chunked meshes**:

- Heightfield is split into chunks (e.g. 128 × 128 m). Each chunk becomes a
  `UDynamicMeshComponent` mesh built via Geometry Script from the
  heightfield, with distance-based LOD chunks.
- Chunks are plain actors registered with World Partition streaming.
- Sim never queries the mesh: all height/slope/mask queries go through
  `FWorldGenResult` (bilinear-sampled heightfield), keeping sim headless.

## 6. Snow System

Two layers with a strict boundary:

1. **Sim snow grid (gameplay truth).** `FSnowGrid` in `MassifSim`: coarse
   grid (≈4 m/cell) of `{SnowDepth, Compaction}` per cell. Updated on sim
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
- **Slope:** spline over the terrain (control points snapped to
  heightfield) with a width, rasterized into the affected snow-grid cells.
  Placement validation (slope angle range, buildable mask, connectivity)
  is a sim-side check.
- Placement flow: presentation runs a preview (ghost meshes, validity
  coloring) using the same sim-side validation functions, then enqueues a
  `PlaceLift`/`PlaceSlope` command.

## 8. Guests (NPC Agents)

- Custom lightweight agent model in `MassifSim`: `TArray<FGuestAgent>`
  (struct-of-data, no actors). Agent state: id, position (2D on
  heightfield + cached height), current graph edge + progress, activity
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

- Save = master seed + engine version tag + `FWorldGenResult` (compressed)
  + full `FMassifSimState` + save format version.
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
- Everything in `MassifSim`/`MassifCore` runs under `-nullrhi`. GPU
  worldgen stages get a small-resolution smoke test plus same-machine
  reproducibility check.

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

## World Scale (decided)

8 × 8 km massif, 4096 × 4096 heightfield (≈2 m/px), altitude range
≈1200–3400 m. Tunable constants, but this is the design target.
