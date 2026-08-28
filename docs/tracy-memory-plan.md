# Tracy memory tracking plan

Plan for adding Tracy memory profiling to the `EnTT-Tracy` branch. Written 2026-08-28, after
the road vehicle pathfinder analysis turned up allocation churn as a cost in its own right.

**This supersedes section 9 of `docs/tracy-integration-plan.md`.** That section is three
paragraphs saying "treat it as a later phase with its own justification". This is the
justification and the phase. The frozen plan is not edited, per ground rule 2, so read this
instead.

The ground rules in `docs/tracy-integration-log.md` govern this work unchanged: keep the log,
commit incrementally with the reason, run `openttd_test`, never push. Progress and deviations go
in the log, not here.

## Why now

The wentbourne pathfinder dig produced a number nobody was looking for. `std::deque` on MSVC
allocates one block per element for any element over 8 bytes, so every YAPF node is an
individual `malloc`, and the deque destructor frees them one at a time when the search ends.
That is 3.04% of `GameLoop` across road, ship and rail node lists, and `RtlAllocateHeap` plus
`RtlFreeHeap` came to 47,550 samples of the capture against 851,768 for all of `openttd.exe`.

Sampling found that by accident, from the shape of one subtree. It cannot answer the follow-up
questions: how many allocations per tick, from where, how large, how long they live, and whether
the pathfinder is the biggest offender or merely the one that happened to sit under a zone we
had already instrumented. Those are the questions memory tracking answers directly.

The second reason is the ECS migration. `entt::basic_registry::get` is the single hottest symbol
in the capture at 9.54% of self samples, and storage growth in `entt::basic_storage` is a real
allocation pattern that will change as the migration proceeds. A before-and-after on registry
allocation is worth having while there is still a before.

## What Tracy gives, and what it costs

From the manual's memory profiling section, the payoff list is: an interactive memory usage
graph, active allocations at exit (so leaks), a memory map view, the ability to rewind both to
any point in the capture, per-zone memory statistics, and an allocation hot-spot tree.

Two of those are the reason to do this. **Per-zone memory statistics** attribute allocations to
the zones already instrumented in T1 through T5, which means the existing zone tree becomes an
allocation tree for free. **The hot-spot tree** ranks call stacks by allocation, which is the
thing sampling could not give us. The usage graph and the memory map are nice and will mostly go
unused.

The costs are specific and worth stating before any design:

**Every free must have a matching alloc, or Tracy kills the capture.** The manual is blunt about
it: "Tracy will terminate the profiling session if this assumption is broken." Not a warning, not
a dropped event. The connection drops and the trace ends. Causes it lists are mismatched
`malloc`/`new`, reporting the same address allocated twice without a free between, double frees,
and allocations made in a library but freed in the application.

**On-demand mode relaxes exactly this.** `TRACY_ON_DEMAND` is already forced on in
`CMakeLists.txt:311`, so a free whose allocation happened before the profiler connected is
tolerated. That is a large part of why this is feasible at all, and it means the option must
never be turned off in a build that reports memory.

**The default macros capture no call stacks.** `TRACY_CALLSTACK` defaults to 0
(`Tracy.hpp:139`), and `TracyAlloc` expands to `MemAllocCallstack(ptr, size, TRACY_CALLSTACK)`,
which with depth 0 falls through to plain `MemAlloc`. So `TracyAlloc` alone gives the usage graph
and the leak list but an **empty hot-spot tree**. The tree needs `TracyAllocS(ptr, size, depth)`,
which walks the stack on every allocation. That is the expensive tier and it needs its own gate.

**Tracy takes its own lock per event.** `MemAlloc` and friends hold `m_serialLock` around the
queue write (`TracyProfiler.hpp:553`), because memory events must preserve program order and
cannot be reconstructed. So every tracked allocation is a lock acquire, on both the game and draw
threads. This is the thing that makes volume the governing constraint, in the same way zone
volume governed T4.

**Tracy's own allocator is separate.** Manual line 1055: the client uses its own memory
allocator, so a global `operator new` override does not recurse into the profiler. Good news, and
worth knowing before someone goes hunting for a recursion guard.

## The volume problem, which decides the design

T4 established the discipline: estimate volume, then check the estimate against a real capture
before trusting it. The same applies here and the numbers are worse.

A rough bound from the pathfinder dig. The `deque<CYapfRoadNode>` subtree is 20,383 samples, or
2.58 seconds, and 97% of that is inside the allocator rather than in deque code. At a plausible
50 to 100 ns per `malloc`/`free` pair that is somewhere between 13 and 26 million pairs over
2,180 ticks, so roughly 6,000 to 12,000 allocations per tick from road pathfinding alone.

