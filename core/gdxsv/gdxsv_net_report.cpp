// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_net_report.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <utility>

#include "gdxsv.pb.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_ggpo_hooks.h"
#include "network/ggpo.h"

std::atomic<bool> gdxsv_net_report_on{false};

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFramePeriodUs = 16683;  // 59.94 Hz
constexpr int kOffPeriodUs = 2000;     // an interval further than this from the frame period is off period
constexpr int kDepthBuckets = 8;	   // rollbacks of 0 to 6 frames; the last bucket: 7 or more
constexpr int kMaxTimesyncFrames = 32;
constexpr int kWaits = static_cast<int>(GdxsvWait::Count);

// Each counter has one writer thread; the report reads them from another
// thread. A relaxed load and store compiles to plain moves, unlike fetch_add.
void Bump(std::atomic<int32_t>& c, int32_t d = 1) { c.store(c.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }
int32_t Get(const std::atomic<int32_t>& c) { return c.load(std::memory_order_relaxed); }
void Set(std::atomic<int32_t>& c, int32_t v) { c.store(v, std::memory_order_relaxed); }

int32_t ClampUs(int64_t us) { return static_cast<int32_t>(std::clamp<int64_t>(us, INT32_MIN, INT32_MAX)); }
int32_t SinceUs(Clock::time_point t, Clock::time_point now) {
	return ClampUs(std::chrono::duration_cast<std::chrono::microseconds>(now - t).count());
}

// Fixed-size log-linear histogram of signed microseconds: magnitudes below kSub
// are exact, above them each power of two has kSub buckets (width under 1/kSub
// of the value). count, sum, min, max and off_period are exact.
class Hist {
   public:
	void Reset() {
		for (auto& b : pos_) Set(b, 0);
		for (auto& b : neg_) Set(b, 0);
		Set(count_, 0);
		Set(off_, 0);
		sum_.store(0, std::memory_order_relaxed);
		Set(min_, INT32_MAX);
		Set(max_, INT32_MIN);
	}

	// periodUs 0: off_period is not counted.
	void Add(int32_t us, int periodUs) {
		const int64_t v = us;
		const uint32_t m = static_cast<uint32_t>(v < 0 ? -v : v);
		Bump(v < 0 ? neg_[Bucket(m)] : pos_[Bucket(m)]);
		Bump(count_);
		sum_.store(sum_.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
		if (us < Get(min_)) Set(min_, us);
		if (Get(max_) < us) Set(max_, us);
		if (periodUs != 0 && kOffPeriodUs < std::abs(v - periodUs)) Bump(off_);
	}

	int32_t Count() const { return Get(count_); }

	void Fill(proto::TimingStats* out) const {
		std::array<int32_t, kBuckets> pos, neg;
		int64_t n = 0;
		for (int i = 0; i < kBuckets; i++) {
			pos[i] = Get(pos_[i]);
			neg[i] = Get(neg_[i]);
			n += pos[i] + neg[i];
		}
		if (n == 0) return;
		const int32_t min = Get(min_), max = Get(max_);
		out->set_count(Get(count_));
		out->set_mean_us(static_cast<int32_t>(sum_.load(std::memory_order_relaxed) / std::max(1, Get(count_))));
		out->set_min_us(min);
		out->set_max_us(max);
		out->set_off_period(Get(off_));
		// Nearest rank, the bucket's middle, within [min, max].
		auto percentile = [&](double p) {
			const int64_t k = std::min<int64_t>(n - 1, std::max<int64_t>(0, static_cast<int64_t>(std::ceil(p * n)) - 1));
			int64_t seen = 0;
			for (int i = kBuckets - 1; 0 <= i; i--) {
				seen += neg[i];
				if (k < seen) return std::clamp(ClampUs(-Middle(i)), min, max);
			}
			for (int i = 0; i < kBuckets; i++) {
				seen += pos[i];
				if (k < seen) return std::clamp(ClampUs(Middle(i)), min, max);
			}
			return max;
		};
		out->set_p1_us(percentile(0.01));
		out->set_p5_us(percentile(0.05));
		out->set_p50_us(percentile(0.50));
		out->set_p99_us(percentile(0.99));
	}

   private:
	static constexpr int kSubBits = 5;
	static constexpr int kSub = 1 << kSubBits;
	static constexpr int kBuckets = kSub * (32 - kSubBits + 1);

	static int Bucket(uint32_t m) {
		if (m < kSub) return static_cast<int>(m);
		int e = kSubBits;  // the highest set bit
		while (e < 31 && (m >> (e + 1))) e++;
		return (e - kSubBits + 1) * kSub + static_cast<int>((m >> (e - kSubBits)) - kSub);
	}
	static int64_t Middle(int bucket) {
		if (bucket < kSub) return bucket;
		const int shift = bucket / kSub - 1;
		const int64_t lo = static_cast<int64_t>(kSub + bucket % kSub) << shift;
		return lo + ((int64_t{1} << shift) >> 1);
	}

	std::array<std::atomic<int32_t>, kBuckets> pos_{};
	std::array<std::atomic<int32_t>, kBuckets> neg_{};
	std::atomic<int32_t> count_{0};
	std::atomic<int32_t> off_{0};
	std::atomic<int64_t> sum_{0};
	std::atomic<int32_t> min_{INT32_MAX};
	std::atomic<int32_t> max_{INT32_MIN};
};

// An interval series: the time between two events.
struct Intervals {
	Clock::time_point last{};
	Hist hist;

	void Reset() {
		last = {};
		hist.Reset();
	}
	// Returns the interval, -1 for the first event.
	int32_t Add(Clock::time_point now) {
		int32_t us = -1;
		if (last != Clock::time_point{}) {
			us = SinceUs(last, now);
			hist.Add(us, kFramePeriodUs);
		}
		last = now;
		return us;
	}
};

// Least squares of a frame's Present wait (y) on the previous frame's rollback work (x). A vsync wait that the
// next frame's work absorbs shrinks by that work: slope -1. A GPU or swap wait does not: slope 0.
struct PresentWaitFit {
	static constexpr int kMinRollbacks = 16;  // frames with rollback work before the slope is used
	double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
	int rollbacks = 0;

	void Add(int64_t x, int64_t y) {
		n++;
		sx += x;
		sy += y;
		sxx += double(x) * x;
		sxy += double(x) * y;
		if (x > 0) rollbacks++;
	}
	// Minus the slope, per mille, 0 to 1000; -1 while there are too few rollbacks to tell.
	int32_t ElasticityPm() const {
		const double d = n * sxx - sx * sx;
		if (rollbacks < kMinRollbacks || d <= 0) return -1;
		return static_cast<int32_t>(std::clamp(-1000 * (n * sxy - sx * sy) / d, 0.0, 1000.0));
	}
};

// Writers: the emulation thread, except present (the UI thread with threaded
// rendering).
struct NetReport {
	Intervals inputRead;
	Intervals inputSend;
	Intervals present;
	Hist rollbackFrame;	 // per rollback: its time / the frames it re-ran
	Clock::time_point pendingRead{};
	std::atomic<int32_t> rollbackCount{0};
	std::array<std::atomic<int32_t>, kDepthBuckets> depthHist{};
	Clock::time_point rollbackStart{};
	Clock::duration rollbackWaitAtStart{};	// the frame's rollback wait when the running rollback started
	int rollbackToFrame = -1;  // the frame a running rollback ends at; -1: none runs
	int rollbackReruns = 0;	   // frames the running rollback re-ran so far
	std::atomic<int32_t> rollbackMaxUs{0};
	// The emulation-thread frame: from a limiter wake to the next limiter call.
	Clock::time_point lastWake{};					 // none: the next frame is not measured
	int64_t rollbackUs = 0;							 // rollback time in this frame
	std::array<Clock::duration, kWaits> waitTime{};	 // per wait kind, in this frame
	std::array<bool, kWaits> waited{};
	std::atomic<int32_t> periodUs{0};
	Hist work;
	Hist rollbackSlack;
	std::array<Hist, kWaits> waits;
	PresentWaitFit presentWaitFit;
	int64_t prevRollbackWork = -1;	// of the last measured frame; -1: none
	std::atomic<int32_t> elasticityPm{-1};
	// Deepest rollback since the last Present. Written by the emulation thread, taken by the Present thread: a
	// rollback ending during the exchange may count for the next Present instead.
	std::atomic<int32_t> depthSincePresent{0};
	std::array<std::atomic<int32_t>, kDepthBuckets> presentDepthHist{}, lateDepthHist{}, droppedDepthHist{};
	// DXGI frame statistics of the last Present (the Present thread). Frame periods an image stayed too long, by the
	// depth bucket of the Present that replaced it: what the display really missed. Counted from the first input
	// read: the session start before it can hold the picture for seconds.
	std::atomic<int32_t> inputSeen{0};
	struct DisplayStats {
		uint32_t present, displayed, refresh, syncRefresh;
		int64_t syncNs;
	};
	DisplayStats pendingDisplay{}, lastDisplay{}, firstSync{};
	bool hasPendingDisplay = false, hasLastDisplay = false, hasFirstSync = false;
	std::array<uint8_t, 64> presentBucket{};
	std::atomic<int32_t> displayStatsPresents{0}, displayComposedPresents{0};
	std::atomic<int32_t> vsyncPeriodNs{0};	// measured from the sampled vsyncs; 0 until enough of them
	std::array<std::atomic<int32_t>, kDepthBuckets> displayLateDepthHist{};

	void ResetFrame(Clock::time_point wake) {
		lastWake = wake;
		rollbackUs = 0;
		waitTime.fill({});
		waited.fill(false);
	}
	std::atomic<int32_t> late15{0}, late2{0}, dropped{0};
	std::atomic<int32_t> timesyncCount{0};
	std::array<std::atomic<int32_t>, kMaxTimesyncFrames> timesyncFrames{};

	void Reset() {
		inputRead.Reset();
		inputSend.Reset();
		present.Reset();
		rollbackFrame.Reset();
		pendingRead = {};
		Set(rollbackCount, 0);
		for (auto& c : depthHist) Set(c, 0);
		rollbackStart = {};
		rollbackToFrame = -1;
		rollbackReruns = 0;
		Set(rollbackMaxUs, 0);
		ResetFrame({});
		Set(periodUs, 0);
		work.Reset();
		rollbackSlack.Reset();
		for (auto& h : waits) h.Reset();
		presentWaitFit = {};
		prevRollbackWork = -1;
		Set(elasticityPm, -1);
		Set(depthSincePresent, 0);
		for (auto* hist : {&presentDepthHist, &lateDepthHist, &droppedDepthHist, &displayLateDepthHist})
			for (auto& c : *hist) Set(c, 0);
		Set(late15, 0);
		Set(late2, 0);
		Set(dropped, 0);
		hasPendingDisplay = hasLastDisplay = hasFirstSync = false;
		Set(vsyncPeriodNs, 0);
		Set(displayStatsPresents, 0);
		Set(displayComposedPresents, 0);
		Set(inputSeen, 0);
		Set(timesyncCount, 0);
		for (auto& c : timesyncFrames) Set(c, 0);
	}
};

NetReport report;

int DepthBucket(int depth) { return std::clamp(depth, 0, kDepthBuckets - 1); }

}  // namespace

void gdxsv_net_report_session_start() {
	report.Reset();
	gdxsv_net_report_on = true;
}

void gdxsv_net_report_session_stop() { gdxsv_net_report_on = false; }

void gdxsv_net_report_reset() { report.Reset(); }

void gdxsv_net_report_rollback_begin(int toFrame) {
	report.rollbackStart = Clock::now();
	report.rollbackWaitAtStart = report.waitTime[static_cast<int>(GdxsvWait::Rollback)];
	report.rollbackToFrame = toFrame;
	report.rollbackReruns = 0;
}

void gdxsv_net_report_rerun_end(int frame) {
	if (report.rollbackToFrame < 0) return;
	report.rollbackReruns++;
	if (frame + 1 < report.rollbackToFrame) return;
	const int depth = report.rollbackReruns;
	Bump(report.rollbackCount);
	Bump(report.depthHist[DepthBucket(depth)]);
	if (Get(report.depthSincePresent) < depth) Set(report.depthSincePresent, depth);
	const int32_t us = SinceUs(report.rollbackStart, Clock::now());
	if (Get(report.rollbackMaxUs) < us) Set(report.rollbackMaxUs, us);
	report.rollbackUs += us;
	// Threaded rendering: the wait for the render thread to release VRAM is not re-run time.
	const int64_t waitUs = std::chrono::duration_cast<std::chrono::microseconds>(
							   report.waitTime[static_cast<int>(GdxsvWait::Rollback)] - report.rollbackWaitAtStart)
							   .count();
	report.rollbackFrame.Add(ClampUs((us - waitUs) / depth), 0);
	report.rollbackToFrame = -1;
}

void gdxsv_net_report_input_read() {
	report.pendingRead = Clock::now();
	Set(report.inputSeen, 1);
}

void gdxsv_net_report_input_sent() {
	report.inputRead.Add(report.pendingRead);
	report.inputSend.Add(Clock::now());
}

void gdxsv_net_report_timesync() {
	int frame = -1;
	ggpo::getCurrentFrame(&frame);
	const int i = Get(report.timesyncCount);
	if (i < kMaxTimesyncFrames) {
		Set(report.timesyncFrames[i], frame);
		Set(report.timesyncCount, i + 1);
	}
}

void gdxsv_net_report_display(uint32_t presentCount, uint32_t displayedPresent, uint32_t displayedRefresh, uint32_t syncRefresh,
							  int64_t syncNs, bool composed) {
	if (!gdxsv_net_report_on || !Get(report.inputSeen)) return;
	if (composed) {
		Bump(report.displayComposedPresents);
		return;
	}
	report.pendingDisplay = {presentCount, displayedPresent, displayedRefresh, syncRefresh, syncNs};
	report.hasPendingDisplay = true;
}

namespace {
// The vsync period, from the first and the last sampled vsync: after 120 vsyncs it is within a few ns.
constexpr uint32_t kMinVsyncsForPeriod = 120;

// The statistics name the last Present shown and the vsync it was shown at. Between two shown Presents, the old
// image stayed (vsyncs between them) * (vsync period). An image that stayed over 1.5 frame periods counts the periods
// past the first, rounded, whatever the display's refresh rate (at 144 Hz an image normally stays 2 or 3 vsyncs).
// Comparing with the Presents between them instead would hide a late Present followed by one that never showed.
void CountDisplayMisses(int bucket) {
	NetReport& r = report;
	if (!r.hasPendingDisplay) {
		r.hasLastDisplay = false;
		return;
	}
	r.hasPendingDisplay = false;
	const NetReport::DisplayStats& d = r.pendingDisplay;
	r.presentBucket[d.present % r.presentBucket.size()] = bucket;
	Bump(r.displayStatsPresents);
	// A new swap chain starts its counts again.
	if (!r.hasFirstSync || d.syncRefresh < r.firstSync.syncRefresh || d.syncNs <= r.firstSync.syncNs) {
		r.firstSync = d;
		r.hasFirstSync = true;
	} else if (kMinVsyncsForPeriod <= d.syncRefresh - r.firstSync.syncRefresh) {
		const int64_t ns = (d.syncNs - r.firstSync.syncNs) / (d.syncRefresh - r.firstSync.syncRefresh);
		Set(r.vsyncPeriodNs, static_cast<int32_t>(std::clamp<int64_t>(ns, 0, INT32_MAX)));
	}
	const int64_t vsyncNs = Get(r.vsyncPeriodNs);
	const NetReport::DisplayStats& last = r.lastDisplay;
	if (vsyncNs != 0 && r.hasLastDisplay && d.displayed != last.displayed && d.present - d.displayed < r.presentBucket.size() &&
		d.displayed - last.displayed < r.presentBucket.size()) {
		const int64_t heldNs = static_cast<int64_t>(d.refresh - last.refresh) * vsyncNs;
		const int64_t periodNs = int64_t{kFramePeriodUs} * 1000;
		if (periodNs * 3 / 2 < heldNs)
			Bump(r.displayLateDepthHist[r.presentBucket[d.displayed % r.presentBucket.size()]],
				 static_cast<int32_t>(std::min<int64_t>((heldNs - periodNs / 2) / periodNs, 1000)));
	}
	if (!r.hasLastDisplay || d.displayed != last.displayed) r.lastDisplay = d;
	r.hasLastDisplay = true;
}
}  // namespace

void gdxsv_net_report_present() {
	if (!gdxsv_net_report_on) return;
	const int32_t us = report.present.Add(Clock::now());
	const int bucket = DepthBucket(report.depthSincePresent.exchange(0, std::memory_order_relaxed));
	CountDisplayMisses(bucket);
	if (us < 0) return;
	Bump(report.presentDepthHist[bucket]);
	if (kFramePeriodUs * 3 / 2 < us) {
		Bump(report.late15);
		Bump(report.lateDepthHist[bucket]);
	}
	if (kFramePeriodUs * 2 < us) Bump(report.late2);
	// An interval of n periods shows the last frame for n - 1 more vsyncs.
	const int32_t dropped = std::max(0, (us + kFramePeriodUs / 2) / kFramePeriodUs - 1);
	Bump(report.dropped, dropped);
	Bump(report.droppedDepthHist[bucket], dropped);
}

int64_t gdxsv_net_report_ticks() { return Clock::now().time_since_epoch().count(); }

void gdxsv_net_report_wait(GdxsvWait wait, int64_t beginTicks) {
	const int i = static_cast<int>(wait);
	report.waitTime[i] += Clock::now() - Clock::time_point(Clock::duration(beginTicks));
	report.waited[i] = true;
}

void gdxsv_net_report_limiter_off() { report.lastWake = {}; }

void gdxsv_net_report_wake(Clock::time_point called, Clock::time_point woke, int64_t periodUs) {
	NetReport& r = report;
	// Before the first input, the frame includes the session setup.
	if (gdxsv_net_report_on && r.pendingRead != Clock::time_point{} && r.lastWake != Clock::time_point{}) {
		const int32_t period = ClampUs(periodUs);
		const int32_t busy = SinceUs(r.lastWake, called);
		int32_t waited = 0;
		for (int i = 0; i < kWaits; i++) {
			if (!r.waited[i]) continue;
			const int32_t us = ClampUs(std::chrono::duration_cast<std::chrono::microseconds>(r.waitTime[i]).count());
			r.waits[i].Add(us, 0);
			waited += us;
		}
		// A rollback's VRAM wait is in its time, but it is a wait, not rollback work.
		const int64_t rollbackWork =
			r.rollbackUs - std::chrono::duration_cast<std::chrono::microseconds>(r.waitTime[static_cast<int>(GdxsvWait::Rollback)]).count();
		Set(r.periodUs, period);
		r.work.Add(busy - waited, 0);
		// The elastic part of the Present wait is time a rollback could use; the rest is lost.
		const int i = static_cast<int>(GdxsvWait::Present);
		const int64_t presentWait = r.waited[i] ? std::chrono::duration_cast<std::chrono::microseconds>(r.waitTime[i]).count() : 0;
		if (r.waited[i] && 0 <= r.prevRollbackWork) {
			r.presentWaitFit.Add(r.prevRollbackWork, presentWait);
			Set(r.elasticityPm, r.presentWaitFit.ElasticityPm());
		}
		const int64_t elasticPresentWait = presentWait * std::max(0, Get(r.elasticityPm)) / 1000;
		r.rollbackSlack.Add(ClampUs(period - (busy - rollbackWork - elasticPresentWait)), 0);
		r.prevRollbackWork = rollbackWork;
	} else {
		r.prevRollbackWork = -1;
	}
	r.ResetFrame(woke);
}

void gdxsv_net_report_fill(proto::P2PMatchingReport* out) {
	const NetReport& r = report;
	out->set_rollback_count(Get(r.rollbackCount));
	for (const auto& c : r.depthHist) out->add_rollback_depth_hist(Get(c));
	out->set_rollback_max_us(Get(r.rollbackMaxUs));
	r.rollbackFrame.Fill(out->mutable_rollback_frame_time());

	out->set_late_frames_1_5(Get(r.late15));
	out->set_late_frames_2(Get(r.late2));
	out->set_dropped_frames(Get(r.dropped));
	for (const auto& c : r.presentDepthHist) out->add_present_depth_hist(Get(c));
	for (const auto& c : r.lateDepthHist) out->add_late_present_depth_hist(Get(c));
	for (const auto& c : r.droppedDepthHist) out->add_dropped_depth_hist(Get(c));
	out->set_display_stats_presents(Get(r.displayStatsPresents));
	out->set_display_composed_presents(Get(r.displayComposedPresents));
	for (const auto& c : r.displayLateDepthHist) out->add_display_late_depth_hist(Get(c));
	out->set_display_refresh_period_ns(Get(r.vsyncPeriodNs));

	const int timesyncs = std::min(Get(r.timesyncCount), kMaxTimesyncFrames);
	for (int i = 0; i < timesyncs; i++) out->add_timesync_frames(Get(r.timesyncFrames[i]));
	r.inputRead.hist.Fill(out->mutable_input_read_interval());
	r.inputSend.hist.Fill(out->mutable_input_send_interval());
	r.present.hist.Fill(out->mutable_present_interval());
	if (r.work.Count() == 0) return;
	auto* ft = out->mutable_frame_timing();
	ft->set_period_us(Get(r.periodUs));
	r.work.Fill(ft->mutable_work());
	r.rollbackSlack.Fill(ft->mutable_rollback_slack());
	ft->set_present_wait_elasticity(Get(r.elasticityPm));
	using W = GdxsvWait;
	const std::pair<W, proto::TimingStats* (proto::FrameTiming::*)()> waits[] = {
		{W::Present, &proto::FrameTiming::mutable_present_wait},
		{W::Render, &proto::FrameTiming::mutable_render_wait},
		{W::Rollback, &proto::FrameTiming::mutable_rollback_wait},
		{W::Queue, &proto::FrameTiming::mutable_queue_wait},
	};
	for (const auto& [wait, field] : waits) {
		const Hist& h = r.waits[static_cast<int>(wait)];
		if (h.Count() != 0) h.Fill((ft->*field)());
	}
}
