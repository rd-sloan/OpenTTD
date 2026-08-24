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
Once sorted the storage is contiguous in ID order, so the locality you came for is
preserved.

> **Unverified assumption, flagged.** This section originally continued "structural
> churn is rare next to the tick rate, so this amortises to nearly nothing". That
> has never been measured, and it is load-bearing: it is the entire reason to expect
> a sorted registry to be affordable. A sort is O(n log n) on the live set, around
> 1.4 million comparisons on wentbourne's 85,000 parts, so if the dirty flag fires
> every few ticks the sort could swamp the locality win it exists to enable.
> Phase 6 measures the sort frequency directly before either dispatch variant is
> built. Given how the other confident-sounding claims in this document have fared
> against measurement, treat the amortisation as an open question rather than a
> premise.

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
4. Save-resume equivalence: N ticks then save, reload and run M more, gives the
   same fingerprint as N+M ticks run straight through.
5. A save written by the migrated build loads in unmodified OpenTTD.
6. Regression suite green.
7. Benchmark timings recorded against the phase 0 baseline.

Criterion 4 is newer than the others. It was identified while planning phase 6, which
is the first phase able to violate it, and is listed here rather than there because it
applies to anything touching visit order: criterion 3 runs the same save twice from a
fresh load, so it cannot detect an order that depends on session history.

**It is currently unenforceable.** Phase 5 implemented it as `-CheckResume` and found
that it fails on unmodified code, because a game resumed from an autosave-on-exit save
does not evolve deterministically -- measured on a build predating the migration. See
phase 5 for the evidence and phase 6 for what unblocking it requires. Until then this
criterion is aspirational, and that gap is itself the most interesting thing the
verification harness has turned up since the fingerprint replaced byte comparison.

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
| 4 -- Motion components | CAUTION | **all fields migrated**; +37%, ships/disasters deferred |
| 5 -- Save staging | CAUTION | done -- no staging struct needed |
| 6 -- Devirtualising the tick dispatch | DANGER | both dispatch variants, A/B'd |
| 7 -- Economy and cargo | DANGER | |
| 8 -- Hollowing out `Vehicle` | CAUTION | |

#### The agreed working sequence

Phase 5, then finish phase 4, then phase 6 running **both** dispatch variants for
comparison. Two things are worth recording about that order.

**It is forced, not chosen.** Every one of the nine motion fields is serialised --
checked against the descriptor lists rather than assumed:

| Field | Descriptor sites in `src/saveload/` |
| --- | --- |
| `x_pos`, `y_pos`, `z_pos` | 6 each |
| `progress` | 3 |
| `direction`, `tick_counter` | 2 each |
| `cur_speed`, `subspeed`, `motion_counter` | 1 each |

So commit 3 is impossible for all nine without staging, and until commit 3 lands
each migrated field pays dual-write cost. There is no ordering that gets phase 4
finished before phase 5.

**Phase 5 is also the first honest test of whether commit 3 gives anything back.**
Finishing `vcache` showed that deleting a member is not automatically a win: its
dual writes were cold and its bytes vanished into padding. The motion fields differ
on the first count -- `subspeed` and `motion_counter` are written *per tick* -- so
the mechanism that was absent for `vcache` is genuinely present here. That makes
the two-field commit 3 immediately after phase 5 a cheap, well-isolated measurement
before committing to the other seven field groups. Take it before doing the bulk
work, not after.

`vcache` is now fully migrated: it is not serialised, so its commits 2 and 3 needed
nothing from phase 5 and are done. It is the first field group to complete all three
commits and have its original member deleted.

This document previously predicted that finishing `vcache` would *reduce* cost
rather than add it. **That prediction was wrong, and the reason is instructive
enough to be worth stating up front: it never named a mechanism.** Measurement
found that neither of the two mechanisms a saving would have required actually
exists here -- see "Why finishing `vcache` did not pay off" below.

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
wrong as guidance -- it would have dismissed this 2.4% regression.

**Corrected again, later, in the other direction.** The replacement figure of "about
0.3% on Hilbergen, three samples and a median" was itself derived from two baseline
runs that happened to land 0.5% apart. Two samples cannot estimate a spread; they
can only ever produce one gap, and a small gap looks like precision. Eight
consecutive Hilbergen runs while finishing `vcache` spanned **6.8%** -- 7,153 ms to
7,637 ms -- with no code change between them.

The pattern across all the batches taken so far is that each batch is internally
tight (0.5% to 0.9%) but batches sit at different levels, which is what thermal
state and background activity produce. Tight-within-batch is therefore not evidence
of precision, and it is actively misleading: it invites quoting a three-sample
median to four significant figures.

Revised working rule:

- **At least five samples** before quoting a figure; three only for a change big
  enough that the sign is obvious.
- **Compare minima, not medians.** The minimum is the least-contaminated sample and
  is the standard robust statistic for this kind of benchmark; a median tracks the
  batch's thermal level as much as the code.
- Discard the first run after a build (cold caches).
- **Treat anything under about 7% on Hilbergen as unproven** without interleaved
  A/B sampling.

Applied backwards, this weakens some earlier numbers but does not overturn any:
phase 3's +2.4%, phase 4's +9.2% and the phase 2 `bounds` +74% were each measured
within a batch against a baseline from a different batch. Phase 2's +74% and phase
4's roughly +9% are far enough outside the band to stand. Phase 3's +2.4% is inside
it and should now be read as "small, sign probably right, magnitude unproven".

**Exit: met.** Shadow mismatches zero on both saves, fingerprints unchanged,
registry valid, both configurations build, 102 of 102 tests pass. The `vcache`
member was deliberately still present at this point, because shadow mode requires
both copies; it has since been deleted, in phase 4's `vcache` completion.

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

For the motion fields, only commit 1 of the three-commit sequence is done. Commit 2
flips reads to the component; commit 3 deletes the member. Both are outstanding,
and `GetMotion()` currently exists but is called nowhere -- it is the commit 2 tool
waiting to be used.

