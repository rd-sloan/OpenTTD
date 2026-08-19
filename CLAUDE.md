# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project AI policy — read first

`CONTRIBUTING.md` contains an explicit **Use of AI** policy: the project does not accept issues or pull requests written by an LLM, and "using an AI tool to generate entire lines of code is unacceptable". The stated rationale is reviewer time and GPL v2 authorship.

Treat this repo as **read/understand/assist** territory. Help with investigation, explanation, debugging, and reviewing. If asked to write code that will be submitted upstream, say plainly that upstream policy forbids AI-generated submissions, and let the user decide (a private fork or patchpack is their call, not yours to assume).

## Build

C++20, CMake, no in-source builds. Dependencies come from vcpkg on Windows.

An MSVC build tree already exists at `build/` (Visual Studio 17 2022, `x64-windows-static`, toolchain `C:/git/vcpkg/scripts/buildsystems/vcpkg.cmake`, `CMAKE_BUILD_TYPE` unset → multi-config; asserts ON).

```powershell
# Build (from repo root; multi-config, so pass --config)
cmake --build build --config Debug
cmake --build build --config Debug --target openttd       # game only
cmake --build build --config Debug --target openttd_test  # unit tests only

# Reconfigure from scratch
cmake -S . -B build -G"Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE="C:/git/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET="x64-windows-static"
```

Binaries land in `build/Debug/openttd.exe` and `build/Debug/openttd_test.exe`.

Useful CMake options (see `COMPILING.md`, `cmake/Options.cmake`): `-DCMAKE_BUILD_TYPE=RelWithDebInfo`, `-DOPTION_DEDICATED=ON` (no GUI), `-DOPTION_USE_ASSERTS=OFF`, `-DOPTION_TOOLS_ONLY=ON` (only `strgen`/`settingsgen`), `-DCMAKE_CXX_FLAGS_INIT="-DRANDOM_DEBUG"` (desync hunting).

**Adding or removing a source file requires editing the relevant `CMakeLists.txt`** — sources are listed explicitly via `add_files()` / `add_test_files()` (see `cmake/SourceList.cmake`). Duplicate basenames anywhere in the tree are a hard configure error.

`GRFCodec`/`NFORenum`, if installed, will regenerate `.grf` files **into the source tree** and dirty your working copy. Clear `GRFCODEC_EXECUTABLE`/`NFORENUM_EXECUTABLE` in the CMake cache to avoid that.

## Tests

Two distinct suites, both wired into CTest:

```powershell
ctest --test-dir build -C Debug --timeout 120        # everything (CI runs `ctest --timeout 120`)
ctest --test-dir build -C Debug -R regression        # only the script regressions
ctest --test-dir build -C Debug -R "<test name>"     # single test
```

**Unit tests** — Catch2 (`src/3rdparty/catch2`), sources in `src/tests/`, entry point `src/tests/test_main.cpp`, registered individually via `catch_discover_tests`. Run a single case directly:

```powershell
build\Debug\openttd_test.exe "name of the test case"
build\Debug\openttd_test.exe "[tag]"
build\Debug\openttd_test.exe --list-tests
```

New unit test files must be added to `src/tests/CMakeLists.txt`. `mock_environment.h` / `mock_spritecache.*` / `mock_fontcache.h` exist because most of the codebase depends on global game state — prefer testing pure helpers in `src/core/` and `src/misc/`.

**Regression tests** (`regression/`) drive the real `openttd` binary with a GameScript and diff its output against a recorded `.txt`. Four suites: `regression`, `stationlist`, `gs`, `gs_compat`. Prefer the custom targets over ctest — they resolve dependencies and are more verbose:

```powershell
cmake --build build --config Debug --target regression             # all
cmake --build build --config Debug --target regression_regression  # one
```

They run serially and are the safety net for the Script API. See `cmake/CreateRegression.cmake` and `cmake/scripts/Regression.cmake`.

## Architecture

### The central invariant: deterministic game state

