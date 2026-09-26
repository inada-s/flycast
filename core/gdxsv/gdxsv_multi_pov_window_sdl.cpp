// Linux backend for the 4-player replay grid, through SDL (X11 and Wayland).
#include "gdxsv_multi_pov_window.h"

#include <SDL.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

static SDL_Window* Window() { return gdxsv_headless() ? nullptr : sdl_get_window(); }

bool gdxsv_multi_pov_window_available() { return Window() != nullptr; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() {
	GdxsvMultiPovWindowState state;
	SDL_Window* w = Window();
	if (w == nullptr) return state;
	state.frame = gdxsv_multi_pov_window_get_frame();
	const Uint32 flags = SDL_GetWindowFlags(w);
	if ((flags & SDL_WINDOW_FULLSCREEN) != 0)
		state.mode = GdxsvMultiPovWindowMode::Fullscreen;
	else if ((flags & SDL_WINDOW_MAXIMIZED) != 0)
		state.mode = GdxsvMultiPovWindowMode::Maximized;
	return state;
}

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state) {
	SDL_Window* w = Window();
	if (w == nullptr) return;
	if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
	gdxsv_multi_pov_window_set_frame(state.frame);
	if (state.mode == GdxsvMultiPovWindowMode::Maximized)
		SDL_MaximizeWindow(w);
	else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
		SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	GdxsvMultiPovRect out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;

	SDL_GetWindowPosition(w, &out.x, &out.y);
	SDL_GetWindowSize(w, &out.w, &out.h);
	return out;
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	SDL_Window* w = Window();
	if (w == nullptr || rect.w <= 0 || rect.h <= 0) return;

	// Size first: a window manager clamps the position against the current size.
	int cur_w = 0, cur_h = 0;
	SDL_GetWindowSize(w, &cur_w, &cur_h);
	if (cur_w != rect.w || cur_h != rect.h) SDL_SetWindowSize(w, rect.w, rect.h);

	int cur_x = 0, cur_y = 0;
	SDL_GetWindowPosition(w, &cur_x, &cur_y);
	if (cur_x != rect.x || cur_y != rect.y) SDL_SetWindowPosition(w, rect.x, rect.y);
}

bool gdxsv_multi_pov_window_is_maximized() {
	SDL_Window* w = Window();
	return w != nullptr && (SDL_GetWindowFlags(w) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
}

void gdxsv_multi_pov_window_unmaximize() {
	SDL_Window* w = Window();
	if (w == nullptr) return;
	if (SDL_SetWindowFullscreen(w, 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	SDL_RestoreWindow(w);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	GdxsvMultiPovRect out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;

	const int display = SDL_GetWindowDisplayIndex(w);
	SDL_Rect usable{};
	if (display < 0 || SDL_GetDisplayUsableBounds(display, &usable) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot read the display work area: %s", SDL_GetError());
		return out;
	}
	out.x = usable.x;
	out.y = usable.y;
	out.w = usable.w;
	out.h = usable.h;
	return out;
}

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() {
	GdxsvMultiPovRect out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;

	const int display = SDL_GetWindowDisplayIndex(w);
	SDL_Rect bounds{};
	if (display < 0 || SDL_GetDisplayBounds(display, &bounds) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot read the display bounds: %s", SDL_GetError());
		return out;
	}
	out.x = bounds.x;
	out.y = bounds.y;
	out.w = bounds.w;
	out.h = bounds.h;
	return out;
}

void gdxsv_multi_pov_window_set_topmost(bool topmost) {
	SDL_Window* w = Window();
	if (w != nullptr) SDL_SetWindowAlwaysOnTop(w, topmost ? SDL_TRUE : SDL_FALSE);
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	SDL_Window* w = Window();
	if (w == nullptr) return out;
	int top = 0, left = 0, bottom = 0, right = 0;
	// Not supported by every driver; zero insets then.
	if (SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right) != 0) return out;
	out.left = left;
	out.top = top;
	out.right = right;
	out.bottom = bottom;
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	SDL_Window* w = Window();
	if (w != nullptr) SDL_SetWindowBordered(w, borderless ? SDL_FALSE : SDL_TRUE);
}
