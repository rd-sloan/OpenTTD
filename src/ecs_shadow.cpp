/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ecs_shadow.cpp Shadow-mode verification for the ECS migration. @see ecs_shadow.h */

#include "stdafx.h"

#include "ecs_shadow.h"

#include "safeguards.h"

/** Human-readable names, used as the key suffix in the benchmark report. */
static const EnumIndexArray<std::string_view, ShadowCheck, ShadowCheck::End> _shadow_check_names{
	"vehicle_cache",
	"vehicle_motion",
};

/** Tallies, indexed by #ShadowCheck. */
static EnumIndexArray<ShadowCheckResult, ShadowCheck, ShadowCheck::End> _shadow_results{};

/**
 * Get the reporting name of a shadow check.
 * @param check The check to name.
 * @return Its name.
 */
std::string_view GetShadowCheckName(ShadowCheck check)
{
	return _shadow_check_names[check];
}

/**
 * Get the tally for a shadow check.
 * @param check The check to query.
 * @return Comparison and mismatch counts. Both are zero when shadow mode is compiled out.
 */
ShadowCheckResult GetShadowCheckResult(ShadowCheck check)
{
	return _shadow_results[check];
}

#ifdef OTTD_ECS_SHADOW

/**
 * Record one comparison between a shadowed field group and its component.
 *
 * Deliberately does not assert. See ecs_shadow.h for why a count is more useful than
 * a stop at the first divergence.
 *
 * @param check Which field group was compared.
 * @param matched Whether the two copies agreed.
 */
void RecordShadowComparison(ShadowCheck check, bool matched)
{
	ShadowCheckResult &result = _shadow_results[check];
	result.comparisons++;
	if (!matched) result.mismatches++;
}

#endif /* OTTD_ECS_SHADOW */