Commit 2 is safe **only at sites where verification already runs**. Zero mismatches
across a hundred million comparisons proves the copies agree *at those points*, not
everywhere: a write site missed and then corrected before the next verification
point is invisible to the check. Extend `Verify` coverage to any site before
flipping it. Finishing `vcache` turned this from a caution into a demonstrated bug
-- see the next section.

Commit 3 is blocked on the staging mechanism, now phase 5, because `subspeed` and
`motion_counter` are serialised. So is every other field this phase has to move --
all nine, verified against the descriptor lists -- so **phase 4 cannot be completed
before phase 5**.

#### All nine fields are now migrated -- and it cost 90%

Every field this phase set out to move is out of `Vehicle`. Two components hold them:
`VehicleMotion` (`cur_speed`, `subspeed`, `motion_counter`, `tick_counter`, `progress`,
`direction`) and `VehiclePosition` (`x_pos`, `y_pos`, `z_pos`). Grouping is by *what a
hot loop reads together*, not by what the fields mean -- `tick_counter` is a generic
counter, but `CallVehicleTicks` reads it in the same few lines as `motion_counter`, so
one lookup serves both. Position is separate because a great deal of code wants
coordinates and nothing else.

Correctness held at every step. Each field group was gated independently, and the
fingerprint never moved: `3C94DD1E5614C300` on the 3,000-tick Debug gate and
`015ED3D109C5CCCC` on the 20,000-tick release fixture, with 102 of 102 tests passing
throughout.

`sizeof(Vehicle)` fell **544 → 528** in release, 560 → 544 in Debug. The first two
groups changed nothing, as expected from the padding finding; `progress` crossed a
boundary at 560 → 552, and the twelve contiguous bytes of position took it the rest of
the way. That confirms the rule from phase 5 -- small scalars vanish into padding, a
large contiguous block does not.

**And the game loop went from 6,580 ms to 12,529 ms. Ninety percent slower.**

| State | Minimum | Samples |
| --- | --- | --- |
| Phase 0 baseline | 6,580 ms | 2 |
| Phase 5 (`subspeed`, `motion_counter`) | 7,473 ms | 6 |
| + `tick_counter`, `progress`, `cur_speed`, `direction` | 9,537 ms | 5 |
| + `x_pos`, `y_pos`, `z_pos` | 12,529 ms | 3 |

This is far outside the phase's predicted 5--20% and outside any noise argument -- the
last batch spread 0.3%. It is a real result and it needs stating plainly rather than
filed under "phase 4 is expected to be slower".

**The cause is known, and it is a deliberate shortcut that now has to be paid back.**
Each field group was converted mechanically, rewriting every `v->x_pos` into
`v->GetPos().x_pos` one site at a time. That was chosen on purpose: a per-site rewrite
is correct regardless of how `v`, `front`, `this` and `consist` alias each other in the
movement code, whereas hoisting a reference requires proving object identity at every
site. Correctness first, with roughly 850 sites to convert and only the fingerprint as
a net.

The bill for that choice is exactly what phase 2 measured on `bounds` and what this
document has warned about since: **the accessor resolved per access rather than per
function**. Three opaque calls per field access, none of them inlinable across
translation units:

1. `GetVehicleRegistry()` -- a call returning a function-local static;
2. `GetVehicleEntity(index)` -- another call, guarded static plus a bounds check plus a
   vector index;
3. `registry.get<T>()` -- the sparse-set lookup itself, a page index plus a packed index.

None of it can be common-subexpression-eliminated, because the calls are opaque to the
optimiser. `direction` alone doubled the Debug run, which is unsurprising once counted:
86 `v->direction` sites plus the sprite path.

**The repair, in order of expected value:**

1. **Make the two registry lookups inlinable.** Move `VehicleRegistryData` into
   `vehicle_registry.h`, expose the singleton as a pointer, and make
   `GetVehicleRegistry()` and `GetVehicleEntity()` inline. This alone turns three calls
   into zero and lets the optimiser see the whole chain.
2. **Cache the entity handle on `Vehicle`.** Four bytes buys the removal of the
   `VehicleID → entity` indirection on every access. It gives back a little of the
   `sizeof` win, which is the right trade if it buys back tens of percent.
3. **Then hoist by hand**, in the functions that remain hot after 1 and 2 -- guided by
   measurement rather than by grepping for multiple accesses.

Doing 1 and 2 before hand-hoisting is deliberate: they are small, surgical and
measurable, and they change how much hand-hoisting is worth doing at all. Hoisting
hundreds of sites first would be the expensive way to discover that the constant factor
was the real problem.

#### The repair, measured: +90% down to +42%

All three steps done, measured separately.

| Step | Hilbergen minimum | Change |
| --- | --- | --- |
| Per-site access, out-of-line accessors | 12,529 ms | -- |
| + inline registry lookups, cached entity handle | 10,136 ms | **-19%** |
| + hoisting in the train and shared hot paths | 9,326 ms | **-8%** |
| + hoisting aircraft and road vehicles | 9,017 ms | drift, see below |

Against the 6,580 ms baseline that is +37%, down from +90%. The last row is not an
improvement to Hilbergen -- that fixture is trains-only, so aircraft and road vehicle
changes cannot affect it, and the 3.3% is machine drift. It is listed because that drift
is what makes the wentbourne numbers below readable.

Correctness held throughout: fingerprints unchanged on both fixtures
(`015ED3D109C5CCCC`, `29B52DBB6E7D2558`) and 102 of 102 tests at every step.

**Step 1, inlining the lookups.** `VehicleRegistryData` moved into
`vehicle_registry.h`, the singleton is now an `extern` pointer, and
`GetVehicleRegistry()` / `GetVehicleEntity()` are inline. The lazy initialisation is
kept -- a null check rather than a dynamic initialiser -- because the reason for the
laziness is a static *destruction* order hazard, and a namespace-scope initialiser would
trade it for a static *initialisation* order hazard. One perfectly predicted branch is
far cheaper than a guarded static behind a call.

