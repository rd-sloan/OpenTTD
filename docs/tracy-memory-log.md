# Tracy memory tracking work log

Running record of the memory tracking work planned in
[docs/tracy-memory-plan.md](tracy-memory-plan.md).

**This file takes precedence over that plan wherever the two disagree.** The plan is the
as-designed record and is not edited once work starts, the same arrangement
`docs/tracy-integration-log.md` has with `docs/tracy-integration-plan.md`. Read this before
resuming.

The ground rules in `docs/tracy-integration-log.md` govern this work unchanged: keep this log,
commit incrementally with the reason in the body, run `openttd_test` before submitting, never
push.

## Log

### 2026-08-28, phase M0: counters only

Built, measured, gates run. The headline is that M0 answered the question it existed to answer
and the answer changes the plan.

#### What was built

| File | Change |
| --- | --- |
| `cmake/Options.cmake` | `OPTION_TRACY_MEM`, a `FATAL_ERROR` guard against setting it without `OPTION_TRACY`, a `show_options()` line and a warning |
| `src/profiling_mem.cpp` | new: the replaced `operator new`/`delete` family, three counters, and `ScopedProfilerMemoryTick` |
| `src/profiling.h` | `ScopedProfilerMemoryTick` declaration and the `OTTD_MEM_TICK` macro, gated on `WITH_TRACY && OTTD_TRACY_MEM` |
| `src/CMakeLists.txt` | `profiling_mem.cpp` added with `CONDITION OPTION_TRACY_MEM` |
| `src/openttd.cpp` | one `OTTD_MEM_TICK` in `StateGameLoop`, below the paused early return |

A fifth build tree, `build-tracy-mem`, configured `OPTION_TRACY=ON OPTION_TRACY_MEM=ON`, as
the plan called for. It is a capture tree, not a gate tree, and the T5 rule applies to it
unchanged: rebuild it before capturing.

`OPTION_TRACY_MEM_CALLSTACK` was **not** added. The plan lists it, but M3 is what needs it, and
an option that changes nothing is exactly the failure mode the `OPTION_TRACY_DETAIL` guard was
written to prevent. It goes in when it does something.

**The over-aligned operator family is deliberately left alone.** The plan said to replace the
whole family or neither half of any pair; the choice made was to replace the plain, array,
nothrow and sized-delete forms, all backed by `malloc`/`free`, and to leave
`operator new(size_t, align_val_t)` and its partners to the default implementation as an intact
pair on `_aligned_malloc`/`_aligned_free`. Mixing the two allocators is what would break, and
over-aligned allocations are then invisible rather than miscounted, which is the right way for
this to fail. If OpenTTD grows an over-aligned hot type, this is the note to come back to.

#### The numbers

Both fixtures, 2,000 ticks, `build-tracy-mem` RelWithDebInfo, headless null driver, capture
attached. Per-tick figures are the allocations made inside the game loop body, not process-wide.

| | wentbourne | Hilbergen |
| --- | --- | --- |
| vehicle parts | 85,259 | 2,813 |
| allocations per tick, p50 | 29,235 | 65 |
| allocations per tick, mean | **31,416** | **100** |
| allocations per tick, p90 | 39,628 | 198 |
| allocations per tick, p99 | 108,587 | 554 |
| allocations per tick, max | 167,098 | 6,053 |
| bytes per tick, mean | 4,186,041 | 15,148 |
| live blocks, mean | 731,741 | 82,271 |
| total allocations, 2,000 ticks | 62,831,371 | 200,959 |
| total bytes, 2,000 ticks | 8.37 GB | 30.3 MB |

**A factor of 313 between the two fixtures, on a 30x difference in vehicle parts.** Per part per
tick that is 0.37 allocations on wentbourne against 0.036 on Hilbergen, so a 10x gap that scale
does not explain. Hilbergen is not a small version of wentbourne for this question, and any
memory figure quoted without the fixture named is worthless. Same standing correction as the
pathfinder work.

Mean allocation size across the wentbourne run is 133 bytes. These are small objects, not
buffers.

**The volume is not spike-driven.** The 20 ticks at or above p99 hold 4.3% of all allocations on
wentbourne, and they all sit in the first 1.7 seconds of the run, which is savegame load
settling rather than steady state. The median tick already does 29,235 allocations. There is no
version of this that gets cheap by skipping outliers.

**No leak.** `mem.live_blocks` on wentbourne climbs from 544,857 and plateaus around 741,000,
with the last quarter averaging 741,840 against the first quarter's 702,989. Hilbergen is
different and worth a second look later: it climbs from 40,107 to 107,922 and is still climbing
at tick 2,000, a 2.7x rise across the run. That is probably towns and industries growing on a
map that has room to grow, but it is not verified.

#### Road pathfinding is about half of it, and the trace proves it without M2

M0 cannot attribute allocations. The capture can correlate them, which turns out to be enough.
Per-tick allocation counts against per-tick zone totals from the same wentbourne trace, steady
state, ticks 100 to 2,000:

| Signal | r against allocations per tick |
| --- | --- |
| `YapfRoadVehicleChooseTrack` total time | **0.915** |
| `YapfRoadVehicleChooseTrack` call count | 0.536 |
| `YapfShipChooseTrack` total time | 0.144 |
| `YapfRoadVehicleFindNearestDepot` count | 0.077 |
| `YapfTrainChooseTrack` total time | 0.055 |

Over the whole run including the load-time spikes, road `ChooseTrack` time reaches r = 0.966
against allocations and 0.932 against bytes.

**Time correlates and call count does not**, which is the shape the previous entry predicted
without being able to test it. Allocations scale with nodes explored, and so does time; call
count does not, because the search size distribution is enormously wide (p50 42 us, p90 503 us).

Least squares on the steady state gives 3,156 allocations per millisecond of road pathfinding
with an intercept of 14,592, or equivalently 606 allocations per road `ChooseTrack` call. At a
mean 4.74 ms of road pathfinding per tick that attributes roughly 15,000 of the 29,539 mean
allocations per tick to road pathfinding, so **about half**.

The depot search correlating at 0.077 is not a contradiction. Its per-call cost is nearly
constant, so its allocation contribution is nearly constant too and cannot correlate with a
varying total. It is a floor, not a signal.

**The plan's estimate was low.** It guessed 6,000 to 12,000 allocations per tick from road
pathfinding by dividing a sampled time by a guessed cost per `malloc`, and said not to trust it.
The measured figure is about 15,000, so the guess was 25% to 60% short. Right order, wrong
number, which is what that method is worth.

#### Plan change: M2 cannot be a standard-tier feature

This is the decision M0 existed to make.

| Tier | Events per tick, wentbourne |
| --- | --- |
| Standard zones, as instrumented today | 117 |
| **Memory events at M2**, alloc plus free | **~63,000** |
| Detail zones (T4 figures) | ~170,000 |

Memory tracking lands between the two and much closer to the detail tier, and each event is
worse than a zone: Tracy takes its serial lock per memory event (`TracyProfiler.hpp:553`) where
zones use a lock-free per-thread queue.

