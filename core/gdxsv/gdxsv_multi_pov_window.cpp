// Platform-independent part of the 4-player replay grid.
#include "gdxsv_multi_pov_window.h"

#include <optional>

#include "log/LogManager.h"
#include "types.h"
#ifdef USE_SDL
#include "sdl/sdl.h"
#endif

// Host: last published layout, so the generation only moves when it changed.
static uint32_t g_generation = 0;
static GdxsvMultiPovRect g_last_group;
static bool g_last_maximized = false;
static bool g_last_fullscreen = false;

// Guest: the generation already applied.
static uint32_t g_applied_generation = 0;
static bool g_borderless_done = false;
static bool g_guest_topmost = false;

// Host: the first tick turns the full-size window into a quadrant.
static bool g_host_placed = false;
static std::optional<GdxsvMultiPovWindowState> g_host_window_restore;

// Host: full-screen grid, and the quadrant to restore when leaving it.
static bool g_fullscreen = false;
static GdxsvMultiPovRect g_fullscreen_restore;

static void SaveHostWindow() {
	if (!g_host_window_restore) {
		g_host_window_restore = gdxsv_multi_pov_window_get_state();
#ifdef USE_SDL
		sdl_preserve_window_state(true);
#endif
	}
}

static bool UnmaximizeHostWindow() {
	if (gdxsv_multi_pov_window_is_maximized()) {
		gdxsv_multi_pov_window_unmaximize();
		if (gdxsv_multi_pov_window_is_maximized()) return false;
	}
	// Capture the normal rectangle before the first quadrant resize. Later
	// grid maximizes/fullscreen toggles must not replace it with a quadrant.
	if (!g_host_placed)
		g_host_window_restore->frame = gdxsv_multi_pov_window_get_frame();
	return true;
}

static bool FitsWithin(const GdxsvMultiPovRect& inner, const GdxsvMultiPovRect& outer) {
	if (outer.w <= 0 || outer.h <= 0) return true;
	return outer.x <= inner.x && outer.y <= inner.y && inner.x + inner.w <= outer.x + outer.w &&
		   inner.y + inner.h <= outer.y + outer.h;
}

// The work area less the host's title bar and left border, so the host's frame
// stays on screen when the grid covers the work area.
static GdxsvMultiPovRect HostGridArea() {
	GdxsvMultiPovRect area = gdxsv_multi_pov_window_work_area();
	const GdxsvMultiPovInsets insets = gdxsv_multi_pov_window_frame_insets();
	area.x += insets.left;
	area.w -= insets.left;
	area.y += insets.top;
	area.h -= insets.top;
	return area;
}

static void TickHost() {
	SaveHostWindow();
	GdxsvMultiPovHostWindow hw;

	if (g_fullscreen) {
		// The grid tiles the whole display; the host's quadrant is re-applied
		// if anything moved it.
		const GdxsvMultiPovRect area = gdxsv_multi_pov_window_display_area();
		GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
		gdxsv_multi_pov_compute_grid(area, quadrants);
		if (gdxsv_multi_pov_window_get_frame() != quadrants[0]) gdxsv_multi_pov_window_set_frame(quadrants[0]);
		hw.group = area;
		hw.maximized = false;
		hw.fullscreen = true;
	} else if (gdxsv_multi_pov_window_is_maximized() ||
			   (!g_host_placed && g_host_window_restore->mode != GdxsvMultiPovWindowMode::Windowed)) {
		// The grid tiles the work area; the host leaves the maximized state
		// for its quadrant. Keep this path if leaving that state takes a tick.
		const GdxsvMultiPovRect area = HostGridArea();
		GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
		gdxsv_multi_pov_compute_grid(area, quadrants);
		if (!UnmaximizeHostWindow()) return;
		gdxsv_multi_pov_window_set_frame(quadrants[0]);
		hw.group = area;
		hw.maximized = true;
	} else {
		// The grid is twice the host's window.
		const GdxsvMultiPovRect frame = gdxsv_multi_pov_window_get_frame();
		hw.group = {frame.x, frame.y, frame.w * 2, frame.h * 2};
		hw.maximized = false;

		if (!g_host_placed) {
			// First tick: a grid twice the host's full-size window usually
			// runs off the display, so lay it over the work area instead.
			const GdxsvMultiPovRect area = HostGridArea();
			if (!FitsWithin(hw.group, area)) {
				hw.group = area;
				GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
				gdxsv_multi_pov_compute_grid(hw.group, quadrants);
				gdxsv_multi_pov_window_set_frame(quadrants[0]);
				NOTICE_LOG(COMMON, "multi-pov: grid laid out over the work area %dx%d", area.w, area.h);
			}
		}
	}
	g_host_placed = true;
	hw.rect = gdxsv_multi_pov_window_get_frame();

	if (hw.group != g_last_group || hw.maximized != g_last_maximized || hw.fullscreen != g_last_fullscreen) {
		g_last_group = hw.group;
		g_last_maximized = hw.maximized;
		g_last_fullscreen = hw.fullscreen;
		++g_generation;
	}
	hw.generation = g_generation;

	// Every frame, so a guest that is still booting reads the current layout.
	gdxsv_multi_pov_publish_host_window(hw);
}

