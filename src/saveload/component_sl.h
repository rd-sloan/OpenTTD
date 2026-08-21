/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file component_sl.h Save/load descriptors that read and write ECS components instead of members. */

#ifndef SAVELOAD_COMPONENT_SL_H
#define SAVELOAD_COMPONENT_SL_H

#include "saveload.h"

#include "../vehicle_base.h"
#include "../vehicle_registry.h"

/**
 * Descriptors for fields that have moved out of #Vehicle and into a component.
 *
 * The problem this solves is the one constraint 3.3 of the migration plan describes:
 * `SLE_VAR(Vehicle, subspeed, ...)` expands to an address getter that does
 * `std::addressof(static_cast<const Vehicle *>(b)->subspeed)`, so the descriptor is
 * bound to a *member of Vehicle*. A field cannot leave the struct while a descriptor
 * names it, which blocks commit 3 of every migration for every serialised field --
 * and all nine motion fields are serialised.
 *
 * The migration plan originally proposed a staging struct: members mirroring the
 * descriptor names, gathered from components before a save and scattered back after
 * a load. That turned out to be unnecessary for the common case. #SaveLoad holds its
 * address getter as a plain function pointer (#SaveLoad::AddressFunction) and
 * #GetVariableAddress const_casts the result, so a getter may return the address of
 * *anything* -- including a field inside a component. Pointing the descriptor at the
 * component directly means:
 *
 * - no duplicate storage, so `sizeof(Vehicle)` actually falls;
 * - no gather step, so a field cannot be missed on the way out;
 * - no scatter step, so a component cannot start out stale on the way in. This is
 *   the bug that produced 462 shadow mismatches in phase 4, and here it cannot
 *   happen, because load writes the component and there is nowhere else to write.
 *
 * The savegame is unchanged. #SLE_GENERAL_NAME already separates the field's name in
 * the savegame from the member expression used to find it, so the name stays
 * `"subspeed"` while the storage moves. Nothing about the file format depends on
 * where the value lived in memory.
 *
 * Staging is still the answer when the mapping is not one-to-one -- a saved field
 * computed from several component fields, a representation that differs between disk
 * and memory, or a field whose component does not exist for every vehicle. These
 * macros deliberately do not try to cover that; they cover the plain case, which is
 * most of them.
 *
 * **The entity must already exist when a descriptor runs.** On load that holds
 * because the chunk handler constructs the vehicle before calling #SlObject --
 * `Train::CreateAtIndex(index)` runs the #Vehicle constructor, which calls
 * #RegisterVehicleEntity -- so the component is present by the time the getter is
 * invoked. Anything that reorders those two steps breaks these descriptors, which is
 * why it is spelled out here rather than left to be rediscovered.
 */

/**
 * Storage of a vehicle component field, under an explicit savegame field name.
 * @param name      Field name in the savegame. Must match the name the member had.
 * @param base      Class or struct the save object is; must expose `index`.
 * @param component Component type holding the field.
 * @param field     Name of the field within \a component.
 * @param type      Storage of the data in memory and in the savegame.
 * @param from      First savegame version that has the field.
 * @param to        Last savegame version that has the field.
 */
#define SLE_CONDVAR_COMPONENT_NAME(name, base, component, field, type, from, to) \
	SaveLoad {name, SaveLoadType::Variable, type, 0, from, to, [] (const void *b, size_t) -> const void * { \
		static_assert(SlCheckVarSize(SaveLoadType::Variable, type, 0, sizeof(std::declval<component &>().field))); \
		assert(b != nullptr); \
		return std::addressof(GetVehicleRegistry().get<component>(GetVehicleEntity(static_cast<const base *>(b)->index)).field); \
	}, 0, nullptr}

/**
 * Storage of a vehicle component field in some savegame versions.
 * @see SLE_CONDVAR_COMPONENT_NAME
 */
#define SLE_CONDVAR_COMPONENT(base, component, field, type, from, to) \
	SLE_CONDVAR_COMPONENT_NAME(#field, base, component, field, type, from, to)

/**
 * Storage of a vehicle component field in every savegame version.
 * @see SLE_CONDVAR_COMPONENT_NAME
 */
#define SLE_VAR_COMPONENT(base, component, field, type) \
	SLE_CONDVAR_COMPONENT(base, component, field, type, SaveLoadVersion::MinVersion, SaveLoadVersion::MaxVersion)

#endif /* SAVELOAD_COMPONENT_SL_H */
