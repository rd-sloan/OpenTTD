/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file benchmark_stats.h Recording of benchmark timings and workload statistics. */

#ifndef BENCHMARK_STATS_H
#define BENCHMARK_STATS_H

void WriteBenchmarkStats(std::string_view filename, uint ticks, uint64_t wallclock_us);

#endif /* BENCHMARK_STATS_H */
