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

**Confirmed the same day.** Sloan ran an interactive session and observed both the lock
contention and the five plots. T3 is verified in full and nothing in it remains on trust.

### 2026-08-26, phase T4: vehicle tick zones

Standard-tier zones for the per-tick structure of `CallVehicleTicks`, and a detail-tier zone
per vehicle *part*. Files touched: `src/vehicle.cpp`, `src/vehicle_registry.cpp`,
`src/profiling.h`.

#### Two zones cannot share a scope

`ZoneScoped` and friends all declare a variable literally named `___tracy_scoped_zone`, so a
second one in the same scope is a redefinition error. `CallVehicleTicks` needs two: one for
the function and one for the autoreplace loop. Tracy's answer is `ZoneNamedN(varname, name,
active)`, now wrapped as `OTTD_ZONE_NAMED_N`.

Worth recording because of how it surfaced. **The `build` tree compiled cleanly**, since every
macro expands to nothing there, and both Tracy trees then failed with `C2374`. That is the
third time this session that the Tracy-off configuration has proved nothing about the Tracy-on
one, after the CRT mismatch in T1 and the untested detail tier in T2. Build `build-tracy`
before believing a compile.

#### Deviations from the plan

**The vehicle tick pass gets no zone of its own.** Wrapping it would mean adding a scope
around the `#ifdef`/`#else` block and reindenting roughly thirty lines of `CallVehicleTicks`,
which is the function the ECS migration is actively rewriting. Every other part of the
function is named, so the pass shows up as `CallVehicleTicks` self time, which carries the
same information for a one-line diff instead of a thirty-line one.

**The typed passes are deliberately not instrumented.** `OTTD_ECS_TICK_TYPED_PASSES` is
defined only when `OTTD_ECS_TICK_VARIANT_B` or `_C` is, and both are commented out in
`vehicle_registry.h`, so `TickVehiclesOfType` is not compiled in any current build. Zones
there would be dead code that no capture could check. Given that the last three phases each
turned up a defect in exactly the code that had not been observed in a trace, adding
unverifiable instrumentation is the wrong trade. Add and verify those zones when a variant is
actually enabled; the template will need a per-type name, since a literal cannot vary across
instantiations.

