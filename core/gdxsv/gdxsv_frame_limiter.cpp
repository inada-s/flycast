// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// FixedFrequency frame limiter for the main UI loop (mainui_loop).
#include "gdxsv_frame_limiter.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "cfg/option.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_present_pacing.h"
#include "gdxsv_sleep.h"
#include "log/Log.h"
#include "ui/gui.h"
#include "gdxsv_fptrace.h"

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
	currentDupeFrames = config::DupeFrames;
	gdxsv_set_timer_resolution();
	gdxsv_present_pacing_reset();
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

	auto period = gdxsv_trimmed_frame_period_us();
	static const char* fptPeriod = getenv("FPT_PERIOD");
	if (fptPeriod != nullptr) {
		period = atoi(fptPeriod);
		if (period == 0) {
			FPT("lim", 0, 0, 0);
			return;
		}
		if (period < 0) {
			// phase lock: one frame per host vblank, released -period us after it
			static int64_t usedVbl;
			const int64_t t0 = gdxsv_fpt_now();
			int64_t v;
			while ((v = gdxsv_fpt_last_vbl.load()) == usedVbl)
				gdxsv_sleep_and_busy_wait(200);
			usedVbl = v;
			const int64_t wait = v - period - gdxsv_fpt_now();
			if (wait > 0)
				gdxsv_sleep_and_busy_wait(wait);
			FPT("lim", gdxsv_fpt_now() - t0, 0, v);
			return;
		}
	}
	// probe: artificial hang every N calls (FPT_HANG=ms:N)
	static int hangMs = -1, hangEvery = 0, hangCount = 0;
	if (hangMs < 0) {
		hangMs = 0;
		if (const char* h = getenv("FPT_HANG")) sscanf(h, "%d:%d", &hangMs, &hangEvery);
	}
	if (hangMs > 0 && hangEvery > 0 && ++hangCount % hangEvery == 0) {
		FPT("hang", hangMs);
		gdxsv_sleep_us(hangMs * 1000);
	}
	// probe: limiter variants (FPT_LIM=a1|a2, FPT_HRT=1)
	static const char* lim = getenv("FPT_LIM");
	if (lim != nullptr && (!strcmp(lim, "a1") || !strcmp(lim, "a2") || !strcmp(lim, "b2"))) {
		// ideal: the ideal time of this wake; err: how late the last wake was vs its ideal time.
		// a1 pays back all of err (absolute schedule), a2 a quarter per frame.
		static int64_t ideal, err;
		const int64_t now = gdxsv_fpt_now();
		const bool b2 = lim[0] == 'b';
		const bool a1 = lim[1] == '1' || b2;
		if (ideal == 0 || now - ideal > 10 * period) {
			ideal = now;
			err = 0;
		}
		ideal += period;
		int64_t wake = a1 ? ideal : ideal + err * 3 / 4;
		if (b2) {
			// B2: wake on the host vblank nearest to the ideal time (+0.5 ms), so the present lands
			// mid-interval; the ideal schedule keeps the game rate.
			const int64_t vp = gdxsv_fpt_vbl_period.load(), lv = gdxsv_fpt_last_vbl.load();
			if (lv != 0) {
				const int64_t k = (ideal - lv + vp / 2) / vp;
				wake = lv + k * vp + 500;
			}
		}
		if (wake > now)
			gdxsv_fpt_hr_sleep_until(wake);
		const int64_t woke = gdxsv_fpt_now();
		err = woke - ideal;
		if (err >= period) {  // a hang: no catch-up
			if (a1)  // PCSX2: skip whole frames, keep the phase
				ideal += err / period * period;
			else
				ideal = woke;
			err = woke - ideal;
		}
		FPT("lim", wake - now, woke - wake, now);
		return;
	}
	int64_t overSlept = 0;

	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - start).count();
	const int64_t shift = gdxsv_present_pacing_shift_us(period);
	if (shift != 0) FPT("pace", shift, period, 0);
	const auto remaining = period + shift - elapsed;
	if (remaining > 0)
		overSlept = gdxsv_sleep_and_busy_wait(remaining);
	FPT("lim", remaining, overSlept, elapsed);

	start = std::chrono::steady_clock::now();
	if (1000 <= overSlept)
		WARN_LOG(RENDERER, "FixedFrequency: Over slept %d [us]", (int)overSlept);

	gdxsv_sleep_benchmark_periodic_report();
}
