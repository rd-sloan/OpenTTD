# Migrating performance-critical systems to an EnTT registry

**Status:** exploratory, fork-local. Not intended for the upstream repository.

This document plans a phased migration of OpenTTD's performance-critical vehicle
systems onto an entity-component architecture using
[EnTT](https://github.com/skypjack/entt) 3.16.0.

Ground rules that shape the whole plan:

- The game must build (win64) and be playable after **every** phase.
- The savegame format must not change, because existing saves are used as
  benchmark fixtures.

Line references are to master at `3a33cd64c9` and will drift.

## Table of contents

- [1. Verdict](#1-verdict)
- [2. Where the time goes](#2-where-the-time-goes)
- [3. Three constraints that shape everything](#3-three-constraints-that-shape-everything)
- [4. Strategy: a parallel registry, not a rewrite](#4-strategy-a-parallel-registry-not-a-rewrite)
- [5. Phases](#5-phases)
- [6. What to leave alone](#6-what-to-leave-alone)
- [7. What to actually expect](#7-what-to-actually-expect)
- [8. Provenance](#8-provenance)

## 1. Verdict

Yes for targeted extraction. No for wholesale conversion.

OpenTTD has already independently invented half of an ECS and then stopped.
The pool system in `src/core/pool_type.hpp` is a slotmap: stable integer handles
(`PoolID`), O(1) lookup by index, a free-slot bitmap, and iteration that walks
live entries in ascending index order.
That is the entity half, and it is genuinely good.

What it lacks is the component half.
`Vehicle` is a single struct carrying roughly ninety fields, an embedded
`VehicleCargoList`, an embedded `Order`, three separate cache sub-structs, six
intrusive pointers, a vtable, and `Money` accounting fields -- all resident
whether or not the current operation touches them.
The per-tick loop reads perhaps twenty bytes of that object.

So the win is real and locatable.
But a full conversion is not on the table, because of the blast radius:

| Measure | Count |
| --- | --- |
| Files referencing `Vehicle` | 281 |
| Raw `Vehicle *` declarations | 1051 |
| Full-pool sweeps (`::Iterate()`) | 143 |
| -- untyped / typed | 95 / 48 |
| Vehicle field reads in `newgrf_engine.cpp` alone | 201 |
| Lines in the core vehicle translation units | ~21,300 |

The workable shape is a **parallel registry** that holds hot data alongside the
existing pool, with hot loops moved over one at a time.

## 2. Where the time goes

You do not have to guess at the hot systems, because OpenTTD instruments them
itself.
`PerformanceElement` in `src/framerate_type.h` enumerates exactly the loops an
ECS targets -- and the four vehicle categories are accumulators, meaning they
were split out precisely because they dominate.

| Instrumented element | Backing loop | ECS fit |
| --- | --- | --- |
| `GameLoopTrains`, `GameLoopRoadVehicles`, `GameLoopShips`, `GameLoopAircraft` | `CallVehicleTicks()`, `vehicle.cpp:985` | Strong |
| `GameLoopEconomy` | `LoadUnloadStation()` per station, cargo aging | Moderate |
| `ViewportDrawing` | sprite cache, viewport hash chains | Strong |
| `GameLoopLandscape` | tile-driven, not pooled objects | Poor |
| `GameLoopLinkGraph` | already a separate job on its own thread | Poor |

The single most telling detail is inside `CallVehicleTicks()`: it calls
`virtual bool Tick()` on every vehicle, then immediately runs a
`switch (v->type)` over the same object.
Virtual dispatch *and* a type switch, per vehicle, per tick.

## 3. Three constraints that shape everything

These are not style preferences.
Each one eliminates specific EnTT features, and getting them wrong produces bugs
that only appear in multiplayer or on reload.

### 3.1 Canonical iteration order

OpenTTD needs two kinds of determinism.
Every client must evolve the game state identically, and *a savegame resumed must
evolve identically to a run that was never saved* -- the second is what makes
desync replay and mid-game joins work at all.
See [desync.md](./desync.md).

Today's pool iteration satisfies both trivially: `PoolIterator` visits live
entries in ascending index order, which is a pure function of the live set and
carries no history.
EnTT's packed-array order is the opposite -- it is a function of the entire
create/destroy history, because swap-and-pop moves the last element into each
hole.

Two specifics, both pinned down by tests in `src/tests/entt_smoke.cpp` so that an
EnTT upgrade which changed either one fails loudly rather than silently
desyncing:

- **Views iterate the packed array in reverse.** A registry populated with ids 0
  to 7 and nothing removed iterates as `7, 6, 5, 4, 3, 2, 1, 0`. Iteration is
  not creation order even before history enters the picture.
- **After a removal there is no canonical order at all.** Destroying the id 2
  entity from that set leaves the packed array as `0, 1, 7, 3, 4, 5, 6`, which
  the reversed walk yields as `6, 5, 4, 3, 7, 1, 0` -- neither ascending nor
  descending.

The failure this produces: the savegame stores the live set, not the allocation
history.
Load a save and EnTT's packed order is fresh and sequential; in the run that
produced that save it was scrambled.
Any loop whose result depends on visit order then diverges -- and in OpenTTD
several do, because `Random()` is a single shared LCG.
If vehicle A draws before B instead of after, both get different numbers, and the
game states separate immediately.

**Rule:** any storage that game-state code iterates must be sorted into canonical
ID order before use.
Call `registry.sort<T>()` keyed on the stable ID, gated on a dirty flag set by
create and destroy.
Structural churn is rare next to the tick rate, so this amortises to nearly
nothing -- and once sorted the storage is contiguous in ID order, so the
locality you came for is preserved.

Usefully, `sort` arranges storage so that *iteration* matches the comparator
rather than the raw packed layout, so a plain `lhs.value < rhs.value` yields
ascending iteration despite the reversed walk.
That reproduces `PoolIterator`'s ascending-index order exactly, which is what
makes an order-preserving migration possible at all.

The mechanism, since it is worth knowing rather than trusting: iteration walks
the packed array backwards (`begin()` is `iterator{packed, packed.size()}` and
`operator*` reads `packed[offset - 1]`), and `sort_n` sorts through *reverse*
iterators (`algo(packed.rend() - length, packed.rend(), compare)`), leaving the
array physically descending.
Walking a descending array backwards yields ascending order, so the two
reversals cancel by design.
EnTT defines `sort` in terms of iteration order, not physical layout.

The practical consequence is that the reversed walk is invisible **as long as you
sort**, and leaks the moment you iterate unsorted storage.
That is a live trap for phase 2, where the client-side components deliberately do
not need sorting: code there must not assume creation order.

**Consequence:** owning groups are unavailable for game-state components.
EnTT forbids sorting a pool once a group owns it, and sorting is exactly what is
needed here.
Use views, or non-owning groups.

### 3.2 Pointer stability

EnTT's default swap-and-pop invalidates references on removal -- and *sorting
moves components too*, so switching a storage to `in_place_delete` does not
rescue you.

In fact `in_place_delete` is not merely insufficient, it is unavailable. `sort_n`
opens with

```c++
ENTT_ASSERT((mode != deletion_policy::in_place) || (head == max_size), "Sorting with tombstones not allowed");
```

so a storage using in-place deletion cannot be sorted once it holds a tombstone.
Pointer stability via `in_place_delete` and canonical order via `sort` are
therefore mutually exclusive for any storage that game-state code iterates, and
canonical order is not negotiable. The rule below is the only route, not the
better of two.

Meanwhile `Vehicle` is threaded through six intrusive pointers (`next`,
`previous`, `first`, `last`, and the shared-order pair) and two hash chains that
store `Vehicle **` outright:

```c++
Vehicle **hash_tile_prev     = nullptr;  // vehicle_base.h:240
Vehicle **hash_viewport_prev = nullptr;  // vehicle_base.h:237
```

**Rule:** `VehicleID` is the only durable reference.
A component reference is valid within one tick phase and is never stored across
one.
The legacy `Vehicle *` path stays alive and correct throughout the migration --
that is what keeps the game playable between phases.

### 3.3 The savegame format is frozen

This constraint turns out to be the friendly one.
`SLE_VAR` does not use `offsetof`; it expands to a lambda that returns a member
address:

```c++
return std::addressof(static_cast<const base *>(b)->variable);
/* SLE_GENERAL_NAME, saveload.h:933 */
```

The descriptor list is therefore already an indirection layer, keyed by field
*name* -- and the `"VEHS"` chunk is a `ChunkType::SparseTable`, so pool indices
are written explicitly and exact IDs are recoverable on load.

**Rule:** never touch the descriptor lists.
Keep the named fields on `Vehicle` as a save/load staging area: gather
components into `Vehicle` before saving, scatter back into components after
loading.
Output is byte-identical by construction, `vehicle_sl.cpp` barely changes, and
the benchmark saves keep working.

Ignore EnTT's `snapshot` and `continuous_loader` entirely -- they would write a
new format, which is the one thing we cannot have.

## 4. Strategy: a parallel registry, not a rewrite

Every phase below preserves this division of labour.
`Vehicle` and its pool keep three jobs: identity, cold data, and the save
facade.
The registry holds hot per-tick data.
Loops migrate individually, and both representations stay valid at once during
each migration -- verified, not assumed.

The technique that makes this safe is **shadow mode**: when a field moves, write
it to both places and assert equality every tick behind a debug flag.
Ship the phase with shadow mode on, run the benchmark saves, and only remove the
old field once the assert has been silent across a long run.

Note the deliberate ordering of the phases: the early ones have *no* determinism
exposure, so the EnTT API is learned against systems that cannot desync and are
not in the savegame.
Risk climbs only after the idioms are familiar.

### Verification harness

Built in phase 0 and documented in [benchmark/README.md](../benchmark/README.md).
The null video driver already accepted a tick count, and now also accepts a
`stats=<path>` parameter that writes a report of timings, workload counts and
object sizes when the run finishes:

```powershell
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label phase0 -CheckDeterminism
```

Use the runner rather than invoking the binary directly. Three headless
behaviours on Windows are invisible failures rather than error messages, and the
runner already accounts for them:

- The binary is GUI subsystem, and `CreateConsole` reopens the standard streams
  onto a freshly allocated console, so redirected stdout and stderr capture
  nothing even for a failing run. Reports go to a file for this reason.
- `-c <path>` makes the config file's parent directory a data search directory
  (`fileio.cpp`), so a config outside the build tree hides `build/baseset` and
  the game exits 1 with no output.
- `gui.autosave_on_exit` defaults to false, so without `benchmark/bench.cfg`
  there is no `exit.sav` and a determinism check silently compares nothing.

Determinism checks available on top of that:

- `-d desync=2` adds vehicle cache validation (`src/cachecheck.cpp`) and command
  logging; `-d desync=3` adds monthly `dmp_cmds_*.sav` dumps. See
  [debugging_desyncs.md](./debugging_desyncs.md).
- `ctest --test-dir build -C Debug -R regression` must stay green. It is the
  closest thing in-tree to a state-equivalence test.

### Comparing savegames byte for byte does not work

This plan originally proposed comparing exit saves byte for byte as the
determinism check. Phase 0 measured that and it does not hold, **on unmodified
master**, so it cannot be an exit criterion.

Running Hilbergen twice with identical arguments produces exit saves that differ
in a few hundred bytes. Two observations identify what is going on:

- One 32 byte cluster is `_game_session_stats.savegame_id` (`misc_sl.cpp:105`), a
  random per-session identifier. Expected, and harmless.
- The remaining differences are scattered in small 1 to 4 byte runs through the
  `VEHS` chunk at roughly one per vehicle record, and **the count does not grow
  with the number of ticks simulated**: 488 differing bytes after 1 tick, 442
  after 100, 445 after 20000.

Immediate divergence that does not accumulate is not simulation drift. It points
at bytes in the vehicle records that are not a function of game state, most
likely fields that are never initialised for some vehicle types and are
serialised with whatever the allocator happened to leave there.

Ruled out along the way: the link graph never ran (`perf.link_graph.count` is 0),
`_interactive_random` is not saved (only `_random.state`), `StationCompare` orders
by index rather than pointer, and realtime autosaves are disabled in `bench.cfg`.

**A game state fingerprint is used instead**, implemented in
`src/state_fingerprint.cpp`. It hashes the values that define behaviour -- vehicle
position, speed, order state, cargo and chain structure, company balances, station
cargo, town populations, industry production, and the shared randomiser -- and
records the result in the benchmark report.

Two design points carry the weight. Everything is visited in ascending pool index
order, so the hash is a function of the live set rather than of allocation
history. Object references are hashed as pool indices, never as pointers, so the
result is independent of the allocator and of address space layout -- which is
what makes it survive the very refactors it is meant to check.

The hash is split per subsystem rather than combined into one number, so a
mismatch says *where* behaviour changed instead of merely that it did.

Verified both ways. Two identical 3000 tick runs produce identical fingerprints
while their savegames differ by roughly 100 KB, which also confirms the simulation
itself is deterministic and the savegame noise really is noise. Running 3001 ticks
instead changes the vehicle, company, station and global hashes, and correctly
leaves towns and industries untouched because those only update on periodic ticks.

Note that this says nothing about save *format* compatibility, which remains a
hard requirement and is verified differently: a save written by a migrated build
must still load in unmodified OpenTTD.

### Standard exit criteria

Unless a phase says otherwise, all of these must hold before moving on:

1. Builds clean on win64.
2. Game boots, loads every benchmark save, and is playable.
3. Game state fingerprint unchanged from the phase 0 baseline.
4. A save written by the migrated build loads in unmodified OpenTTD.
5. Regression suite green.
6. Benchmark timings recorded against the phase 0 baseline.

## 5. Phases

Risk labels: `CLEAR` = no game-state exposure, cannot desync.
`CAUTION` = touches game state, shadow-verified.
`DANGER` = alters visit order, or deep coupling.

### Phase 0 -- Toolchain and baseline (CLEAR) -- done

No EnTT usage in the game yet, only the ability to use it and the numbers to
judge later phases by.

Done:

- EnTT 3.16.0 added to `vcpkg.json`, wired up with `find_package(EnTT CONFIG
  REQUIRED)` and linked into `openttd_lib`.
- `src/tests/entt_smoke.cpp` proves the toolchain works and pins down the two
  ordering properties from section 3.1. Writing it immediately caught a wrong
  assumption: views iterate in reverse, not creation order.
- Whole-run totals added to `PerformanceData` and exposed via
  `GetPerformanceTotal`, because the existing rolling 512 sample buffer covers
  only the last fraction of a long run.
- `src/benchmark_stats.cpp` writes a report of timings, workload counts and
  object sizes, driven by a new `stats=<path>` parameter on the null video driver.
- `benchmark/run-benchmark.ps1` and `benchmark/bench.cfg` wrap the run, including
  the several ways headless OpenTTD fails silently on Windows.
- `src/state_fingerprint.cpp` provides the per-subsystem game state hash that
  replaces byte-identical savegame comparison, wired into the report and into the
  runner's `-CheckDeterminism` and `-CompareTo` gates.

Baseline, Hilbergen, RelWithDebInfo, 20000 ticks. Note that asserts are still
enabled in this build tree (`OPTION_USE_ASSERTS=ON` applies to every
configuration), so these are not clean release numbers; a separate build tree
with asserts off would be better:

| Figure | Value |
| --- | --- |
| Wall clock | 10.85 s (1843 ticks/s) |
| `GameLoop` total | 8892 ms |
| `GameLoopTrains` | 6966 ms, 78.4% of the game loop |
| `GameLoopLandscape` | 1051 ms, 11.8% |
| `GameLoopEconomy` | 204 ms, 2.3% |
| Train cost per part per tick | 127.6 ns |
| Train cost per consist per tick | 1507.9 ns |
| `sizeof(Vehicle)` / `sizeof(Train)` | 552 / 648 bytes |
| Vehicle parts / consists | 2818 / 231 |

For comparison the same save in Debug ran at 142 ticks/s, roughly fifteen times
slower, with trains at 88% of the game loop. Debug is not a benchmark.

Absolute timings vary by up to 15% between invocations depending on what else the
machine is doing, while the percentage shares stay within about one point. Compare
shares across sessions, and take the best of several runs for absolute figures.

Baseline, wentbourne, RelWithDebInfo, 5000 ticks. A 1024x1024 map in year 3739
with 85,259 vehicle parts across 13,899 consists, 2,263 stations and 1,309
industries:

| Element | Total | Share | ns/part/tick | ns/consist/tick |
| --- | --- | --- | --- | --- |
| `GameLoop` | 257,009 ms | | 602.9 | 3698.2 |
| `GameLoopTrains` | 118,978 ms | 46.3% | 316.5 | 4923.6 |
| `GameLoopRoadVehicles` | 85,176 ms | 33.1% | 3097.9 | 3097.9 |
| `GameLoopShips` | 8,249 ms | 3.2% | 585.4 | 585.4 |
| `GameLoopAircraft` | 2,639 ms | 1.0% | 345.1 | 704.5 |
| `GameLoopLandscape` | 7,044 ms | 2.7% | | |
| `GameLoopEconomy` | 6,563 ms | 2.6% | | |
| **All vehicle ticks** | **215,041 ms** | **83.7%** | **504.4** | |

Vehicle ticks are 84% of the game loop, and the save runs at 19.3 ticks/s against
the roughly 33.3 needed for real time, so it is genuinely at the limit rather than
merely large. Whatever fraction of that 215 s is attributable to memory layout is
what phases 4 and 5 are competing for.

**Read the two normalised columns together, because they disagree.** Per part,
road vehicles look ten times worse than trains (3098 against 317 ns). Per consist
the ranking reverses and trains are the more expensive by 1.6x (4924 against
3098 ns). The parts-per-consist ratios explain it exactly: 15.6 for trains, 1.0
for road vehicles and ships, 2.0 for aircraft, which carry a separate shadow part.
A train consist does its pathfinding and order processing once and spreads the cost
over sixteen parts; a road vehicle bears it alone.

The consequence for this plan is that the two figures answer different questions,
and phases 4 and 5 care about different ones:

- Phase 4 moves hot fields into packed components, which helps whatever is walked
  per part. The 85,259 parts and the per-part column are the relevant measure, and
  trains dominate the part count by an order of magnitude.
- Phase 5 devirtualises per-consist decision work. The per-consist column is the
  relevant measure there, and it says road vehicles are worth as much attention as
  trains despite being a fourteenth of the parts.

Determinism confirmed on this save too: two invocations separated by a rebuild both
produced `state.hash.combined = 29B52DBB6E7D2558`.

**Exit:** standard criteria, plus baseline timings and struct sizes recorded.

### Phase 0 aside: the regression suite was broken on Windows

Worth recording because a phase gate depended on it. All four regression tests
failed with "Regression did not output anything; did the compilation fail?".

The cause was unrelated to any migration work. `cmake/scripts/Regression.cmake`
copies the binary and re-points `OPENTTD_EXECUTABLE` at the bare relative name
`regression_<test>.exe`, but `execute_process` does not resolve a relative
executable name against the working directory, so every launch failed with "no
such file or directory". Because the script does not check `RESULT_VARIABLE`, that
surfaced later as the misleading message above. It is Windows-specific: elsewhere
`EDITBIN_EXECUTABLE` is not found, the copy is skipped, and the original absolute
path is used.

Making the path absolute fixes it, and all four tests now pass in about 22 s.

### Phase 1 -- Registry, identity mapping, lifecycle (CLEAR) -- done

Stand up an `entt::registry` beside `_vehicle_pool` and keep them in lockstep:
create an entity in the `Vehicle` constructor, destroy it in `PreDestructor()`.

Map identity both ways -- a dense `std::vector<entt::entity>` indexed by
`VehicleID` for the forward direction, and a small `VehicleRef { VehicleID }`
component for the reverse.
Do not try to make `entt::entity` values equal `VehicleID`; version bits make
that a trap, and the savegame carries indices anyway.

Establish the canonical-order discipline in this phase, while it is cheap: the
dirty flag, the sort call, and a debug-only assert that a view's first-to-last
order is ascending `VehicleID`.
Getting this invariant in before any data depends on it is the highest-leverage
step in the whole plan.

No data moves yet.

Implemented in `src/vehicle_registry.{h,cpp}`. Notes on what the implementation
settled, since some of it was not obvious from the plan:

- **One constructor hook suffices.** `Pool::CreateAtIndex` does
  `::new (data) T(index, ...)`, so savegame load comes through
  `Vehicle::Vehicle(VehicleID, VehicleType)` like everything else. Destruction hooks
  into `~Vehicle()` *above* its `CleaningPool()` early return, so wholesale pool
  cleaning on new game or load empties the registry too -- `CleanPool` deletes each
  item individually, so the hook runs once per vehicle either way.
- **The registry is a deliberately leaked singleton**, following
  `PoolBase::GetPools()`. Static destruction order across translation units is
  unspecified, so a file-scope registry could be destroyed while a `Vehicle`
  destructor elsewhere is still running during teardown. Leaking one allocation
  removes the hazard rather than reasoning about the ordering.
- **Validation is split by cost.** `SortVehicleRegistry` carries an O(1) assert that
  the entity count matches `Vehicle::GetNumItems()`, which runs on every structural
  change and catches a leak at the moment it happens. The O(n) checks -- identity
  mapping both ways, and ascending iteration order -- live in
  `ValidateVehicleRegistry` and are reported as `ecs.registry_valid` at the end of a
  benchmark run. Running the O(n) checks per tick was not viable: effect vehicles
  churn constantly, so the registry is dirty most ticks, and 85,000 vehicles times
  20,000 ticks is not a debug build anyone would wait for.

**Exit: met.** Entity count equalled `Vehicle::GetNumItems()` and
`ecs.registry_valid` was 1 in every case:

| Case | Vehicles | Entities | Valid |
| --- | --- | --- | --- |
| Hilbergen, 20,000 ticks | 2,818 | 2,818 | yes |
| wentbourne, 5,000 ticks | 85,259 | 85,259 | yes |
| Hilbergen, Debug, 3,000 ticks | 2,812 | 2,812 | yes |
| New game from seed, 3,000 ticks | 10 | 10 | yes |
| Reload of a phase 1 exit save | 2,818 | 2,818 | yes |

Both fingerprints matched the phase 0 baseline exactly -- `015ED3D109C5CCCC` for
Hilbergen and `29B52DBB6E7D2558` for wentbourne -- confirming the phase changed no
behaviour. 102 of 102 tests pass. wentbourne ran at 20.9 ticks/s against a phase 0
baseline of 19.3 to 20.5, i.e. no measurable cost from the per-tick sort call, which
is expected since it early-returns unless the structure changed.

Note that load and new-game both exercise pool cleaning: OpenTTD generates an intro
map before switching to the requested game, so by the time a savegame loads the pools
have already been cleaned and repopulated at least once.

### Phase 2 -- First real components (CLEAR) -- done, re-scoped

This was meant to move `MutableSpriteCache`, `colourmap`, `coord` and `bounds`
into components and win on `ViewportDrawing`. Two things found on contact changed
the phase, and both are worth recording because they invalidate reasoning above.

**`sprite_cache` is not NOSAVE.** It is serialised, unconditionally, in two
descriptor lists:

```c++
SLE_VAR(Vehicle, sprite_cache.sprite_seq.seq[0].sprite, VarFileType::U16 | VarMemType::U32),
/* vehicle_sl.cpp:1018 in SlVehicleEffect, and :1073 in SlVehicleDisaster */
```

Which makes sense in hindsight: effect vehicles (smoke, sparks) and disaster
vehicles have no engine to regenerate a sprite from, so the sprite id *is* game
state. Moving `sprite_cache` therefore has save-format contact and violates the
defining property of this phase, so it is deferred to phase 7 where the staging
struct technique is already on the table.

**The sweeps are cold, so the headline win was illusory.** `ResetVehicleColourMap`
is called from a company colour change, company creation and bankruptcy, and one
settings change. `ResetVehicleHash` is called from `afterload.cpp` and
`InitializeVehicles`. None of them run from the game loop. "Expect a large
multiple on that specific loop" was true and worthless: the loop runs on user
actions, not per tick.

Worse, `colourmap` is a *memoisation cache* read by `GetEngineColourMap` once per
vehicle per draw, so moving it behind an accessor adds indirection to the path
that actually runs and speeds up a path that essentially never does. The phase is
perf-neutral by design, not a win. That is a genuinely useful thing to have
learned early: "move hot fields into components" does not imply "move every
NOSAVE field", and a cache read per draw can be better off as a plain field.

**What was actually done.** `colourmap` moved to a `VehicleColourMap` component
(`src/vehicle_components.h`), which is the exemplar rather than the whole job. The
transferable result is the shape:

- A component holding the data, defaulted so no constructor initialisation is needed.
- An **accessor seam** on `Vehicle` -- `GetColourMap`, `SetColourMap`,
  `InvalidateColourMap` -- so the sixteen call sites do not know where the data
  lives. This is the part that makes later phases cheap: storage can move again
  without touching call sites. All three are `const`, because the value is a cache
  rather than part of the vehicle's identity, which is the same reasoning that
  already made `coord` and `sprite_cache` `mutable`. Two `const_cast`s disappeared
  as a side effect.
- The sweep as `view<T>().each(callback)`, which uses EnTT's positional fast path
  for `swap_and_pop` storage rather than going through entity indirection.
- Eager attachment alongside `VehicleRef`, appropriate because every vehicle needs
  one. A component only some vehicles need should be emplaced on demand instead.

The sweep is deliberately **not** sorted, and says so in a comment: clearing every
element is order-independent, and the component cannot influence game state. This
is the case the phase 0 finding warned about, where EnTT's reversed walk is
visible -- so the comment records that nothing may depend on the order.

**Results.** `sizeof(Vehicle)` fell from 552 to 544 bytes, which is the first
concrete layout win and is deterministic rather than noise.

That is 8 bytes for removing a 4 byte field, which is worth understanding rather
than banking. Measured by re-adding a 4 byte probe in the same slot and diffing
member offsets: `build_year` shifted by 4 as expected, but everything from
`last_loading_tick` onwards shifted by 8. `last_loading_tick` is a
`TimerGameTick::TickCounter`, i.e. 64 bits, so it needs 8 byte alignment -- and
`colourmap`'s presence pushed the run of 4 byte fields before it onto an odd 4
byte boundary, forcing 4 bytes of padding to realign it. Removing `colourmap`
happened to land that run exactly on an 8 byte boundary.

So the field cost 8 bytes: its own 4, plus 4 it forced elsewhere. The general
lesson is that a struct's shrinkage is a function of *where* a field sat, not
just how big it was: moving a different 4 byte field could easily yield nothing
at all. Do not extrapolate per-field savings from this one. Fingerprints were
unchanged on both saves (`015ED3D109C5CCCC`, `29B52DBB6E7D2558`), the registry
stayed valid at 2,818 and 85,259 entities, and 102 of 102 tests pass.

A temporary probe verified the accessor seam does not alias: it wrote a distinct
value to all 2,819 vehicles and confirmed every one read back its own, which is
the failure mode that nothing else in the harness would have caught. It was
removed afterwards, since a stats function should not mutate state.

Drawing timings could not resolve anything. Across phases 0, 1 and 2 the Hilbergen
`Drawing` total was 1516, 2022 and 1782 ms -- and phase 1 changed no drawing code
at all, so a 33% swing there is pure machine noise. No claim either way is
defensible from this harness.

**Deferred, with reasons.** `sprite_cache`, as above.

`coord` (29 sites in 3 files) and `bounds` (47 sites in 7 files) were attempted
and **reverted**. Both are genuinely presentation-only, so the phase's premise
held, and the mechanical part went fine. Two things went wrong, and both are
worth more than the change would have been.

**The destructor read a component after the entity was gone.** `~Vehicle()` calls
`MarkAllViewportsDirty()`, which reads `coord`, but the phase 1 hook released the
entity at the *top* of the destructor. That is an access violation, not a leak,
and it crashed 1.3 seconds into a run. `colourmap` never exposed it because
nothing during teardown reads the colour map.

The fix is to release the entity last, on both the normal path and the
`CleaningPool()` early return, and **that fix has been kept** even though the
components were reverted: it is unconditionally more correct for any future
component that teardown touches, and phase 4 will certainly add one.

**The accessor seam is only cheap when it is called once per operation.**
`UpdateDeltaXY` writes `bounds` field by field -- the road vehicle implementation
alone has about fifteen separate writes -- and it runs for every moving vehicle
every tick. Replacing one field access with a registry lookup *per write* meant a
function call, a vector index, a sparse-set lookup and a dereference, fifteen
times where there had been fifteen offsets from `this`. Measured on Hilbergen at
20,000 ticks:

| | `GameLoop` total | `sizeof(Vehicle)` |
| --- | --- | --- |
| Phase 0 baseline | 7,971 ms | 552 |
| Phase 2, `colourmap` only | 8,948 ms | 544 |
| With `coord` and `bounds` moved | **13,874 ms** | 520 |

That is roughly +74% on the game loop, far outside the 15% noise band, in
exchange for 24 bytes on a struct. Reverted.

**This is the most important finding of the phase, because phase 4 is exactly the
same shape.** `x_pos`, `y_pos`, `z_pos`, `direction`, `cur_speed` and `subspeed`
are all written individually in hot movement code. Giving each a naive accessor
would reproduce this regression precisely, and it would be much harder to spot
there because phase 4 is *expected* to change timings.

Phase 4 must therefore resolve the component reference **once per function** and
work through a local, not once per field access:

```c++
/* Wrong: one registry lookup per write. */
this->GetBounds().extent.x = 24;
this->GetBounds().extent.y = 24;

/* Right: one lookup, then plain member writes. */
SpriteBounds &bounds = this->GetBounds();
bounds.extent.x = 24;
bounds.extent.y = 24;
```

That would very likely have recovered most of the loss here, but recovering a
self-inflicted regression on presentation fields with no upside is not work worth
doing. The lesson transfers for free; the code does not need to.

**Exit: met.** Builds in both configurations, both saves load and play,
fingerprints unchanged, registry valid, regression suite green. The visual
criterion is the one thing this harness cannot check: colour remapping is
presentation, so it is absent from the fingerprint by design. It wants a human
looking at a running game with more than one company livery.

### Phase 3 -- Game-state sweeps under shadow mode (CAUTION)

The same mechanical work, but on fields the simulation reads -- so the canonical
sort becomes load-bearing and shadow mode comes on.

Pick the cheapest sweeps first, the ones that touch one or two fields across the
whole pool, and introduce the shadow-assert scaffolding that the rest of the
migration will reuse.

Discipline to hold: one field group per commit, shadow mode on, benchmark saves
run to completion, byte-identical save verified.
If a save diverges there is a small diff to bisect rather than a rewrite to
unpick.

**Exit:** standard criteria, plus shadow asserts silent over a 50k-tick run on
every benchmark save; two independent runs from the same save produce
byte-identical output saves.

### Phase 4 -- Motion components (CAUTION)

The main event, and the reason for the exercise.

Extract the fields the tick loop actually reads -- `x_pos`, `y_pos`, `z_pos`,
`direction`, `cur_speed`, `subspeed`, `progress`, `tick_counter`,
`motion_counter` -- into one or two tightly packed components.
This is where the ~600-byte stride collapses toward a cache line.

**Resolve the component reference once per function**, not once per field access.
Phase 2 measured a 74% game loop regression from doing the latter with `bounds`,
and the movement code here has exactly the same shape: many individual writes to
`x_pos`, `y_pos`, `direction` and friends inside one hot function. Bind a local
reference at the top and work through it. This is not a micro-optimisation, it is
the difference between the phase being a win and being a large loss.

Sequence it in three commits per field group:

1. Add the component, write both (shadow mode).
2. Flip readers over to the component.
3. Delete the shadow write.

The `Vehicle` fields stay declared throughout, because the save descriptors name
them.
From here they are the staging area described in 3.3, populated at save time
rather than continuously.

**Exit:** standard criteria, plus the four vehicle accumulator timings recorded
against baseline, and the `sizeof(Vehicle)` reduction recorded.

### Phase 5 -- Devirtualising the tick dispatch (DANGER)

Replacing `virtual bool Tick()` plus `switch (v->type)` with typed passes is the
largest structural win available -- and it forces a decision that should be made
deliberately, because the two options differ in kind, not degree.

- **Order-preserving.** Keep one pass in ascending `VehicleID`, replacing virtual
  dispatch with a type tag and a switch.
  Smaller win; continuation from existing saves stays bit-identical.
- **Typed passes.** Four separate views, one per vehicle type.
  Much better locality and branch behaviour -- but it changes the interleaving of
  `Random()` draws, so a save made by stock OpenTTD will no longer continue
  bit-identically.
  The *format* still loads perfectly; the *trajectory* diverges.

For a learning fork the typed version is the more instructive build, and the
divergence is acceptable as long as it is a decision rather than a discovery.
Put it behind a compile-time flag so the two can be A/B'd against the same save.
That comparison is itself one of the more interesting measurements in the
project.

**Exit:** standard criteria, *except* that bit-identical continuation is waived
for the typed build.
Save format must still be byte-identical, both dispatch modes must build and
play, and A/B timings recorded.

### Phase 6 -- Economy and cargo (DANGER)

`GameLoopEconomy` is a genuine cost centre, but the most entangled target here.
`LoadUnloadStation()` walks stations and consists together, and `CargoPacket`
and `VehicleCargoList` are themselves pooled, allocating structures with their
own lifetime rules.
Cargo distribution is also order-sensitive in a way position updates are not --
who loads first changes who gets the cargo.

Attempt this only once phases 3 and 4 have made shadow mode routine.
Consider stopping at cargo *aging* (a clean per-vehicle counter) without
touching cargo *movement*.
Knowing where to stop is part of the exercise.

**Exit:** standard criteria, plus cargo totals and company balances matching a
stock run tick-for-tick under the order-preserving dispatch build.

### Phase 7 -- Hollowing out `Vehicle` (CAUTION)

Consolidation rather than new capability.

One target is already measured. `Vehicle` inherits `BaseConsist`, so all 88 bytes
of it are carried by every part, while its contents -- name, timetable and lateness
counters, depot unbunching timestamps, service interval, current order indices --
are only meaningful on the front vehicle. On wentbourne that is 71,360 of 85,259
parts holding data that means nothing to them: roughly 6.0 MB of the 7.2 MB total,
and 16% of the 552 byte `Vehicle`. Moving it to a component held only by consists
is the clearest single win available here, and `sizeof.BaseConsist` in the
benchmark report tracks it.

Formalise the save facade as an explicit staging struct whose members mirror the
descriptor names exactly, so the coupling between save format and runtime layout
becomes one documented file instead of an implicit constraint on a live class.

What remains in `Vehicle` is identity, cold accounting, and the intrusive chains
that the consist logic legitimately needs.

**Exit:** standard criteria, plus save-format coupling isolated to one
translation unit, and a final `sizeof`/timing table against the phase 0
baseline.

## 6. What to leave alone

**The consist chain.**
Train logic is intra-consist: it walks `First()` and `Next()` constantly.
ECS buys locality *across* entities, which is orthogonal to that access pattern.
Converting the chain adds indirection and buys nothing.

**NewGRF variable resolution.**
`newgrf_engine.cpp` alone contains 201 vehicle field reads in a wide switch over
variable IDs.
It is a cold path per callback, it touches nearly every field, and it is
author-facing API surface.
Extremely high cost, no measurable gain.

**Saveload descriptors.**
The one thing the ground rules forbid changing.
Keep the lists exactly as they are and let the staging struct absorb the layout
change.

**YAPF and the link graph.**
Both already have purpose-built data structures -- YAPF its own node arena, the
link graph its own background job.
Different optimisation problems that an ECS does not address.

## 7. What to actually expect

Worth calibrating now, so the measurements read as success rather than
disappointment.

The cheap sweeps should improve dramatically.
Loops that currently stride a large object to touch four bytes are pure
bandwidth, and packing them is close to a best case.

The main tick loop will improve considerably less.
It does real work per vehicle: chain walking, pathfinder entry, NewGRF
callbacks, sound decisions.
Memory layout is one term in that sum, not the whole of it, so think tens of
percent on the loop rather than a multiple.

A headline framerate number is the wrong goal, because on large saves OpenTTD is
frequently bound by systems this plan deliberately does not touch.
The right goals are the per-element timings from `PerformanceElement`, the
`sizeof(Vehicle)` reduction, and a hot path whose structure can be explained.

For a learning fork the most valuable outcome is not in any of those columns.
Applying ECS to a hostile real-world codebase -- one with a shared RNG, a frozen
wire format, a save compatibility guarantee stretching back twenty years, and a
thousand raw pointers -- teaches far more about where the model actually earns
its keep than any greenfield project can.
The constraints are the curriculum.

## 8. Provenance

Grounded in a read of `src/core/pool_type.hpp`, `src/vehicle_base.h`,
`src/vehicle.cpp`, `src/saveload/saveload.h`, `src/saveload/vehicle_sl.cpp`,
`src/framerate_type.h` and `src/video/null_v.cpp`, against EnTT's
`docs/md/entity.md`.

Scoped for a private fork.
OpenTTD's [CONTRIBUTING.md](../CONTRIBUTING.md) declines AI-generated
contributions upstream, and nothing here is intended for the main repository.
