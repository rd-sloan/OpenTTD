/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file profiling.h Tracy profiler instrumentation macros.
 *
 * All Tracy usage goes through the OTTD_* macros below. No other file includes
 * <tracy/Tracy.hpp> directly, for three reasons:
 *
 * - safeguards.h defines memcpy, memset, malloc and printf into compile errors, and
 *   Tracy's headers use them. Routing every include through one header keeps that
 *   ordering hazard in one place rather than in every call site.
 * - The build must work with WITH_TRACY undefined, where each macro becomes a no-op
 *   and no call site needs an \#ifdef of its own.
 * - Zone volume needs tiers that Tracy does not provide; see #OTTD_ZONE_DETAIL.
 *
 * This header is included from stdafx.h, so the macros are available everywhere without
 * per-file includes and the Tracy headers land in the precompiled header.
 *
 * WITH_TRACY is defined by OPTION_TRACY; see cmake/Options.cmake.
 *
 * @see docs/tracy-integration-plan.md for the instrumentation plan.
 * @see docs/tracy-integration-log.md for what has actually been done.
 */

#ifndef PROFILING_H
#define PROFILING_H

#ifdef WITH_TRACY
#	include <tracy/Tracy.hpp>
#endif /* WITH_TRACY */

#ifdef WITH_TRACY

/** Scoped zone named after the enclosing function. */
#	define OTTD_ZONE ZoneScoped
/** Scoped zone with an explicit literal name. */
#	define OTTD_ZONE_N(name) ZoneScopedN(name)
/** Scoped zone with an explicit literal name and a colour. */
#	define OTTD_ZONE_C(name, colour) ZoneScopedNC(name, colour)
/**
 * As #OTTD_ZONE_N, but with an explicit variable name.
 *
 * The plain forms all declare a variable called \c ___tracy_scoped_zone, so two of them in
 * the same scope is a redefinition error. Use this for the second and later zones in a scope.
 */
#	define OTTD_ZONE_NAMED_N(variable, name) ZoneNamedN(variable, name, true)
/** Attach runtime text to the zone in scope. */
#	define OTTD_ZONE_TEXT(text, size) ZoneText(text, size)

/** End of the primary frame, which is the drawing frame. */
#	define OTTD_FRAME_MARK FrameMark
/** End of a named secondary frame, such as the simulation tick. */
#	define OTTD_FRAME_MARK_N(name) FrameMarkNamed(name)

/** Record a numeric value on the timeline. */
#	define OTTD_PLOT(name, value) TracyPlot(name, value)
/** Post a literal message to the trace. */
#	define OTTD_MESSAGE(text) TracyMessageL(text)
/** Name the calling thread. */
#	define OTTD_THREAD_NAME(name) tracy::SetThreadName(name)

/** Begin a program phase. Returns an id that must be passed to #OTTD_SECTION_LEAVE. */
#	define OTTD_SECTION_ENTER(name) TracySectionEnter(name)
/** End the program phase identified by \a id. */
#	define OTTD_SECTION_LEAVE(id) TracySectionLeave(id)

/**
 * RAII marker for a coarse program phase, such as startup or loading a savegame.
 *
 * Tracy's own section API is id-based rather than scope-based, so this wrapper exists to
 * leave the phase on every exit path. #openttd_main alone has five early returns between
 * the start of the startup phase and reaching the main loop.
 */
class ScopedProfilerSection {
public:
	/* The name is passed as an argument rather than as the format string, so that a name
	 * containing a percent sign cannot be read as a conversion specifier. */
	explicit ScopedProfilerSection(const char *name) : id(TracySectionEnter("%s", name)) {}
	~ScopedProfilerSection() { TracySectionLeave(this->id); }

	ScopedProfilerSection(const ScopedProfilerSection &) = delete;
	ScopedProfilerSection &operator=(const ScopedProfilerSection &) = delete;

private:
	uint32_t id; ///< Section id, or 0 when no profiler is attached.
};

/** Mark the enclosing scope as a program phase. Preferred over the enter/leave macros. */
#	define OTTD_SECTION(variable, name) ScopedProfilerSection variable(name)

/** Declare a mutex whose contention is recorded. Replaces a plain declaration. */
#	define OTTD_LOCKABLE(type, name) TracyLockable(type, name)
/** As #OTTD_LOCKABLE, with an explicit description instead of the variable name. */
#	define OTTD_LOCKABLE_N(type, name, description) TracyLockableN(type, name, description)
/** The type of an #OTTD_LOCKABLE variable, for use in signatures and lock guards. */
#	define OTTD_LOCKABLE_BASE(type) LockableBase(type)

#else /* WITH_TRACY */