**No zone was added around the `LoadUnloadStation` loop.** The plan asked for one, but that
scope already carries the `GameLoopEconomy` measurer, so T2's bridge zone covers it. A second
zone on the same scope is noise.

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy` | 98 cases, 2176 assertions, pass. |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS**, `015ED3D109C5CCCC`. |
| Both tiers reach a trace with correct counts | **Verified**, below. |

Standard tier, 2,000 headless Hilbergen ticks. All four new zones fire exactly once per tick:

| Zone | Per tick | Mean |
| --- | --- | --- |
| `CallVehicleTicks` | 1 | 409 us |
| `SortVehicleRegistry` | 1 | 48.1 us |
| `RunEconomyVehicleDayProc` | 1 | 1.7 us |
| `Autoreplace` | 1 | 79 ns |

`SortVehicleRegistry` at 48.1 us matches the `ecs.sort_us` plot's 47.8 us mean from T3, which
is a zone and a plot built from separate code agreeing on the same quantity.

Detail tier, 300 ticks, 1,735,577 zones:

| Zone | Per tick | Mean | Cross-check |
| --- | --- | --- | --- |
| `TickVehicle` | 2,813.7 | 129 ns | T3's `vehicles.parts` plot averaged 2,813.4 |
| `AgeCargoAndPlaySound` | 2,730 | 16 ns | exactly Hilbergen's train part count |
| `GameLoopTrains` | 231 | 1,387 ns | the consist count, as in the T2 follow-up |

**Parts against consists is now directly visible in one trace**: 2,813.7 parts to 231
consists, a factor of 12.2 on this fixture. That ratio is the thing the harness README spends
a section explaining, and it is now a property of the capture rather than a calculation.

#### The detail tier inflates what it measures

`GameLoop` averages 441 us per tick under the standard tier and 611 us under the detail tier.
`CallVehicleTicks` goes 409 us to 570 us. So roughly 5,500 extra zones per tick cost about
**40% of the measured region** on this fixture.

**Read detail traces for shape and counts, not for absolute cost.** Relative comparisons
inside a single detail trace are still sound, since every part pays the same overhead, but a
number taken from one does not belong next to a `build-release` figure. This is the same
discipline the harness README applies to assert-enabled builds, and it applies more strongly
here.

#### Trace sizing revised again

3,030.88 KB for 1,735,577 zones is **1.8 bytes per zone**, against 4.7 measured in the T2
follow-up and 7.3 in T2. Tracy's compression improves sharply with volume, so the earlier
figures were pessimistic for exactly the captures that need sizing.

Re-estimating wentbourne with detail: about 184,000 zones per tick, so a 300 tick capture is
roughly 55 million zones and **about 100 MB**. That is far more comfortable than the 330 MB
projected in the T2 follow-up or the 500 MB in T2. Use 1.8 bytes per zone for large captures
and 7 for short ones.

#### The instrumentation cost still does not show

`build-tracy` ran 20,000 Hilbergen ticks at 1,735 ticks/s. Across the four phases the figures
are 1,782, 1,734, 1,775 and 1,735 against `build-release`'s 1,770, which is a band of about
2.7% with no trend. Standard-tier instrumentation remains free at this fixture's noise level
even after four phases of additions.

### 2026-08-26, phase T5: pathfinder entry points only

Nine zones at the YAPF entry points, and nothing deeper. Files touched:
`src/pathfinder/yapf/yapf_rail.cpp`, `yapf_road.cpp`, `yapf_ship.cpp`.

#### Driven by sampling, which is what the plan asked for

The plan says to sample first and instrument based on what the samples say, rather than
guessing where pathfinder time goes. Sloan captured an elevated interactive Hilbergen session
and read the sampling statistics. The whole of YAPF came to roughly **0.33% of samples**:

| Symbol | Share of samples |
| --- | --- |
| `CYapfCostRailT<>::PfCalcCost` | 0.15% |
| `CYapfSegmentCostCacheGlobalT<>::PfNodeCacheFetch` | 0.12% |
| `CYapfBaseT<>::FindPath` | 0.04% |
| `CYapfBaseT<>::AddNewNode` | 0.02% |

**So the internals are deliberately left uninstrumented.** Per-node zones inside
`follow_track.hpp` and the cost functions would carry the volume risk the plan flagged, in
exchange for resolving a third of one percent. The entry points fire per path request, which
is bounded by vehicle count, so they are standard tier and safe at any run length.

This is the phase the plan was least specific about, and the sampling made the decision
cheaply. Worth noting that the process worked: an hour of instrumentation was avoided by five
minutes of looking.

#### The result is a fixture property as much as a code property

Hilbergen is 231 settled consists on established routes, and the captured zones show
`YapfTrainChooseTrack` firing 0.51 times per tick, so a given consist re-paths roughly once
every 450 ticks. Path caching is doing its job on a network that does not change.

Wentbourne has 13,899 consists on a 1024x1024 map and would exercise this far harder. **These
zones are the instrument that answers the same question there**, without another
instrumentation pass. Do not read "YAPF is negligible" as a general conclusion; read it as
"YAPF is negligible on Hilbergen, and here is how to check wentbourne".

#### Gate results

| Gate | Result |
| --- | --- |
| `openttd_test`, `build`, Debug | 98 cases, 2176 assertions, pass. |
| `openttd_test`, `build-tracy` | 98 cases, 2176 assertions, pass. |
| State fingerprint, Hilbergen, 20,000 ticks | **PASS**, `015ED3D109C5CCCC`. |
| Zones reach a trace at safe volume | **Verified**, below. |

From 2,000 headless Hilbergen ticks:

| Zone | Count | Per tick | Mean | Share of captured time |
| --- | --- | --- | --- | --- |
| `YapfTrainChooseTrack` | 1,027 | 0.51 | 15.5 us | 0.99% |
| `YapfTrainCheckReverse` | 86 | 0.04 | **22.9 us** | 0.12% |
| `YapfTrainFindNearestDepot` | 65 | 0.03 | 5.3 us | 0.02% |
| `YapfTrainFindNearestSafeTile` | 8 | 0.00 | 11.0 us | 0.01% |
| `YapfNotifyTrackLayoutChange` | 1 | 0.00 | 0.3 us | 0.00% |

1,187 pathfinder zones across 2,000 ticks, so **0.59 per tick**. The standard-tier decision is
confirmed empirically rather than assumed: this is nothing next to the 6.4 zones per tick the
rest of the standard tier already emits.

The zone total of about 1.14% of captured time and the sampling's 0.33% are not the same
measure. Zones cover wall time inside the call including children, sampling covers CPU
self-time across the whole process. They agree that pathfinding is small, which is all that
was being asked.

#### One thing sampling could not have shown

**`YapfTrainCheckReverse` is the most expensive pathfinder call on this fixture**, at 22.9 us
mean against `YapfTrainChooseTrack`'s 15.5 us. Sampling ranked it nowhere, because it runs 12
times less often, and share-of-samples flattens per-call cost into total cost.

It is not an optimisation target at 0.12% of the trace. It is a good illustration of why the
two instruments are both worth having: sampling finds where the time is, zones find what is
expensive per call, and those are different questions with different answers.

#### Process gap: the detail tree went stale

T5 was gated on `build` and `build-tracy` but **not** on `build-tracy-detail`, which was left
holding a T4-era binary with no pathfinder zones in it. Sloan caught it. Nothing incorrect was
claimed, since no T5 result came from that tree, but anyone running it would have got stale
code with no indication.

Rebuilt and re-verified: a 300 tick detail capture now carries both tiers, 1,735,760 zones,
with `TickVehicle` at 844,120 and `AgeCargoAndPlaySound` at 819,000, identical to the T4
capture, plus the five pathfinder zones.

**Rule going forward: `build-tracy-detail` is a capture tree, not a gate tree, and its binary
must be rebuilt immediately before any detail capture.** Its age is not a reliable signal,
because most phases change instrumentation without changing anything the tree's own
configuration would notice. Adding it to every phase's gates would mean a fourth full rebuild
per phase for a tree used occasionally; rebuilding on demand is the right trade, provided the
demand is actually honoured.

The three gate trees remain `build` for the unit suite, `build-release` for the fingerprint
baseline, and `build-tracy` for everything Tracy-specific.

#### The T2 slowdown was noise

The T2 entry recorded 1,734 ticks/s against T1's 1,782 and flagged it as inside the noise band
but with the sign of a real cost. T3 measured 1,775 ticks/s having added lock wrappers and five
plots on top. So the T2 figure was a low sample, not a trend, and the unattached cost of the
instrumentation remains unmeasurable at this fixture's noise level. Recorded because the T2
entry invited watching for a trend that does not exist.

### 2026-08-28: Tracy MCP server

Separate track from T0-T5 and outside the phase numbering, because it changes nothing in this
repository's build. Plan and post-mortem in `docs/tracy-mcp-plan.md`, operating instructions in
`docs/tracy-mcp.md`. Only the parts that bear on the Tracy work are repeated here.

Sloan approved the three open decisions: `eval` code execution yes, MCP scope local, clone at
`C:\git\tracy`.

The server is a Python sidecar in Tracy's own tree. It reads `.tracy` files, or attaches to a
running game over the same port and protocol the GUI uses. No OpenTTD source, CMake option or
build tree is touched, so none of the gates or benchmark figures are affected. Registered with
Claude Code at local scope on `http://127.0.0.1:47380/mcp`; it only speaks HTTP, rejecting
`stdio`, so it has to be started before Claude Code rather than launched on demand.