So `OPTION_TRACY_MEM` is a short-capture tier in the manner of `OPTION_TRACY_DETAIL`, not
something a 20,000 tick run can carry. A few hundred ticks on wentbourne is the working budget.
Scaling the standard tier's 8.6 bytes per zone puts a 2,000 tick wentbourne memory capture near
1.8 GB, and 300 ticks near 270 MB, so trace size alone rules out the long runs.

Hilbergen at 100 allocations per tick is a different story and could carry memory tracking for a
whole 20,000 tick run comfortably. That is not as useful as it sounds, because Hilbergen has
almost no road pathfinding and road pathfinding is half the thing worth looking at.

#### The counters themselves are nearly free

Like-for-like, same fixture, same tick count, same config:

| Fixture | `build-tracy` game loop | `build-tracy-mem` game loop | Delta |
| --- | --- | --- | --- |
| wentbourne, 2,000 ticks | 103,919.2 ms | 104,660.5 ms | +0.71% |
| Hilbergen, 2,000 ticks | 884.4 ms | 889.8 ms | +0.61% |

Both deltas are an upper bound rather than a measurement of the counters, because the
`build-tracy-mem` runs had a live `tracy-capture` attached and the baselines did not. Two relaxed
atomic increments per allocation really is cheap, even at 31,416 allocations per tick.

That also means M0's counters can stay in for M2 and M3 as the cross-check the plan asked for,
at no meaningful cost.

**Do not read the wentbourne row at that precision.** The M1 entry below establishes that this
machine drifted 14.7% across one session on an unchanged binary, which makes a single pair of
two-minute runs worth nothing to two significant figures. The Hilbergen row survives, because
both of its runs take two seconds and were taken seconds apart. The conclusion, that the counters
are cheap enough to keep, is unaffected; the numbers behind it are weaker than they look.

#### Gates

| Gate | Result |
| --- | --- |
| `openttd_test`, `build` with the option off | **PASS**, 2176 assertions in 98 test cases |
| `openttd_test`, `build-tracy-mem` with it on | **PASS**, 2176 assertions in 98 test cases |
| Configure guard, `OPTION_TRACY_MEM` without `OPTION_TRACY` | **PASS**, configure refuses with the FATAL_ERROR |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS**, `015ED3D109C5CCCC`, the recorded baseline |
| State fingerprint, wentbourne, 2,000 ticks | **PASS**, `497945F2C42DED8F`, identical in `build-tracy` and `build-tracy-mem` |
| Interactive run in `build-tracy-mem`, threaded and unthreaded | **PASS.** Run by Sloan; see the follow-up entry below. |

The unit suite passing in `build-tracy-mem` is worth more here than it usually is. Catch2
allocates heavily during test registration, before `main`, which is exactly where a replaced
`operator new` with static initialisation order problems would show up. It is still not evidence
about the balance of the family under Tracy, because M0 reports no Tracy memory events; that
gate arrives with M2.

The wentbourne fingerprint is a second-fixture check the integration log never recorded. Worth
keeping: `497945F2C42DED8F` at 2,000 ticks from `wentbourne.sav`.

**The interactive gate mattered more for this phase than for most**, because the override
changes every allocation in the process, including the draw thread, the GUI and the sound mixer,
and the headless null driver runs none of those. It has since been run and it passed; the entry
below has the numbers.

#### One red herring that cost about forty minutes

Recording it because it will happen again on the next new build tree.

The first `build-tracy-mem` run of the harness reported `exit 1` after 409 seconds with no
output, no stats file, and a Tracy capture containing 0 zones, 0 threads and 0 samples across a
408 second span. It looked exactly like the replaced `operator new` deadlocking the process
during startup, and the process was in fact fully blocked with all threads waiting and flat CPU.

It was a missing graphics set. `build-tracy-mem` had no `opengfx-8.0.tar` in its `baseset`
directory, so OpenTTD failed the graphics set check and put up a modal dialog, which nobody could
click. The title was `Error!` and the text was "Failed to find a graphics set."

Three things conspired to make that unreadable:

- `win32_main.cpp:64` calls `_set_error_mode(_OUT_TO_MSGBOX)`, so a startup failure becomes a
  dialog rather than a line on stderr.
- The process then blocks forever, which presents as a hang with no diagnostic. Redirected
  stdout is block buffered, so even the output that was produced never reached the file.
- The harness checks `Test-Path (Join-Path $buildPath 'baseset')` and the directory exists.
  CMake creates it and populates it with the fonts, `openttd.grf` and the null sound and music
  sets. The graphics set is the one thing not in it, and it is the one thing required.

The way to read a hang like this without a debugger, which is not installed on this machine, is
to enumerate the process's top-level windows and their children and print the text. That found
the answer in one shot after a long detour through allocator theories. Worth remembering: on
Windows a headless OpenTTD that hangs with no output has probably put up a message box.

**Recommendation, not done here because it is not M0:** the harness's baseset check should look
for a graphics set rather than for the directory. Something that fails with "no graphics set in
`<dir>`/baseset, copy the opengfx tar from another tree" would have saved the whole detour.

#### Captures

`benchmark/manual-traces/m0-wentbourne.tracy` and `m0-Hilbergen.tracy`, both 2,000 ticks,
standard tier plus the M0 plots, integrity guard gap zero on each. Reports under
`benchmark/out/` with the `m0`, `m0gate` and `m0base` labels.

#### What M1 needs to know

1. Named pools in `pool_func.hpp` are bounded and cheap, so M1 stands as planned and can be a
   standard-tier feature. Nothing measured here argues against it.
2. M2 needs its own capture discipline written down before it is built, not after: tick budget,
   trace size expectation, and which fixture answers which question.
3. The `mem.live_blocks` climb on Hilbergen is unexplained. It is not urgent and it is not a leak
   in the obvious sense, since the plateau on wentbourne shows the counters behave.

### 2026-08-28, M0 interactive gate: passed, and it corrects one of my claims

Sloan ran the gate in `build-tracy-mem` and reported all four items green, threaded and
unthreaded. Two traces were saved, so most of it can be checked rather than taken on trust:

| Trace | Mode |
| --- | --- |
| `benchmark/manual-traces/m0-manual-wentbourne.tracy` | threaded, the default |
| `benchmark/manual-traces/m0-manual-wentbourne-single-thread.tracy` | `-v win32:no_threads` |

**What the traces confirm directly.**

Threading really was off in the second one. The threaded trace has three threads, `ottd:game`
with 31,624 zones, `Main thread` with 15,059, and an unnamed sound thread with 724. The
unthreaded trace has two, `Main thread` with 73,758 and the sound thread with 1,103, and no
`ottd:game` at all.

Both traces exercise the ground the headless gate structurally cannot. `Drawing`,
`ViewportDrawing`, `Video` and `Sound` all fire, and there are 3,401 and 3,750 lock events
respectively where a null-driver capture has none.

**Gate item 4 is confirmed: all three M0 plots are present in both traces**, with one point per
game loop iteration, 284 in the threaded trace and 417 in the unthreaded one. That was the item
worth checking, because a plot that compiles but never fires is invisible to every other gate.

A nice incidental confirmation of the T3 work: the unthreaded trace records 3,750 lock events and
`get_lock_wait_stats()` returns an empty list, so lock activity with zero contention. That is
exactly what the T3 entry said an unthreaded run should look like, and it had never been checked
from the bindings side.

