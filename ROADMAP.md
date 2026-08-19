# Massif — Roadmap

Current phase: **Phase 1 — Playable Winter Core**. Everything not listed
under Phase 1 is out of scope until this file says otherwise.

Phase 1 goal, end to end: enter a seed → generated massif with snow →
place one lift and one slope → guest NPCs arrive, queue, ride up, ski
down, repeat.

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

### M3 — Erosion (GPU) 
- Hydraulic erosion (virtual pipes model) + thermal erosion as compute
  shader passes over the generated heightfield, results read back into
  `FWorldGenResult`. Skipped gracefully under `-nullrhi`.
- Erosion parameters in `FWorldGenConfig` (iteration counts, rain,
  sediment capacity, talus angle).
- **Verify:** GPU reproducibility test (two runs, same seed, identical
  hash; auto-skips headless); CPU-stage tests unchanged; manual —
  drainage gullies, smoothed flanks, sediment-flattened valley floors.

### M4 — Masks, Textured Terrain & PCG Forests
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

### M5 — Snow: Sim Grid + Visual Prototype
- `FSnowGrid` in sim: snowfall and altitude/temperature-driven melt on
  sim tick, deterministic.
- Snow material on terrain: coverage/depth from GPU copy of sim grid;
  deformation render target for tracks (presentation-only); Nanite
  displacement prototype.
- Debug: snow depth overlay, manual "dump snow here" debug command.
- **Verify:** tests — snow accumulation/melt determinism, sim grid hash
  stable across runs; manual — snow visibly accumulates, a dragged debug
  brush leaves tracks that do not affect sim hash.

### M6 — Building Tools: Lift & Slope Placement
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

### M7 — Lift Simulation
- Chairs advance along the lift line each sim tick; guest queue with
  `LiftQueueCapacity`; board/alight at stations.
- Presentation: station/tower/chair placeholder meshes from sim data,
  chairs interpolate between sim ticks.
- **Verify:** tests — chair spacing/period math, queue FIFO under load,
  boarding conservation (nobody duplicated/lost), determinism hash;
  manual — chairs visibly cycle.

### M8 — Guest Agents
- `FGuestAgent` array in sim: spawn at resort entry, state machine
  Arrive → QueueLift → RideLift → SkiSlope → loop → Leave; ski speed from
  slope angle + snow condition + seeded skill.
- Presentation: pooled placeholder guest meshes, small animation cycle
  set (ski, turn, sit, walk), interpolated from sim state.
- Guests' ski runs write the visual deformation RT (tracks).
- **Verify:** tests — full agent loop headless over N ticks (guests cycle
  and never wedge in a state), determinism hash with 200 agents;
  manual — guests queue, ride up, ski down, repeat.

### M9 — Playable Loop & Save/Load
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
