// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_represent.h"
#ifdef GDXSV_REPRESENT

#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <thread>

#include "cfg/option.h"
#include "gdxsv_config.h"
#include "gdxsv_perf_trace.h"
#include "gdxsv_video_hooks.h"
#include "log/Log.h"
#include "oslib/oslib.h"
#include "windows/comptr.h"

HWND getNativeHwnd();

namespace {

// 60 Hz * 1.5: below it a frame is shown for one or two vblanks anyway
constexpr int kMinHz = 90;
// vblanks between checks of the window's display (moved to another one, refresh rate changed)
constexpr int kCheckVblanks = 120;

struct Represent {
	std::thread thread;
	std::atomic<bool> running{false};
	std::atomic<bool> due{false};
	std::atomic<bool> active{false};
	// QueryPerformanceCounter of the last present
	std::atomic<int64_t> last_present{0};
	void (*wake)() = nullptr;
	HWND hwnd = nullptr;
	HANDLE stop_event = nullptr;
};

Represent rep;

int64_t qpc() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

// The output showing the window, and its refresh rate in Hz
ComPtr<IDXGIOutput> find_output(IDXGIFactory1* factory, HMONITOR monitor, int& hz) {
	hz = 0;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter.get()) != DXGI_ERROR_NOT_FOUND; a++) {
		ComPtr<IDXGIOutput> output;
		for (UINT o = 0; adapter->EnumOutputs(o, &output.get()) != DXGI_ERROR_NOT_FOUND; o++) {
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
				DEVMODEW mode{};
				mode.dmSize = sizeof(mode);
				if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) hz = mode.dmDisplayFrequency;
				return output;
			}
			output.reset();
		}
		adapter.reset();
	}
	return {};
}

void vblank_loop() {
	ThreadName _("Flycast-vblank");
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory.get()))) return;
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	HMONITOR monitor = nullptr;
	ComPtr<IDXGIOutput> output;
	int64_t half_vblank = 0;
	for (int n = 0; rep.running; n++) {
		if (n % kCheckVblanks == 0) {
			HMONITOR m = MonitorFromWindow(rep.hwnd, MONITOR_DEFAULTTONEAREST);
			if (m != monitor || output == nullptr) {
				monitor = m;
				int hz;
				output = find_output(factory, monitor, hz);
				rep.active = output != nullptr && hz >= kMinHz;
				if (rep.active && getenv("GDXSV_TEST_REPRESENT_PRIMARY") != nullptr) {
					int primary_hz;
					output = find_output(factory, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), primary_hz);
					rep.active = output != nullptr;
					hz = primary_hz;
				}
				half_vblank = hz > 0 ? freq.QuadPart / hz / 2 : 0;
				INFO_LOG(RENDERER, "Re-present: display %d Hz, %s", hz, rep.active ? "on" : "off");
			}
		}
		if (!rep.active) {
			// the window may move to a faster display
			WaitForSingleObject(rep.stop_event, 500);
			n = -1;
			continue;
		}
		if (FAILED(output->WaitForVBlank())) {
			output.reset();
			rep.active = false;
			continue;
		}
		if (settings.input.fastForwardMode || qpc() - rep.last_present.load(std::memory_order_relaxed) < half_vblank)
			continue;
		rep.due = true;
		rep.wake();
	}
	rep.active = false;
}

}  // namespace

void gdxsv_represent_start(void (*wake)()) {
	if (rep.thread.joinable() || !config::GdxRepresentFrames || !config::VSync || gdxsv_video_hidden_window()) return;
	rep.hwnd = getNativeHwnd();
	if (rep.hwnd == nullptr) return;
	rep.wake = wake;
	rep.due = false;
	if (rep.stop_event == nullptr) rep.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ResetEvent(rep.stop_event);
	rep.running = true;
	rep.thread = std::thread(vblank_loop);
}

void gdxsv_represent_stop() {
	if (!rep.thread.joinable()) return;
	rep.running = false;
	SetEvent(rep.stop_event);
	rep.thread.join();
	rep.due = false;
}

bool gdxsv_represent_active() { return rep.active.load(std::memory_order_relaxed); }

void gdxsv_represent_presented() {
	rep.last_present.store(qpc(), std::memory_order_relaxed);
	rep.due = false;
}

bool gdxsv_represent_due() { return rep.due.exchange(false); }
#endif
