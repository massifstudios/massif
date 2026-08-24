# Massif — Object Store and Save Format

Design, captured 2026-08-24. **Nothing here is built yet.** Same standing as
`ARCHITECTURE.md` once it is: changes are deliberate decisions, not side effects
of implementation work.

Related: `ARCHITECTURE.md` §5.4 (resident tiles), §5.5 (masks and PCG), §9
(save data); `DESIGN.md` (UI design system); `ROADMAP.md` M11 (playable loop and
save/load).

---

## 1. Why

Two systems place discrete objects today, by two unrelated mechanisms, and
neither survives a save:

| | foliage | rockfall |
|---|---|---|
| where positions come from | `MassifFoliageScatter::ScatterTile`, a pure function of habitat and tile | the fall itself, solved world-wide |
| when | every time a tile comes into view | once, at world gen |
| stored | nothing | `Geology.RockPlacements`, in the environment state |
| survives a save | no | no (the bake is never serialized) |

A third system would be a third mechanism. The point set built for the rockfall
is the prototype of the thing this document describes, and should be folded into
it rather than sitting beside it.

## 2. What is stored, and what is not

**Only objects with identity get stored: trees and boulders.** Everything else
is scattered at view time from the seed and the position, as it is now, and is
never written anywhere.

The rule is not arbitrary — the counts force it. Over the 20 × 20 km world:

| tier | species | candidates | at 16 B each | stored |
|---|---|---|---|---|
| trees | 10 | 11.4 M | 182 MB | **yes** |
| boulders | — | 0.64 M (measured) | 10 MB | **yes** |
| shrubs | 10 | 377 M | 6.0 GB | no |
| grass, flowers, cushion, rosette | 34 | 2.4 B | 38 GB | no |

Candidates are before habitat rejection; most of this window is above the
treeline, so placed trees should land nearer 2–4 M. **Expected save: 50–75 MB
uncompressed**, and positions within a chunk are spatially coherent, so it
compresses well.

Small rock and gravel is in the same bucket as grass: scattered on the fly
*among* the stored boulders, from the `Geology.RockfallDeposit` density the same
solve already produces. Nobody notices an individual clast, and there is no
sense in which one can be removed.

The test for the boundary: **can the player interact with this object as an
individual?** A boulder can be blasted for a piste; a tree can be felled. A
tussock cannot, and the moment one could, it would move tiers.

## 3. The record — 16 bytes

```
 0  x            4 bytes
 4  y            4 bytes
 8  z            4 bytes
12  type         2 bytes
14  metadata     2 bytes
```

### Position: int32 millimetres, not float32

Both are 4 bytes. `float32` holding absolute metres resolves about 1.2 mm at
10 km from the origin and better near it, so precision depends on where in the
world the object stands. `int32` millimetres is uniform everywhere, round-trips
exactly, and spans ±2147 km — far past any world this project will have.

Note this is a *storage* decision and does not contradict `ARCHITECTURE.md` §5,
which requires **double** for positions in the terrain field. That rule is about
evaluating a surface, where coordinate error compounds into the detail budget.
A boulder recorded to the nearest millimetre is recorded better than it can be
placed.

### Type: 2 bytes, append-only

65 536 object types, identifying the exact thing — `PiceaAbies` at a given
growth stage, a particular boulder mesh — so the mesh is a table lookup.

**Type ids are save data.** Once an id ships in a save it can never be reused or
renumbered; retiring a type leaves a hole. The registry is append-only, maps id
→ mesh path plus per-type defaults (size band, whether it collides), and lives
in `MassifCore` so both sim and presentation reach it.

### Metadata: 2 bytes, and the one tight spot

This is where the design has to give something up, and it is worth being
explicit about it. The rockfall currently carries per block: radius, yaw, pitch,
roll, buried fraction — about 8 bytes of real information. It does not fit.

The resolution is to **derive what is derivable from the position, and store
only what is not**:

- **Rotation — stored in 0 bytes.** Hash the quantised position; yaw, pitch and
  roll fall out of it. Deterministic, free, and identical on every machine.
  A tumbled boulder's orientation is arbitrary and a tree's yaw is arbitrary, so
  nothing is lost. The one thing that does go is the rockfall aiming a block's
  yaw along its travel direction, which is not visible on a tumbled block.
