/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// gdxsv: added by the fork (docs/gdxsv/upstream.md)
#include "render_thread.h"
#include "mainui.h"
#include "gui.h"
#include "imgui_driver.h"
#include "emulator.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "wsi/context.h"
#include "gdxsv/gdxsv_emu_hooks.h"
#include "gdxsv/gdxsv_perf_trace.h"
#include "gdxsv/gdxsv_represent.h"
#include "hw/pvr/Renderer_if.h"

#include <atomic>
#include <chrono>
#include <thread>
#ifdef USE_SDL
#include <SDL.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace renderthread
{

static std::thread thread;
static std::atomic<bool> stopRequested;
static std::atomic<bool> done;
static std::exception_ptr error;
static bool reinit;
static std::atomic<bool> resizePending;
static std::atomic<bool> stylePending;
static thread_local bool onRenderThread;

static void wakeMainThread()
{
#ifdef USE_SDL
	static const u32 type = SDL_RegisterEvents(1);
	if (type != (u32)-1)
	{
		SDL_Event event{};
		event.type = type;
		SDL_PushEvent(&event);
	}
#endif
}

// Headless: no context
static void makeCurrent(bool current)
{
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->makeCurrent(current);
}

static void applyDisplayChange()
{
	if (resizePending.exchange(false) && GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->resize();
	if (stylePending.exchange(false))
		gui_updateStyle();
}

// The last frame with its OSD, presented again (gdxsv_represent.h)
static void represent()
{
	ImDrawData *drawData = ImGui::GetDrawData();
	if (drawData == nullptr)
		return;
	imguiDriver->renderDrawData(drawData, true);
	GDXSV_PERF_TRACE("represent", -1, 0);
	imguiDriver->present();
	gdxsv_represent_presented();
}

static void loop()
{
	ThreadName _("Flycast-render");
	onRenderThread = true;
	INFO_LOG(RENDERER, "Render thread started");
	makeCurrent(true);
	gdxsv_represent_start(rend_wake_render_thread);
	try {
		while (!stopRequested && emu.running() && !gui_is_open())
		{
			applyDisplayChange();
			if (GraphicsContext::Instance() != nullptr)
			{
				// a new frame must not wait for a re-presented one to show
				GraphicsContext::Instance()->setPresentWait(!gdxsv_represent_active());
				GraphicsContext::Instance()->waitPresentReady();
			}
			const bool rendered = emu.render();
			if (!rendered && !gdxsv_represent_due())
				continue;
			if (rendered && config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
			try {
				if (rendered)
				{
					GDXSV_PERF_TRACE("frame_present", -1, 0);
					imguiDriver->present();
				}
				else
					represent();
			} catch (const FlycastException&) {
				reinit = true;
				break;
			}
			if (!rendered)
				continue;
			gdxsv_represent_presented();
			MainFrameCount++;
			gdxsv_emu_render_presented();
		}
	} catch (...) {
		error = std::current_exception();
	}
	gdxsv_represent_stop();
	// the GUI's presents wait again
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->setPresentWait(true);
	makeCurrent(false);
	done = true;
	wakeMainThread();
}

void start()
{
	if (thread.joinable())
		return;
	stopRequested = false;
	done = false;
	error = nullptr;
	reinit = false;
	makeCurrent(false);
	thread = std::thread(loop);
}

bool started() {
	return thread.joinable();
}

bool finished() {
	return done;
}

std::exception_ptr stop()
{
	if (!thread.joinable())
		return nullptr;
	stopRequested = true;
#ifdef __APPLE__
	// SDL's Cocoa GL swap waits for the main thread after a window change ([NSOpenGLContext update] by
	// dispatch_sync): run the main queue until the render thread ends, or both wait for each other
	while (!done)
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
#endif
	thread.join();
	makeCurrent(true);
	// a change that came while it stopped
	applyDisplayChange();
	done = false;
	std::exception_ptr e = error;
	error = nullptr;
	return e;
}

bool current() {
	return onRenderThread;
}

bool takeReinit()
{
	bool r = reinit;
	reinit = false;
	return r;
}

void waitEvents(int timeoutMs)
{
#ifdef USE_SDL
	SDL_WaitEventTimeout(nullptr, timeoutMs);
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

void displayChanged(bool scaleChanged)
{
	if (thread.joinable())
	{
		resizePending = true;
		if (scaleChanged)
			stylePending = true;
		return;
	}
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->resize();
	if (scaleChanged)
		gui_updateStyle();
}

} // namespace renderthread
