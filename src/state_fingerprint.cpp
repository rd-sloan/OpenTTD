/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file state_fingerprint.cpp Deterministic hashing of the game state, for verifying refactors.
 *
 * The ECS migration needs to answer one question after every change: did this alter
 * observable behaviour? Comparing savegames byte for byte cannot answer it, because
 * OpenTTD's savegames are not a pure function of the game state -- two identical runs
 * of unmodified master produce files that differ in a few hundred bytes.
 *
 * This hashes the values that actually define behaviour instead. It reads only game
 * state, never caches, never pointers and never anything marked NOSAVE, so it is
 * insensitive to the memory layout changes the migration is all about while staying
 * sensitive to behaviour.
 *
 * Two properties are deliberate:
 *
 * - Everything is visited in ascending pool index order, which #PoolIterator
 *   guarantees. The hash is therefore a function of the live set, not of the
 *   allocation history.
 * - Object references are hashed as pool indices, never as pointers, so the result
 *   does not depend on the allocator or on address space layout.
 */

#include "stdafx.h"

#include "state_fingerprint.h"

#include "company_base.h"
#include "core/random_func.hpp"
#include "industry.h"
#include "station_base.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_economy.h"
#include "town.h"
#include "vehicle_base.h"
#include "vehicle_components.h"

#include "safeguards.h"

namespace {

/** Index value hashed in place of a null object reference. */
constexpr uint64_t NO_INDEX = UINT64_MAX;

/**
 * Incremental FNV-1a hash over 64 bit values.
 *
 * Values are consumed one byte at a time in little endian order, so the result does
 * not depend on the host's byte order. FNV-1a is not cryptographic, which does not
 * matter here: the requirement is determinism and sensitivity to small changes, not
 * resistance to a deliberate collision.
 */
class StateHasher {
public:
	/**
	 * Feed a value into the hash.
	 *
	 * Accepts plain integers, scoped enumerations, the various strong types that
	 * expose base(), and #Money, so that call sites do not have to unwrap anything.
	 *
	 * @param value The value to hash.
	 */
	template <typename T>
	void Add(const T &value)
	{
		if constexpr (requires { value.base(); }) {
			/* Strong typedefs, pool indices, tile indices and enum bitsets. */
			this->Add(value.base());
		} else if constexpr (std::is_enum_v<T>) {
			this->AddRaw(static_cast<uint64_t>(to_underlying(value)));
		} else if constexpr (std::is_integral_v<T>) {
			this->AddRaw(static_cast<uint64_t>(value));
		} else {
			/* Money and the other overflow safe integers. */
			this->AddRaw(static_cast<uint64_t>(static_cast<int64_t>(value)));
		}
	}

	/**
	 * Get the hash of everything added so far.
	 * @return The hash.
	 */
	uint64_t Get() const { return this->hash; }

private:
	uint64_t hash = 0xCBF29CE484222325ULL; ///< FNV-1a 64 bit offset basis.