**Step 2, caching the entity handle.** `Vehicle` stores its own `entt::entity`, returned
by `RegisterVehicleEntity`, so component access skips the `VehicleID → entity` vector
lookup entirely. The handle is stable for the vehicle's whole life: sorting the registry
permutes *components*, not entity identifiers. `sizeof(Vehicle)` went back up 528 → 536,
four bytes plus padding, which is the right trade at this magnitude -- the regression was
overwhelmingly indirection, not storage.

Together these removed three opaque calls per field access and left one sparse-set
lookup. That is the 19%.

**Step 3, hoisting.** Partial and deliberately targeted: `train_cmd.cpp`, `vehicle.cpp`,
`ground_vehicle.hpp` and `vehicle_base.h`, the paths Hilbergen actually exercises since
it is a trains-only save. The densest win was `GroundVehicle::UpdateZPosition` and its
inclination sibling, with about twenty accesses to the same component across two
per-tick functions. Two spots also had a redundant double lookup left over from the
mechanical pass, where a hoisted reference sat immediately after a `GetMutableMotion()`
call that should have used it.

**Step 4, aircraft and road vehicles.** Done as a follow-up, since neither executes on
Hilbergen. `aircraft_cmd.cpp` went from 111 accesses to 54, `roadveh_cmd.cpp` from 82 to
65. The wentbourne accumulators, which is the fixture that exercises them:

| Accumulator | Before | After | Change |
| --- | --- | --- | --- |
| `game_loop` | 248,614 ms | 238,671 ms | -4.0% |
| `trains` *(untouched)* | 125,444 ms | 121,039 ms | -3.5% |
| `ships` *(untouched)* | 8,415 ms | 8,029 ms | -4.6% |
| `road_vehicles` | 62,874 ms | 59,909 ms | -4.7% |
| **`aircraft`** | 3,111 ms | 2,719 ms | **-12.6%** |

**The untouched accumulators are the useful part of this table.** Trains and ships moved
-3.5% and -4.6% despite no code change, so roughly 4% of every figure here is drift, not
effect. Netting that out: aircraft improved by about 8-9%, and road vehicles did not move
beyond noise. Hilbergen agrees -- it dropped 9,326 to 9,017 ms on trains-only code that
was not touched at all, the same favourable drift from the other direction.

That is worth keeping as a technique: **a run that reports per-subsystem timings contains
its own drift control.** Any subsystem you did not touch is a measurement of the machine,
so the comparison to make is against *those*, not against an earlier absolute number. It
is cheaper than interleaving and it works on a single sample.

The asymmetry between aircraft and road vehicles is explained by what was actually
possible rather than by effort. `AircraftController` took one function-wide pair of
references for 38 accesses, because nothing in it destroys a vehicle -- the crash path
only *creates* an effect vehicle, and EnTT's paged storage keeps existing elements put
across insertion. `IndividualRoadVehicleController` could not: it calls
`VehicleEnterTile` five times, and while the code immediately after a `CannotEnter`
result writes to the vehicle's own component (so `v` provably survives), a tile-entry
handler destroying *some other* vehicle would still relocate elements and dangle the
reference. That could not be ruled out by inspection, so hoisting there was confined to
the stretches between those calls -- 17 accesses out of 39.

The lesson is that **the destruction hazard, not the edit count, is what bounds
hoisting.** A function with forty accesses and no destroying calls is one edit; a
function with forty accesses and five opaque calls that might destroy something is a
dozen careful ones with a worse payoff.

#### Deferred: ships and disaster vehicles

`disaster_vehicle.cpp` (65 accesses) and `ship_cmd.cpp` (52) are still per-site, and are
deliberately left that way for now.

Ships are 3.4% of the wentbourne game loop and disaster vehicles are rare enough not to
register at all, so the upside is small. More importantly, **phase 6 may make this work
unnecessary.** Its whole point is to replace per-vehicle registry lookups with a packed
walk; if the ship controller ends up driven by a view, hoisting inside it now is effort
spent on code that is about to change shape. Doing it first would be optimising the thing
being replaced.

So: revisit **after phase 6**, and only if the ship and disaster paths are still
per-vehicle at that point. If phase 6 converts them, this item disappears. If phase 6
turns out to be narrower than planned -- trains only, say -- then these two files become
the obvious cleanup, and the recipe is already established: check for destroying calls
first, hoist function-wide where there are none, and confine it to barrier-free stretches
where there are.

Recorded here rather than in phase 6's body because it is phase 4 debt, not phase 6 work,
and it should not be allowed to look like a prerequisite for starting phase 6.

#### What the repair actually taught

The *order* is the transferable part. The instinct is to hoist first, because "resolve
once per function" is the rule this document states and phase 2's 74% made memorable.
But hoisting is hundreds of edits requiring aliasing analysis at every site, while
**the constant factor was two header changes**. Measuring after steps 1 and 2 showed
that inlining was worth more than every hoist performed afterwards, and took about
twenty minutes.

So the rule needs a qualifier. "Resolve once per function" is right, but it is the
*second* thing to check. First make one access as cheap as it can be -- that multiplies
through every site whether hoisted or not, and it tells you what the hoisting is worth
before you commit to doing it. Phase 4 spent a lot of effort converting ~850 sites and
then recovered half the regression without touching most of them.

A smaller note worth keeping: **the Debug tree got slower at every step of this repair**,
ending at 272s against 217s before. That is not a contradiction, it is MSVC Debug not
inlining anything, so the change adds call layers there while removing them in release.
It is the clearest justification yet for the two-tree setup -- a single Debug-only
measurement would have rejected the fix that actually worked.

The remaining +37% is now the seam cost the phase was always going to incur: one
sparse-set lookup per resolved reference against a direct member offset. Phase 6's packed
walk is what pays that back, and it is now measurable against a sane baseline rather than
against a self-inflicted constant factor.