Unit suite before commit: 2176 assertions in 98 test cases, pass.

#### The verification gate failed, and that was the point

The plan gated M3 on numbers from the 300 tick detail capture recorded above: `TickVehicle` at
844,120, 1,735,760 zones total. Every one of them mismatched by three orders of magnitude.

The reason is mundane. That capture was never saved. The file sitting at
`benchmark/manual-traces/t5-Hilbergen-detail.tracy` is a 56,194 tick run with 324,127,539
zones, and the numbers in this log came from a capture I made, read and discarded. **A recorded
number is only a reference if the artefact behind it still exists.** Worth remembering the next
time this log quotes a figure as a baseline.

The replacement gate is better and should have been the plan all along: run `tracy-csvexport`
over the same file and compare. Two independent readers of one trace need no historical
reference, and either agreement means something or it does not.

#### A real defect in the bindings

The cross-check found that `get_all_zone_stats()` is keyed by bare zone name
(`python/bindings/ServerModule.cpp:265`, `result[name] = ZoneStats{...}`). When two source
locations share a name the second overwrites the first, with no summing and no warning.
`get_root_zone_stats()` has the same bug at line 313.

This bites our instrumentation directly. `AgeCargoAndPlaySound` is instantiated per vehicle
type and has four source locations, all at `vehicle.cpp:1126`, with counts 152,843,120 / 7,260
/ 4,400 / 440. The bindings report 4,400 and drop 152,850,820 zone instances, 47% of the trace.

Every one of the 25 single-source-location zones agreed exactly on both count and total
nanoseconds, which is the uncomfortable part: a gate that happened not to touch
`AgeCargoAndPlaySound` would have looked clean.

The guard is to compare `sum(s.count for s in stats.values())` against `get_zone_count()` and
distrust the ranking if they differ. On this trace the gap is exactly the dropped instances
plus two zones left unterminated at capture end.

