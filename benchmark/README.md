# Benchmark harness

Phase 0 harness for the EnTT migration described in
[docs/ecs-migration-plan.md](../docs/ecs-migration-plan.md).

It runs a fixed number of game ticks headlessly from a savegame, records timings and
workload counts, and can verify that two identical runs produce byte-identical saves.

## Contents

| Path | Tracked | Purpose |
| --- | --- | --- |
| `run-benchmark.ps1` | yes | Runner. Wraps the awkward parts of running OpenTTD headlessly. |
| `bench.cfg` | yes | Config making runs quiet, headless and reproducible. |
| `saves/` | no | Savegame fixtures. Local only, they are large. |
| `out/` | no | Generated reports and copied exit saves. |

## Usage

```powershell
# One run, recording a report
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label phase0

# Two runs, comparing exit saves byte for byte
.\benchmark\run-benchmark.ps1 -Save wentbourne -Ticks 5000 -Label phase0 -CheckDeterminism

# Debug build, for correctness work rather than timings
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 2000 -Config Debug -Label shadow
```

```powershell
# Gate a migration phase against the phase 0 baseline
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label phase4 `
    -CompareTo .\benchmark\out\phase0-Hilbergen-RelWithDebInfo.tsv
```

Reports land in `out/<label>-<save>-<config>.tsv` as tab separated key/value pairs.
Keys are grouped by prefix: `run.`, `world.`, `load.`, `sizeof.`, `perf.`, `ecs.`,
`state.`, `determinism.`, `info.`.

The `ecs.` keys track the EnTT registry added in phase 1. `ecs.vehicle_entities` must
always equal `load.vehicle_parts`, and `ecs.registry_valid` must be 1 -- it checks one
entity per pooled vehicle, a correct identity mapping in both directions, and ascending
`VehicleID` iteration order. Both are cheap to eyeball and worth checking on every run.

The `shadow.` keys track fields part way through migration, where the value lives both on
`Vehicle` and in a component and every read compares the two. `mismatches` must be zero.
Note that `comparisons` of zero is **not** a pass: it means shadow mode was compiled out,
which is the normal state of the `build-release` tree. A shadow run needs `-BuildDir
build`, or a release build with `OTTD_ECS_SHADOW` defined. See `src/ecs_shadow.h`.

**A non-zero `comparisons` with zero `mismatches` is weaker evidence than it looks.** The
count aggregates a whole field group, and verification only happens at read sites that go
through the accessor — so a field nothing reads that way contributes no comparisons and
gets no checking, while the group still reports clean. This is not hypothetical: a missing
sync on `VehicleCache::cached_vis_effect` survived tens of millions of `vehicle_cache`
comparisons precisely because no accessor read that field. Before trusting a zero, confirm
that something actually reads the field you care about through the accessor.

Each `perf.<group>` entry carries five figures, which answer different questions:

| Key | Meaning |
| --- | --- |
| `total_ms` | Cumulative time over the whole run. |
| `count` | Number of measurements. For the vehicle groups this equals the tick count, because the accumulator is reset once per tick. |
| `mean_ms` | Cost of one measurement. |
| `per_tick_us` | Cost per simulated tick. Use this to compare groups that are measured at different rates, such as `drawing`. |
| `pct_of_game_loop` | Share of the game loop. Makes reports comparable between savegames of different sizes. |
| `ns_per_object_tick` | Cost per vehicle *part* per tick, with the count in `objects`. |
| `ns_per_consist_tick` | Cost per *front* vehicle per tick, with the count in `consists`. |

`ns_per_object_tick` is the one to watch for the migration. It divides the workload out,
so it distinguishes a real improvement from a savegame that merely has fewer vehicles.
Two vehicle denominators are reported because neither alone is honest; see
[Parts and consists](#parts-and-consists) for what they mean and which to use when.

## Two build trees

`OPTION_USE_ASSERTS` is a CMake cache variable and applies to every configuration in a
tree, so a single tree cannot give both assert-checked correctness runs and clean
timings. There are therefore two:

| Tree | Asserts | Purpose |
| --- | --- | --- |
| `build` | ON | Correctness. Shadow-mode assertions, registry invariants, the test suite. |
| `build-release` | OFF | Timings. Every number that goes in a results table. |

The runner defaults to `build-release`. Pass `-BuildDir build` for correctness work:

```powershell
# Timings (default tree)
.\benchmark\run-benchmark.ps1 -Save wentbourne -Ticks 5000 -Label phase4