**What I did not verify.** Items 1 to 3 are Sloan's report, not mine. The framerate comparison
against `build-release`, the two connect and disconnect cycles inside one session, and the clean
exit with no profiler attached leave no trace evidence I can read. Two saved traces do prove at
least two clean connect-and-disconnect cycles across sessions.

#### The correction

`ScopedProfilerMemoryTick` carries a comment, repeated in the M0 entry above, warning that the
counters are process-wide so a threaded interactive run's per-tick delta includes whatever the
draw thread managed between the two samples, and that the figure is exact only for the null
driver. **The warning is technically true and practically negligible.**

| Mode | allocations per tick, mean | min | max |
| --- | --- | --- | --- |
| headless, null driver, 2,000 ticks | 31,416 | 13,018 | 167,098 |
| interactive, threaded, 284 ticks | 30,869 | 15,642 | 51,498 |
| interactive, unthreaded, 417 ticks | 31,080 | 13,845 | 104,605 |

All three sit within 1.7% of each other, and the interactive figures are slightly **lower** than
headless rather than higher, which is the opposite of what I predicted.

The reason is the threading design rather than luck. The draw thread holds `game_state_mutex`
while it draws and the game thread waits on it, so during the game loop body the draw thread is
mostly blocked and not allocating. There is little for it to contaminate the delta with.

The wider point is more useful than the correction: allocation volume is essentially the same
whether or not the process is rendering, so it is the simulation that allocates, not the GUI.
That makes the M2 volume budget in the entry above safe to rely on for interactive captures too,
which I had left open.

The maxima differ for a boring reason. The headless 167,098 and the unthreaded 104,605 are
savegame load settling, which both captures happened to include; the threaded capture was
connected mid-session and its `mem.live_blocks` starts at 736,748 already on the plateau, against
495,659 rising in the unthreaded one.

#### Two smaller things worth keeping

Per-tick game loop cost, for anyone comparing later: 56.1 ms threaded and 55.8 ms unthreaded
against 52.3 ms headless, so interactive is about 7% slower per tick because drawing competes for
the same core. Not a memory-tracking cost.

The integrity guard returns a gap of 0 on the unthreaded trace and **2** on the threaded one.
That is two zones left open when the profiler disconnected mid-session, not a name collision, and
it is the same signature `docs/tracy-mcp.md` already records for a capture that ends inside live
zones. A gap equal to the number of threads with an open zone at disconnect is expected; a gap in
the thousands is the defect that note is really about.

### 2026-08-28, phase M1: named pools

Markers in `Pool::AllocateItem` and `Pool::FreeItem` and in the Squirrel allocator. This is the
first phase that reports anything to Tracy's memory subsystem, so it is the first one that can
be killed by an unbalanced pair. It was not.

#### What was built

| File | Change |
| --- | --- |
| `src/profiling.h` | `OTTD_MEM_ALLOC_N` and `OTTD_MEM_FREE_N` wrapping `TracyAllocN`/`TracyFreeN` |
| `src/core/pool_func.hpp` | one marker in `AllocateItem`, one in `FreeItem` |
| `src/script/squirrel.cpp` | markers in `ScriptAllocator::DoAlloc` and `::Free`, plus the `SQUIRREL_MEM_POOL` name constant |

Two lines in one template gives a Tracy memory pool per object pool, which is the best coverage
per line changed in this plan and the same argument that put T2 early.

#### Three deviations from the plan, all deliberate

**1. Gated on `OTTD_TRACY_MEM`, not the standard tier.** The plan put named pools on
`WITH_TRACY` on the grounds that their volume is bounded, which is true and is confirmed below.
But volume is about cost and the thing that actually bites here is risk: an unbalanced pair
terminates the capture, and `build-tracy` is the interactive tree and a gate tree under decision
D2. Keeping every memory event in one tree means a pairing bug can only ruin `build-tracy-mem`
captures. Promotion later is one line in `profiling.h`, and the volume figures below say it would
be safe. I would rather do it after M2 and M3 have shaken the pairing out.

**2. No discard marker anywhere.** The plan suggested `TracyMemoryDiscard` for `Pool::CleanPool`.
Reading the code says otherwise: `CleanPool` reaches every live item through
`delete this->Get(i)`, which lands in `PoolItem::operator delete` and then `FreeItem`, so every
slot is already reported freed one at a time. A discard on top would report each of them freed
twice and Tracy would drop the connection. The `Tcache` drain that follows deallocates blocks
whose frees were already reported, and needs no marker either. So M1 ships no discard macro at
all, because there is nowhere correct to put one.

**3. One `Squirrel` pool, not one per VM.** Tracy keys a memory pool on the *address* of its name
string, so the name must be a compile-time constant, and there is one `ScriptAllocator` per
Squirrel instance. All VMs therefore share a pool. It is also why the name is a named `static
const char[]` rather than a repeated `"Squirrel"` literal: without string pooling two identical
literals are two addresses and would show up as two pools with the same name.

#### Volume, and the tier question

wentbourne, 2,000 ticks, headless. 20 pools registered, 481,236 allocation records, so **241
allocations per tick** and roughly 482 memory events per tick counting the frees.

| Pool | allocations | share | element size |
| --- | --- | --- | --- |
| `CargoPacket` | 359,133 | 74.6% | 40 |
| `Vehicle` | 84,608 | 17.6% | 632, 624, 568 |
| `CargoPayment` | 14,704 | 3.1% | 40 |
| `OrderList` | 13,899 | 2.9% | 56 |
| `Station` | 2,648 | 0.6% | |
| `Depot` | 2,015 | 0.4% | |
| everything else | 4,229 | 0.9% | |

Set against M2's ~63,000 events per tick, M1 is two orders of magnitude cheaper and only about
twice the standard zone tier's 117 per tick. **On volume alone M1 belongs on the standard tier**,
which is what the plan said. Deviation 1 above is a risk decision, not a cost one, and the
numbers do not support keeping it gated forever.

`CargoPacket` dominating by three quarters is the `Tcache` pool doing exactly what the marker was
warned about: those 359,133 events are slot cycles on the free list, not heap traffic. Median
lifetime is under a millisecond, so cargo packets are created and destroyed inside a tick.
`Vehicle`'s median lifetime is the whole run, which is the savegame load allocating them and
`CleanPool` freeing them at exit.

#### The pairing holds

Every pool reports allocations and frees in equal numbers with nothing live at capture end:

| Pool | allocations | freed | live at end |
| --- | --- | --- | --- |
| `CargoPacket` | 359,133 | 359,133 | 0 |
| `Vehicle` | 84,608 | 84,608 | 0 |
| `CargoPayment` | 14,704 | 14,704 | 0 |
| `OrderList` | 13,899 | 13,899 | 0 |

A completed capture is the real proof, since Tracy drops the connection the moment it sees a free
it cannot match. The 2,000 tick run finished with 233,147 zones and an integrity gap of zero.

`(default)` shows zero events, confirming that M0's global counters still report nothing to Tracy
and that the named pools are the only source. That also means the plan's warning about pool
figures being a second accounting of bytes the default pool already counted does not bite until
M2.

#### A real caveat: usage and high water marks are meaningless here

