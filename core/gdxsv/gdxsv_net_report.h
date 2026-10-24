// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdint>

// Timing statistics of a rollback battle for P2PMatchingReport: rollbacks and
// their time per frame, Present and input intervals, late Presents by the depth
// of the rollback before them, and per emulation-thread
// frame its work, the time left for a rollback and its waits. Recorded while a
// GGPO session runs (the recording hooks are in gdxsv_ggpo_hooks.h and
// gdxsv_emu_hooks.h) in fixed-size histograms, without locks or allocation;
// those of the last session are kept until the next one or a reset.

namespace proto {
class P2PMatchingReport;
}

// Drops the samples of the last session.
void gdxsv_net_report_reset();
// A frame was presented (vsync blocking included).
void gdxsv_net_report_present();
// DXGI frame statistics after a Present: the swap chain Present count, the last Present shown, the vsync it was
// shown at, the last vsync sampled and its time (ns), and whether DWM composed it. The next
// gdxsv_net_report_present counts the late images of Presents not composed.
void gdxsv_net_report_display(uint32_t presentCount, uint32_t displayedPresent, uint32_t displayedRefresh, uint32_t syncRefresh,
							  int64_t syncNs, bool composed);
// The frame limiter was called at `called` and woke at `woke`; a frame ends at the call.
void gdxsv_net_report_wake(std::chrono::steady_clock::time_point called, std::chrono::steady_clock::time_point woke, int64_t periodUs);
// The frame limiter did not run for this frame (menu open, fast forward, off).
void gdxsv_net_report_limiter_off();
// Sets the timing fields of the report.
void gdxsv_net_report_fill(proto::P2PMatchingReport* report);