Consequence for how we use this: `tracy-csvexport` stays the authority on zone counts and
totals, and the bindings are for what csvexport cannot see, which is sampled call stacks, plots,
locks, sections, frames and context switches. That was the reason to want the server anyway.

Also worth flagging because it cost time: Tracy's own `extra/mcp/eval_guide.md` documents these
keys as `'name (addr)[arch] <srcloc_id>'` with a parseable source-location id. That format does
not exist in v0.14.0, and no binding enumerates source-location ids at all.

### 2026-08-28: per-type names for the AgeCargoAndPlaySound zone

Follows directly from the bindings defect above. `AgeCargoAndPlaySound` is a template over
`VehicleType`, and `src/vehicle.cpp` was the only zone in the tree that let Tracy derive its
label from `__FUNCTION__` instead of naming itself. MSVC reports `__FUNCTION__` without
template arguments, so all four instantiations emitted their own source location under one
shared name, and anything aggregating by name saw one of the four.

The fix is a `constexpr` name selector per instantiation, giving
`AgeCargoAndPlaySound<Train>`, `<Road>`, `<Ship>` and `<Aircraft>`.

**Naming it explicitly would not have been enough.** A fixed literal still gives four source
locations carrying one name. The name has to vary with the template parameter, which is the
rule to carry into the deferred `TickVehiclesOfType` typed-pass zones, since those are
templates too.

Cost is nil. Each instantiation already built its own `static constexpr SourceLocationData`;
only the name pointer differs, and it resolves at compile time. Nothing is added to a path
that runs 3.4 million times per 40 ticks on wentbourne.

#### Verified

Hilbergen, 300 ticks, detail: `TickVehicle` 844,120 and `AgeCargoAndPlaySound<Train>` 819,000,
both identical to the pre-rename figures. Only the train instantiation appears, and the harness
report says why: `load.consists_road`, `load.consists_ships` and `load.consists_aircraft` are
all 0 at load. That also settles the open question from T4 and T5 about why the road, ship and
aircraft detail zones were never observed. It was the fixture, not the instrumentation.

Wentbourne, 40 ticks, detail, which does have all four types:

| Zone | Count | Cross-check |
| --- | --- | --- |
| `AgeCargoAndPlaySound<Train>` | 3,007,280 | 15.6x `GameLoopTrains` (193,320), long consists |
| `AgeCargoAndPlaySound<Road>` | 219,960 | exactly `GameLoopRoadVehicles`, single-part |
| `AgeCargoAndPlaySound<Ship>` | 112,720 | exactly `GameLoopShips`, single-part |
| `AgeCargoAndPlaySound<Aircraft>` | 61,160 | 2.04x `GameLoopAircraft` (29,960), shadow and rotor parts |

The four sum to 3,401,120 against `TickVehicle` at 3,410,360. The 9,240 difference is effect
and disaster vehicles, which tick but carry no cargo, exactly as the `static_assert` says.

The bindings integrity guard now returns a gap of **zero** on both captures, where the older
Hilbergen trace was short by 152,850,822.

State fingerprint, Hilbergen, 20,000 ticks: `build-tracy` and `build-tracy-detail` both
reproduce the recorded baseline `015ED3D109C5CCCC`. Unit suite 2176 assertions in 98 test
cases, pass. `build-tracy-detail` was rebuilt before capturing, per the rule above.

### 2026-08-28: the T5 pathfinder caveat, closed

T5 instrumented the nine YAPF entry points and nothing inside them, on the strength of a
Hilbergen sampling panel where the hottest pathfinder frame was 0.15%. That entry recorded the
caveat plainly: the finding was Hilbergen-only and wentbourne was unchecked. First real use of
the Python bindings was to check it.

**The conclusion does not carry over.** Wentbourne spends 12.79% of `GameLoop` inside the YAPF
entry points, against 2.17% on Hilbergen. By sampled self time the same gap shows as 5.11% of
`openttd.exe` self samples against 0.69%.

Both captures are `t5-*.tracy`, standard tier, elevated, with the integrity guard returning a
gap of zero on each.

| Zone | wentbourne calls | wentbourne total | Hilbergen calls | Hilbergen total |
| --- | --- | --- | --- | --- |
| `YapfRoadVehicleChooseTrack` | 56,840 | 9,413.87 ms | 10 | 0.15 ms |
| `YapfRoadVehicleFindNearestDepot` | 27,404 | 1,236.55 ms | 0 | 0 |
| `YapfShipChooseTrack` | 53,198 | 1,164.01 ms | 2 | 0.18 ms |
| `YapfTrainChooseTrack` | 38,316 | 928.45 ms | 102,316 | 1,986.12 ms |
| `YapfTrainCheckReverse` | 5,942 | 188.50 ms | 8,075 | 220.69 ms |
| `YapfShipCheckReverse` | 2,620 | 151.44 ms | 1 | 0.07 ms |
| `YapfTrainFindNearestDepot` | 12,414 | 61.12 ms | 4,219 | 27.89 ms |
| **Total** | | **13,143.95 ms** | | **2,238.45 ms** |
| | | **12.79% of GameLoop** | | **2.17% of GameLoop** |

