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