Worth noting that wentbourne sits at **+22%** against its own baseline, well below
Hilbergen's +37%. Different vehicle mix, and wentbourne spends proportionally more time in
work this phase never touched. Neither figure is wrong; they are answers to slightly
different questions, which is the argument for keeping both fixtures.

#### Sizing, as it stood before the work

Kept for the record, since the estimates proved reasonable. Counted rather than
estimated. Totals are textual matches on
`->field` / `.field` / `this->field`, and the write column is a rough upper bound
that over-counts (`direction` in particular collides with unrelated `Direction
direction` parameters), so read these as magnitudes:

| Field group | Sites | Writes (upper bound) | Status |
| --- | --- | --- | --- |
| `subspeed`, `motion_counter` | -- | -- | commit 1 done |
| `x_pos`, `y_pos`, `z_pos` | 162 / 165 / 129 | ~38 / 42 / 44 | blocked on 5 |
| `cur_speed` | 158 | ~68 | blocked on 5 |
| `direction` | 142 | ~79 | blocked on 5 |
| `progress` | 56 | ~41 | blocked on 5 |
| `tick_counter` | 35 | ~12 | blocked on 5 |

Roughly 850 sites and a few hundred writes still to move. Two implications for how
to sequence it. Do the field groups **one at a time, measuring each**, because a
single 850-site commit makes an unexplained regression unattributable -- phase 2's
74% was only diagnosable because it was isolated to `bounds`. And do the three
position fields as **one component**, not three: they are read and written together
throughout the movement code, so splitting them triples the lookups for no benefit.

`sizeof(Vehicle)` went from 544 to 560 bytes, as it must while both copies exist.

#### Completing `vcache`: shadow mode's blind spot, demonstrated

`vcache` is unserialised, so it needed nothing from phase 5 and went all the way to
commit 3. It is the first field group to complete the sequence, and the first
`Vehicle` member the migration has actually deleted.

Flipping the reads immediately exposed a real bug in the already-committed phase 4
work. `Vehicle::UpdateVisualEffect` ends with a conditional fixup for broken NewGRF
powered wagons:

```cpp
this->vcache.cached_vis_effect = visual_effect;
this->SyncVehicleCache();

if (!allow_power_change && powered_before != HasBit(this->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER)) {
    ToggleBit(this->vcache.cached_vis_effect, VE_DISABLE_WAGON_POWER);   /* <- no sync after this */
    ShowNewGrfVehicleError(...);
}
```

The `ToggleBit` mutates the member after the sync, so the component kept the
pre-toggle value indefinitely. Shadow mode reported **zero mismatches** the whole
time.

The reason is the limitation to internalise from this phase: **shadow mode verifies
coverage only at read sites that go through the accessor.** `cached_vis_effect` had
no accessor reads at all, so it had no verification at all -- the field was
unprotected while sitting inside a field group the report described as clean. Only
`cached_max_speed` and `cached_cargo_age_period` were genuinely covered, because
only they were read through `GetVehicleCache()`.

So "zero mismatches" is a statement about read coverage, not about the field group.
The practical rule: before trusting a shadow result for a field, check that
something actually reads that field through the accessor. A per-field comparison
count would make this visible directly, and is worth adding if another field group
turns out to have partial coverage.

The bug was latent rather than live -- nothing read the component copy, so nothing
consumed the stale value -- but it would have become a real behaviour change the
moment commit 2 flipped `train_cmd.cpp`'s powered-wagon check over. It was found by
reading the write sites while flipping the reads, which is the one point in the
sequence where every site gets looked at.

#### Why finishing `vcache` did not pay off

Commit 2 flipped 23 read sites; commit 3 converted the write sites, deleted the
member, and retired the `VehicleCache` shadow check. Correctness held throughout:
fingerprints unchanged on both fixtures (`015ED3D109C5CCCC`, `29B52DBB6E7D2558`),
determinism check passing, 102 of 102 tests, and `vehicle_motion` still at zero
mismatches over 9,117,260 comparisons.

Timings, Hilbergen game loop, 20,000 ticks:

| State | Samples | Minimum | Spread |
| --- | --- | --- | --- |
| Phase 0 baseline | 2 | 6,580 ms | 0.5% |
| Phase 4, motion commit 1 | 3 | 7,182 ms | 0.7% |
| `vcache` commit 2 | 3 | 7,252 ms | 0.9% |
| `vcache` commit 3 | 8 | 7,153 ms | 6.8% |

**No saving.** Commit 3's eight-sample range straddles both earlier states, so
commit 2, commit 3 and the phase 4 increment are indistinguishable from each other
at this sample size, and all three sit about 9% above baseline.

The prediction of a saving rested on two mechanisms, and measurement found neither:

1. **`sizeof(Vehicle)` did not shrink** -- 560 bytes in Debug and 544 in release,
   before and after. `sizeof(VehicleCache)` is 6, and the whole 6 bytes were
   absorbed by alignment padding. Measured offsets after removal: `grf_cache` at
   440, `group_id` at 464, `sprite_cache` at 468, so 2 bytes of interior padding
   now sit after `group_id` where none did before. `sprite_cache` is 4-aligned
   (468 is not a multiple of 8), which fixes the earlier layout: `vcache` at 464,
   `group_id` at 470, `sprite_cache` at 472, with no padding anywhere. Members
   after the removal therefore shifted down by 4, not 6, and the tail padded back
   up to the same total. No struct shrink means no cache-density win.
2. **The removed dual writes were cold.** Every `SyncVehicleCache()` call sat on a
   `ConsistChanged` or `Update*Cache` path, which run when a consist's composition
   changes -- not per tick. Deleting per-tick work would have shown up; deleting
   cold work does not.

The transferable lesson is about how to predict: **a performance prediction has to
name its mechanism, and the mechanism has to be checked for existence before the
prediction is worth recording.** "Deleting a member and its dual write should be
cheaper" sounds like reasoning but asserts no mechanism. Two minutes with
`offsetof` and a grep for the sync call sites would have falsified it before any
code moved. This is the same failure mode as the earlier wrong guesses about tail
padding and cold sweeps: the measured claims in this document have held up, the
inferred ones keep not holding up.

