/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_registry.h The EnTT registry holding vehicle entities, and its identity mapping. */

#ifndef VEHICLE_REGISTRY_H
#define VEHICLE_REGISTRY_H

#include "vehicle_type.h"

#include <entt/entity/registry.hpp>

/**
 * Phase 6 variant selector.
 *
 * Undefined: variant A, one pass in ascending #VehicleID with the tick handler reached
 * through a type switch. Defined: variant B, one typed pass per vehicle type over a
 * private storage, which changes the interleaving of `Random()` draws and therefore the
 * trajectory a stock savegame continues on. See docs/ecs-migration-plan.md phase 6.
 *
 * A compile-time switch rather than a setting because the two variants are being
 * compared, not offered: a runtime branch would put the cost of both in each binary, and
 * a game whose trajectory depends on a setting is worse than one that does not.
 */
/* #define OTTD_ECS_TICK_VARIANT_B */

/**
 * The stable pool identity of a vehicle entity.
 *
 * Every vehicle entity carries exactly one of these, which makes it both the reverse
 * half of the identity mapping and the key the registry is kept sorted by.
 *
 * #VehicleID is the durable reference throughout the migration, never `entt::entity`:
 * entity identifiers carry version bits and are recycled, whereas #VehicleID is what
 * the savegame and the network protocol already agree on.
 */
struct VehicleRef {
	VehicleID id; ///< Pool index of the vehicle this entity represents.
};

/** The registry, its identity mapping, and the sort state that goes with them. */
struct VehicleRegistryData {
	/** The registry itself. Holds one entity per pooled vehicle. */
	entt::registry registry{};

	/**
	 * Forward half of the identity mapping, indexed by VehicleID.
	 * Dense rather than a hash map because VehicleIDs are pool indices and therefore
	 * already compact. Dead slots hold `entt::null`.
	 */
	std::vector<entt::entity> entity_by_vehicle_id{};

	/** Whether the structure changed since the last sort. @see SortVehicleRegistry */
	bool dirty = false;
};

/**
 * The registry data, or nullptr before first use.
 *
 * Exposed here, rather than hidden behind a function-local static in the .cpp, so that
 * the accessors below can inline. That is not a stylistic preference: with these lookups
 * out of line, every component access cost three opaque calls that the optimiser could
 * not see through, and phase 4 measured the result at +90% on the game loop.
 */
extern VehicleRegistryData *_vehicle_registry_data;

void InitVehicleRegistryData();

/**
 * Get the registry data, creating it on first use.
 *
 * The null check keeps the lazy initialisation the out-of-line version had. That laziness
 * is deliberate rather than incidental -- see #InitVehicleRegistryData for why a plain
 * file-scope object would be a static destruction order hazard -- and one perfectly
 * predicted branch is a great deal cheaper than a guarded static behind a call.
 *
 * @return The registry data.
 */
inline VehicleRegistryData &GetVehicleRegistryData()
{
	if (_vehicle_registry_data == nullptr) InitVehicleRegistryData();
	return *_vehicle_registry_data;
}

/**
 * Get the registry holding vehicle entities.
 * @return The registry.
 */
inline entt::registry &GetVehicleRegistry()
{
	return GetVehicleRegistryData().registry;
}

/**
 * Get the entity representing a vehicle.
 *
 * Prefer #Vehicle::GetEntity where a #Vehicle is in hand: it holds its own handle and
 * so skips this lookup entirely. This remains for code that has only an ID.
 *
 * @param id The vehicle to look up.
 * @return The entity, or `entt::null` if there is none.
 */
inline entt::entity GetVehicleEntity(VehicleID id)
{
	const VehicleRegistryData &data = GetVehicleRegistryData();

	const size_t index = id.base();
	if (index >= data.entity_by_vehicle_id.size()) return entt::null;
	return data.entity_by_vehicle_id[index];
}

size_t GetVehicleEntityCount();

/**
 * What keeping canonical iteration order costs.
 *
 * Phase 6 variant A only gets its locality if the storage is genuinely sorted, so the
 * sort runs whenever a vehicle was created or destroyed. Whether that is free or fatal
 * depends entirely on how often the dirty flag fires, which is a property of the
 * savegame rather than of the code -- hence a measurement rather than an argument.
 * @see SortVehicleRegistry
 */
struct VehicleRegistrySortStats {
	uint64_t calls = 0; ///< Calls to #SortVehicleRegistry, i.e. iteration points that needed the order.
	uint64_t sorts = 0; ///< Calls that found the registry dirty and therefore sorted. #calls minus this is the cheap path.
	uint64_t entities_sorted = 0; ///< Live set size summed over those sorts, which is what the O(n log n) applies to.
	uint64_t sort_ns = 0; ///< Time spent sorting the #VehicleRef key storage. Nanoseconds, because a small fixture sorts in under a microsecond.
	uint64_t sort_components_ns = 0; ///< Time spent matching the component storages to it, which is linear per storage rather than O(n log n).
	uint64_t registrations = 0; ///< Entities created. Together with #unregistrations, the churn that sets the dirty flag.
	uint64_t unregistrations = 0; ///< Entities destroyed.
};

const VehicleRegistrySortStats &GetVehicleRegistrySortStats();
void ResetVehicleRegistrySortStats();

void SortVehicleRegistry();
bool ValidateVehicleRegistry();

/* Lifecycle hooks. Called only from Vehicle's constructor and destructor, which
 * between them cover every creation and destruction path including savegame load
 * (Pool::CreateAtIndex forwards to the constructor) and pool cleaning. */
entt::entity RegisterVehicleEntity(VehicleID id, VehicleType type);
void UnregisterVehicleEntity(VehicleID id);

#endif /* VEHICLE_REGISTRY_H */
