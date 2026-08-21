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

**The order changed after phase 4.** The save staging mechanism was originally
bundled into the last phase as consolidation; it is really infrastructure that
phases 4 and 6 both depend on, so it became phase 5 and everything after it moved
down one. Two things forced it: commit 3 of a field migration is impossible for a
serialised field without staging, and every field phase 4 still has to move is
serialised -- and a view-iterating tick loop cannot show a benefit while it still
has to write back to authoritative members on a 560-byte `Vehicle`.

Incidentally the original order also put a `DANGER` phase before a `CAUTION` one,
against the risk-ascending principle the rest of the plan follows. The new order
fixes that as a side effect. Current sequence:

| Phase | Risk | Status |
| --- | --- | --- |
| 0 -- Toolchain and baseline | CLEAR | done |
| 1 -- Registry, identity, lifecycle | CLEAR | done |
| 2 -- First real components | CLEAR | done, re-scoped |
| 3 -- Shadow mode | CAUTION | done, re-scoped |
| 4 -- Motion components | CAUTION | first increment done, blocked on 5 |
| 5 -- Save staging | CAUTION | next |
| 6 -- Devirtualising the tick dispatch | DANGER | |
| 7 -- Economy and cargo | DANGER | |
| 8 -- Hollowing out `Vehicle` | CAUTION | |

Before phase 5 there is one step available that needs nothing new: `vcache` is not
serialised, so its commits 2 and 3 can be finished now, deleting the member and
removing its dual write. It is also the only step currently available that
*reduces* cost rather than adding it.

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

Baseline, Hilbergen, RelWithDebInfo, 20000 ticks. **These figures were taken with
asserts enabled and have since been superseded** -- see "Asserts were distorting
the baseline" below. They are kept because the phase 1 and 2 comparisons were made
against them.

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
what phases 4 and 6 are competing for.

**Read the two normalised columns together, because they disagree.** On the
corrected asserts-off figures, per part road vehicles look about eight times worse
than trains (1986 against 261 ns), while per consist the ranking reverses and
trains are 2.05x the more expensive (4061 against 1986 ns). The parts-per-consist
ratios explain it exactly: 15.6 for trains, 1.0 for road vehicles and ships, 2.0
for aircraft, which carry a separate shadow part. A train consist does its
pathfinding and order processing once and spreads the cost over sixteen parts; a
road vehicle bears it alone.

The consequence for this plan is that the two figures answer different questions,
and phases 4 and 6 care about different ones:

- Phase 4 moves hot fields into packed components, which helps whatever is walked
  per part. The 85,259 parts and the per-part column are the relevant measure, and
  trains dominate the part count by an order of magnitude.
- Phase 6 devirtualises per-consist decision work, so the per-consist column
  governs. Trains lead there too, by a factor of two. Road vehicles remain the
  clear second target -- 28% of the game loop from a fourteenth of the parts -- but
  they are not the near-tie that the assert-on numbers implied.

Determinism confirmed on this save too: two invocations separated by a rebuild both
produced `state.hash.combined = 29B52DBB6E7D2558`.

**Exit:** standard criteria, plus baseline timings and struct sizes recorded.

### Asserts were distorting the baseline

`OPTION_USE_ASSERTS` is a CMake cache variable and applies to every configuration
in a tree, so the original `build` tree had asserts on even in `RelWithDebInfo`.
A second tree, `build-release`, was configured with `OPTION_USE_ASSERTS=OFF`
before starting phase 3, since everything from here depends on timings.

That did not merely produce tidier numbers, it corrected a headline figure.
Hilbergen, 20,000 ticks, same commit:

| Figure | Asserts on | Asserts off | Change |
| --- | --- | --- | --- |
| Ticks per second | 2,082 | 2,505 | +20% |
| `GameLoop` total | 7,971 ms | 6,610 ms | -17% |
| `GameLoopTrains` total | 6,354 ms | 4,636 ms | **-27%** |
| Trains, ns per part per tick | 116.4 | 84.9 | **-27%** |
| Trains as share of game loop | 79.7% | **70.1%** | -9.6 pp |