# Correctness, with asserts live
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 3000 -Config Debug -BuildDir build -Label shadow
```

Configure the release tree with:

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" `
  -DCMAKE_TOOLCHAIN_FILE=C:/git/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DOPTION_USE_ASSERTS=OFF
```

Report filenames include the tree name, because timings from the two are not comparable
and silently overwriting a baseline is worse than a verbose filename. Fingerprints *are*
comparable across trees, since asserts do not change game logic, so `-CompareTo` works
between them. The runner also warns if it produces timings from an assert-enabled build.

Note that a new tree starts without a graphics set: copy `baseset/opengfx-*.tar` across
from an existing tree, since CMake only installs the files that ship with the source.

A Debug build is roughly an order of magnitude slower, and the ratios between subsystems
shift enough to point at the wrong hot spot.

## Parts and consists

Several columns in the report count vehicles two different ways, and the difference
matters enough to change which subsystem looks expensive. "Consist" is OpenTTD's own
term, from `src/base_consist.h`.

**Part** is one entry in the vehicle pool, i.e. one `Vehicle` object. A locomotive is a
part; each wagon coupled behind it is a *separate* part with its own pool slot. This is
what `Vehicle::Iterate()` walks and what `CallVehicleTicks()` loops over, so it is the
count that scales with memory traffic.

**Consist** is the whole coupled chain, counted once at its front vehicle -- roughly
"what a player calls one vehicle": one row in the vehicle list, one thing you click and
give orders to. The report counts these with `IsPrimaryVehicle()`, which each type
defines differently:

| Type | `IsPrimaryVehicle()` | Source |
| --- | --- | --- |
| Train | `IsFrontEngine()` | `train.h:124` |
| RoadVehicle | `IsFrontEngine()` | `roadveh.h:128` |
| Ship | always true, ships are single-part | `ship.h:47` |
| Aircraft | `IsNormalAircraft()` | `aircraft.h:95` |

Effect vehicles (smoke, sparks) and disaster vehicles are parts but never primary, so
they appear in `load.vehicle_parts` and in no consist count.

### Why both denominators are reported

Pathfinding and order processing happen once *per consist*. Divide that cost by a long
train's parts and trains look cheap; divide it by a road vehicle's single part and road
vehicles look catastrophic. Same work, different denominator. Wentbourne, asserts off:

| Group | Parts | Consists | Parts/consist | ns/part/tick | ns/consist/tick |
| --- | --- | --- | --- | --- | --- |
| Trains | 75,182 | 4,833 | 15.6 | 261.1 | 4061.2 |
| Road vehicles | 5,499 | 5,499 | 1.00 | 1985.5 | 1985.5 |
| Ships | 2,818 | 2,818 | 1.00 | 503.0 | 503.0 |
| Aircraft | 1,529 | 749 | 2.04 | 306.1 | 624.9 |

Per part, road vehicles look about eight times worse than trains. Per consist the
ranking reverses and trains are 2.05x the more expensive. The ratios are all explicable:
ships are single-part by definition, this save has no articulated road vehicles, and
aircraft sit at 2.04 because each plane owns a separate shadow part with helicopters
adding a rotor.

These are the asserts-off figures. The assert-enabled tree inflated road vehicles by 36%
against trains by 17.5%, which made the per-consist gap look like 1.6x rather than 2.05x
— a good illustration of why timings belong in `build-release`.

So compare like with like. Prefer **per part** when judging a change to data layout,
since that is what scales with the number of objects walked. Prefer **per consist** when
judging a change to decision-making work such as pathfinding or order handling.

### Consist data is stored per part

Worth knowing because it is a concrete migration target. `Vehicle` *inherits*
`BaseConsist`, so all 88 bytes of it (`sizeof.BaseConsist` in the report) are carried by
every part, while the contents -- vehicle name, timetable and lateness counters, depot
unbunching timestamps, service interval, current order indices -- are only meaningful on
the front vehicle.

On Wentbourne that is 85,259 parts against 13,899 consists, so 71,360 parts carry 88
bytes of data that means nothing to them: about 6.0 MB of the 7.2 MB total, or 84% dead
weight. It is also 16% of the 552 byte `Vehicle`. This is exactly the "optional data that
currently costs every entity" case the migration plan targets in phase 7, and the report
tracks the inputs so the figure can be recomputed at any point.

## The fixtures

| Save | Map | Year | Vehicle parts | Consists | Stations | Industries | Ticks/s |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Hilbergen.sav` | 512x512 | 2023 | 2,818 (2,730 trains) | 231 | 111 | 233 | 1,843 |
| `wentbourne.sav` | 1024x1024 | 3739 | 85,259 | 13,899 | 2,263 | 1,309 | 19.3 |

