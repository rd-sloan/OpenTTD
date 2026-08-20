/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file entt_smoke.cpp Tests that EnTT is wired up, and that its ordering behaves as the migration assumes.
 *
 * These are not tests of EnTT itself. They pin down the two properties the ECS migration
 * plan depends on, so that a future EnTT upgrade that changed either one would fail here
 * rather than silently producing desyncs:
 *
 * - views walk the packed array in reverse, so iteration is not creation order;
 * - default storage reorders on removal, so packed order is a function of history;
 * - sorting a storage by a stable key restores a canonical order regardless of history,
 *   and compensates for the reversed walk so that iteration matches the comparator.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <vector>

#include "../safeguards.h"

namespace {

/** Stand-in for a stable external identifier, i.e. the role VehicleID plays in the real game. */
struct StableId {
	uint32_t value;
};

/** Stand-in for a hot per-tick component. */
struct Position {
	int32_t x;
	int32_t y;
};

/**
 * Collect the stable ids of a view in iteration order.
 * @param registry The registry to read.
 * @return The ids, in the order the view yields them.
 */
std::vector<uint32_t> IterationOrder(entt::registry &registry)
{
	std::vector<uint32_t> order;
	for (auto entity : registry.view<StableId>()) {
		order.push_back(registry.get<StableId>(entity).value);
	}
	return order;
}

}

TEST_CASE("EnTT is available and functional")
{
	entt::registry registry;

	const auto entity = registry.create();
	registry.emplace<StableId>(entity, 42u);
	registry.emplace<Position>(entity, 3, 4);

	CHECK(registry.valid(entity));
	CHECK(registry.all_of<StableId, Position>(entity));
	CHECK(registry.get<StableId>(entity).value == 42u);

	int seen = 0;
	for (auto [e, id, pos] : registry.view<StableId, Position>().each()) {
		CHECK(id.value == 42u);
		CHECK(pos.x == 3);
		seen++;
	}
	CHECK(seen == 1);

	registry.destroy(entity);
	CHECK_FALSE(registry.valid(entity));
	CHECK(registry.view<StableId>().empty());
}

TEST_CASE("Views iterate in reverse packed order")
{
	/* Worth pinning down explicitly because it is a genuine surprise: a view walks the
	 * packed array backwards, so entities come out in reverse creation order even when
	 * nothing has been removed. Any code that assumes "first created, first visited"
	 * is wrong from the outset, before history dependence enters the picture.
	 *
	 * This is visible only on unsorted storage. sparse_set::begin() starts at
	 * packed.size() and dereferences packed[offset - 1], walking down to packed[0]. */
	entt::registry registry;

	for (uint32_t i = 0; i < 8; i++) {
		registry.emplace<StableId>(registry.create(), i);
	}

	CHECK(IterationOrder(registry) == std::vector<uint32_t>{7, 6, 5, 4, 3, 2, 1, 0});
}

TEST_CASE("Default storage order is history dependent")
{
	/* This is the property that makes naive EnTT iteration unsafe for OpenTTD's game
	 * state: the same live set can be visited in different orders depending on how it
	 * was arrived at. A savegame records the live set but not the history, so a reloaded
	 * game would iterate differently from the run that saved it. */
	entt::registry registry;

	std::vector<entt::entity> entities;
	for (uint32_t i = 0; i < 8; i++) {
		const auto entity = registry.create();
		registry.emplace<StableId>(entity, i);
		entities.push_back(entity);
	}

	/* Remove from the middle. Swap-and-pop moves the last element into the hole, so the
	 * packed array becomes 0,1,7,3,4,5,6 and the reversed walk yields 6,5,4,3,7,1,0. */
	registry.destroy(entities[2]);

	const std::vector<uint32_t> after = IterationOrder(registry);
	REQUIRE(after.size() == 7);
	CHECK(after == std::vector<uint32_t>{6, 5, 4, 3, 7, 1, 0});

	/* The result is in no canonical order at all: not ascending, and no longer the
	 * descending order that an untouched registry would have produced. */
	CHECK_FALSE(std::is_sorted(after.begin(), after.end()));
	CHECK_FALSE(std::is_sorted(after.rbegin(), after.rend()));
}

TEST_CASE("Sorting by a stable key restores canonical order")
{
	/* The migration's answer to the above: sort the storage by the stable id after any
	 * structural change, which makes iteration order a pure function of the live set.
	 *
	 * Note that a 'less than' comparator yields ascending iteration despite the reversed
	 * walk, i.e. sort arranges storage so that iteration matches the comparator. That is
	 * the behaviour the migration relies on to reproduce OpenTTD's ascending-index order.
	 *
	 * The two reversals cancel by design: sort_n sorts through reverse iterators
	 * (packed.rend() - length, packed.rend()), leaving the array physically descending,
	 * and iteration then walks it backwards. So the reversed walk is invisible as long
	 * as the storage is sorted, and only leaks on unsorted storage as above. */
	entt::registry registry;

	std::vector<entt::entity> entities;
	for (uint32_t i = 0; i < 8; i++) {
		const auto entity = registry.create();
		registry.emplace<StableId>(entity, i);
		entities.push_back(entity);
	}

	registry.destroy(entities[2]);
	registry.destroy(entities[5]);

	registry.sort<StableId>([](const StableId &lhs, const StableId &rhs) { return lhs.value < rhs.value; });

	const std::vector<uint32_t> sorted = IterationOrder(registry);
	CHECK(sorted == std::vector<uint32_t>{0, 1, 3, 4, 6, 7});
	CHECK(std::is_sorted(sorted.begin(), sorted.end()));
}