What the phase *did* buy is structural rather than numeric: one field group is
fully migrated, its member is gone, and the shadow scaffolding for it is retired.
That is the shape every remaining field group has to follow, and it is now known
to work end to end.

#### Component references are stable across creation, not destruction

Commit 3 needed a mutable component reference held across a NewGRF callback, which
raised the question of when an EnTT component reference can dangle. Verified
against `entt/entity/storage.hpp` at 3.16.0 rather than assumed:

- **Insertion is safe.** `assure_at_least` grows a paged vector of page *pointers*
  and allocates new pages; existing elements never move. A held reference survives
  vehicle creation.
- **Removal is the hazard.** `basic_storage::pop` moves the storage's *last*
  element into the vacated slot before popping, so destroying some other vehicle
  can silently turn a held reference into a different entity's data.
- **Sorting relocates elements** for the same reason.

So the rule is narrower than "don't hold references": hold them freely across
creation, never across vehicle destruction or a registry sort. `UpdateVisualEffect`
resolves its reference after the callback for that reason, and both the accessor
declaration and the call site carry the note. An initial draft of that comment said
"create or destroy", which was wrong on the create half -- worth flagging because a
hazard note that overstates the risk teaches the wrong model.

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

### Phase 5 -- The save staging mechanism (CAUTION) -- done, and simpler than planned

**This phase was moved forward from what used to be phase 7.** It was originally
bundled with the hollowing-out work as "consolidation", but it is not
consolidation, it is infrastructure that two later phases depend on. Discovering
that is what prompted the reordering; the reasoning is recorded below because a
reader coming to this document fresh will otherwise wonder why staging sits in the
middle.

Constraint 3.3 explains the mechanism: `SLE_VAR(Vehicle, x_pos, …)` names a
member, so a field cannot leave `Vehicle` while a descriptor reads it.

#### No staging struct was needed

The plan called for a staging struct: members mirroring the descriptor names,
gathered from components before a save and scattered back after a load. **That turned
out to be unnecessary, and the simpler answer is strictly better.**

`SaveLoad` holds its address getter as a plain function pointer, and
`GetVariableAddress` const_casts the result before load writes through it
(`saveload.h:1388`). A getter may therefore return the address of *anything* --
including a field inside a component. `SLE_GENERAL_NAME` already separates the
savegame field name from the member expression used to find it, so the name stays
`"subspeed"` while the storage moves somewhere else entirely.

The result is `src/saveload/component_sl.h`, three macros wrapping that pattern.
Compared with staging it means:

- no duplicate storage, so `sizeof(Vehicle)` can actually fall;
- no gather step, so a field cannot be missed on the way out;
- no scatter step, so a component cannot start out stale on the way in -- which is
  precisely the bug that produced 462 shadow mismatches in phase 4. Here it is not
  possible, because load writes the component and there is nowhere else to write.
  The `SyncMotion()` call added to `AfterLoadVehiclesPhase2` in phase 4 is deleted.

Staging is still the right answer when the mapping is not one-to-one: a saved field
computed from several component fields, a representation differing between disk and
memory, or a component that does not exist for every vehicle. The macros deliberately
do not attempt that. The plain case is most of them.

One precondition, spelled out in the header because it is easy to break by accident:
**the entity must exist when a descriptor runs.** Both loaders satisfy it --
`Train::CreateAtIndex(index)` runs the `Vehicle` constructor, and therefore
`RegisterVehicleEntity`, before `SlObject` is called (`vehicle_sl.cpp:1151`), and the
TTD-era loader likewise creates the vehicle before `LoadChunk`.

#### What landed

`subspeed` and `motion_counter` are fully migrated -- commits 2 and 3 both done, both
members deleted. That makes them the second and third fields to complete the sequence
after `vcache`, and the first *serialised* ones.

Sequencing that mattered: commit 2 flipped only **pure reads**. Read-modify-write
sites were left alone until commit 3, because flipping just the read half of an RMW
pair reproduces the exact phase 4 corruption -- read the component, write the member,
and staleness propagates instead of being reported. In commit 3 the member disappears
and the RMW converts atomically, so the hazard never exists.

Commit 3 also turned out to be **compiler-verified in a way commit 2 is not**: delete
the member and every remaining reference is a build error. Thirty-nine of them, across
seven files, each one mechanical. That asymmetry is worth remembering -- it is why the
descriptor switch and the member deletion were landed together rather than separately,
and why commit 3 is the cheap half of a migration despite touching more lines.

The TTD-era loader needed one hand-written `OldChunks` entry, since `OCL_SVAR` also
takes a member offset. `LoadChunk` skips entries whose base is `nullptr`
(`oldloader.cpp:166`), so the component lookup always has a real vehicle -- worth
checking rather than assuming, because a null base would have turned a harmless
offset computation into a crash.

**Results.** Fingerprints unchanged on both fixtures across seven samples
(`015ED3D109C5CCCC`, `29B52DBB6E7D2558`), determinism check passing, 102 of 102 tests.

Timing: no measurable change. Six Hilbergen samples gave a minimum of 7,473 ms
against 7,153 ms for the previous state, which is inside the ~7% band, and that batch
spread 9.6% on a visibly warm machine. The mechanism that should have helped is real
-- `DoUpdateSpeed` went from two registry lookups per call to one, because
`SyncMotion()` did its own lookup -- but it is too small to resolve at this noise
level. Resolving it needs interleaved A/B sampling, which is recorded as measurement
debt rather than guessed at.

