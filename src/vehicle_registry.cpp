/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file vehicle_registry.cpp The EnTT registry holding vehicle entities, and its identity mapping.
 *
 * Phase 1 of the ECS migration (see docs/ecs-migration-plan.md). This stands the
 * registry up alongside #_vehicle_pool and keeps the two in lockstep, but moves no
 * data: every entity carries only a #VehicleRef. The point of the phase is to get the
 * identity mapping and the ordering discipline right while nothing depends on them yet.
 *
 * Two invariants matter, and both are checked rather than assumed.
 *
 * **One entity per pooled vehicle.** Entities are created and destroyed from Vehicle's
 * constructor and destructor, which between them cover every path: normal construction,
 * savegame load via Pool::CreateAtIndex, and wholesale pool cleaning on new game or
 * load, where CleanPool deletes every item individually.
 *
 * **Iteration in ascending VehicleID order.** This is the load-bearing one. OpenTTD's
 * PoolIterator visits live entries in ascending index order, which is a pure function of
 * the live set. EnTT's packed order is not: views walk the packed array backwards, and
 * swap-and-pop on removal makes the layout a function of the entire create/destroy
 * history. A savegame records the live set but not that history, so a reloaded game
 * would iterate differently from the run that saved it -- and with `Random()` being a
 * single shared LCG, a different visit order is a different game.
 *
 * The fix is to sort the storage by #VehicleRef whenever the structure changed. Sorting
 * arranges storage so that *iteration* matches the comparator, so a plain `<` yields
 * ascending order despite the reversed walk, reproducing PoolIterator exactly. Structural
 * churn is rare next to the tick rate, so the cost amortises away, and once sorted the
 * storage is contiguous in id order, which is the locality the migration is after.
 *
 * A consequence worth recording: owning groups cannot be used for game state components,
 * because EnTT forbids sorting a pool once a group owns it. Views, or non-owning groups.
 */

#include "stdafx.h"

#include "vehicle_registry.h"

#include "vehicle_base.h"
#include "vehicle_components.h"

#include "safeguards.h"

/** @copydoc _vehicle_registry_data */
VehicleRegistryData *_vehicle_registry_data = nullptr;

/**
 * Create the registry data on first use.
 *
 * Deliberately never freed, following the same reasoning as PoolBase::GetPools(). Static
 * destruction order across translation units is unspecified, so a plain file-scope
 * object could in principle be destroyed while a Vehicle destructor elsewhere is still
 * running during teardown. Leaking one allocation that lives until the process exits
 * removes that hazard entirely and is cheaper than trying to reason about the ordering.
 *
 * Constructed lazily for the same reason, rather than by a dynamic initialiser on
 * #_vehicle_registry_data: an initialiser would run at an unspecified point relative to
 * other translation units' static initialisation, which is the hazard this avoids.
 */
void InitVehicleRegistryData()
{
	if (_vehicle_registry_data == nullptr) _vehicle_registry_data = new VehicleRegistryData();
}

/** Shorthand for the registry data, matching the name the rest of this file used. */
static inline VehicleRegistryData &Data()
{
	return GetVehicleRegistryData();
}

/**
 * Get the number of vehicle entities.
 * @return The count, which should always equal Vehicle::GetNumItems().
 */
size_t GetVehicleEntityCount()
{
	return Data().registry.view<VehicleRef>().size();
}

/**
 * Create the entity for a newly constructed vehicle.
 * @param id The vehicle that was constructed.
 * @return The entity, which the vehicle stores so that component access needs no lookup.
 */
entt::entity RegisterVehicleEntity(VehicleID id)
{
	VehicleRegistryData &data = Data();

	const size_t index = id.base();
	if (index >= data.entity_by_vehicle_id.size()) {
		data.entity_by_vehicle_id.resize(index + 1, entt::null);
	}

	assert(data.entity_by_vehicle_id[index] == entt::null);

	const entt::entity entity = data.registry.create();
	data.registry.emplace<VehicleRef>(entity, id);

	/* Components every vehicle has. Attached eagerly because they are universal; a
	 * component that only some vehicles need would be better emplaced on demand. */
	data.registry.emplace<VehicleColourMap>(entity);
	data.registry.emplace<VehicleCacheComponent>(entity);
	data.registry.emplace<VehicleMotion>(entity);
	data.registry.emplace<VehiclePosition>(entity);

	data.entity_by_vehicle_id[index] = entity;

	data.dirty = true;

	return entity;
}

/**
 * Destroy the entity for a vehicle that is going away.
 *
 * Tolerates being called for a vehicle that has no entity. That is not expected, but
 * this runs from a destructor on paths that are awkward to enumerate exhaustively
 * (bankruptcy, crash cleanup, pool cleaning), and a stray call is caught by the count
 * check in #SortVehicleRegistry rather than by crashing here.
 *
 * @param id The vehicle being destroyed.
 */
void UnregisterVehicleEntity(VehicleID id)
{
	VehicleRegistryData &data = Data();

	const size_t index = id.base();
	if (index >= data.entity_by_vehicle_id.size()) return;

	const entt::entity entity = data.entity_by_vehicle_id[index];
	if (entity == entt::null) return;

	data.registry.destroy(entity);
	data.entity_by_vehicle_id[index] = entt::null;

	data.dirty = true;
}

/**
 * Restore canonical iteration order, if the structure changed since last time.
 *
 * Call this before any iteration whose result can affect the game state. It is cheap
 * when nothing changed, which is the common case within a tick.
 */
void SortVehicleRegistry()
{
	VehicleRegistryData &data = Data();
	if (!data.dirty) return;

	data.registry.sort<VehicleRef>([](const VehicleRef &lhs, const VehicleRef &rhs) {
		return lhs.id < rhs.id;
	});
	data.dirty = false;

	/* Cheap enough to run on every structural change, and it catches a leaked or
	 * doubly-destroyed entity at the moment it happens rather than much later.
	 * The O(n) checks live in ValidateVehicleRegistry instead. */
	assert(GetVehicleEntityCount() == Vehicle::GetNumItems());
}

/**
 * Check every invariant the registry is supposed to hold.
 *
 * O(number of vehicles), so this is for occasional use: after a load, at the end of a
 * benchmark run, or when hunting a specific bug. The per-change checking is the cheap
 * assert in #SortVehicleRegistry.
 *
 * Sorts first, so that the order check verifies EnTT's sort really does produce
 * ascending iteration rather than merely verifying that we sorted at some point.
 *
 * @return True if the registry is consistent with the vehicle pool.
 */
bool ValidateVehicleRegistry()
{
	SortVehicleRegistry();

	const entt::registry &registry = Data().registry;

	if (GetVehicleEntityCount() != Vehicle::GetNumItems()) return false;

	/* Every live vehicle has an entity, and that entity points back at it. */
	for (const Vehicle *v : Vehicle::Iterate()) {
		const entt::entity entity = GetVehicleEntity(v->index);
		if (entity == entt::null) return false;
		if (!registry.valid(entity)) return false;
		if (registry.get<VehicleRef>(entity).id != v->index) return false;
	}

	/* Iteration is in ascending VehicleID order, i.e. the same order PoolIterator uses. */
	bool seen_any = false;
	VehicleID previous{};
	for (const entt::entity entity : registry.view<VehicleRef>()) {
		const VehicleID id = registry.get<VehicleRef>(entity).id;
		if (seen_any && !(previous < id)) return false;
		previous = id;
		seen_any = true;
	}

	return true;
}
