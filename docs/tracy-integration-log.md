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

Verified via `SetCurrentThreadName`, which is the path every worker takes. The main thread is
named by a separate explicit call in `openttd_main`, so `ottd:main` specifically was not
called out in the report; it is expected among the rows and uses the same macro, but treat it
as inferred rather than observed until someone confirms the label.
