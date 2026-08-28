# Tracy MCP plan

Standalone plan for exposing our Tracy captures to Claude Code through Tracy's
MCP server. Companion to `docs/tracy-integration-plan.md` and
`docs/tracy-integration-log.md`; it does not modify either.

The five ground rules from the Tracy integration still apply here: log the work,
leave the original plan file alone, commit incrementally with reasons, run the
unit suite before submitting, and never push.

## Why bother

Right now every profiling answer goes through one of two paths. Either Sloan
opens the GUI, reads a number off the timeline, and types it into chat, or I run
a headless capture and parse it with the CLI export tools. The first path does
not scale past a handful of numbers and loses the surrounding context. The
second one cannot see sampled call stacks at all, which is exactly the data that
settled the YAPF question in T5 and which I had to ask for secondhand.

The MCP server closes that gap. It loads a `.tracy` file (or attaches to a live
game) into a Tracy `Worker` and lets me run Python against it. Same object the
GUI queries, same numbers, no transcription step.

Concretely, it would have let me answer these T5-era questions myself: which
zones dominate self time, what the sampled stacks under `CallVehicleTicks`
actually look like, and whether the road and ship detail zones ever fire on a
map that has road vehicles.

## What I verified

I read the v0.14.0 source we already fetched rather than trusting release notes.
Everything below is from `build-tracy/_deps/tracy-src`.

The MCP server is `extra/mcp/tracy_mcp.py`, 935 lines of Python. It is not built
by CMake and not shipped in the prebuilt Windows profiler download. It needs
`TracyServerBindings.pyd`, a pybind11 module built from `python/CMakeLists.txt`,
on `PYTHONPATH`.

Tools it exposes: `list_captures`, `list_instances`, `discover_instances`,
`live_connect`, `load_capture`, `save_trace`, `unload_capture`, `eval`, `task`,
`shutdown_server`. Two resources, `tracy://prompt` and `tracy://eval-guide`,
carry the analysis guidance and the `ctx` object model.

`eval` is where the work happens. It binds a `TracyServerBindings.Worker` as
`ctx` and runs arbitrary Python against it. The query surface is wide:
`get_all_zone_stats`, `get_root_zone_stats`, `get_child_zone_stats` (subtract for
self time), `get_frame_times_named`, `get_plots`, `get_locks`, `get_sections`,
`get_callstack_frames`, `get_thread_context_switches`. All times are nanoseconds.

Facts that shape the plan:

- Transport is `sse` or `streamable-http` only. **stdio is rejected at startup**
  (line 903). Claude Code cannot spawn this server as a subprocess; it has to
  connect over HTTP to a server someone started.
- The server binds `127.0.0.1` and nothing else, and refuses to start a second
  copy if one is already running.
- Port is `TRACY_MCP_PORT` or 47380, but `_find_free_port` walks upward 16 ports
  if that one is busy, then falls back to an OS-assigned port. It writes the real
  port to `extra/mcp/tracy_mcp.port`.
- Protocol version comes from parsing `TracyProtocol.hpp` in the server's own
  tree. Ours is 82. A bindings build from a different tag will refuse to attach
  to our client.
- The `python/` build pulls pybind11 v2.13.6, which supports Python up to 3.13.
  Python 3.14 is not supported by that release.
- `VENDOR_GUI` is off for the bindings build, so glfw, freetype, imgui, curl,
  tidy, usearch and pugixml are all skipped. It needs capstone, zstd, PPQSort,
  nlohmann/json and pybind11. Much lighter than the full profiler GUI.

## Blockers

**There is no Python on this machine.** `python --version` hits the Microsoft
Store alias stub and `py -0p` finds no launcher. This is the first thing to fix
and it is a real install, not a flag.

No MCP servers are registered with Claude Code yet, so there is no existing
config to work around.

## Shape of the thing

Three processes, all local:

```
openttd.exe (WITH_TRACY)  --TCP 8086-->  tracy_mcp.py  <--HTTP 47380--  Claude Code
                                              |
                                       TracyServerBindings.pyd
                                              |
                                     .tracy files on disk
```