So the "trains are 79% of the game loop" figure from phase 0 was partly an
artefact of assertion checking. The true share is around 70% -- still dominant,
but nearly ten points lower, and the per-part cost that phases 4 and 6 are trying
to beat is 85 ns rather than 116. Chasing the inflated number would have made any
improvement look better than it was.

wentbourne, 5,000 ticks, tells a subtler story. The *total* vehicle share barely
moved, but the split between types did:

| Element | Asserts on | Asserts off | Change |
| --- | --- | --- | --- |
| Ticks per second | 20.5 | 25.4 | +24% |
| `GameLoop` total | 243,896 ms | 195,001 ms | -20% |
| `GameLoopTrains` | 118,978 ms (46.3%) | 98,138 ms (**50.3%**) | -17.5% |
| `GameLoopRoadVehicles` | 85,176 ms (33.1%) | 54,592 ms (**28.0%**) | **-36%** |
| `GameLoopShips` | 8,249 ms (3.2%) | 7,087 ms (3.6%) | -14% |
| `GameLoopAircraft` | 2,639 ms (1.0%) | 2,340 ms (1.2%) | -11% |
| **All vehicle ticks** | 215,041 ms (83.7%) | 162,157 ms (**83.2%**) | -25% |

Assertion checking was more than twice as expensive in road vehicle code as in
train code, -36% against -17.5%. So the four vehicle elements were not merely
inflated, they were inflated *unevenly*, and any prioritisation based on the
assert-on split was skewed.

The per-denominator figures move accordingly, and this is the part that changes a
recommendation made earlier in this document:

| Group | ns/part, on | ns/part, off | ns/consist, on | ns/consist, off |
| --- | --- | --- | --- | --- |
| Trains | 316.5 | 261.1 | 4923.6 | 4061.2 |
| Road vehicles | 3097.9 | 1985.5 | 3097.9 | 1985.5 |

Per consist, trains are now **2.05x** the cost of road vehicles rather than 1.6x.
The earlier claim that road vehicles "deserve as much attention as trains" for
phase 6 was reading assert overhead as vehicle cost. Trains dominate on both
denominators; road vehicles are second by a clear margin, not a near tie.

The fingerprint was **identical** across both trees on both saves
(`015ED3D109C5CCCC`, `29B52DBB6E7D2558`), which is the empirical confirmation that
asserts do not change game logic and that `-CompareTo` is valid between the two
trees.

Which tree to use:

| Tree | Asserts | Purpose |
| --- | --- | --- |
| `build` | ON | Correctness: shadow-mode assertions, registry invariants, the test suite. |
| `build-release` | OFF | Timings: every number that goes in a results table. |

`benchmark/run-benchmark.ps1` now defaults to `build-release` and takes a
`-BuildDir` parameter for the other. Report filenames include the tree name,
because the timings are not interchangeable.

One trap worth recording: a fresh build tree has no graphics set, because CMake
only installs the basesets that ship with the source. Copy
`baseset/opengfx-*.tar` across from an existing tree or the game will not start.

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
defining property of this phase, so it is deferred to phase 8, which is where the
cleanup that depends on the staging struct belongs.

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

### Phase 3 -- Shadow mode (CAUTION) -- done, re-scoped

The premise here was "pick the cheapest game-state sweeps first". That premise does
not hold, for the same reason phase 2's did not: grepping every full-pool loop that
writes a saved field shows they live almost entirely in `afterload.cpp` and
`vehicle_sl.cpp`, i.e. one-time savegame migration. `CallVehicleTicks` is the only
hot full-pool loop in the game, and converting *that* is phase 4 and 5 work.

So this phase delivered its durable half instead: **the shadow-mode scaffolding**,
in `src/ecs_shadow.{h,cpp}`, demonstrated on `VehicleCache`.

