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
