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
 * Recomputed by the `ConsistChanged` paths whenever a consist's composition changes, and
 * absent from the savegame, which is why this component needed no save staging: the
 * afterload `ConsistChanged` / `Update*Cache` calls repopulate it the same way they
 * always populated the member.
 *
 * Unlike the phase 2 components this one **affects the game state**: `cached_max_speed`
 * drives movement and `cached_cargo_age_period` drives cargo ageing in
 * `CallVehicleTicks`, so a divergence would have been a desync rather than a cosmetic
 * defect. It was migrated under shadow verification for that reason, and now holds the
 * only copy. @see ecs_shadow.h
 *
 * Wraps #VehicleCache rather than replacing it, so the type keeps its defaulted
 * `operator<=>` -- which is what let the shadow check compare whole structs, and what
 * still lets `cachecheck.cpp` compare a saved copy against a recomputed one.
 */
struct VehicleCacheComponent {
	VehicleCache cache{}; ///< The cached consist values.
};

/**
 * Sub-tile motion accumulators, read and written every tick for every moving vehicle.
 *
 * Unlike the phase 2 and 3 components these fields **are** serialised, so the members
 * on #Vehicle stay declared and remain what the save descriptors read. While shadow
 * verification is in place they are also authoritative for writes, with the component
 * kept in step; the save format therefore needs no staging machinery yet.
 *
 * This is the first component whose fields are genuinely hot: `subspeed` is
 * accumulated in `GroundVehicle::DoUpdateSpeed` and `motion_counter` in
 * `CallVehicleTicks`, both once per vehicle per tick. Phase 4 is expected to be
 * *slower* for exactly that reason -- the packed walk that pays for the indirection is
 * phase 5's job. @see docs/ecs-migration-plan.md
 */
struct VehicleMotion {
	uint8_t subspeed = 0; ///< Fractional speed, the remainder that did not become movement.
	uint32_t motion_counter = 0; ///< Accumulates speed, used to time running sounds.

	auto operator<=>(const VehicleMotion &) const = default;
};

#endif /* VEHICLE_COMPONENTS_H */
