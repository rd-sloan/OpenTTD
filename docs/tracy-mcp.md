# Tracy MCP server

How to build, run and trust the Tracy MCP server on this machine. Plan and
rationale are in `docs/tracy-mcp-plan.md`; the integration history is in
`docs/tracy-integration-log.md`.

The server is a Python sidecar shipped in the Tracy source tree. It loads a
`.tracy` capture (or attaches to a running game) into a Tracy `Worker` and lets
an assistant run Python against it. No OpenTTD source, CMake option or build tree
changes for any of this.

## What is installed

| Piece | Version | Where |
| --- | --- | --- |
| CPython | 3.13.15 | `%LOCALAPPDATA%\Programs\Python\Python313` |
| Virtualenv for the sidecar | | `C:\git\tracy\.venv` |
| `mcp` package | 1.29.1, pinned `<2` | in that venv |
| Tracy source | v0.14.0, protocol 82 | `C:\git\tracy` |
| `TracyServerBindings` | `cp313-win_amd64.pyd` | `C:\git\tracy\build\python\Release` |
| Prebuilt profiler and CLI tools | 0.14.0 | `C:\git\tracy-0.14.0` |

Three of those pins are load-bearing.

**`mcp<2`.** Tracy v0.14.0's `tracy_mcp.py` imports `mcp.server.fastmcp`. In mcp
2.x that module was renamed to `mcp.server.mcpserver` and raises a
`ModuleNotFoundError` telling you to pin `<2`. Installing plain `mcp` gets you
2.x and the sidecar will not start.

**Python 3.13, not 3.14.** Tracy pulls pybind11 v2.13.6, which predates 3.14.

**Tracy v0.14.0, not newer.** The sidecar parses `ProtocolVersion` out of its own
`TracyProtocol.hpp` and refuses to attach to a client that disagrees. Ours is 82.
A bindings build from a different tag cannot connect to our game.

## Building the bindings

The bindings are built from the top-level Tracy project, not from `python/` —
that directory has no `project()` of its own and fails to configure standalone.

```powershell
git clone --depth 1 --branch v0.14.0 https://github.com/wolfpld/tracy.git C:\git\tracy

$py = "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe"
& $py -m venv C:\git\tracy\.venv
& C:\git\tracy\.venv\Scripts\python.exe -m pip install "mcp<2"

cmake -S C:\git\tracy -B C:\git\tracy\build -G "Visual Studio 17 2022" -A x64 `
    -DTRACY_CLIENT_PYTHON=ON -DTRACY_STATIC=OFF -DNO_LTO=ON `
    -DPython_EXECUTABLE="C:\git\tracy\.venv\Scripts\python.exe"
cmake --build C:\git\tracy\build --config Release --target TracyServerBindings --parallel
```

`TRACY_STATIC=OFF` is required; the Python bindings need a shared client library
and the CMake script hard-errors otherwise.

`NO_LTO=ON` is the fix for a generate-time failure. `cmake/config.cmake` turns on
`CMAKE_INTERPROCEDURAL_OPTIMIZATION` whenever `CMAKE_BUILD_TYPE` is not `Debug`,
and under a multi-config Visual Studio generator that variable is empty, so the
condition fires. Generation then dies with "CMake doesn't support IPO for current
compiler" on `TracyGetOpt`, the one C-only target. `NO_LTO` is the upstream
escape hatch for this.

This build is completely separate from OpenTTD's. It uses no vcpkg toolchain and
shares nothing with `build`, `build-release`, `build-tracy` or
`build-tracy-detail`.

## Running

```powershell
.\benchmark\start-tracy-mcp.ps1
```

It sets `PYTHONPATH` to the bindings directory, points `TRACY_CAPTURES_DIR` at
`benchmark/manual-traces`, pins the port to 47380, and launches the sidecar. On
success it prints:

```
Tracy MCP listening on http://127.0.0.1:47380/mcp
```

The server binds loopback only and runs as a singleton. Starting a second copy
detects the occupied port and exits.

**Pin the port and believe it.** If 47380 is busy the sidecar silently walks
upward through 16 ports and then falls back to whatever the OS hands out, writing
the real value to `extra/mcp/tracy_mcp.port`. A registered Claude Code URL points
at a fixed port, so a moved server looks like a dead one. If the printed port is
not 47380, find out what took it rather than re-registering around it.

## Registering with Claude Code

```powershell
claude mcp add --transport http --scope local tracy http://127.0.0.1:47380/mcp
claude mcp list
```

