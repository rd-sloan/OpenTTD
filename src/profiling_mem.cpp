/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file profiling_mem.cpp Global operator new and delete replacements that count allocations.
 *
 * Compiled only when OPTION_TRACY_MEM is set, which also requires OPTION_TRACY. This is
 * phase M0 of docs/tracy-memory-plan.md: the allocation count is measured and plotted, and
 * nothing is reported to Tracy's memory subsystem yet. Allocation volume decides whether
 * the later phases are viable at all, and a counter that reads wrong is a bad number, while
 * a Tracy memory event that reads wrong terminates the whole capture with no diagnostic.
 *
 * This file deliberately does not include safeguards.h. Every other translation unit
 * includes it last, which turns malloc into a compile error; a replacement operator new has
 * to call malloc, so the exemption lives here and nowhere else. No CI check enforces the
 * inclusion, so this is a convention deviation rather than a build failure waiting to happen.
 *
 * Replacement operator new must be defined exactly once in the program, so none of this can
 * live in a header. It is compiled into openttd_lib, which is an OBJECT library, so every
 * object is linked into openttd and openttd_test whether or not anything references it.
 *
 * @see docs/tracy-memory-plan.md for the phasing.
 * @see docs/tracy-memory-log.md for what was measured and what changed.
 */

#include "stdafx.h"

#include <atomic>
#include <new>

/*
 * Relaxed ordering throughout. Nothing here orders anything else, and the plotted figures
 * are diagnostics rather than a reconciliation of two views of the same data structure.
 *
 * std::atomic's value-initialising constructor is constexpr for integral types, so these are
 * constant-initialised and therefore already zero before any dynamic initialisation runs.
 * That matters: operator new is called during the static initialisation of other translation
 * units, in an order this file cannot influence.
 */
static std::atomic<uint64_t> _mem_alloc_count{0}; ///< Allocations counted since process start.
static std::atomic<uint64_t> _mem_free_count{0}; ///< Deallocations counted since process start.
static std::atomic<uint64_t> _mem_alloc_bytes{0}; ///< Bytes requested, summed over all allocations.

/**
 * Allocate and count.
 * @param size The requested size in bytes.
 * @return The allocation, or nullptr on failure.
 * @note A zero-byte request is bumped to one byte, because operator new must return a
 *       distinct non-null pointer for it and malloc(0) is permitted to return nullptr.
 */
static void *CountedAlloc(size_t size)
{
	void *ptr = malloc(size == 0 ? 1 : size);
	if (ptr != nullptr) {
		_mem_alloc_count.fetch_add(1, std::memory_order_relaxed);
		_mem_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
	}
	return ptr;
}

/**
 * Free and count.
 * @param ptr The allocation to release; may be nullptr.
 * @note Deleting a null pointer is well defined and is not counted, so that the free count
 *       stays comparable with the allocation count.
 */
static void CountedFree(void *ptr)
{
	if (ptr == nullptr) return;
	_mem_free_count.fetch_add(1, std::memory_order_relaxed);
	free(ptr);
}

/*
 * The replaceable operator new and delete family, minus the over-aligned forms.
 *
 * Both halves of every pair below are replaced. That is the whole discipline here: a
 * replaced operator delete paired with the default operator new reports frees that were
 * never allocated, which is precisely what Tracy terminates a capture over once phase M2
 * starts reporting events. Overriding neither half of a pair is safe, overriding one is not.
 *
 * The over-aligned forms, operator new(size_t, align_val_t) and friends, are left to the
 * default implementation as a complete pair. They route through _aligned_malloc and
 * _aligned_free, which cannot be mixed with malloc and free, so replacing them means a
 * second platform-specific allocator path for the sake of the over-aligned types OpenTTD
 * does not appear to have. The cost is that such allocations are invisible rather than
 * miscounted, which is the right way round.
 */

void *operator new(size_t size)
{
	void *ptr = CountedAlloc(size);
	if (ptr == nullptr) throw std::bad_alloc();
	return ptr;
}

void *operator new[](size_t size)
{
	void *ptr = CountedAlloc(size);
	if (ptr == nullptr) throw std::bad_alloc();
	return ptr;
}

void *operator new(size_t size, const std::nothrow_t &) noexcept
{
	return CountedAlloc(size);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
	return CountedAlloc(size);
}

void operator delete(void *ptr) noexcept
{
	CountedFree(ptr);
}

void operator delete[](void *ptr) noexcept
{
	CountedFree(ptr);
}

/* Sized delete is what the MSVC standard library actually calls for most container
 * teardown, so leaving it to the default implementation would lose most of the free count
 * while the allocation count stayed complete. */
void operator delete(void *ptr, size_t) noexcept
{
	CountedFree(ptr);
}

void operator delete[](void *ptr, size_t) noexcept
{
	CountedFree(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
	CountedFree(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
	CountedFree(ptr);
}

ScopedProfilerMemoryTick::ScopedProfilerMemoryTick() :
	allocs(_mem_alloc_count.load(std::memory_order_relaxed)),
	bytes(_mem_alloc_bytes.load(std::memory_order_relaxed))
{
}

ScopedProfilerMemoryTick::~ScopedProfilerMemoryTick()
{
	const uint64_t allocs_now = _mem_alloc_count.load(std::memory_order_relaxed);
	const uint64_t bytes_now = _mem_alloc_bytes.load(std::memory_order_relaxed);
	const uint64_t frees_now = _mem_free_count.load(std::memory_order_relaxed);

	OTTD_PLOT("mem.allocs_per_tick", (int64_t)(allocs_now - this->allocs));
	OTTD_PLOT("mem.bytes_per_tick", (int64_t)(bytes_now - this->bytes));
	/* Signed on purpose. This can only go negative if something frees memory that a
	 * different operator new allocated, which would be the first sign that the family
	 * above is incomplete. */
	OTTD_PLOT("mem.live_blocks", (int64_t)allocs_now - (int64_t)frees_now);
}
