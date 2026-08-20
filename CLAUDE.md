# Massif

Mountain resort building game. Unreal Engine 5, C++ first, Blueprint only as
thin glue. Single-player, offline.

Locked technical decisions live in `ARCHITECTURE.md`; current scope and
milestones in `ROADMAP.md`. Read both before starting work. The invariants
below override everything else in the repo.

## Invariants

- **Sim/presentation separation.** Gameplay simulation state never lives in
  rendering, UI, or presentation code. Multiplayer may be added later: keep
  sim state cleanly serializable and avoid patterns that would block UE
  replication. Any change that leaks sim state into presentation is wrong
  even if it works.
- **Determinism.** World generation and simulation are seed-driven and
  deterministic. In sim code, never use `FMath::Rand`, wall-clock time, or
  frame-rate-dependent logic — use the seeded random streams and fixed sim
  ticks provided by the architecture.
- **Scope.** Only what the current `ROADMAP.md` phase covers. Do not build
  ahead (multiplayer, extra seasons, extra activities) without being asked.

## Code Style

Prioritize readability and maintainability over cleverness.

- Follow Epic's UE C++ conventions (PascalCase, `A`/`U`/`F`/`S`/`E` type
  prefixes, `b`-prefixed bools). They take precedence over any camelCase
  default from other projects.
- Preserve descriptive names. Do not shorten identifiers.
  - Good: `SnowDepthMap`, `LiftQueueCapacity`, `ErosionIterationCount`
  - Bad: `sdm`, `cap`, `iters`
- Prefer explicit control flow. Avoid ternaries and compact conditionals;
  the only exception is when the explicit form would require duplicating a
  non-trivial expression — a preference for brevity alone is not an
  exception.
- Maximum 2 indentation levels inside a function body. Use guard clauses,
  early returns, or helper functions instead. Extracted helpers must be
  self-contained and meaningfully named — do not split a function into
  arbitrary fragments just to satisfy the indent limit.
- Keep functions short, with one clear responsibility each.
- Logic lives in C++. Blueprints only wire things together or hold data;
  anything that touches sim state is C++.
- Comments: only where the code is not readable when skimming. Comments must
  be technical and concise. Never write comments that restate the code, and
  never write historical comments — do not document old decisions, previous
  implementations, or what changed (no "previously...", "changed from...",
  "used to...").
- Do not change behavior, public APIs, data shapes, or side effects unless
  explicitly asked. If a required refactor (e.g. for the indent rule) would
  change behavior, stop and ask instead.
- Existing code is not authoritative just because it exists. If an existing
  pattern looks like a design mistake, do not copy it — ask, or verify
  against Epic's documented approach. The user is the authority.

This is a development project with no users. When editing code do not state
what something was changed from; git has the history.

## Save Data

When changing the save-game format or serialized sim state, ask whether
existing saves should be overwritten (dev reset) or migrated, and create
your changes accordingly.

## Assets

- Git LFS handles `*.uasset`, `*.umap`, and other binary content. Never
  commit `Binaries/`, `Intermediate/`, `Saved/`, or `DerivedDataCache/`.
- Binary assets cannot be merged: never edit the same `.uasset`/`.umap` on
  parallel branches. Prefer C++- or data-defined content over editor-placed
  content where practical.
- Current meshes/materials are placeholders to be swapped for AI-generated
  assets later. Use standard UE prefixes (`SM_`, `SK_`, `M_`, `MI_`, `T_`,
  `ABP_`) and stable names so replacements can drop in without reference
  fixups.

## Tests

After completing all requested changes, and before committing:

1. Decide whether any change needs a new test. Behavior changes and bug
   fixes need one; pure refactors and comment/style edits do not. If you
   decide no new test is needed, say so in one sentence. New tests are UE
   Automation tests in the owning module's `Tests/` subfolder. Sim logic
   must be testable headless (`-nullrhi`) — if a change makes sim logic
   untestable without rendering, the sim/presentation separation is broken;
   fix that instead of skipping the test.
2. Verify the project compiles (Development Editor) and run the automation
   suite regardless of whether you wrote a new test:

   ```sh
   UnrealEditor-Cmd Massif.uproject \
     -ExecCmds="Automation RunTests Massif; Quit" \
     -nullrhi -unattended -nosplash -log
   ```

3. If tests fail, fix the code until they pass. Never delete, skip,
   weaken, or rewrite a test to make it pass unless the test itself is
   wrong — and if so, say so explicitly and explain why.

## Visual Verification

Never judge how the game looks yourself. Do not run the game to take a
screenshot and do not open one to inspect it — you cannot frame a view or
tell what is worth looking at.

When a change needs to be seen, say what you changed and ask the user to
look, naming what to look for. Running the game to read *logged numbers* is
fine; reading pixels is not.

## Git

You may use git. Work in this order: change → test → commit → push.

- If the worktree is dirty before you start, commit the existing state
  separately with the message `wip: pre-existing changes`. Never mix
  pre-existing changes into your own commits.
- Commit at each logically complete step (a step that builds and passes
  tests), not one giant commit at the end.
- Commit messages: short, to-the-point title; add a body only when the
  change is not obvious from the title. If a change spans unrelated
  concerns, split it into multiple commits.
- Mention in the commit message that the commit was made by you (an AI)
  and not a human. Do not use a `Co-Authored-By` trailer.
- On a feature branch: commit only changes belonging to that branch's
  feature, then merge back into main. Merge only after verification
  passes — never merge red. Unrelated changes go on a separate branch.
- Push only after verification passes. Never force-push.