The game is untouched. `live_connect` speaks the same protocol the GUI does, on
the same port, against the client we already ship. **No OpenTTD source changes at
all**, which is the best thing about this plan. Nothing here can affect the
determinism gate, the benchmark numbers, or a non-Tracy build.

## Phases

### M0: Python toolchain

Install CPython 3.13 for the current user (not the Store alias), confirm
`python --version` and `pip` resolve from a fresh shell, then
`pip install mcp`.

3.13 rather than 3.14 because of the pybind11 2.13.6 ceiling, and rather than
3.12 because there is no reason to give up two years of runtime.

Gate: `python -c "import mcp.server.fastmcp"` exits clean.

### M1: A pinned Tracy source tree for the bindings

Clone `wolfpld/tracy` at tag v0.14.0 to a path outside the repo, `C:\git\tracy`.

Not `build-tracy/_deps/tracy-src`, even though it is the right tag today.
FetchContent owns that directory and will reset it on reconfigure, and I would
rather not have a build tree whose contents depend on whether anyone ran cmake
recently. We already learned that lesson with `build-tracy-detail` going stale.

The v0.14.0 pin is not cosmetic. Protocol 82 has to match on both ends.

Gate: `ProtocolVersion = 82` in the clone's `TracyProtocol.hpp`.

### M2: Build TracyServerBindings

Configure and build `C:\git\tracy\python` with MSVC, Release, x64. It will pull
capstone, zstd, PPQSort, json and pybind11 through CPM on first configure.

Risk worth naming up front: this is the phase most likely to fight us. Capstone
6.0.0-Alpha10 and zstd both build fine on MSVC in general, but the bindings link
`${Python_LIBRARIES}` explicitly on top of what `pybind11_add_module` already
does, which is redundant and occasionally trips over debug/release CRT pairing.
Build Release only and do not mix in a Debug Python.

Gate: with `PYTHONPATH` set to the output directory,
`python -c "import TracyServerBindings; print(TracyServerBindings.__file__)"`
succeeds.

### M3: Run the server and prove it against a known capture

Start with `TRACY_MCP_PORT=47380` and `TRACY_CAPTURES_DIR` pointed at
`benchmark/manual-traces`, then confirm the printed URL and the contents of
`tracy_mcp.port` agree.

Verification is the part I care about. I will not accept "the tool returned a
number" as proof. Load the 300-tick Hilbergen detail capture and check the
numbers I already know independently:

| Check | Expected |
| --- | --- |
| `TickVehicle` zone count | 844,120 |
| `AgeCargoAndPlaySound` zone count | 819,000 |
| Total zones | 1,735,760 |
| `YapfTrainChooseTrack` count | 159 |
| `vehicles.parts` plot mean | 2813.7 |

If those match, the bindings and the file reader are sound and I can trust the
rest of the query surface. If they do not, something is wrong at a layer where I
would otherwise never notice.

Then repeat against a live elevated session to confirm `live_connect` attaches
and sampled stacks come through, since that is the capability the whole exercise
is for.

Open question for M3: whether the GUI and the MCP server can attach to the same
running game at once. Tracy clients historically accept a single server
connection, so I expect these to be mutually exclusive and will confirm rather
than assume. If they are, that is a workflow constraint worth writing down, not a
defect.

### M4: Register with Claude Code

```
claude mcp add --transport http tracy http://127.0.0.1:47380/mcp
```

Scope is the open decision. Project scope writes `.mcp.json` into the repo, which
is a tracked file in a repo whose upstream does not want our Tracy work anyway.
Local scope keeps it in Claude Code's own state and out of the tree. I lean local
for that reason, but it is Sloan's call.

Gate: `/mcp` in Claude Code lists tracy as connected, and I can run
`list_captures` and get the manual-traces directory back.

### M5: Write it down

Add a `docs/tracy-mcp.md` covering how to start the server, the environment
variables that matter, and the capture-verification numbers from M3 so the next
person can re-run the same check. Log the whole thing in
`docs/tracy-integration-log.md` under its own heading.

## Things that will bite

**`eval` is unsandboxed.** It compiles and runs whatever string it is given with
full `__builtins__` in the server process. That is not a bug, it is the entire
design, and it is why the server binds loopback only. But it does mean that
registering this server gives me arbitrary code execution on the machine under
Sloan's account, which is a larger grant than any MCP server currently
configured here. Worth an explicit yes before M4, not a footnote after.

