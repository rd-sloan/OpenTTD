/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file benchmark_stats.cpp Recording of benchmark timings and workload statistics.
 *
 * This writes a tab separated key/value report describing one benchmark run: how long
 * the run took, how much time each instrumented subsystem accounted for, how large the
 * simulated world was, and how large the relevant game objects are in memory.
 *
 * The workload figures matter as much as the timings. A timing is only comparable
 * against another run that did the same amount of work, so recording the vehicle and
 * station counts alongside is what makes two numbers safe to compare.
 *
 * Output goes to a file rather than to stdout because on Windows OpenTTD is a GUI
 * subsystem binary: #CreateConsole allocates a fresh console and reopens the standard
 * streams onto it, so anything printed to stdout never reaches a redirecting shell.
 */

#include "stdafx.h"

#include "benchmark_stats.h"

#include "aircraft.h"
#include "base_consist.h"
#include "company_base.h"
#include "core/enum_type.hpp"
#include "core/format.hpp"
#include "debug.h"
#include "fileio_func.h"
#include "framerate_type.h"
#include "industry.h"
#include "map_func.h"
#include "rev.h"
#include "roadveh.h"
#include "ship.h"
#include "state_fingerprint.h"
#include "station_base.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "town.h"
#include "train.h"
#include "vehicle_base.h"

#include "safeguards.h"

/** Counts of the game objects that drive the per-tick workload. */
struct WorkloadCounts {
	uint64_t vehicle_parts = 0; ///< Every vehicle in the pool, which is what the tick loop iterates.
	uint64_t vehicle_primary = 0; ///< Front vehicles only, i.e. what the player would call a vehicle.
	EnumIndexArray<uint64_t, VehicleType, VehicleType::End> parts_by_type{}; ///< #vehicle_parts split by type.
	EnumIndexArray<uint64_t, VehicleType, VehicleType::End> primary_by_type{}; ///< #vehicle_primary split by type.
	uint64_t stations = 0; ///< Number of stations.
	uint64_t towns = 0; ///< Number of towns.
	uint64_t industries = 0; ///< Number of industries.
	uint64_t companies = 0; ///< Number of companies.
};

/**
 * Tally the live game objects.
 * @return The counts.
 */
static WorkloadCounts CountWorkload()
{
	WorkloadCounts wl;

	for (const Vehicle *v : Vehicle::Iterate()) {
		wl.vehicle_parts++;
		if (v->type < VehicleType::End) wl.parts_by_type[v->type]++;
		if (v->IsPrimaryVehicle()) {
			wl.vehicle_primary++;
			if (v->type < VehicleType::End) wl.primary_by_type[v->type]++;
		}
	}

	for ([[maybe_unused]] const Station *st : Station::Iterate()) wl.stations++;
	for ([[maybe_unused]] const Town *t : Town::Iterate()) wl.towns++;
	for ([[maybe_unused]] const Industry *i : Industry::Iterate()) wl.industries++;
	for ([[maybe_unused]] const Company *c : Company::Iterate()) wl.companies++;

	return wl;
}

/**
 * Write one performance element as a row of timing figures.
 *
 * Four derived figures are reported alongside the total, each answering a different
 * question:
 * - \c mean_ms is the cost of one measurement, which for the vehicle elements is one
 *   accumulation cycle and therefore one tick.
 * - \c per_tick_us is the cost per simulated tick, which is the figure to compare
 *   across elements that are measured at different rates.
 * - \c pct_of_game_loop makes reports comparable between savegames of different sizes.
 * - \c ns_per_object_tick divides the workload out, which is what isolates a genuine
 *   improvement from a savegame that simply has more vehicles in it.
 * - \c ns_per_consist_tick does the same against front vehicles rather than parts.
 *
 * Both vehicle denominators are reported because neither alone is honest. Per part
 * flatters trains, since one consist of eighteen wagons amortises its pathfinding and
 * order processing across all of them, while a road vehicle is usually a single part
 * doing that work alone. Per consist flatters road vehicles for the mirror reason.
 * Compare like with like, and prefer per part when judging a change to data layout,
 * since that is what scales with the number of objects walked.
 *
 * @param f The file to write to.
 * @param key Key prefix identifying the element.
 * @param elem The element to report.
 * @param ticks Number of ticks the run covered.
 * @param game_loop_us Total game loop time, for computing a share. Zero to omit.
 * @param objects Number of objects this element processes, for normalising. Zero to omit.
 * @param consists Number of front vehicles, for a second normalisation. Zero to omit.
 */
