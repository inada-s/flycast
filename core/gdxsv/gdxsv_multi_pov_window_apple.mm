// macOS backend for the 4-player replay grid. Cocoa rather than SDL: AppKit
// uses bottom-left frame rects, the grid top-left content rects, and the
// conversion is done here against the real NSWindow.
#include "gdxsv_multi_pov_window.h"

#import <AppKit/AppKit.h>

#include <cmath>

#include <SDL.h>
#include <SDL_syswm.h>

#include "gdxsv_emu_hooks.h"
#include "log/LogManager.h"
#include "sdl/sdl.h"
#include "types.h"

static NSWindow* CocoaWindow() {
	if (gdxsv_headless()) return nil;
	SDL_Window* w = sdl_get_window();
	if (w == nullptr) return nil;

	SDL_SysWMinfo info;
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(w, &info)) return nil;
	if (info.subsystem != SDL_SYSWM_COCOA) return nil;
	return info.info.cocoa.window;
}

// AppKit measures from the primary screen, whose height is the flip axis.
static CGFloat PrimaryHeight() {
	NSArray<NSScreen*>* screens = [NSScreen screens];
	if (screens.count == 0) return 0;
	return NSMaxY(screens[0].frame);
}

// AppKit rect (bottom-left origin) -> desktop rect (top-left origin).
static GdxsvMultiPovRect FromCocoa(NSRect r) {
	GdxsvMultiPovRect out;
	out.x = static_cast<int32_t>(std::lround(NSMinX(r)));
	out.y = static_cast<int32_t>(std::lround(PrimaryHeight() - NSMaxY(r)));
	out.w = static_cast<int32_t>(std::lround(NSWidth(r)));
	out.h = static_cast<int32_t>(std::lround(NSHeight(r)));
	return out;
}

static NSRect ToCocoa(const GdxsvMultiPovRect& r) {
	return NSMakeRect(r.x, PrimaryHeight() - (r.y + r.h), r.w, r.h);
}

bool gdxsv_multi_pov_window_available() { return CocoaWindow() != nil; }

GdxsvMultiPovWindowState gdxsv_multi_pov_window_get_state() {
	GdxsvMultiPovWindowState state;
	NSWindow* win = CocoaWindow();
	if (win == nil) return state;
	state.frame = gdxsv_multi_pov_window_get_frame();
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0)
		state.mode = GdxsvMultiPovWindowMode::Fullscreen;
	else if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0)
		state.mode = GdxsvMultiPovWindowMode::NativeFullscreen;
	else if (win.isZoomed)
		state.mode = GdxsvMultiPovWindowMode::Maximized;
	return state;
}

void gdxsv_multi_pov_window_restore_state(const GdxsvMultiPovWindowState& state) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	if (gdxsv_multi_pov_window_is_maximized()) gdxsv_multi_pov_window_unmaximize();
	// Position on the original display before restoring its window mode.
	gdxsv_multi_pov_window_set_frame(state.frame);
	if (state.mode == GdxsvMultiPovWindowMode::Maximized && !win.isZoomed)
		[win zoom:nil];
	else if (state.mode == GdxsvMultiPovWindowMode::NativeFullscreen) {
		// SDL fullscreen suppresses the native menu action and menu-bar reveal.
		// Keep its flags clear when restoring a native macOS fullscreen window.
		if ((win.styleMask & NSWindowStyleMaskFullScreen) == 0)
			[win toggleFullScreen:nil];
	}
	else if (state.mode == GdxsvMultiPovWindowMode::Fullscreen &&
		SDL_SetWindowFullscreen(sdl_get_window(), SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		WARN_LOG(COMMON, "multi-pov: cannot restore full screen: %s", SDL_GetError());
}

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};
	return FromCocoa([win contentRectForFrameRect:win.frame]);
}

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect& rect) {
	NSWindow* win = CocoaWindow();
	if (win == nil || rect.w <= 0 || rect.h <= 0) return;

	// setFrame: moves without ordering the window front.
	const NSRect content = ToCocoa(rect);
	[win setFrame:[win frameRectForContentRect:content] display:YES];
}

bool gdxsv_multi_pov_window_is_maximized() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return false;
	// Zoom and full screen both tile the grid.
	if ((win.styleMask & NSWindowStyleMaskFullScreen) != 0) return true;
	if ((SDL_GetWindowFlags(sdl_get_window()) & SDL_WINDOW_FULLSCREEN) != 0) return true;
	return win.isZoomed;
}

void gdxsv_multi_pov_window_unmaximize() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	// Adopt fullscreen entered through the green button before leaving via
	// SDL. SDL waits for the animation, so AppKit cannot overwrite our frame.
	SDL_Window* w = sdl_get_window();
	const bool native_fullscreen = (win.styleMask & NSWindowStyleMaskFullScreen) != 0;
	if ((native_fullscreen && SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) ||
		SDL_SetWindowFullscreen(w, 0) != 0) {
		WARN_LOG(COMMON, "multi-pov: cannot leave full screen: %s", SDL_GetError());
		return;
	}
	if (win.isZoomed) [win zoom:nil];
}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};

	NSScreen* screen = win.screen;
	if (screen == nil) screen = [NSScreen mainScreen];
	if (screen == nil) return {};

	return FromCocoa(screen.visibleFrame);
}

GdxsvMultiPovRect gdxsv_multi_pov_window_display_area() {
	NSWindow* win = CocoaWindow();
	if (win == nil) return {};

	NSScreen* screen = win.screen;
	if (screen == nil) screen = [NSScreen mainScreen];
	if (screen == nil) return {};

	return FromCocoa(screen.frame);
}

void gdxsv_multi_pov_window_set_topmost(bool topmost) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;
	win.level = topmost ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

GdxsvMultiPovInsets gdxsv_multi_pov_window_frame_insets() {
	GdxsvMultiPovInsets out;
	NSWindow* win = CocoaWindow();
	if (win == nil) return out;

	const NSRect content = NSMakeRect(0, 0, 100, 100);
	const NSRect frame = [win frameRectForContentRect:content];
	out.left = static_cast<int32_t>(std::lround(NSMinX(content) - NSMinX(frame)));
	out.right = static_cast<int32_t>(std::lround(NSMaxX(frame) - NSMaxX(content)));
	out.top = static_cast<int32_t>(std::lround(NSMaxY(frame) - NSMaxY(content)));
	out.bottom = static_cast<int32_t>(std::lround(NSMinY(content) - NSMinY(frame)));
	return out;
}

void gdxsv_multi_pov_window_set_borderless(bool borderless) {
	NSWindow* win = CocoaWindow();
	if (win == nil) return;

	if (borderless) {
		// Keep it resizable so AppKit still lets us set an arbitrary frame.
		win.styleMask = NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable;
	} else {
		win.styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
						NSWindowStyleMaskResizable;
	}
}