**`sizeof(Vehicle)` did not move**: 560 in Debug, 544 in release, before and after
removing five bytes of fields. This is the *second* time a removal has been absorbed
by padding, so it is a property of the struct rather than a coincidence. Measured
offsets after removal: `cur_speed` at 310, `acceleration` at 312, `progress` at 313,
then `waiting_random_triggers` at 314, `random_bits` at 316, `last_station_visited` at
318, and `last_loading_tick` at **328** -- an 8-byte-aligned `TickCounter` now preceded
by six bytes of padding. `alignof(Vehicle)` is 8, and the anchors further down
(`grf_cache` at 440, `sprite_cache` at 468) did not move at all.

The lesson for the remaining field groups: **`Vehicle` is padding-rich around its
8-byte-aligned members, so removing small scalars does not shrink it.** A size win
needs either a large contiguous block -- which is what phase 8 targets in
`BaseConsist` and `sprite_cache` -- or enough fields removed at once to cross a
boundary. The three position fields are 12 bytes together and are the next real
candidate. Check with `offsetof` before predicting; do not assume.

**Exit: met**, with one criterion partly argued rather than measured.

#### Savegame compatibility: what is proven and what is argued

Worth separating, because this is the phase most able to break the file format.

**The reading half is measured.** Both fixtures were written by a stock build. They
load in the migrated build and reproduce the baseline fingerprint exactly, across
seven samples. Since `subspeed` feeds both the fingerprint and the trajectory, a
descriptor reading into the wrong place could not produce a matching 20,000-tick
fingerprint. Reading a stock savegame is therefore correct, empirically.

**The writing half is argued from the macro expansion**, not measured, because there
is no stock build in this tree to load the output. The argument is narrow enough to be
solid: `SLE_VAR_COMPONENT(Vehicle, VehicleMotion, subspeed, VarTypes::U8)` expands to
`SLE_CONDVAR_COMPONENT_NAME("subspeed", …)`, producing a `SaveLoad` with the same
`name`, the same `SaveLoadType::Variable`, the same `VarType`, and the same version
range as the `SLE_VAR` it replaced. Only `address_func` differs. The writer derives the
field table from exactly those members, so the emitted bytes cannot change. That is a
statement about literal macro expansion rather than about behaviour, which is why it is
acceptable here -- but it is still weaker than a load test, and a stock binary should be
kept around to close it properly.

#### The save-resume check could not be built, and why that is a finding

The new standard criterion 4 -- save-resume equivalence -- was implemented as
`-CheckResume` and **does not work**. It fails on unmodified code. Chasing that down
produced something more useful than the check itself:

- Runs from the curated fixtures are perfectly deterministic. Hilbergen at 20,000
  ticks has reproduced the same fingerprint on every sample ever taken.
- Runs from an autosave-on-exit save are **not**. Loading one fixed exit-save file and
  running 1,000 ticks three times in the same binary gave `C3D46F481F656B66`,
  `C3D46F481F656B66` and `7DBAF9A059E04D46`.

Confirmed against a build predating any component work, so it is pre-existing rather
than a migration artefact. A game resumed from an exit save does not evolve
deterministically at all, which makes the comparison meaningless -- and the first
diagnosis, "the exit save is not tick-precise", was wrong: tick precision would not
fix indeterminate content.

This is very likely the same root cause as the phase 0 finding that two identical runs
produce exit saves differing by a few hundred bytes in `VEHS` that do not scale with
run length: uninitialised state gets serialised, and on load it feeds back into the
simulation. **That remains a hypothesis** -- what is measured is the instability, not
its source.

Two consequences. The switch stays in the harness marked known-broken, because a
silently-absent check is worse than a loudly-broken one. And **phase 6's variant A gate
now depends on unblocking this**, which means answering the uninitialised-state question
and adding a tick-precise save (`-vnull:save_at=N`) to the null video driver. That is
real work that phase 6 inherits, and it is better to know now than to discover it while
trying to validate a dispatch rewrite.

There is also a methodological warning here. The first crossload comparison looked like
phase 5 had broken the round-trip: same input file, different fingerprint from the
control. Running it three more times showed the same binary disagreeing with itself.
**A single differing sample from a path whose determinism has not been established is
not evidence.** The straight-run fingerprints had earned that trust over many samples;
the resume path never had.

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

#### The narrow de-risk experiment, and why it is being skipped

`vcache` is **not** serialised, so its commit 3 needed no staging and is now done:
the member is gone and the component holds the only copy. It is also read-only in
the tick loop, so a view over `VehicleCacheComponent` could be walked with no
write-back at all -- a narrow but clean early measurement of whether packed
iteration pays off.

That experiment was previously the recommended next step, on the reasoning that the
ordering argument for staging-before-loop-conversion is an *inference* and this
document's inferences have a poor record next to its measurements.

**Decision: skip it and proceed with staging.** Two reasons, and they are better
than the argument for doing it.

First, the ordering argument stopped being merely inferred. All nine motion fields
are now confirmed serialised against the descriptor lists, so commit 3 is blocked
for every one of them regardless of what a locality experiment shows. Staging is on
the critical path either way -- a null result would not have saved the work.

Second, `vcache` is a weak probe. Its reads are two fields out of a 6-byte struct,
so a packed walk has almost no data to be dense *about*. The asymmetry runs badly:
a null result would be weak evidence against phase 6, while only a positive result
would have told us much. Spending the time on a test that can mostly only fail to
inform is the wrong trade when phase 6 will measure both dispatch variants directly
and definitively.

The de-risking effort is better spent on the sort-frequency measurement described
under phase 6, which is cheaper still and answers a question that actually gates a
design decision.

**Exit:** standard criteria, plus a save written by the staged build loads in
unmodified OpenTTD, `subspeed` and `motion_counter` deleted from `Vehicle` with
shadow mode removed for them, and the resulting `sizeof(Vehicle)` and timing
change recorded.

On that last point, temper the expectation with what finishing `vcache` measured:
deleting a member is not automatically a timing win. It gives back the dual-write
cost only where those writes were hot, and it shrinks `sizeof(Vehicle)` only if the
bytes were not already absorbable as padding. Check both before predicting -- the
motion fields are written per tick, so the first mechanism is genuinely present
here in a way it was not for `vcache`, but the second still needs an `offsetof`
check rather than an assumption.

