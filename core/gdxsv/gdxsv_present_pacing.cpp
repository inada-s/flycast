// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_present_pacing.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include "gdxsv_config.h"
#include "gdxsv_emu_hooks.h"
#include "network/ggpo.h"
#include "gdxsv_fptrace.h"

namespace {
// Present calls closer than this to a vblank can land on either side of it.
constexpr int64_t kMarginUs = 1500;
// Present calls whose median predicts the next one.
constexpr size_t kCalls = 15;
// Vblank intervals whose median is the display period.
constexpr size_t kVblanks = 31;
// Frames between two shifts. A display that follows the presents (VRR) has them always at its vblank.
constexpr int kShiftInterval = 120;

std::mutex mtx;
std::deque<int64_t> calls;
std::deque<int64_t> vblanks;
int framesSinceShift = kShiftInterval;

int64_t floorMod(int64_t a, int64_t m) { return ((a % m) + m) % m; }

// a folded into (-m/2, m/2]
int64_t fold(int64_t a, int64_t m) {
	a = floorMod(a, m);
	return a > m / 2 ? a - m : a;
}

int64_t median(std::vector<int64_t> v) {
	std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
	return v[v.size() / 2];
}
}  // namespace

int64_t gdxsv_present_pacing_now_us() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void gdxsv_present_pacing_presented(int64_t callUs) {
	std::lock_guard<std::mutex> lock(mtx);
	calls.push_back(callUs);
	if (calls.size() > kCalls) calls.pop_front();
}

void gdxsv_present_pacing_vblank(int64_t vblankUs) {
	std::lock_guard<std::mutex> lock(mtx);
	vblanks.push_back(vblankUs);
	if (vblanks.size() > kVblanks + 1) vblanks.pop_front();
}

int64_t gdxsv_present_pacing_shift_us(int64_t periodUs) {
	if (!config::GdxPresentPacing || !ggpo::active() || !gdxsv_enabled()) return 0;
	std::lock_guard<std::mutex> lock(mtx);
	if (framesSinceShift < kShiftInterval) framesSinceShift++;
	if (framesSinceShift < kShiftInterval || calls.size() < kCalls || vblanks.size() < kVblanks + 1) return 0;
	const int64_t now = gdxsv_present_pacing_now_us();
	// A display gone quiet (minimized, mode change) has no current vblank grid.
	if (now - vblanks.back() > 100000) return 0;
	std::vector<int64_t> intervals;
	for (size_t i = 1; i < vblanks.size(); i++) intervals.push_back(vblanks[i] - vblanks[i - 1]);
	const int64_t refreshUs = median(intervals);
	if (refreshUs <= 0) return 0;
	// Phase drift of the presents against the vblanks per frame. A fast drift crosses a vblank
	// in one frame and does not dither.
	const int64_t drift = fold(periodUs, refreshUs);
	if (std::abs(drift) > refreshUs / 8) return 0;

	// The next present call, predicted from the recent ones at the limiter's rate (no jitter).
	std::vector<int64_t> next;
	for (size_t i = 0; i < calls.size(); i++) next.push_back(calls[i] + (int64_t)(calls.size() - i) * periodUs);
	const int64_t delta = fold(median(next) - vblanks.back(), refreshUs);
	FPT("ppd", delta, refreshUs, drift);
	if (std::abs(delta) >= kMarginUs) return 0;

	// Cross the vblank at once, in the direction of the drift; then the calls drift away from it.
	framesSinceShift = 0;
	calls.clear();
	return drift >= 0 ? kMarginUs - delta : -kMarginUs - delta;
}

void gdxsv_present_pacing_reset() {
	std::lock_guard<std::mutex> lock(mtx);
	calls.clear();
	vblanks.clear();
	framesSinceShift = kShiftInterval;
}