**Road vehicles are the whole story.** `YapfRoadVehicleChooseTrack` alone is 9.41 of the 13.14
seconds, 72% of all pathfinding, and it costs 165.6 us per call against 24.2 us for the train
equivalent. Hilbergen called it ten times in the entire capture, which is why none of this was
visible there. Same fixture blindness that hid the road, ship and aircraft detail zones.

The two figures above are not the same measurement and should not be averaged. The zone number
is YAPF wall time over `GameLoop` wall time and is the direct answer. The sample number divides
by all `openttd.exe` self samples across the whole capture, which includes viewport and GUI work
outside the game loop, so it reads lower. Whole-capture percentages are worse still, because
Tracy resolves its own call stacks through dbghelp and that accounts for roughly 10% of samples
in these traces.

#### The T5 decision holds up, for a reason worth keeping

Entry-point-only instrumentation was the right call even though the premise behind it was wrong.
The zones were enough to size the cost, and sampling supplied the internals for free:

| Symbol | wentbourne self | Hilbergen self |
| --- | --- | --- |
| `CBinaryHeapT<CYapfRoadNode>::HeapifyDown` | 0.533% | 0.018% |
| `HashTableSlot<CYapfRoadNode>::Find` | 0.233% | n/a |
| `CYapfCostRailT<...>` | n/a | 0.090% |
| `CFollowTrackT<0,Train,1,1>::Follow` | n/a | 0.060% |

The hot code differs by map. Wentbourne is node list machinery, the binary heap and hash table
lookups. Hilbergen is cost calculation and track following. Instrumenting either set internally
would have paid for zones in the other map's cold path. Entry-point zones for attribution plus
sampling for internals is the combination that works, and it is now demonstrated rather than
assumed.

#### Standing correction

T5's summary should be read as "pathfinding is negligible on Hilbergen", not "pathfinding is
negligible". Any future statement about a cost being negligible needs the fixture named in the
same sentence. Hilbergen loads with zero road, ship and aircraft consists, so it cannot answer
questions about three of the four vehicle types.

### 2026-08-28: where road vehicle pathfinding actually spends its time

The entry above sized road pathfinding at 12.79% of `GameLoop` on wentbourne and stopped there.
This is the breakdown inside it, from `t5-wentbourne.tracy` (2,180 ticks, standard tier,
elevated, integrity guard gap zero). Three separate costs came out of it, and one of them is
not really about pathfinding at all.

#### Method, and two things that nearly went wrong

Sampling is the only way in, because our zones do not capture call stacks.
`get_zone_callstacks("YapfRoadVehicleChooseTrack")` returns an empty list. That is not a
bindings defect. `src/profiling.h` builds on `ZoneScopedN`, not the call-stack-capturing
`ZoneScopedNS`, which is the right choice for a zone on a hot path. It does mean that per-zone
callstack attribution, which `docs/tracy-mcp.md` lists as a reason to have the bindings, is
unavailable for every capture we have taken.

So the tree below is built from `get_symbol_stats()` inclusive counts. That is sound here
because the YAPF classes are templates and every symbol in the road path is its own
instantiation, `CYapfBaseT<CYapfRoad_TypesT<CYapfRoad,CYapfDestinationTileRoadT> >` and so on.
The depot search instantiates `CYapfRoadAnyDepot` separately, so the two do not mix. Only
`CFollowTrackT<1,RoadVehicle,1,0>` and the tile helpers are shared, and those are called out.

**Samples convert to time on this capture.** Dividing the `YapfRoadVehicleChooseTrack` zone
total by its inclusive sample count gives 126,528.9 ns per sample, which is 7.90 kHz, and Tracy
samples at 8 kHz. Two independent measurements of the same subtree agreeing to 1.3% is the
licence to quote microseconds per call off sample counts. Do this check per capture rather than
assuming the rate.

**`get_symbol_stats()` returns one row per address, and inlined copies share a name.** Keying a
dict by name loses samples the way `get_all_zone_stats()` does, and it is not documented as a
hazard because here the rows are all present and it is the reader who drops them. My first table
had `HeapifyDown` at 4,537 self samples from one row when the three rows sum to 7,549, and
`PopOpenNode` at 1,303 inclusive against a true 11,510. Sum by name, always.

