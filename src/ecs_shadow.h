/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ecs_shadow.h Shadow-mode verification for the ECS migration. */

#ifndef ECS_SHADOW_H
#define ECS_SHADOW_H

#include "core/enum_type.hpp"

/**
 * Shadow mode is how a migration phase proves that moving a field changed nothing.
 *
 * While a field is being migrated it lives in two places at once: the original member
 * on #Vehicle and the new component. Every read compares them and records the result.
 * Once a long run reports zero mismatches, the original member can be deleted.
 *
 * Counting rather than asserting is deliberate. An assert stops at the first mismatch,
 * which tells you only that something is wrong; a counter lets the run finish and
 * reports "4 mismatches out of 431 million comparisons", which says how *often* and
 * therefore usually suggests why. The count is reported by the benchmark harness as
 * `shadow.<name>.mismatches`, so a phase's exit criterion can be checked rather than
 * assumed.
 *
 * **Know the blind spot.** Verification happens where a read goes through the accessor,
 * so a field that nothing reads that way is never checked -- while the *group* it belongs
 * to still reports zero mismatches, because the counters are per group and not per field.
 * A missing sync on `VehicleCache::cached_vis_effect` survived tens of millions of
 * `vehicle_cache` comparisons for exactly this reason. Zero mismatches is a statement
 * about read coverage, not about the field group: before trusting one, check that
 * something reads the field in question through the accessor.
 *
 * Enabled alongside asserts by default, because both are correctness tooling rather
 * than something to ship. Define OTTD_ECS_SHADOW explicitly to force it on in a
 * release build, which is worth doing at least once per migrated field: it is the only
 * practical way to check a five-thousand-tick wentbourne run, where a Debug build
 * would take the best part of an hour.
 */
#if !defined(OTTD_ECS_SHADOW) && defined(WITH_ASSERT) && !defined(OTTD_ECS_NO_SHADOW)
#	define OTTD_ECS_SHADOW
#endif

/**
 * The field groups currently under shadow verification.
 *
 * Empty at the moment: `VehicleCache` and `VehicleMotion` have both completed their
 * migrations, and a field group is removed from here once its member is deleted and the
 * component holds the only copy. The scaffolding stays because the remaining phase 4
 * field groups -- `cur_speed`, `direction`, `progress`, `tick_counter` and the three
 * position fields -- each need it in turn.
 */
enum class ShadowCheck : uint8_t {
	End, ///< End of enum, must be last.
};
DECLARE_ENUM_AS_SEQUENTIAL(ShadowCheck)

/** Outcome of one shadow-checked field group. */
struct ShadowCheckResult {
	uint64_t comparisons = 0; ///< How many times the two copies were compared.
	uint64_t mismatches = 0; ///< How many of those disagreed. Must be zero.
};

std::string_view GetShadowCheckName(ShadowCheck check);
ShadowCheckResult GetShadowCheckResult(ShadowCheck check);

#ifdef OTTD_ECS_SHADOW

void RecordShadowComparison(ShadowCheck check, bool matched);

/**
 * Compare a shadowed field group against its component and record the outcome.
 * @param check Which field group is being verified.
 * @param original The value still held on Vehicle.
 * @param shadow The value held in the component.
 * @return The component's value, so this can wrap a return statement.
 */
template <typename T>
inline const T &ShadowVerify(ShadowCheck check, const T &original, const T &shadow)
{
	RecordShadowComparison(check, original == shadow);
	return shadow;
}

#else

/** @copydoc ShadowVerify */
template <typename T>
inline const T &ShadowVerify(ShadowCheck, const T &, const T &shadow)
{
	return shadow;
}

#endif /* OTTD_ECS_SHADOW */

#endif /* ECS_SHADOW_H */