### Phase 6 -- Devirtualising the tick dispatch (DANGER) -- sort cost measured, not started

This is where phase 4 gets paid for. Phase 4 moves the fields and accepts a
regression; this phase converts the loop to iterate views, so the registry lookup
is amortised across a packed walk rather than repeated per vehicle. Judge the two
together as well as separately: the pair is only worthwhile if phase 6's gain
exceeds phase 4's loss, and knowing both halves is the point of having run them
apart.

Replacing `virtual bool Tick()` plus `switch (v->type)` with typed passes is the
largest structural win available -- and it forces a decision that should be made
deliberately, because the two options differ in kind, not degree.

- **Variant A, order-preserving.** One pass in ascending `VehicleID`, replacing
  virtual dispatch with a type tag and a switch. Smaller win; continuation from
  existing saves stays equivalent.
- **Variant B, typed passes.** Four separate views, one per vehicle type. Much
  better locality and branch behaviour -- but it changes the interleaving of
  `Random()` draws, so a save made by stock OpenTTD no longer continues on the same
  trajectory. The *format* still loads perfectly; the *trajectory* diverges.

**Both will be built and A/B'd against the same save.** That is the plan, and the
comparison is the most interesting single measurement left in the project, because
the gap between the two variants is a price tag on the determinism constraint
itself -- the thing section 3.1 says the codebase cannot give up. Very few projects
get to measure what their hardest invariant costs them.

#### Variant B is not merely "different from master" -- it is not self-consistent

This needs stating plainly, because the earlier draft of this section understated it
and the distinction changes what variant B is allowed to be used for.

An unsorted EnTT view walks the packed array, and **packed order is a function of
history, not of the live set.** That is pinned by a test rather than assumed:
`src/tests/entt_smoke.cpp`, "Default storage order is history dependent", shows the
same seven live entities coming out in an order that is neither ascending nor the
descending order an untouched registry would give, purely because one entity was
removed and the last element was swapped into the hole.

The consequence is not just divergence from stock OpenTTD. It is divergence from
*itself*:

- Play to tick N and keep going: packed order reflects the whole create/destroy
  history of the session.
- Play to tick N, save, quit, reload, keep going: the registry is rebuilt from the
  pool in ascending index order, so packed order is fresh.

Same savegame, same live vehicles, two different visit orders, two different
futures. That breaks save-resume equivalence -- the second of the two determinism
requirements in section 3.1 -- and it would desync a multiplayer join instantly,
because a joining client builds its registry fresh from the transferred savegame
while the server carries its accumulated order.

**The existing determinism check will not catch this.** The harness runs the same
save twice and compares fingerprints; both runs start from the same fresh load, so
both get the same order and the check passes. Variant B would look clean.

So phase 6 needs one more verification tool, and building it is part of the phase's
cost rather than an optional extra: **a save-resume equivalence check.** Run N ticks
then save, reload and run M more; separately run N+M ticks straight through;
compare final fingerprints. Under variant A they must match. Under variant B they
will not, and that failing check is the phase's evidence rather than a bug to chase.

**Phase 5 tried to build this and could not.** The switch exists in
`run-benchmark.ps1` as `-CheckResume` and fails on unmodified code: a game resumed
from an autosave-on-exit save does not evolve deterministically, measured on a build
predating the migration. See phase 5 for the numbers. So phase 6 inherits three pieces
of work here, in order:

1. **Answer the uninitialised-state question.** Exit saves appear to carry state that
   was never initialised, which is the leading hypothesis for both this instability and
   the phase 0 byte-comparison noise. A tick-precise save of indeterminate content
   would still not reproduce, so this comes first.
2. **Add a tick-precise save** to the null video driver, `-vnull:save_at=N`, the
   natural companion to its existing tick counter. Autosave-on-exit fires at process
   exit rather than at a defined tick boundary.
3. **Then** wire the check up and use it as variant A's gate.

Until that is done, variant A cannot actually be *proven* order-preserving beyond the
fingerprint check, and variant B's headline property cannot be demonstrated rather than
argued. That is a real dependency and it should be scheduled, not discovered.

**Descoped, deliberately.** The three items above are dropped: phase 6 gates on the
existing fingerprint checks, the same ones every phase so far has used. The reason is
that this project's purpose is measurement rather than a shippable branch, and the
save-resume check would cost a tick-precise save plus the uninitialised-state
investigation to produce evidence for a property that can be argued from the code -- an
unsorted EnTT view walks packed order, packed order is a function of history, and
`src/tests/entt_smoke.cpp` already pins that. What is lost is that variant B's
divergence is demonstrated by argument and a unit test rather than by a failing
end-to-end check, and variant A's order preservation rests on the fingerprint alone.
That is a weaker guarantee, stated here so nobody later mistakes it for a strong one.

Given that, variant B is scoped as a **measurement branch, not a foundation.**
Nothing gets built on top of it, and phase 7 continues to target variant A. It is
playable single-player -- a player cannot observe that resuming produced a different
future -- which satisfies the ground rule, but it is not a base for further work.

#### Variant A has its own risk, and it is worth pricing first

Variant A gets locality only if the storage is actually sorted; iterating the pool
and looking up components per vehicle is what the code does *now*, and phase 4
measured that at no locality benefit. So variant A means `SortVehicleRegistry()`
running whenever the registry is dirty.

That sort is O(n log n) on the live set -- on wentbourne, ~85,000 parts, so around
1.4 million comparisons per sort. It is correct and deterministic, so this is purely
a cost question, and the cost depends entirely on **how often the dirty flag
fires**. If vehicle creation and destruction are rare, the sort amortises to
nothing. If they happen every few ticks in a busy game, the sort could swamp the
locality win outright and variant A becomes pointless.

That is cheap to find out and should be measured before either variant is built:
instrument `SortVehicleRegistry()` with a call count and accumulated time, and emit
them as `ecs.sorts` and `ecs.sort_ms`. A single wentbourne run then says whether
variant A is viable at all. **Do this first** -- it is an hour's work that could
change the whole shape of the phase, and this document's record is that the cheap
measurement usually beats the confident argument.