I do not trust that estimate and neither should anyone else. It divides a sampled time by a
guessed constant. But it is enough to establish the shape of the problem: allocation volume on
wentbourne is plausibly in the same range as the detail-tier zone volume that T4 had to gate
behind `OPTION_TRACY_DETAIL`, and a lock acquire per event is more expensive than a zone.

**So phase M0 measures the volume with counters before anything reports to Tracy.** Everything
downstream depends on the answer and there is no point designing tiers around a guess.

## OpenTTD's allocation surface

What is reachable, and how. This is the inventory the design has to cover.

| Path | Mechanism | Reachable how |
| --- | --- | --- |
| Everything using `new`/`delete`, `std::vector`, `std::string`, `std::deque` | global `operator new` | one override, catches all of it |
| Pool objects (`Vehicle`, `Town`, `Station`, ...) | `pool_func.hpp:100` `allocator.allocate(size)` | named pool per object pool |
| `CargoPacket` | same, but `Tcache` free list keeps freed slots | see the warning below |
| Squirrel VM | `ScriptAllocator::DoAlloc`, `squirrel.cpp:78` | named pool, already tracks its own total |
| YAPF node lists | `std::deque<Titem>` in `nodelist.hpp:27` | global override, or a named pool if the deque is replaced |
| EnTT registry and storages | `std::allocator` inside EnTT | global override only |
| C third-party code (ICU, zlib, libpng via vcpkg) | `malloc` directly | not reachable, and consistently invisible |

Two things fall out of that table.

**There is no global `operator new` override in the tree today.** The only `operator new`
declarations are deletions, in `pool_type.hpp:308`, `backup_type.hpp:198` and `window_gui.h:297`,
plus the pool's placement forms. So adding one is a new global behaviour rather than an edit to
an existing hook, and it is the single change that makes the whole C++ allocation surface visible.

**`std::allocator<uint8_t>` routes through `operator new`.** Both the pool allocator and
`ScriptAllocator` use it, so a global override catches them too, and any named-pool markers we
add on top are a *second, independent* accounting of the same bytes. Tracy keeps pools separate
and that is the intent, but nobody should ever add the pool figures to the default pool figure.

**The `Tcache` warning.** Only `CargoPacket` sets `Tcache` (`cargopacket.h:28`). With it,
`FreeItem` pushes the slot onto `alloc_cache` instead of deallocating, and `AllocateItem` pops it
back. So markers placed in `AllocateItem`/`FreeItem` measure **pool slot occupancy, not heap
traffic**, and for `CargoPacket` they will show churn where the heap sees none. That is arguably
the more useful view for a pool, but it has to be labelled as such or the two numbers will get
compared and one of them will look wrong.

## Design

### Macros

Same shape as the existing `OTTD_*` wrappers in `src/profiling.h`, for the same three reasons
that header already documents: `safeguards.h` bans `malloc`, the build must work with
`WITH_TRACY` undefined, and the tiers do not exist in Tracy.

```
OTTD_MEM_ALLOC(ptr, size)              default pool, no callstack
OTTD_MEM_FREE(ptr)
OTTD_MEM_ALLOC_N(ptr, size, name)      named pool
OTTD_MEM_FREE_N(ptr, name)
OTTD_MEM_DISCARD_N(name)               mass free of a named pool
```

The callstack depth is not a macro parameter. It is a compile-time constant chosen by the tier,
so no call site has to know or care whether stacks are being captured. That keeps the decision in
one place and out of thirty.

`OTTD_MEM_DISCARD_N` maps to `TracyMemoryDiscard`, which the manual describes for arena
allocators that free everything at once. It is the right marker for `Pool::CleanPool` and for the
chunked-arena replacement of the YAPF node deque, if that gets written.

### Named pools need a stable unique pointer