#### The distribution, which is the first surprise

| Percentile | Road ChooseTrack | Train ChooseTrack | Road FindNearestDepot |
| --- | --- | --- | --- |
| p50 | 42.2 us | 13.6 us | 46.9 us |
| p75 | 194.8 us | 26.4 us | 60.7 us |
| p90 | 503.3 us | 51.9 us | 72.1 us |
| p99 | 1,281.5 us | 181.6 us | 99.3 us |
| max | 3,479.9 us | 1,054.9 us | 1,932.3 us |
| mean | 165.6 us | 24.2 us | 45.1 us |
| slowest 10% holds | 50.6% of total | 44.0% | 19.2% |

The road median is 42 us, a quarter of the mean. The 165.6 us figure quoted in the entry above
is not a typical search, it is a median search plus a fat tail, and half the total time sits in
the slowest tenth of calls. The slowest 1% only holds 9.9%, so this is not a handful of
pathological searches that could be clamped. The whole upper half of the distribution is
expensive.

The depot search is the opposite shape, tightly clustered between 47 and 72 us. That is the
signature of a search that almost always runs to its cost budget rather than finding anything.

#### The breakdown, per call of the 165.6 us mean

| | incl samples | % of entry | us/call | self |
| --- | --- | --- | --- | --- |
| `YapfRoadVehicleChooseTrack` | 74,401 | 100% | 165.6 | 3 |
| `FindPath` | 71,063 | 95.5% | 158.2 | 760 |
| ..`PfFollowNode` | 59,249 | 79.6% | 131.9 | 29 |
| ....`AddMultipleNodes` | 56,257 | 75.6% | 125.2 | 143 |
| ......`AddNewNode` | 43,687 | 58.7% | 97.2 | 2,091 |
| ........`PfCalcCost` | 31,781 | 42.7% | 70.7 | 1,809 |
| ..........`SlopeCost` | 8,626 | 11.6% | 19.2 | 447 |
| ........`CreateNewNode` to `deque::_Emplace_back_internal` | 11,578 | 15.6% | 25.8 | 491 |
| ........`InsertOpenNode` | 2,772 | 3.7% | 6.2 | 923 |
| ....`CFollowTrackT<1,RoadVehicle,1,0>::Follow` | 17,771 | 23.9% | 39.6 | 1,100 |
| ..`PopOpenNode` | 11,510 | 15.5% | 25.6 | 366 |
| ....`CBinaryHeapT::HeapifyDown` | 7,549 | 10.1% | 16.8 | 5,130 |
| ..`HashTable` open and closed `Find` | 5,514 | 7.4% | 10.2 | 578 |
| ....`HashTableSlot::Find` | 5,082 | 6.8% | 11.3 | 3,445 |
| ..`deque` destructor and `_Tidy` | 2,900 | 3.9% | 6.5 | 5 |

`Follow` is indented under `PfFollowNode` for readability but it is called from two places, once
per followed node and again per tile inside `PfCalcCost`'s segment walk. Subtracting the other
children puts roughly 15,000 of its 17,771 samples inside `PfCalcCost`. So segment cost
evaluation, counting the track following it drives, is about 105 us of the 165.6 us.

### Finding 1: on MSVC every pathfinder node is its own heap allocation

`NodeList` stores nodes in `std::deque<Titem> items` (`nodelist.hpp:27`). The code holds `Node *`
into that storage across insertions, so a growing `vector` is genuinely not an option and the
`deque` is a reasonable-looking choice. On MSVC it is not.

From `VC/Tools/MSVC/14.44.35207/include/deque:550`:

```cpp
static constexpr int _Block_size = _Bytes <= 1 ? 16
                                 : _Bytes <= 2 ? 8
                                 : _Bytes <= 4 ? 4
                                 : _Bytes <= 8 ? 2
                                               : 1; // elements per block (a power of 2)
```

Any element over 8 bytes gets one element per block, which is one heap allocation per element.
`CYapfRoadNode` is about 48 bytes by layout: an 8-byte key, `hash_next` and `parent`, two ints,
`is_choice`, then `segment_last_tile` and `segment_last_td`. So every node A* touches is a
separate `malloc`, and the `deque` destructor frees them one at a time when the search ends.

The samples say so plainly. The whole `deque<CYapfRoadNode>` subtree is 20,383 inclusive samples
with 613 of self time, so 97% of it is inside the allocator. `RtlAllocateHeap` is 34,046
inclusive samples across the entire capture and `RtlFreeHeap` is 12,933 of self.