*Done. The paragraph above is kept as written because the measurement contradicted part
of it -- see "The sort, measured" below, where one wentbourne run turns out to be
exactly the wrong way to answer the question.*

If the sort does turn out to dominate, the honest options are to accept
pool-order iteration with no locality win (variant A becomes just devirtualisation,
which is still worth something), or to note that canonical order and packed
locality are fundamentally in tension here and let variant B's number speak to what
that tension costs. Both are legitimate results; neither is a failure of the phase.

#### The sort, measured -- and the fixtures disagree by a factor of 74

Done, and it was worth doing first. `SortVehicleRegistry()` now records call count,
sort count, time, and the registration and unregistration churn that sets the dirty
flag; the report carries them as `ecs.sort_*`. Fingerprints unchanged on both fixtures
and 102 of 102 tests, since none of this touches game state.

| | Hilbergen | wentbourne |
| --- | --- | --- |
| Vehicle parts | 2,818 | 85,259 |
| Ticks | 20,000 | 5,000 |
| Ticks that found the registry dirty | **88.3%** | **2.56%** |
| Churn per tick (registrations) | 1.09 | 0.107 |
| Key sort, mean | 35.9 us | 1,623 us |
| Component `sort_as`, mean | 18.4 us | 913 us |
| Total sort time | 960 ms | 325 ms |
| **Share of the game loop** | **10.4%** | **0.14%** |

**The small fixture is the expensive one, by 74x in relative terms.** That inverts the
assumption the section above was built on. The O(n log n) is not what decides this: the
30x larger save pays a 5x larger cost *per sort* and still comes out two orders of
magnitude cheaper overall, because it sorts 128 times where Hilbergen sorts 17,665.
**Sort cost is set by how often the dirty flag fires, and that is a property of the
savegame, not of its size.**

The churn is create/destroy balanced almost exactly -- 21,771 against 21,765 on
Hilbergen, 534 against 534 on wentbourne -- which is the signature of short-lived effect
vehicles (smoke, sparks) rather than of players building things. Hilbergen fits that
reading arithmetically: 88 effect vehicles alive at 1.09 created per tick implies a mean
lifetime around 80 ticks, which is what an effect vehicle lives. Wentbourne does not fit
it -- 231 alive at 0.107 per tick implies 2,159 ticks, far too long for smoke -- so its
effect population must be mostly long-lived, and the low churn is unexplained rather than
understood. Recorded as a loose end; it does not change the numbers.

So **variant A is viable, but the tax is real and it is worst on the precision
fixture.** Phase 6 must beat 10.4% on Hilbergen, not the 0.14% a single wentbourne run
would have suggested. The plan above says "a single wentbourne run then says whether
variant A is viable at all", and that was wrong: wentbourne alone would have reported
the sort as free and hidden the entire cost. **Run both fixtures whenever the answer
might depend on churn rather than on scale.**

#### Sorting the key storage is not sorting the registry

Found while instrumenting, and it would have quietly cost the phase its headline.
`registry.sort<VehicleRef>()` orders the `VehicleRef` storage and nothing else. The
component storages are separate sparse sets with their own packed arrays, so a
`view<VehicleMotion>` would have walked them in an order that was neither ascending nor
matched to the key -- with no locality, which is the entire point of variant A.

Matching them is `registry.sort<To, VehicleRef>()`, EnTT's `sort_as`, one linear pass per
storage rather than another comparison sort. It is now done for all four components, and
it costs a further 51% on top of the key sort on Hilbergen and 56% on wentbourne. Timed
separately as `ecs.sort_components_ms` because the two scale differently.

The trap to record: **a component added in `RegisterVehicleEntity` and forgotten in
`SortVehicleRegistry` is not a correctness bug today** -- nothing iterates a component
storage for game state yet -- **but it silently costs phase 6 the locality it exists to
buy.** No test catches that, and the fingerprint cannot: the order is right, the data is
right, only the layout is wrong.

#### A 5.3x speedup that is a 16.8x slowdown

The sort runs on an array that is already sorted except for a handful of appended
entities, which is the textbook case for insertion sort over introsort. EnTT takes the
algorithm as a parameter, so this is a one-word change and was measured rather than
argued.

| Fixture | `std_sort` | `insertion_sort` | |
| --- | --- | --- | --- |
| Hilbergen, mean per sort | 35.9 us | 6.8 us | **5.3x faster** |
| wentbourne, mean per sort | 1,623 us | 27,249 us | **16.8x slower** |

Both are the same one-word change and the fingerprints held in both cases. The cause is
swap-and-pop: destroying an entity moves the *last* element of the packed array into the
hole, so one destruction in an 85,000 element array displaces an element by up to 85,000
positions, and insertion sort pays that displacement linearly. Hilbergen churns often but
is small; wentbourne is large, so each of its rare sorts faces a few enormous
displacements.

Left as `std_sort`, which is the algorithm that never falls over. The available win is
recorded rather than taken, because taking it needs a guard -- count structural changes
since the last sort and pick the algorithm from that count, rather than picking one
statically -- and that guard belongs in phase 6 with its own measurement. A mass
destruction event such as a bankruptcy would be the worst case, and neither fixture
exercises one.

The transferable part is the shape of the result: **the optimisation that is obviously
right for the fixture you are iterating on can be catastrophic on the one you are not.**
Both numbers came from the same one-word change, twenty minutes apart.

**Exit:** standard criteria for variant A, including the new save-resume check.
For variant B: save format byte-identical, both variants build and play, the
save-resume check recorded as failing *by design* with the mechanism explained, a
fresh fingerprint baseline captured and shown stable across repeat runs, and A/B
timings on both fixtures with at least five samples each per the revised noise
rules. The headline deliverable is the A-versus-B gap and an explanation of where it
comes from.

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