Multiplayer sync is the constraint that shapes nearly everything. Clients receive a savegame on join, then only receive *commands* (unpredictable inputs: player actions, AI, GameScript, admin port). Everything else — vehicle movement, tree growth, industry production — is re-simulated identically on every client. Read `docs/desync.md` before touching simulation code.

Practical consequences:

- **Game state vs. client state.** `_settings_game` is part of the savegame and identical everywhere; `_settings_client` is local and must never influence simulation. Same split for `_current_company` vs. `_local_company`.
- Never let GUI state, local time, uninitialised memory, pointer values, iteration order of address-keyed containers, or unsynchronised randomness leak into game state.
- Game-state randomness goes through `Random()` (`src/core/random_func.hpp`); GUI/cosmetic randomness uses `InteractiveRandom()`. Mixing them up desyncs.
- Caches must be derivable from the state they cache; `src/cachecheck.cpp` verifies this when `-d desync=2` is set.

### Commands: the only sanctioned way to mutate game state

- `Commands` enum in `src/command_type.h` lists every command. Handlers live in `src/<feature>_cmd.cpp`, declared in `src/<feature>_cmd.h`, and are bound to the enum with `DEF_CMD_TRAIT(Commands::X, CmdX, flags, type)`.
- Handlers return `CommandCost` (or `std::tuple<CommandCost, ...>` for extra results) and take `DoCommandFlags` plus typed parameters — the parameter list *is* the network serialisation format (`src/misc/endian_buffer.hpp`).
- **Every handler runs twice**: once without `DoCommandFlag::Execute` (test run — compute cost, validate, change nothing) and once with it (apply). Both passes must agree, and the test pass must be side-effect free.
- Callers use `Command<Commands::X>::Post(...)` (send to server, async, optional error `StringID` and callback) or `::Do(flags, ...)` (immediate, in-place, used by other commands and by test runs). GUI code almost always wants `Post`.
- `CommandFlag`s gate who may run what (`CommandFlag::Deity`, `CommandFlag::Server`, `CommandFlag::Offline`, ...) and how it behaves during pause; see `IsCommandAllowedWhilePaused`.

### Object pools

Most game objects (`Vehicle`, `Town`, `Station`, `Industry`, `Company`, `Order`, `Engine`, `Sign`, ...) live in fixed-index pools — `src/core/pool_type.hpp`, base classes in `src/<feature>_base.h`. Identity is the pool index (`VehicleID`, `StationID`, ... built on `PoolID` / strong typedefs), not the pointer, because indices are what savegames and the network protocol carry. `Get`, `GetIfValid`, `Iterate`, and `INVALID_*` / `::Invalid()` sentinels are the standard access patterns. Pool types (`Normal`, `NetworkClient`, `NetworkAdmin`, `Data`) control what gets cleared on new game vs. what survives.

### The tile map

The world is a flat array of 8-byte `TileBase` + extended entries (`Map` in `src/map_func.h`). Raw bits are never touched directly outside the accessor headers: each tile class owns a `src/<thing>_map.h` (`rail_map.h`, `station_map.h`, `town_map.h`, `industry_map.h`, `water_map.h`, `tree_map.h`, `void_map.h`, ...) which defines the typed getters/setters and asserts the tile type. `src/tile_map.h` handles the common header (tile type, owner, height), `src/slope_func.h` the geometry. `docs/landscape.html` and `docs/landscape_grid.html` document the bit layout.

### Saveload

`src/saveload/` — one `*_sl.cpp` chunk handler per subsystem, orchestrated by `saveload.cpp`. Backwards compatibility is a hard project guarantee ("every revision can load savegames from every older revision"):

- `SaveLoadVersion` enum + `SAVEGAME_VERSION` in `saveload.h`; fields are versioned with `SLE_COND*` / `SLEG_COND*` macros, guarded at runtime by `IsSavegameVersionBefore()`.
- `saveload/afterload.cpp` fixes up old saves after loading — this is where migrations live, and it is large and version-ordered.
- `saveload/compat/` holds the field ordering for pre-named-field savegames; `oldloader*.cpp` reads TTD-era saves.
- Format details: `docs/savegame_format.md`.