| Node type | deque subtree samples | seconds | % of `GameLoop` |
| --- | --- | --- | --- |
| `CYapfRoadNode` | 20,383 | 2.58 | 2.51% |
| `CYapfShipNode` | 2,741 | 0.35 | 0.34% |
| `CYapfRailNode` | 1,562 | 0.20 | 0.19% |
| **Total** | **24,686** | **3.12** | **3.04%** |

Three percent of the simulation on wentbourne is `malloc` and `free` for pathfinder nodes, and
it is 19.5% of road `ChooseTrack` specifically. This is the most attractive item on the list: a
chunked arena keeps the pointer stability the algorithm needs, the node contents are trivially
destructible so teardown becomes dropping a bump pointer, and the storage can survive across
searches instead of being rebuilt each time. It changes no cost, no ordering and no result, so
it is the rare pathfinder change a state fingerprint can fully validate.

It also hits rail and ship, and it is a libstdc++ and libc++ question separately. Both use a
512-byte minimum block, so they were already batching about ten nodes per allocation. This is
substantially an MSVC problem, which is worth saying out loud before anyone treats the 3% as a
universal figure.

### Finding 2: `SlopeCost` computes every tile height twice

`CYapfCostRoadT::SlopeCost` (`yapf_road.cpp:36`) takes `tile` and `next_tile` and calls
`GetSlopePixelZ` on the centre of each. `PfCalcCost`'s segment walk then does this:

```cpp
segment_cost += Yapf().SlopeCost(tile, follower_local.new_tile, trackdir);
...
tile = follower_local.new_tile;
```

Next iteration's `z1` is the height of the tile just measured as `z2`. Every interior tile of
every segment is measured twice, and the second measurement is guaranteed to produce the number
the first one already had.

`SlopeCost` is 19.2 us of the 165.6 us in `ChooseTrack` and 5.3 us of the 45.1 us in the depot
search, so 9,784 samples or 1.24 seconds, 1.2% of `GameLoop`. `GetTileSlopeZ` shows 9,328
inclusive samples across the whole capture, which within sampling noise means the road pathfinder
is essentially its only caller. Carrying the previous `z` forward halves it.

Smaller than finding 1 and less interesting, but it is provably result-identical, confined to one
function, and worth about 0.6% of `GameLoop` on this fixture.

### Finding 3: road has no segment cost cache, and probably cannot have one

Rail runs `CYapfSegmentCostCacheGlobalT` and `CSegmentCostCacheT::Get`. Road runs the `None`
variant, so `PfNodeCacheFetch` always returns false and `AddNewNode` calls `PfCalcCost`
unconditionally. That is most of the 7x gap in per-call cost, and it also explains the 9x gap in
the depot searches, 45.1 us for road against 4.92 us for trains.

Before proposing a cache, note why there is not one. Road segment cost depends on the vehicle and
on live state, not only on the geometry. `OneTileCost` reads `entry.GetOccupied()` and
`rs->IsFreeBay()`, and the segment walk penalises speed limits against `v->GetDisplayMaxSpeed()`
and `v->current_order.GetMaxSpeed()`. A cache keyed on the segment alone would return wrong
costs, and wrong costs on a shared cache is a desync class of bug, not a performance regression.
I would leave this alone.

There is a cheaper variant of the same idea, though. `AddNewNode` calls `PfCalcCost` **before**
it checks the open and closed lists, so a child node whose key is already closed pays a full
segment walk and is then discarded. The key is set by `n.Set(...)` before the cost call, so the
closed-list lookup could come first. That is not free of consequence: the current code treats a
closed node that turns out cheaper as a bug in the cost or estimate function, and reordering
makes that case silently unreachable rather than asserted. Measure it before deciding.

### The separate finding: `RunEconomyVehicleDayProc` is 71% road depot searches

Not part of the pathfinder question, and it fell out of `get_child_zone_stats`:

| Child | Calls | Total | Share of parent |
| --- | --- | --- | --- |
| `YapfRoadVehicleFindNearestDepot` | 27,404 | 1,236.6 ms | 70.8% |
| `YapfTrainFindNearestDepot` | 12,414 | 61.1 ms | 3.5% |

`RunEconomyVehicleDayProc` is 1,746.6 ms over 2,180 ticks and road-vehicle depot searches are
nearly three quarters of it. They come from `CheckIfRoadVehNeedsService`
(`roadveh_cmd.cpp:1686`), gated on `NeedsAutomaticServicing()`, bounded by
`maximum_go_to_depot_penalty`, and the result is thrown away when `best_length` exceeds that
penalty.