static void TickGuest() {
	const int screen = gdxsv_multi_pov_screen_index();
	if (screen < 1 || kGdxsvMultiPovScreens <= screen) return;

	if (!g_borderless_done) {
		gdxsv_multi_pov_window_set_borderless(true);
		g_borderless_done = true;
	}

	GdxsvMultiPovHostWindow hw;
	if (!gdxsv_multi_pov_read_host_window(hw)) return;
	if (hw.generation == g_applied_generation) return;
	g_applied_generation = hw.generation;

	if (hw.fullscreen != g_guest_topmost) {
		g_guest_topmost = hw.fullscreen;
		gdxsv_multi_pov_window_set_topmost(hw.fullscreen);
	}

	GdxsvMultiPovRect quadrants[kGdxsvMultiPovScreens];
	gdxsv_multi_pov_compute_grid(hw.group, quadrants);
	gdxsv_multi_pov_window_set_frame(quadrants[screen]);
}

static void LeaveFullscreen() {
	g_fullscreen = false;
	gdxsv_multi_pov_window_set_topmost(false);
	gdxsv_multi_pov_window_set_borderless(false);
	gdxsv_multi_pov_window_set_frame(g_fullscreen_restore);
}

bool gdxsv_multi_pov_toggle_fullscreen() {
	const GdxsvMultiPovRole role = gdxsv_multi_pov_current_role();
	if (role == GdxsvMultiPovRole::None) return false;
	if (!gdxsv_multi_pov_window_available()) return false;
	// Guests have no controls of their own.
	if (role == GdxsvMultiPovRole::Guest) return true;

	SaveHostWindow();
	if (g_fullscreen) {
		LeaveFullscreen();
		NOTICE_LOG(COMMON, "multi-pov: grid left full screen");
	} else {
		if (!UnmaximizeHostWindow()) return true;
		g_fullscreen_restore = gdxsv_multi_pov_window_get_frame();
		gdxsv_multi_pov_window_set_borderless(true);
		gdxsv_multi_pov_window_set_topmost(true);
		g_fullscreen = true;
		NOTICE_LOG(COMMON, "multi-pov: grid full screen");
	}
	return true;
}

void gdxsv_multi_pov_window_tick() {
	const GdxsvMultiPovRole role = gdxsv_multi_pov_current_role();
	if (!gdxsv_multi_pov_window_available()) return;

	if (role == GdxsvMultiPovRole::None) {
		// Session over: undo the temporary grid layout on the UI thread.
		if (g_fullscreen) LeaveFullscreen();
		if (g_host_window_restore) {
			gdxsv_multi_pov_window_restore_state(*g_host_window_restore);
			g_host_window_restore.reset();
#ifdef USE_SDL
			sdl_preserve_window_state(false);
#endif
		}
		g_host_placed = false;
		return;
	}

	if (role == GdxsvMultiPovRole::Host)
		TickHost();
	else
		TickGuest();
}
