// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gdxsv_frame_trace.h"

#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include <xxhash.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

bool gdxsv_trace_on = false;

namespace {
struct Row {
	long long qpc;
	unsigned tid;
	int frame;
	const char* event;
	long long arg;
};

std::atomic<int> currentFrame{-1};
std::mutex rowsMutex;
std::vector<Row> rows;
FILE* file = nullptr;

long long now() {
#ifdef _WIN32
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
#else
	return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

unsigned threadId() {
#ifdef _WIN32
	return GetCurrentThreadId();
#else
	return (unsigned)std::hash<std::thread::id>()(std::this_thread::get_id());
#endif
}

// Drains the rows every 100 ms, so a killed process keeps all but the last ones.
void writerLoop() {
	std::vector<Row> batch;
	for (;;) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		{
			std::lock_guard<std::mutex> lock(rowsMutex);
			batch.swap(rows);
		}
		for (const Row& r : batch)
			fprintf(file, "%lld,%u,%d,%s,%lld\n", r.qpc, r.tid, r.frame, r.event, r.arg);
		fflush(file);
		batch.clear();
	}
}

#ifdef _WIN32
// The first output of the first adapter: the desktop the window is on in the rig.
void vblankLoop() {
	using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
	HMODULE dxgi = LoadLibraryA("dxgi.dll");
	auto create = dxgi ? (CreateFactory)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
	IDXGIFactory1* factory = nullptr;
	IDXGIAdapter1* adapter = nullptr;
	IDXGIOutput* output = nullptr;
	if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), (void**)&factory)) ||
		FAILED(factory->EnumAdapters1(0, &adapter)) || FAILED(adapter->EnumOutputs(0, &output))) {
		gdxsv_trace_event("hvbl_unavailable", 0);
		return;
	}
	for (;;) {
		const HRESULT hr = output->WaitForVBlank();
		gdxsv_trace_event("hvbl", (long long)hr);
		if (FAILED(hr))
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}
#endif
}  // namespace

void gdxsv_trace_init() {
	const char* path = getenv("GDXSV_FRAME_TRACE");
	if (path == nullptr || file != nullptr)
		return;
	file = fopen(path, "w");
	if (file == nullptr)
		return;
	setvbuf(file, nullptr, _IOFBF, 1 << 20);
	long long freq = 1000000000;
#ifdef _WIN32
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	freq = f.QuadPart;
#endif
	fprintf(file, "qpc,tid,frame,event,arg\n%lld,0,-1,qpc_freq,%lld\n", now(), freq);
	rows.reserve(1 << 16);
	gdxsv_trace_on = true;
	std::thread(writerLoop).detach();
#ifdef _WIN32
	std::thread(vblankLoop).detach();
#endif
}

void gdxsv_trace_event(const char* event, long long arg) {
	const Row r{now(), threadId(), currentFrame.load(std::memory_order_relaxed), event, arg};
	std::lock_guard<std::mutex> lock(rowsMutex);
	rows.push_back(r);
}

void gdxsv_trace_set_frame(int frame) { currentFrame.store(frame, std::memory_order_relaxed); }

bool gdxsv_trace_ta_hash = getenv("GDXSV_TA_HASH") != nullptr;

void gdxsv_trace_press(int frame, int player, unsigned& kcode) {
	static int who = -1, first = -1, frames = 0;
	static unsigned keys = 0;
	static const bool on = [] {
		const char* env = getenv("GDXSV_PRESS");
		return env != nullptr && sscanf(env, "%d,%d,%x,%d", &who, &first, &keys, &frames) == 4;
	}();
	if (on && player == who && first <= frame && frame < first + frames) {
		kcode &= ~keys;  // active low
		GDXSV_TRACE("press", keys);
	}
}

void gdxsv_trace_ram(int frame) {
	static char dir[512];
	static int first = 0, last = -1;
	static const bool on = [] {
		const char* env = getenv("GDXSV_RAM_DUMP");
		return env != nullptr && sscanf(env, "%511[^,],%d,%d", dir, &first, &last) == 3;
	}();
	if (!on || frame < first || last < frame)
		return;
	char path[600];
	snprintf(path, sizeof(path), "%s/%d.bin", dir, frame);
	if (FILE* f = fopen(path, "wb")) {
		fwrite(&mem_b[0], 1, settings.platform.ram_size, f);
		fclose(f);
	}
}

void gdxsv_trace_pos(int frame) {
	static const char* const names[] = {"pos0", "pos1", "pos2", "pos3"};
	static unsigned addr[4], len[4];
	static const int count = [] {
		const char* env = getenv("GDXSV_TRACE_POS");
		int n = 0, used = 0;
		while (env != nullptr && n < 4 && sscanf(env, "%x,%u%n", &addr[n], &len[n], &used) == 2) {
			n++;
			env += used;
			if (*env != ';')
				break;
			env++;
		}
		return n;
	}();
	if (!gdxsv_trace_on)
		return;
	for (int i = 0; i < count; i++) {
		const unsigned off = addr[i] & (settings.platform.ram_size - 1);
		if (off + len[i] <= settings.platform.ram_size)
			gdxsv_trace_event(names[i], (long long)(XXH3_64bits(&mem_b[off], len[i]) >> 1));
	}
}