Local scope on purpose: project scope writes `.mcp.json` into the repository, and
upstream OpenTTD does not want our Tracy work in the first place.

Claude Code reads its MCP list at startup, so a session already running when you
register will not see the tools. Restart it.

The server has to be up before Claude Code starts, because the sidecar only
speaks `sse` and `streamable-http`. It rejects `stdio` outright, so Claude Code
cannot launch it on demand.

## What the tools are good for

`list_captures`, `list_instances`, `discover_instances`, `live_connect`,
`load_capture`, `save_trace`, `unload_capture`, `eval`, `task`,
`shutdown_server`. Two resources, `tracy://prompt` and `tracy://eval-guide`,
carry the analysis guidance and the `ctx` object model.

The real surface is `eval`, which binds a `Worker` as `ctx` and runs arbitrary
Python against it. All times are nanoseconds.

This is worth being clear-eyed about: `eval` compiles and executes whatever
string it is handed, with full builtins, inside the server process. That is the
design rather than an oversight, and it is why the thing listens on loopback
only. But registering this server does hand an assistant arbitrary code execution
under your account. Stop the sidecar when you are not using it.

## Verification, and one real defect

The bindings were checked against `tracy-csvexport` reading the same file,
`benchmark/manual-traces/t5-Hilbergen-detail.tracy` (735 MB, 56,194 game ticks,
324,127,539 zones). Two independent readers, one trace.

All 25 zone names that occur at a single source location agree **exactly** on
both count and total nanoseconds. `TickVehicle` at 157,552,185 /
24,986,797,620 ns, `CallVehicleTicks` at 56,194 / 37,209,127,778 ns, every YAPF
entry point, all six plots present and named correctly.

The twenty-sixth name is where it breaks.

**`get_all_zone_stats()` silently drops source locations that share a zone
name.** From `python/bindings/ServerModule.cpp:265`, it iterates
`GetSourceLocationZones()` and assigns `result[name] = ZoneStats{...}` keyed by
the bare zone name. Two callsites with the same name means the second overwrites
the first. No summing, no warning, no error.

In this trace `AgeCargoAndPlaySound` is instantiated per vehicle type and has
four source locations, all at `vehicle.cpp:1126`, with counts 152,843,120 /
7,260 / 4,400 / 440. `get_all_zone_stats()` reports **4,400** — the road vehicle
one, whichever landed last — and 152,850,820 zone instances vanish from the
result. That is 47% of the whole trace.

`get_root_zone_stats()` has the same bug at line 313. It accumulates correctly
into a map keyed by source location id and then flattens to names on the way out.

`get_all_zone_source_locations()` is name-keyed too and documents itself as
"Uses first srcloc found per name".

Note also that `docs`' own `eval_guide.md` is wrong here. It claims
`get_all_zone_stats()` returns keys of the form `'name (addr)[arch] <srcloc_id>'`
with a parseable source-location id. In v0.14.0 the keys are bare names and no
binding enumerates source-location ids at all, so the join key it describes does
not exist.

### The guard

Compare the summed stat counts against `get_zone_count()`. They agree only when
no names collide.

```python
stats = ctx.get_all_zone_stats()
gap = ctx.get_zone_count() - sum(s.count for s in stats.values())
# gap 0 -> trustworthy. gap > 0 -> some names collide and their stats are wrong.
```

On this trace the gap is 152,850,822: the 152,850,820 dropped instances plus two
zones left unterminated when the capture ended. Everything reconciles.

Run that check before believing any "hottest zones" ranking.

### What to use instead

`get_zone_durations(name, max_samples)` and `get_zone_occurrences(...)` do
accumulate across every source location sharing a name, and their counts matched
csvexport exactly on spot checks. They materialise one entry per occurrence
though, so they are unusable at 152M zones.

The practical split is to let each tool do what it is actually good at:

- **`tracy-csvexport` for zone counts and totals.** It reports one row per source
  location and gets collisions right.
- **The bindings for everything csvexport cannot see**: sampled call stacks,
  per-zone callstack attribution, plots, locks, sections, frames, thread names,
  context switches. This is the reason to have the server at all.

## Housekeeping

Every loaded capture stays resident for the life of the process and ours are
large, so call `unload_capture` when finished. The built-in backstops (four
instances, 30 minute idle TTLs) are there for forgotten sessions, not routine
cleanup.

`load_capture` returns before statistics finish building. Poll
`is_background_done()` before touching zone stats or you will get partial results
with no error. On the 735 MB capture above it took about 4.5 seconds after a
4.1 second load.
