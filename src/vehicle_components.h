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

#endif /* VEHICLE_COMPONENTS_H */