`VehicleCache` was chosen because it is the one field group that is
game-state-affecting but *not* serialised -- `cached_max_speed` drives movement and
`cached_cargo_age_period` drives cargo ageing, while the single mention of `vcache`
in `vehicle_sl.cpp` is a read during afterload rather than a descriptor. That makes
it a genuine CAUTION target without also needing the save-staging machinery, which
can wait for phase 4 where the motion fields really are saved.

**What shadow mode actually verifies here.** Writes still go to the `vcache` member,
with a `SyncVehicleCache()` call after each one, and reads go through an accessor
that compares the two copies. So the check does not verify a computation -- the
component is a copy -- it verifies **coverage**: if a write site was missed, the
component goes stale and the next read reports a mismatch. With 47 usage sites
across 9 files, "did I find all 11 writes" is the actual risk.

**Counting beats asserting.** An assert stops at the first divergence and tells you
only that something is wrong. A counter lets the run finish and reports mismatches
against total comparisons, which says how *often*, and that usually suggests why.
The counts surface as `shadow.<name>.mismatches` in the benchmark report, so the
exit criterion is checked rather than asserted. Note that a comparison count of
zero is not a pass: it means shadow mode was compiled out, which is the normal
state of the release tree.

**Results.** Zero mismatches everywhere:

| Run | Comparisons | Mismatches |
| --- | --- | --- |
| Hilbergen, Debug, 20,000 ticks | 54,614,511 | 0 |
| wentbourne, Debug, 500 ticks | 42,564,438 | 0 |

The wentbourne run matters more than its tick count suggests: coverage comes from
vehicle diversity rather than run length, and it exercises all four types at once
(4,833 train, 5,499 road, 2,818 ship and 749 aircraft consists). Fingerprints
matched the baseline on both saves, and the Hilbergen match was between a *Debug*
run and a *release-tree* baseline, which is further confirmation that the
fingerprint is build-independent.

**It costs 2.4%, and that is measurable rather than noise.** Repeated 20,000-tick
Hilbergen runs on a quiet machine:

| Set | `GameLoop` samples | Median |
| --- | --- | --- |
| Before | 6610.3, 6579.9 ms | 6595 ms |
| After | 6772.5, 6734.9, 6746.3 ms | 6751 ms |

Within-set spread is about 0.3%, so +2.4% is a real effect. Shadow mode is compiled
out of the release tree, so this is not verification overhead -- it is the accessor
indirection itself, one registry lookup per vehicle per tick in `CallVehicleTicks`
plus one in `GetAcceleration`.

That is the cost of the seam, measured cleanly, and it is the number phase 4 has to
beat. Nine motion fields read several times per tick cannot pay for themselves
through accessors alone; the win has to come from converting the tick loop to
iterate views, so that the lookup is amortised across a packed walk instead of
repeated per vehicle. Phase 3 quantifies what phase 4 is up against.

Two details worth carrying forward. First, **the cost is larger on the stress save,
but its magnitude there is not yet pinned down.** Three wentbourne samples came in
at 206.9, 211.8 and 228.2 seconds against a single 195.0 second baseline, so the
direction is consistent -- every sample is slower -- but the spread across those
three is 10.4%, which is far too wide to quote a figure from. Somewhere between 6%
and 17%, and it needs interleaved sampling to narrow.

That spread is itself the finding: **long runs are much noisier than short ones**,
which inverts the usual intuition that a longer run averages noise out. Hilbergen
takes nine seconds and repeats to 0.3%; wentbourne takes three and a half minutes
and repeats to 10%, climbing monotonically across consecutive runs in a way that
looks like thermal throttling. Short runs for precision, long runs for coverage,
and interleave A/B samples rather than running all of one then all of the other.

Second, **the cost landed outside the vehicle accumulators**, which is initially
confusing: on wentbourne `GameLoop` rose while `all_vehicles.pct_of_game_loop` fell
from 83.2% to 79.7%. That is consistent rather than contradictory. The cargo-ageing
block in `CallVehicleTicks` sits inside the full-pool loop but outside the per-type
`PerformanceAccumulator` scopes, so work added there is counted in `GameLoop` and
not in `GameLoopTrains` and friends. A share moving in the unexpected direction is
worth chasing down rather than shrugging at -- here it confirmed the cost was
exactly where the change was.