Every pool except `Engine` reports `usage` of 0 at capture end, and `Vehicle` reports 0 despite
85,259 vehicle parts existing throughout. That is not a bug in the markers.

`TRACY_ON_DEMAND` means Tracy only records from the moment the profiler connects. Pools populated
by savegame load before or during connection have allocations Tracy never saw, and the on-demand
relaxation forgives the unmatched frees rather than counting them. What survives is churn, which
is what we wanted, but **occupancy and the high water mark are not trustworthy for any pool
filled at load**. The plan claimed a `Vehicle` memory pool would turn the harness's static
88-byte `BaseConsist` figure into a churn figure; churn yes, occupancy no.

The `high` and `low` fields the bindings return are pointer addresses bounding the pool's memory
map, not byte totals. Easy to misread as sizes, and I did for a minute.

`callstack_idx` is 0 on every event, as designed. `TracyAllocN` captures no stack; that arrives
with M3 and the `S` forms.

#### The Squirrel pool needed a different test

The headless benchmark runs no AI and no GameScript, so the `Squirrel` pool recorded zero events
in the wentbourne capture and the marker was completely unexercised by it.

The regression suite is the right test, and it is the project's own safety net for the Script API,
which makes it the correct gate for a change to `squirrel.cpp` anyway. All four suites pass in
`build-tracy-mem`. A capture of one 30,000 tick regression run shows **13,986 Squirrel
allocations, 2.67 MB, every one freed by the end**, across sizes from 32 to 160 bytes. That
exercises VM teardown as well as steady-state churn.

Getting there needed a detour. `regression/regression.cfg` is absent from every build tree except
`build`, so `Regression.cmake` could not find the config it passes with `-c`, OpenTTD fell back to
the OS locale, and the whole suite failed on `Power Station` against `Power Plant` and
`Railway construction` against `Railroad construction`. It reads like a catastrophic regression
and is entirely a missing config file. `cmake --build <tree> --config <cfg> --target
regression_files` places it. Worth knowing before the next person runs the suite outside `build`.

#### Gates

| Gate | Result |
| --- | --- |
| `openttd_test`, `build` with the option off | **PASS**, 2176 assertions in 98 test cases |
| `openttd_test`, `build-tracy-mem` | **PASS**, 2176 assertions in 98 test cases |
| Regression suites, `build-tracy-mem` | **PASS**, all four |
| State fingerprint, wentbourne, 2,000 ticks | **PASS**, `497945F2C42DED8F` |
| Capture survives a full run | **PASS**, 2,000 ticks, gap zero |
| Interactive run | **PASS.** Run by Sloan; see the follow-up entry below. |

Trace size for the same 2,000 tick wentbourne run goes from 2.0 MB at M0 to 6.1 MB at M1.

#### Overhead could not be measured, and that corrects the M0 entry too

I could not get a usable number, and the reason is worth recording so nobody trusts the ones
already written down.

| Time | Tree | wentbourne 2,000 tick game loop |
| --- | --- | --- |
| 12:32 | `build-tracy`, no memory tracking | 103,919 ms |
| 12:28 | `build-tracy-mem`, M0 | 104,660 ms |
| 13:47 | `build-tracy-mem`, M1, capture attached | 107,297 ms |
| 13:52 | `build-tracy-mem`, M1, no capture | 126,092 ms |
| 13:55 | `build-tracy-mem`, M1, no capture | 125,772 ms |
| 13:57 | `build-tracy-mem`, M1, no capture | 123,241 ms |
| 14:00 | `build-tracy-mem`, M1, capture attached | 127,871 ms |
| **14:05** | **`build-tracy`, no memory tracking, unchanged binary** | **119,159 ms** |

That last row is the point. The same binary that ran in 103,919 ms at 12:32 took 119,159 ms at
14:05, **14.7% slower with nothing changed**. This is an ordinary desktop with Discord, Steam,
Slack and an Epic launcher on it, and the harness README already says timings from an
instrumented tree are indicative only.

Within the later window M1 sits about 3.4% above the no-memory baseline, which for 482 events per
tick would be roughly 4 microseconds per event and is not credible for a lock and a queue write.
Most of that 3.4% is drift too. The honest statement is that M1's cost is below what this method
can resolve here.

**Consequently the M0 entry's "+0.71%" for wentbourne should not be read at that precision.** It
was a single pair of runs four minutes apart, and 15% drift across a session makes two
significant figures meaningless. The Hilbergen pair in that entry, 884.4 ms against 889.8 ms, is
better evidence because both runs take two seconds and sit seconds apart, so drift has no time to
act. Read M0 and M1 as "small enough not to matter" and, if a real number is ever needed, measure
it on a quiet machine with the two conditions interleaved.

#### Captures

`benchmark/manual-traces/m1-wentbourne.tracy` (2,000 ticks, standard tier plus M0 plots and M1
pools) and `m1-regression.tracy` (one regression run, for the Squirrel pool).

#### What M2 needs to know

1. The pairing discipline works. Two markers placed where the pairing is guaranteed by
   construction produced 481,236 matched pairs and four green regression suites. M2's global
   override is a much wider surface and the same discipline will not carry it automatically.
2. On-demand mode makes occupancy unusable, and that applies to the default pool at M2 as well.
   Every allocation live at connection time is invisible. If occupancy ever matters, it needs a
   `TRACY_NO_EXIT` scratch build and a capture that starts before the savegame loads.
3. Whether to promote M1 to the standard tier is a decision waiting on M2 and M3, not on data.

### 2026-08-28, M1 interactive gate: passed, and it found something better than a bug

Sloan ran the gate and saved `benchmark/manual-traces/m1-wentbourne-manual.tracy`, 793 game loop
iterations across 46.8 seconds of a windowed session with a draw thread, sound, an AI, and the
profiler connecting and disconnecting.

Nothing is wrong with it. Integrity gap zero, 139,042 zones, 9,522 lock events, all nineteen
active memory pools reporting allocations and frees in equal numbers. The capture survived, which
is what a memory phase's gate is really testing.

#### Per-tick allocation counts are deterministic

This was not what I went looking for. Comparing `mem.allocs_per_tick` tick for tick against the
headless null-driver capture of the same savegame:

| | |
| --- | --- |
| ticks compared | 793 |
| **exactly equal** | **782** |
| within 1% | 792 |
| interactive mean | 33,932 |
| headless mean over the same first 793 ticks | 33,934 |

Two allocations per tick apart on the mean, and four fifths of the individual ticks are bit
identical, between a headless run with no video driver and a windowed session that is also
drawing, mixing sound and running a Squirrel VM.

The eleven ticks that differ are more interesting than the 782 that do not, because their
differences **cancel in pairs**: +310 at tick 300 against -310 at 319, -192 at 445 against +192
at 448, +181 at 601 against -181 at 633. That is the same allocations happening a few ticks
either side, not different allocations. Asynchronous work landing on a different tick depending
on thread scheduling, and the link graph is the obvious candidate since its jobs complete on a
worker thread. The two genuine outliers are tick 0, off by 1,630, which is startup, and three
ticks off by exactly 12.

Three things follow.

