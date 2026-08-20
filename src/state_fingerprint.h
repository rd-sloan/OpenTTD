/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file state_fingerprint.h Deterministic hashing of the game state, for verifying refactors. */

#ifndef STATE_FINGERPRINT_H
#define STATE_FINGERPRINT_H

/**
 * Hashes of the game state, split by subsystem.
 *
 * Split rather than combined into a single number so that a mismatch says *where*
 * behaviour changed. A refactor that alters vehicle movement leaves #towns alone,
 * which turns "something diverged" into a much shorter search.
 */
struct StateFingerprint {
	uint64_t vehicles = 0; ///< Vehicle positions, speeds, cargo, orders and chain structure.
	uint64_t companies = 0; ///< Company balances.
	uint64_t stations = 0; ///< Station cargo waiting.
	uint64_t towns = 0; ///< Town populations.
	uint64_t industries = 0; ///< Industry production.
	uint64_t globals = 0; ///< Shared randomiser, dates and object counts.
	uint64_t combined = 0; ///< All of the above.

	uint32_t random_state_0 = 0; ///< Raw `_random` state, reported separately as a canary.
	uint32_t random_state_1 = 0; ///< Raw `_random` state, reported separately as a canary.
};

StateFingerprint ComputeStateFingerprint();

#endif /* STATE_FINGERPRINT_H */