#	define OTTD_ZONE
#	define OTTD_ZONE_N(name)
#	define OTTD_ZONE_C(name, colour)
#	define OTTD_ZONE_NAMED_N(variable, name)
#	define OTTD_ZONE_TEXT(text, size)

#	define OTTD_FRAME_MARK
#	define OTTD_FRAME_MARK_N(name)

#	define OTTD_PLOT(name, value)
#	define OTTD_MESSAGE(text)
#	define OTTD_THREAD_NAME(name)

/* Mirrors Tracy's own disabled forms: enter yields an id of 0, leave consumes it so
 * that the caller's variable does not become unused. */
#	define OTTD_SECTION_ENTER(name) 0
#	define OTTD_SECTION_LEAVE(id) ((void)(id))
#	define OTTD_SECTION(variable, name)

#	define OTTD_LOCKABLE(type, name) type name
#	define OTTD_LOCKABLE_N(type, name, description) type name
#	define OTTD_LOCKABLE_BASE(type) type

#endif /* WITH_TRACY */

/**
 * Detail zones. These fire per vehicle, per tile or per pathfinder node, so they are
 * compiled out even in a Tracy build unless OTTD_TRACY_DETAIL is defined.
 *
 * Wentbourne holds 85,259 vehicle parts, so one zone per part over a 5,000 tick run is
 * roughly 426 million zones and about 5 GB of trace. Detail captures are for a few
 * hundred ticks once the subsystem is already known.
 */
#if defined(WITH_TRACY) && defined(OTTD_TRACY_DETAIL)
#	define OTTD_ZONE_DETAIL OTTD_ZONE
#	define OTTD_ZONE_DETAIL_N(name) OTTD_ZONE_N(name)
#	define OTTD_ZONE_DETAIL_C(name, colour) OTTD_ZONE_C(name, colour)
#else
#	define OTTD_ZONE_DETAIL
#	define OTTD_ZONE_DETAIL_N(name)
#	define OTTD_ZONE_DETAIL_C(name, colour)
#endif /* WITH_TRACY && OTTD_TRACY_DETAIL */

/**
 * Memory tracking, gated on OTTD_TRACY_MEM because it replaces global operator new and
 * delete and so touches every allocation in the process.
 *
 * Phase M0 counts allocations and plots them; it reports nothing to Tracy's memory
 * subsystem. The counters stay in later phases as the cross-check against the event count
 * Tracy receives. @see docs/tracy-memory-plan.md
 */
#if defined(WITH_TRACY) && defined(OTTD_TRACY_MEM)

/**
 * RAII marker for one game loop iteration, which plots the allocations made inside it.
 *
 * Scoped rather than a pair of calls because #StateGameLoop has an early return for the
 * paused case, and a tick that did not run should not produce a data point.
 *
 * The counters behind this are process-wide, so on a threaded interactive run the delta
 * includes whatever the draw thread managed between the two samples. It is exact only for
 * the null driver, which has no game thread. @see docs/tracy-memory-log.md
 */
class ScopedProfilerMemoryTick {
public:
	ScopedProfilerMemoryTick();
	~ScopedProfilerMemoryTick();

	ScopedProfilerMemoryTick(const ScopedProfilerMemoryTick &) = delete;
	ScopedProfilerMemoryTick &operator=(const ScopedProfilerMemoryTick &) = delete;

private:
	uint64_t allocs; ///< Allocation count when the scope was entered.
	uint64_t bytes; ///< Allocated byte total when the scope was entered.
};

/** Plot the allocations made in the enclosing scope, which is one game loop iteration. */
#	define OTTD_MEM_TICK(variable) ScopedProfilerMemoryTick variable

/**
 * Report an allocation in a named memory pool.
 *
 * \a name must be a pointer to a string literal, and Tracy identifies the pool by that
 * *pointer* rather than by the characters it points at. So a name has to come from one object:
 * two identical literals in different translation units, or in one that is compiled without
 * string pooling, are two pools that look the same in the UI.
 *
 * Every reported free must have a matching reported allocation or Tracy terminates the capture
 * outright, so a marker only belongs where the pairing is guaranteed by construction.
 * TRACY_ON_DEMAND, forced on in CMakeLists.txt, forgives the case where the allocation happened
 * before the profiler connected, and nothing else.
 */
#	define OTTD_MEM_ALLOC_N(ptr, size, name) TracyAllocN(ptr, size, name)
/** Report a deallocation in a named memory pool. @see OTTD_MEM_ALLOC_N */
#	define OTTD_MEM_FREE_N(ptr, name) TracyFreeN(ptr, name)

#else

#	define OTTD_MEM_TICK(variable)
#	define OTTD_MEM_ALLOC_N(ptr, size, name)
#	define OTTD_MEM_FREE_N(ptr, name)

#endif /* WITH_TRACY && OTTD_TRACY_MEM */

#endif /* PROFILING_H */