**Port drift breaks a static config.** If 47380 is occupied when the server
starts, it silently moves and the registered URL points at nothing. Pin
`TRACY_MCP_PORT`, and if the server ever reports a different port, that is a sign
something else grabbed it rather than something to work around.

**Traces stay resident.** Every loaded capture holds its full contents in memory
for the life of the process, and our detail captures are large. `unload_capture`
when done. The built-in backstops (4 instances, 30 minute idle TTLs) are for
forgotten sessions, not routine cleanup.

**`load_capture` returns before statistics are built.** A background thread
populates zone and symbol stats after the file is read. Check
`is_background_done()` before trusting `get_all_zone_stats`, or get partial
results with no error. On our captures this is probably subsecond, but "probably"
is how the stale detail tree happened.

**The server has to be running.** No stdio means no autostart. Every session that
wants profiling data starts with someone launching the script. If that friction
turns out to matter, a scheduled task or a shortcut is the fix, not a code
change.

## What this does not do

No changes to OpenTTD source, CMake, or any build tree. `WITH_TRACY` and
`OTTD_TRACY_DETAIL` stay exactly as they are. The four existing build trees keep
their current roles, and the three gate trees are unaffected. Nothing here can
change a benchmark number or a state fingerprint.

## Cost

M0 and M1 are minutes. M2 is the unknown; a clean CPM-fed MSVC build of capstone
plus zstd plus the server library is maybe 10 to 20 minutes of wall clock if
nothing goes wrong, and an afternoon if capstone does not like MSVC. M3 through
M5 are short. Call it half a day with the M2 risk priced in.

## Decisions

Answered 2026-08-28:

1. `eval` code execution: approved.
2. MCP scope: local.
3. Tracy clone at `C:\git\tracy`: confirmed.

## Outcome

All five phases done the same day. Operating instructions live in
`docs/tracy-mcp.md`; this section only records where the plan was wrong.

Four things the plan did not predict:

**The `mcp` package needed pinning.** `pip install mcp` gets 2.x, which renamed
`FastMCP` to `MCPServer`. Tracy v0.14.0's sidecar imports `mcp.server.fastmcp`
and will not start. Pinned `mcp<2` in a venv at `C:\git\tracy\.venv` rather than
polluting the base interpreter.

**M2 was right to be flagged as the risky phase, but wrong about why.** I
expected capstone to fight MSVC. It built without complaint. The two failures
were both CMake wiring: `python/` has no `project()` and cannot be configured
standalone (the top-level project with `TRACY_CLIENT_PYTHON=ON` is the supported
route), and `cmake/config.cmake` enables IPO whenever `CMAKE_BUILD_TYPE` is not
`Debug`, which under a multi-config generator means always, killing generation on
the one C-only target. `NO_LTO=ON` is the upstream escape hatch. Total build time
was closer to 10 minutes than the afternoon I priced in.

**My M3 reference numbers were attached to a capture that no longer exists.** The
plan gated on `TickVehicle` at 844,120 from a 300 tick detail capture. The file at
`benchmark/manual-traces/t5-Hilbergen-detail.tracy` is a 56,194 tick capture with
324 million zones, so nothing matched and the gate failed on first run. The
numbers in the log were real, the capture behind them was never saved. Replaced
the gate with something better anyway: cross-checking the bindings against
`tracy-csvexport` on the same file compares two independent readers and needs no
historical reference at all.

**That cross-check found a genuine bindings defect**, which is the single best
argument for having run it. `get_all_zone_stats()` and `get_root_zone_stats()`
are keyed by bare zone name, so source locations sharing a name overwrite each
other silently. On the trace above that discards 47% of all zone instances.
`eval_guide.md`'s documented `'name (addr)[arch] <srcloc_id>'` key format does
not exist in v0.14.0. Details, the integrity guard, and the workaround are in
`docs/tracy-mcp.md`.

Had the original gate "passed" against a matching capture, this defect would have
sailed through: every single-source-location zone agrees perfectly, so a check
that happened to avoid `AgeCargoAndPlaySound` would have looked clean.
