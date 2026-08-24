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

| tier | species | candidates | at 24 B each | stored |
|---|---|---|---|---|
| trees | 10 | 11.4 M | 274 MB | **yes** |
| boulders | — | 0.64 M (measured) | 15 MB | **yes** |
| shrubs | 10 | 377 M | 9.0 GB | no |
| grass, flowers, cushion, rosette | 34 | 2.4 B | 58 GB | no |

Candidates are before habitat rejection; most of this window is above the
treeline, so placed trees should land nearer 2–4 M. **Expected save: 65–110 MB
uncompressed**, and positions within a chunk are spatially coherent, so it
compresses well.

Small rock and gravel is in the same bucket as grass: scattered on the fly
*among* the stored boulders, from the `Geology.RockfallDeposit` density the same
solve already produces. Nobody notices an individual clast, and there is no
sense in which one can be removed.

The test for the boundary: **can the player interact with this object as an
individual?** A boulder can be blasted for a piste; a tree can be felled. A
tussock cannot, and the moment one could, it would move tiers.

## 3. The record — 24 bytes

```
 0  x        int32   millimetres, absolute world
 4  y        int32
 8  z        int32
12  type     uint16  append-only registry id
14  size     uint8   over the type's own band
15  flags    uint8   variant 0-3, felled 4, dead 5
16  yaw      uint16  1/65536 turn
18  pitch    uint8   1/256 turn
19  roll     uint8
20  planted  int16   half-months from world start; negative is before it
22  vigour   uint8   255 healthy, 0 dead standing
23  buried   uint8   0 resting on the ground, 255 level with it
```

Four-byte aligned with no padding, so a chunk is a flat array a reader can
`memcpy`. No bitfield packing beyond `flags`, and no field that means a
different thing per type — that is the version of this format nobody can read
back in two years.

### Why not 16 bytes

The narrow version squeezed rotation out entirely and **derived it from a hash
of the position**. That is only sound while rotation is immutable, and it is
not: a felled tree lies in the direction the cut chose, and a boulder blasted
off a piste both moves and would therefore spin for no reason. Storing rotation
closes a hole rather than buying comfort.

The extra eight bytes cost 21–37 MB over the whole world (§2). Cheap for a
format that stops needing exceptions.

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

### Size: 1 byte over the type's band

Genuinely not derivable, which is why it is stored: the rockfall *sorted* blocks
by size, so size correlates with position without being a function of it. 256
steps over a 0.5–4 m band is 14 mm.

Because the band belongs to the type, a record alone does not yield a radius —
decoding needs the registry. That is deliberate. Retuning a species' size range
then reshapes every tree of it without rewriting a save.

### Age is not stored; the planting date is

`planted` is fixed the moment the object exists and never moves again, so age
advances with the calendar rather than by touching 3 M records. Signed, because
every generated tree was planted before the world started, and ±32767
half-months is ±1365 years.

Growth *rate* is the one intrinsic that has to be stored rather than derived: it
is a function of site quality, site quality lives in the bake, and the bake is
never serialized (§5.6). It folds into `size`, which already means size at
planting — drawn size is `f(band, size, planted, now)`.

### What boulders leave empty

`planted`, `vigour` and most of `flags`. 0.64 M boulders × 8 unused bytes is
5 MB, which does not justify a second record layout, a second array and a second
code path on every reader.

## 4. Both stored tiers come from world passes

Boulders already do. **Trees become one too**, rather than staying a per-tile
scatter run lazily.

Storage is the smaller half of the reason. The larger half is that a per-tile
scatter *structurally cannot* model how a forest is arranged, because every
mechanism that arranges one reaches past a tile:

- **Competition.** A mature spruce suppresses what grows within its own crown
  radius. Two candidates either side of a tile edge cannot see each other.
- **Stand structure.** Age classes, gaps, an edge that thins rather than stops.
- **Seed dispersal.** A stand spreads from parent trees, which is why a treeline
  is ragged and why a cleared slope refills from its edges inward.
- **Disturbance.** Avalanche paths cut forest-free stripes; `Snow.AvalanchePath`
  is already a channel, and a tree pass can read it directly.

None of that fits behind an apron. A world pass gets all of it, and it makes the
forest the graph simulated (`Vegetation.Cover`, `Height`, `Canopy`,
`VegetationClass` from E11) and the trees actually drawn into the same forest —
the same argument that keeps `RockfallDeposit` and the boulders honest.

Shape: `Vegetation.TreePlacements`, a point set produced by a process
downstream of the vegetation relaxation group, a leaf of the graph exactly as
the rockfall is. Cost should be well under the rockfall's 3.6 s — 11.4 M
candidates, but one habitat evaluation each rather than a 400-step fall.

