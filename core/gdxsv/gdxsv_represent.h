// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Re-present (gdxsv:RepresentFrames): on a display of 90 Hz or more, the render thread presents the last frame again
// at a vblank when nothing was presented for half a vblank. A window then gets a frame at about every vblank, as from a
// game rendering at the display rate: without it, a DWM-composed window on a 120 Hz display drops a frame and holds the
// next one 4 vblanks every few frames. A thread waiting for the window's display vblanks (IDXGIOutput::WaitForVBlank,
// no busy wait) wakes the render thread; a new frame is never delayed for a re-present.

#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
#define GDXSV_REPRESENT 1
// Render thread: started, stopping (before it presents its last frame, so before the GUI shows).
void gdxsv_represent_start(void (*wake)());
void gdxsv_represent_stop();
// Render thread, after each present (a new frame or a re-present).
void gdxsv_represent_presented();
// Render thread, woken while waiting for a frame: true once if a vblank asked for a re-present.
bool gdxsv_represent_due();
// The window's display is fast enough: the frames are re-presented.
bool gdxsv_represent_active();
#else
inline void gdxsv_represent_start(void (*)()) {}
inline void gdxsv_represent_stop() {}
inline void gdxsv_represent_presented() {}
inline bool gdxsv_represent_due() { return false; }
inline bool gdxsv_represent_active() { return false; }
#endif