**Also corrected: the noise band.** This document previously said absolute timings
vary by up to 15%. That was measured with compiles running in parallel and is badly
wrong as guidance -- it would have dismissed this 2.4% regression. On a quiet
machine repeatability is around 0.3%, and the rule is three samples and a median,
discarding the first run after a build, which is consistently slow from cold caches.

**Exit: met.** Shadow mismatches zero on both saves, fingerprints unchanged,
registry valid, both configurations build, 102 of 102 tests pass. The `vcache`
member is deliberately still present: shadow mode requires both copies, and
deleting it is the follow-up once the remaining read sites move over.

#### The original phase 3 text, kept for the record

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

### Phase 4 -- Motion components (CAUTION) -- first increment done

Scoped down on contact. The nine fields the plan names come to roughly 866 usage
sites across 24 files, and because the once-per-function rule requires judgement at
each site rather than a rename, that is not one change. Position alone
(`x_pos`, `y_pos`, `z_pos`) is about 530 sites.

The first increment migrates `subspeed` and `motion_counter` -- 20 sites, chosen
for the best measurement signal per unit of risk, since both are touched once per
vehicle per tick in the hottest paths there are (`GroundVehicle::DoUpdateSpeed` and
`CallVehicleTicks`). `progress` was deferred: 52 sites of which 35 are effect
vehicle tick handlers, which are 231 of wentbourne's 85,259 vehicles and therefore
almost no measurement value for a lot of churn.

#### The measured regression

Three samples each on a quiet machine, 20,000 ticks of Hilbergen:

| Build | `GameLoop` samples | Median | Against baseline |
| --- | --- | --- | --- |
| Baseline | 6610.3, 6579.9 ms | 6595 ms | |
| First attempt | 7375.2, 7367.9, 7358.7 ms | 7368 ms | +11.7% |
| After fixing a bug of mine | 7231.0, 7201.4, 7181.8 ms | 7201 ms | **+9.2%** |

wentbourne came in at 209,862 ms against a 195,001 ms baseline, or +7.6%. That is a
single sample against a single sample on a fixture whose repeatability is only
around 10%, so it is consistent with the Hilbergen figure rather than independent
confirmation of it. Both saves passed the fingerprint gate.

The bug is worth recording. `VerifyMotion` left its body unguarded, so although
`ShadowVerify` compiles to nothing in a release build, the registry lookup feeding
it did not -- a release build was paying for verification it was not performing.
That was about a fifth of the apparent regression. **Guard the whole body, not just
the comparison.**

So the honest figure is **+9.2% for two fields**, against phase 3's 2.4% for three.
The prediction recorded before measuring was 5% to 20% for all nine fields; the
outcome lands inside that range having migrated two of them, which means the cost
model was wrong -- per-field cost is far higher than a linear scaling from phase 3
suggested.

Two reasons, and the second matters for how the figure should be read:

- Phase 3's `vcache` accessor was **read-only**. These fields are written, so
  `SyncMotion` dirties a cache line in the component in addition to the one in
  `Vehicle`. Two dirty lines per vehicle per tick rather than one clean read.
- **Shadow mode overstates the end state.** While both copies exist every write
  happens twice. When the members are eventually deleted the dual write goes away,
  so the steady-state cost of the seam is lower than 9.2%. This measurement is the
  cost of *migrating*, not the cost of *having migrated*, and the two should not be
  conflated when judging phase 6.

#### Savegame load bypasses every accessor

The shadow check earned its keep immediately, and not in the way expected. It
reported 462 mismatches out of 60.9 million comparisons -- a small, non-zero
number, which is the most informative kind.