**The M0 interactive entry understated its own result.** It concluded the GUI does not allocate
inside the game loop from three means agreeing within 1.7%. The real statement is much stronger:
the individual ticks agree exactly. The draw thread contributes essentially nothing to the delta,
not merely little on average.

**Allocation profiling is reproducible.** A before-and-after comparison in M2 or M3 can be read at
single-allocation granularity rather than statistically, which is worth a great deal when the
point of M2 is to attribute allocations to code and then change that code.

**Allocation count tracks game state.** It is deterministic in the same way and for the same
reason the state fingerprint is. I am not proposing it as a desync check, and the eleven
scheduling-sensitive ticks would have to be understood first, but it is worth writing down.

#### One pool has live allocations at the end, and it is the interesting one

| Pool | allocations | freed | live at end | usage |
| --- | --- | --- | --- | --- |
| `Engine` | 1,024 | 768 | **256** | 69,632 bytes |
| every other pool | | | 0 | 0 |

Not a leak. `Engine` is the only pool whose objects are created **after** the profiler connects,
so it is the only one where Tracy sees a complete lifecycle. Everything else was populated by
savegame load before or during connection, which is exactly why they all read zero live and zero
usage, as the M1 entry above warned. 1,024 being four times 256 says the engine table was built
four times and torn down three, which is what loading a game a few times in one session looks
like.

So `Engine` is the one pool that demonstrates the whole path working, including `CleanPool`
reporting each slot freed through `PoolItem::operator delete`. It is also the answer to anyone
who reads the zero-usage columns and concludes the markers are broken.

#### The Squirrel marker is exercised here, unlike in the benchmark

10,119 allocations, 1.74 MB, every one freed, sizes clustered at 32, 144, 160 and 88 bytes,
median lifetime 0.258 ms and a longest of 46 seconds, which is the session. The headless benchmark
records zero because it runs no scripts, so between this and the regression capture the marker is
now covered by two independent runs.

#### Two notes, neither a problem

**Sampling was off.** The capture has zero callstack samples, because sampling on Windows needs an
elevated profiler and this one was not. It does not matter for this gate, and it is worth knowing
before anyone tries to read sampled stacks out of this trace.

**Tracy invents a memory usage plot per named pool.** The plot list in this trace suddenly has
nineteen extra entries, `CargoPacket` alone carrying 380,935 points, which is two per memory
event. They cost nothing on disk: `TracyWorker.cpp:8598` explicitly skips `PlotType::Memory` when
writing a trace and rebuilds them from the events on load. Checked rather than assumed, because
two plot points per allocation would have been a real problem for M2.

### 2026-08-28, phase M2: the global heap tier

M0's counters now also report to Tracy's default memory pool. This is the phase the whole plan
was built around and the one most likely to fail, and it did not.

#### What was built

| File | Change |
| --- | --- |
| `cmake/Options.cmake` | `OPTION_TRACY_MEM_GLOBAL`, a `FATAL_ERROR` guard requiring `OPTION_TRACY_MEM`, a status line and a volume warning |
| `CMakeLists.txt` | `OTTD_TRACY_MEM_GLOBAL` and `SKIP_PRECOMPILE_HEADERS` set on `profiling_mem.cpp` alone |
| `src/profiling.h` | `OTTD_MEM_ALLOC` and `OTTD_MEM_FREE` wrapping `TracyAlloc`/`TracyFree` |
| `src/profiling_mem.cpp` | the tier switch and the two reporting calls |

**Three options now, where the plan had two.** `OPTION_TRACY_MEM` covers M0's counters and M1's
named pools, both of which are cheap enough for long captures; `OPTION_TRACY_MEM_GLOBAL` covers
this tier, which is not. Folding them together would have made M1 unusable for anything but a
short run, which is the opposite of what the M1 entry concluded about its volume.

**The define is scoped to one translation unit** rather than added with `add_definitions`, so
switching the tier rebuilds `profiling_mem.cpp` and relinks, instead of rebuilding the tree twice
over as `OPTION_TRACY_DETAIL` does. Verified: three files touched on the switch. That is why
there is no sixth build tree. It costs one opt-out from the precompiled header, because MSVC
rejects a translation unit whose definitions differ from the ones the PCH was built with.

Reporting order is not symmetric and the comment in `profiling.h` says why. An allocation is
reported after `malloc` returns; a free is reported **before** `free` releases the address.
Release first and another thread can win the same address and report its allocation before this
thread reports the free, which Tracy reads as one address allocated twice and terminates the
capture over.

#### The bug I nearly recorded a result from

The first M2 capture looked fine. It ran, it finished, the fingerprint matched, 38,481 zones,
gap zero. It was also completely wrong: the global tier had never been compiled in.

`set_source_files_properties` had gone into `src/CMakeLists.txt` next to the `add_files` call for
the same source, which is the obvious place and the wrong one. **Source file properties are
visible only in the directory scope that sets them**, and `openttd_lib` is declared in the top
level `CMakeLists.txt`, so neither the define nor the PCH opt-out reached the compiler. No error,
no warning. `grep -c OTTD_TRACY_MEM_GLOBAL build-tracy-mem/openttd_lib.vcxproj` returned 0.

Two things gave it away, and neither was the build:

- Tracy's `(default)` memory pool had **zero** events after a run that the M0 counters said made
  11,998,045 allocations.
- The trace was 2.18 MB. With the tier actually on it is 109.32 MB.

The fix is one block moved into the top level file, immediately below the identical
`set_source_files_properties` call that `src/3rdparty/fmt/format.cc` already uses. That existing
line was sitting there as a worked example the whole time.

That is the second silent no-op in this work, after the baseset directory check that passed on an
empty baseset. Both had the same shape: a check or a setting that looks right, produces no error,
and quietly does nothing. **Verify that a build option reached the compiler**, not just that the
build succeeded.

#### It balances

wentbourne, 300 ticks, headless, with the tier live:

| | |
| --- | --- |
| default pool allocations | **12,971,700** |
| M0 counters, summed over the game loop bodies | 11,998,045 |
| difference | 973,655, or 7.5% |
| zones | 38,533, integrity gap 0 |
| bytes live in the default pool at capture end | 7,976,126 |

The capture completed, which is the proof that matters: Tracy drops the connection the instant it
sees a free it cannot match, and 26 million events went through without one.

The two counts reconcile in the right direction and by a sensible amount. Tracy sees every
allocation in the process; the M0 plots only see the ones inside the game loop body. The 973,655
difference is savegame load, the null driver's own work and the save at exit. This is the
cross-check the plan asked for, and it is the first time the two readers have been compared.

#### The finding: 81% of allocations are pathfinder nodes

In a steady-state window of 0.606 seconds, about 7 ticks, taken 3 seconds into the capture:

| Allocation size | count | share |
| --- | --- | --- |
| **48 bytes** | **470,123** | **81.0%** |
| 40 bytes | 67,854 | 11.7% |
| 64 bytes | 6,543 | 1.1% |
| everything else | 36,095 | 6.2% |

48 bytes is `CYapfRoadNode`. Inside `YapfRoadVehicleChooseTrack` specifically, 448,098 of 454,126
allocations are exactly 48 bytes, so **98.7% of what road pathfinding allocates is nodes**. The
40-byte band is `CargoPacket`, which the M1 named pool confirms independently at the same size.

