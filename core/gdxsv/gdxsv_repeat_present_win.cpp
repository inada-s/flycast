// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_repeat_present.h"

#include <dxgi1_3.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "cfg/option.h"
#include "gdxsv_config.h"
#include "gdxsv_perf_trace.h"
#include "gdxsv_sleep.h"
#include "gdxsv_video_hooks.h"
#include "windows/comptr.h"

bool gdxsv_video_repeat_active = false;

namespace {

// Render thread state, except next_frame and limiter_elsewhere (written by the limiter's thread).
struct GdxsvRepeatPresent {
	void (*present_again)() = nullptr;
	std::thread::id render_thread;
	bool composed = false;
	// A four-screen replay guest renders into a window nobody sees.
	bool hidden = false;
	unsigned presents = 0;
	// Recent blocks of a repeated present, us, for the room a repeat needs before the next frame.
	std::array<int64_t, 32> blocks{};
	unsigned block_count = 0;
	std::atomic<int64_t> next_frame{0};
	// The limiter runs on another thread (threaded rendering in a rollback battle): the render thread repeats while it
	// waits for the next frame. Otherwise the next frame may already be queued when the render thread waits for it.
	std::atomic<bool> limiter_elsewhere{false};
};

GdxsvRepeatPresent repeat;

// A repeat must end this long before the next frame is due.
constexpr int64_t kMarginUs = 1000;
// Room needed before the first repeat is timed: a vsync of a 240 Hz display plus the margin.
constexpr int64_t kInitialBlockUs = 4000;

// The room a repeat needs: the 4th longest of the recent blocks, so that a single stall does not stop the repeats,
// while a display whose vsyncs are always long (a 60 Hz primary display) does not get repeats that make frames late.
int64_t block_room_us() {
	if (repeat.block_count < 8) return kInitialBlockUs;
	auto b = repeat.blocks;
	const size_t n = std::min<size_t>(repeat.block_count, b.size());
	std::nth_element(b.begin(), b.begin() + 3, b.begin() + n, std::greater<int64_t>());
	return std::max(b[3], kInitialBlockUs);
}

int64_t steady_now_ns() {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void update_active() {
	gdxsv_video_repeat_active = repeat.present_again != nullptr && repeat.composed && config::GdxRepeatPresent &&
								!repeat.hidden && config::VSync && !settings.input.fastForwardMode;
}

bool repeat_now(int64_t until_next_us) {
	update_active();
	if (!gdxsv_video_repeat_active || std::this_thread::get_id() != repeat.render_thread) return false;
	if (until_next_us < block_room_us() + kMarginUs) return false;
	const auto start = std::chrono::steady_clock::now();
	repeat.present_again();
	const int64_t block =
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
	repeat.blocks[repeat.block_count++ % repeat.blocks.size()] = block;
	// arg: microseconds the repeated present blocked
	GDXSV_PERF_TRACE("repeat", -1, block);
	return true;
}

}  // namespace

void gdxsv_video_dx11_attach(void (*present_again)()) {
	repeat.present_again = present_again;
	repeat.render_thread = std::this_thread::get_id();
	repeat.composed = false;
	repeat.hidden = present_again != nullptr && gdxsv_video_hidden_window();
	repeat.block_count = 0;
	update_active();
}

void gdxsv_video_dx11_presented(IDXGISwapChain* swapchain) {
	GDXSV_PERF_TRACE("dx11_present", -1, 0);
	// Sampled: the composition mode changes only with the window (fullscreen, covered, moved).
	if (repeat.presents++ % 16 != 0 || (!config::GdxRepeatPresent && !gdxsv_perf_trace_on)) return;
	ComPtr<IDXGISwapChainMedia> media;
	DXGI_FRAME_STATISTICS_MEDIA stats{};
	// Fails until a frame was displayed, and after a mode change or a move to another display.
	if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&media.get()))) || FAILED(media->GetFrameStatisticsMedia(&stats)))
		return;
	repeat.composed = stats.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_COMPOSED;
	update_active();
	// arg: DXGI_FRAME_PRESENTATION_MODE (0 composed, 1 overlay, 2 none, 3 composition failure)
	GDXSV_PERF_TRACE("present_mode", -1, stats.CompositionMode);
}

int gdxsv_video_repeat_wait_ms(int timeout) {
	if (!repeat.limiter_elsewhere.load(std::memory_order_relaxed)) return timeout;
	const int64_t next = repeat.next_frame.load(std::memory_order_relaxed);
	if (next == 0) {
		// An OS sleep, not a busy wait: a late check only starts the repeats a little later.
		gdxsv_sleep_us(1000);
		return 0;
	}
	return repeat_now((next - steady_now_ns()) / 1000) ? -1 : timeout;
}

bool gdxsv_repeat_present(int64_t until_next_us) { return repeat_now(until_next_us); }

void gdxsv_repeat_present_next_frame(int64_t steady_ns) {
	repeat.limiter_elsewhere.store(std::this_thread::get_id() != repeat.render_thread, std::memory_order_relaxed);
	repeat.next_frame.store(steady_ns, std::memory_order_relaxed);
}
