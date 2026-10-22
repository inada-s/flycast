// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Present pacing from measured display timing (rollback battles).
// The renderer reports each present call and the vblanks of the display that shows the window. The frame
// limiter keeps its own rate (59.94 Hz) and only moves its phase, once, when the present calls drift
// to a vblank; there, jitter would put consecutive frames on both sides of it and show some twice.
// Times are std::chrono::steady_clock microseconds.
#pragma once
#include <cstdint>

int64_t gdxsv_present_pacing_now_us();
// UI thread: a present call at callUs.
void gdxsv_present_pacing_presented(int64_t callUs);
// Any thread: a vblank of the display that shows the window.
void gdxsv_present_pacing_vblank(int64_t vblankUs);
// Limiter thread: extra wait in us (may be negative) for the frame period periodUs.
int64_t gdxsv_present_pacing_shift_us(int64_t periodUs);
void gdxsv_present_pacing_reset();
