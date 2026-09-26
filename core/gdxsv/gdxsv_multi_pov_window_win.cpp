// Win32 backend for the 4-player replay grid. Native rather than SDL so a
// guest can be moved without being activated (SWP_NOACTIVATE).
#include "gdxsv_multi_pov_window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

// Defined in core/sdl/sdl.cpp.
HWND getNativeHwnd();

static HWND Hwnd() { return gdxsv_headless() ? nullptr : getNativeHwnd(); }

// The window rect that produces the wanted client rect.
static RECT ClientToWindowRect(HWND hwnd, const GdxsvMultiPovRect& client) {
	RECT r{client.x, client.y, client.x + client.w, client.y + client.h};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
	AdjustWindowRectEx(&r, style, FALSE, ex_style);
	return r;
}

bool gdxsv_multi_pov_window_available() { return Hwnd() != nullptr; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() {
	GdxsvMultiPovWindowState state;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return state;
	state.frame = gdxsv_multi_pov_window_get_frame();
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0)
		state.mode = GdxsvMultiPovWindowMode::Fullscreen;
	else if (IsZoomed(hwnd))
		state.mode = GdxsvMultiPovWindowMode::Maximized;
	return state;
}

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
	gdxsv_multi_pov_window_set_frame(state.frame);
	if (state.mode == GdxsvMultiPovWindowMode::Maximized)
		ShowWindow(hwnd, SW_MAXIMIZE);
	else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
		SDL_SetWindowFullscreen(sdl_get_window(), SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	RECT client{};
	POINT origin{0, 0};
	if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) return out;
	out.x = origin.x;
	out.y = origin.y;
	out.w = client.right - client.left;
	out.h = client.bottom - client.top;
	return out;
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr || rect.w <= 0 || rect.h <= 0) return;

	const RECT r = ClientToWindowRect(hwnd, rect);
	SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER);
}

bool gdxsv_multi_pov_window_is_maximized() {
	HWND hwnd = Hwnd();
	return hwnd != nullptr && (IsZoomed(hwnd) ||
		(SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0);
}

void gdxsv_multi_pov_window_unmaximize() {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	if (SDL_SetWindowFullscreen(sdl_get_window(), 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	// SW_RESTORE keeps the window on the display it was maximized on.
	ShowWindow(hwnd, SW_RESTORE);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfo(monitor, &info)) return out;

	out.x = info.rcWork.left;
	out.y = info.rcWork.top;
	out.w = info.rcWork.right - info.rcWork.left;
	out.h = info.rcWork.bottom - info.rcWork.top;
	return out;
}

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() {
	GdxsvMultiPovRect out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfo(monitor, &info)) return out;

	out.x = info.rcMonitor.left;
	out.y = info.rcMonitor.top;
	out.w = info.rcMonitor.right - info.rcMonitor.left;
	out.h = info.rcMonitor.bottom - info.rcMonitor.top;
	return out;
}

void gdxsv_multi_pov_window_set_topmost(bool topmost) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;
	SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return out;

	// AdjustWindowRectEx on an empty rect yields the frame alone.
	RECT r{0, 0, 0, 0};
	const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
	const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
	if (!AdjustWindowRectEx(&r, style, FALSE, ex_style)) return out;
	out.left = -r.left;
	out.top = -r.top;
	out.right = r.right;
	out.bottom = r.bottom;
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	HWND hwnd = Hwnd();
	if (hwnd == nullptr) return;

	LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
	const LONG_PTR decorations = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
	const LONG_PTR wanted = borderless ? (style & ~decorations) : (style | decorations);
	if (wanted == style) return;

	SetWindowLongPtr(hwnd, GWL_STYLE, wanted);
	// The frame only changes once the window is told to recalculate it.
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
