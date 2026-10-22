// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Frame pacing probe (local measurement only, not for commit).
#include "gdxsv_fptrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

namespace {
FILE* fp;
std::mutex mtx;
const auto t0 = std::chrono::steady_clock::now();

#ifdef _WIN32
void vblankThread() {
	HMODULE h = LoadLibraryA("dxgi.dll");
	if (!h) return;
	auto create = (HRESULT(WINAPI*)(REFIID, void**))GetProcAddress(h, "CreateDXGIFactory1");
	if (!create) return;
	IDXGIFactory1* f = nullptr;
	if (FAILED(create(__uuidof(IDXGIFactory1), (void**)&f))) return;
	IDXGIAdapter1* ad = nullptr;
	if (FAILED(f->EnumAdapters1(0, &ad))) return;
	IDXGIOutput* out = nullptr;
	if (FAILED(ad->EnumOutputs(0, &out))) return;
	for (;;) {
		if (FAILED(out->WaitForVBlank())) {
			Sleep(1);
			continue;
		}
		const int64_t fptV = gdxsv_fpt_now(), fptD = fptV - gdxsv_fpt_last_vbl.load();
		if (fptD > 3000 && fptD < 50000) gdxsv_fpt_vbl_period = (gdxsv_fpt_vbl_period.load() * 15 + fptD) / 16;
		gdxsv_fpt_last_vbl = fptV;
		gdxsv_fpt_emit("vbl", 0, 0, 0);
	}
}
#endif

bool init() {
	const char* dir = getenv("FPTRACE");
	if (!dir || !*dir) return false;
	char path[512];
#ifdef _WIN32
	snprintf(path, sizeof(path), "%s/fpt_%lu.csv", dir, GetCurrentProcessId());
#else
	snprintf(path, sizeof(path), "%s/fpt.csv", dir);
#endif
	fp = fopen(path, "w");
	if (!fp) return false;
	setvbuf(fp, nullptr, _IOFBF, 1 << 20);
#ifdef _WIN32
	std::thread(vblankThread).detach();
#endif
	return true;
}
}  // namespace

std::atomic<int64_t> gdxsv_fpt_last_vbl{0};
std::atomic<int64_t> gdxsv_fpt_vbl_period{16667};
bool gdxsv_fpt_on = init();

int64_t gdxsv_fpt_now() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

void gdxsv_fpt_emit(const char* ev, int64_t a, int64_t b, int64_t c) {
	const int64_t t = gdxsv_fpt_now();
#ifdef _WIN32
	const unsigned long tid = GetCurrentThreadId();
#else
	const unsigned long tid = 0;
#endif
	std::lock_guard<std::mutex> lock(mtx);
	fprintf(fp, "%lld,%lu,%s,%lld,%lld,%lld\n", (long long)t, tid, ev, (long long)a, (long long)b, (long long)c);
	static int n;
	if (++n % 2000 == 0) fflush(fp);
}

#include "gdxsv_sleep.h"
// Sleep until fpt time t. FPT_HRT=1: high-resolution waitable timer to t - 300 us, then busy wait;
// else the limiter's own sleep (1 ms steps, then busy wait).
void gdxsv_fpt_hr_sleep_until(int64_t t) {
#ifdef _WIN32
	static const bool hrt = getenv("FPT_HRT") && atoi(getenv("FPT_HRT"));
	if (hrt) {
		static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
		const int64_t s = t - 300 - gdxsv_fpt_now();
		if (timer && s > 0) {
			LARGE_INTEGER due;
			due.QuadPart = -s * 10;
			if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
				WaitForSingleObject(timer, INFINITE);
		}
		while (gdxsv_fpt_now() < t) {
		}
		return;
	}
#endif
	const int64_t r = t - gdxsv_fpt_now();
	if (r > 0) gdxsv_sleep_and_busy_wait(r);
}