**This supersedes part of `ARCHITECTURE.md` §5.4.** Its table puts "trees, rocks,
scree" under *discrete scatter*, seeded per tile with the eight neighbours also
sampled. That is now true only of the tiers that are not stored. §5.4 needs a
deliberate edit, not a silent one — it is a locked-decisions document.

## 5. Chunk store

- A chunk is **512 m**, matching the resident tiles (§5.4) and the buckets the
  rockfall point set already uses.
- A chunk holds a flat array of 24-byte records. No per-record header; the chunk
  header carries the count.
- **Chunks are not generated lazily.** With both stored tiers coming from world
  passes (§4), the store is filled once at world gen by draining the two point
  sets into it, and is complete from that moment. Order-independence is not a
  property the chunking has to defend, because nothing is decided per chunk —
  the chunk is a storage and streaming unit only.
- What is lazy is **reading**: a chunk is pulled off disk when the camera or
  player activity needs it and dropped when they leave, which is what
  `AMassifRockManager` and the foliage manager already do against the point set.
- This is the "loading already generated terrain" half. The cost of *placing*
  objects is paid once ever, at world gen, and then never again for that save —
  not once per chunk and not once per view.

## 6. Save file

The whole map goes in the save, per the call made when this was specified. The
argument for it is stronger than the size argument against:

- **A save becomes self-contained.** If placement is regenerated from the seed
  instead, then retuning the rockfall or editing the flora table silently
  changes the world inside every existing save. Storing the objects decouples a
  save from generator tuning entirely.
- 65–110 MB is affordable for a single-player game in 2026.

Against, recorded so the decision is revisitable: it duplicates what the seed
already determines, and it is roughly a thousand times the size of the sim state
beside it.

Required in the header, extending §9: the format version
(`FMassifCustomVersion`), the elevation model's hash — §9 already makes loading
against a different model a hard error — and the master seed. A chunk index, so
loading a region does not read the whole file.

## 7. Mutable state lives beside the store, not in it

A tree's vigour falls, a tree gets felled, a boulder gets blasted off a piste.
That is state changing during play, and the naive answer — write it back into
the record — would make the store savegame state that presentation reads
directly, which is the pattern `CLAUDE.md`'s separation invariant exists to
forbid.

So the store is **immutable once written**, and what the record holds is the
*initial* condition the generator produced. A generator-placed windthrow or dead
standing snag is initial condition and belongs in the record; a tree the player
felled is not.

Deviations live in a **sparse overlay** the sim owns and saves:

```
TMap<FMassifObjectIndex, FMassifObjectState>
```

keyed by (chunk, index within chunk). Almost no object ever deviates — a resort
fells thousands of trees, not millions — so the overlay is small enough that
storing full state per entry costs nothing, and it stays empty for the entire
boulder tier until the first blast.

This buys three things beyond the invariant:

- **Reads stay branch-free where it matters.** Drawing a chunk walks a flat
  array; the overlay is consulted once per chunk, not once per instance, because
  an absent chunk key means no instance in it deviates.
- **A regenerated world diffs cleanly against a played one.** The immutable half
  is a pure function of the seed, so what the player changed is exactly the
  overlay.
- **Presentation never reads sim state.** It reads the store, plus whatever the
  sim publishes about the overlay through the existing mask (§5.5) and response
  table (§5.6.6) paths.

The cost is one indirection on every gameplay query about a specific object, and
a rule that has to hold: **nothing writes a record after the store is built.**

## 8. What this changes in what exists

- **Rockfall.** `Geology.RockPlacements` stays the solve's output. The store
  becomes its consumer, converting placements to records once. The environment
  bake stays unserialized (§5.6) — the store is what persists, not the bake.
- **Foliage.** The ten tree species leave `ScatterTile` entirely for a world
  pass (§4) writing `Vegetation.TreePlacements`. The other 44 species keep the
  present view-time path unchanged, so `ScatterTile` stays, with a smaller job.
- **`AMassifRockManager` and `AMassifFoliageManager`** both become readers of
  the store rather than of their own sources.
- **Sim separation.** The store is immutable world data; the sim's overlay (§7)
  holds everything that changes. Presentation reads the store directly and never
  the overlay.

## 9. Main menu

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

## 10. Open questions

1. **Compression.** Worth it at 65–110 MB, and if so per chunk (so a region can
   be read alone) rather than over the whole file.
2. **What happens to a save whose generator has changed?** Storing everything
   means the old world loads correctly. Confirm that is wanted, rather than a
   warning that the world is stale.

Settled since first writing: the store holds only what generation produced —
lifts, stations and pistes are what the player built and belong to the sim's own
save section, on the same line §7 draws for object state.
