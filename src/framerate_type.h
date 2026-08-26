/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file framerate_type.h Types for recording game performance data.
 *
 * @par Adding new measurements
 * Adding a new measurement requires multiple steps, which are outlined here.
 * The first thing to do is add a new member of the #PerformanceElement enum.
 * It must be added before \c PerformanceElement::End and should be added in a logical place.
 * For example, an element of the game loop would be added next to the other game loop elements, and a rendering element next to the other rendering elements.
 *
 * @par
 * Second is adding a member to the \link anonymous_namespace{framerate_gui.cpp}::_pf_data _pf_data \endlink array, in the same position as the new #PerformanceElement member.
 *
 * @par
 * Third is adding strings for the new element. There is an array in #ConPrintFramerate with strings used for the console command.
 * Additionally, there are two sets of strings in \c english.txt for two GUI uses, also in the #PerformanceElement order.
 * Search for \c STR_FRAMERATE_GAMELOOP and \c STR_FRAMETIME_CAPTION_GAMELOOP in \c english.txt to find those.
 *
 * @par
 * Fourth, when the Tracy profiler bridge is compiled in, add an entry to the \c _pf_srcloc array in \c framerate_gui.cpp, again in the same position.
 * That array is sized from \c PerformanceElement::End, so omitting an entry appends a zeroed one instead of failing to compile; an assertion in #GetPerformanceSourceLocation catches it the first time the element is measured.
 * Nothing at all catches an entry in the wrong position, which would silently label every following zone with its neighbour's name, so check a capture rather than trusting the build.
 * If the new element is measured once per object rather than once per frame or tick, also add it to #IsPerformanceZoneActive so that its zones stay behind \c OTTD_TRACY_DETAIL.
 *
 * @par
 * Last is actually adding the measurements. There are two ways to measure, either one-shot (a single function/block handling all processing),
 * or as an accumulated element (multiple functions/blocks that need to be summed across each frame/tick).
 * Use either the PerformanceMeasurer or the PerformanceAccumulator class respectively for the two cases.
 * Either class is used by instantiating an object of it at the beginning of the block to be measured, so it auto-destructs at the end of the block.
 * For PerformanceAccumulator, make sure to also call PerformanceAccumulator::Reset once at the beginning of a new frame. Usually the StateGameLoop function is appropriate for this.
 *
 * @see framerate_gui.cpp for implementation
 */

#ifndef FRAMERATE_TYPE_H
#define FRAMERATE_TYPE_H

#include "core/enum_type.hpp"

/**
 * Elements of game performance that can be measured.
 *
 * @note When adding new elements here, make sure to also update all other locations depending on the length and order of this enum.
 * See <em>Adding new measurements</em> above.
 */
enum class PerformanceElement : uint8_t {
	GameLoop, ///< Speed of gameloop processing.
	GameLoopEconomy, ///< Time spent processing cargo movement
	GameLoopTrains, ///< Time spent processing trains
	GameLoopRoadVehicles, ///< Time spend processing road vehicles
	GameLoopShips, ///< Time spent processing ships
	GameLoopAircraft, ///< Time spent processing aircraft
	GameLoopLandscape, ///< Time spent processing other world features
	GameLoopLinkGraph, ///< Time spent waiting for link graph background jobs
	Drawing, ///< Speed of drawing world and GUI.
	ViewportDrawing, ///< Time spent drawing world viewports in GUI
	Video, ///< Speed of painting drawn video buffer.
	Sound, ///< Speed of mixing audio samples
	AllScripts, ///< Sum of all GS/AI scripts
	GameScript, ///< Game script execution
	AI0, ///< AI execution for player slot 1
	AI1, ///< AI execution for player slot 2
	AI2, ///< AI execution for player slot 3
	AI3, ///< AI execution for player slot 4
	AI4, ///< AI execution for player slot 5
	AI5, ///< AI execution for player slot 6
	AI6, ///< AI execution for player slot 7
	AI7, ///< AI execution for player slot 8
	AI8, ///< AI execution for player slot 9
	AI9, ///< AI execution for player slot 10
	AI10, ///< AI execution for player slot 11
	AI11, ///< AI execution for player slot 12
	AI12, ///< AI execution for player slot 13
	AI13, ///< AI execution for player slot 14
	AI14, ///< AI execution for player slot 15
	End, ///< End of enum, must be last.
};
DECLARE_ENUM_AS_SEQUENTIAL(PerformanceElement)