Hilbergen is almost entirely trains, which makes it a clean fixture for attributing a
change to `GameLoopTrains` without road, ship or aircraft noise. It is also fast enough
to iterate on: 20,000 ticks in about 10 s.

Wentbourne is the stress case, and a good one. Its 85,259 vehicle parts break down as
75,182 train, 5,499 road, 2,818 ship and 1,529 aircraft, so it exercises all four tick
paths. It runs at 19.3 ticks/s against the roughly 33.3 needed for real time, meaning it
is genuinely at the limit rather than merely big. Budget about four minutes per 5,000
ticks.

Recorded baselines (RelWithDebInfo, phase 0) live in `out/` and are summarised in
[docs/ecs-migration-plan.md](../docs/ecs-migration-plan.md). Vehicle ticks are 84% of
the game loop on Wentbourne and 79% on Hilbergen.

## Timing noise

**Within a batch the harness looks very precise. Across batches it is not.** This
distinction cost two wrong noise figures before it was understood, so it is worth reading
before quoting any number.

Repeated 20,000-tick Hilbergen runs, each batch taken back to back with nothing else
running:

| Set | `perf.game_loop.total_ms` | Spread |
| --- | --- | --- |
| Before a change | 6610.3, 6579.9 | 0.5% |
| After a change | 6772.5, 6734.9, 6746.3 | 0.6% |
| Eight consecutive runs, no code change | 7152.9 … 7636.5 | **6.8%** |

The first two batches suggested a 0.3% band. The third batch — same binary, same fixture,
eight samples instead of two or three — spanned 6.8%. Both readings are accurate about
what they measured; only the third is a usable noise band.

**Two samples cannot estimate a spread.** They produce exactly one gap, and a small gap
reads as precision when it is really just one draw. That is how the 0.3% figure got into
this file: it came from the two-sample row above.

The pattern is that each batch is internally tight but batches sit at different levels —
consistent with thermal state and background activity drifting over minutes. So
tight-within-batch is *not* evidence of precision, and it is actively misleading, because
it invites quoting a three-sample median to four significant figures.

An earlier version of this file claimed a 15% noise band. That figure was measured while
compiles were running in parallel, and it is badly wrong as general guidance: applying it
would have dismissed a genuine regression. Machine load, not the harness, was that noise.
The 0.3% that replaced it was wrong in the other direction. The honest figure for
Hilbergen at 20,000 ticks is **about 7%**.

**Long runs are much noisier than short ones**, which inverts the usual intuition that a
longer run averages noise out. The same change measured on both fixtures:

| Fixture | Run length | Samples | Spread |
| --- | --- | --- | --- |
| Hilbergen, 20,000 ticks | ~9 s | 6772, 6735, 6746 ms | 0.6% |
| wentbourne, 5,000 ticks | ~3.5 min | 206.9, 211.8, 228.2 s | 10.4% |

The wentbourne samples climb monotonically across consecutive runs, which looks like
thermal throttling. So a 3.5-minute run cannot resolve a few percent, no matter how many
times it is repeated back to back.

So: **run benchmarks with nothing else running, take at least five samples, and compare
minima rather than medians.** The minimum is the least-contaminated sample and is the
standard robust statistic for benchmarks like this; a median tracks the batch's thermal
level as much as it tracks the code. Three samples are enough only when the change is
large enough that its sign is obvious.

On Hilbergen that resolves differences above roughly 7%. On wentbourne, expect no better
than about 10% unless samples are interleaved (A, B, A, B) with a pause between them, so
prefer Hilbergen for precision and wentbourne for coverage and scale.

To do better than 7% on Hilbergen, interleave A and B rather than taking more samples of
each: batch-to-batch drift is the dominant term, and interleaving is the only thing that
cancels it. Adding samples within a batch narrows the wrong distribution.

Discard any sample taken immediately after a build — the first run after a rebuild is
consistently slow, at 6991 ms against a 6751 ms median above, presumably cold caches.

`pct_of_game_loop` remains the right thing to compare across sessions or machines, since
it divides out whatever the absolute speed happened to be.

## Things that will waste your afternoon