static void WriteElement(FILE *f, std::string_view key, PerformanceElement elem, uint ticks, uint64_t game_loop_us, uint64_t objects, uint64_t consists = 0)
{
	const PerformanceTotal pt = GetPerformanceTotal(elem);

	/* Durations are accumulated in microseconds. */
	const double total_ms = static_cast<double>(pt.total_us) / 1000.0;
	const double mean_ms = pt.count == 0 ? 0.0 : total_ms / static_cast<double>(pt.count);
	const double per_tick_us = ticks == 0 ? 0.0 : static_cast<double>(pt.total_us) / static_cast<double>(ticks);

	fmt::print(f, "perf.{}.total_ms\t{:.3f}\n", key, total_ms);
	fmt::print(f, "perf.{}.count\t{}\n", key, pt.count);
	fmt::print(f, "perf.{}.mean_ms\t{:.6f}\n", key, mean_ms);
	fmt::print(f, "perf.{}.per_tick_us\t{:.3f}\n", key, per_tick_us);

	if (game_loop_us != 0) {
		fmt::print(f, "perf.{}.pct_of_game_loop\t{:.2f}\n", key,
				100.0 * static_cast<double>(pt.total_us) / static_cast<double>(game_loop_us));
	}

	if (objects != 0 && ticks != 0) {
		fmt::print(f, "perf.{}.ns_per_object_tick\t{:.1f}\n", key,
				1000.0 * static_cast<double>(pt.total_us) / (static_cast<double>(ticks) * static_cast<double>(objects)));
		fmt::print(f, "perf.{}.objects\t{}\n", key, objects);
	}

	if (consists != 0 && ticks != 0) {
		fmt::print(f, "perf.{}.ns_per_consist_tick\t{:.1f}\n", key,
				1000.0 * static_cast<double>(pt.total_us) / (static_cast<double>(ticks) * static_cast<double>(consists)));
		fmt::print(f, "perf.{}.consists\t{}\n", key, consists);
	}
}

/**
 * Write a benchmark report describing the run that just finished.
 *
 * Intended to be called once, after a fixed number of ticks has been simulated.
 * Failure to open the file is reported through the debug log and is otherwise ignored,
 * because losing a benchmark report is not a reason to fail a run.
 *
 * @param filename Path of the file to write. Overwritten if it exists.
 * @param ticks Number of ticks that were simulated.
 * @param wallclock_us Wall clock duration of the simulated ticks, in microseconds.
 */