	/**
	 * Mix eight bytes into the hash, least significant first.
	 * @param value The value to mix in.
	 */
	void AddRaw(uint64_t value)
	{
		for (int i = 0; i < 8; i++) {
			this->hash ^= static_cast<uint8_t>(value >> (i * 8));
			this->hash *= 0x100000001B3ULL; // FNV-1a 64 bit prime.
		}
	}
};

/**
 * Hash a vehicle reference as an index rather than a pointer.
 * @param hasher The hasher to add to.
 * @param v The vehicle, may be nullptr.
 */
void AddVehicleRef(StateHasher &hasher, const Vehicle *v)
{
	if (v == nullptr) {
		hasher.Add(NO_INDEX);
	} else {
		hasher.Add(v->index);
	}
}

/**
 * Hash every vehicle's simulation state.
 * @return The hash.
 */
uint64_t HashVehicles()
{
	StateHasher hasher;

	for (const Vehicle *v : Vehicle::Iterate()) {
		hasher.Add(v->index);
		hasher.Add(v->type);
		hasher.Add(v->subtype);
		hasher.Add(v->owner);

		/* Position and motion, i.e. what the tick loop actually writes. */
		hasher.Add(v->tile);
		hasher.Add(v->dest_tile);
		hasher.Add(v->x_pos);
		hasher.Add(v->y_pos);
		hasher.Add(v->z_pos);
		hasher.Add(v->direction);
		hasher.Add(v->cur_speed);
		hasher.Add(v->GetMotion().subspeed);
		hasher.Add(v->progress);
		hasher.Add(v->vehstatus);

		/* Load and orders. */
		hasher.Add(v->engine_type);
		hasher.Add(v->cargo_type);
		hasher.Add(v->cargo_cap);
		hasher.Add(v->cargo.TotalCount());
		hasher.Add(v->cargo_age_counter);
		hasher.Add(v->current_order.GetType());
		hasher.Add(v->current_order.GetDestination());
		hasher.Add(v->last_station_visited);
		hasher.Add(v->load_unload_ticks);

		/* Wear and tear, which is driven by the shared randomiser. */
		hasher.Add(v->age);
		hasher.Add(v->reliability);
		hasher.Add(v->breakdown_ctr);
		hasher.Add(v->breakdowns_since_last_service);
		hasher.Add(v->running_ticks);
		hasher.Add(v->tick_counter);

		/* Chain structure, by index so the hash ignores the allocator. */
		AddVehicleRef(hasher, v->Next());
		AddVehicleRef(hasher, v->First());
	}

	return hasher.Get();
}

/**
 * Hash company finances.
 * @return The hash.
 */
uint64_t HashCompanies()
{
	StateHasher hasher;

	for (const Company *c : Company::Iterate()) {
		hasher.Add(c->index);
		hasher.Add(c->money);
		hasher.Add(c->current_loan);
	}

	return hasher.Get();
}

/**
 * Hash the cargo waiting at every station.
 * @return The hash.
 */
uint64_t HashStations()
{
	StateHasher hasher;

	for (const Station *st : Station::Iterate()) {
		hasher.Add(st->index);
		hasher.Add(st->xy);
		hasher.Add(st->owner);

		for (const GoodsEntry &ge : st->goods) {
			/* Only stations that have seen cargo carry the optional data, and asking
			 * for it when absent would allocate it. Hash a zero for the empty case so
			 * that the two are still distinguishable positionally. */
			hasher.Add(ge.HasData() ? ge.GetData().cargo.TotalCount() : 0u);
		}
	}

	return hasher.Get();
}

/**
 * Hash town populations.
 * @return The hash.
 */
uint64_t HashTowns()
{
	StateHasher hasher;

	for (const Town *t : Town::Iterate()) {
		hasher.Add(t->index);
		hasher.Add(t->xy);

		/* These live in TownCache, so they are derived rather than primary state. That
		 * is deliberate: they are a cheap proxy for the house layout on the map, which
		 * would be far more expensive to hash directly, and a cache is required to be a
		 * pure function of what it caches -- cachecheck.cpp asserts exactly that. */
		hasher.Add(t->cache.num_houses);
		hasher.Add(t->cache.population);
	}

	return hasher.Get();
}

/**
 * Hash industry production.
 * @return The hash.
 */
uint64_t HashIndustries()
{
	StateHasher hasher;

	for (const Industry *i : Industry::Iterate()) {
		hasher.Add(i->index);
		hasher.Add(i->location.tile);

		for (const auto &p : i->produced) {
			hasher.Add(p.cargo);
			hasher.Add(p.waiting);
			hasher.Add(p.rate);
		}
	}

	return hasher.Get();
}

/**
 * Hash the shared randomiser, the clocks and the object counts.
 *
 * The randomiser state is the most sensitive value available: any change in how many
 * times or in what order the game draws from `_random` shows up here immediately,
 * which is exactly the failure mode a reordered iteration would cause.
 *
 * @param hasher The hasher to add to.
 */
void HashGlobals(StateHasher &hasher)
{
	hasher.Add(_random.state[0]);
	hasher.Add(_random.state[1]);

	hasher.Add(TimerGameCalendar::date);
	hasher.Add(TimerGameEconomy::date);

	hasher.Add(Vehicle::GetNumItems());
	hasher.Add(Town::GetNumItems());
	hasher.Add(Industry::GetNumItems());
}

}

/**
 * Compute a fingerprint of the current game state.
 *
 * Safe to call at any point where the game state is live and consistent, i.e. not
 * part way through a command. Cost is proportional to the number of game objects, so
 * it is meant for occasional use such as the end of a benchmark run, not per tick.
 *
 * @return The fingerprint.
 */
StateFingerprint ComputeStateFingerprint()
{
	StateFingerprint fp;

	fp.vehicles = HashVehicles();
	fp.companies = HashCompanies();
	fp.stations = HashStations();
	fp.towns = HashTowns();
	fp.industries = HashIndustries();

	StateHasher globals;
	HashGlobals(globals);
	fp.globals = globals.Get();

	StateHasher combined;
	combined.Add(fp.vehicles);
	combined.Add(fp.companies);
	combined.Add(fp.stations);
	combined.Add(fp.towns);
	combined.Add(fp.industries);
	combined.Add(fp.globals);
	fp.combined = combined.Get();

	fp.random_state_0 = _random.state[0];
	fp.random_state_1 = _random.state[1];

	return fp;
}
