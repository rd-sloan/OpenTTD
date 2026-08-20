/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file null_v.cpp The video driver that doesn't blit. */

#include "../stdafx.h"
#include "../benchmark_stats.h"
#include "../gfx_func.h"
#include "../blitter/factory.hpp"
#include "../saveload/saveload_func.h"
#include "../window_func.h"
#include "null_v.h"

#include <chrono>

#include "../safeguards.h"

/** Factory for the null video driver. */
static FVideoDriver_Null iFVideoDriver_Null;

std::optional<std::string_view> VideoDriver_Null::Start(const StringList &parm)
{
#ifdef _MSC_VER
	/* Disable the MSVC assertion message box. */
	_set_error_mode(_OUT_TO_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

	this->UpdateAutoResolution();

	this->ticks = GetDriverParamInt(parm, "ticks", 1000);

	/* Optional 'stats=<path>' parameter writes a benchmark report once the ticks are done. */
	auto stats = GetDriverParam(parm, "stats");
	if (stats.has_value()) this->stats_file.assign(*stats);

	_screen.width  = _screen.pitch = _cur_resolution.width;
	_screen.height = _cur_resolution.height;
	_screen.dst_ptr = nullptr;
	ScreenSizeChanged();

	/* Do not render, nor blit */
	Debug(misc, 1, "Forcing blitter 'null'...");
	BlitterFactory::SelectBlitter("null");
	return std::nullopt;
}

void VideoDriver_Null::Stop() { }

void VideoDriver_Null::MakeDirty(int, int, int, int) {}

void VideoDriver_Null::MainLoop()
{
	uint i;

	const auto start = std::chrono::steady_clock::now();

	for (i = 0; i < this->ticks; i++) {
		::GameLoop();
		::InputLoop();
		::UpdateWindows();
	}

	const auto elapsed = std::chrono::steady_clock::now() - start;

	/* Report before the exit save, so the timing does not include writing the savegame.
	 * The game state is still live at this point, which is what the workload counts need. */
	if (!this->stats_file.empty()) {
		WriteBenchmarkStats(this->stats_file, this->ticks,
				std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
	}

	/* If requested, make a save just before exit. The normal exit-flow is
	 * not triggered from this driver, so we have to do this manually. */
	if (_settings_client.gui.autosave_on_exit) {
		DoExitSave();
	}
}

bool VideoDriver_Null::ChangeResolution(int, int) { return false; }

bool VideoDriver_Null::ToggleFullscreen(bool) { return false; }