void WriteBenchmarkStats(std::string_view filename, uint ticks, uint64_t wallclock_us)
{
	auto handle = FileHandle::Open(filename, "w");
	if (!handle.has_value()) {
		Debug(misc, 0, "Could not open benchmark stats file '{}'", filename);
		return;
	}
	FILE *f = *handle;

	const WorkloadCounts wl = CountWorkload();

	fmt::print(f, "# OpenTTD benchmark report. Tab separated key/value pairs.\n");

	fmt::print(f, "run.revision\t{}\n", _openttd_revision);
	fmt::print(f, "run.ticks\t{}\n", ticks);
	fmt::print(f, "run.wallclock_ms\t{:.3f}\n", static_cast<double>(wallclock_us) / 1000.0);
	fmt::print(f, "run.ticks_per_second\t{:.2f}\n",
			wallclock_us == 0 ? 0.0 : static_cast<double>(ticks) * 1000000.0 / static_cast<double>(wallclock_us));

	/* Worth recording, because a build with asserts on is not a build worth timing. */
#ifdef WITH_ASSERT
	fmt::print(f, "run.asserts_enabled\t1\n");
#else
	fmt::print(f, "run.asserts_enabled\t0\n");
#endif

	fmt::print(f, "world.map_size_x\t{}\n", Map::SizeX());
	fmt::print(f, "world.map_size_y\t{}\n", Map::SizeY());
	fmt::print(f, "world.calendar_year\t{}\n", TimerGameCalendar::year.base());
	fmt::print(f, "world.calendar_date\t{}\n", TimerGameCalendar::date.base());
	fmt::print(f, "world.economy_date\t{}\n", TimerGameEconomy::date.base());

	fmt::print(f, "load.vehicle_parts\t{}\n", wl.vehicle_parts);
	fmt::print(f, "load.vehicle_primary\t{}\n", wl.vehicle_primary);
	fmt::print(f, "load.consists_trains\t{}\n", wl.primary_by_type[VehicleType::Train]);
	fmt::print(f, "load.consists_road\t{}\n", wl.primary_by_type[VehicleType::Road]);
	fmt::print(f, "load.consists_ships\t{}\n", wl.primary_by_type[VehicleType::Ship]);
	fmt::print(f, "load.consists_aircraft\t{}\n", wl.primary_by_type[VehicleType::Aircraft]);
	fmt::print(f, "load.vehicle_trains\t{}\n", wl.parts_by_type[VehicleType::Train]);
	fmt::print(f, "load.vehicle_road\t{}\n", wl.parts_by_type[VehicleType::Road]);
	fmt::print(f, "load.vehicle_ships\t{}\n", wl.parts_by_type[VehicleType::Ship]);
	fmt::print(f, "load.vehicle_aircraft\t{}\n", wl.parts_by_type[VehicleType::Aircraft]);
	fmt::print(f, "load.vehicle_effect\t{}\n", wl.parts_by_type[VehicleType::Effect]);
	fmt::print(f, "load.vehicle_disaster\t{}\n", wl.parts_by_type[VehicleType::Disaster]);
	fmt::print(f, "load.stations\t{}\n", wl.stations);
	fmt::print(f, "load.towns\t{}\n", wl.towns);
	fmt::print(f, "load.industries\t{}\n", wl.industries);
	fmt::print(f, "load.companies\t{}\n", wl.companies);

	/* Object sizes are the headline figure for the ECS migration: the point of moving
	 * hot fields into components is that these numbers come down. */
	fmt::print(f, "sizeof.Vehicle\t{}\n", sizeof(Vehicle));
	fmt::print(f, "sizeof.Train\t{}\n", sizeof(Train));
	fmt::print(f, "sizeof.RoadVehicle\t{}\n", sizeof(RoadVehicle));
	fmt::print(f, "sizeof.Ship\t{}\n", sizeof(Ship));
	fmt::print(f, "sizeof.Aircraft\t{}\n", sizeof(Aircraft));

	/* Vehicle inherits BaseConsist, so every part carries it, but the data is only
	 * meaningful on front vehicles. Multiply by (vehicle_parts - vehicle_primary) for
	 * the dead weight, which is what phase 7 is aiming to remove. */
	fmt::print(f, "sizeof.BaseConsist\t{}\n", sizeof(BaseConsist));
	fmt::print(f, "sizeof.Station\t{}\n", sizeof(Station));
	fmt::print(f, "sizeof.Town\t{}\n", sizeof(Town));

	/* Shares are expressed against the game loop, which encloses all the others. */
	const uint64_t game_loop_us = GetPerformanceTotal(PerformanceElement::GameLoop).total_us;

	WriteElement(f, "game_loop", PerformanceElement::GameLoop, ticks, 0, wl.vehicle_parts, wl.vehicle_primary);
	WriteElement(f, "economy", PerformanceElement::GameLoopEconomy, ticks, game_loop_us, wl.stations);
	WriteElement(f, "trains", PerformanceElement::GameLoopTrains, ticks, game_loop_us,
			wl.parts_by_type[VehicleType::Train], wl.primary_by_type[VehicleType::Train]);
	WriteElement(f, "road_vehicles", PerformanceElement::GameLoopRoadVehicles, ticks, game_loop_us,
			wl.parts_by_type[VehicleType::Road], wl.primary_by_type[VehicleType::Road]);
	WriteElement(f, "ships", PerformanceElement::GameLoopShips, ticks, game_loop_us,
			wl.parts_by_type[VehicleType::Ship], wl.primary_by_type[VehicleType::Ship]);
	WriteElement(f, "aircraft", PerformanceElement::GameLoopAircraft, ticks, game_loop_us,
			wl.parts_by_type[VehicleType::Aircraft], wl.primary_by_type[VehicleType::Aircraft]);
	WriteElement(f, "landscape", PerformanceElement::GameLoopLandscape, ticks, game_loop_us, 0);
	WriteElement(f, "link_graph", PerformanceElement::GameLoopLinkGraph, ticks, game_loop_us, 0);
	WriteElement(f, "drawing", PerformanceElement::Drawing, ticks, 0, 0);
	WriteElement(f, "viewport_drawing", PerformanceElement::ViewportDrawing, ticks, 0, 0);

	/* All four vehicle elements together, which is the figure the migration is aiming at. */
	const uint64_t vehicles_us =
			GetPerformanceTotal(PerformanceElement::GameLoopTrains).total_us +
			GetPerformanceTotal(PerformanceElement::GameLoopRoadVehicles).total_us +
			GetPerformanceTotal(PerformanceElement::GameLoopShips).total_us +
			GetPerformanceTotal(PerformanceElement::GameLoopAircraft).total_us;
	fmt::print(f, "perf.all_vehicles.total_ms\t{:.3f}\n", static_cast<double>(vehicles_us) / 1000.0);
	if (game_loop_us != 0) {
		fmt::print(f, "perf.all_vehicles.pct_of_game_loop\t{:.2f}\n",
				100.0 * static_cast<double>(vehicles_us) / static_cast<double>(game_loop_us));
	}
	if (ticks != 0 && wl.vehicle_parts != 0) {
		fmt::print(f, "perf.all_vehicles.ns_per_object_tick\t{:.1f}\n",
				1000.0 * static_cast<double>(vehicles_us) / (static_cast<double>(ticks) * static_cast<double>(wl.vehicle_parts)));
	}

	/* A fingerprint of the game state, which is how a phase proves it changed nothing.
	 * See state_fingerprint.cpp for why this exists rather than comparing savegames. */
	const StateFingerprint fp = ComputeStateFingerprint();
	fmt::print(f, "state.random_state_0\t{}\n", fp.random_state_0);
	fmt::print(f, "state.random_state_1\t{}\n", fp.random_state_1);
	fmt::print(f, "state.hash.vehicles\t{:016X}\n", fp.vehicles);
	fmt::print(f, "state.hash.companies\t{:016X}\n", fp.companies);
	fmt::print(f, "state.hash.stations\t{:016X}\n", fp.stations);
	fmt::print(f, "state.hash.towns\t{:016X}\n", fp.towns);
	fmt::print(f, "state.hash.industries\t{:016X}\n", fp.industries);
	fmt::print(f, "state.hash.globals\t{:016X}\n", fp.globals);
	fmt::print(f, "state.hash.combined\t{:016X}\n", fp.combined);

	Debug(misc, 0, "Benchmark stats written to '{}'", filename);
}