- **Size — 1 byte**, over the type's own size band. Genuinely not derivable: the
  fall *sorted* blocks by size, so size correlates with position without being a
  function of it. 256 steps over a 0.5–4 m band is 14 mm.
- **Flags and variant — 1 byte.** Buried fraction, mesh variant, and later state
  like felled, damaged, snow-laden.

If a future object type needs more than this, **the record grows to 20 bytes**.
It does not get solved by overloading what the two bytes mean per type — that is
the version of this decision that cannot be read back in two years.

## 4. Chunk store

- A chunk is **512 m**, matching the resident tiles (§5.4) and the buckets the
  rockfall point set already uses.
- A chunk holds a flat array of 16-byte records. No per-record header; the chunk
  header carries the count.
- A chunk is generated on first need, then **owned by the store** — generation
  does not run again for it. This is the "loading already generated terrain"
  half: the cost of placing objects is paid once per chunk, not once per view.
- Chunk generation must stay order-independent, so a chunk generated alone is
  identical to the same chunk generated after its neighbours. Boulders already
  hold to this by coming from a world-wide solve; trees hold to it through the
  scatter's existing per-tile purity.

## 5. Save file

The whole map goes in the save, per the call made when this was specified. The
argument for it is stronger than the size argument against:

- **A save becomes self-contained.** If placement is regenerated from the seed
  instead, then retuning the rockfall or editing the flora table silently
  changes the world inside every existing save. Storing the objects decouples a
  save from generator tuning entirely.
- 50–75 MB is affordable for a single-player game in 2026.

Against, recorded so the decision is revisitable: it duplicates what the seed
already determines, and it is roughly a thousand times the size of the sim state
beside it.

Required in the header, extending §9: the format version
(`FMassifCustomVersion`), the elevation model's hash — §9 already makes loading
against a different model a hard error — and the master seed. A chunk index, so
loading a region does not read the whole file.

## 6. What this changes in what exists

- **Rockfall.** `Geology.RockPlacements` stays the solve's output. The store
  becomes its consumer, converting placements to records once. The environment
  bake stays unserialized (§5.6) — the store is what persists, not the bake.
- **Foliage.** `ScatterTile` stops running per view and runs once per chunk into
  the store, for the ten tree species only. The other 44 species keep the
  present view-time path unchanged.
- **`AMassifRockManager` and `AMassifFoliageManager`** both become readers of
  the store rather than of their own sources.
- **Sim separation.** The store is world data, not sim state; presentation reads
  it directly. If an object ever becomes gameplay-relevant — a boulder blocking
  a piste — that must reach the sim through masks (§5.5) and the response table
  (§5.6.6), never by the sim reading instances.

## 7. Main menu

There is no way into the game but PIE, and the store above needs a load path, so
the menu is what the save work hangs off.

- **New game** — seed entry → generate → play. This is `ROADMAP.md` M11's
  new-game flow arriving early; M11 should be updated rather than duplicated.
- **Load game** — saves listed with seed, elevation model and timestamp.
- **Settings** — the environment settings screen (`Private/UI/`) finally has a
  home that is not a debug key.
- **Quit.**

Visual language comes from `DESIGN.md` — Swiss topographic cartography crossed
with alpine resort signage — which currently disclaims player-facing UI and will
need a section for it.

**Generation needs a progress screen.** The environment bake is about 24 s over
the full world and chunk object generation is on top of that, so "New game"
cannot be an unexplained pause. The scheduler already reports per-process
timings to the graph inspector, which is enough to drive a real progress bar
rather than a fake one.

## 8. Open questions

1. **Trees are not solved yet.** Boulders come from a world-wide fall; trees
   come from a per-tile scatter that has never been run world-wide at once.
   Does tree generation stay lazy per chunk, or become a world pass like the
   rockfall?
2. **Compression.** Worth it at 50–75 MB, and if so per chunk (so a region can
   be read alone) rather than over the whole file.
3. **Does the store hold anything but scattered content?** Lifts, stations and
   pistes are placed objects too, but they are sim state and belong in the sim's
   own save section. The line is probably: the store holds what generation
   produced, the sim holds what the player built.
4. **What happens to a save whose generator has changed?** Storing everything
   means the old world loads correctly. Confirm that is wanted, rather than a
   warning that the world is stale.
