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
nothing -- and once sorted, packed order *is* ascending ID order, so the
contiguity you came for is preserved.

**Consequence:** owning groups are unavailable for game-state components.
EnTT forbids sorting a pool once a group owns it, and sorting is exactly what is
needed here.
Use views, or non-owning groups.

### 3.2 Pointer stability

EnTT's default swap-and-pop invalidates references on removal -- and *sorting
moves components too*, so switching a storage to `in_place_delete` does not
rescue you.

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

The null video driver already accepts a tick count
(`GetDriverParamInt(parm, "ticks", 1000)`, `src/video/null_v.cpp:33`), which
gives a headless fixed-work benchmark:

```
build\Debug\openttd.exe -x -Q -snull -mnull -vnull:ticks=50000 -g bench.sav
```

Determinism checks available on top of that:

- Run the same save twice and compare the output savegames byte for byte.
- `-d desync=2` adds vehicle cache validation (`src/cachecheck.cpp`) and command
  logging; `-d desync=3` adds monthly `dmp_cmds_*.sav` dumps that are convenient
  byte-comparison fixtures. See [debugging_desyncs.md](./debugging_desyncs.md).
- `ctest --test-dir build -C Debug -R regression` must stay green. It is the
  closest thing in-tree to a state-equivalence test.

### Standard exit criteria

Unless a phase says otherwise, all of these must hold before moving on:

1. Builds clean on win64.
2. Game boots, loads every benchmark save, and is playable.
3. Save round-trip is byte-identical.
4. Regression suite green.
5. Benchmark timings recorded against the phase 0 baseline.

## 5. Phases

Risk labels: `CLEAR` = no game-state exposure, cannot desync.
`CAUTION` = touches game state, shadow-verified.
`DANGER` = alters visit order, or deep coupling.

### Phase 0 -- Toolchain and baseline (CLEAR)

No EnTT usage yet, only the ability to use it and the numbers to judge later
phases by.

EnTT 3.16.0 is already a port in the local vcpkg tree, so add it to
`vcpkg.json`, then `find_package(EnTT CONFIG REQUIRED)` and link it into
`openttd_lib`.
Prove the toolchain with a Catch2 test in `src/tests/` that creates a registry,
emplaces a component, and iterates a view.
Remember to add the new test file to `src/tests/CMakeLists.txt`.

Then build the harness described above, and record `sizeof(Vehicle)` and
`sizeof(Train)` -- a deliberately wrong `static_assert` will report the real
numbers. They are the headline before/after figure.

**Exit:** standard criteria, plus baseline timings and struct sizes recorded.

### Phase 1 -- Registry, identity mapping, lifecycle (CLEAR)

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

**Exit:** standard criteria, plus entity count tracks `Vehicle::GetNumItems()`
exactly across create, destroy, save, load and new-game; order assert silent.

### Phase 2 -- First real components: sprite and viewport cache (CLEAR)

This is the learning phase, chosen because it is unable to hurt you.
`MutableSpriteCache`, `colourmap`, `coord` and `bounds` are all marked `NOSAVE`
and are purely client-side presentation: they are absent from the savegame and
cannot cause a desync, so both hard constraints are suspended.

Move them into components and convert the drawing-side sweeps, of which this is
the archetype:

```c++
for (Vehicle *v : Vehicle::Iterate()) { v->colourmap = PAL_NONE; }
/* vehicle.cpp:689 -- strides ~600 bytes to write 4 */
```

Expect a large multiple on that specific loop.
Use the phase to settle the idioms: how views are obtained, where the sort
happens, how the legacy accessors forward.
`ViewportDrawing` is a real cost centre, so this is not busywork.

**Exit:** standard criteria, plus no visual regressions when panning, zooming or
following vehicles; `ViewportDrawing` timing recorded.

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