This matters because of how the pathfinder entry got there. It reasoned from MSVC's `deque`
source, where `_Block_size` is 1 for any element over 8 bytes, to "every YAPF node is an
individual `malloc`", and supported it with sampled time sitting inside the allocator. That was
inference. This is the measurement, and the inferred 48-byte figure derived by hand from the
struct layout turns out to be exactly right.

Attribution by zone in the same window:

| Zone | calls | allocations | share |
| --- | --- | --- | --- |
| `YapfRoadVehicleChooseTrack` | 443 | 454,126 | 78.2% |
| `YapfShipChooseTrack` | 276 | 56,834 | 9.8% |
| `YapfRoadVehicleFindNearestDepot` | 98 | 18,940 | 3.3% |
| `YapfTrainChooseTrack` | 103 | 6,395 | 1.1% |

**Do not read 78.2% as the whole-run share.** This window's road searches average 510.8 us
against the 165.6 us whole-run mean from the T5 trace, so it is roughly three times busier than
typical and was chosen for convenience rather than representativeness. The M0 regression put road
pathfinding at about half of the run, and a per-millisecond figure from this window puts it
nearer a third. Somewhere between a third and a half, and pinning it down needs a longer M2
capture or M3's hot-spot tree.

The two figures that do travel are per-call ones: **1,025 allocations per road search** in this
window, and **2,007 allocations per millisecond of road pathfinding**. Roughly one allocation per
node explored, plus a handful of container growths per search, visible as single allocations of
16,423, 512, 256, 128 and 64 bytes at about one per call.

#### Cost

Three runs inside seven minutes, which is as controlled as this machine gets:

| Tree and tier | wentbourne, 300 ticks | vs baseline |
| --- | --- | --- |
| `build-tracy`, no memory tracking | 19,792.7 ms | |
| `build-tracy-mem`, counters and named pools | 19,889.8 ms | +0.5% |
| `build-tracy-mem`, global tier live | 24,853.0 ms | **+25.6%** |

Single runs, so read the +0.5% as "lost in the noise" rather than as a measurement. The +25.6%
is large enough to survive this machine's drift and is the honest price of the tier: it is what a
lock acquire and a queue write per allocation costs at 40,000 allocations per tick.

#### Trace size, and a correction to M0

109.32 MB for 300 ticks, so 364 KB per tick. The M0 entry estimated 270 MB for 300 ticks by
scaling the standard tier's bytes per zone, and that was **2.5 times too pessimistic**. Tracy
compresses memory events better than zones.

The practical budget is therefore looser than M0 claimed. A 1,000 tick wentbourne capture lands
near 360 MB and a 2,000 tick one near 730 MB, both of which the bindings have already handled at
larger sizes. Twenty thousand ticks would be 7 GB and is still out.

#### Gates

| Gate | Result |
| --- | --- |
| `openttd_test`, `build` with the options off | **PASS**, 2176 assertions in 98 test cases |
| `openttd_test`, `build-tracy-mem` with the tier on | **PASS**, 2176 assertions in 98 test cases |
| Configure guard, `OPTION_TRACY_MEM_GLOBAL` alone | **PASS**, configure refuses |
| State fingerprint, wentbourne, 300 ticks | **PASS**, `21135C81503802F4`, identical to `build-tracy` |
| Capture survives a full run | **PASS**, 12,971,700 allocations, no termination |
| Interactive run | **PASS.** Run by Sloan; see the follow-up entry below. |

The unit suite is worth more than usual again. Catch2 allocates heavily before `main`, and the
tier is live in that binary, so a badly ordered or unbalanced pair had every chance to show.

#### A method note for anyone reading a big memory trace

`get_memory_events` takes a maximum count and no offset, so only a prefix of the events is
reachable through the bindings. On this trace 4,000,000 events came back in 1.8 seconds and
covered the first 3.49 seconds of a 28 second capture, nearly all of it savegame load.

Steady-state analysis therefore has to work inside a window: pull a prefix long enough to reach
past the load phase, drop everything before a cutoff, and bucket what remains against zone
occurrences. That is what produced the numbers above, and it is why they carry a window caveat
that a whole-run figure would not.

#### What M3 needs to know

1. The tier holds up. Thirteen million events, no unmatched pair, a fingerprint that matches and
   a capture that completes.
2. M3 adds a stack walk to every one of those events. At 40,000 allocations per tick against the
   +25.6% this tier already costs, expect the callstack tier to be far more expensive again and
   plan captures in tens of ticks rather than hundreds.
3. The hot-spot tree is what settles the whole-run attribution question this entry had to leave
   open, and `get_memory_events` already returns a `callstack_idx` that
   `get_callstack_frames` resolves. The plumbing on the analysis side is done.

### 2026-08-28, M2 interactive gate: passed, and it exposed a second bindings defect

`benchmark/manual-traces/m2-wentbourne-manual.tracy`, 695 game loop iterations over 44.6 seconds,
224 MB. **26,627,684 allocations reported to the default pool with no unmatched pair**, roughly
twice the headless run's count, integrity gap zero, capture completed. The tier survives a real
session with a draw thread, sound, an AI and profiler connects and disconnects.

#### `get_zone_occurrences_with_thread` returns the wrong thread

This took a while to unpick because it presents as the trace being wrong rather than the reader.
`GameLoop`, `CallVehicleTicks` and `YapfRoadVehicleChooseTrack` all appeared to run on a thread
named `ottd:linkgraph` that had zero zones, while `ottd:game` sat there with 80,933 zones and
nothing attributed to it.

The trace is fine. From `python/bindings/ServerModule.cpp:484`:

```cpp
const uint16_t tidx = ztd.Thread();
const uint64_t tid = ( tidx < threads.size() && threads[tidx] ) ? threads[tidx]->id : 0;
```

`ztd.Thread()` is Tracy's **compressed** thread index and `threads` is `GetThreadData()`. Those
are different index spaces, and the binding uses one to subscript the other. The correct call is
`w.DecompressThread( tidx )`, which `TracyWorker.hpp:664` provides for exactly this.

On this trace the two spaces happen to differ by one, so every thread attribution lands on the
next thread in the list. Verified three ways once the offset was applied: `GameLoop` resolves to
`ottd:game`, `Drawing` and `ViewportDrawing` to `Main thread`, and `Sound` to the unnamed thread
whose 1,909 zones match the 1,910 `Sound` zones. Do not rely on the offset being one on another
trace; it is a coincidence of ordering, not a fixed relationship.

**`get_memory_events` has the same problem and does not even try.** `ServerModule.cpp:734` writes
`d["thread_alloc"] = (uint32_t)ev.ThreadAlloc()`, which is the raw compressed index with no
decompression at all, despite the surrounding comment describing it as an OS thread id.

`get_threads()` is correct. It reads `t->id` directly and asks `GetThreadName` about that same
id, and its per-thread zone counts sum exactly to the trace total.

That is the second real defect in these bindings after the `get_all_zone_stats()` name collision,
and it has the same character: a plausible-looking result, no error, and nothing to warn you.
Recorded in `docs/tracy-mcp.md` next to the first.

#### Memory tracking finds threads that no zone ever will

