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

void SortVehicleRegistry();
bool ValidateVehicleRegistry();

/* Lifecycle hooks. Called only from Vehicle's constructor and destructor, which
 * between them cover every creation and destruction path including savegame load
 * (Pool::CreateAtIndex forwards to the constructor) and pool cleaning. */
entt::entity RegisterVehicleEntity(VehicleID id);
void UnregisterVehicleEntity(VehicleID id);

#endif /* VEHICLE_REGISTRY_H */