Five non-obvious behaviours, all of which the runner already handles. They are recorded
here because they are invisible failures rather than error messages.

**OpenTTD cannot print to your shell on Windows.** The binary is built for the GUI
subsystem, and `CreateConsole` in `os/windows/win32.cpp` calls `AllocConsole` and then
reopens the standard streams onto that new console. Redirecting stdout or stderr from a
shell captures nothing at all, even for a failing run. This is why the report is written
to a file, and why a failed run gives you an exit code and no explanation.

**`-c <path>` moves the data search path.** `fileio.cpp` derives a search directory from
the config file's parent directory, so pointing `-c` at a config outside the build tree
makes that directory the personal data directory, `build/baseset` stops being found, and
the game exits 1 silently. The runner copies `bench.cfg` into `build/` before running.

**`gui.autosave_on_exit` defaults to false.** Without it no `exit.sav` is produced, and a
determinism check would compare files that do not exist. It is set in `bench.cfg`, along
with `gui.threaded_saves = false` so the save is complete before the process exits.

**PowerShell variables are case-insensitive.** `$buildDir = Join-Path $repoRoot $BuildDir`
silently overwrites the `$BuildDir` *parameter* with an absolute path, because they are the
same variable. That produced a report filename containing a drive letter and a colon, so
the run "succeeded" and wrote nothing. The runner uses `$buildPath` for the resolved path
to keep them distinct; do the same for any parameter you add.

**An unknown key in `bench.cfg` is ignored silently.** Verify any setting you add against
`src/table/settings/*.ini`, including its section, which is the part before the dot in the
`var` field. A key in the wrong section does nothing and reports nothing.

## The game state fingerprint

Every report ends with a set of `state.` keys: a hash of the values that define
behaviour, split by subsystem.

```
state.random_state_0    2671971205
state.random_state_1    4105569083
state.hash.vehicles     7008ECA72A80BC30
state.hash.companies    B16C6C111B91F99D
state.hash.stations     38451EB535F6A5B1
state.hash.towns        8A4870BAD0A6AFAE
state.hash.industries   536EB26AB9CF03AC
state.hash.globals      451480E1F844C42C
state.hash.combined     3C94DD1E5614C300
```

This is the phase gate. A migration step meant to preserve behaviour must reproduce
the baseline fingerprint:

```powershell
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label phase4 `
    -CompareTo .\benchmark\out\phase0-Hilbergen-RelWithDebInfo.tsv
```

The split by subsystem is the useful part. A mismatch names *where* behaviour changed,
so "something diverged" becomes "vehicles diverged, towns did not". `random_state` is
the most sensitive single value: any change to how many times or in what order the game
draws from the shared randomiser shows up there immediately, which is precisely what a
reordered iteration would cause.

Verified on both counts. Two identical 3000 tick runs produce identical fingerprints
while their savegames differ by around 100 KB; and running 3001 ticks instead of 3000
changes the vehicle, company, station and global hashes while correctly leaving towns
and industries alone, since those only update on periodic ticks.

### Why savegames are not compared byte for byte

Because that does not work, on unmodified master. Two identical runs produce exit saves
that differ:

- 32 bytes are `_game_session_stats.savegame_id` (`misc_sl.cpp:105`), a random
  per-session identifier. Harmless.
- The rest are scattered 1 to 4 byte runs through the `VEHS` chunk, roughly one per
  vehicle record. The count does not grow with run length: 488 differing bytes after
  1 tick, 442 after 100, 445 after 20000.

Divergence that is immediate and does not accumulate is not simulation drift. It points
at bytes in the vehicle records that are not a function of game state, most likely fields
never initialised for some vehicle types that get serialised with whatever was in memory.
The fingerprint being stable across those same runs confirms the simulation itself is
deterministic.

The save hash is still recorded as `info.exit_save_sha256`, for reference only. Do not
gate on it.

Save *format* compatibility is a separate, still-hard requirement, and is checked
differently: a save written by a migrated build must load in unmodified OpenTTD.

## Where the numbers come from

`PerformanceElement` timings come from OpenTTD's existing instrumentation, but not
through the framerate window. That reports averages over a rolling buffer of at most 512
samples, which on a 20,000 tick run covers the last few percent. Phase 0 added
whole-run totals to `PerformanceData` in `framerate_gui.cpp`, exposed through
`GetPerformanceTotal` in `framerate_type.h`, and `src/benchmark_stats.cpp` writes the
report from those.