Ten threads in this trace where the M1 interactive capture showed four. The new ones are
`ottd:dmusic` and **six `ottd:linkgraph` threads, all with zero zones**.

They were always there. Tracy registers a thread the first time it sees any event from it, and
until M2 these threads produced no events at all, because nothing in `src/linkgraph` carries a
zone. T3 instrumented the link graph's job count as a plot and T4 and T5 never went near the
worker threads.

So memory tracking doubles as thread discovery, and it has just pointed at a subsystem that runs
on six threads with no instrumentation whatsoever. Worth remembering when the linkgraph next
comes up as a cost.

#### Where the allocations happen

Steady-state window of 0.63 seconds, thread indices resolved through the offset above:

| Thread | allocations | share | bytes |
| --- | --- | --- | --- |
| `ottd:game` | 864,373 | 98.3% | 89,686,203 |
| `Main thread` (drawing) | 15,174 | 1.7% | 902,218 |

Whole run, the default pool holds 26,627,684 allocations against 23,999,881 summed from
`mem.allocs_per_tick`, so **2,627,803 or 9.9% happen outside the game loop body**, against 7.5%
on the headless run. The extra is drawing, GUI and sound.

That is the complement of the M1 interactive finding rather than a contradiction of it. The GUI
allocates plenty; it just does not allocate *inside* the game loop, which is why per-tick counts
came out identical between a headless run and a windowed one.

`mem.allocs_per_tick` peaks at **167,098** again, the third run in a row to hit that exact figure
on this savegame.

#### Cost, with the usual caveat

`GameLoop` averages 57.8 ms per tick here against 53.2 ms in the M1 interactive capture, so about
9% for the tier. That is well below the 25.6% measured headless, and I would not read anything
into the gap: the two captures are half an hour apart on a machine that has already drifted 15%
in a session, and the interactive ones differ in how much drawing happened. The headless number
is the one with a controlled comparison behind it.

#### Gates

All four items reported green by Sloan. What the trace confirms directly: the capture completed
with 26.6 million memory events and no termination, `Drawing`, `ViewportDrawing`, `Video` and
`Sound` all fire, 8,346 lock events, and the M0 plots and M1 pools are all present alongside the
default pool. Items 1 to 3, the framerate comparison, the two connect and disconnect cycles and
the clean exit, leave no trace evidence and are Sloan's report.

### 2026-08-28, phase M3: allocation call stacks

The last phase in the plan. Every allocation now carries a stack, which is what turns M2's
thirteen million anonymous events into an answer.

#### What was built

`OPTION_TRACY_MEM_CALLSTACK`, requiring `OPTION_TRACY_MEM_GLOBAL`, with the same
`FATAL_ERROR` guard and the same per-file define mechanism M2 uses, so switching it still
rebuilds one translation unit. `REPORT_ALLOC` becomes `TracyAllocS` at depth 16.

**Stacks on allocation only.** `REPORT_FREE` stays on plain `TracyFree`. A free's stack says
where memory was released; the hot-spot tree is built from where it was requested, and capturing
both would double the most expensive thing in the file for something nothing here asks about.
Tracy accepts the mixture, since a callstack-carrying allocation and a plain free are independent
event types. The plan said "the `S` forms everywhere" and this is narrower on purpose.

**The named pools keep plain markers too.** A stack on a `Pool::AllocateItem` allocation would
say which code created the vehicle, which is mildly interesting and not what M1 is for. The
pools already know exactly where they came from.

#### Depth 16 is generous, not tight

The plan guessed 16 and said to check rather than trust it. Checked: **16 return addresses
resolve to 26 frames**, because inline expansion happens at symbol resolution rather than
capture, and they reach all the way from the allocation to `WinMain` and `BaseThreadInitThunk`.
Nothing is truncated.

Four of those frames are overhead that no analysis wants: `tracy::Callstack`,
`tracy::Profiler::MemAllocCallstack`, and this file's own `CountedAlloc` and `operator new`. So
the useful depth is nearer twelve, and it was still enough to reach past the pathfinder entry
points into `GameLoop` itself.

Symbol resolution gives file and line for our code and for the standard library, so a stack reads
`NodeList<CYapfRoadNode,8,10>::CreateNewNode` at `nodelist.hpp:70` rather than an address.

#### The answer

wentbourne, 30 ticks, allocations made inside the game loop, attributed to the nearest
recognisable subsystem entry in each stack. 1,668,441 allocations across 1,171 distinct stacks:

| Attributed to | allocations | share | bytes |
| --- | --- | --- | --- |
| `YapfRoadVehicleChooseTrack` | 1,250,835 | **75.0%** | 108,314,438 |
| `YapfShipChooseTrack` | 218,025 | 13.1% | 30,195,487 |
| `YapfRoadVehicleFindNearestDepot` | 42,146 | 2.5% | 6,435,383 |
| `FormatString` | 21,050 | 1.3% | 842,000 |
| `YapfShipCheckReverse` | 19,560 | 1.2% | 1,894,014 |
| `YapfTrainChooseTrack` | 14,528 | 0.9% | 4,833,217 |
| `YapfShipRegions` | 12,325 | 0.7% | 18,728,546 |
| `StationFinder::{ctor}` | 7,175 | 0.4% | 287,000 |
| **all YAPF entry points** | **1,563,747** | **93.7%** | |

**Ninety-four percent of what the game loop allocates is the pathfinder**, and the single hottest
stack, 73.7% of everything on its own, is:

```
NodeList<CYapfRoadNode,8,10>::CreateNewNode
  std::deque<CYapfRoadNode>::emplace_back
    std::deque<CYapfRoadNode>::_Emplace_back_internal
      std::allocator<CYapfRoadNode>::allocate
```

That is the finding from the pathfinder dig, arrived at a fourth time by a fourth method. The dig
inferred it from MSVC's `deque` source and sampled time inside the allocator; M2 confirmed the
48-byte size signature; M3 names the call stack.

#### Two smaller things worth a look

`YapfShipRegions` allocates 12,325 times for 18.7 MB, so **1,519 bytes per allocation** against
the 86-byte average everywhere else. It is 0.7% of the count and 10.5% of the bytes. Ship region
pathfinding allocates in large blocks and nothing has ever looked at it.

`StationFinder`'s constructor builds a `std::set<Station *>` and it is called from
`TileLoop_Town`, so there is a tree allocation per town tile loop iteration. Only 0.4%, but it is
the kind of thing that is trivially avoidable and would never have been found any other way.

`FormatString` and the Uniscribe text shaping entries together come to about 3.5% in a **headless
run with the null driver**. Not investigated. Worth knowing that string formatting and text
layout allocate during ticks at all.

#### The share question the earlier phases left open

Three methods have now estimated road pathfinding's share of allocations and they do not agree:

| Method | window | road `ChooseTrack` share |
| --- | --- | --- |
| M0 regression on per-tick counts | 1,900 ticks | about 50% |
| M2 size and zone attribution | 7 ticks | 78.2% |
| M3 call stacks | 30 ticks | 75.0% |

The disagreement is not a contradiction, it is the sampling. M2 and M3 both use short windows
early in the run where road pathfinding is unusually busy, and M0's regression covers 1,900 ticks
including quiet ones. The M0 figure is the one to quote for a whole run.

