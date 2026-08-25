# Tracy integration plan

Plan for adding the [Tracy profiler](https://github.com/wolfpld/tracy) v0.14.0 client to this
branch, and for the instrumentation that goes with it.

Written against the `EnTT-Tracy` branch at 6c474f1df2. It assumes the benchmark harness in
[benchmark/README.md](../benchmark/README.md) and the phase gates in
[docs/ecs-migration-plan.md](ecs-migration-plan.md).

## What this buys over the existing harness

The harness already answers "how much time went to trains". It cannot answer any of these:

- Which call inside `Train::Tick` is expensive, and whether the cost is spread evenly or
  concentrated in a few hundred consists.
- How long the draw thread holds `game_state_mutex` while the game thread waits on it.
  `VideoDriver::Tick` takes that lock around input, `UpdateWindows` and sprite population, and
  nothing measures the stall it causes.
- Where a single slow tick came from. `PerformanceData` keeps a 512-sample rolling buffer and
  the whole-run totals added in phase 0 are sums, so an outlier tick is invisible in both.
- What the pathfinder does per call, since YAPF has no `PerformanceElement` at all.
- What the link graph threads are doing while `GameLoopLinkGraph` reports the main thread
  waiting for them.

Tracy gives per-call zones with a real distribution, lock contention, thread timelines, and
sampling-based call stacks for code nobody instrumented. The two tools answer different
questions and both should stay. The harness stays the phase gate, because it produces a
number you can put in a table and a fingerprint you can diff. Tracy is for finding out why a
number moved.

## Version constraint: 0.14.0 is not in vcpkg

The vcpkg tracy port is at 0.13.1, both at the baseline pinned in `vcpkg.json`
(`f3e10653`) and at local vcpkg HEAD (`c5a15727`). EnTT came in through vcpkg, so that is the
obvious path, and it does not work here.

Three ways to get 0.14.0:

| Option | Cost | Notes |
| --- | --- | --- |
| `FetchContent` pinned to `v0.14.0` | ~13 MB shallow clone at configure time | Recommended. One CMake block, and the pin is a single line if a future bump is ever wanted. |
| vcpkg overlay port | Copy `ports/tracy`, bump version and SHA512 | Keeps everything in one dependency system. More moving parts, and the overlay has to travel with the branch. |
| Vendor `public/` into `src/3rdparty/tracy` | ~2 MB in tree, manual updates | Offline builds, matches how fmt and squirrel are handled. Fall back to this if configure-time network access is a problem. |

Go with `FetchContent`. Tracy ships `public/TracyClient.cpp` as a single-translation-unit
amalgamation and its own `CMakeLists.txt` defines the `TracyClient` target plus a
`Tracy::TracyClient` alias, so there is nothing to hand-write.

### 0.14.0 specifics worth knowing

From the upstream `NEWS`:

- **API break in 0.14.0.** The `Secure` variants of the memory macros are gone.
  `TracySecureAlloc` becomes `TracyAlloc`. The secure path is now always on. This only
  matters if you copy older sample code.
- 0.14.0 adds **sections** (`TracySectionEnter` / `TracySectionLeave`) for marking coarse
  program phases. Useful here, see below.
- 0.14.0 adds `tracy-capture-daemon` and `tracy-merge`.
- **Client and server versions must match.** Grab `windows-0.14.0.zip` from the release, not
  whatever profiler build you already have.

**0.14.1 is explicitly not used.** It shipped on 2026-08-22 with two entries relevant here.
It silences MSVC warning C4366, which `TracyClient.cpp` triggers under the global `/W3`, and
it hardens string length encoding in the protocol. Neither changes the decision. C4366 is
noise rather than a build failure, because `/WX` is not set anywhere in
`cmake/CompileFlags.cmake`. The protocol change is the reason to hold still: a 0.14.1
profiler will most likely refuse a 0.14.0 client, and bumping later invalidates every capture
already taken.

Practical consequence: **use `windows-0.14.0.zip` from the 0.14.0 release and nothing else.**
Keep that profiler binary somewhere stable and do not let a later one replace it. A
mismatched profiler fails at connect time rather than producing a subtly wrong trace, so the
failure is loud, but it is still an afternoon lost if the version is not written down.

## Build integration

### CMake

Add to the root `CMakeLists.txt`, after the `openttd_lib` target exists:

```cmake
option(OPTION_TRACY "Build with the Tracy profiler client" OFF)

if(OPTION_TRACY)
    include(FetchContent)
    set(TRACY_STATIC ON CACHE BOOL "" FORCE)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
    set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(tracy)

    target_link_libraries(openttd_lib Tracy::TracyClient)
    target_link_libraries(openttd Tracy::TracyClient)
    target_link_libraries(openttd_test PRIVATE Tracy::TracyClient)
    target_compile_definitions(openttd_lib PUBLIC WITH_TRACY)
endif()
```

Four things that will bite:

**`openttd_lib` is an `OBJECT` library.** Linking `Tracy::TracyClient` to it propagates the
interface but does not guarantee the static library lands in every consumer. Link it to
`openttd` and `openttd_test` explicitly, as above, rather than relying on transitivity.

**`add_compile_options` in `compile_flags()` is directory-scoped**, so the FetchContent
subdirectory inherits `/W3`, `/utf-8` and `-D_FORTIFY_SOURCE=2`. That is fine, and it is
where C4366 comes from on 0.14.0.

**Do not route Tracy sources through `add_files`.** `_add_files_tgt` in
`cmake/SourceList.cmake` hard-fails on duplicate basenames and attaches sources to
`openttd_lib`, which carries `stdafx.h` as a precompiled header. Tracy compiled against
OpenTTD's PCH will not build. Using Tracy's own target through `add_subdirectory` sidesteps
both problems.

**`OPTION_TRACY` is a cache variable**, so like `OPTION_USE_ASSERTS` it applies to every
configuration in a tree. The harness already documents needing two trees for that reason.
Tracy wants a third.

### The third tree

`build-tracy` serves two roles at once: headless capture runs, and the interactive build that
has to stay playable after every phase.

```powershell
cmake -S . -B build-tracy -G "Visual Studio 17 2022" `
  -DCMAKE_TOOLCHAIN_FILE=C:/git/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DOPTION_USE_ASSERTS=OFF -DOPTION_TRACY=ON
```

Asserts off, for the reason the harness gives: an assert-enabled tree inflated road vehicles
36% against trains 17.5%, which moves the apparent ranking of subsystems. A profile that
points at the wrong hot spot is worse than no profile.

Never take a timing number from this tree either. The instrumentation is the thing you added,
so `build-release` remains the only source of numbers that go in a table.

Copy `baseset/opengfx-*.tar` across from an existing tree once it is configured. CMake only
installs the files that ship with the source, so a fresh tree has no graphics set. Headless
runs fail with a silent exit 1 and the interactive build will not start at all, which is a
confusing way to discover a missing tar file.

### Option choices

`TRACY_ON_DEMAND=ON` is mandatory, not a preference. Two independent reasons. Without it the
client records from process start and buffers everything, so a headless 20,000-tick run
allocates until it dies. And `build-tracy` has to stay playable after every phase, which
means the common case is running the game with no profiler attached and paying close to
nothing for it. On-demand is what makes that true.

`TRACY_NO_FRAME_IMAGE=ON` drops a thread Tracy would otherwise spawn for screenshot capture,
which is useless for a headless benchmark and irrelevant for the rest.

`TRACY_ONLY_LOCALHOST=ON` keeps the client from listening on the network. OpenTTD already
opens listening sockets and there is no reason to add another reachable one.

**`TRACY_NO_EXIT` stays off, and that is a deliberate trade.** With it on, the client blocks
at exit until the profiler has drained everything, which is the reliable way to capture the
tail of a fixed-tick headless run. It also hangs the game at exit whenever no profiler is
attached, which makes the build unusable for interactive testing. Since `build-tracy` is the
interactive tree, interactive wins. The headless capture flow works around it by starting the
capture before the run, or better by using `tracy-capture-daemon`. See
[Capture workflow](#capture-workflow).

If a specific headless capture genuinely needs the guarantee, configure a throwaway fourth
tree with `-DTRACY_NO_EXIT=ON` rather than flipping it in `build-tracy` and forgetting.

Leave sampling on. Tracy's periodic call stack sampling is what covers the code nobody
instruments, and 0.14.0's zone call stack reconstruction depends on it.

## The wrapper header

Add `src/profiling.h`. Everything else includes that, never `tracy/Tracy.hpp` directly.

Three reasons this is not optional:

**`safeguards.h` will destroy Tracy.** It `#define`s `memcpy`, `memset`, `malloc`, `printf`,
`to_string`, `min` and `max` into a compile error. Tracy's headers use several of those.
OpenTTD's convention of including `safeguards.h` last saves you, but only if the Tracy
include is genuinely earlier. A wrapper makes that one file's problem instead of every
file's.

**The build has to work with `WITH_TRACY` off.** The wrapper defines no-op macros in that
case, so no call site needs an `#ifdef`.

**The macro set should be ours, not Tracy's.** Zone volume is the central design problem here
(see below) and it needs tiers that Tracy does not provide.

Sketch:

```cpp
/** @file profiling.h Tracy profiler instrumentation macros. */

#ifndef PROFILING_H
#define PROFILING_H

#ifdef WITH_TRACY
#	include <tracy/Tracy.hpp>
#	define OTTD_ZONE               ZoneScoped
#	define OTTD_ZONE_N(name)       ZoneScopedN(name)
#	define OTTD_ZONE_C(name, col)  ZoneScopedNC(name, col)
#	define OTTD_FRAME_MARK         FrameMark
#	define OTTD_FRAME_MARK_N(name) FrameMarkNamed(name)
#	define OTTD_PLOT(name, value)  TracyPlot(name, value)
#	define OTTD_MESSAGE(text)      TracyMessageL(text)
#	define OTTD_THREAD_NAME(name)  tracy::SetThreadName(name)
#else
#	define OTTD_ZONE
#	define OTTD_ZONE_N(name)
/* ... and so on, all empty ... */
#endif /* WITH_TRACY */

/* Detail zones. Off even in a Tracy build unless explicitly asked for, because
 * these fire per vehicle and per tile. See "Zone volume" in
 * docs/tracy-integration-plan.md. */
#if defined(WITH_TRACY) && defined(OTTD_TRACY_DETAIL)
#	define OTTD_ZONE_DETAIL           OTTD_ZONE
#	define OTTD_ZONE_DETAIL_N(name)   OTTD_ZONE_N(name)
#else
#	define OTTD_ZONE_DETAIL
#	define OTTD_ZONE_DETAIL_N(name)
#endif

#endif /* PROFILING_H */
```

Include it from `stdafx.h`. `stdafx.h` is the precompiled header for `openttd_lib`, so the
macros become available everywhere without touching includes in a hundred files, and the
Tracy headers get compiled once. The cost is that flipping `OPTION_TRACY` rebuilds
everything, which it does anyway.

## Zone volume is the main design constraint

Get this wrong and the profiler is unusable. Wentbourne holds 85,259 vehicle parts. A zone
per part per tick, over a 5,000-tick run, is 426 million zones. At roughly 12 bytes per zone
in the trace that is about 5 GB, and the client will spend more time serialising than the
game spends simulating.

So the instrumentation has two tiers.

**Standard zones** fire once per tick or once per subsystem call. Budget: under about 500
zones per tick. Safe for a full 20,000-tick Hilbergen run or a 5,000-tick wentbourne run.
This tier is always on in a Tracy build.

**Detail zones** fire per vehicle, per tile or per pathfinder node. These sit behind
`OTTD_TRACY_DETAIL` and are compiled out otherwise. Use them for captures of a few hundred
ticks when you already know which subsystem you are chasing. Hilbergen at 2,818 parts and 300
ticks is about 850,000 zones, which is comfortable. Wentbourne at 300 ticks is 25 million,
which is the practical ceiling.

State the limit in the capture notes every time. A detail capture that silently dropped data
looks exactly like a fast subsystem.

## Instrumentation

### 1. Threads

`SetCurrentThreadName` is the single choke point. It is declared in `src/thread.h:33` and
implemented per platform in `src/os/windows/win32.cpp:537` and `src/os/unix/unix.cpp:245`.
`StartNewThread` calls it for every worker, and `win32_s.cpp` and `dedicated_v.cpp` call it
directly for their own threads.

Add `OTTD_THREAD_NAME` inside both platform implementations. That covers, with OpenTTD's own
names:

| Thread | Started at |
| --- | --- |
| `ottd:game` | `video_driver.cpp:91`, only when `is_game_threaded` |
| `ottd:linkgraph` | `linkgraph/linkgraphjob.cpp:63`, one per running job |
| `ottd:savegame` | `saveload/saveload.cpp:3127` |
| `ottd:http` | `network/core/http_curl.cpp:289` |
| `ottd:resolve` | `network/core/tcp_connect.cpp:279`, one per connection attempt |
| `ottd:dmusic` | `music/dmusic.cpp:1187` |
| `ottd:win-sound` | `sound/win32_s.cpp:47` |
| `ottd:win-console` | `video/dedicated_v.cpp:60` |

The main thread never calls `SetCurrentThreadName`, so add an explicit
`OTTD_THREAD_NAME("ottd:main")` at the top of `openttd_main`. Without it the most important
thread in the trace shows up unnamed.

### 2. Frame marks

This is the one place where OpenTTD's structure does not match Tracy's assumptions, so decide
it deliberately rather than dropping `FrameMark` in the first loop you find.

OpenTTD has two loops that could be "the frame", and whether they are on the same thread
depends on `is_game_threaded`:

- The simulation tick, `StateGameLoop` in `openttd.cpp:1207`, driven by
  `VideoDriver::GameLoop`. Runs on `ottd:game` when threaded, otherwise on the main thread.
- The draw tick in `VideoDriver::Tick` at `video_driver.cpp:118`, always on the main thread.

Use Tracy's unnamed primary frame for the draw tick, since that is what a player perceives as
a frame and what the frame statistics window is designed around. Use a named secondary frame
for the simulation:

- `OTTD_FRAME_MARK` at the end of the draw tick in `VideoDriver::Tick`, inside the
  `HasGUI()` branch, after `UnlockVideoBuffer`.
- `OTTD_FRAME_MARK_N("GameTick")` at the end of `VideoDriver::GameLoop`, after the
  `game_state_mutex` scope closes.

The null video driver is the exception and matters most, because it is what the benchmark
harness runs. `VideoDriver_Null::MainLoop` at `video/null_v.cpp:57` is a bare loop over
`GameLoop`, `InputLoop`, `UpdateWindows` with no draw and no threading. There, one iteration
is the frame: emit both marks at the bottom of the loop body so headless traces line up with
interactive ones.

### 3. Bridging the existing PerformanceElement measurements

The cheapest large win. `PerformanceMeasurer` and `PerformanceAccumulator`
(`framerate_type.h:90` and `:112`) already wrap every game loop subsystem, drawing, video,
sound and scripts. Emitting a Tracy zone from their constructors covers all of it without
touching a single call site.

The catch is that both take a runtime `PerformanceElement`, and `ZoneScoped` needs a
compile-time source location. Tracy supports this: build a static array of
`tracy::SourceLocationData`, one entry per element, and construct
`tracy::ScopedZone` with a pointer into it.

```cpp
/* In framerate_gui.cpp, indexed by PerformanceElement. */
static constexpr tracy::SourceLocationData _pf_srcloc[] = {
	{ "GameLoop",        "StateGameLoop", __FILE__, __LINE__, 0 },
	{ "GameLoopEconomy", "LoadUnloadStation", __FILE__, __LINE__, 0 },
	/* ... one per element, in PerformanceElement order ... */
};
static_assert(std::size(_pf_srcloc) == static_cast<size_t>(PerformanceElement::End));
```

Then hold an `std::optional<tracy::ScopedZone>` in the measurer and emit on construction.

The `static_assert` matters. `framerate_type.h` already warns that the enum's length and
order are load-bearing across `_pf_data`, `ConPrintFramerate` and two string sets in
`english.txt`. Adding a fourth parallel array without a compile-time check invites a silent
mismatch where every zone is labelled as its neighbour.

Because both systems are staying (see [Decisions](#decisions)), this array is a permanent
maintenance obligation rather than scaffolding. Update the "Adding new measurements" Doxygen
block at the top of `framerate_type.h` in the same commit, so the list of things to touch
when adding a `PerformanceElement` reads as five steps rather than four. That block is the
only place the parallel-array discipline is written down, and a plan document is not a
substitute for it.

Two elements need different handling:

**Sound.** Sound timings are taken on the audio thread and shuttled to the main thread
through `_sound_perf_measurements` and `_sound_perf_lock` at `framerate_gui.cpp:40`, then
replayed by `ProcessPendingPerformanceMeasurements`. Tracy cannot backdate a zone onto
another thread. Instrument the mixer directly on the audio thread instead and leave the
deferred path alone.

**Paused elements.** `PerformanceMeasurer::Paused` at `openttd.cpp:1215` records a gap, not a
duration. There is no zone to emit. Ignore it, and let the absence of zones show the pause.

`PerformanceAccumulator` fires many times per frame from scattered sites and each becomes a
separate zone. That is the point. The accumulator sums them and hides the distribution;
Tracy shows you that 400 of the calls were fast and three were not.

**Both wrappers stay on the same scope permanently.** In a Tracy build each measured scope now
carries two instruments: the accumulator that feeds `PerformanceData` and the report, and the
zone that feeds the profiler. That duplication is intentional and the overhead does not
matter, because `build-tracy` never produces a number anyone quotes. The division of
responsibility is worth stating plainly, since it is the thing that will drift:

| System | Owns | Runs in |
| --- | --- | --- |
| `PerformanceElement` | Phase-gate numbers, the report, the framerate window | Every tree, no tooling attached |
| Tracy zones | Diagnosis of why a number moved | `build-tracy`, profiler attached |

When they disagree, `PerformanceElement` is right about the number and Tracy is right about
the shape. Neither is a check on the other, and treating a Tracy total as a second opinion on
a harness total will waste time, because they measure the same scope under different build
settings.

### 4. Vehicle ticks

The hot path, and where the ECS migration lives. `CallVehicleTicks` at `vehicle.cpp:1223`.

Standard tier:

- `OTTD_ZONE_N("CallVehicleTicks")` at function entry.
- `OTTD_ZONE_N("SortVehicleRegistry")` around the sort at `vehicle.cpp:1229`. The harness
  already tracks `ecs.sort_total_pct_of_game_loop` and records that Hilbergen pays 10.4%
  against wentbourne's 0.14%, driven by dirty rate rather than scale. A zone shows the
  per-tick distribution behind that average, which is what tells you whether the cost is
  steady or spiky.
- A zone per typed pass inside the `OTTD_ECS_TICK_TYPED_PASSES` branch, so
  `TickVehiclesOfType<Train, ...>` and its four siblings each get one. The `#else` branch is
  a single interleaved loop and can only carry one zone for the whole thing, which is itself
  a reason to prefer the typed passes when profiling.
- `OTTD_ZONE_N("Autoreplace")` around the `_vehicles_to_autoreplace` loop at
  `vehicle.cpp:1277`.
- `OTTD_ZONE_N("RunEconomyVehicleDayProc")` and one around the `LoadUnloadStation` loop.

Detail tier, all `OTTD_ZONE_DETAIL`:

- Inside `TickVehicle`, and inside each of `Train::Tick`, `RoadVehicle::Tick`, `Ship::Tick`,
  `Aircraft::Tick`.
- Inside `AgeCargoAndPlaySound`.

Colour the four vehicle types consistently and keep the same colours in every capture. On a
wentbourne trace the type mix is the first thing you want to read off the timeline without
squinting at labels.

### 5. Lock contention

The highest-value thing Tracy adds, because the current harness is blind to it.

`VideoDriver` holds two mutexes, declared at `video_driver.hpp:366`:

- `game_state_mutex`, taken by `VideoDriver::GameLoop` around `::GameLoop()` and by
  `VideoDriver::Tick` around input handling, `UpdateWindows` and `PopulateSystemSprites`.
- `game_thread_wait_mutex`, whose entire purpose is to force a context switch, per the
  comment at `video_driver.cpp:54`.

Convert both to `TracyLockable(std::mutex, ...)` and the timeline shows exactly how long the
game thread spends blocked while the draw thread renders. `GameLoopPause` at
`video_driver.cpp:68` does a manual unlock and relock rather than using a guard, so check
that the lockable wrapper is used consistently there.

Also worth wrapping: `_sound_perf_lock` in `framerate_gui.cpp`, and the
`thread_startup_mutex` in `StartNewThread` at `thread.h:50`, which serialises every thread
creation in the process.

Lock instrumentation is not free. Keep it behind its own switch if a capture ever needs to be
clean of it.

### 6. Plots

`TracyPlot` puts a numeric series on the timeline next to the zones, which is how you connect
a spike in cost to a change in workload. Emit these once per tick from `StateGameLoop`, since
they are cheap and the harness already computes most of them:

| Plot | Source |
| --- | --- |
| `vehicles.parts` | Vehicle pool size |
| `vehicles.consists` | `IsPrimaryVehicle` count, cached rather than recounted per tick |
| `ecs.registry_dirty` | 1 when `SortVehicleRegistry` sorted, 0 when it skipped |
| `ecs.sort_us` | Time the sort took, in microseconds |
| `linkgraph.jobs_running` | Active `LinkGraphJob` count |
| `map.tileloop_tiles` | Tiles walked by `RunTileLoop` |

`ecs.registry_dirty` is the one to add first. The harness reports `ecs.sort_dirty_pct` as a
whole-run average, 88% on Hilbergen against 2.6% on wentbourne, and that average is the
figure the phase 6 decision rests on. Plotting it per tick shows whether the churn is uniform
or bursty, which the average cannot tell you and which changes what an order-preserving
variant would actually save.

### 7. Sections, new in 0.14.0

`TracySectionEnter` and `TracySectionLeave` mark coarse phases and the 0.14.0 profiler can
filter statistics, compare traces and limit frame ranges by them. Three obvious ones:

- `Startup`, from `openttd_main` to the first completed tick.
- `LoadSavegame`, around `SaveOrLoad` on the load path.
- `Benchmark`, around the fixed-tick loop in `VideoDriver_Null::MainLoop`.

The third pays for itself immediately. It lets you exclude savegame loading and first-tick
cold cache effects from every statistic, and the harness README already notes that the first
run after a rebuild is consistently slow.

### 8. Pathfinder, deliberately later

YAPF has no `PerformanceElement`, so it is invisible today, and per-consist pathfinding is
exactly the work the harness's per-consist denominator was built to expose. It is a strong
candidate and it is not phase 1, because zone placement inside `follow_track.hpp` needs care
to avoid landing in the detail-tier volume problem. Rely on Tracy's sampling for a first look
at where YAPF time goes, then instrument based on what the samples say.

### 9. Memory, optional

There is no global `operator new` override in the tree. Pool objects have their own
allocation path in `core/pool_type.hpp:308`, which is the natural place for `TracyAlloc` and
`TracyFree` if you want pool churn on the timeline.

Two warnings. Upstream documented in 0.14.1 that memory allocation reporting has to happen
under a lock, so this is not a drop-in pair of macros. And the harness already reports the
interesting static figure, that 71,360 of wentbourne's parts carry 88 bytes of `BaseConsist`
that means nothing to them. Allocation tracking answers a different question about churn over
time. Treat it as a later phase with its own justification.

## Determinism, and why it is the acceptance criterion

Everything in `docs/desync.md` applies. Instrumentation must not touch game state, and the
things to watch are:

- No instrumentation calls `Random()`. Use `InteractiveRandom()` or nothing.
- Plot values read game state and never write it.
- Nothing branches on whether a profiler is connected. On-demand mode means connection state
  varies between runs by definition, so any behaviour that depends on it is a desync waiting
  to happen.
- Tracy spawns its own threads and allocates. That is fine, because it never touches the
  simulation, but it does perturb timing. OpenTTD's simulation is not timing-dependent within
  a tick, so this changes how many ticks run in real time, not what a tick does.

You do not have to argue any of this, because the branch can already prove it. The state
fingerprint in `src/state_fingerprint.cpp` and the `-CompareTo` flag exist for exactly this.

**Acceptance criterion for every instrumentation phase: a `build-tracy` run reproduces the
`build-release` baseline fingerprint, on both fixtures, with a profiler attached and again
with none attached.**

```powershell
.\benchmark\run-benchmark.ps1 -Save Hilbergen -Ticks 20000 -Label tracy-p1 `
    -BuildDir build-tracy `
    -CompareTo .\benchmark\out\phase0-Hilbergen-RelWithDebInfo-build-release.tsv
```

Run it with a profiler connected as well as without. The connected case is the one that could
plausibly diverge, and it is the one nobody remembers to test.

### The interactive gate

Since `build-tracy` has to stay playable, the fingerprint check is necessary and not
sufficient. Every phase also ends with an interactive run, and it covers ground the headless
one structurally cannot.

The null driver never sets `is_game_threaded`. `VideoDriver_Null::MainLoop` is a bare loop
with no game thread and no draw tick, so a headless run never takes `game_state_mutex`, never
exercises `VideoDriver::Tick`, and never touches drawing, viewport sorting, input or sound.
That is most of what phases T2 and T3 instrument. A green fingerprint says nothing about any
of it.

Per phase, load a fixture in the GUI and confirm:

1. **It starts and plays with no profiler attached**, and the framerate window shows numbers
   in the same range as `build-release`. This is the on-demand claim being tested. If an
   unattached instrumented build costs anything visible, something is recording when it
   should not be.
2. **A profiler connects mid-session and disconnects cleanly**, twice, without the game
   stalling or dying. Connect and disconnect are the on-demand code paths and they only run
   here.
3. **The game exits normally with no profiler attached.** This is the `TRACY_NO_EXIT` check.
   It should pass by construction, and it is one keystroke, and getting it wrong hangs every
   subsequent headless run in a way that looks like a benchmark problem.
4. **Whatever the phase added is actually visible in the trace.** A zone that compiled but
   never fires is the normal failure mode for instrumentation work, and it is invisible in
   every other check listed here.

Run it threaded and unthreaded from T3 onward, since the lock instrumentation only means
anything when `is_game_threaded` is true.

Note that the harness will warn about timings from an instrumented tree, and it should. Add
`OPTION_TRACY` to the conditions that trigger that warning, and to the report filename, for
the same reason the tree name is already in there: silently overwriting a baseline with
instrumented numbers is worse than a verbose filename.

## Capture workflow

For interactive sessions, on-demand mode means you launch the game normally, connect
`tracy-profiler.exe` when you reach the situation you care about, and disconnect when done.

For headless benchmark runs, `TRACY_NO_EXIT` is off so the client will not wait for you. The
sequence is ordered for that reason:

1. Start `tracy-capture -o out.tracy` first, so it is already listening.
2. Run the harness against `-BuildDir build-tracy`.
3. Check the zone count in the capture against what the tick count predicts.

Step 3 is not optional. Without `TRACY_NO_EXIT`, a run that finishes before the capture
drains loses its tail silently, and a truncated trace reads as a run that got faster near the
end. Predict the count before you look: standard-tier zones are roughly a fixed number per
tick, so 20,000 Hilbergen ticks should land within a few percent of that number times 20,000.
A capture 30% short is a lost tail, not a discovery.

0.14.0's `tracy-capture-daemon` is the better answer and removes the ordering requirement
entirely. It discovers clients over UDP and captures each to its own file, so the run can
start whenever it likes. Move to it once the basic flow works rather than debugging both at
once. It needs broadcast, which `TRACY_ONLY_LOCALHOST` still permits on loopback, so leave
`TRACY_NO_BROADCAST` off.

If a particular capture matters enough that a lost tail would be expensive, configure the
throwaway `-DTRACY_NO_EXIT=ON` tree for it. That is the whole reason the option is documented
rather than forgotten.

Add a `-Tracy` switch to `run-benchmark.ps1` that selects the tree, tags the report filename,
and refuses to write to a path that an uninstrumented baseline already occupies.

## Phasing

Each phase ends with both gates: a fingerprint check on both fixtures, and the interactive
run described above. `build-tracy` is playable at the end of every phase, without exception.
That constraint is why T0 exists as its own phase and why T1 comes before any zone work.

**Phase T0, build only.** `OPTION_TRACY`, the FetchContent block, `src/profiling.h` with
every macro defined, and the `stdafx.h` include. No instrumentation at all. Confirm all three
trees still build with the option off, that `build-tracy` builds with it on, and that the
interactive build launches, loads a save, plays and exits cleanly with no profiler attached.

That last check is the point of the phase. It establishes the playable baseline before any
zone exists, so when phase T1 breaks something interactive you know it was T1. It is also the
phase most likely to surface the `OBJECT` library linking problem, and a link error is much
easier to read when nothing else changed.

**Phase T1, skeleton.** Thread names, frame marks in both video drivers, the three sections.
At this point a capture shows real thread timelines and frame pacing with no zones inside
them. That is already enough to see the draw and game thread interleaving, which is the first
thing the headless harness could never show.

T1 is also the first phase where connect and disconnect run against real instrumentation, so
give the second interactive check a proper try rather than a single connect. Attach, play for
a minute, detach, play on, attach again.

**Phase T2, the PerformanceElement bridge.** The `SourceLocationData` array and the two RAII
classes. One change, and every existing measurement appears as a zone. Handle sound
separately. Biggest coverage gain per line changed, so it goes early.

**Phase T3, locks and plots.** `game_state_mutex`, `game_thread_wait_mutex`, and the six
plots. This is where Tracy starts answering questions the harness cannot, so if the
integration has to stop somewhere, stop after this.

T3 can only be validated interactively, and only with `is_game_threaded` true. The null
driver has no game thread, so a headless capture of this phase shows an empty lock timeline
that looks identical to zero contention. Run threaded and unthreaded and confirm the two
traces differ.

**Phase T4, vehicle detail.** Standard zones in `CallVehicleTicks` and the typed passes, then
the `OTTD_TRACY_DETAIL` tier. Validate the volume estimates against a real capture before
trusting them.

**`OTTD_TRACY_DETAIL` must stay off in `build-tracy`.** Per-vehicle zones at wentbourne scale
will not survive interactive play, and the interactive build has to keep working. Enable it
per capture through a compile definition on a scratch tree, and treat any trace taken with it
as short-run only.

**Phase T5, pathfinder.** Driven by what T1 to T4 sampling actually showed, not by this
document.

## Risks

**Instrumented timings are not comparable to clean ones.** The harness README makes this
argument already for asserts. The same discipline applies, and the same fix: a separate tree
and the tree name in the filename.

**Zone volume in the vehicle loop.** Covered above. The failure mode is a capture that
quietly drops data and reads as a fast subsystem.

**MSVC C4366 on 0.14.0.** A warning under the global `/W3`, not an error, and fixed in
0.14.1, which D1 rules out. Accept the warning. If the noise becomes a problem, suppress
C4366 on the `TracyClient` target alone rather than reaching for a version bump.

**Version skew between client and profiler.** Pin the profiler binary next to the tree or
write the version into the capture notes. A mismatched profiler fails at connect time, which
is at least loud.

**`FetchContent` at configure time needs network access.** If that becomes a problem, vendor
`public/` into `src/3rdparty/tracy` and write a five-line `CMakeLists.txt` for it. Skip
upstream's root `CMakeLists.txt` in that case, since it drags in `cmake/version.cmake`,
Fortran bindings and rocprofiler detection you do not want.

**Upstream will not take this.** `CONTRIBUTING.md` aside, OpenTTD deliberately keeps its own
lightweight framerate instrumentation rather than depending on an external profiler. This is
branch-local work, same as the EnTT migration. Worth stating once so nobody plans a PR
around it.

## Decisions

Three questions were open when this plan was drafted. All three are now settled, and the body
above reflects them. They are recorded here because the reasoning explains several choices
that would otherwise look arbitrary.

**D1. Pin v0.14.0. Do not take 0.14.1.** The 0.14.1 protocol change to string length encoding
means a bump invalidates every capture taken before it, and the only thing it buys this repo
is silencing MSVC C4366, which is a warning rather than an error because `/WX` is not set.
Not worth the churn. The consequence to remember is that the profiler binary is pinned too:
`windows-0.14.0.zip` and nothing later.

**D2. The interactive build is instrumented, and stays playable after every phase.** This is
the constraint with the widest reach in the plan, and it drives four things that would look
like overcaution on their own. `TRACY_ON_DEMAND` becomes mandatory rather than advisable.
`TRACY_NO_EXIT` stays off, which is why the headless capture flow has an ordering requirement
and a zone-count check instead of a guarantee. `OTTD_TRACY_DETAIL` stays off in `build-tracy`,
because per-vehicle zones and interactive play are incompatible. And every phase gets a second
gate, since the null driver never exercises the game thread, the locks, drawing or input,
which between them are most of what T2 and T3 instrument.

**D3. `PerformanceElement` and Tracy both stay, permanently.** The harness needs numbers in a
file with no profiler running, and the ECS phase gates depend on those numbers, so the
existing accumulators are not scaffolding to be removed once Tracy works. The cost is two
instruments wrapping the same scopes and a fourth parallel array keyed to the enum. Pay it
deliberately: add the array to the "Adding new measurements" block in `framerate_type.h` so
the obligation lives next to the code rather than in this file.

The division that follows from D3 is worth repeating, because it is what will drift. Numbers
come from `PerformanceElement` in `build-release`. Explanations come from Tracy in
`build-tracy`. A disagreement between them is a difference in build settings, not a finding.
