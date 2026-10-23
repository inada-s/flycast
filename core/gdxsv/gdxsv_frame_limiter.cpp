// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// FixedFrequency frame limiter for the main UI loop (mainui_loop).
#include "gdxsv_frame_limiter.h"

#include <algorithm>
#include <chrono>

#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_perf_trace.h"
#include "gdxsv_repeat_present.h"
#include "gdxsv_sleep.h"
#include "log/Log.h"
#include "ui/gui.h"

// When the next frame is due.
static std::chrono::time_point<std::chrono::steady_clock> nextFrame;

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
	gdxsv_set_timer_resolution();
	nextFrame = {};
}

void gdxsv_emu_mainui_loop_end() {
	gdxsv_reset_timer_resolution();
}

void gdxsv_frame_limiter_wait() {
	if (!config::FixedFrequency || gui_is_open() || settings.input.fastForwardMode) {
		nextFrame = {};
		gdxsv_repeat_present_next_frame(0);
		return;
	}

	const auto period = std::chrono::microseconds(gdxsv_trimmed_frame_period_us());
	int64_t overSlept = 0;

	auto now = std::chrono::steady_clock::now();
	if (nextFrame == decltype(nextFrame){}) nextFrame = now;
	auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(nextFrame - now).count();
	while (0 < remaining && gdxsv_repeat_present(remaining))
		remaining = std::chrono::duration_cast<std::chrono::microseconds>(nextFrame - std::chrono::steady_clock::now()).count();
	if (remaining > 0)
		overSlept = gdxsv_sleep_and_busy_wait(remaining);

	// The schedule is absolute: a frame late by less than a period is paid back by the next waits.
	// A frame late by a period or more skips whole periods, so a stall is not followed by a burst
	// of short frames.
	now = std::chrono::steady_clock::now();
	if (now - nextFrame >= period) nextFrame += (now - nextFrame) / period * period;
	nextFrame += period;
	gdxsv_repeat_present_next_frame(std::chrono::duration_cast<std::chrono::nanoseconds>(nextFrame.time_since_epoch()).count());
	// arg: the slack waited before this wake, us; negative: the frame was late
	GDXSV_PERF_TRACE("wake", -1, remaining);
	if (1000 <= overSlept)
		WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", (int)overSlept);

	gdxsv_sleep_benchmark_periodic_report();
}
