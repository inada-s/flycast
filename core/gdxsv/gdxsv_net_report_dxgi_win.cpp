// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HAVE_D3D11
#include <windows.h>
#include <dxgi.h>

#include "gdxsv_net_report.h"
#include "gdxsv_video_hooks.h"

void gdxsv_video_dx11_presented_stats(IDXGISwapChain* swapchain, long presentResult) {
	if (swapchain == nullptr || FAILED(presentResult) || presentResult == DXGI_STATUS_OCCLUDED) return;
	UINT presentCount;
	DXGI_FRAME_STATISTICS stats;
	if (FAILED(swapchain->GetLastPresentCount(&presentCount)) || FAILED(swapchain->GetFrameStatistics(&stats))) return;
	// A DWM-composed window gets S_OK with all counts 0.
	const bool composed = stats.PresentCount == 0 && stats.SyncRefreshCount == 0;
	static const int64_t freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f.QuadPart;
	}();
	const int64_t qpc = stats.SyncQPCTime.QuadPart;
	const int64_t syncNs = qpc / freq * 1000000000 + qpc % freq * 1000000000 / freq;
	gdxsv_net_report_display(presentCount, stats.PresentCount, stats.PresentRefreshCount, stats.SyncRefreshCount, syncNs, composed);
}
#endif