The cause: **savegame load writes the members directly through the save
descriptors**, whose address getters reach into `Vehicle` without going near an
accessor or a sync. So every migrated component starts out stale after a load. No
amount of grepping for `->subspeed =` finds that write, because it is not written
as one. Fixed by scattering into the components in `AfterLoadVehiclesPhase2`, which
already sweeps every vehicle; this is the load half of the staging arrangement in
3.3, arriving earlier than the plan expected.

#### And a mistake the check caught before it shipped

The first attempt conflated commits 1 and 2 of this phase's own three-commit
sequence. Read-modify-write sites were converted to read the *component* and write
the *member*:

```c++
v->motion_counter = v->GetMotion().motion_counter + front->cur_speed; /* wrong */
```

If the component is stale for any reason, that stale value propagates into
authoritative state: a reportable coverage gap becomes silent corruption, and the
safety net becomes the bug. The fingerprint duly changed on all nine values.

The rule the three-commit sequence encodes, made explicit: **while writes are still
authoritative on `Vehicle`, read sites must read the member and merely *verify* the
component.** Hence `VerifyMotion()` alongside `GetMotion()` -- verification that
returns nothing, so it cannot be accidentally consumed. Reads flip to the component
only after a long run reports zero mismatches.

**Exit: met for this increment.** Shadow mismatches zero on both checks,
fingerprints unchanged, registry valid, both configurations build, 102 of 102 tests
pass, and the regression measured, stated and explained.

#### What is left, and what blocks it

Only commit 1 of the three-commit sequence is done, for this field group. Commit 2
flips reads to the component; commit 3 deletes the member. Both are outstanding,
and `GetMotion()` currently exists but is called nowhere -- it is the commit 2 tool
waiting to be used.

Commit 2 is safe **only at sites where verification already runs**, which is three
sites for motion and two for `vcache`. Zero mismatches across a hundred million
comparisons proves the copies agree *at those points*, not everywhere: a write site
missed and then corrected before the next verification point is invisible to the
check. Extend `Verify` coverage to any site before flipping it.

Commit 3 is blocked on the staging mechanism, now phase 5, because `subspeed` and
`motion_counter` are serialised. The same applies to every field this phase still
has to move -- `progress`, `cur_speed`, `direction`, and the three position fields
are all serialised -- so **phase 4 cannot be completed before phase 5**.

`sizeof(Vehicle)` went from 544 to 560 bytes, as it must while both copies exist.

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

### This phase is expected to be slower, deliberately

Phase 3 measured the accessor seam at 2.4% for one small struct read once or twice
per vehicle per tick. Nine motion fields, read and written repeatedly through the
movement code, will cost more. Moving fields into components does nothing for
locality while the loop still walks `Vehicle::Iterate()` and reaches into the
registry per vehicle -- the packed walk that pays for it is phase 6's job.

The obvious response is to merge phases 4 and 6 so the net effect is never
negative. **We are deliberately not doing that.** Keeping them separate makes each
half independently measurable: phase 4 answers "what does the seam cost?" and phase
5 answers "what does packed iteration buy?". Merged, only the sum is observable,
and if the sum disappoints there is no way to tell which half was responsible.
The regression is the measurement, not a mistake.

Two consequences for how this phase is judged:

- **A slower result is a pass, not a failure.** The exit criteria below record the
  regression rather than requiring its absence. What would be a failure is a
  changed fingerprint, a shadow mismatch, or a regression far outside the range
  below, which would suggest the per-function rule was broken somewhere.
- **The prediction should be written down before measuring.** Scaling phase 3's
  2.4% by the number of lookups per vehicle per tick suggests somewhere between 5%
  and 20% on Hilbergen, wide because it depends on how many functions in the
  movement path end up resolving a reference. Recording the guess first makes the
  result informative either way: well outside that range in either direction means
  the cost model is wrong and worth understanding before phase 6 obscures it.

**Exit:** builds in both configurations, both saves load and play, fingerprints
unchanged, shadow mismatches zero, regression suite green, `sizeof(Vehicle)`
reduction recorded, and the four vehicle accumulator timings recorded against
baseline **with the regression stated explicitly** and compared against the
predicted range.