Tracy identifies a memory pool by the *pointer* to its name string, not by the string's contents
(the manual's section on unique pointers). `Pool::name` is a `std::string_view` and every pool is
constructed from a literal, so `this->name.data()` is stable, unique and null-terminated. That
works, and it breaks silently the day someone constructs a pool from a `std::string` or a
formatted name. Worth an assertion or at least a comment at the call site.

### Tiers

Three tiers, and the middle one is new territory rather than a variation on what exists.

| Tier | Gate | What reports | Volume |
| --- | --- | --- | --- |
| Named pools | `WITH_TRACY` | pool objects, Squirrel VM | bounded, thousands per tick at worst |
| Global heap | `OTTD_TRACY_MEM` | every `new`/`delete` in the process | unknown until M0, assume very high |
| Callstacks | `OTTD_TRACY_MEM_CALLSTACK` | as above, plus a stack walk per event | short captures only |

Named pools go on the standard tier because their volume is bounded by pool churn, which is
orders of magnitude below total allocation, and because they are the tier that answers a question
the harness already half-answers. The harness reports that 71,360 of wentbourne's vehicle parts
carry 88 bytes of `BaseConsist` that means nothing to them. That is a static figure. A `Vehicle`
memory pool on the timeline turns it into a churn figure.

The global tier is off by default even in a Tracy build, for the same reason
`OPTION_TRACY_DETAIL` is: `build-tracy` has to stay playable, and this is the change most likely
to break that.

### Build options

Two new options in `cmake/Options.cmake`, next to the existing pair, with the same
`FATAL_ERROR` guard against enabling a dependent option alone:

```
OPTION_TRACY_MEM            -> OTTD_TRACY_MEM,           requires OPTION_TRACY
OPTION_TRACY_MEM_CALLSTACK  -> OTTD_TRACY_MEM_CALLSTACK, requires OPTION_TRACY_MEM
```

I considered folding both into one option with a depth number. Two booleans win: the callstack
tier is a different kind of capture with a different run length, and a number in a CMake cache
variable invites someone to set it to 64 and wonder why the game stopped.

A fourth build tree, `build-tracy-mem`, following the `build-tracy-detail` precedent. Not a
scratch definition on an existing tree. The log already records why: a tree name in the filename
is how a capture stays attributable, and the detail tree had to be rebuilt before capturing once
already.

### Where the global override lives

A new file, `src/profiling_mem.cpp`, holding the `operator new`/`operator delete` family and
nothing else. Reasons for a dedicated file rather than adding to an existing one:

- It must **not** include `safeguards.h`. The override needs `malloc`, which safeguards defines
  into a compile error. `free` happens not to be banned, but `malloc` is. Every other `.cpp`
  includes safeguards last by convention, so the exemption belongs in a file whose whole purpose
  is to be the exception, with the reason in its `@file` block. No CI check enforces the
  inclusion, so this is a convention deviation rather than a build failure.
- Replacement `operator new` must be defined exactly once in the program, so it cannot live in a
  header.
- It needs an entry in `src/CMakeLists.txt` via `add_files()`, guarded on the option.

**Override the whole family or none of a pair.** The hazard is not overriding too little, it is
overriding half of a matched pair. `operator new(size_t)` with the default
`operator delete(void*)` reports allocs with no frees; the reverse reports frees with no allocs,
which is the one Tracy kills the session over. The pairs are: plain, array, sized delete, nothrow,
and the C++17 aligned forms. `std::allocator<uint8_t>::allocate` uses the plain form, so plain
and sized-delete are the ones that carry the traffic, but any pair left half-done is a live
grenade. This is the part of the implementation to write slowly.

## Phasing

Every phase ends with the gates below, and `build-tracy` stays playable throughout.

**M0, counters only. No Tracy.** The global `operator new` family, incrementing two atomics and
nothing else, plus `OTTD_PLOT` of allocations and bytes per tick at the end of `GameLoop`. This
is the cheapest possible version of the override, so it also proves the override itself is
correct and complete before any Tracy event depends on it.

Deliverable is a number: allocations per tick on both fixtures, and the same split by the
existing zone tree if the plot resolution allows. That number decides whether the global tier is
a standard-tier feature, a detail-tier feature, or not viable at all.

**M0 is not optional and it is not merged with M1.** Getting the operator family wrong is the
main risk in this whole plan, and a counter that reads wrong is a bad number, while a Tracy event
that reads wrong is a terminated capture with no diagnostic beyond "connection lost".

**M1, named pools.** `OTTD_MEM_ALLOC_N`/`FREE_N` in `pool_func.hpp` `AllocateItem` and
`FreeItem`, `OTTD_MEM_DISCARD_N` in `CleanPool`, and the Squirrel allocator. Standard tier. Two
lines in one template gives a memory pool per object pool, which is the best coverage per line
changed in this plan, the same argument that put T2 early.

Label the `CargoPacket` pool explicitly as slot occupancy in the log entry, per the `Tcache`
warning above.

**M2, global heap, no callstacks.** Swap M0's counters for `OTTD_MEM_ALLOC`/`FREE` behind
`OPTION_TRACY_MEM`. Keep the counters and the plot; they are the cross-check that the Tracy
event count matches what the process actually did, which is the memory equivalent of the
integrity guard in `docs/tracy-mcp.md`.

This is the phase that answers the pathfinder question, because the YAPF node deques are only
reachable this way.

**M3, callstacks.** `OPTION_TRACY_MEM_CALLSTACK` switches the macros to the `S` forms with a
fixed depth. Depth 16 to start. The chain from `GameLoop` down to `HeapifyDown` runs through a
dozen or so named frames on the road path, and the interesting distinctions sit near the bottom
of it, so a short stack would collapse exactly the callers worth telling apart. I have not
measured actual stack depth, so treat 16 as a starting point to check rather than a figure with
anything behind it.

Short captures only, hundreds of ticks, in the manner of a detail-tier capture. This is the phase
that produces the hot-spot tree, and it is the one worth doing on both fixtures despite the cost,
because Hilbergen and wentbourne have already disagreed about which code is hot twice.

**Deferred, with reasons.** Replacing the YAPF node deque with a chunked arena is a fix, not
instrumentation, and belongs in its own change with its own fingerprint run. Tracking GPU or
driver memory is not interesting here. Per-zone memory statistics need no work at all, they
appear once M2 lands.

## Gates

The three existing gates apply unchanged, and the table in the log's "What the gates actually
prove" section is still the honest account of what they miss. Two additions specific to this
phase:

**A capture that survives is the balance proof.** Tracy terminating the connection is the only
detector for an unbalanced alloc/free pair, and it is a good one. A completed capture with a
non-empty memory graph is evidence the operator family is consistent. A capture that dies partway
means the family is wrong, so treat a dropped connection as a build defect and not as a flaky
network.

**The counter cross-check.** M0's atomics stay in for M2 and M3. Compare the process's own
allocation count against the memory event count the bindings report. They should agree once
on-demand connection timing is accounted for. This is deliberately the same pattern as the zone
integrity guard, for the same reason: two independent readers of one trace is the only way any of
these numbers have been believed so far.

**On leak detection, one honest caveat.** The active-allocations-at-exit list needs the tail of
the capture, and `TRACY_NO_EXIT` is deliberately off (`CMakeLists.txt:318`) because it hangs the
game at exit when no profiler is attached. So the leak list is unreliable in the normal trees. A
leak run needs a scratch build with `TRACY_NO_EXIT` on, and that build is useless for interactive
testing. Do not chase a leak list from a `build-tracy-mem` capture and believe it.

## Analysis

`tracy-csvexport` does not export memory. Its options are zones, messages and plots, so unlike
every phase so far there is no second reader to cross-check against, and the whole analysis path
is the Python bindings through the MCP server.

The bindings do carry memory properly:

- `get_memory_pools()` returns per-pool high water mark, low, current usage and event count.
- `get_memory_events(max_count, pool_name)` returns one dict per allocation with `ptr`, `size`,
  `time_alloc`, `time_free`, `thread_alloc` and **`callstack_idx`**.

That last field matters more than it looks. The pathfinder dig hit a wall because
`get_zone_callstacks` returns nothing for our zones, so subtree attribution had to be inferred
from template instantiation names. Memory events carry a real callstack index that
`get_callstack_frames` resolves. Memory tracking is where the bindings' callstack access finally
becomes usable, and that is a second argument for M3 over stopping at M2.

Note the default `max_count` of 100,000 and that each event materialises a Python dict.
Aggregate inside `eval` rather than pulling events across the wire, the same lesson as
`get_zone_durations` at 152M zones.

## Risks

**The operator family, as above.** The main one. Mitigated by M0 existing.

**Volume could make M2 non-viable on wentbourne.** Possible outcome, and not a failure. If the
global tier only works on Hilbergen and on short wentbourne runs, that is still more than exists
today, and the standing correction about naming the fixture applies to every number it produces.

**Determinism, which I believe is a non-issue.** Nothing here reads or writes game state, the
counters are process-local and never enter a savegame, and allocation addresses are already
non-deterministic and already excluded from game state by the rules in `docs/desync.md`. The
fingerprint gate stays in place anyway, because that is what it is for and because "I believe" is
not a gate.

**A global override changes allocator behaviour in `openttd_test`.** The test target links Tracy
and `add_definitions` reaches it, so it gets the override too. Catch2 allocates during test
registration, before `main`. Static initialisation order versus a replaced `operator new` is a
classic source of surprise, and this is the reason M0 runs the unit suite in both trees rather
than trusting that a doc-only change cannot break it.

## Open questions

1. Allocations per tick, both fixtures. M0 answers it and everything else waits on it.
2. Whether the pathfinder is really the largest allocation source or just the one under a zone we
   had already instrumented. M2 answers it, and I would not be surprised either way. The EnTT
   storages are the other candidate.
3. Whether per-zone memory statistics attribute usefully through the existing zone tree, given
   that `CallVehicleTicks` is 97.5% of `GameLoop` and swallows most of the interesting work into
   one zone.
