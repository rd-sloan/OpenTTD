/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_components.h Components held by vehicle entities in the ECS registry. */

#ifndef VEHICLE_COMPONENTS_H
#define VEHICLE_COMPONENTS_H

#include "gfx_type.h"
#include "table/sprites.h"

/* For VehicleCache, which VehicleCacheComponent embeds by value so that the shadow
 * check can compare whole structs via its defaulted operator<=>. Heavier than ideal,
 * but there is no cycle: vehicle_base.h only declares the accessors, it does not
 * include this header. */
#include "vehicle_base.h"

/**
 * Cached colour remapping for a vehicle.
 *
 * Presentation only, and never serialised: the value is recomputed on demand from the
 * engine's livery and its NewGRF colour-mapping callback. #PAL_NONE means "not computed
 * yet", so clearing the component is how a livery change is published.
 *
 * Because nothing here affects the game state, this component is exempt from the
 * canonical ordering rule in vehicle_registry.cpp: iteration order cannot influence
 * behaviour, so its views are never sorted. @see ResetVehicleColourMap
 */
struct VehicleColourMap {
	PaletteID map = PAL_NONE; ///< The cached palette, or #PAL_NONE when not yet computed.
};

/**
 * Consist-derived values that the simulation reads every tick.
 *
 * Recomputed by the `ConsistChanged` paths whenever a consist's composition changes,
 * and absent from the savegame -- the single mention of `vcache` in `vehicle_sl.cpp` is
 * a read during afterload, not a descriptor.
 *
 * Unlike the phase 2 components this one **affects the game state**: `cached_max_speed`
 * drives movement and `cached_cargo_age_period` drives cargo ageing in
 * `CallVehicleTicks`. So it is shadow-verified rather than simply moved, and a mismatch
 * would be a desync rather than a cosmetic defect. @see ecs_shadow.h
 *
 * Wraps #VehicleCache rather than replacing it, so the type keeps its defaulted
 * `operator<=>` and the shadow check can compare whole structs.
 */
struct VehicleCacheComponent {
	VehicleCache cache{}; ///< The cached consist values.
};

#endif /* VEHICLE_COMPONENTS_H */
