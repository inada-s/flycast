// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// FixedFrequency frame limiter for the main UI loop (mainui_loop).
#include "gdxsv_frame_limiter.h"
#include "gdxsv_frame_trace.h"

#include <algorithm>
#include <chrono>

#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_sleep.h"
#include "log/Log.h"
#include "ui/gui.h"

static std::chrono::time_point<std::chrono::steady_clock> start;
static bool currentDupeFrames;

static int64_t gdxsv_frame_period_us() {
	const auto mode = config::FixedFrequency.get();
	// Native NTSC/VGA
	if (mode == 2 ||
		(mode == 1 && (config::Cable == 0 || config::Cable == 1)) ||
		(mode == 1 && config::Cable == 3 && (config::Broadcast == 0 || config::Broadcast == 4)))
		return 16683; // 1/59.94
	// Approximate VGA
	if (mode == 3) return 16666; // 1/60
	// PAL
	if (mode == 4 || (mode == 1 && config::Cable == 3))
		return 20000; // 1/50
	// Half Native NTSC/VGA
	if (mode == 5) return 33333; // 1/30
	return 16683;
}

// The nominal period, plus whatever trim replay/Live Spectate is asking for.
// The trim is zero outside those modes.
static int64_t gdxsv_trimmed_frame_period_us() {
	const int64_t trim = gdxsv_frame_period_trim_us;
	return std::max<int64_t>(1000, gdxsv_frame_period_us() + trim);
}

void gdxsv_emu_mainui_loop_start() {
	gdxsv_trace_init();
	currentDupeFrames = config::DupeFrames;
	gdxsv_set_timer_resolution();
	start = {};
}

void gdxsv_emu_mainui_loop_end() {
	gdxsv_reset_timer_resolution();
}

bool gdxsv_emu_mainui_reinit_needed() {
	if (currentDupeFrames == config::DupeFrames)
		return false;
	currentDupeFrames = config::DupeFrames;
	return true;
}

void gdxsv_frame_limiter_wait() {
	if (!config::FixedFrequency || gui_is_open() || settings.input.fastForwardMode)
		return;

	const auto period = gdxsv_trimmed_frame_period_us();
	int64_t overSlept = 0;

	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - start).count();
	const auto remaining = period - elapsed;
	if (remaining > 0)
		overSlept = gdxsv_sleep_and_busy_wait(remaining);

	start = std::chrono::steady_clock::now();
	if (1000 <= overSlept)
		WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", (int)overSlept);

	gdxsv_sleep_benchmark_periodic_report();
}
