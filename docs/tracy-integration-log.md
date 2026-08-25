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