What M3 settles is the part that was actually in doubt: **where** the allocations come from.
That is now a call stack rather than an inference, and no window choice changes it.

#### Cost

| Tree | wentbourne, 30 ticks | vs baseline |
| --- | --- | --- |
| `build-tracy`, no memory tracking | 2,414.2 ms | |
| `build-tracy-mem`, M3 | 5,142.9 ms | **+113%** |

The callstack tier slightly more than doubles the game loop, against M2's +25.6%, so the stack
walk alone is worth about 1.7x on top of the events. Two runs two minutes apart.

Two operational limits matter more than the percentage:

- **64.22 MB for 30 ticks**, or 2.14 MB per tick, against M2's 364 KB. Callstacks multiply the
  trace by six.
- **The client outruns the transport.** `tracy-capture` reported 23.7 seconds elapsed to drain an
  8.02 second span, so the capture takes roughly three times the wall clock of the run itself.
  Budget for that, and do not read the extra time as the game being slow.

Thirty ticks is the right size for this tier. A hundred would be 214 MB and several minutes of
draining for nothing the thirty did not already say.

#### Gates

| Gate | Result |
| --- | --- |
| `openttd_test`, `build` with the options off | **PASS**, 2176 assertions in 98 test cases |
| `openttd_test`, `build-tracy-mem` with callstacks on | **PASS**, 2176 assertions in 98 test cases |
| Configure guard, `OPTION_TRACY_MEM_CALLSTACK` alone | **PASS**, configure refuses |
| State fingerprint, wentbourne, 30 ticks | **PASS**, `31B1257FC65D1424`, identical to `build-tracy` |
| Capture survives a full run | **PASS**, 4,512,746 allocations, every one with a stack |
| Interactive run | **PASS.** Run by Sloan; see the follow-up entry below. |

#### The plan is complete

M0 through M3 are built, gated and recorded. What the plan listed as deferred stays deferred, and
one thing has been added to it:

1. **Replacing the YAPF node `deque` with a chunked arena.** This is now the obvious change and
   four independent measurements point at it. It is a fix, not instrumentation, and belongs in
   its own change with its own fingerprint run.
2. **`YapfShipRegions`' 1,519-byte allocations** and `StationFinder`'s per-tile-loop `std::set`,
   both new here.
3. **Promoting M1's named pools to the standard tier**, still a decision rather than a
   measurement. M2 and M3 have now shaken the pairing out across four captures without a single
   unmatched event, so the argument for keeping them gated is weaker than it was.

### 2026-08-28, M3 interactive gate: passed, and it found the same bug in the draw path

`benchmark/manual-traces/m3-wentbourne-manual.tracy`, 585 game loop iterations over 50.3 seconds,
244 MB. **23,107,549 allocations, every one carrying a call stack, with no unmatched pair.**
Integrity gap zero, 7,026 lock events, the capture completed through connects and disconnects.
`mem.allocs_per_tick` peaks at 167,098 for the fourth run running.

This is the trace that justifies running the gate at all rather than treating it as a formality.
Everything M3 measured headless was the game loop. The draw path only exists here.

#### The viewport sprite sorter allocates 1,859 times per frame

Allocations that fall outside a `GameLoop` zone, attributed by call stack:

| Nearest real caller | allocations | share | bytes |
| --- | --- | --- | --- |
| `ViewportSortParentSpritesSSE41` | 16,735 | **71.2%** | 440,597 |
| `Blitter_32bppOptimized::EncodeInternal<0>` | 3,486 | 14.8% | 70,695,776 |
| Uniscribe text shaping, four entries | 1,747 | 7.4% | 68,132 |
| `UniquePtrSpriteAllocator::AllocatePtr` | 296 | 1.3% | 25,654,531 |
| `DecodeSingleSprite` | 291 | 1.2% | 342,259 |

The first one is the finding. `ViewportSortParentSpritesSSE41`
(`viewport_sprite_sorter_sse4.cpp:29`) constructs three containers fresh on every call:

```cpp
std::stack<ParentSpriteToDraw *> sprite_order;
std::forward_list<std::pair<int64_t, ParentSpriteToDraw *>> sprite_list;
std::vector<ParentSpriteToDraw *> preceding;
```

and the size histogram matches the source exactly. 9,776 allocations of 24 bytes are
`forward_list` nodes, one per sprite, which that container cannot avoid. 5,794 of 16 bytes are
the `stack`'s backing `std::deque`: `ParentSpriteToDraw *` is 8 bytes, so **MSVC's `_Block_size`
is 2** and the deque allocates once per two elements pushed, plus a proxy per construction. The
64, 128 and 256 byte allocations are `preceding` doubling.

That comes to **1,859 allocations per `Drawing` zone**, or about 1,086 `forward_list` nodes and
644 deque blocks per frame at this zoom level.

**It is the same pathology as the YAPF node list, in a completely different subsystem.** Same
`std::deque` block-size rule, same shape of fix: these are function-local containers on a path
that runs every frame, and a reused instance would eliminate nearly all of it. The prize is
smaller, since the draw path is 10.4% of the run's allocations against the pathfinder's share of
the game loop, but it was invisible to every headless capture in this work.

`Blitter_32bppOptimized::EncodeInternal` and `UniquePtrSpriteAllocator::AllocatePtr` are 96 MB of
the roughly 97 MB allocated outside the loop, in 3,782 allocations averaging 20 KB and 86 KB.
That is the sprite cache populating rather than steady-state churn, and it is not a problem, but
it does mean **bytes and counts point at completely different code here** and a reader who
sorts by one will not find what the other shows.

#### On the 1.5% and the 10.4%

In the analysis window, allocations outside the game loop are 1.5% of the total. Across the whole
run they are 10.4%. Both are true and the window is the misleading one: it sits early, where road
pathfinding is at its heaviest and swamps everything else. Quote 10.4% for the run.

#### Cost

`GameLoop` averages 76.8 ms per tick against 57.8 ms in the M2 interactive capture, so about a
third more for the callstack tier. Well below the +113% measured headless, and I would not read
the difference as meaning anything: the two captures are half an hour apart, differ in how much
drawing happened, and this machine has already drifted 15% inside a session. The headless number
is the one with a controlled comparison behind it.

#### Gates

All four items reported green by Sloan. The trace confirms what it can: 23.1 million
callstack-carrying allocations with no termination, `Drawing`, `ViewportDrawing`, `Video` and
`Sound` all firing, the M0 plots and M1 pools present alongside the default pool, and stacks that
resolve through the GUI all the way to `VideoDriver::Tick`. The framerate comparison, the two
connect and disconnect cycles and the clean exit leave no trace evidence and are Sloan's report.

#### The deferred list gains an item

`ViewportSortParentSpritesSSE41`'s per-call containers now sit alongside the YAPF node arena.
Both are the same fix for the same reason, and both are changes rather than instrumentation.
Note that the viewport sorter has a non-SSE sibling, `ViewportSortParentSprites` at
`viewport.cpp:1600`, which declares the same three containers and needs the same treatment. And
that this is again substantially an MSVC problem: libstdc++ and libc++ use a 512-byte minimum
deque block and were already batching 64 pointers per allocation.