### Phase 5 -- The save staging mechanism (CAUTION)

**This phase was moved forward from what used to be phase 7.** It was originally
bundled with the hollowing-out work as "consolidation", but it is not
consolidation, it is infrastructure that two later phases depend on. Discovering
that is what prompted the reordering; the reasoning is recorded below because a
reader coming to this document fresh will otherwise wonder why staging sits in the
middle.

Constraint 3.3 explains the mechanism: `SLE_VAR(Vehicle, x_pos, …)` names a
member, so a field cannot leave `Vehicle` while a descriptor reads it. The answer
is a staging struct whose members mirror the descriptor names exactly, gathered
from components before a save and scattered back after a load. The load half
already exists, in `AfterLoadVehiclesPhase2` -- phase 4 needed it early, because
savegame load writes members straight through the descriptors and leaves every
migrated component stale.

#### Why it has to come before the tick loop is converted

Commit 3 of a field migration -- deleting the member -- is impossible for a
serialised field without this. And every field phase 4 still has to move is
serialised: `progress`, `cur_speed`, `direction`, and the three position fields.
So phase 4 cannot finish at all until this lands, and until it does, every
migrated field stays in dual-write mode paying the cost measured in phase 4.

More sharply, **phase 6 cannot demonstrate its benefit until commit 3 is done.**
A view-iterating tick loop reads packed components, which is the point -- but
while the members are still authoritative it must also write back to them, and
that write-back touches the scattered 560-byte `Vehicle` for every entity. That is
exactly the access pattern the packed walk exists to remove. Converting the loop
first would measure packed reads plus scattered writes plus dual-write overhead,
and produce a number that means very little.

#### A cheap way to de-risk this ordering first

`vcache` is **not** serialised, so its commit 3 is available immediately: move its
remaining reads and delete the member, no staging required. It is also read-only
in the tick loop, so a view over `VehicleCacheComponent` can be walked with no
write-back at all.

That makes a partial version of phase 6 possible before this phase: a clean, if
narrow, measurement of whether packed iteration pays off. Worth doing first if the
ordering argument above should be tested rather than trusted -- it is a day's work
against committing to staging on the strength of an argument.

**Exit:** standard criteria, plus a save written by the staged build loads in
unmodified OpenTTD, `subspeed` and `motion_counter` deleted from `Vehicle` with
shadow mode removed for them, and the resulting `sizeof(Vehicle)` and timing
change recorded -- this is the phase that should give back part of phase 4's 9.2%.

### Phase 6 -- Devirtualising the tick dispatch (DANGER)

This is where phase 4 gets paid for. Phase 4 moves the fields and accepts a
regression; this phase converts the loop to iterate views, so the registry lookup
is amortised across a packed walk rather than repeated per vehicle. Judge the two
together as well as separately: the pair is only worthwhile if phase 6's gain
exceeds phase 4's loss, and knowing both halves is the point of having run them
apart.

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

### Phase 7 -- Economy and cargo (DANGER)

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

### Phase 8 -- Hollowing out `Vehicle` (CAUTION)

Consolidation rather than new capability.

One target is already measured. `Vehicle` inherits `BaseConsist`, so all 88 bytes
of it are carried by every part, while its contents -- name, timetable and lateness
counters, depot unbunching timestamps, service interval, current order indices --
are only meaningful on the front vehicle. On wentbourne that is 71,360 of 85,259
parts holding data that means nothing to them: roughly 6.0 MB of the 7.2 MB total,
and 16% of the 552 byte `Vehicle`. Moving it to a component held only by consists
is the clearest single win available here, and `sizeof.BaseConsist` in the
benchmark report tracks it.

The staging mechanism that used to live here is now phase 5, because phases 4 and
6 both depend on it. What remains here is the cleanup it enables: `sprite_cache`,
deferred from phase 2 because part of it is serialised for effect and disaster
vehicles, and `BaseConsist` above.

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
