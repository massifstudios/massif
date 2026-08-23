# Next

Open work, in the order I would take it. Written 2026-08-23, on branch
`worldgen/environment-bake` at 183 green automation tests.

`ROADMAP.md` holds the milestones; this holds the loose ends underneath them.
Anything here that turns into real scope should move there and come out of this
file.

---

## Decisions only you can make

### 1. Merge the branch

`worldgen/environment-bake` is **102 commits ahead of `main`**, green, and has
never been merged. `main` is still at `wip: pre-existing changes`. There is no
remote, so nothing to push — merging is purely local and purely your call.

The longer it sits the less a review of it means.

### 2. Turn adaptive unity off

UBT picks the non-unity working set out of `git status`, so files being edited
compile alone and files that are committed rejoin one translation unit. Two
helpers with the same name in two anonymous namespaces therefore build and pass
the whole suite, and break the build the moment the work is committed. That has
now happened **nine times**, most recently taking the game down entirely.

One line in the target ends it. It costs some incremental build time and buys
back the property that a green suite means a build the user can run.

Until then: verify with `-DisableAdaptiveUnity` before committing C++, and note
that a module which only _relinks_ was never recompiled.

## Correctness and staleness

### 4. Nothing repaints after a re-solve

`AMassifDebugChannelView::Tick` repaints only when the selected view or slice
changes (`MassifDebugChannelView.cpp:76`). A fidelity change re-solves the
channels underneath it and repaints nothing, so **the settings screen currently
looks like it did nothing**. The same is true of `massif.Env.Fidelity` and
`massif.Env.Preset`.

Smallest honest fix: have the scheduler carry a solve counter, and repaint when
it moves. Cheap, and it makes the whole fidelity feature legible.

### 5. Foliage and masks never regenerate

`MassifFoliageManager` has no rebuild path at all — it scatters once. Turning
Vegetation to Off and back leaves the trees exactly where they were, standing on
ground that no longer grows anything.

Bigger than item 4 and worth doing after it.

### 6. Apply freezes the game

`FMassifEnvironmentSettingsModel::Apply` runs the solve on the game thread. Sun
at High measured **10.6 s**. The screen locks solid for that long with no
spinner, then returns and reports the time.

This is not a UI job — it is **E15**: consumers share the state object the solve
writes into, so it cannot be moved off-thread without double-buffering. Until
that exists, the least I would do is draw an "Applying…" state and let the frame
present once before the solve starts.

---

## Performance

### 7. E14 — GPU paths

Measured on the full 20 km world: **24.1 s total**, of which

| process           | seconds    | shape                                                 |
| ----------------- | ---------- | ----------------------------------------------------- |
| `Sun.Insolation`  | 10.6       | per-cell, read-only terrain, no accumulation          |
| `Soil.Deposition` | 3.1        | droplet scatter, needs integer atomics on fixed point |
| everything else   | < 0.5 each |                                                       |

Sun first: it is 44% of the bake on its own and is deterministic on the GPU
without any special handling. Droplets second — the determinism contract
(integer atomics on a fixed-point encoding) was designed for this port before
the port existed.

The two ordered hydrology solves would have to become fixed-count relaxations
before they could move, which changes their results. Not obviously worth it.

`FMassifEnvironmentScheduler::SetGpuEnabled` already exists and no process reads
it yet.

---

## Settings screen, second pass

Everything here is polish on what shipped this round.

### 8. Presets

A row of Low / Medium / High across the top that moves every system that
implements the level, the way `massif.Env.Preset` already does. The model has
the hook; the screen does not expose it.

### 9. Reset to defaults

Nothing currently gets a saved config back to the graph's own defaults except
deleting `MassifEnvironment.ini` by hand.

### 10. Keyboard and gamepad navigation

The screen is mouse-only. Slate gives focus and arrow-key traversal cheaply, and
Escape should close it.

### 11. Say what a level means

The rows say `Wind Simulation: Low`. They do not say what Low costs or what it
stops modelling. The processes already carry the knowledge in their fidelity
branches; a one-line `Description` per level on the declaration would surface
it, in the same spirit as `DisplayName`.

---

## Cleanup

### 12. `MassifLithologyStrikeBearingDegrees`

Public only so a test can find the strike, duplicating the scheduler's
stream-naming convention rather than deriving from it. Either have the test
discover the bearing by sweeping, or have the scheduler expose the naming.

### 13. Lithology test double-registration guard

`LithologyProcessTest` guards against registering twice by checking
`GetSupportedFidelities(...).Num()`. Climate and snow simply do not register
twice. Make lithology match.

### 14. `Content/Textures/` is 717 MB and untracked

Your decision, recorded and still standing. Worth revisiting only if someone
else ever needs to build this.

---

## Known-and-accepted

Not bugs, but they will look like bugs to whoever hits them next.

- **Fidelity is per install, not per save.** The environment is a pure function
  of the seed and rebuilt at load, so a save reopens at whatever the machine is
  set to. Two machines set differently hold different worlds from one seed.
- **`SpringComparisonRadiusMeters` clamps to a quarter of the grid.** On a world
  smaller than the reach, springs compare over less than 400 m rather than
  returning zero everywhere. Tests that pin exact discharge figures must set the
  reach they expect.
- **`Hydrology.SeasonalFlow` is stored at 32 m** and reduced from 8 m water by
  taking the largest value covering each cell. A mean would lose rivers. It is
  still a lossy reduction, and the alternative — storing a scale factor and
  multiplying at read time — would be exact and cheaper, but changes the channel
  set.