### GUI

`src/<feature>_gui.cpp` per window, widget enums in `src/widgets/<feature>_widget.h`. A window is a `Window` subclass (`src/window_gui.h`) plus a static `WindowDesc` referencing a `NWidget(...)`-macro nested-widget tree (`src/widget_type.h`, laid out by `src/widget.cpp`). GUI code reads game state and issues commands; it must never mutate game state directly. `src/window.cpp` owns the window stack, event dispatch and invalidation.

### Strings and localisation

- **Only edit `src/lang/english.txt`.** Every other language file is owned by the Eints web translator and synced automatically; translation PRs are rejected. `docs/eints.md` has the rules for adding/renaming strings.
- `src/strgen/` compiles `english.txt` into `table/strings.h` (the `STR_*` `StringID` enum) at build time, and each language into a `.lng`.
- Runtime formatting: `src/strings.cpp`, `EncodedString` for parameterised messages that must survive being sent over the network or stored in a savegame.
- CI runs `.github/unused-strings.py`; unreferenced `STR_*` entries fail the build.

### Settings

Settings are declared in `src/table/settings/*.ini` and turned into tables by `src/settingsgen/`. The C++ side is `src/settings_type.h` (the `_settings_game` / `_settings_client` structs), `src/settings.cpp`, `src/settings_table.cpp`. Adding a setting means touching the `.ini`, the struct, and usually `english.txt` — and a game setting also means a savegame version bump.

### NewGRF

Two layers:

- `src/newgrf/` — the loader: one file per GRF pseudo-action (`newgrf_act0*.cpp` for properties, `newgrf_act2.cpp` for spritegroups, etc.), plus `newgrf_bytereader.*`.
- `src/newgrf_*.cpp` at `src/` level — the runtime per feature (`newgrf_engine`, `newgrf_house`, `newgrf_industries`, `newgrf_station`, `newgrf_town`, ...), resolving variables/callbacks (`newgrf_callbacks.h`, `newgrf_spritegroup.*`) into sprites and behaviour.

NewGRF behaviour is an author-facing API: changes need a `[NewGRF]` commit component and must stay backwards compatible for existing GRFs.

### Script API (AI / GameScript)

- Squirrel VM in `src/3rdparty/squirrel`, glue in `src/script/squirrel*`.
- The API is `src/script/api/script_*.hpp|cpp`. CMake auto-generates the Squirrel bindings (`*.sq.hpp`) from the headers, and `script_window.hpp` from the widget enums — so widget enums are part of the public API surface.
- Versioned: `bin/ai/compat_*.nut` and `bin/game/compat_*.nut` shim older API versions; `ai_changelog.hpp` / `game_changelog.hpp` document changes. Anything that changes API behaviour needs a compat script and a `[Script]` commit component.
- Every API function reaching game state must assert its execution mode with `EnforceDeityMode` / `EnforceCompanyModeValid` / `EnforceDeityOrCompanyModeValid` (`script_error.hpp`). CI enforces this via `.github/script-missing-mode-enforcement.py`.
- The `regression`/`gs`/`gs_compat` suites are how API changes get validated.

### Networking

`src/network/` — `network_client.cpp` / `network_server.cpp` (game protocol), `network_command.cpp` (command queue + frame scheduling), `network_coordinator`/`stun`/`turn` (NAT traversal, `docs/game_coordinator.md`), `network_content*` (BaNaNaS downloads), `network_admin.cpp` (`docs/admin_network.md`), `network_crypto*` (monocypher-based auth), `network_survey.cpp`. Wire (de)serialisation lives in `network/core/`.

### Timers

`src/timer/` provides four *different* clocks; picking the wrong one is a common bug:

