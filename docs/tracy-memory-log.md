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
