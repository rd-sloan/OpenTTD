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

entt::registry &GetVehicleRegistry();
entt::entity GetVehicleEntity(VehicleID id);
size_t GetVehicleEntityCount();

void SortVehicleRegistry();
bool ValidateVehicleRegistry();

/* Lifecycle hooks. Called only from Vehicle's constructor and destructor, which
 * between them cover every creation and destruction path including savegame load
 * (Pool::CreateAtIndex forwards to the constructor) and pool cleaning. */
void RegisterVehicleEntity(VehicleID id);
void UnregisterVehicleEntity(VehicleID id);

#endif /* VEHICLE_REGISTRY_H */