- `TimerGameCalendar` — in-game dates as the player sees them (affected by day length).
- `TimerGameEconomy` — economy time units; what production, payments and stats run on.
- `TimerGameTick` — raw game ticks.
- `TimerGameRealtime` — wall clock, for GUI/animation only (never game state).

### Other notable areas

`src/pathfinder/yapf` (the pathfinder; `follow_track.hpp` is the tile-traversal core), `src/linkgraph` (cargodist, `docs/linkgraph.md`), `src/video` + `src/blitter` + `src/spriteloader` (driver/rendering backends, selected at runtime via `src/provider_manager.h`), `src/core` and `src/misc` (reusable containers, `format.hpp`, `string_consumer.hpp`, `EnumBitSet`, `OverflowSafeInt`, kdtree), `src/os` (platform layer), `src/table` (static data tables).

### Naming conventions

Suffixes are load-bearing and consistent across the tree: `_type.h` (types/enums only, minimal includes), `_func.h` (free functions), `_base.h` (pool object class), `_map.h` (tile accessors), `_cmd.h`/`_cmd.cpp` (commands), `_gui.cpp` (windows), `_sl.cpp` (saveload chunk), `.hpp` (templates with inline implementation).

## Code style and conventions

`CODINGSTYLE.md` is authoritative and detailed. Highlights that bite most often:

- **Tabs** for indentation, only at line start. No trailing whitespace (CI fails on it). Unlimited line length; continuations indent two tabs.
- `CamelCase` functions and types; `lower_snake_case` variables; globals prefixed `_`; always `this->` for own members; `*`/`&` next to the name.
- Own-member access via `this->`; conventional short names are expected (`Vehicle *v`, `Station *st`, `Town *t`, `Window *w`, `Engine *e`).
- Braces: `{` on the next line for functions, same line for control flow. Single-statement `if` without `else` may go on one line, unbraced.
- `/* */` for standalone comments, `//` only trailing on a code line. ASCII only, even in comments.
- **Every file needs a `@file` Doxygen block** or Doxygen documents nothing in it — CI checks this (`.github/file-descriptions.py`). Function docs go in the `.cpp`, inline/template docs in the header. `///<` for member/enum comments.
- Don't re-align existing vertical comment blocks to fit an addition; keep diffs small.
- `src/safeguards.h` `#define`s away `malloc`/`realloc`/`strcpy`/`sprintf`/`strcmp`/`memcpy`/`memset`/… into compile errors. Use `std::string`, `std::vector`, `std::unique_ptr`, `std::span`, `fmt`/`src/core/format.hpp`, and the helpers in `src/string_func.h` / `src/core/string_*`.
- `NOT_REACHED()` for unreachable defaults, `[[fallthrough]]` for intentional fall-through.

### Commit messages

Strictly validated by CI (`.github/workflows/commit-checker.yml` → OpenTTD-git-hooks). Format:

```
<keyword>( #<issue>|<commit>)?: ([<component>])? <details>
```

Player-facing keywords: `Feature`, `Add`, `Change`, `Fix`, `Remove`, `Revert`, `Doc`, `Update`. Developer-facing: `Codechange`, `Cleanup`, `Codefix`. Components include `[NewGRF]`, `[Script]`, `[Network]`, `[YAPF]`, `[CI]`, `[CMake]`, OS names. `<details>` starts with a capital, no trailing dot, describes what the *player* notices. Exactly one keyword. For fixes, include the issue number and, if known, the 7-char hash of the introducing commit.

The client-side hooks that check style and commit messages locally are a separate repo (`OpenTTD/OpenTTD-git-hooks`), symlinked into `.git/hooks`.

## Docs worth knowing about

`docs/desync.md` and `docs/debugging_desyncs.md` (determinism, `-d desync=<level>`), `docs/savegame_format.md`, `docs/eints.md`, `docs/landscape*.html` (tile bit layout), `docs/linkgraph.md`, `docs/admin_network.md`, `docs/game_coordinator.md`, `docs/logging_and_performance_metrics.md`, `docs/directory_structure.md`, `docs/releasing_openttd.md`.