The tight 47 to 72 us distribution says these searches are nearly all running the budget out.
That fits wentbourne having road vehicles with no depot in range: the search cannot terminate
early on success because there is no success, so it expands the full radius and returns nothing.
A vehicle in that state repeats the search on its next day proc, and the answer is stable while
the vehicle is where it was.

I have not measured the failure rate, so this is a hypothesis with a distribution shape behind it
rather than a result. It is also the cheapest thing on this page to test.

### What to measure next, in order

1. **Nodes per search, per transport type.** Every estimate above is per call, and per node is
   what the algorithm is actually sensitive to. `nodes.TotalCount()` and `ClosedCount()` at the
   end of `FindPath` as a plot, detail tier. This also answers whether road searches hit
   `max_search_nodes` (default 10,000) and how often.
2. **The depot search failure rate.** A counter on `best_length == UINT_MAX` against total calls.
   One plot, and it decides whether a negative-result cache is worth anything.
3. **Closed-list hits inside `AddNewNode`.** How much of `PfCalcCost` is computed and discarded.

All three are counters and plots rather than new zones, so they cost close to nothing on the hot
path.

### What this does not say

Wentbourne only. Hilbergen loads with zero road consists and called
`YapfRoadVehicleChooseTrack` ten times in an entire capture, so it has nothing to say about any
of this. Following the standing correction from the previous entry, every number here is a
wentbourne number.

The 3.04% allocation figure is MSVC-specific and should not be quoted for a GCC or Clang build.

And the whole page is measurement, not a change. Findings 1 and 2 look like clean wins and
finding 3 looks like a trap, but nothing here has been prototyped, built or fingerprinted.

### 2026-08-28: memory tracking planned, in a new document

`docs/tracy-memory-plan.md`. Sloan asked for a plan rather than an implementation, so nothing is
built yet.

It exists as its own file rather than as an entry here because it is a plan and this is a log,
and because ground rule 2 forbids editing `docs/tracy-integration-plan.md`, whose section 9 is
the three-paragraph placeholder it replaces. The new document says so at the top. Same
precedence rule as before: where the frozen plan and the new one disagree about memory, the new
one wins, and where this log and the new plan disagree about what actually happened, the log
wins.

Four things came out of the research that changed the shape of the plan, all worth having here
in case the plan file drifts:

**`TracyAlloc` captures no call stacks.** `TRACY_CALLSTACK` defaults to 0 (`Tracy.hpp:139`) and
`MemAllocCallstack` with depth 0 falls through to plain `MemAlloc`. So the allocation hot-spot
tree, which is the main reason to want this, is empty unless the `S`-suffixed macros are used
with an explicit depth. That splits the work into a cheap tier and an expensive one and is why
the plan has three tiers rather than two.

**An unbalanced free terminates the capture.** Not a warning. Tracy drops the connection and the
trace ends. That makes a global `operator new` override the riskiest single change in the plan,
because getting half of a new/delete pair overridden is exactly the failure. `TRACY_ON_DEMAND`,
already forced on at `CMakeLists.txt:311`, relaxes the case where the allocation predates the
connection, which is the only reason this is workable at all.

**Leak detection conflicts with a decision already taken.** The active-allocations-at-exit list
needs the tail of the capture, and `TRACY_NO_EXIT` is deliberately off (`CMakeLists.txt:318`)
because it hangs the game at exit with no profiler attached. So the leak list is unreliable in
every tree we have. A leak run needs its own scratch build.

**Memory events carry callstack indices where zones do not.** `get_memory_events` returns a
`callstack_idx` per allocation and `get_callstack_frames` resolves it. That is the same
capability the pathfinder dig went looking for and did not find, since our zones use
`ZoneScopedN` rather than `ZoneScopedNS`. Memory tracking is where the bindings' callstack access
finally becomes useful, which is an argument for going as far as the callstack tier rather than
stopping at the cheap one.

Also recorded there, because it is a trap: only `CargoPacket` sets `Tcache`, so markers in
`Pool::AllocateItem` and `FreeItem` measure slot occupancy rather than heap traffic for that one
pool, and the pool figures are a second independent accounting of bytes the default pool already
counted. Those numbers must never be added together.

Phase M0 is counters only, no Tracy events, because allocation volume is unmeasured and every
tier decision downstream depends on it. The estimate in the plan, 6,000 to 12,000 allocations
per tick from road pathfinding alone, divides a sampled time by a guessed cost per `malloc` and
should not be treated as a measurement.

No code, no build, no capture. `openttd_test` in `build` still passes at 2176 assertions in 98
test cases, which for a documentation change proves only that the tree was clean when this was
written.
