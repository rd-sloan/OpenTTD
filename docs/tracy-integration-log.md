# Tracy integration work log

Running record of work done, decisions taken and deviations from
[docs/tracy-integration-plan.md](tracy-integration-plan.md) during the Tracy profiler
integration on the `EnTT-Tracy` branch.

**This file takes precedence over the plan wherever the two disagree.** The plan is frozen as
the as-designed record and is not edited once work starts, so it will drift as reality
intervenes. Anything that changes goes here, with the reason. Read this file before resuming
work, not the plan alone.

## Ground rules

Agreed with Sloan on 2026-08-25, before any code was written. These govern the whole task.

1. **Keep this log.** Work done and plan updates are recorded here, not in the plan file.

2. **Do not edit `docs/tracy-integration-plan.md`.** It is the as-designed record. Corrections,
   reversals and new decisions belong in this log.

3. **Commit incrementally, and say why.** Small commits wherever the change divides sensibly.
   The message explains the reason for the change, not only its content.

4. **Run the unit tests before submitting anything.** `openttd_test` in the `build` tree at
   minimum. See [What the gates actually prove](#what-the-gates-actually-prove) for why this
   is necessary and not sufficient.

5. **Never push to remote.** Commits are local. Sloan is the only person who pushes, on any
   branch. This also rules out `gh pr create`, which pushes as a side effect.

### Commit message convention

CI validates the format strictly, and the client-side hooks from OpenTTD-git-hooks are **not
installed on this clone** (`.git/hooks` holds only `.sample` files), so nothing catches a bad
message locally. Self-enforce:

```
<keyword>( #<issue>|<commit>)?: ([<component>])? <details>
```

Exactly one keyword, capital start on the details, no trailing dot. Rule 3's "why" does not
fit there, so it goes in the body:

```
Codechange: [CMake] Add OPTION_TRACY to build the Tracy 0.14.0 client

vcpkg's tracy port is 0.13.1 at our pinned baseline, so 0.14.0 has to come
from FetchContent. Linked explicitly to openttd, openttd_lib and openttd_test
rather than transitively, because openttd_lib is an OBJECT library and does
not reliably propagate the static library to consumers.
```

Use `Codechange` for instrumentation and plumbing, since a player notices none of it. Add
`[CMake]` for build files and `Doc` for documentation-only changes.

### What the gates actually prove

Three checks run per phase and they cover different ground. Report them separately. A green
result from one is not evidence about the others.

| Gate | Proves | Blind to |
| --- | --- | --- |
| `openttd_test`, 98 cases | The build works and existing behaviour is intact | Everything Tracy-specific. No case executes a zone, a frame mark or a lock wrapper. |
| State fingerprint vs. `build-release` baseline | The simulation did not diverge | Anything the null driver never runs, which is the game thread, both mutexes, drawing, viewport sorting, input and sound. |
| Interactive run in `build-tracy` | On-demand cost, connect and disconnect, clean exit, and that new zones actually fire | Determinism. |

The unit suite is Catch2 over pure helpers in `src/core` and `src/misc`. `mock_environment.h`
exists because most of the codebase needs global game state, which is why coverage of this
work is zero and will stay zero. Rule 4 is a regression gate on the rest of the codebase, and
it is worth keeping for that. It will never catch instrumentation that is wrong.

Also run `openttd_test` in `build-tracy` at T0 and after any CMake change. It is one of the
three targets that must link `TracyClient`, and it is where the `OBJECT` library linking
hazard would surface.

## Log

### 2026-08-25, setup

Ground rules agreed and recorded above. Plan committed at 978658aec4, 692 lines, frozen from
this point.

Environment checks before starting:

- Working tree clean on `EnTT-Tracy`.
- Build trees present: `build` (asserts on), `build-release` (asserts off). `build-tracy` does
  not exist yet and is created in T0.
- `openttd_test` in `build` reports 98 test cases. Baseline not yet run.
- Git hooks not installed, see above.

No code changes yet. T0 not started.

### 2026-08-25, phase T0: build wiring only

`OPTION_TRACY`, the FetchContent block, `src/profiling.h`, and the `stdafx.h` include. No
instrumentation. Tracy is linked and initialised but nothing calls it, which is the intended
end state for this phase.

Files touched: `cmake/Options.cmake`, `CMakeLists.txt`, `src/CMakeLists.txt`, `src/stdafx.h`,
`src/profiling.h` (new).

#### Deviations from the plan

**The option and the define follow the repo's existing pattern, not the plan's sketch.** The
plan put `option(OPTION_TRACY ...)` and `target_compile_definitions(openttd_lib PUBLIC
WITH_TRACY)` inline in the root `CMakeLists.txt`. The repo already has a home for both, so
`OPTION_TRACY` is declared in `set_options()`, reported in `show_options()`, and turned into
`add_definitions(-DWITH_TRACY)` in `add_definitions_based_on_options()`, exactly mirroring
`OPTION_USE_ASSERTS`.

That is worth understanding rather than just noting, because the ordering looks wrong and is
not. `add_definitions_based_on_options()` runs at `CMakeLists.txt:444`, well after
`add_subdirectory(src)` at line 274. It still reaches `openttd_lib` because that target is
created in the root directory and directory-level `COMPILE_DEFINITIONS` are evaluated at
generate time, not at target-creation time. `WITH_ASSERT` has always worked this way.

Two consequences fall out of it, both wanted. Targets created in subdirectories, `strgen` and
`settingsgen`, do **not** receive `WITH_TRACY`, which is correct since they never link
`TracyClient` and would fail to find the headers. And `openttd_test` **does** receive it,
because it is created in the root directory, which is precisely why the explicit link to
`Tracy::TracyClient` is required rather than optional.

**`TracySectionEnter` does not pair by name.** The plan's phase T1 sketch implies a matched
enter and leave keyed on a string. The real 0.14.0 signature returns an id from
`TracySectionEnter(fmt, ...)` which must be passed to `TracySectionLeave(id)`. The disabled
form returns `0`. `src/profiling.h` mirrors that, and `OTTD_SECTION_LEAVE` casts its argument
to void when disabled so the caller's id variable does not become unused. Adjust the T1 work
accordingly; the plan is wrong on this point.

**`src/profiling.h` covers more macros than T0 needs.** Zone, frame, plot, message, thread
name, section and lock macros are all defined now, so T1 and T3 do not have to reopen the
header. Adding a macro is not the risky part of those phases.

**Added a configure-time warning.** `show_options()` emits a CMake warning when
`OPTION_TRACY` is on, mirroring the harness's warning about timings from an assert-enabled
tree. Same reasoning: an instrumented tree must never be the source of a number in a table.

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug, Tracy off | 98 cases, 2176 assertions, pass. Identical to the pre-change baseline. |
| `openttd_test`, `build-release`, RelWithDebInfo, Tracy off | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy`, RelWithDebInfo, Tracy on | 98 cases, 2176 assertions, pass. |
| State fingerprint | Not run. See blocker below. Moot for T0, which changes no game logic. |
| Headless run, `build-tracy` | `-vnull:ticks=300 -snull -mnull -g` exits 0. Covers startup, map generation, game loop and clean exit with Tracy linked and no profiler attached. |
| GUI launch, `build-tracy` | Launches, survives 10 s, exits 0 on window close. |

Verified directly rather than assumed: `build-tracy/CMakeCache.txt` carries `TRACY_ENABLE`,
`TRACY_ON_DEMAND`, `TRACY_STATIC`, `TRACY_NO_FRAME_IMAGE` and `TRACY_ONLY_LOCALHOST` all ON
with `TRACY_NO_EXIT` OFF; `_deps/tracy-src` reports `v0.14.0`; `openttd_lib.vcxproj` carries
`-DWITH_TRACY`; and `TracyClient.lib` is produced and linked into both `openttd.exe` and
`openttd_test.exe`.

MSVC warning C4366 did not appear. It is presumably level 4 and the tree builds at `/W3`, so
the concern recorded in the plan does not arise in practice. No action needed.

#### Blocker for later phases

**`benchmark/saves/` is empty.** `Hilbergen.sav` and `wentbourne.sav` are untracked and not
present on this machine, so the state fingerprint gate cannot run at all. It does not matter
for T0, which changes no game logic, but it is a hard blocker for T1 onward, where the
fingerprint is the primary determinism gate. The recorded `.tsv` baselines under
`benchmark/example benchmarks/` are present; only the savegames are missing.

**The Tracy 0.14.0 profiler binary is not downloaded.** `windows-0.14.0.zip` from the v0.14.0
release is needed for the connect and disconnect checks from T1 onward. Not needed for T0.

#### Still to be checked by hand

The GUI launch above proves the binary starts and exits. It does not cover the rest of the
interactive gate, which needs a person: that the framerate window in `build-tracy` reads in
the same range as `build-release` with no profiler attached, and that a profiler connects and
disconnects twice without stalling the game. Both are deferred to T1, when there is something
in a trace worth looking at.

### 2026-08-25, T0 blockers cleared

Both blockers recorded above are resolved. T1 is unblocked.

**Fixtures restored.** Sloan put the savegames back. `benchmark/saves/` now holds
`Hilbergen.sav` (0.6 MB) and `wentbourne.sav` (5.2 MB), so the state fingerprint gate can run
from T1 onward. They remain untracked and local, as the harness README intends.

**Profiler installed at `C:\git\tracy-0.14.0`.** Downloaded from the v0.14.0 release,
`windows-0.14.0.zip`, 15,951,463 bytes, SHA256
`2b4b013b52a6473ff699c6e018566ad70e3098a14967c384d014c2c24dcf8167`, verified against the
digest GitHub publishes for the asset. `tracy-capture.exe` self-reports `0.14.0 / 099df3d`.

Eight tools ship in it. The ones this work uses:

| Tool | Use |
| --- | --- |
| `tracy-profiler.exe` | The interactive GUI. Connect to a running client, or open a `.tracy` file. |
| `tracy-capture.exe` | Headless capture to a file. Start it before a benchmark run, since `TRACY_NO_EXIT` is off. |
| `tracy-capture-daemon.exe` | Discovers clients over UDP and captures each to its own file. Removes the start-first ordering requirement. |
| `tracy-csvexport.exe` | Exports zone statistics from a `.tracy` file, for putting numbers next to harness output. |
| `tracy-merge.exe` | Combines multi-process traces. Not expected to be needed here. |

Deliberately outside the repository, so it cannot dirty the working copy or be committed by
accident, and alongside `C:\git\vcpkg` where the other external toolchain already lives.

**Do not replace this with a later build.** Client and server must match, and 0.14.1 changed
the wire protocol. The client is pinned to v0.14.0 by `FetchContent` in `CMakeLists.txt`, so a
0.14.1 profiler would fail to connect. If the profiler is ever upgraded, the pin has to move
in the same change and every capture taken before it becomes incomparable.

### 2026-08-25, phase T1: thread names, frame marks, sections

Skeleton instrumentation. No zones yet, which is the intended end state: a capture now shows
thread timelines and frame pacing with nothing inside them.

Files touched: `src/os/windows/win32.cpp`, `src/os/unix/unix.cpp`, `src/openttd.cpp`,
`src/video/video_driver.cpp`, `src/video/null_v.cpp`, `src/saveload/saveload.cpp`,
`src/profiling.h`, `CMakeLists.txt`.

#### T0 hid a link failure, and T1 found it

**The most important thing in this phase.** T0 reported that `TracyClient.lib` linked into
both executables. That was a false positive. Nothing referenced a Tracy symbol, so the linker
never pulled `TracyClient.obj` in, and the mismatch underneath went unnoticed. The first
actual call site produced:

```
TracyClient.lib(TracyClient.obj) : error LNK2038: mismatch detected for 'RuntimeLibrary':
value 'MD_DynamicRelease' doesn't match value 'MT_StaticRelease' in win32_main.obj
TracyClient.lib(TracyClient.obj) : error LNK2001: unresolved external symbol __imp__fstat64i32
TracyClient.lib(TracyClient.obj) : error LNK2001: unresolved external symbol __imp__stat64i32
```

The MSVC block at the top of `CMakeLists.txt` switches to the static CRT when the vcpkg
triplet ends in `-static`, but it names the three openttd targets explicitly and runs long
before `TracyClient` exists, so Tracy was built against `/MD`. Fixed by setting
`MSVC_RUNTIME_LIBRARY` on `TracyClient` immediately after `FetchContent_MakeAvailable`. The
two unresolved `stat64i32` symbols were a symptom of the same mismatch and went away with it.

The lesson generalises beyond this bug: **"it linked" proves nothing about a library nothing
calls.** Any future phase that first exercises a new dependency should expect link errors
that earlier phases appeared to rule out.

#### Deviations from the plan

**Added `ScopedProfilerSection` to `src/profiling.h`.** Tracy's section API is id-based, so a
raw enter/leave pair leaks the section on any early exit. `DoLoad` in particular throws
through `SlError`, which would leak on every failed load. The RAII wrapper takes the name as a
`%s` argument rather than as the format string, since `TracySectionEnter` is printf-style and
a name containing a percent sign would otherwise be read as a conversion specifier.

**The Startup section still uses the raw macros.** It has to end where the main loop begins,
not where `openttd_main` returns, so RAII does not fit without restructuring the function.
`openttd_main` has five early returns before that point and each leaves the section open.
That is deliberate and harmless: every one of those paths exits the process immediately, and
`SectionLeave` on an id of 0 is a no-op anyway.

**Sectioned `DoLoad` rather than `SaveOrLoad`.** The plan named `SaveOrLoad`, but
`LoadWithFilter` bypasses it and both routes converge on `DoLoad`, so that is the single point
covering every load. Preview reads that populate the load dialog are marked `LoadCheck` and
real loads `LoadSavegame`, rather than suppressing previews, since a stall while browsing
files is worth being able to see.

**On-demand mode makes the Startup section mostly theoretical.** `SectionEnter` returns 0
when no profiler is connected, and interactively you launch the game and connect afterwards,
so startup is over before recording begins. It does record in the headless capture flow, where
`tracy-capture` is already listening. Kept for that case; do not expect to see it in an
interactive trace.

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy`, RelWithDebInfo | 98 cases, 2176 assertions, pass. |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS.** `build-tracy` reproduces the `build-release` baseline exactly, combined hash `015ED3D109C5CCCC`. |
| Frame marks reach a trace | **Verified.** See below. |

Frame marks were checked rather than assumed, at three run lengths:

| Ticks | Frames reported by `tracy-capture` | Zones |
| --- | --- | --- |
| 1,000 | 1,003 | 0 |
| 3,000 | 3,003 | 0 |
| 5,000 | 5,003 | 0 |

Exactly one primary frame per simulated tick. The constant offset of three does not scale
with run length and is not explained; it is small, fixed, and not worth chasing. Zones at zero
is a positive result here, not an absence of evidence: T1 adds no zones and none leaked in.

A sample capture is kept at `benchmark/out/t1-Hilbergen-5000ticks.tracy` for opening in the
profiler. `benchmark/out` is untracked, so it will not be committed.

**On-demand costs nothing measurable when unattached.** `build-tracy` ran the 20,000 tick
Hilbergen fixture at 1,782 ticks/s against `build-release` at 1,770. That difference is well
inside the roughly 7% noise band the harness README documents for this fixture, and the sign
is the wrong way round for a cost, so read it as no measurable difference rather than as a
speedup.

#### Not verified, and needs the GUI

`tracy-capture` reports frames and zones. It does not report thread names, named frame sets or
sections, and `tracy-csvexport` only handles zones, messages and plots. So three T1 additions
are **compiled and believed working but unconfirmed**: the `GameTick` named frame set, the
three sections, and thread naming.

Thread naming in particular cannot be confirmed headlessly at all, because the null driver
never starts a game thread and the fixture starts no link graph jobs, so there is only the
main thread to name.

These need a person with `tracy-profiler.exe`. Open the sample capture above for the frame
sets and sections, and attach to an interactive session for thread names. Deferred rather than
dropped, and carried into T3, which is the first phase that genuinely cannot be validated any
other way.

### 2026-08-26, T1 interactive results, and two things about capture on Windows

Sloan attached the profiler to an interactive session, loaded Hilbergen and let it run about a
minute. Results below, plus the two findings they turned up.

**Confirmed.** The `GameTick` named frame set, the default frame set, and the `LoadSavegame`
and `LoadCheck` sections all appear. That closes three of the four T1 items that could not be
checked headlessly.

**Not seen.** No thread names, and no sampled call stacks. Neither is a fault in the
instrumentation, and they have separate causes.

#### Windows sampling needs Administrator, silently

`SysTraceStart` in Tracy's `client/TracySysTrace.cpp:189` calls `etw::CheckAdminPrivilege()`
and returns false immediately if it fails. Everything after that point in the function is
skipped, so an unelevated process gets **no sampled call stacks, no context switch capture and
no vsync capture**, with no error and nothing in the trace to say why.

This is a Tracy platform requirement on Windows, not a consequence of anything in this
integration, and it is not affected by `TRACY_ON_DEMAND` or by any option set in
`CMakeLists.txt`. `TRACY_NO_SAMPLING` is off, so sampling is compiled in; it is refused at
runtime.

**Run the game elevated for any capture where sampling matters.** That includes the T5
pathfinder work, where the plan explicitly leans on sampling to find where YAPF time goes
before instrumenting it, and any capture meant to cover code nobody has instrumented.
Elevation is not needed for zones, frames, plots, sections or messages, so ordinary
zone-hunting captures do not require it.

#### Thread names only appear once a thread has events

Worth writing down because the mechanism is not obvious and it will come up again.
`tracy::SetThreadName` sends nothing when called. It pushes a `ThreadNameData` record onto a
process-global atomic linked list keyed by thread id, in the `TRACY_ENABLE` block at the end
of `common/TracySystem.cpp`. The profiler resolves a name by id only when that thread already
appears in the event stream.

So a thread that never emits a thread-attributed event never appears in the trace, and its
name is therefore never transmitted. Frame marks do not qualify: they belong to a frame set
rather than to a thread, which is exactly why T1 produces frames and no thread rows.

The two findings compound. Context switch records are thread-attributed, so with elevation the
sampling data alone would have produced named thread rows even with zero zones. Unelevated and
zone-free, there is genuinely nothing to hang a thread row on.

**Thread naming therefore remains unverified**, and there are two ways to close it. Reconnect
to an elevated session, where `ottd:main` and `ottd:game` should appear as named rows carrying
samples and no zones. Or wait for T2, where the `PerformanceElement` bridge puts zones on both
threads and the rows appear regardless of elevation. If the names show as raw thread ids under
either route, `OTTD_THREAD_NAME` is broken and needs investigating.

#### Correction

Guidance given in conversation said to expect sampled call stacks in an interactive trace as a
sign the connection was live. That was wrong, because it omitted the elevation requirement.
An unelevated connection is fully live and simply has no samples in it. Use the frame counter
or a section to confirm a connection instead.

### 2026-08-26, T1 fully verified

Sloan reran the interactive capture elevated. Sampled call stacks appear, and so do named
thread rows: `ottd:game`, several concurrent `ottd:linkgraph` threads, and others.

**T1 is now verified in full.** Every item is confirmed: both frame sets, the `LoadSavegame`
and `LoadCheck` sections, and thread naming. Nothing in the phase remains on trust.

This also confirms the diagnosis in the previous entry rather than merely being consistent
with it. The same build produced no thread rows unelevated and named rows elevated, with no
code change in between, which is what the two mechanisms predicted: elevation enables the
thread-attributed sampling data, and the names then resolve against it.

One incidental finding worth carrying into T3. Hilbergen runs **several `ottd:linkgraph`
threads concurrently**, which was not obvious from the code and is more than one job at a
time. The plan's `linkgraph.jobs_running` plot is therefore measuring something real on this
fixture, and `GameLoopLinkGraph`, which times the main thread waiting for those jobs, is worth
reading against the thread rows rather than on its own.

Verified via `SetCurrentThreadName`, which is the path every worker takes.

#### The main thread shows as "Main thread", and always will

Checked because `ottd:main` was absent from the rows while `Main thread` was present. This is
not a failure of `OTTD_THREAD_NAME`; Tracy declines to use the name.

In `Profiler::HandleServerQuery` (`client/TracyProfiler.cpp:3918`), when the server asks for a
thread's name:

```cpp
if( ptr == m_mainThread ) {
    SendString( ptr, "Main thread", 11, QueueType::ThreadName );
} else {
    auto t = GetThreadNameData( (uint32_t)ptr );
    ...
}
```

`GetThreadNameData` is consulted only in the `else` branch. For the main thread Tracy replies
with the hardcoded `"Main thread"` and never reads the stored name. So the call in
`openttd_main` writes a record that nothing in the trace path can reach.

The call is not useless. `tracy::SetThreadName` also calls the Windows `SetThreadDescription`,
so `ottd:main` does show up in a debugger and in crash dumps, and for the main thread our call
is the only thing that sets it, since the main thread never goes through
`SetCurrentThreadName`. It is worth keeping on those grounds alone.

**The comment above it is wrong and should be corrected.** It claims that without the call the
busiest thread in a trace would be unlabelled. Tracy labels it either way. Whoever touches
this next should not have to rediscover that from the Tracy source.

Generalises to one rule for the rest of this work: **a name being stored is not a name being
displayed.** Tracy has hardcoded and special-cased paths, and the only way to know an
instrumentation call had the intended effect is to look at a trace.

### 2026-08-26, phase T2: the PerformanceElement bridge

Every existing `PerformanceElement` measurement now also emits a Tracy zone. A
`SourceLocationData` table indexed by the enum supplies the static source location that Tracy
needs, since the measurement classes take their element as a runtime value and so cannot use
`ZoneScopedN`.

Files touched: `src/framerate_type.h`, `src/framerate_gui.cpp`.

#### The plan was wrong about this phase being cheap

The plan calls T2 the "biggest coverage gain per line changed" and puts it in the standard
tier. **It is not a standard-tier change**, because four of the elements are measured inside
`Vehicle::Tick` implementations rather than once per tick:

| Element | Site | Fires |
| --- | --- | --- |
| `GameLoopTrains` | `train_cmd.cpp:4196`, inside `if (IsFrontEngine())` | per consist |
| `GameLoopRoadVehicles` | `roadveh_cmd.cpp:1667`, function scope | per part |
| `GameLoopShips` | `ship_cmd.cpp:794`, function scope | per part, and ships are single-part |
| `GameLoopAircraft` | `aircraft_cmd.cpp:2177`, after the `IsNormalAircraft` guard | per normal aircraft |

On wentbourne that sums to 4,833 + 5,499 + 2,818 + 749 = **13,899 zones per tick**, which is
exactly the consist count in the harness README. Over a 5,000 tick run that is about 70
million zones. At the 7.3 bytes per zone measured below, roughly 500 MB of trace.

So the four vehicle elements are gated behind `OTTD_TRACY_DETAIL` through a new
`IsPerformanceZoneActive`, passed to `tracy::ScopedZone`'s `is_active` parameter. It is
`constexpr` and lives in the header because the element is a literal at every construction
site, so the test folds away instead of branching inside the vehicle tick path.

Without the gate, T2 would have broken decision D2: a build emitting 13,899 zones per tick is
not one you can play.

#### Sound needed no special handling after all

The plan said the Sound element needed separate treatment because its timings are taken on the
mixer thread and shuttled to the main thread through `_sound_perf_measurements`. That reasoning
assumed the zone would be emitted where `_pf_data` is written.

Putting the zone in the RAII object instead makes the problem disappear. `PerformanceMeasurer`
for Sound is constructed and destructed on the mixer thread, in `MxMixSamples`, so the zone
opens and closes on that thread, which is both what Tracy requires and where the work actually
is. The deferred `_pf_data` path is untouched.

#### Other deviations

**Copy and move are deleted on both classes, in both configurations.** `tracy::ScopedZone` is
neither copyable nor movable, so a Tracy build would otherwise reject code that a non-Tracy
build accepts. Nothing copies these today; declaring it keeps the two configurations from
diverging into a build that only fails with `OPTION_TRACY=ON`.

**A `static_assert` cannot catch a missing table entry.** The plan assumed it could. The table
is sized from `PerformanceElement::End`, so omitting an entry appends a zeroed one and compiles
cleanly. An assertion in `GetPerformanceSourceLocation` catches it instead, and the header's
"Adding new measurements" block was corrected to say so rather than repeating the wrong claim.

That assertion is **dead in every currently configured tree**, because `build` has asserts and
no Tracy while `build-tracy` has Tracy and no asserts. A zeroed entry would show up as an
unnamed zone in a capture, which is the guard that actually works. Worth knowing before
relying on the assert.

**Source locations point at `framerate_gui.cpp`, not the measured code.** Deliberate: an
accumulated element has several call sites and no single location. The `function` field carries
the real enclosing function name instead, each one verified against the source rather than
guessed.

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy`, RelWithDebInfo | 98 cases, 2176 assertions, pass. |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS**, `015ED3D109C5CCCC`, identical to baseline. |
| Zone names map to the right elements | **Verified**, see below. |

Zone counts from a 2,000 tick headless Hilbergen capture, via `tracy-csvexport`:

| Zone | Count | Expected |
| --- | --- | --- |
| `GameLoop` | 2,000 | once per tick |
| `GameLoopEconomy` | 2,000 | once per tick |
| `GameLoopLandscape` | 6,000 | three call sites per tick |
| `AllScripts` | 2,000 | once per tick |
| `Drawing` | 718 | only when windows are dirty |
| `ViewportDrawing` | 128 | a subset of those redraws |

These sum to 12,846, which is exactly the zone total the capture reported. **This is what
verifies the table is not shifted**, and it is a much stronger check than a passing build: an
off-by-one would have attributed the 6,000 landscape zones to `GameLoopLinkGraph`.
`GameLoopLandscape` landing on exactly three times the tick count is the specific signature
that pins the alignment.

The four vehicle zones are absent, confirming the detail gate works in the off direction.
`Video` and `Sound` are absent because the null drivers never paint or mix, and the script
elements are absent because this fixture runs none.

Trace size was 92.02 KB for 12,846 zones and 2,003 frames, so about **7.3 bytes per zone** on
disk. Use that for sizing future captures rather than the 12 bytes the plan guessed.

#### Two things worth knowing

**`AllScripts` emits a zone even when the harness records nothing.** Its destructor has a hack
that marks the element inactive and returns early when no script is running, but the zone is a
member and closes regardless. So Tracy shows 2,000 `AllScripts` zones at about 139 ns each
while the framerate window shows the element as inactive. Both are correct. Anyone comparing
the two instruments on this element needs to know why they disagree.

**The unattached cost is now non-zero but not resolvable.** `build-tracy` ran 20,000 Hilbergen
ticks at 1,734 ticks/s against 1,782 in T1 and 1,770 for `build-release`. That is a 2.7% drop
from T1, well inside the roughly 7% band the harness README documents for this fixture, so it
cannot be called a real cost from one sample. It is the first measurement whose sign points the
right way for one, though, and worth rechecking if T3 adds another few percent.

#### Untested

The detail tier is only verified in the off direction. Nothing has yet built with
`OTTD_TRACY_DETAIL` defined, so the vehicle zones have never been emitted, and the 13,899 per
tick figure is derived from call site analysis and the harness consist count rather than
observed. T4 is where that tier gets exercised properly.

*Superseded by the next entry: the detail tier now has a CMake option and has been captured.*

### 2026-08-26, T2 follow-up: OPTION_TRACY_DETAIL, and the tier verified

The detail tier had no switch. It was a bare preprocessor define, which meant hand-crafting
`CMAKE_CXX_FLAGS` to use it. Now `OPTION_TRACY_DETAIL` sits next to `OPTION_TRACY` in
`cmake/Options.cmake`, is reported by `show_options()`, and **fails configure with a
FATAL_ERROR if set without `OPTION_TRACY`**, because on its own it changes nothing and a
silent no-op would read as the tier being broken. That guard was tested by setting it on the
`build` tree and confirming configure refuses.

A fourth tree, `build-tracy-detail`, holds it. Separate rather than toggling `build-tracy`
because `OTTD_TRACY_DETAIL` feeds `profiling.h`, which is in the precompiled header, so
flipping it forces a full rebuild each way. Note that `build-tracy` stays the interactive
tree per decision D2; this one is for captures.

#### The per-consist reading is confirmed exactly

First detail capture, Hilbergen, 500 headless ticks:

| Zone | Count | Per tick |
| --- | --- | --- |
| `GameLoopTrains` | 115,500 | **231** |
| `GameLoopLandscape` | 1,500 | 3 |
| `GameLoop` | 500 | 1 |
| `GameLoopEconomy` | 500 | 1 |
| `AllScripts` | 500 | 1 |
| `Drawing` | 187 | when dirty |
| `ViewportDrawing` | 19 | subset |

115,500 is exactly 231 x 500, and 231 is Hilbergen's consist count. The fixture holds 2,730
train **parts**, so a per-part accumulator would have produced 1,365,000. This settles the
question the T2 entry left open: the train accumulator really is inside
`if (IsFrontEngine())` and fires once per consist. All seven counts sum to 118,706, exactly
the total the capture reported.

Road vehicles, ships and aircraft produced no zones because Hilbergen has none of them. Their
detail zones remain unobserved, though the mechanism is now proven by trains.

#### What the detail tier is actually for

The one row that justifies the phase, `GameLoopTrains` over 115,500 samples:

| Statistic | Value |
| --- | --- |
| mean | 1,379 ns |
| min | 41 ns |
| max | 179,790 ns |
| std dev | 2,386 ns |

A 4,385x spread between the cheapest and most expensive consist tick, and a standard deviation
larger than the mean, so the distribution has a long tail rather than being a tight cluster.
**The harness cannot express this.** It reports `ns_per_consist_tick` as a single average, and
an average of a long-tailed distribution invites exactly the wrong conclusion about where the
time goes.

The two instruments do corroborate each other on the average. The harness reported
`perf.trains.ns_per_object_tick` of 121.5 for Hilbergen; scaled by the 2,730 parts to 231
consists ratio that is about 1,436 ns per consist, against Tracy's 1,379 ns mean. Different
run lengths and different trees, so treat the agreement as a sanity check rather than a
measurement.

#### Revised trace sizing

561.19 KB for 118,706 zones is **4.7 bytes per zone**, against 7.3 measured on the much
smaller T2 trace. Larger traces compress better, so use 4.7 for detail captures and 7.3 as the
pessimistic figure for short ones.

That puts a full 5,000 tick wentbourne detail capture at roughly 70 million zones and about
330 MB, rather than the 500 MB estimated in the T2 entry. Still large enough that a few hundred
ticks is the right size for that fixture, but less alarming than it looked.

### 2026-08-26, phase T3: lock contention and plots

Files touched: `src/video/video_driver.hpp`, `src/video/video_driver.cpp`,
`src/vehicle_registry.cpp`, `src/vehicle.cpp`, `src/animated_tile.cpp`,
`src/linkgraph/linkgraphschedule.cpp`.

#### Locks

`game_state_mutex` and `game_thread_wait_mutex` are now `OTTD_LOCKABLE`, which is a plain
`std::mutex` when Tracy is not compiled in. Eight usages, all inside `video_driver.cpp`, and
the five `std::lock_guard<std::mutex>` declarations became plain `std::lock_guard` using class
template argument deduction so they work under either configuration without macro noise. The
manual `unlock`/`lock` pair in `GameLoopPause` needed no change, since `tracy::Lockable`
provides both.

#### Two of the six planned plots do not survive contact

**`map.tileloop_tiles` is dropped.** `RunTileLoop` walks
`1 << (Map::LogX() + Map::LogY() - TILE_UPDATE_FREQUENCY_LOG)` tiles, which is a constant for
a given map size. The plot would be a flat line. `map.animated_tiles` replaces it: the
animated tile list is also landscape workload, it is a cheap `size()` on a vector, and it
actually moves as industries and stations start and stop animating.

**`vehicles.consists` is dropped.** There is no cheap source. Counting primary vehicles needs
a full pool scan, which on wentbourne is 85,259 iterations per tick added to a build purely to
draw a line. `GroupStatistics::num_vehicle` looked like a candidate but its semantics were not
verified, and shipping an unverified metric is worse than shipping none. Parts is the count
that scales with memory traffic anyway, which is what the harness README recommends for
data-layout work. The harness still computes consists correctly if the number is wanted.

**`linkgraph.jobs_running` is plotted at transitions, not per tick.** `LinkGraphSchedule::running`
is protected, so a free function cannot read it, and the count only changes in `SpawnNext` and
`JoinNext`. Tracy holds a plot's last value between points, so a step function draws correctly
from far fewer of them. This is better than the per-tick version the plan described, not a
compromise.

The four that shipped are `vehicles.parts`, `map.animated_tiles`, `ecs.registry_dirty` and
`ecs.sort_us`, plus `linkgraph.jobs_running`.

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy`, RelWithDebInfo | 98 cases, 2176 assertions, pass. |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS**, `015ED3D109C5CCCC`. |
| Plots reach a trace with correct values | **Verified**, see below. |
| Lock contention visible | **Not verified.** Needs an interactive session; see below. |

From a 3,000 tick headless Hilbergen capture, every plot present with the expected point count:
3,000 points each for the per-tick plots and 20 for `linkgraph.jobs_running`, which is
transitions only as designed.

**The values cross-check against figures the harness derived independently**, which is the
part worth trusting:

| Plot | Observed | Independent figure |
| --- | --- | --- |
| `ecs.registry_dirty` | dirty on 2,643 of 3,000 ticks, 88.1% | harness README quotes 88% for Hilbergen |
| `ecs.sort_us` | mean 47.8 us per tick, about 10% of a ~480 us loop tick | README quotes 10.4% of the game loop |
| `vehicles.parts` | 2,800 to 2,834, mean 2,813 | report `load.vehicle_parts` is 2,818 |
| `linkgraph.jobs_running` | 4 to 5 concurrent | matches the several `ottd:linkgraph` rows seen in T1 |

Two instruments built from different data agreeing to a tenth of a percent on the dirty rate is
much better evidence than a plot merely existing.

#### What the plots already show that the report cannot

`ecs.sort_us` ranges from 0 to **744 us** on a single tick, against a mean of 47.8 us and a
game loop tick averaging roughly 480 us. So some individual ticks are spent mostly sorting the
registry. The report's `sort_total_pct_of_game_loop` averages that away into a single 10.4%,
which reads as a steady tax rather than as occasional ticks dominated by one operation. That
distinction matters for the phase 6 decision, since a bursty cost and a steady one call for
different fixes.

#### Lock contention is not verified, and cannot be headlessly

The null video driver never sets `is_game_threaded`, so it has no game thread and never
contends either mutex. A headless capture therefore shows an empty lock timeline that is
indistinguishable from zero contention. This was anticipated in the T1 entry and is now the
concrete blocker for closing T3.

Needs an interactive elevated session, threaded, with the profiler attached. Expect
`game_state_mutex` to show the game thread blocked while the draw thread holds it across
input, `UpdateWindows` and `PopulateSystemSprites`. Running once with `-v win32:no_threads`
gives a control: that trace should show no contention at all.

#### The T2 slowdown was noise

The T2 entry recorded 1,734 ticks/s against T1's 1,782 and flagged it as inside the noise band
but with the sign of a real cost. T3 measured 1,775 ticks/s having added lock wrappers and five
plots on top. So the T2 figure was a low sample, not a trend, and the unattached cost of the
instrumentation remains unmeasurable at this fixture's noise level. Recorded because the T2
entry invited watching for a trend that does not exist.
