// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// Repeat present (gdxsv:RepeatPresent): while the FixedFrequency limiter waits for the next frame, the last frame is
// presented again at every vsync. The desktop compositor then gets a frame at each of its own ticks, as from a game
// rendering at the display rate, and every new frame shows on a regular vblank. Without it a window on a display above
// 60 Hz holds frames 1 or 3 vblanks instead of 2, or drops them, because the compositor's clock is not the limiter's.
// The emulation timing does not change. DirectX 11 only, while the compositor composes the window (not in fullscreen)
// and vsync is on.

#ifdef _WIN32
// Limiter, before it sleeps: presents the last frame again if the wait has room for it (it blocks until a vsync).
// until_next_us: time to the next scheduled frame. Returns true if it presented. Render thread only.
bool gdxsv_repeat_present(int64_t until_next_us);
// Limiter: while it waits, when it wakes (steady_clock nanoseconds); 0 once awake or off. The render thread repeats
// only while the limiter's thread waits, not while it runs a rollback or the next frame.
void gdxsv_repeat_present_next_frame(int64_t steady_ns);
#else
inline bool gdxsv_repeat_present(int64_t) { return false; }
inline void gdxsv_repeat_present_next_frame(int64_t) {}
#endif