/** Type used to hold a performance timing measurement */
typedef uint64_t TimingMeasurement;

#ifdef WITH_TRACY

/**
 * Whether an element's profiler zone should be emitted.
 *
 * The four vehicle elements are measured inside \c Vehicle::Tick implementations, so they fire
 * once per vehicle rather than once per tick. Wentbourne emits 13,899 of them per tick, which
 * is roughly 70 million zones over a 5,000 tick run and far more trace than is usable. They
 * are therefore detail tier and stay compiled out unless \c OTTD_TRACY_DETAIL is defined.
 *
 * Every other element fires at most a few times per frame and is always on.
 *
 * Kept inline and constexpr because the argument is a literal at every construction site, so
 * the compiler folds the whole test away rather than branching in the vehicle tick path.
 *
 * @param elem The element to test.
 * @return True when a zone should be emitted for this element.
 */
constexpr bool IsPerformanceZoneActive(PerformanceElement elem)
{
	switch (elem) {
		case PerformanceElement::GameLoopTrains:
		case PerformanceElement::GameLoopRoadVehicles:
		case PerformanceElement::GameLoopShips:
		case PerformanceElement::GameLoopAircraft:
#ifdef OTTD_TRACY_DETAIL
			return true;
#else
			return false;
#endif /* OTTD_TRACY_DETAIL */

		default:
			return true;
	}
}

const tracy::SourceLocationData *GetPerformanceSourceLocation(PerformanceElement elem);

#endif /* WITH_TRACY */

/**
 * RAII class for measuring simple elements of performance.
 * Construct an object with the appropriate element parameter when processing begins,
 * time is automatically taken when the object goes out of scope again.
 *
 * Call Paused at the start of a frame if the processing of this element is paused.
 */
class PerformanceMeasurer {
	PerformanceElement elem;
	TimingMeasurement start_time;
#ifdef WITH_TRACY
	/* Emitted on whichever thread constructs this object, which is what Tracy needs. For
	 * PerformanceElement::Sound that is the mixer thread, so the zone lands where the work
	 * happens rather than where _pf_data is later drained. */
	tracy::ScopedZone zone;
#endif /* WITH_TRACY */
public:
	PerformanceMeasurer(PerformanceElement elem);
	~PerformanceMeasurer();

	/* Declared in both configurations so that a Tracy build cannot fail to compile something
	 * that builds without it. tracy::ScopedZone is neither copyable nor movable. */
	PerformanceMeasurer(const PerformanceMeasurer &) = delete;
	PerformanceMeasurer &operator=(const PerformanceMeasurer &) = delete;

	void SetExpectedRate(double rate);
	static void SetInactive(PerformanceElement elem);
	static void Paused(PerformanceElement elem);
};

/**
 * RAII class for measuring multi-step elements of performance.
 * At the beginning of a frame, call Reset on the element, then construct an object in the scope where
 * each processing cycle happens. The measurements are summed between resets.
 *
 * Usually StateGameLoop is an appropriate function to place Reset calls in, but for elements with
 * more isolated scopes it can also be appropriate to Reset somewhere else.
 * An example is the CallVehicleTicks function where all the vehicle type elements are reset.
 *
 * The PerformanceMeasurer::Paused function can also be used with elements otherwise measured with this class.
 */
class PerformanceAccumulator {
	PerformanceElement elem;
	TimingMeasurement start_time;
#ifdef WITH_TRACY
	/* One zone per measured block rather than one per frame. That is the point: the
	 * accumulator sums the blocks and hides their distribution, and the zones show it. */
	tracy::ScopedZone zone;
#endif /* WITH_TRACY */
public:
	PerformanceAccumulator(PerformanceElement elem);
	~PerformanceAccumulator();

	/* See the equivalent in PerformanceMeasurer. */
	PerformanceAccumulator(const PerformanceAccumulator &) = delete;
	PerformanceAccumulator &operator=(const PerformanceAccumulator &) = delete;

	static void Reset(PerformanceElement elem);
};

/** Whole-run totals for one performance element, unaffected by the rolling sample window. */
struct PerformanceTotal {
	TimingMeasurement total_us; ///< Total measured time, in microseconds.
	uint64_t count; ///< Number of measurements that make up #total_us.
};

PerformanceTotal GetPerformanceTotal(PerformanceElement elem);

void ShowFramerateWindow();
void ProcessPendingPerformanceMeasurements();

#endif /* FRAMERATE_TYPE_H */
