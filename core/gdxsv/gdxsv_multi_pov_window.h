#pragma once
#include "gdxsv_multi_pov.h"

// Window control for the 4-player replay grid: the host's window is the
// top-left quadrant, the grid is twice its size, and the guests take the other
// three quadrants.
//
// The per-platform functions below (Win32, Cocoa, SDL for Linux, or a stub
// that reports unavailable) work in the window's content rect, in desktop
// coordinates with the origin at the top-left of the primary display.

// False when there is no window to move (headless, or a platform without
// desktop windows).
bool gdxsv_multi_pov_window_available();

enum class GdxsvMultiPovWindowMode {
	Windowed,
	Maximized,
	Fullscreen,
	NativeFullscreen, // macOS green button / Globe+F, without SDL fullscreen's menu restrictions.
};

struct GdxsvMultiPovWindowState {
	GdxsvMultiPovRect frame;
	GdxsvMultiPovWindowMode mode = GdxsvMultiPovWindowMode::Windowed;
};

// Snapshot the current presentation. Before tiling, the host replaces frame
// with the normal window rectangle revealed by leaving maximized/fullscreen.
// Restore that rectangle before re-entering the saved mode.
GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state();
void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state);

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame();

// Moves and resizes without raising or focusing the window.
void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect);

// Includes native fullscreen: both modes must be left before tiling.
bool gdxsv_multi_pov_window_is_maximized();
void gdxsv_multi_pov_window_unmaximize();

// The display's area less task bar, dock or menu bar.
GdxsvMultiPovRect gdxsv_multi_pov_window_work_area();

// The whole display.
GdxsvMultiPovRect gdxsv_multi_pov_window_display_area();

// Keeps the window above all others (the task bar would otherwise cover the
// bottom row of a full-screen grid).
void gdxsv_multi_pov_window_set_topmost(bool topmost);

void gdxsv_multi_pov_window_set_borderless(bool borderless);

// The window frame around the content area, in pixels per side.
struct GdxsvMultiPovInsets {
	int32_t left = 0, top = 0, right = 0, bottom = 0;
};
GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets();

// Runs every frame on the UI thread: the host publishes the grid, the guests
// place themselves in it. No-op outside a session.
void gdxsv_multi_pov_window_tick();

// Alt+Enter / F11 in a session: the host toggles the grid between its window
// layout and full screen (four borderless quadrants over the whole display).
// Returns true when the key was consumed, including on a guest. False outside
// a session.
bool gdxsv_multi_pov_toggle_fullscreen();
